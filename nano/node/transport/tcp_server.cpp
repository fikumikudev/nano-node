#include <nano/node/bootstrap/bootstrap_bulk_push.hpp>
#include <nano/node/bootstrap/bootstrap_frontier.hpp>
#include <nano/node/messages.hpp>
#include <nano/node/node.hpp>
#include <nano/node/transport/message_deserializer.hpp>
#include <nano/node/transport/tcp.hpp>
#include <nano/node/transport/tcp_server.hpp>

#include <boost/format.hpp>

/*
 * tcp_listener
 */

nano::transport::tcp_listener::tcp_listener (uint16_t port_a, nano::node & node_a) :
	node (node_a),
	port (port_a)
{
}

void nano::transport::tcp_listener::start ()
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	on = true;
	listening_socket = std::make_shared<nano::transport::server_socket> (node, boost::asio::ip::tcp::endpoint (boost::asio::ip::address_v6::any (), port), node.config.tcp_incoming_connections_max);
	boost::system::error_code ec;
	listening_socket->start (ec);
	if (ec)
	{
		node.logger.always_log (boost::str (boost::format ("Network: Error while binding for incoming TCP/bootstrap on port %1%: %2%") % listening_socket->listening_port () % ec.message ()));
		throw std::runtime_error (ec.message ());
	}

	// the user can either specify a port value in the config or it can leave the choice up to the OS:
	// (1): port specified
	// (2): port not specified
	//
	const auto listening_port = listening_socket->listening_port ();
	{
		// (1) -- nothing to do, just check that port values match everywhere
		//
		if (port == listening_port)
		{
			debug_assert (port == node.network.port);
			debug_assert (port == node.network.endpoint ().port ());
		}
		// (2) -- OS port choice happened at TCP socket bind time, so propagate this port value back;
		// the propagation is done here for the `tcp_listener` itself, whereas for `network`, the node does it
		// after calling `tcp_listener.start ()`
		//
		else
		{
			port = listening_port;
		}
	}

	listening_socket->on_connection ([this] (std::shared_ptr<nano::transport::socket> const & new_connection, boost::system::error_code const & ec_a) {
		if (!ec_a)
		{
			accept_action (ec_a, new_connection);
		}
		return true;
	});
}

void nano::transport::tcp_listener::stop ()
{
	decltype (connections) connections_l;
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		on = false;
		connections_l.swap (connections);
	}
	if (listening_socket)
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		listening_socket->close ();
		listening_socket = nullptr;
	}
}

std::size_t nano::transport::tcp_listener::connection_count ()
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	return connections.size ();
}

void nano::transport::tcp_listener::accept_action (boost::system::error_code const & ec, std::shared_ptr<nano::transport::socket> const & socket)
{
	auto channel = node.network.tcp_channels.create_channel (socket, /* temporary */ true);
	if (!channel)
	{
		// TODO: stat & log
		return;
	}

	auto server = std::make_shared<nano::transport::tcp_server> (socket, channel, node.shared (), /* allow bootstrap */ true);
	nano::lock_guard<nano::mutex> lock{ mutex };
	connections[server.get ()] = server;
	server->start ();
}

boost::asio::ip::tcp::endpoint nano::transport::tcp_listener::endpoint ()
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	if (on && listening_socket)
	{
		return boost::asio::ip::tcp::endpoint (boost::asio::ip::address_v6::loopback (), port);
	}
	else
	{
		return boost::asio::ip::tcp::endpoint (boost::asio::ip::address_v6::loopback (), 0);
	}
}

std::unique_ptr<nano::container_info_component> nano::transport::collect_container_info (nano::transport::tcp_listener & bootstrap_listener, std::string const & name)
{
	auto sizeof_element = sizeof (decltype (bootstrap_listener.connections)::value_type);
	auto composite = std::make_unique<container_info_composite> (name);
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "connections", bootstrap_listener.connection_count (), sizeof_element }));
	return composite;
}

/*
 * tcp_server
 */

