#include <nano/node/bootstrap/bootstrap_config.hpp>
#include <nano/node/bootstrap/peer_scoring.hpp>
#include <nano/node/transport/channel.hpp>

/*
 * peer_scoring
 */

nano::bootstrap::peer_scoring::peer_scoring (bootstrap_config const & config_a, nano::network_constants const & network_constants_a) :
	config{ config_a },
	network_constants{ network_constants_a }
{
}

bool nano::bootstrap::peer_scoring::limit_exceeded (std::shared_ptr<nano::transport::channel> const & channel) const
{
	auto & index = scoring.get<tag_channel> ();
	if (auto existing = index.find (channel.get ()); existing != index.end ())
	{
		return existing->outstanding >= config.channel_limit;
	}
	return false;
}

bool nano::bootstrap::peer_scoring::try_send_message (std::shared_ptr<nano::transport::channel> const & channel)
{
	auto & index = scoring.get<tag_channel> ();
	auto existing = index.find (channel.get ());
	if (existing == index.end ())
	{
		index.emplace (channel, 1, 1, 0);
	}
	else
	{
		if (existing->outstanding < config.channel_limit)
		{
			[[maybe_unused]] auto success = index.modify (existing, [] (auto & score) {
				++score.outstanding;
				++score.request_count_total;
			});
			debug_assert (success);
		}
		else
		{
			return true;
		}
	}
	return false;
}

void nano::bootstrap::peer_scoring::received_message (std::shared_ptr<nano::transport::channel> const & channel)
{
	auto & index = scoring.get<tag_channel> ();
	if (auto existing = index.find (channel.get ()); existing != index.end ())
	{
		if (existing->outstanding > 1)
		{
			[[maybe_unused]] auto success = index.modify (existing, [] (auto & score) {
				--score.outstanding;
				++score.response_count_total;
			});
			debug_assert (success);
		}
	}
}

std::shared_ptr<nano::transport::channel> nano::bootstrap::peer_scoring::channel ()
{
	for (auto const & channel : channels)
	{
		if (!channel->max (nano::transport::traffic_type::bootstrap))
		{
			if (!try_send_message (channel))
			{
				return channel;
			}
		}
	}
	return nullptr;
}

std::size_t nano::bootstrap::peer_scoring::size () const
{
	return scoring.size ();
}

std::size_t nano::bootstrap::peer_scoring::available () const
{
	return std::count_if (channels.begin (), channels.end (), [this] (auto const & channel) {
		return !limit_exceeded (channel);
	});
}

void nano::bootstrap::peer_scoring::timeout ()
{
	erase_if (scoring, [] (auto const & score) {
		if (auto channel = score.shared ())
		{
			if (channel->alive ())
			{
				return false; // Keep
			}
		}
		return true;
	});

	for (auto score = scoring.begin (), n = scoring.end (); score != n; ++score)
	{
		scoring.modify (score, [] (auto & score_a) {
			score_a.decay ();
		});
	}
}

void nano::bootstrap::peer_scoring::sync (std::deque<std::shared_ptr<nano::transport::channel>> const & list)
{
	channels = list;
}

nano::container_info nano::bootstrap::peer_scoring::container_info () const
{
	nano::container_info info;
	info.put ("scores", size ());
	info.put ("available", available ());
	info.put ("channels", channels.size ());
	return info;
}

/*
 * peer_score
 */

nano::bootstrap::peer_scoring::peer_score::peer_score (std::shared_ptr<nano::transport::channel> const & channel_a, uint64_t outstanding_a, uint64_t request_count_total_a, uint64_t response_count_total_a) :
	channel{ channel_a },
	channel_ptr{ channel_a.get () },
	outstanding{ outstanding_a },
	request_count_total{ request_count_total_a },
	response_count_total{ response_count_total_a }
{
}
