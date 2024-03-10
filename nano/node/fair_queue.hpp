#pragma once

#include <nano/lib/locks.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/lib/stats.hpp>
#include <nano/lib/thread_roles.hpp>
#include <nano/lib/threading.hpp>
#include <nano/lib/utility.hpp>
#include <nano/node/bandwidth_limiter.hpp>
#include <nano/node/transport/channel.hpp>

#include <boost/circular_buffer.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace nano
{
template <typename Request, typename... Sources>
class fair_queue final
{
public:
	struct source
	{
		std::tuple<Sources...> sources;
		std::shared_ptr<nano::transport::channel> channel;

		source (std::tuple<Sources...> sources, std::shared_ptr<nano::transport::channel> channel = nullptr) :
			sources{ sources },
			channel{ std::move (channel) }
		{
		}

		bool alive () const
		{
			if (channel)
			{
				return channel->alive ();
			}
			// Some sources (eg. local RPC) don't have an associated channel, never remove their queue
			return true;
		}

		auto operator<=> (source const &) const = default;

		friend std::ostream & operator<< (std::ostream & os, source const & source)
		{
			os << "sources: ";
			std::apply ([&os] (auto const &... sources) {
				((os << to_string (sources) << ", "), ...);
			},
			source.sources);
			if (source.channel)
			{
				os << " channel: " << source.channel->to_string ()
				   << " (" << source.channel->get_node_id () << ")"
				   << " [" << source.channel.get () << "]"
				   << " ( use_count: " << source.channel.use_count () << ", alive: " << source.channel->alive () << ")";
			}
			return os;
		}
	};

private:
	struct entry
	{
		using queue_t = std::deque<Request>;
		queue_t requests;

		size_t priority;
		size_t max_size;

		entry (size_t max_size, size_t priority) :
			priority{ priority },
			max_size{ max_size }
		{
		}

		Request pop ()
		{
			release_assert (!requests.empty ());

			auto request = std::move (requests.front ());
			requests.pop_front ();
			return request;
		}

		bool push (Request request)
		{
			if (requests.size () < max_size)
			{
				requests.push_back (std::move (request));
				return true; // Added
			}
			return false; // Dropped
		}

		bool empty () const
		{
			return requests.empty ();
		}

		size_t size () const
		{
			return requests.size ();
		}
	};

public:
	using source_type = source;
	using value_type = std::pair<Request, source_type>;

public:
	size_t size (source_type source) const
	{
		auto it = queues.find (source);
		return it == queues.end () ? 0 : it->second.size ();
	}

	size_t max_size (source_type source) const
	{
		auto it = queues.find (source);
		return it == queues.end () ? 0 : it->second.max_size;
	}

	size_t priority (source_type source) const
	{
		auto it = queues.find (source);
		return it == queues.end () ? 0 : it->second.priority;
	}

	size_t total_size () const
	{
		return std::accumulate (queues.begin (), queues.end (), 0, [] (size_t total, auto const & queue) {
			return total + queue.second.size ();
		});
	};

	bool empty () const
	{
		return std::all_of (queues.begin (), queues.end (), [] (auto const & queue) {
			return queue.second.empty ();
		});
	}

	size_t queues_size () const
	{
		return queues.size ();
	}

	void clear ()
	{
		queues.clear ();
	}

	/**
	 * Should be called periodically to clean up stale channels and update queue priorities and max sizes
	 */
	bool periodic_update (std::chrono::milliseconds interval = std::chrono::milliseconds{ 1000 * 30 })
	{
		if (elapsed (last_update, interval))
		{
			last_update = std::chrono::steady_clock::now ();

			cleanup ();
			update ();

			std::cout << "fair_queue:\n"
					  << *this
					  << std::endl;

			return true; // Updated
		}
		return false; // Not updated
	}

	void dump (std::ostream & os) const
	{
		for (auto const & [source, queue] : queues)
		{
			os << "queue: " << source << " - " << queue.size () << " / " << queue.max_size << " (priority: " << queue.priority << ")\n";
		}
	}

	friend std::ostream & operator<< (std::ostream & os, fair_queue const & queue)
	{
		queue.dump (os);
		return os;
	}

	/**
	 * Push a request to the appropriate queue based on the source
	 * Request will be dropped if the queue is full
	 * @return true if added, false if dropped
	 */
	bool push (Request request, source_type source)
	{
		auto it = queues.find (source);

		// Create a new queue if it doesn't exist
		if (it == queues.end ())
		{
			// TODO: Right now this is constant and initialized when the queue is created, but it could be made dynamic
			auto max_size = max_size_query (source);
			auto priority = priority_query (source);

			// It's safe to not invalidate current iterator, since std::map container guarantees that iterators are not invalidated by insert operations
			it = queues.emplace (source, entry{ max_size, priority }).first;
		}
		release_assert (it != queues.end ());

		auto & queue = it->second;
		return queue.push (std::move (request)); // True if added, false if dropped
	}

public:
	using max_size_query_t = std::function<size_t (source_type const &)>;
	using priority_query_t = std::function<size_t (source_type const &)>;

	max_size_query_t max_size_query{ [] (auto const & origin) { debug_assert (false, "max_size_query callback empty"); return 0; } };
	priority_query_t priority_query{ [] (auto const & origin) { debug_assert (false, "priority_query callback empty"); return 0; } };

public:
	value_type next ()
	{
		debug_assert (!empty ()); // Should be checked before calling next

		auto should_seek = [&, this] () {
			if (iterator == queues.end ())
			{
				return true;
			}
			auto & queue = iterator->second;
			if (queue.empty ())
			{
				return true;
			}
			// Allow up to `queue.priority` requests to be processed before moving to the next queue
			if (counter >= queue.priority)
			{
				return true;
			}
			return false;
		};

		if (should_seek ())
		{
			seek_next ();
		}

		release_assert (iterator != queues.end ());

		auto & source = iterator->first;
		auto & queue = iterator->second;

		++counter;
		return { queue.pop (), source };
	}

	std::deque<value_type> next_batch (size_t max_count)
	{
		// TODO: Naive implementation, could be optimized
		std::deque<value_type> result;
		while (!empty () && result.size () < max_count)
		{
			result.emplace_back (next ());
		}
		return result;
	}

private:
	void seek_next ()
	{
		counter = 0;
		do
		{
			if (iterator != queues.end ())
			{
				++iterator;
			}
			if (iterator == queues.end ())
			{
				iterator = queues.begin ();
			}
			release_assert (iterator != queues.end ());
		} while (iterator->second.empty ());
	}

	void cleanup ()
	{
		// Invalidate the current iterator
		iterator = queues.end ();

		erase_if (queues, [] (auto const & entry) {
			return !entry.first.alive ();
		});
	}

	void update ()
	{
		for (auto & [source, queue] : queues)
		{
			queue.max_size = max_size_query (source);
			queue.priority = priority_query (source);
		}
	}

private:
	std::map<source_type, entry> queues;
	std::map<source_type, entry>::iterator iterator{ queues.end () };
	size_t counter{ 0 };

	std::chrono::steady_clock::time_point last_update{};

public:
	std::unique_ptr<container_info_component> collect_container_info (std::string const & name)
	{
		auto composite = std::make_unique<container_info_composite> (name);
		composite->add_component (std::make_unique<container_info_leaf> (container_info{ "queues", queues.size (), sizeof (typename decltype (queues)::value_type) }));
		composite->add_component (std::make_unique<container_info_leaf> (container_info{ "total_size", total_size (), sizeof (typename decltype (queues)::value_type) }));
		return composite;
	}
};
}
