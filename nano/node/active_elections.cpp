#include <nano/lib/blocks.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/lib/threading.hpp>
#include <nano/node/active_elections.hpp>
#include <nano/node/confirmation_solicitor.hpp>
#include <nano/node/confirming_set.hpp>
#include <nano/node/election.hpp>
#include <nano/node/node.hpp>
#include <nano/node/repcrawler.hpp>
#include <nano/node/scheduler/component.hpp>
#include <nano/node/scheduler/priority.hpp>
#include <nano/node/vote_router.hpp>
#include <nano/secure/ledger.hpp>
#include <nano/secure/ledger_set_any.hpp>
#include <nano/store/component.hpp>

#include <ranges>

using namespace std::chrono;

nano::active_elections::active_elections (nano::node & node_a, nano::confirming_set & confirming_set, nano::block_processor & block_processor_a) :
	config{ node_a.config.active_elections },
	node{ node_a },
	confirming_set{ confirming_set },
	block_processor{ block_processor_a },
	recently_confirmed{ config.confirmation_cache },
	recently_cemented{ config.confirmation_history_size },
	election_time_to_live{ node_a.network_params.network.is_dev_network () ? 0s : 2s }
{
	// Register a callback which will get called after a block is cemented
	confirming_set.cemented_observers.add ([this] (std::shared_ptr<nano::block> const & callback_block_a) {
		this->block_cemented_callback (callback_block_a);
	});

	// Register a callback which will get called if a block is already cemented
	confirming_set.block_already_cemented_observers.add ([this] (nano::block_hash const & hash_a) {
		this->block_already_cemented_callback (hash_a);
	});

	// Notify elections about alternative (forked) blocks
	block_processor.block_processed.add ([this] (auto const & result, auto const & context) {
		switch (result)
		{
			case nano::block_status::fork:
				publish (context.block);
				break;
			default:
				break;
		}
	});
}

nano::active_elections::~active_elections ()
{
	// Thread must be stopped before destruction
	debug_assert (!thread.joinable ());
}

void nano::active_elections::start ()
{
	if (node.flags.disable_request_loop)
	{
		return;
	}

	debug_assert (!thread.joinable ());

	thread = std::thread ([this] () {
		nano::thread_role::set (nano::thread_role::name::request_loop);
		request_loop ();
	});
}

void nano::active_elections::stop ()
{
	{
		nano::lock_guard<nano::mutex> guard{ mutex };
		stopped = true;
	}
	condition.notify_all ();
	nano::join_or_pass (thread);
	clear ();
}

auto nano::active_elections::insert (std::shared_ptr<nano::block> const & block, nano::election_behavior behavior, nano::bucket_t bucket, nano::priority_t priority) -> insert_result
{
	release_assert (block);
	release_assert (block->has_sideband ());

	nano::unique_lock<nano::mutex> lock{ mutex };

	if (stopped)
	{
		return {};
	}

	auto const root = block->qualified_root ();
	auto const hash = block->hash ();

	// If the election already exists, return it
	if (auto existing = elections.find (root))
	{
		return { existing, /* inserted */ false };
	}

	if (recently_confirmed.exists (root))
	{
		return {};
	}

	// Election does not exist, create a new one

	auto observe_rep_cb = [&node = node] (auto const & rep_a) {
		// Representative is defined as online if replying to live votes or rep_crawler queries
		node.online_reps.observe (rep_a);
	};

	auto election = nano::make_shared<nano::election> (node, block, nullptr, observe_rep_cb, behavior);
	elections.insert (election, behavior, bucket, priority);
	node.vote_router.connect (hash, election);

	node.stats.inc (nano::stat::type::active_started, to_stat_detail (behavior));
	node.logger.trace (nano::log::type::active_elections, nano::log::detail::active_started,
	nano::log::arg{ "behavior", behavior },
	nano::log::arg{ "election", election });

	node.logger.debug (nano::log::type::active_elections, "Started new election for block: {} (behavior: {}, bucket: {}, priority: {})",
	hash.to_string (),
	to_string (behavior),
	bucket,
	priority);

	lock.unlock ();

	node.vote_router.trigger_vote_cache (hash);
	node.observers.active_started.notify (hash);
	vacancy_update ();

	// Votes are generated for inserted or ongoing elections
	election->broadcast_vote ();

	return { election, /* inserted */ true };
}

