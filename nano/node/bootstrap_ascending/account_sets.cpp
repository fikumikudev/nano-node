#include <nano/lib/stats.hpp>
#include <nano/lib/utility.hpp>
#include <nano/node/bootstrap/bootstrap_config.hpp>
#include <nano/node/bootstrap_ascending/account_sets.hpp>

#include <algorithm>
#include <memory>
#include <vector>

/*
 * account_sets
 */

nano::bootstrap_ascending::account_sets::account_sets (nano::stats & stats_a, nano::account_sets_config config_a) :
	stats{ stats_a },
	config{ std::move (config_a) }
{
}

void nano::bootstrap_ascending::account_sets::priority_up (nano::account const & account)
{
	if (!blocked (account))
	{
		stats.inc (nano::stat::type::bootstrap_ascending_accounts, nano::stat::detail::prioritize);

		auto iter = priorities.get<tag_account> ().find (account);
		if (iter != priorities.get<tag_account> ().end ())
		{
			priorities.get<tag_account> ().modify (iter, [] (auto & val) {
				val.priority = std::min ((val.priority * account_sets::priority_increase), account_sets::priority_max);
			});
		}
		else
		{
			priorities.get<tag_account> ().insert ({ account, account_sets::priority_initial });
			stats.inc (nano::stat::type::bootstrap_ascending_accounts, nano::stat::detail::priority_insert);

			trim_overflow ();
		}
	}
	else
	{
		stats.inc (nano::stat::type::bootstrap_ascending_accounts, nano::stat::detail::prioritize_failed);
	}
}

void nano::bootstrap_ascending::account_sets::priority_down (nano::account const & account)
{
	auto iter = priorities.get<tag_account> ().find (account);
	if (iter != priorities.get<tag_account> ().end ())
	{
		stats.inc (nano::stat::type::bootstrap_ascending_accounts, nano::stat::detail::deprioritize);

		auto priority_new = iter->priority - account_sets::priority_decrease;
		if (priority_new <= account_sets::priority_cutoff)
		{
			priorities.get<tag_account> ().erase (iter);
			stats.inc (nano::stat::type::bootstrap_ascending_accounts, nano::stat::detail::priority_erase_threshold);
		}
		else
		{
			priorities.get<tag_account> ().modify (iter, [priority_new] (auto & val) {
				val.priority = priority_new;
			});
		}
	}
	else
	{
		stats.inc (nano::stat::type::bootstrap_ascending_accounts, nano::stat::detail::deprioritize_failed);
	}
}

void nano::bootstrap_ascending::account_sets::block (nano::account const & account, nano::block_hash const & dependency)
{
	stats.inc (nano::stat::type::bootstrap_ascending_accounts, nano::stat::detail::block);

	auto existing = priorities.get<tag_account> ().find (account);
	auto entry = existing == priorities.get<tag_account> ().end () ? priority_entry{ 0, 0 } : *existing;

	priorities.get<tag_account> ().erase (account);
	stats.inc (nano::stat::type::bootstrap_ascending_accounts, nano::stat::detail::priority_erase_block);

	blocking.get<tag_account> ().insert ({ account, dependency, entry });
	stats.inc (nano::stat::type::bootstrap_ascending_accounts, nano::stat::detail::blocking_insert);

	trim_overflow ();
}

void nano::bootstrap_ascending::account_sets::unblock (nano::account const & account, std::optional<nano::block_hash> const & hash)
{
	// Unblock only if the dependency is fulfilled
	auto existing = blocking.get<tag_account> ().find (account);
	if (existing != blocking.get<tag_account> ().end () && (!hash || existing->dependency == *hash))
	{
		stats.inc (nano::stat::type::bootstrap_ascending_accounts, nano::stat::detail::unblock);

		debug_assert (priorities.get<tag_account> ().count (account) == 0);
		if (!existing->original_entry.account.is_zero ())
		{
			debug_assert (existing->original_entry.account == account);
			priorities.get<tag_account> ().insert (existing->original_entry);
		}
		else
		{
			priorities.get<tag_account> ().insert ({ account, account_sets::priority_initial });
		}
		blocking.get<tag_account> ().erase (account);

		trim_overflow ();
	}
	else
	{
		stats.inc (nano::stat::type::bootstrap_ascending_accounts, nano::stat::detail::unblock_failed);
	}
}

void nano::bootstrap_ascending::account_sets::timestamp_set (const nano::account & account)
{
	auto iter = priorities.get<tag_account> ().find (account);
	if (iter != priorities.get<tag_account> ().end ())
	{
		priorities.get<tag_account> ().modify (iter, [] (auto & entry) {
			entry.timestamp = std::chrono::steady_clock::now ();
		});
	}
}