nano::transport::tcp_server::tcp_server (std::shared_ptr<nano::transport::socket> socket_a, std::shared_ptr<nano::transport::channel_tcp> channel_a, std::shared_ptr<nano::node> node_a, bool allow_bootstrap_a) :
	socket{ std::move (socket_a) },
	channel{ std::move (channel_a) },
	node{ std::move (node_a) },
	allow_bootstrap{ allow_bootstrap_a },
	message_deserializer{ std::make_shared<nano::transport::message_deserializer> (node->network_params.network, node->network.publish_filter, node->block_uniquer, node->vote_uniquer) }
{
	debug_assert (socket != nullptr);
}

nano::transport::tcp_server::~tcp_server ()
{
	if (node->config.logging.bulk_pull_logging ())
	{
		node->logger.try_log ("Exiting incoming TCP/bootstrap server");
	}

	if (socket->type () == nano::transport::socket::type_t::bootstrap)
	{
		--node->tcp_listener.bootstrap_count;
	}
	else if (socket->type () == nano::transport::socket::type_t::realtime)
	{
		--node->tcp_listener.realtime_count;

		// Clear temporary channel
		auto exisiting_response_channel (node->network.tcp_channels.find_channel (remote_endpoint));
		if (exisiting_response_channel != nullptr)
		{
			exisiting_response_channel->temporary = false;
			node->network.tcp_channels.erase (remote_endpoint);
		}
	}

	stop ();

	nano::lock_guard<nano::mutex> lock{ node->tcp_listener.mutex };
	node->tcp_listener.connections.erase (this);
}

void nano::transport::tcp_server::start ()
{
	// Set remote_endpoint
	if (remote_endpoint.port () == 0)
	{
		remote_endpoint = socket->remote_endpoint ();
		debug_assert (remote_endpoint.port () != 0);
	}
	receive_message ();
}

void nano::transport::tcp_server::stop ()
{
	if (!stopped.exchange (true))
	{
		socket->close ();
	}
}

void nano::transport::tcp_server::receive_message ()
{
	if (stopped)
	{
		return;
	}

	message_deserializer->read (socket, [this_l = shared_from_this ()] (boost::system::error_code ec, std::unique_ptr<nano::message> message) {
		if (ec)
		{
			// IO error or critical error when deserializing message
			this_l->node->stats.inc (nano::stat::type::error, nano::transport::message_deserializer::to_stat_detail (this_l->message_deserializer->status));
			this_l->stop ();
		}
		else
		{
			this_l->received_message (std::move (message));
		}
	});
}

void nano::transport::tcp_server::received_message (std::unique_ptr<nano::message> message)
{
	bool should_continue = true;
	if (message)
	{
		should_continue = process_message (std::move (message));
	}
	else
	{
		// Error while deserializing message
		debug_assert (message_deserializer->status != transport::message_deserializer::parse_status::success);
		node->stats.inc (nano::stat::type::error, nano::transport::message_deserializer::to_stat_detail (message_deserializer->status));
		if (message_deserializer->status == transport::message_deserializer::parse_status::duplicate_publish_message)
		{
			node->stats.inc (nano::stat::type::filter, nano::stat::detail::duplicate_publish);
		}
	}

	if (should_continue)
	{
		receive_message ();
	}
}