bool nano::active_elections::publish (std::shared_ptr<nano::block> const & block)
{
	nano::unique_lock<nano::mutex> lock{ mutex };

	if (auto election = elections.find (block->qualified_root ()))
	{
		lock.unlock ();

		bool result = election->publish (block); // false => new block was added
		if (!result)
		{
			lock.lock ();
			node.vote_router.connect (block->hash (), election);
			lock.unlock ();
			node.vote_router.trigger_vote_cache (block->hash ());

			node.stats.inc (nano::stat::type::active, nano::stat::detail::election_block_conflict);

			return false; // Added
		}
	}

	return true; // Not added
}

std::size_t nano::active_elections::size () const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	return elections.size ();
}

size_t nano::active_elections::size (nano::election_behavior behavior) const
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	return elections.size (behavior);
}

size_t nano::active_elections::size (nano::election_behavior behavior, nano::bucket_t bucket) const
{
	debug_assert (behavior == nano::election_behavior::priority); // We do not expect other behaviors to use buckets

	nano::lock_guard<nano::mutex> guard{ mutex };
	return elections.size (behavior, bucket);
}

bool nano::active_elections::empty () const
{
	return size () == 0;
}

std::shared_ptr<nano::election> nano::active_elections::top (nano::election_behavior behavior, nano::bucket_t bucket) const
{
	debug_assert (behavior == nano::election_behavior::priority); // We do not expect other behaviors to use buckets

	nano::lock_guard<nano::mutex> guard{ mutex };
	return elections.top (behavior, bucket);
}

bool nano::active_elections::active (nano::qualified_root const & root) const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	return elections.exists (root);
}

bool nano::active_elections::active (nano::block const & block) const
{
	return active (block.qualified_root ());
}

std::shared_ptr<nano::election> nano::active_elections::election (nano::qualified_root const & root) const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	return elections.find (root);
}

bool nano::active_elections::erase (nano::qualified_root const & root)
{
	nano::unique_lock<nano::mutex> lock{ mutex };

	if (auto election = elections.find (root))
	{
		erase_impl (lock, election);
		return true;
	}
	else
	{
		return false;
	}
}

bool nano::active_elections::erase (nano::block const & block)
{
	return erase (block.qualified_root ());
}

void nano::active_elections::erase_impl (nano::unique_lock<nano::mutex> & lock, std::shared_ptr<nano::election> election)
{
	debug_assert (!mutex.try_lock ());
	debug_assert (lock.owns_lock ());
	debug_assert (elections.exists (election));
	debug_assert (!election->confirmed () || recently_confirmed.exists (election->qualified_root));

	auto blocks_l = election->blocks ();
	node.vote_router.disconnect (*election);

	elections.erase (election);

	node.stats.inc (nano::stat::type::active_elections, nano::stat::detail::election_cleanup);
	node.stats.inc (nano::stat::type::election_cleanup, to_stat_detail (election->state ()));
	node.stats.inc (to_completion_type (election->state ()), to_stat_detail (election->behavior ()));
	node.logger.trace (nano::log::type::active_elections, nano::log::detail::active_stopped, nano::log::arg{ "election", election });

	node.logger.debug (nano::log::type::active_elections, "Erased election for blocks: {} (behavior: {}, state: {})",
	fmt::join (std::views::keys (blocks_l), ", "),
	to_string (election->behavior ()),
	to_string (election->state ()));

	lock.unlock ();

	node.stats.sample (nano::stat::sample::active_election_duration, { 0, 1000 * 60 * 10 /* 0-10 minutes range */ }, election->duration ().count ());

	// Notify observers without holding the lock
	vacancy_update ();

	for (auto const & [hash, block] : blocks_l)
	{
		// Notify observers about dropped elections & blocks lost confirmed elections
		if (!election->confirmed () || hash != election->winner ()->hash ())
		{
			node.observers.active_stopped.notify (hash);
		}

		if (!election->confirmed ())
		{
			// Clear from publish filter
			node.network.publish_filter.clear (block);
		}
	}
}

