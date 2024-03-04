#include <nano/lib/threading.hpp>
#include <nano/lib/timer.hpp>
#include <nano/node/blockprocessor.hpp>
#include <nano/node/node.hpp>
#include <nano/store/component.hpp>

#include <boost/format.hpp>

#include <magic_enum.hpp>

/*
 * block_processor::context
 */

nano::block_processor::context::context (std::shared_ptr<nano::block> block, nano::block_source source_a) :
	block{ block },
	source{ source_a }
{
	debug_assert (source != nano::block_source::unknown);
}

auto nano::block_processor::context::get_future () -> std::future<result_t>
{
	return promise.get_future ();
}

void nano::block_processor::context::set_result (result_t const & result)
{
	promise.set_value (result);
}

/*
 * block_processor
 */

nano::block_processor::block_processor (nano::node & node_a, nano::write_database_queue & write_database_queue_a) :
	node (node_a),
	write_database_queue (write_database_queue_a),
	next_log (std::chrono::steady_clock::now ())
{
	batch_processed.add ([this] (auto const & items) {
		// For every batch item: notify the 'processed' observer.
		for (auto const & [result, context] : items)
		{
			block_processed.notify (result, context);
		}
	});

	queue.max_size_query = [this] (auto const & origin) {
		switch (origin.source)
		{
			case nano::block_source::live:
				return 128;
			default:
				return 1024 * 16;
		}
	};

	queue.priority_query = [this] (auto const & origin) {
		switch (origin.source)
		{
			case nano::block_source::live:
				return 1;
			case nano::block_source::local:
				return 16;
			case nano::block_source::bootstrap:
				return 8;
			default:
				return 1;
		}
	};

	queue.rate_limit_query = [this] (auto const & origin) -> std::pair<size_t, double> {
		switch (origin.source)
		{
			case nano::block_source::live:
				return { 100, 3.0 };
			default:
				return { 0, 1.0 }; // Unlimited
		}
	};
}

nano::block_processor::~block_processor ()
{
	// Thread must be stopped before destruction
	debug_assert (!thread.joinable ());
}

void nano::block_processor::start ()
{
	debug_assert (!thread.joinable ());

	thread = std::thread ([this] () {
		nano::thread_role::set (nano::thread_role::name::block_processing);
		run ();
	});
}

void nano::block_processor::stop ()
{
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		stopped = true;
	}
	condition.notify_all ();
	if (thread.joinable ())
	{
		thread.join ();
	}
}

// TODO: Remove and replace all checks with calls to size (block_source)
std::size_t nano::block_processor::size () const
{
	nano::unique_lock<nano::mutex> lock{ mutex };
	return queue.total_size ();
}

std::size_t nano::block_processor::size (nano::block_source source) const
{
	nano::unique_lock<nano::mutex> lock{ mutex };
	return queue.size (source);
}

bool nano::block_processor::full () const
{
	return size () >= node.flags.block_processor_full_size;
}

bool nano::block_processor::half_full () const
{
	return size () >= node.flags.block_processor_full_size / 2;
}

void nano::block_processor::add (std::shared_ptr<nano::block> const & block, block_source const source, std::shared_ptr<nano::transport::channel> channel)
{
	if (full ())
	{
		node.stats.inc (nano::stat::type::blockprocessor, nano::stat::detail::overfill);
		return;
	}
	if (node.network_params.work.validate_entry (*block)) // true => error
	{
		node.stats.inc (nano::stat::type::blockprocessor, nano::stat::detail::insufficient_work);
		return;
	}

	node.stats.inc (nano::stat::type::blockprocessor, nano::stat::detail::process);
	node.logger.debug (nano::log::type::blockprocessor, "Processing block (async): {} (source: {} {})",
	block->hash ().to_string (),
	to_string (source),
	channel ? channel->to_string () : "<unknown>"); // TODO: Lazy eval

	add_impl (context{ block, source }, channel);
}

std::optional<nano::block_status> nano::block_processor::add_blocking (std::shared_ptr<nano::block> const & block, block_source const source)
{
	node.stats.inc (nano::stat::type::blockprocessor, nano::stat::detail::process_blocking);
	node.logger.debug (nano::log::type::blockprocessor, "Processing block (blocking): {} (source: {})", block->hash ().to_string (), to_string (source));

	context ctx{ block, source };
	auto future = ctx.get_future ();
	add_impl (std::move (ctx));

	try
	{
		auto status = future.wait_for (node.config.block_process_timeout);
		debug_assert (status != std::future_status::deferred);
		if (status == std::future_status::ready)
		{
			return future.get ();
		}
	}
	catch (std::future_error const &)
	{
		node.stats.inc (nano::stat::type::blockprocessor, nano::stat::detail::process_blocking_timeout);
		node.logger.error (nano::log::type::blockprocessor, "Timeout processing block: {}", block->hash ().to_string ());
	}

	return std::nullopt;
}