bool nano::transport::tcp_server::process_message (std::unique_ptr<nano::message> message)
{
	node->stats.inc (nano::stat::type::tcp_server, nano::to_stat_detail (message->header.type), nano::stat::dir::in);

	debug_assert (is_undefined_connection () || is_realtime_connection () || is_bootstrap_connection ());

	/*
	 * Server initially starts in undefined state, where it waits for either a handshake or booststrap request message
	 * If the server receives a handshake (and it is successfully validated) it will switch to a realtime mode.
	 * In realtime mode messages are deserialized and queued to `tcp_message_manager` for further processing.
	 * In realtime mode any bootstrap requests are ignored.
	 *
	 * If the server receives a bootstrap request before receiving a handshake, it will switch to a bootstrap mode.
	 * In bootstrap mode once a valid bootstrap request message is received, the server will start a corresponding bootstrap server and pass control to that server.
	 * Once that server finishes its task, control is passed back to this server to read and process any subsequent messages.
	 * In bootstrap mode any realtime messages are ignored
	 */
	if (is_undefined_connection ())
	{
		handshake_message_visitor handshake_visitor{ shared_from_this () };
		message->visit (handshake_visitor);
		if (handshake_visitor.bootstrap)
		{
			if (!to_bootstrap_connection ())
			{
				stop ();
				return false;
			}
		}
		else
		{
			// Neither handshake nor bootstrap received when in handshake mode
			return true;
		}
	}
	else if (is_realtime_connection ())
	{
		realtime_message_visitor realtime_visitor{ *this };
		message->visit (realtime_visitor);
		if (realtime_visitor.process)
		{
			queue_realtime (std::move (message));
		}
		return true;
	}
	// The server will switch to bootstrap mode immediately after processing the first bootstrap message, thus no `else if`
	if (is_bootstrap_connection ())
	{
		bootstrap_message_visitor bootstrap_visitor{ shared_from_this () };
		message->visit (bootstrap_visitor);
		return !bootstrap_visitor.processed; // Stop receiving new messages if bootstrap serving started
	}
	debug_assert (false);
	return true; // Continue receiving new messages
}

void nano::transport::tcp_server::queue_realtime (std::unique_ptr<nano::message> message)
{
	debug_assert (!channel->get_node_id ().is_zero ()); // Handshake should be completed prior to switching to realtime mode
	node->network.tcp_message_manager.put_message (nano::tcp_message_item{ std::move (message), remote_endpoint, remote_node_id, socket, channel });
}

/*
 * Handshake
 */

nano::transport::tcp_server::handshake_message_visitor::handshake_message_visitor (std::shared_ptr<tcp_server> server) :
	server{ std::move (server) }
{
}

void nano::transport::tcp_server::handshake_message_visitor::node_id_handshake (nano::node_id_handshake const & message)
{
	if (server->node->flags.disable_tcp_realtime)
	{
		if (server->node->config.logging.network_node_id_handshake_logging ())
		{
			server->node->logger.always_log (boost::str (boost::format ("Disabled realtime TCP for handshake %1%") % server->remote_endpoint));
		}
		// Stop invalid handshake
		server->stop ();
		return;
	}

	if (server->node->config.logging.network_node_id_handshake_logging ())
	{
		server->node->logger.always_log (boost::str (boost::format ("Received node_id_handshake message from %1% | query: %2% | response: %3%") % server->remote_endpoint % message.query.has_value () % message.response.has_value ()));
	}

	if (message.response)
	{
		if (!server->verify_handshake_response (*message.response))
		{
			if (server->node->config.logging.network_node_id_handshake_logging ())
			{
				server->node->logger.always_log (boost::str (boost::format ("Invalid node_id_handshake response from %1%") % server->remote_endpoint));
			}
			// Stop invalid handshake
			server->stop ();
			return;
		}
		else
		{
			if (server->node->config.logging.network_node_id_handshake_logging ())
			{
				server->node->logger.always_log (boost::str (boost::format ("OK node_id_handshake response from %1%, upgrading to realtime") % server->remote_endpoint));
			}
			server->to_realtime_connection (message.response->node_id, message.header.version_using);
		}
	}
	if (message.query)
	{
		if (server->handshake_query_received)
		{
			if (server->node->config.logging.network_node_id_handshake_logging ())
			{
				server->node->logger.always_log (boost::str (boost::format ("Detected multiple node_id_handshake query from %1%") % server->remote_endpoint));
			}
			// Stop invalid handshake
			server->stop ();
			return;
		}
		else
		{
			server->handshake_query_received = true;
			server->send_handshake_response (*message.query, /* send own query with response only if we haven't sent our query already */ !server->handshake_query_sent);
		}
	}

	process = true; // Keep processing new messages
}