void nano::active_elections::clear ()
{
	{
		nano::lock_guard<nano::mutex> guard{ mutex };
		elections.clear ();
	}
	// Notify observers without holding the lock
	vacancy_update ();
}

void nano::active_elections::block_cemented_callback (std::shared_ptr<nano::block> const & block)
{
	debug_assert (node.block_confirmed (block->hash ()));
	if (auto election_l = election (block->qualified_root ()))
	{
		election_l->try_confirm (block->hash ());
	}
	auto election = remove_election_winner_details (block->hash ());
	nano::election_status status;
	std::vector<nano::vote_with_weight_info> votes;
	status.winner = block;
	if (election)
	{
		status = election->get_status ();
		votes = election->votes_with_weight ();
	}
	if (confirming_set.exists (block->hash ()))
	{
		status.type = nano::election_status_type::active_confirmed_quorum;
	}
	else if (election)
	{
		status.type = nano::election_status_type::active_confirmation_height;
	}
	else
	{
		status.type = nano::election_status_type::inactive_confirmation_height;
	}
	recently_cemented.put (status);
	auto transaction = node.ledger.tx_begin_read ();
	notify_observers (transaction, status, votes);
	bool cemented_bootstrap_count_reached = node.ledger.cemented_count () >= node.ledger.bootstrap_weight_max_blocks;
	bool was_active = status.type == nano::election_status_type::active_confirmed_quorum || status.type == nano::election_status_type::active_confirmation_height;

	// Next-block activations are only done for blocks with previously active elections
	if (cemented_bootstrap_count_reached && was_active && !node.flags.disable_activate_successors)
	{
		activate_successors (transaction, block);
	}
}

void nano::active_elections::notify_observers (nano::secure::read_transaction const & transaction, nano::election_status const & status, std::vector<nano::vote_with_weight_info> const & votes)
{
	auto block = status.winner;
	auto account = block->account ();
	auto amount = node.ledger.any.block_amount (transaction, block->hash ()).value_or (0).number ();
	auto is_state_send = block->type () == block_type::state && block->is_send ();
	auto is_state_epoch = block->type () == block_type::state && block->is_epoch ();
	node.observers.blocks.notify (status, votes, account, amount, is_state_send, is_state_epoch);

	if (amount > 0)
	{
		node.observers.account_balance.notify (account, false);
		if (block->is_send ())
		{
			node.observers.account_balance.notify (block->destination (), true);
		}
	}
}

void nano::active_elections::activate_successors (nano::secure::read_transaction const & transaction, std::shared_ptr<nano::block> const & block)
{
	node.scheduler.priority.activate (transaction, block->account ());

	// Start or vote for the next unconfirmed block in the destination account
	if (block->is_send () && !block->destination ().is_zero () && block->destination () != block->account ())
	{
		node.scheduler.priority.activate (transaction, block->destination ());
	}
}

void nano::active_elections::add_election_winner_details (nano::block_hash const & hash_a, std::shared_ptr<nano::election> const & election_a)
{
	nano::lock_guard<nano::mutex> guard{ election_winner_details_mutex };
	election_winner_details.emplace (hash_a, election_a);
}