void nano::bootstrap_ascending::account_sets::timestamp_reset (const nano::account & account)
{
	auto iter = priorities.get<tag_account> ().find (account);
	if (iter != priorities.get<tag_account> ().end ())
	{
		priorities.get<tag_account> ().modify (iter, [] (auto & entry) {
			entry.timestamp = {};
		});
	}
}

// Returns false if the account is busy
bool nano::bootstrap_ascending::account_sets::check_timestamp (const nano::account & account) const
{
	auto iter = priorities.get<tag_account> ().find (account);
	if (iter != priorities.get<tag_account> ().end ())
	{
		auto const cutoff = std::chrono::steady_clock::now () - config.cooldown;
		if (iter->timestamp > cutoff)
		{
			return false;
		}
	}
	return true;
}

void nano::bootstrap_ascending::account_sets::trim_overflow ()
{
	while (priorities.size () > config.priorities_max)
	{
		// Erase the oldest entry
		priorities.pop_front ();
		stats.inc (nano::stat::type::bootstrap_ascending_accounts, nano::stat::detail::priority_erase_overflow);
	}
	while (blocking.size () > config.blocking_max)
	{
		// Erase the oldest entry
		blocking.pop_front ();
		stats.inc (nano::stat::type::bootstrap_ascending_accounts, nano::stat::detail::blocking_erase_overflow);
	}
}

nano::account nano::bootstrap_ascending::account_sets::next_priority ()
{
	if (priorities.empty ())
	{
		return { 0 };
	}

	std::vector<float> weights;
	std::vector<nano::account> candidates;

	int iterations = 0;
	while (candidates.size () < config.consideration_count && iterations++ < config.consideration_count * 10)
	{
		debug_assert (candidates.size () == weights.size ());

		// Use a dedicated, uniformly distributed field for sampling to avoid problematic corner case when accounts in the queue are very close together
		auto search = nano::bootstrap_ascending::generate_id ();
		auto iter = priorities.get<tag_id> ().lower_bound (search);
		if (iter == priorities.get<tag_id> ().end ())
		{
			iter = priorities.get<tag_id> ().begin ();
		}

		if (check_timestamp (iter->account))
		{
			candidates.push_back (iter->account);
			weights.push_back (iter->priority);
		}
	}

	if (candidates.empty ())
	{
		return { 0 }; // All sampled accounts are busy
	}

	std::discrete_distribution dist{ weights.begin (), weights.end () };
	auto selection = dist (rng);
	debug_assert (!weights.empty () && selection < weights.size ());
	auto result = candidates[selection];
	return result;
}

nano::block_hash nano::bootstrap_ascending::account_sets::next_blocking ()
{
	if (blocking.empty ())
	{
		return { 0 };
	}

	// Use a dedicated, uniformly distributed field for sampling to avoid problematic corner case when accounts in the queue are very close together
	auto search = nano::bootstrap_ascending::generate_id ();
	auto iter = blocking.get<tag_id> ().lower_bound (search);
	if (iter == blocking.get<tag_id> ().end ())
	{
		iter = blocking.get<tag_id> ().begin ();
	}

	auto result = iter->dependency;
	return result;
}

bool nano::bootstrap_ascending::account_sets::blocked (nano::account const & account) const
{
	return blocking.get<tag_account> ().count (account) > 0;
}

std::size_t nano::bootstrap_ascending::account_sets::priority_size () const
{
	return priorities.size ();
}

std::size_t nano::bootstrap_ascending::account_sets::blocked_size () const
{
	return blocking.size ();
}

float nano::bootstrap_ascending::account_sets::priority (nano::account const & account) const
{
	if (blocked (account))
	{
		return 0.0f;
	}
	auto existing = priorities.get<tag_account> ().find (account);
	if (existing != priorities.get<tag_account> ().end ())
	{
		return existing->priority;
	}
	return account_sets::priority_cutoff;
}

auto nano::bootstrap_ascending::account_sets::info () const -> nano::bootstrap_ascending::account_sets::info_t
{
	return { blocking, priorities };
}

std::unique_ptr<nano::container_info_component> nano::bootstrap_ascending::account_sets::collect_container_info (const std::string & name)
{
	auto composite = std::make_unique<container_info_composite> (name);
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "priorities", priorities.size (), sizeof (decltype (priorities)::value_type) }));
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "blocking", blocking.size (), sizeof (decltype (blocking)::value_type) }));
	return composite;
}