void nano::transport::tcp_server::send_handshake_query ()
{
	handshake_query_sent = true;

	std::optional<nano::node_id_handshake::query_payload> query;
	if (auto cookie = node->network.syn_cookies.assign (nano::transport::map_tcp_to_endpoint (remote_endpoint)); cookie)
	{
		nano::node_id_handshake::query_payload pld{ *cookie };
		query = pld;
	}
	else
	{
		// Error assigning cookie (too many connections per IP?)
		// TODO: Stat & log
		stop ();
		return;
	}

	nano::node_id_handshake handshake_request{ node->network_params.network, query };

	if (node->config.logging.network_node_id_handshake_logging ())
	{
		node->logger.always_log (boost::str (boost::format ("Node ID handshake request sent with node ID %1% to %2%: query %3%") % node->node_id.pub.to_node_id () % remote_endpoint % (query ? query->cookie.to_string () : "not set")));
	}

	// TODO: Use channel
	auto shared_const_buffer = handshake_request.to_shared_const_buffer ();
	socket->async_write (shared_const_buffer, [this_l = shared_from_this ()] (boost::system::error_code const & ec, std::size_t size_a) {
		if (ec)
		{
			if (this_l->node->config.logging.network_node_id_handshake_logging ())
			{
				this_l->node->logger.always_log (boost::str (boost::format ("Error sending node_id_handshake query to %1%: %2%") % this_l->remote_endpoint % ec.message ()));
			}
			// Stop invalid handshake
			this_l->stop ();
		}
		else
		{
			this_l->node->stats.inc (nano::stat::type::message, nano::stat::detail::node_id_handshake, nano::stat::dir::out);
		}
	});
}

bool nano::transport::tcp_server::verify_handshake_response (const nano::node_id_handshake::response_payload & response)
{
	// Check if signature OK
	if (node->network.syn_cookies.validate (nano::transport::map_tcp_to_endpoint (remote_endpoint), response.node_id, response.signature))
	{
		return false; // Fail
	}
	// Prevent connection with ourselves
	if (response.node_id == node->node_id.pub)
	{
		return false; // Fail
	}
	return true; // Success
}

void nano::transport::tcp_server::send_handshake_response (nano::node_id_handshake::query_payload const & query, bool send_own_query)
{
	nano::node_id_handshake::response_payload response{ node->node_id.pub, nano::sign_message (node->node_id.prv, node->node_id.pub, query.cookie) };
	debug_assert (!nano::validate_message (response.node_id, query.cookie, response.signature));

	std::optional<nano::node_id_handshake::query_payload> own_query;
	if (send_own_query)
	{
		if (auto own_cookie = node->network.syn_cookies.assign (nano::transport::map_tcp_to_endpoint (remote_endpoint)); own_cookie)
		{
			nano::node_id_handshake::query_payload pld{ *own_cookie };
			own_query = pld;
		}
		else
		{
			// Error assigning cookie (too many connections per IP?)
			// Not critical, the other side will still receive our query response
			// TODO: Stat & log
		}
	}

	nano::node_id_handshake handshake_response{ node->network_params.network, own_query, response };

	if (node->config.logging.network_node_id_handshake_logging ())
	{
		node->logger.always_log (boost::str (boost::format ("Node ID handshake response sent with node ID %1% to %2%: query %3% | own_query: %4%") % node->node_id.pub.to_node_id () % remote_endpoint % query.cookie.to_string () % (own_query ? own_query->cookie.to_string () : "not set")));
	}

	// TODO: Use channel
	auto shared_const_buffer = handshake_response.to_shared_const_buffer ();
	socket->async_write (shared_const_buffer, [this_l = shared_from_this ()] (boost::system::error_code const & ec, std::size_t size_a) {
		if (ec)
		{
			if (this_l->node->config.logging.network_node_id_handshake_logging ())
			{
				this_l->node->logger.always_log (boost::str (boost::format ("Error sending node_id_handshake response to %1%: %2%") % this_l->remote_endpoint % ec.message ()));
			}
			// Stop invalid handshake
			this_l->stop ();
		}
		else
		{
			this_l->node->stats.inc (nano::stat::type::message, nano::stat::detail::node_id_handshake, nano::stat::dir::out);
		}
	});
}