std::shared_ptr<nano::election> nano::active_elections::remove_election_winner_details (nano::block_hash const & hash_a)
{
	nano::lock_guard<nano::mutex> guard{ election_winner_details_mutex };
	std::shared_ptr<nano::election> result;
	auto existing = election_winner_details.find (hash_a);
	if (existing != election_winner_details.end ())
	{
		result = existing->second;
		election_winner_details.erase (existing);
	}
	return result;
}

void nano::active_elections::block_already_cemented_callback (nano::block_hash const & hash_a)
{
	// Depending on timing there is a situation where the election_winner_details is not reset.
	// This can happen when a block wins an election, and the block is confirmed + observer
	// called before the block hash gets added to election_winner_details. If the block is confirmed
	// callbacks have already been done, so we can safely just remove it.
	remove_election_winner_details (hash_a);
}

int64_t nano::active_elections::limit (nano::election_behavior behavior) const
{
	switch (behavior)
	{
		case nano::election_behavior::manual:
		{
			return std::numeric_limits<int64_t>::max ();
		}
		case nano::election_behavior::priority:
		{
			return static_cast<int64_t> (config.size);
		}
		case nano::election_behavior::hinted:
		{
			const uint64_t limit = config.hinted_limit_percentage * config.size / 100;
			return static_cast<int64_t> (limit);
		}
		case nano::election_behavior::optimistic:
		{
			const uint64_t limit = config.optimistic_limit_percentage * config.size / 100;
			return static_cast<int64_t> (limit);
		}
	}

	debug_assert (false, "unknown election behavior");
	return 0;
}

int64_t nano::active_elections::vacancy (nano::election_behavior behavior) const
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	switch (behavior)
	{
		case nano::election_behavior::manual:
			return std::numeric_limits<int64_t>::max ();
		case nano::election_behavior::priority:
			return limit (nano::election_behavior::priority) - static_cast<int64_t> (elections.size ());
		case nano::election_behavior::hinted:
		case nano::election_behavior::optimistic:
			return limit (behavior) - elections.size (behavior);
	}
	debug_assert (false);
	return 0;
}

void nano::active_elections::request_confirm (nano::unique_lock<nano::mutex> & lock_a)
{
	debug_assert (lock_a.owns_lock ());

	std::size_t const this_loop_target_l (elections.size ());
	auto const elections_l{ list_active_impl (this_loop_target_l) };

	lock_a.unlock ();

	nano::confirmation_solicitor solicitor (node.network, node.config);
	solicitor.prepare (node.rep_crawler.principal_representatives (std::numeric_limits<std::size_t>::max ()));

	std::size_t unconfirmed_count_l (0);
	nano::timer<std::chrono::milliseconds> elapsed (nano::timer_state::started);

	/*
	 * Loop through active elections in descending order of proof-of-work difficulty, requesting confirmation
	 *
	 * Only up to a certain amount of elections are queued for confirmation request and block rebroadcasting. The remaining elections can still be confirmed if votes arrive
	 * Elections extending the soft config.size limit are flushed after a certain time-to-live cutoff
	 * Flushed elections are later re-activated via frontier confirmation
	 */
	for (auto const & election_l : elections_l)
	{
		bool const confirmed_l (election_l->confirmed ());
		unconfirmed_count_l += !confirmed_l;

		if (election_l->transition_time (solicitor))
		{
			erase (election_l->qualified_root);
		}
	}

	solicitor.flush ();
	lock_a.lock ();
}

std::vector<std::shared_ptr<nano::election>> nano::active_elections::list_active (std::size_t max_a)
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	return list_active_impl (max_a);
}

std::vector<std::shared_ptr<nano::election>> nano::active_elections::list_active_impl (std::size_t max_a) const
{
	std::vector<std::shared_ptr<nano::election>> result_l;
	result_l.reserve (std::min (max_a, elections.size ()));
	{
		auto listed (elections.list ());
		std::size_t count_l{ 0 };
		for (auto i = listed.begin (), n = listed.end (); i != n && count_l < max_a; ++i, ++count_l)
		{
			result_l.push_back (*i);
		}
	}
	return result_l;
}