void nano::block_processor::force (std::shared_ptr<nano::block> const & block_a)
{
	node.stats.inc (nano::stat::type::blockprocessor, nano::stat::detail::force);
	node.logger.debug (nano::log::type::blockprocessor, "Forcing block: {}", block_a->hash ().to_string ());

	add_impl (context{ block_a, block_source::forced });
}

void nano::block_processor::add_impl (context ctx, std::shared_ptr<nano::transport::channel> channel)
{
	{
		nano::lock_guard<nano::mutex> guard{ mutex };
		bool overflow = queue.push (std::move (ctx), ctx.source, channel);
		if (overflow)
		{
			node.stats.inc (nano::stat::type::blockprocessor, nano::stat::detail::queue_overflow);
		}
	}
	condition.notify_all ();
}

void nano::block_processor::rollback_competitor (store::write_transaction const & transaction, nano::block const & block)
{
	auto hash = block.hash ();
	auto successor = node.ledger.successor (transaction, block.qualified_root ());
	if (successor != nullptr && successor->hash () != hash)
	{
		// Replace our block with the winner and roll back any dependent blocks
		node.logger.debug (nano::log::type::blockprocessor, "Rolling back: {} and replacing with: {}", successor->hash ().to_string (), hash.to_string ());

		std::vector<std::shared_ptr<nano::block>> rollback_list;
		if (node.ledger.rollback (transaction, successor->hash (), rollback_list))
		{
			node.stats.inc (nano::stat::type::ledger, nano::stat::detail::rollback_failed);
			node.logger.error (nano::log::type::blockprocessor, "Failed to roll back: {} because it or a successor was confirmed", successor->hash ().to_string ());
		}
		else
		{
			node.stats.inc (nano::stat::type::ledger, nano::stat::detail::rollback);
			node.logger.debug (nano::log::type::blockprocessor, "Blocks rolled back: {}", rollback_list.size ());
		}

		// Deleting from votes cache, stop active transaction
		for (auto & i : rollback_list)
		{
			rolled_back.notify (i);

			node.history.erase (i->root ());
			// Stop all rolled back active transactions except initial
			if (i->hash () != successor->hash ())
			{
				node.active.erase (*i);
			}
		}
	}
}

void nano::block_processor::run ()
{
	nano::unique_lock<nano::mutex> lock{ mutex };
	while (!stopped)
	{
		if (!queue.empty ())
		{
			active = true;
			lock.unlock ();

			auto processed = process_batch (lock);
			debug_assert (!lock.owns_lock ());

			// Set results for futures when not holding the lock
			for (auto & [result, context] : processed)
			{
				context.set_result (result);
			}

			batch_processed.notify (processed);

			lock.lock ();
			active = false;
		}
		else
		{
			condition.notify_one ();
			condition.wait (lock);
		}
	}
}

bool nano::block_processor::should_log ()
{
	auto result (false);
	auto now (std::chrono::steady_clock::now ());
	if (next_log < now)
	{
		next_log = now + std::chrono::seconds (15);
		result = true;
	}
	return result;
}

auto nano::block_processor::next () -> context
{
	debug_assert (!mutex.try_lock ());
	debug_assert (!queue.empty ()); // This should be checked before calling next

	if (!queue.empty ())
	{
		auto [request, origin] = queue.next ();
		release_assert (origin.source != nano::block_source::forced || request.source == nano::block_source::forced);
		return std::move (request);
	}

	release_assert (false, "next() called when no blocks are ready");
}

auto nano::block_processor::process_batch (nano::unique_lock<nano::mutex> & lock_a) -> processed_batch_t
{
	processed_batch_t processed;

	auto scoped_write_guard = write_database_queue.wait (nano::writer::process_batch);
	auto transaction (node.store.tx_begin_write ({ tables::accounts, tables::blocks, tables::frontiers, tables::pending }));
	nano::timer<std::chrono::milliseconds> timer_l;

	lock_a.lock ();

	queue.periodic_cleanup ();

	timer_l.start ();

	// Processing blocks
	unsigned number_of_blocks_processed (0), number_of_forced_processed (0);
	auto deadline_reached = [&timer_l, deadline = node.config.block_processor_batch_max_time] { return timer_l.after_deadline (deadline); };
	auto processor_batch_reached = [&number_of_blocks_processed, max = node.flags.block_processor_batch_size] { return number_of_blocks_processed >= max; };
	auto store_batch_reached = [&number_of_blocks_processed, max = node.store.max_block_write_batch_num ()] { return number_of_blocks_processed >= max; };

	while (!queue.empty () && (!deadline_reached () || !processor_batch_reached ()) && !store_batch_reached ())
	{
		// TODO: Cleaner periodical logging
		if (should_log ())
		{
			node.logger.debug (nano::log::type::blockprocessor, "{} blocks in processing queue", queue.total_size ());
		}

		auto ctx = next ();
		auto const hash = ctx.block->hash ();
		bool const force = ctx.source == nano::block_source::forced;

		lock_a.unlock ();

		if (force)
		{
			number_of_forced_processed++;
			rollback_competitor (transaction, *ctx.block);
		}

		number_of_blocks_processed++;

		auto result = process_one (transaction, ctx, force);
		processed.emplace_back (result, std::move (ctx));

		lock_a.lock ();
	}

	lock_a.unlock ();

	if (number_of_blocks_processed != 0 && timer_l.stop () > std::chrono::milliseconds (100))
	{
		node.logger.debug (nano::log::type::blockprocessor, "Processed {} blocks ({} forced) in {} {}", number_of_blocks_processed, number_of_forced_processed, timer_l.value ().count (), timer_l.unit ());
	}

	return processed;
}

