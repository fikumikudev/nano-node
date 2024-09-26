#include <nano/lib/utility.hpp>
#include <nano/node/bootstrap_ascending/database_scan.hpp>
#include <nano/secure/common.hpp>
#include <nano/secure/ledger.hpp>
#include <nano/secure/ledger_set_any.hpp>
#include <nano/store/account.hpp>
#include <nano/store/component.hpp>
#include <nano/store/pending.hpp>

/*
 * database_scan
 */

nano::bootstrap_ascending::database_scan::database_scan (nano::ledger & ledger_a) :
	ledger{ ledger_a },
	account_scanner{ ledger },
	pending_scanner{ ledger }
{
}

nano::account nano::bootstrap_ascending::database_scan::next (std::function<bool (nano::account const &)> const & filter)
{
	if (queue.empty ())
	{
		fill ();
	}

	while (!queue.empty ())
	{
		auto result = queue.front ();
		queue.pop_front ();

		if (filter (result))
		{
			return result;
		}
	}

	return { 0 };
}

void nano::bootstrap_ascending::database_scan::fill ()
{
	auto transaction = ledger.store.tx_begin_read ();

	auto set1 = account_scanner.next_batch (transaction, batch_size);
	auto set2 = pending_scanner.next_batch (transaction, batch_size);

	queue.insert (queue.end (), set1.begin (), set1.end ());
	queue.insert (queue.end (), set2.begin (), set2.end ());
}

bool nano::bootstrap_ascending::database_scan::warmed_up () const
{
	return account_scanner.completed > 0 && pending_scanner.completed > 0;
}

std::unique_ptr<nano::container_info_component> nano::bootstrap_ascending::database_scan::collect_container_info (std::string const & name) const
{
	auto composite = std::make_unique<container_info_composite> (name);
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "account_scan", account_scanner.completed, 0 }));
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "pending_scan", pending_scanner.completed, 0 }));
	return composite;
}

/*
 * account_database_iterator
 */

std::deque<nano::account> nano::bootstrap_ascending::account_database_scanner::next_batch (nano::store::transaction & transaction, size_t batch_size)
{
	std::deque<nano::account> result;

	auto it = ledger.store.account.begin (transaction, next);
	auto const end = ledger.store.account.end ();

	for (size_t count = 0; it != end && count < batch_size; ++it, ++count)
	{
		auto const & account = it->first;
		result.push_back (account);
		next = account.number () + 1;
	}

	if (it == end)
	{
		// Reset for the next ledger iteration
		next = { 0 };
		++completed;
	}

	return result;
}

/*
 * pending_database_iterator
 */

std::deque<nano::account> nano::bootstrap_ascending::pending_database_scanner::next_batch (nano::store::transaction & transaction, size_t batch_size)
{
	std::deque<nano::account> result;

	auto it = ledger.store.pending.begin (transaction, next);
	auto const end = ledger.store.pending.end ();

	// TODO: This pending iteration heuristic should be encapsulated in a pending_iterator class and reused across other components
	auto advance_iterator = [&] () {
		auto const starting_account = it->first.account;

		// For RocksDB, sequential access is ~10x faster than performing a fresh lookup (tested on my machine)
		const size_t sequential_attempts = 10;

		// First try advancing sequentially
		for (size_t count = 0; count < sequential_attempts && it != end; ++count, ++it)
		{
			if (it->first.account != starting_account)
			{
				break;
			}
		}

		// If we didn't advance to the next account, perform a fresh lookup
		if (it != end && it->first.account != starting_account)
		{
			it = ledger.store.pending.begin (transaction, { starting_account.number () + 1, 0 });
		}

		debug_assert (it == end || it->first.account != starting_account);
	};

	for (size_t count = 0; it != end && count < batch_size; advance_iterator (), ++count)
	{
		auto const & account = it->first.account;
		result.push_back (account);
		next = { account.number () + 1, 0 };
	}

	if (it == end)
	{
		// Reset for the next ledger iteration
		next = { 0, 0 };
		++completed;
	}

	return result;
}