void nano::active_elections::request_loop ()
{
	nano::unique_lock<nano::mutex> lock{ mutex };
	while (!stopped)
	{
		auto const stamp_l = std::chrono::steady_clock::now ();

		node.stats.inc (nano::stat::type::active, nano::stat::detail::loop);

		request_confirm (lock);
		debug_assert (lock.owns_lock ());

		if (!stopped)
		{
			auto const min_sleep_l = std::chrono::milliseconds (node.network_params.network.aec_loop_interval_ms / 2);
			auto const wakeup_l = std::max (stamp_l + std::chrono::milliseconds (node.network_params.network.aec_loop_interval_ms), std::chrono::steady_clock::now () + min_sleep_l);
			condition.wait_until (lock, wakeup_l, [&wakeup_l, &stopped = stopped] { return stopped || std::chrono::steady_clock::now () >= wakeup_l; });
		}
	}
}

std::size_t nano::active_elections::election_winner_details_size ()
{
	nano::lock_guard<nano::mutex> guard{ election_winner_details_mutex };
	return election_winner_details.size ();
}

std::unique_ptr<nano::container_info_component> nano::collect_container_info (active_elections & active_elections, std::string const & name)
{
	nano::lock_guard<nano::mutex> guard{ active_elections.mutex };

	auto composite = std::make_unique<container_info_composite> (name);
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "elections", active_elections.elections.size (), sizeof (decltype (active_elections.elections)::value_type) }));
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "election_winner_details", active_elections.election_winner_details_size (), sizeof (decltype (active_elections.election_winner_details)::value_type) }));

	composite->add_component (active_elections.recently_confirmed.collect_container_info ("recently_confirmed"));
	composite->add_component (active_elections.recently_cemented.collect_container_info ("recently_cemented"));

	return composite;
}

nano::stat::type nano::active_elections::to_completion_type (nano::election_state state)
{
	switch (state)
	{
		case election_state::passive:
		case election_state::active:
			return nano::stat::type::active_dropped;
			break;
		case election_state::confirmed:
		case election_state::expired_confirmed:
			return nano::stat::type::active_confirmed;
			break;
		case election_state::expired_unconfirmed:
			return nano::stat::type::active_timeout;
			break;
		case election_state::cancelled:
			return nano::stat::type::active_cancelled;
			break;
	}
	debug_assert (false);
	return {};
}

/*
 * active_elections_config
 */

nano::active_elections_config::active_elections_config (const nano::network_constants & network_constants)
{
}

nano::error nano::active_elections_config::serialize (nano::tomlconfig & toml) const
{
	toml.put ("size", size, "Number of active elections. Elections beyond this limit have limited survival time.\nWarning: modifying this value may result in a lower confirmation rate. \ntype:uint64,[250..]");
	toml.put ("hinted_limit_percentage", hinted_limit_percentage, "Limit of hinted elections as percentage of `active_elections_size` \ntype:uint64");
	toml.put ("optimistic_limit_percentage", optimistic_limit_percentage, "Limit of optimistic elections as percentage of `active_elections_size`. \ntype:uint64");
	toml.put ("confirmation_history_size", confirmation_history_size, "Maximum confirmation history size. If tracking the rate of block confirmations, the websocket feature is recommended instead. \ntype:uint64");
	toml.put ("confirmation_cache", confirmation_cache, "Maximum number of confirmed elections kept in cache to prevent restarting an election. \ntype:uint64");

	return toml.get_error ();
}

nano::error nano::active_elections_config::deserialize (nano::tomlconfig & toml)
{
	toml.get ("size", size);
	toml.get ("hinted_limit_percentage", hinted_limit_percentage);
	toml.get ("optimistic_limit_percentage", optimistic_limit_percentage);
	toml.get ("confirmation_history_size", confirmation_history_size);
	toml.get ("confirmation_cache", confirmation_cache);

	return toml.get_error ();
}