nano::block_status nano::block_processor::process_one (store::write_transaction const & transaction_a, context const & context, bool const forced_a)
{
	auto block = context.block;
	auto const hash = block->hash ();
	nano::block_status result = node.ledger.process (transaction_a, block);

	node.stats.inc (nano::stat::type::blockprocessor_result, to_stat_detail (result));
	node.stats.inc (nano::stat::type::blockprocessor_source, to_stat_detail (context.source));
	node.logger.trace (nano::log::type::blockprocessor, nano::log::detail::block_processed,
	nano::log::arg{ "result", result },
	nano::log::arg{ "source", context.source },
	nano::log::arg{ "arrival", nano::log::microseconds (context.arrival) },
	nano::log::arg{ "forced", forced_a },
	nano::log::arg{ "block", block });

	switch (result)
	{
		case nano::block_status::progress:
		{
			queue_unchecked (transaction_a, hash);
			/* For send blocks check epoch open unchecked (gap pending).
			For state blocks check only send subtype and only if block epoch is not last epoch.
			If epoch is last, then pending entry shouldn't trigger same epoch open block for destination account. */
			if (block->type () == nano::block_type::send || (block->type () == nano::block_type::state && block->sideband ().details.is_send && std::underlying_type_t<nano::epoch> (block->sideband ().details.epoch) < std::underlying_type_t<nano::epoch> (nano::epoch::max)))
			{
				/* block->destination () for legacy send blocks
				block->link () for state blocks (send subtype) */
				queue_unchecked (transaction_a, block->destination ().is_zero () ? block->link () : block->destination ());
			}
			break;
		}
		case nano::block_status::gap_previous:
		{
			node.unchecked.put (block->previous (), block);
			node.stats.inc (nano::stat::type::ledger, nano::stat::detail::gap_previous);
			break;
		}
		case nano::block_status::gap_source:
		{
			node.unchecked.put (node.ledger.block_source (transaction_a, *block), block);
			node.stats.inc (nano::stat::type::ledger, nano::stat::detail::gap_source);
			break;
		}
		case nano::block_status::gap_epoch_open_pending:
		{
			node.unchecked.put (block->account (), block); // Specific unchecked key starting with epoch open block account public key
			node.stats.inc (nano::stat::type::ledger, nano::stat::detail::gap_source);
			break;
		}
		case nano::block_status::old:
		{
			node.stats.inc (nano::stat::type::ledger, nano::stat::detail::old);
			break;
		}
		case nano::block_status::bad_signature:
		{
			break;
		}
		case nano::block_status::negative_spend:
		{
			break;
		}
		case nano::block_status::unreceivable:
		{
			break;
		}
		case nano::block_status::fork:
		{
			node.stats.inc (nano::stat::type::ledger, nano::stat::detail::fork);
			break;
		}
		case nano::block_status::opened_burn_account:
		{
			break;
		}
		case nano::block_status::balance_mismatch:
		{
			break;
		}
		case nano::block_status::representative_mismatch:
		{
			break;
		}
		case nano::block_status::block_position:
		{
			break;
		}
		case nano::block_status::insufficient_work:
		{
			break;
		}
	}
	return result;
}

void nano::block_processor::queue_unchecked (store::write_transaction const & transaction_a, nano::hash_or_account const & hash_or_account_a)
{
	node.unchecked.trigger (hash_or_account_a);
}

std::unique_ptr<nano::container_info_component> nano::block_processor::collect_container_info (std::string const & name)
{
	//	std::size_t blocks_count;
	//	{
	//		nano::lock_guard<nano::mutex> guard{ mutex };
	//		blocks_count = queue.total_size ();
	//	}

	auto composite = std::make_unique<container_info_composite> (name);
	composite->add_component (queue.collect_container_info ("queue"));
	//	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "blocks", blocks_count, sizeof (decltype (blocks)::value_type) }));
	//	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "forced", forced_count, sizeof (decltype (forced)::value_type) }));
	return composite;
}

std::string_view nano::to_string (nano::block_source source)
{
	return magic_enum::enum_name (source);
}

nano::stat::detail nano::to_stat_detail (nano::block_source type)
{
	auto value = magic_enum::enum_cast<nano::stat::detail> (magic_enum::enum_name (type));
	debug_assert (value);
	return value.value_or (nano::stat::detail{});
}
