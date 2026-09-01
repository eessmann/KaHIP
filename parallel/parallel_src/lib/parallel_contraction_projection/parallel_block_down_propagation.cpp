/******************************************************************************
 * parallel_block_down_propagation.cpp
 * *
 * Source of KaHIP -- Karlsruhe High Quality Partitioning.
 * Christian Schulz <christian.schulz.phone@gmail.com>
 *****************************************************************************/

#include "parallel_block_down_propagation.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "communication/contiguous_owner_layout.h"
#include "communication/ghost_exchange_plan.h"
#include "communication/mpi_adapter.h"
#include "communication/mpi_trace.h"

namespace parhip {
void parallel_block_down_propagation::propagate_block_down(
    MPI_Comm communicator,
    PPartitionConfig& config,
    parallel_graph_access& G,
    parallel_graph_access& Q) {
  auto const graph_communicator = mpi::communicator_view{Q.getCommunicator()};
  auto const rank = graph_communicator.rank();
  auto const size = graph_communicator.size();
  auto const rank_index = static_cast<std::size_t>(rank);

  auto communicators_are_compatible =
      communicator != MPI_COMM_NULL && G.getCommunicator() != MPI_COMM_NULL;
  if (communicators_are_compatible) {
    auto graph_comparison = int{MPI_UNEQUAL};
    auto quotient_comparison = int{MPI_UNEQUAL};
    mpi::check_or_abort(
        MPI_Comm_compare(communicator, G.getCommunicator(), &graph_comparison),
        Q.getCommunicator(), "MPI_Comm_compare(block-down finer graph)");
    mpi::check_or_abort(MPI_Comm_compare(communicator, Q.getCommunicator(),
                                         &quotient_comparison),
                        Q.getCommunicator(),
                        "MPI_Comm_compare(block-down quotient graph)");
    communicators_are_compatible =
        (graph_comparison == MPI_IDENT || graph_comparison == MPI_CONGRUENT) &&
        (quotient_comparison == MPI_IDENT ||
         quotient_comparison == MPI_CONGRUENT);
  }
  mpi::validate_collectively(communicators_are_compatible, graph_communicator,
                             "block-down communicator validation failed");

  auto const number_of_blocks = mpi::agree_collectively(
      config.k, graph_communicator, "block-down block-count agreement failed");
  mpi::validate_collectively(number_of_blocks > PartitionID{0},
                             graph_communicator,
                             "block-down requires a positive block count");
  auto const number_of_coarse_nodes =
      mpi::agree_collectively(Q.number_of_global_nodes(), graph_communicator,
                              "block-down coarse-node count agreement failed");
  auto const ownership = mpi::contiguous_owner_layout<NodeID>{
      number_of_coarse_nodes, static_cast<std::size_t>(size)};
  auto const expected_from = ownership.begin(rank_index);
  auto const expected_end = ownership.end(rank_index);
  auto const expected_local_nodes = expected_end - expected_from;
  auto const expected_to =
      expected_local_nodes == 0 ? expected_from : expected_end - NodeID{1};

  auto ownership_metadata_is_valid =
      Q.number_of_local_nodes() == expected_local_nodes &&
      Q.get_from_range() == expected_from && Q.get_to_range() == expected_to &&
      std::in_range<std::size_t>(Q.number_of_local_nodes()) &&
      std::in_range<std::size_t>(Q.number_of_ghost_nodes()) &&
      Q.number_of_local_nodes() < std::numeric_limits<NodeID>::max() &&
      Q.number_of_ghost_nodes() <= std::numeric_limits<NodeID>::max() -
                                       (Q.number_of_local_nodes() + NodeID{1});
  auto const& range_array = Q.get_range_array();
  ownership_metadata_is_valid =
      ownership_metadata_is_valid &&
      range_array.size() == static_cast<std::size_t>(size) + std::size_t{1};
  auto const range_limit = std::min(
      range_array.size(), static_cast<std::size_t>(size) + std::size_t{1});
  for (auto boundary = std::size_t{0}; boundary < range_limit; ++boundary) {
    ownership_metadata_is_valid =
        ownership_metadata_is_valid &&
        range_array[boundary] == ownership.boundary(boundary);
  }
  mpi::validate_collectively(
      ownership_metadata_is_valid, graph_communicator,
      "block-down quotient ownership metadata validation failed");

  auto local_updates = std::vector<block_down::block_update>{};
  auto local_updates_are_valid =
      std::in_range<std::size_t>(G.number_of_local_nodes());
  try {
    if (local_updates_are_valid) {
      local_updates.reserve(
          static_cast<std::size_t>(G.number_of_local_nodes()));
    }
    for (auto node = NodeID{0}; node < G.number_of_local_nodes(); ++node) {
      auto const coarse_global_id = G.getCNode(node);
      auto const raw_block = G.getSecondPartitionIndex(node);
      auto const block_is_representable = std::in_range<PartitionID>(raw_block);
      auto const block = block_is_representable
                             ? static_cast<PartitionID>(raw_block)
                             : PartitionID{0};
      auto const local_global_id = G.getGlobalID(node);
      local_updates_are_valid = local_updates_are_valid &&
                                ownership.owner(coarse_global_id).has_value() &&
                                block_is_representable &&
                                block < number_of_blocks &&
                                G.find_local_id(local_global_id) == node;
      local_updates.push_back({coarse_global_id, block});
    }
    std::ranges::stable_sort(local_updates, {}, [](auto const& update) {
      return std::tie(update.coarse_global_id, update.block);
    });
    for (auto index = std::size_t{1}; index < local_updates.size(); ++index) {
      auto const& previous = local_updates[index - std::size_t{1}];
      auto const& current = local_updates[index];
      local_updates_are_valid =
          local_updates_are_valid &&
          (previous.coarse_global_id != current.coarse_global_id ||
           previous.block == current.block);
    }
  } catch (...) {
    mpi::abort_on_exception(Q.getCommunicator(),
                            "block-down local update staging");
  }
  mpi::validate_collectively(local_updates_are_valid, graph_communicator,
                             "block-down local update validation failed");

  auto const& plan = Q.ghost_plan();
  auto dense_sends = mpi::segmented_buffer<block_down::block_update>{};
  try {
    auto updates_by_destination =
        std::vector<std::vector<block_down::block_update>>(
            static_cast<std::size_t>(size));
    for (auto const& update : local_updates) {
      auto const destination = ownership.owner(update.coarse_global_id);
      if (!destination.has_value()) {
        mpi::abort_on_programming_error(
            plan.topology().native_handle(),
            "validated block-down update has no owner");
      }
      updates_by_destination[*destination].push_back(update);
    }
    dense_sends =
        mpi::segmented_buffer<block_down::block_update>::from_segments(
            updates_by_destination);
  } catch (...) {
    mpi::abort_on_exception(plan.topology().native_handle(),
                            "block-down dense send staging");
  }

  auto dense_received = mpi::all_to_all_v(std::move(dense_sends),
                                          mpi::communicator_view{communicator});
  auto owned_blocks = std::vector<PartitionID>{};
  auto owned_assigned = std::vector<unsigned char>{};
  auto dense_received_is_valid =
      dense_received.segment_count() == static_cast<std::size_t>(size);
  try {
    auto const local_count = static_cast<std::size_t>(expected_local_nodes);
    owned_blocks.assign(local_count, PartitionID{0});
    owned_assigned.assign(local_count, static_cast<unsigned char>(0));
    auto const source_limit = std::min(dense_received.segment_count(),
                                       static_cast<std::size_t>(size));
    for (auto source = std::size_t{0}; source < source_limit; ++source) {
      for (auto const& update : dense_received.segment(source)) {
        auto const owner = ownership.owner(update.coarse_global_id);
        auto const local_id = Q.find_local_id(update.coarse_global_id);
        auto const local_id_is_representable =
            local_id.has_value() && std::in_range<std::size_t>(*local_id);
        auto const index = local_id_is_representable
                               ? static_cast<std::size_t>(*local_id)
                               : std::size_t{0};
        auto const record_is_valid =
            owner.has_value() && *owner == rank_index &&
            local_id_is_representable && index < owned_blocks.size() &&
            update.coarse_global_id < number_of_coarse_nodes &&
            update.block < number_of_blocks;
        dense_received_is_valid = dense_received_is_valid && record_is_valid;
        if (!record_is_valid) {
          continue;
        }
        dense_received_is_valid =
            dense_received_is_valid &&
            Q.getGlobalID(*local_id) == update.coarse_global_id &&
            (owned_assigned[index] == 0 || owned_blocks[index] == update.block);
        if (owned_assigned[index] == 0) {
          owned_blocks[index] = update.block;
          owned_assigned[index] = 1;
        }
      }
    }
    dense_received_is_valid =
        dense_received_is_valid &&
        std::ranges::all_of(owned_assigned,
                            [](auto assigned) { return assigned != 0; });
  } catch (...) {
    mpi::abort_on_exception(plan.topology().native_handle(),
                            "block-down dense receive staging");
  }
  if (!mpi::detail::collective_predicate(dense_received_is_valid,
                                         plan.topology().view())) {
    mpi::throw_collectively_agreed_semantic_error(
        plan.topology().native_handle(),
        "block-down dense received validation failed");
  }

  auto neighbor_sends = mpi::segmented_buffer<block_down::block_update>{};
  auto neighbor_outgoing_is_valid = true;
  try {
    auto updates_by_destination =
        std::vector<std::vector<block_down::block_update>>(
            plan.topology().destinations().size());
    for (auto destination_index = std::size_t{0};
         destination_index < plan.topology().destinations().size();
         ++destination_index) {
      auto const local_nodes = plan.outgoing_local_nodes(destination_index);
      auto& updates = updates_by_destination[destination_index];
      updates.reserve(local_nodes.size());
      auto previous = std::optional<NodeID>{};
      for (auto const local : local_nodes) {
        auto const local_is_representable = std::in_range<std::size_t>(local);
        auto const index = local_is_representable
                               ? static_cast<std::size_t>(local)
                               : std::size_t{0};
        auto const local_is_valid =
            local_is_representable && local < Q.number_of_local_nodes() &&
            index < owned_blocks.size() && owned_assigned[index] != 0 &&
            (!previous.has_value() || *previous < local);
        neighbor_outgoing_is_valid =
            neighbor_outgoing_is_valid && local_is_valid;
        if (!local_is_valid) {
          continue;
        }
        auto const global_id = Q.getGlobalID(local);
        neighbor_outgoing_is_valid = neighbor_outgoing_is_valid &&
                                     Q.is_interface_node(local) &&
                                     global_id < number_of_coarse_nodes &&
                                     Q.find_local_id(global_id) == local &&
                                     owned_blocks[index] < number_of_blocks;
        updates.push_back({global_id, owned_blocks[index]});
        previous = local;
      }
    }
    neighbor_sends =
        mpi::segmented_buffer<block_down::block_update>::from_segments(
            updates_by_destination);
  } catch (...) {
    mpi::abort_on_exception(plan.topology().native_handle(),
                            "block-down neighbor send staging");
  }
  if (!mpi::detail::collective_predicate(neighbor_outgoing_is_valid,
                                         plan.topology().view())) {
    mpi::throw_collectively_agreed_semantic_error(
        plan.topology().native_handle(),
        "block-down neighbor outgoing validation failed");
  }

  auto neighbor_received =
      mpi::neighbor_all_to_all_v(std::move(neighbor_sends), plan.topology());
  using pending_ghost_update = std::tuple<NodeID, PEID, NodeID, PartitionID>;
  auto pending_ghost_updates = std::vector<pending_ghost_update>{};
  auto ghost_blocks = std::vector<PartitionID>{};
  auto ghost_assigned = std::vector<unsigned char>{};
  auto neighbor_received_is_valid =
      neighbor_received.segment_count() == plan.topology().sources().size();
  try {
    auto const ghost_count =
        static_cast<std::size_t>(Q.number_of_ghost_nodes());
    auto const ghost_begin = Q.number_of_local_nodes() + NodeID{1};
    ghost_blocks.assign(ghost_count, PartitionID{0});
    ghost_assigned.assign(ghost_count, static_cast<unsigned char>(0));
    pending_ghost_updates.reserve(ghost_count);
    auto const source_limit = std::min(neighbor_received.segment_count(),
                                       plan.topology().sources().size());
    for (auto source_index = std::size_t{0}; source_index < source_limit;
         ++source_index) {
      auto const source = plan.topology().sources()[source_index];
      auto const updates = neighbor_received.segment(source_index);
      auto const expected = plan.expected_ghost_nodes(source_index);
      auto received_ids = std::vector<NodeID>{};
      received_ids.reserve(updates.size());
      neighbor_received_is_valid =
          neighbor_received_is_valid && updates.size() == expected.size();
      for (auto update_index = std::size_t{0}; update_index < updates.size();
           ++update_index) {
        auto const& update = updates[update_index];
        received_ids.push_back(update.coarse_global_id);
        auto const owner = ownership.owner(update.coarse_global_id);
        auto const source_is_representable = std::in_range<std::size_t>(source);
        auto const local_id =
            Q.find_ghost_local_id(update.coarse_global_id, source);
        auto const local_id_is_representable =
            local_id.has_value() && std::in_range<std::size_t>(*local_id);
        auto const local_id_is_ghost =
            local_id_is_representable && *local_id >= ghost_begin;
        auto const ghost_index_node =
            local_id_is_ghost ? *local_id - ghost_begin : NodeID{0};
        auto const ghost_index_is_representable =
            local_id_is_ghost && std::in_range<std::size_t>(ghost_index_node);
        auto const ghost_index =
            ghost_index_is_representable
                ? static_cast<std::size_t>(ghost_index_node)
                : std::size_t{0};
        auto const record_is_valid =
            owner.has_value() && source_is_representable &&
            *owner == static_cast<std::size_t>(source) &&
            update.coarse_global_id < number_of_coarse_nodes &&
            update.block < number_of_blocks && ghost_index_is_representable &&
            ghost_index < ghost_blocks.size() &&
            ghost_assigned[ghost_index] == 0;
        neighbor_received_is_valid =
            neighbor_received_is_valid && record_is_valid;
        if (!record_is_valid) {
          continue;
        }
        neighbor_received_is_valid =
            neighbor_received_is_valid &&
            Q.getGlobalID(*local_id) == update.coarse_global_id;
        ghost_blocks[ghost_index] = update.block;
        ghost_assigned[ghost_index] = 1;
        pending_ghost_updates.emplace_back(update.coarse_global_id, source,
                                           *local_id, update.block);
      }
      std::ranges::sort(received_ids);
      neighbor_received_is_valid =
          neighbor_received_is_valid &&
          std::ranges::adjacent_find(received_ids) == received_ids.end() &&
          std::ranges::equal(received_ids, expected);
    }
    std::ranges::sort(pending_ghost_updates, {}, [](auto const& update) {
      return std::tie(std::get<0>(update), std::get<1>(update));
    });
    neighbor_received_is_valid =
        neighbor_received_is_valid &&
        std::ranges::all_of(ghost_assigned,
                            [](auto assigned) { return assigned != 0; }) &&
        std::ranges::adjacent_find(
            pending_ghost_updates, [](auto const& lhs, auto const& rhs) {
              return std::get<0>(lhs) == std::get<0>(rhs);
            }) == pending_ghost_updates.end();
  } catch (...) {
    mpi::abort_on_exception(plan.topology().native_handle(),
                            "block-down neighbor receive staging");
  }
  if (!mpi::detail::collective_predicate(neighbor_received_is_valid,
                                         plan.topology().view())) {
    mpi::throw_collectively_agreed_semantic_error(
        plan.topology().native_handle(),
        "block-down neighbor received validation failed");
  }

  try {
    for (auto local = std::size_t{0}; local < owned_blocks.size(); ++local) {
      Q.setSecondPartitionIndex(static_cast<NodeID>(local),
                                owned_blocks[local]);
    }
    for (auto const& [global_id, source, local_id, block] :
         pending_ghost_updates) {
      static_cast<void>(global_id);
      static_cast<void>(source);
      Q.setSecondPartitionIndex(local_id, block);
    }
  } catch (...) {
    mpi::abort_on_exception(plan.topology().native_handle(),
                            "block-down state commit");
  }

  try {
    for (auto local = std::size_t{0}; local < owned_blocks.size(); ++local) {
      auto const node = static_cast<NodeID>(local);
      KAHIP_MPI_TRACE(mpi::trace::block_propagation(
          mpi::trace::current_hierarchy(), Q.getGlobalID(node), rank, rank,
          owned_blocks[local]));
    }
    for (auto const& [global_id, source, local_id, block] :
         pending_ghost_updates) {
      static_cast<void>(local_id);
      KAHIP_MPI_TRACE(mpi::trace::block_propagation(
          mpi::trace::current_hierarchy(), global_id, source, rank, block));
    }
  } catch (...) {
    mpi::abort_on_exception(plan.topology().native_handle(),
                            "block-down trace commit");
  }
}
}  // namespace parhip
