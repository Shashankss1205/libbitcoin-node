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
#include <bitcoin/node/chasers/chaser_block.hpp>

#include <algorithm>
#include <functional>
#include <utility>
#include <variant>
#include <bitcoin/network.hpp>
#include <bitcoin/node/error.hpp>
#include <bitcoin/node/full_node.hpp>
#include <bitcoin/node/chasers/chaser.hpp>

namespace libbitcoin {
namespace node {

#define CLASS chaser_block

using namespace system;
using namespace system::chain;
using namespace network;
using namespace std::placeholders;

BC_PUSH_WARNING(NO_NEW_OR_DELETE)
BC_PUSH_WARNING(NO_THROW_IN_NOEXCEPT)

chaser_block::chaser_block(full_node& node) NOEXCEPT
  : chaser(node),
    settings_(config().bitcoin)
{
}

// start
// ----------------------------------------------------------------------------

code chaser_block::start() NOEXCEPT
{
    BC_ASSERT(node_stranded());

    // Initialize cache of top candidate chain state.
    state_ = archive().get_candidate_chain_state(settings_,
        archive().get_top_candidate());

    return SUBSCRIBE_EVENTS(handle_event, _1, _2, _3);
}

// disorganize
// ----------------------------------------------------------------------------

void chaser_block::handle_event(const code&, chase event_, link value) NOEXCEPT
{
    if (event_ == chase::unconfirmed)
    {
        POST(do_disorganize, std::get<header_t>(value));
    }
}

// TODO: see chaser_header::do_disorganize
void chaser_block::do_disorganize(header_t header) NOEXCEPT
{
    BC_ASSERT(stranded());

    // Skip already reorganized out, get height.
    // ------------------------------------------------------------------------

    // Upon restart candidate chain validation will hit unconfirmable block.
    if (closed())
        return;

    // If header is not a current candidate it has been reorganized out.
    // If header becomes candidate again its unconfirmable state is handled.
    auto& query = archive();
    if (!query.is_candidate_block(header))
        return;

    size_t height{};
    if (!query.get_height(height, header) || is_zero(height))
    {
        close(error::internal_error);
        return;
    }

    const auto fork_point = query.get_fork();
    if (height <= fork_point)
    {
        close(error::internal_error);
        return;
    }

    // Mark candidates above and pop at/above height.
    // ------------------------------------------------------------------------

    // Pop from top down to and including header marking each as unconfirmable.
    // Unconfirmability isn't necessary for validation but adds query context.
    for (auto index = query.get_top_candidate(); index > height; --index)
    {
        const auto link = query.to_candidate(index);

        LOGN("Invalidating candidate [" << index << ":"
            << encode_hash(query.get_header_key(link)) << "].");

        if (!query.set_block_unconfirmable(link) || !query.pop_candidate())
        {
            close(error::store_integrity);
            return;
        }
    }

    LOGN("Invalidating candidate [" << height << ":"
        << encode_hash(query.get_header_key(header)) << "].");

    // Candidate at height is already marked as unconfirmable by notifier.
    if (!query.pop_candidate())
    {
        close(error::store_integrity);
        return;
    }

    // Reset top chain state cache to fork point.
    // ------------------------------------------------------------------------

    const auto top_candidate = state_->height();
    const auto prev_forks = state_->forks();
    const auto prev_version = state_->minimum_block_version();
    state_ = query.get_candidate_chain_state(settings_, fork_point);
    if (!state_)
    {
        close(error::store_integrity);
        return;
    }

    // TODO: this could be moved to deconfirmation.
    const auto next_forks = state_->forks();
    if (prev_forks != next_forks)
    {
        const binary prev{ to_bits(sizeof(forks)), to_big_endian(prev_forks) };
        const binary next{ to_bits(sizeof(forks)), to_big_endian(next_forks) };
        LOGN("Forks reverted from ["
            << prev << "] at candidate ("
            << top_candidate << ") to ["
            << next << "] at confirmed ["
            << fork_point << ":" << encode_hash(state_->hash()) << "].");
    }

    // TODO: this could be moved to deconfirmation.
    const auto next_version = state_->minimum_block_version();
    if (prev_version != next_version)
    {
        LOGN("Minimum block version reverted ["
            << prev_version << "] at candidate ("
            << top_candidate << ") to ["
            << next_version << "] at confirmed ["
            << fork_point << ":" << encode_hash(state_->hash()) << "].");
    }

    // Copy candidates from above fork point to top into block tree.
    // ------------------------------------------------------------------------

    auto state = state_;
    for (auto index = add1(fork_point); index <= top_candidate; ++index)
    {
        const auto save = query.get_block(query.to_candidate(index));
        if (!save)
        {
            close(error::store_integrity);
            return;
        }

        state.reset(new chain_state{ *state, *save, settings_ });
        cache(save, state);
    }

    // Pop candidates from top to above fork point.
    // ------------------------------------------------------------------------
    for (auto index = top_candidate; index > fork_point; --index)
    {
        LOGN("Deorganizing candidate [" << index << "].");

        if (!query.pop_candidate())
        {
            close(error::store_integrity);
            return;
        }
    }

    // Push confirmed headers from above fork point onto candidate chain.
    // ------------------------------------------------------------------------
    const auto top_confirmed = query.get_top_confirmed();
    for (auto index = add1(fork_point); index <= top_confirmed; ++index)
    {
        if (!query.push_candidate(query.to_confirmed(index)))
        {
            close(error::store_integrity);
            return;
        }
    }
}

// organize
// ----------------------------------------------------------------------------

void chaser_block::organize(const block::cptr& block,
    organize_handler&& handler) NOEXCEPT
{
    POST(do_organize, block, std::move(handler));
}

void chaser_block::do_organize(const block::cptr& block_ptr,
    const organize_handler& handler) NOEXCEPT
{
    BC_ASSERT(stranded());

    auto& query = archive();
    const auto& block = *block_ptr;
    const auto& header = block.header();
    const auto hash = header.hash();

    // Skip existing/orphan, get state.
    // ------------------------------------------------------------------------

    if (closed())
    {
        handler(network::error::service_stopped, {});
        return;
    }

    if (tree_.contains(hash))
    {
        handler(error::duplicate_block, {});
        return;
    }

    // If header exists test for prior invalidity as a block.
    const auto link = query.to_header(hash);
    if (!link.is_terminal())
    {
        const auto ec = query.get_block_state(link);
        if (ec == database::error::block_unconfirmable)
        {
            handler(ec, {});
            return;
        }

        if (ec != database::error::unassociated)
        {
            handler(error::duplicate_block, {});
            return;
        }
    }

    // Results from running headers-first and then blocks-first.
    auto state = get_chain_state(header.previous_block_hash());
    if (!state)
    {
        handler(error::orphan_block, {});
        return;
    }

    // Roll chain state forward from previous to current header.
    // ------------------------------------------------------------------------

    const auto prev_forks = state->forks();
    const auto prev_version = state->minimum_block_version();

    // Do not use block parameter here as that override is for tx pool.
    state.reset(new chain_state{ *state, header, settings_ });
    const auto height = state->height();

    // TODO: this could be moved to confirmation.
    const auto next_forks = state->forks();
    if (prev_forks != next_forks)
    {
        const binary prev{ to_bits(sizeof(forks)), to_big_endian(prev_forks) };
        const binary next{ to_bits(sizeof(forks)), to_big_endian(next_forks) };
        LOGN("Forked from ["
            << prev << "] to ["
            << next << "] at ["
            << height << ":" << encode_hash(hash) << "].");
    }

    // TODO: this could be moved to confirmation.
    const auto next_version = state->minimum_block_version();
    if (prev_version != next_version)
    {
        LOGN("Minimum block version ["
            << prev_version << "] changed to ["
            << next_version << "] at ["
            << height << ":" << encode_hash(hash) << "].");
    }

    // Check/Accept/Connect block.
    // ------------------------------------------------------------------------
    // Blocks are accumulated following genesis, not cached until current.

    // Checkpoints are considered chain not block/header validation.
    if (checkpoint::is_conflict(settings_.checkpoints, hash, height))
    {
        handler(system::error::checkpoint_conflict, height);
        return;
    };

    // Block validations are bypassed when under checkpoint/milestone.
    if (!checkpoint::is_under(settings_.checkpoints, height))
    {
        // Requires no population.
        if (const auto error = block.check())
        {
            handler(error, height);
            return;
        }

        // Requires no population.
        if (const auto error = block.check(state->context()))
        {
            handler(error, height);
            return;
        }

        // Populate prevouts from self/tree and store.
        populate(block);
        if (!query.populate(block))
        {
            handler(network::error::protocol_violation, height);
            return;
        }

        // Requires only prevout population.
        if (const auto error = block.accept(state->context(),
            settings_.subsidy_interval_blocks, settings_.initial_subsidy()))
        {
            handler(error, height);
            return;
        }

        // Requires only prevout population.
        if (const auto error = block.connect(state->context()))
        {
            handler(error, height);
            return;
        }
    }

    // Compute relative work.
    // ------------------------------------------------------------------------
    // Current is not used for blocks due to excessive cache requirement.

    uint256_t work{};
    hashes tree_branch{};
    size_t branch_point{};
    header_links store_branch{};
    if (!get_branch_work(work, branch_point, tree_branch, store_branch, header))
    {
        handler(error::store_integrity, height);
        close(error::store_integrity);
        return;
    }

    bool strong{};
    if (!get_is_strong(strong, work, branch_point))
    {
        handler(error::store_integrity, height);
        close(error::store_integrity);
        return;
    }

    // If a long candidate chain is first created using headers-first and then
    // blocks-first is executed (after a restart/config) it can result in up to
    // the entire blockchain being cached into memory before becoming strong,
    // which means stronger than the candidate chain. While switching config
    // between modes by varying network protocol is supported, blocks-first is
    // inherently inefficient and weak on this aspect of DoS protection. This
    // is acceptable for its purpose and consistent with early implementations.
    if (!strong)
    {
        // Block is new top of current weak branch.
        cache(block_ptr, state);
        handler(error::success, height);
        return;
    }

    // Reorganize candidate chain.
    // ------------------------------------------------------------------------

    auto top = state_->height();
    if (top < branch_point)
    {
        handler(error::store_integrity, height);
        close(error::store_integrity);
        return;
    }

    // Pop down to the branch point.
    while (top-- > branch_point)
    {
        if (!query.pop_candidate())
        {
            handler(error::store_integrity, height);
            close(error::store_integrity);
            return;
        }
    }

    // Push stored strong block headers to candidate chain.
    for (const auto& id: views_reverse(store_branch))
    {
        if (!query.push_candidate(id))
        {
            handler(error::store_integrity, height);
            close(error::store_integrity);
            return;
        }
    }

    // Store strong tree blocks and push headers to candidate chain.
    for (const auto& key: views_reverse(tree_branch))
    {
        if (!push_block(key))
        {
            handler(error::store_integrity, height);
            close(error::store_integrity);
            return;
        }
    }

    // Push new block as top of candidate chain.
    if (push_block(block_ptr, state->context()).is_terminal())
    {
        handler(error::store_integrity, height);
        close(error::store_integrity);
        return;
    }

    // ------------------------------------------------------------------------

    const auto point = possible_narrow_cast<height_t>(branch_point);
    notify(error::success, chase::block, point);

    state_ = state;
    handler(error::success, height);
}

// utilities
// ----------------------------------------------------------------------------

chain_state::ptr chaser_block::get_chain_state(
    const hash_digest& hash) const NOEXCEPT
{
    if (!state_)
        return {};

    // Top state is cached because it is by far the most commonly retrieved.
    if (state_->hash() == hash)
        return state_;

    const auto it = tree_.find(hash);
    if (it != tree_.end())
        return it->second.state;

    size_t height{};
    const auto& query = archive();
    if (query.get_height(height, query.to_header(hash)))
        return query.get_candidate_chain_state(settings_, height);

    return {};
}

// Sum of work from header to branch point (excluded).
// Also obtains branch point for work summation termination.
// Also obtains ordered branch identifiers for subsequent reorg.
bool chaser_block::get_branch_work(uint256_t& work, size_t& branch_point,
    hashes& tree_branch, header_links& store_branch,
    const header& header) const NOEXCEPT
{
    // Use pointer to avoid const/copy.
    auto previous = &header.previous_block_hash();
    const auto& query = archive();
    work = header.proof();

    // Sum all branch work from tree.
    for (auto it = tree_.find(*previous); it != tree_.end();
        it = tree_.find(*previous))
    {
        previous = &it->second.block->header().previous_block_hash();
        tree_branch.push_back(it->second.block->header().hash());
        work += it->second.block->header().proof();
    }

    // Sum branch work from store.
    database::height_link link{};
    for (link = query.to_header(*previous); !query.is_candidate_block(link);
        link = query.to_parent(link))
    {
        uint32_t bits{};
        if (link.is_terminal() || !query.get_bits(bits, link))
            return false;

        store_branch.push_back(link);
        work += chain::header::proof(bits);
    }

    // Height of the highest candidate header is the branch point.
    return query.get_height(branch_point, link);
}

// ****************************************************************************
// CONSENSUS: branch with greater work causes candidate reorganization.
// Chasers eventually reorganize candidate branch into confirmed if valid.
// ****************************************************************************
bool chaser_block::get_is_strong(bool& strong, const uint256_t& work,
    size_t branch_point) const NOEXCEPT
{
    strong = false;
    uint256_t candidate_work{};
    const auto& query = archive();
    const auto top = query.get_top_candidate();

    for (auto height = top; height > branch_point; --height)
    {
        uint32_t bits{};
        if (!query.get_bits(bits, query.to_candidate(height)))
            return false;

        // Not strong is candidate work equals or exceeds new work.
        candidate_work += header::proof(bits);
        if (candidate_work >= work)
            return true;
    }

    strong = true;
    return true;
}

void chaser_block::cache(const block::cptr& block,
    const chain_state::ptr& state) NOEXCEPT
{
    tree_.insert({ block->hash(), { block, state } });
}

// Store block to database and push to top of candidate chain.
database::header_link chaser_block::push_block(const block::cptr& block,
    const context& context) const NOEXCEPT
{
    auto& query = archive();
    const auto link = query.set_link(*block, database::context
    {
        possible_narrow_cast<flags_t>(context.forks),
        possible_narrow_cast<height_t>(context.height),
        context.median_time_past,
    });

    return query.push_candidate(link) ? link : database::header_link{};
}

// Move tree block to database and push to top of candidate chain.
bool chaser_block::push_block(const hash_digest& key) NOEXCEPT
{
    const auto value = tree_.extract(key);
    BC_ASSERT_MSG(!value.empty(), "missing tree value");

    auto& query = archive();
    const auto& node = value.mapped();
    const auto link = query.set_link(*node.block, node.state->context());
    return query.push_candidate(link);
}

void chaser_block::set_prevout(const input& input) const NOEXCEPT
{
    const auto& point = input.point();

    // Scan all blocks for matching tx (linear :/)
    std::for_each(tree_.begin(), tree_.end(), [&](const auto& element) NOEXCEPT
    {
        const auto& txs = *element.second.block->transactions_ptr();
        const auto it = std::find_if(txs.begin(), txs.end(),
            [&](const auto& tx) NOEXCEPT
            {
                return tx->hash(false) == point.hash();
            });

        if (it != txs.end())
        {
            const auto& tx = **it;
            const auto& outs = *tx.outputs_ptr();
            if (point.index() < outs.size())
            {
                input.prevout = outs.at(point.index());
                return;
            }
        }
    });
}

// metadata is mutable so can be set on a const object.
void chaser_block::populate(const block& block) const NOEXCEPT
{
    block.populate();
    const auto ins = block.inputs_ptr();
    std::for_each(ins->begin(), ins->end(), [&](const auto& in) NOEXCEPT
    {
        if (!in->prevout && !in->point().is_null())
            set_prevout(*in);
    });
}

BC_POP_WARNING()
BC_POP_WARNING()

} // namespace database
} // namespace libbitcoin
