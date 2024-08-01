/**
 * Copyright (c) 2011-2024 libbitcoin developers (see AUTHORS)
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
#ifndef LIBBITCOIN_NODE_BLOCK_ARENA_HPP
#define LIBBITCOIN_NODE_BLOCK_ARENA_HPP

#include <shared_mutex>
#include <bitcoin/system.hpp>
#include <bitcoin/node/define.hpp>

namespace libbitcoin {
namespace node {

/// Thread safe block memory arena.
class BCN_API block_arena final
  : public arena
{
public:
    inline std::shared_mutex& get_mutex() NOEXCEPT
    {
        return mutex_;
    }

    block_arena() NOEXCEPT;
    ~block_arena() NOEXCEPT;

private:
    void* do_allocate(size_t bytes, size_t align) THROWS override;
    void do_deallocate(void* ptr, size_t bytes, size_t align) NOEXCEPT override;
    bool do_is_equal(const arena& other) const NOEXCEPT override;

    // This is thread safe.
    std::shared_mutex mutex_{};
};
} // namespace node
} // namespace libbitcoin

#endif
