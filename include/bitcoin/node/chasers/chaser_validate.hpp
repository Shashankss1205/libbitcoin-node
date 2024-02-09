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
#ifndef LIBBITCOIN_NODE_CHASERS_CHASER_VALIDATE_HPP
#define LIBBITCOIN_NODE_CHASERS_CHASER_VALIDATE_HPP

#include <bitcoin/network.hpp>
#include <bitcoin/node/define.hpp>

namespace libbitcoin {
namespace node {

class full_node;

/// Chase down blocks in the the candidate header chain for validation.
/// Notify subscribers with the "block connected" event.
class BCN_API chaser_validate
  : public network::reporter, protected network::tracker<chaser_validate>
{
public:
    typedef uint64_t object_key;
    typedef network::desubscriber<object_key> subscriber;
    typedef subscriber::handler notifier;
    DELETE_COPY_MOVE(chaser_validate);

    /// Construct an instance.
    /// -----------------------------------------------------------------------
    chaser_validate(full_node& node) NOEXCEPT;
    ~chaser_validate() NOEXCEPT;

    /// Start/stop.
    /// -----------------------------------------------------------------------
    void start(network::result_handler&& handler) NOEXCEPT;
    void stop() NOEXCEPT;

    /// Subscriptions.
    /// -----------------------------------------------------------------------
    object_key subscribe(notifier&& handler) NOEXCEPT;
    bool notify(object_key key) NOEXCEPT;

    /// Properties.
    /// -----------------------------------------------------------------------
    bool stopped() const NOEXCEPT;
    bool stranded() const NOEXCEPT;

private:
    object_key create_key() NOEXCEPT;
    void do_stop() NOEXCEPT;

    // These are thread safe (mostly).
    full_node& node_;
    network::asio::strand strand_;
    std::atomic_bool stopped_{ true };

    // These are protected by the strand.
    object_key keys_{};
    subscriber subscriber_;
};

} // namespace node
} // namespace libbitcoin

#endif
