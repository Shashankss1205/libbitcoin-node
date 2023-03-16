/**
 * Copyright (c) 2011-2019 libbitcoin developers (see AUTHORS)
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
#include <bitcoin/node/protocols/protocol_header_in_31800.hpp>

#include <utility>
#include <bitcoin/database.hpp>
#include <bitcoin/network.hpp>
#include <bitcoin/node/define.hpp>

namespace libbitcoin {
namespace node {

#define CLASS protocol_header_in_31800

using namespace system;
using namespace network;
using namespace network::messages;
using namespace std::placeholders;

// Start.
// ----------------------------------------------------------------------------

void protocol_header_in_31800::start() NOEXCEPT
{
    BC_ASSERT_MSG(stranded(), "protocol_header_in_31800");

    if (started())
        return;

    SUBSCRIBE_CHANNEL3(headers, handle_receive_headers, _1, _2, unix_time());
    SEND1(create_get_headers(), handle_send, _1);
    protocol::start();
}

// Inbound (headers).
// ----------------------------------------------------------------------------

bool protocol_header_in_31800::handle_receive_headers(const code& ec,
    const headers::cptr& message, uint32_t start) NOEXCEPT
{
    BC_ASSERT_MSG(stranded(), "protocol_address_in_31402");

    if (stopped(ec))
        return false;

    LOGP("Received (" << message->header_ptrs.size() << ") headers from ["
        << authority() << "].");

    code check{};
    const auto& coin = config().bitcoin;

    // TODO: optimize header hashing using read buffer in message deserialize.
    // Store each header, drop channel if invalid.
    for (const auto& header: message->header_ptrs)
    {
        check = header->check(
            coin.timestamp_limit_seconds,
            coin.proof_of_work_limit,
            coin.scrypt_proof_of_work);

        if (check)
        {
            LOGR("Invalid header [" << encode_hash(header->hash())
                << "] from [" << authority() << "] " << check.message());

            stop(network::error::protocol_violation);
            return false;
        }

        // TODO: maintain context progression and store with header.
        if (!archive().set(*header, database::context{ 1, 42, 7 }))
        {
            // Header with missing non-genesis parent.
            LOGR("Database error set(header) [" << encode_hash(header->hash())
                << "] from [" << authority() << "].");

            stop(network::error::protocol_violation);
            return false;
        }
    }

    // Protocol presumes max_get_headers unless complete.
    if (message->header_ptrs.size() == max_get_headers)
    {
        // Requesting at max_get_headers assumes there may be more.
        SEND1(create_get_headers({ message->header_ptrs.back()->hash() }),
            handle_send, _1);
    }
    else
    {
        complete(*message, start);
    }

    return true;
}

void protocol_header_in_31800::complete(const headers& message,
    uint32_t LOG_ONLY(start)) NOEXCEPT
{
    if (message.header_ptrs.empty())
    {
        // Empty message may happen in case where last header is 2000th.
        LOGN("Headers from [" << authority() << "] complete in ("
            << (unix_time() - start) << ") secs.");
    }
    else
    {
        // Using header_fk as height proxy.
        LOGN("Headers from [" << authority() << "] stopped at ("
            << archive().to_header(message.header_ptrs.back()->hash())
            << ") in (" << (unix_time() - start) << ") secs.");
    }
}

// private
get_headers protocol_header_in_31800::create_get_headers() NOEXCEPT
{
    return create_get_headers(archive().get_hashes(get_blocks::heights(
        archive().get_top_candidate())));
}

// private
get_headers protocol_header_in_31800::create_get_headers(
    hashes&& hashes) NOEXCEPT
{
    if (!hashes.empty())
    {
        LOGP("Request headers after [" << encode_hash(hashes.front())
            << "] from [" << authority() << "].");
    }

    return { std::move(hashes) };
}

} // namespace node
} // namespace libbitcoin
