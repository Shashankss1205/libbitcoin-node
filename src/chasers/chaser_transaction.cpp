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
#include <bitcoin/node/chasers/chaser_transaction.hpp>

#include <functional>
#include <bitcoin/network.hpp>
#include <bitcoin/node/full_node.hpp>

namespace libbitcoin {
namespace node {

BC_PUSH_WARNING(NO_THROW_IN_NOEXCEPT)

chaser_transaction::chaser_transaction(full_node& node) NOEXCEPT
  : node_(node),
    strand_(node.service().get_executor()),
    subscriber_(strand_),
    reporter(node.log),
    tracker<chaser_transaction>(node.log)
{
}

chaser_transaction::~chaser_transaction() NOEXCEPT
{
    BC_ASSERT_MSG(stopped(), "The transaction chaser was not stopped.");
    if (!stopped()) { LOGF("~chaser_transaction is not stopped."); }
}

void chaser_transaction::start(network::result_handler&& handler) NOEXCEPT
{
    if (!stopped())
    {
        handler(network::error::operation_failed);
        return;
    }

    stopped_.store(false);
    handler(network::error::success);
}

void chaser_transaction::stop() NOEXCEPT
{
    stopped_.store(true);

    // The chaser_transaction can be deleted once threadpool joins after this call.
    boost::asio::post(strand_,
        std::bind(&chaser_transaction::do_stop, this));
}

chaser_transaction::object_key chaser_transaction::subscribe(notifier&& handler) NOEXCEPT
{
    BC_ASSERT_MSG(stranded(), "strand");
    const auto key = create_key();
    subscriber_.subscribe(std::move(handler), key);
    return key;
}

// TODO: closing channel notifies itself to desubscribe.
bool chaser_transaction::notify(object_key key) NOEXCEPT
{
    return subscriber_.notify_one(key, network::error::success);
}

bool chaser_transaction::stopped() const NOEXCEPT
{
    return stopped_.load();
}

bool chaser_transaction::stranded() const NOEXCEPT
{
    return strand_.running_in_this_thread();
}

// private
chaser_transaction::object_key chaser_transaction::create_key() NOEXCEPT
{
    BC_ASSERT_MSG(stranded(), "strand");

    if (is_zero(++keys_))
    {
        BC_ASSERT_MSG(false, "overflow");
        LOGF("Chaser object overflow.");
    }

    return keys_;
}

// private
void chaser_transaction::do_stop() NOEXCEPT
{
    BC_ASSERT_MSG(stranded(), "strand");

    subscriber_.stop(network::error::service_stopped);
}

BC_POP_WARNING()

} // namespace database
} // namespace libbitcoin