void nano::transport::tcp_server::handshake_message_visitor::bulk_pull (const nano::bulk_pull & message)
{
	bootstrap = true;
}

void nano::transport::tcp_server::handshake_message_visitor::bulk_pull_account (const nano::bulk_pull_account & message)
{
	bootstrap = true;
}

void nano::transport::tcp_server::handshake_message_visitor::bulk_push (const nano::bulk_push & message)
{
	bootstrap = true;
}

void nano::transport::tcp_server::handshake_message_visitor::frontier_req (const nano::frontier_req & message)
{
	bootstrap = true;
}

/*
 * Realtime
 */

nano::transport::tcp_server::realtime_message_visitor::realtime_message_visitor (nano::transport::tcp_server & server_a) :
	server{ server_a }
{
}

void nano::transport::tcp_server::realtime_message_visitor::keepalive (const nano::keepalive & message)
{
	process = true;
}

void nano::transport::tcp_server::realtime_message_visitor::publish (const nano::publish & message)
{
	process = true;
}

void nano::transport::tcp_server::realtime_message_visitor::confirm_req (const nano::confirm_req & message)
{
	process = true;
}

void nano::transport::tcp_server::realtime_message_visitor::confirm_ack (const nano::confirm_ack & message)
{
	process = true;
}

void nano::transport::tcp_server::realtime_message_visitor::frontier_req (const nano::frontier_req & message)
{
	process = true;
}

void nano::transport::tcp_server::realtime_message_visitor::telemetry_req (const nano::telemetry_req & message)
{
	// Only handle telemetry requests if they are outside the cooldown period
	if (server.last_telemetry_req + server.node->network_params.network.telemetry_request_cooldown < std::chrono::steady_clock::now ())
	{
		server.last_telemetry_req = std::chrono::steady_clock::now ();
		process = true;
	}
	else
	{
		server.node->stats.inc (nano::stat::type::telemetry, nano::stat::detail::request_within_protection_cache_zone);
	}
}

void nano::transport::tcp_server::realtime_message_visitor::telemetry_ack (const nano::telemetry_ack & message)
{
	process = true;
}

void nano::transport::tcp_server::realtime_message_visitor::asc_pull_req (const nano::asc_pull_req & message)
{
	process = true;
}

void nano::transport::tcp_server::realtime_message_visitor::asc_pull_ack (const nano::asc_pull_ack & message)
{
	process = true;
}

/*
 * Bootstrap
 */

nano::transport::tcp_server::bootstrap_message_visitor::bootstrap_message_visitor (std::shared_ptr<tcp_server> server) :
	server{ std::move (server) }
{
}

void nano::transport::tcp_server::bootstrap_message_visitor::bulk_pull (const nano::bulk_pull & message)
{
	if (server->node->flags.disable_bootstrap_bulk_pull_server)
	{
		return;
	}

	if (server->node->config.logging.bulk_pull_logging ())
	{
		server->node->logger.try_log (boost::str (boost::format ("Received bulk pull for %1% down to %2%, maximum of %3% from %4%") % message.start.to_string () % message.end.to_string () % message.count % server->remote_endpoint));
	}

	server->node->bootstrap_workers.push_task ([server = server, message = message] () {
		// TODO: Add completion callback to bulk pull server
		// TODO: There should be no need to re-copy message as unique pointer, refactor those bulk/frontier pull/push servers
		auto bulk_pull_server = std::make_shared<nano::bulk_pull_server> (server, std::make_unique<nano::bulk_pull> (message));
		bulk_pull_server->send_next ();
	});

	processed = true;
}

