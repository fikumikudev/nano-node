#pragma once

#include <nano/node/bootstrap/bootstrap_attempt.hpp>

#include <random>
#include <thread>

namespace nano
{
namespace transport
{
	class channel;
}
namespace bootstrap
{
	class bootstrap_ascending
	{
	public:
		explicit bootstrap_ascending (nano::node & node_a);
		virtual ~bootstrap_ascending ();

		void init ();
		void start ();
		void stop ();
		void run ();

		// Make an account known to ascending bootstrap and set its priority
		void prioritize (nano::account const & account_a, float priority_a);

		void get_information (boost::property_tree::ptree &);
		std::unique_ptr<nano::container_info_component> collect_container_info (const std::string & name);

	private: // Dependencies
		nano::stat & stats;

	private:
		nano::node & node;
		bool stopped{ false };
		nano::condition_variable condition;
		mutable nano::mutex mutex;
		std::thread main_thread;
		void debug_log (const std::string &) const;
		// void dump_miss_histogram ();

	public:
		class async_tag;

		class connection_pool
		{
		public:
			using socket_channel = std::pair<std::shared_ptr<nano::socket>, std::shared_ptr<nano::transport::channel>>;

		public:
			connection_pool (nano::bootstrap::bootstrap_ascending & bootstrap);
			/** Given a tag context, find or create a connection to a peer and then call the op callback */
			bool operator() (std::shared_ptr<async_tag> tag, std::function<void ()> op);
			void add (socket_channel const & connection);
			std::unique_ptr<nano::container_info_component> collect_container_info (const std::string & name);

		private:
			std::deque<socket_channel> connections;
			const nano::bootstrap::bootstrap_ascending & bootstrap;
		};

		using socket_channel = connection_pool::socket_channel;

		/** This class tracks accounts various account sets which are shared among the multiple bootstrap threads */
		class account_sets
		{
		public:
			explicit account_sets (nano::stat &);

			/**
			 * If an account is not blocked, then add it to the forwarding set and, additioally, if it does not exist in the backoff set, set the priority.
			 */
			void prioritize (nano::account const & account, float priority);
			void block (nano::account const & account, nano::block_hash const & dependency);
			void unblock (nano::account const & account, nano::block_hash const & hash);
			void force_unblock (nano::account const & account);
			void dump () const;
			std::string to_string () const;
			std::unique_ptr<nano::container_info_component> collect_container_info (const std::string & name);

			/**
			 * Pop an account out of forwarding set, or get a random account, if forwardinbg set is empty.
			 * Then move it into backoff set with priority 1 so that another account can get a chance to be pulled.
			 */
			nano::account next ();

		public:
			bool blocked (nano::account const & account) const;

		private: // Dependencies
			nano::stat & stats;

		private:
			nano::account random ();

			// A forwarded account is an account that has recently had a new block inserted or has been a destination reference and therefore is a more likely candidate for furthur block retrieval
			std::unordered_set<nano::account> forwarding;
			// A blocked account is an account that has failed to insert a block because the source block is gapped.
			// An account is unblocked once it has a block successfully inserted.
			std::map<nano::account, nano::block_hash> blocking;
			// Tracks the number of requests for additional blocks without a block being successfully returned
			// Each time a block is inserted to an account, this number is reset.
			std::map<nano::account, float> backoff;

			static size_t constexpr backoff_exclusion = 4;
			std::default_random_engine rng;

			/**
				Convert a vector of attempt counts to a probability vector suitable for std::discrete_distribution
				This implementation applies 1/2^i for each element, effectivly an exponential backoff
			*/
			std::vector<double> probability_transform (std::vector<decltype (backoff)::mapped_type> const & attempts) const;

		public:
			using backoff_info_t = std::tuple<decltype (forwarding), decltype (blocking), decltype (backoff)>; // <forwarding, blocking, backoff>

			backoff_info_t backoff_info () const;
		};

		/** A single thread performing the ascending bootstrap algorithm
			Each thread tracks the number of outstanding requests over the network that have not yet completed.
		*/
		class thread : public std::enable_shared_from_this<thread>
		{
		public:
			explicit thread (bootstrap_ascending & bootstrap);

			/// Wait for there to be space for an additional request
			bool wait_available_request ();
			bool request_one ();
			void run ();
			std::shared_ptr<thread> shared ();
			nano::account pick_account ();
			// Send a request for a specific account or hash `start' to `tag' which contains a bootstrap socket.
			void send (std::shared_ptr<async_tag> tag, nano::hash_or_account const & start);
			// Reads a block from a specific `tag' / bootstrap socket.
			void read_block (std::shared_ptr<async_tag> tag);

			// given an account, pick the start point of the pull request
			nano::hash_or_account pick_start (const nano::account & account_a);

			std::atomic<int> requests{ 0 };
			static constexpr int requests_max = 4;

			bootstrap_ascending & bootstrap;
		};

		/** This class tracks the lifetime of a network request within a bootstrap attempt thread
			Each async_tag will increment the number of bootstrap requests tracked by a bootstrap_ascending::thread object
			A shared_ptr is used  for its copy semantics, as is required by callbacks through the boost asio system
			The tag also tracks success of a specific request. Success is defined by the correct receipt of a stream of blocks, followed by a not_a_block terminator
		*/
		class async_tag : public std::enable_shared_from_this<async_tag>
		{
		public:
			explicit async_tag (std::shared_ptr<nano::bootstrap::bootstrap_ascending::thread> bootstrap);

			// bootstrap_ascending::thread::requests will be decemented when destroyed.
			// If success () has been called, the socket will be reused, otherwise it will be abandoned therefore destroyed.
			~async_tag ();
			void success ();
			void connection_set (socket_channel const & connection);
			socket_channel & connection ();

			// Tracks the number of blocks received from this request
			std::atomic<int> blocks{ 0 };

		private:
			bool success_m{ false };
			std::optional<socket_channel> connection_m;

			// storing weak pointers to dependencies to avoid dependency cycles
			// which cause problems during shutdown
			std::weak_ptr<bootstrap_ascending::thread> thread_weak;
			std::weak_ptr<nano::node> node_weak;
		};

		bool blocked (nano::account const & account);
		void inspect (nano::transaction const & tx, nano::process_return const & result, nano::block const & block);
		void dump_stats ();

		account_sets::backoff_info_t backoff_info () const;

		// pull optimistically, pull unconfirmed blocks without limit
		bool optimistic_pulling{ false };

	private:
		account_sets accounts;
		connection_pool pool;

		static std::size_t constexpr parallelism = 16;
		static std::size_t constexpr request_message_count = 128;

		std::atomic<int> responses{ 0 };
		std::atomic<int> requests_total{ 0 };
		std::atomic<float> weights{ 0 };
		std::atomic<int> forwarded{ 0 };
		std::atomic<int> block_total{ 0 };
	};
}
}
