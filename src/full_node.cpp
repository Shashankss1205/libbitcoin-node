/**
 * Copyright (c) 2011-2023 libbitcoin developers (see AUTHORS)
 *
 * This file is part of libbitcoin.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <bitcoin/node/full_node.hpp>

#include <bitcoin/network.hpp>
#include <bitcoin/node/chasers/chasers.hpp>
#include <bitcoin/node/define.hpp>
#include <bitcoin/node/sessions/sessions.hpp>

namespace libbitcoin {
namespace node {

using namespace system;
using namespace network;

BC_PUSH_WARNING(NO_THROW_IN_NOEXCEPT)

// p2p::strand() is safe to call from constructor (non-virtual).
full_node::full_node(query& query, const configuration& configuration,
    const logger& log) NOEXCEPT
  : p2p(configuration.network, log),
    config_(configuration),
    query_(query),
    chaser_block_(*this),
    chaser_header_(*this),
    chaser_check_(*this),
    chaser_preconfirm_(*this),
    chaser_confirm_(*this),
    chaser_transaction_(*this),
    chaser_template_(*this),
    chaser_snapshot_(*this),
    chaser_storage_(*this),
    event_subscriber_(strand())
{
}

full_node::~full_node() NOEXCEPT
{
}

// Sequences.
// ----------------------------------------------------------------------------

void full_node::start(result_handler&& handler) NOEXCEPT
{
    if (!query_.is_initialized())
    {
        handler(error::store_uninitialized);
        return;
    }

    // Base (p2p) invokes do_start().
    p2p::start(std::move(handler));
}

void full_node::do_start(const result_handler& handler) NOEXCEPT
{
    BC_ASSERT(stranded());
    code ec;

    if (((ec = (config().node.headers_first ?
            chaser_header_.start() : chaser_block_.start()))) ||
        ((ec = chaser_check_.start())) ||
        ((ec = chaser_preconfirm_.start())) ||
        ((ec = chaser_confirm_.start())) ||
        ((ec = chaser_transaction_.start())) ||
        ((ec = chaser_template_.start())) ||
        ((ec = chaser_snapshot_.start())) ||
        ((ec = chaser_storage_.start())))
    {
        handler(ec);
        return;
    }

    p2p::do_start(handler);
}

void full_node::run(result_handler&& handler) NOEXCEPT
{
    // Base (p2p) invokes do_run().
    p2p::run(std::move(handler));
}

void full_node::do_run(const result_handler& handler) NOEXCEPT
{
    BC_ASSERT(stranded());

    if (closed())
    {
        handler(network::error::service_stopped);
        return;
    }

    // Bump sequential chasers to their starting heights.
    // This will kick off lagging validations even if not current.
    do_notify(error::success, chase::start, height_t{});

    p2p::do_run(handler);
}

void full_node::close() NOEXCEPT
{
    // Base (p2p) invokes do_close().
    p2p::close();
}

// Base (p2p) invokes do_close().
void full_node::do_close() NOEXCEPT
{
    BC_ASSERT(stranded());
    event_subscriber_.stop(network::error::service_stopped, chase::stop, {});
    p2p::do_close();
}

// Organizers.
// ----------------------------------------------------------------------------

void full_node::organize(const system::chain::header::cptr& header,
    organize_handler&& handler) NOEXCEPT
{
    chaser_header_.organize(header, std::move(handler));
}

void full_node::organize(const system::chain::block::cptr& block,
    organize_handler&& handler) NOEXCEPT
{
    chaser_block_.organize(block, std::move(handler));
}

void full_node::get_hashes(map_handler&& handler) NOEXCEPT
{
    chaser_check_.get_hashes(std::move(handler));
}

void full_node::put_hashes(const map_ptr& map,
    result_handler&& handler) NOEXCEPT
{
    chaser_check_.put_hashes(map, std::move(handler));
}

// Events.
// ----------------------------------------------------------------------------

object_key full_node::subscribe_events(event_notifier&& handler) NOEXCEPT
{
    BC_ASSERT(stranded());
    const auto key = create_key();
    event_subscriber_.subscribe(std::move(handler), key);
    return key;
}

void full_node::subscribe_events(event_notifier&& handler,
    event_completer&& complete) NOEXCEPT
{
    boost::asio::post(strand(),
        std::bind(&full_node::do_subscribe_events,
            this, std::move(handler), std::move(complete)));
}

// private
void full_node::do_subscribe_events(const event_notifier& handler,
    const event_completer& complete) NOEXCEPT
{
    BC_ASSERT(stranded());
    const auto key = create_key();
    complete(event_subscriber_.subscribe(move_copy(handler), key), key);
}

void full_node::notify(const code& ec, chase event_,
    event_value value) NOEXCEPT
{
    boost::asio::post(strand(),
        std::bind(&full_node::do_notify,
            this, ec, event_, value));
}

// private
void full_node::do_notify(const code& ec, chase event_,
    event_value value) NOEXCEPT
{
    BC_ASSERT(stranded());
    event_subscriber_.notify(ec, event_, value);
}

void full_node::notify_one(object_key key, const code& ec, chase event_,
    event_value value) NOEXCEPT
{
    boost::asio::post(strand(),
        std::bind(&full_node::do_notify_one,
            this, key, ec, event_, value));
}

// private
void full_node::do_notify_one(object_key key, const code& ec, chase event_,
    event_value value) NOEXCEPT
{
    BC_ASSERT(stranded());
    event_subscriber_.notify_one(key, ec, event_, value);
}

void full_node::unsubscribe_events(object_key key) NOEXCEPT
{
    notify_one(key, network::error::service_stopped, chase::stop, {});
}

// private
// At one object/session/ns, this overflows in ~585 years (and handled).
// Could just use channel.identifier() if we didn't have subscribed chasers.
object_key full_node::create_key() NOEXCEPT
{
    BC_ASSERT(stranded());

    if (is_zero(++keys_))
    {
        BC_ASSERT_MSG(false, "overflow");
        LOGF("Session object overflow.");
    }

    return keys_;
}

// Suspensions.
// ----------------------------------------------------------------------------

code full_node::snapshot(const store::event_handler& handler) NOEXCEPT
{
    if (query_.is_fault())
        return query_.get_code();

    const auto running = !suspended();
    suspend(error::store_snapshot);
    const auto ec = query_.snapshot([&](auto event, auto table) NOEXCEPT
    {
        // Suspend channels that missed previous suspend events.
        if (event == database::event_t::wait_lock)
            suspend(error::store_snapshot);

        handler(event, table);
    });

    // Could become full before snapshot start (and it could still succeed).
    if (running && !query_.is_full())
        resume();

    return ec;
}

// This is just a best effort, it generally has to be repeated.
code full_node::suspend(const code& ec) NOEXCEPT
{
    LOGS("Suspending network connections: " << ec.message());
    p2p::suspend(ec);

    // Multiple messages will be absorbed by snapshot chaser.
    if (ec == database::error::disk_full)
        notify(ec, chase::snapshot, {});

    notify(error::success, chase::suspend, ec.value());
    return ec;
}

void full_node::resume() NOEXCEPT
{
    LOGS("Resuming network connections.");
    p2p::resume();
}

void full_node::reset_full() NOEXCEPT
{
    query_.reset_full();
}

// Properties.
// ----------------------------------------------------------------------------

query& full_node::archive() const NOEXCEPT
{
    return query_;
}

const configuration& full_node::config() const NOEXCEPT
{
    return config_;
}

bool full_node::is_current() const NOEXCEPT
{
    if (is_zero(config_.node.currency_window_minutes))
        return true;

    uint32_t timestamp{};
    const auto top = query_.to_candidate(query_.get_top_candidate());
    return query_.get_timestamp(timestamp, top) && is_current(timestamp);
}

// en.wikipedia.org/wiki/Time_formatting_and_storage_bugs#Year_2106
bool full_node::is_current(uint32_t timestamp) const NOEXCEPT
{
    if (is_zero(config_.node.currency_window_minutes))
        return true;

    const auto time = wall_clock::from_time_t(timestamp);
    const auto current = wall_clock::now() - config_.node.currency_window();
    return time >= current;
}

// Session attachments.
// ----------------------------------------------------------------------------

network::session_manual::ptr full_node::attach_manual_session() NOEXCEPT
{
    return p2p::attach<node::session_manual>(*this);
}

network::session_inbound::ptr full_node::attach_inbound_session() NOEXCEPT
{
    return p2p::attach<node::session_inbound>(*this);
}

network::session_outbound::ptr full_node::attach_outbound_session() NOEXCEPT
{
    return p2p::attach<node::session_outbound>(*this);
}

BC_POP_WARNING()

} // namespace node
} // namespace libbitcoin