void nano::transport::tcp_server::bootstrap_message_visitor::bulk_pull_account (const nano::bulk_pull_account & message)
{
	if (server->node->flags.disable_bootstrap_bulk_pull_server)
	{
		return;
	}

	if (server->node->config.logging.bulk_pull_logging ())
	{
		server->node->logger.try_log (boost::str (boost::format ("Received bulk pull account for %1% with a minimum amount of %2%") % message.account.to_account () % nano::amount (message.minimum_amount).format_balance (nano::Mxrb_ratio, 10, true)));
	}

	server->node->bootstrap_workers.push_task ([server = server, message = message] () {
		// TODO: Add completion callback to bulk pull server
		// TODO: There should be no need to re-copy message as unique pointer, refactor those bulk/frontier pull/push servers
		auto bulk_pull_account_server = std::make_shared<nano::bulk_pull_account_server> (server, std::make_unique<nano::bulk_pull_account> (message));
		bulk_pull_account_server->send_frontier ();
	});

	processed = true;
}

void nano::transport::tcp_server::bootstrap_message_visitor::bulk_push (const nano::bulk_push &)
{
	server->node->bootstrap_workers.push_task ([server = server] () {
		// TODO: Add completion callback to bulk pull server
		auto bulk_push_server = std::make_shared<nano::bulk_push_server> (server);
		bulk_push_server->throttled_receive ();
	});

	processed = true;
}

void nano::transport::tcp_server::bootstrap_message_visitor::frontier_req (const nano::frontier_req & message)
{
	if (server->node->config.logging.bulk_pull_logging ())
	{
		server->node->logger.try_log (boost::str (boost::format ("Received frontier request for %1% with age %2%") % message.start.to_string () % message.age));
	}

	server->node->bootstrap_workers.push_task ([server = server, message = message] () {
		// TODO: There should be no need to re-copy message as unique pointer, refactor those bulk/frontier pull/push servers
		auto response = std::make_shared<nano::frontier_req_server> (server, std::make_unique<nano::frontier_req> (message));
		response->send_next ();
	});

	processed = true;
}

// TODO: We could periodically call this (from a dedicated timeout thread for eg.) but socket already handles timeouts,
//  and since we only ever store tcp_server as weak_ptr, socket timeout will automatically trigger tcp_server cleanup
void nano::transport::tcp_server::timeout ()
{
	if (socket->has_timed_out ())
	{
		if (node->config.logging.bulk_pull_logging ())
		{
			node->logger.try_log ("Closing incoming tcp / bootstrap server by timeout");
		}
		{
			nano::lock_guard<nano::mutex> lock{ node->tcp_listener.mutex };
			node->tcp_listener.connections.erase (this);
		}
		socket->close ();
	}
}

bool nano::transport::tcp_server::to_bootstrap_connection ()
{
	if (!allow_bootstrap)
	{
		return false;
	}
	if (node->flags.disable_bootstrap_listener)
	{
		return false;
	}
	if (node->tcp_listener.bootstrap_count >= node->config.bootstrap_connections_max)
	{
		return false;
	}
	if (socket->type () != nano::transport::socket::type_t::undefined)
	{
		return false;
	}

	++node->tcp_listener.bootstrap_count;
	socket->type_set (nano::transport::socket::type_t::bootstrap);
	return true;
}

bool nano::transport::tcp_server::to_realtime_connection (nano::account const & node_id, uint8_t network_version)
{
	if (socket->type () == nano::transport::socket::type_t::undefined && !node->flags.disable_tcp_realtime)
	{
		remote_node_id = node_id;

		// Setup channel with remote node info
		channel->set_node_id (remote_node_id);
		channel->set_network_version (network_version);

		// Insert channel into tcp channels container, so network knows about it
		node->network.tcp_channels.insert (channel, socket, shared_from_this ());

		++node->tcp_listener.realtime_count;
		socket->type_set (nano::transport::socket::type_t::realtime);
		return true;
	}
	return false;
}

bool nano::transport::tcp_server::is_undefined_connection () const
{
	return socket->type () == nano::transport::socket::type_t::undefined;
}

bool nano::transport::tcp_server::is_bootstrap_connection () const
{
	return socket->is_bootstrap_connection ();
}

bool nano::transport::tcp_server::is_realtime_connection () const
{
	return socket->is_realtime_connection ();
}
