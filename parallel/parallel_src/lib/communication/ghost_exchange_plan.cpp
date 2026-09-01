#include "communication/ghost_exchange_plan.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <memory>
#include <ranges>
#include <utility>
#include <vector>

#include "communication/mpi_failure.h"
#include "data_structure/parallel_graph_access.h"

namespace parhip {
namespace {
struct flat_segments {
  std::vector<NodeID> storage;
  std::vector<std::size_t> offsets;
  std::vector<std::size_t> counts;
};

[[nodiscard]] auto flatten(std::vector<std::vector<NodeID>> const& segments)
    -> flat_segments {
  auto result = flat_segments{};
  result.offsets.reserve(segments.size());
  result.counts.reserve(segments.size());
  for (auto const& segment : segments) {
    result.offsets.push_back(result.storage.size());
    result.counts.push_back(segment.size());
    result.storage.insert(result.storage.end(), segment.begin(), segment.end());
  }
  return result;
}

[[nodiscard]] auto segment(std::span<NodeID const> storage,
                           std::span<std::size_t const> offsets,
                           std::span<std::size_t const> counts,
                           std::size_t index) noexcept
    -> std::span<NodeID const> {
  if (index >= offsets.size() || index >= counts.size() ||
      offsets[index] > storage.size() ||
      counts[index] > storage.size() - offsets[index]) {
    return {};
  }
  return storage.subspan(offsets[index], counts[index]);
}
}  // namespace

ghost_exchange_plan::ghost_exchange_plan(
    mpi::distributed_graph topology,
    std::vector<NodeID> outgoing_nodes,
    std::vector<std::size_t> outgoing_offsets,
    std::vector<std::size_t> outgoing_counts,
    std::vector<NodeID> expected_ghosts,
    std::vector<std::size_t> expected_offsets,
    std::vector<std::size_t> expected_counts) noexcept
    : topology_(std::move(topology)),
      outgoing_nodes_(std::move(outgoing_nodes)),
      outgoing_offsets_(std::move(outgoing_offsets)),
      outgoing_counts_(std::move(outgoing_counts)),
      expected_ghosts_(std::move(expected_ghosts)),
      expected_offsets_(std::move(expected_offsets)),
      expected_counts_(std::move(expected_counts)) {}

auto ghost_exchange_plan::outgoing_local_nodes(
    std::size_t destination_index) const noexcept -> std::span<NodeID const> {
  return segment(outgoing_nodes_, outgoing_offsets_, outgoing_counts_,
                 destination_index);
}

auto ghost_exchange_plan::expected_ghost_nodes(
    std::size_t source_index) const noexcept -> std::span<NodeID const> {
  return segment(expected_ghosts_, expected_offsets_, expected_counts_,
                 source_index);
}

auto make_ghost_exchange_plan(parallel_graph_access const& graph)
    -> std::unique_ptr<ghost_exchange_plan> {
  auto const graph_communicator = mpi::communicator_view{graph.m_communicator};
  auto const rank = graph_communicator.rank();
  auto const size = graph_communicator.size();

  auto local_structure_is_valid =
      graph.m_graph_construction_complete && !graph.m_building_graph &&
      std::in_range<std::size_t>(graph.m_num_local_nodes);
  auto owners_by_local_node = std::vector<std::vector<int>>{};
  auto outgoing_destinations = std::vector<int>{};
  auto outgoing_by_rank = std::vector<std::pair<int, std::vector<NodeID>>>{};
  auto referenced_ghosts = std::vector<unsigned char>{};
  try {
    if (local_structure_is_valid) {
      auto const local_count =
          static_cast<std::size_t>(graph.m_num_local_nodes);
      local_structure_is_valid =
          graph.m_nodes.size() >= local_count + std::size_t{1} &&
          graph.m_nodes_data.size() == graph.m_nodes.size() &&
          graph.m_ghost_adddata_array_offset ==
              graph.m_num_local_nodes + NodeID{1};
      owners_by_local_node.resize(local_count);
      referenced_ghosts.assign(graph.m_add_non_local_node_data.size(),
                               static_cast<unsigned char>(0));

      for (std::size_t local = 0;
           local < local_count && local_structure_is_valid; ++local) {
        auto const first = graph.m_nodes[local].firstEdge;
        auto const last = graph.m_nodes[local + 1].firstEdge;
        local_structure_is_valid =
            first <= last && std::in_range<std::size_t>(first) &&
            std::in_range<std::size_t>(last) &&
            static_cast<std::size_t>(last) <= graph.m_edges.size();
        if (!local_structure_is_valid) {
          break;
        }

        auto& owners = owners_by_local_node[local];
        for (auto edge = static_cast<std::size_t>(first);
             edge < static_cast<std::size_t>(last); ++edge) {
          auto const target = graph.m_edges[edge].local_target;
          if (target < graph.m_num_local_nodes) {
            continue;
          }
          if (target < graph.m_ghost_adddata_array_offset ||
              !std::in_range<std::size_t>(target -
                                          graph.m_ghost_adddata_array_offset)) {
            local_structure_is_valid = false;
            break;
          }
          auto const ghost_index = static_cast<std::size_t>(
              target - graph.m_ghost_adddata_array_offset);
          if (ghost_index >= graph.m_add_non_local_node_data.size()) {
            local_structure_is_valid = false;
            break;
          }
          referenced_ghosts[ghost_index] = 1;
          auto const owner = graph.m_add_non_local_node_data[ghost_index].peID;
          if (owner < 0 || owner >= size || owner == rank) {
            local_structure_is_valid = false;
            break;
          }
          owners.push_back(owner);
        }
        std::ranges::sort(owners);
        auto const unique_end = std::ranges::unique(owners);
        owners.erase(unique_end.begin(), unique_end.end());
        outgoing_destinations.insert(outgoing_destinations.end(),
                                     owners.begin(), owners.end());
      }

      std::ranges::sort(outgoing_destinations);
      auto const unique_end = std::ranges::unique(outgoing_destinations);
      outgoing_destinations.erase(unique_end.begin(), unique_end.end());
      outgoing_by_rank.reserve(outgoing_destinations.size());
      for (auto const destination : outgoing_destinations) {
        outgoing_by_rank.emplace_back(destination, std::vector<NodeID>{});
      }
      for (std::size_t local = 0; local < owners_by_local_node.size();
           ++local) {
        for (auto const owner : owners_by_local_node[local]) {
          auto const position = std::ranges::lower_bound(
              outgoing_by_rank, owner, {},
              &std::pair<int, std::vector<NodeID>>::first);
          if (position == outgoing_by_rank.end() || position->first != owner) {
            local_structure_is_valid = false;
            continue;
          }
          position->second.push_back(static_cast<NodeID>(local));
        }
      }
      local_structure_is_valid =
          local_structure_is_valid &&
          std::ranges::all_of(
              referenced_ghosts,
              [](unsigned char const referenced) { return referenced != 0; });
    }
  } catch (...) {
    mpi::abort_on_exception(graph.m_communicator,
                            "ghost exchange plan pre-topology allocation");
  }

  if (!mpi::detail::collective_predicate(local_structure_is_valid,
                                         graph_communicator)) {
    mpi::throw_collectively_agreed_semantic_error(
        graph.m_communicator, "ghost exchange plan graph validation failed");
  }

  auto semantic_failure = false;
  auto result = std::unique_ptr<ghost_exchange_plan>{};
  {
    auto topology =
        mpi::distributed_graph{graph_communicator, outgoing_destinations};
    try {
      auto local_plan_is_valid = true;
      auto sorted_sources = std::vector<int>{topology.sources().begin(),
                                             topology.sources().end()};
      auto sorted_destinations = std::vector<int>{
          topology.destinations().begin(), topology.destinations().end()};
      std::ranges::sort(sorted_sources);
      std::ranges::sort(sorted_destinations);
      local_plan_is_valid = sorted_sources == sorted_destinations;

      auto outgoing_segments =
          std::vector<std::vector<NodeID>>(topology.destinations().size());
      for (std::size_t index = 0; index < topology.destinations().size();
           ++index) {
        auto const destination = topology.destinations()[index];
        auto const position = std::ranges::lower_bound(
            outgoing_by_rank, destination, {},
            &std::pair<int, std::vector<NodeID>>::first);
        if (position == outgoing_by_rank.end() ||
            position->first != destination) {
          local_plan_is_valid = false;
          continue;
        }
        outgoing_segments[index] = position->second;
        local_plan_is_valid =
            local_plan_is_valid &&
            std::ranges::is_sorted(outgoing_segments[index]) &&
            std::ranges::adjacent_find(outgoing_segments[index]) ==
                outgoing_segments[index].end();
      }

      auto expected_segments =
          std::vector<std::vector<NodeID>>(topology.sources().size());
      auto const ghost_offset = graph.m_ghost_adddata_array_offset;
      local_plan_is_valid =
          local_plan_is_valid && std::in_range<std::size_t>(ghost_offset) &&
          static_cast<std::size_t>(ghost_offset) <= graph.m_nodes.size() &&
          graph.m_nodes.size() - static_cast<std::size_t>(ghost_offset) ==
              graph.m_add_non_local_node_data.size() &&
          graph.m_global_to_local_id.size() ==
              graph.m_add_non_local_node_data.size();

      for (std::size_t ghost_index = 0;
           ghost_index < graph.m_add_non_local_node_data.size();
           ++ghost_index) {
        auto const& metadata = graph.m_add_non_local_node_data[ghost_index];
        auto const source_index = topology.source_index(metadata.peID);
        if (!source_index.has_value() || metadata.peID < 0 ||
            metadata.peID >= size || !std::in_range<NodeID>(ghost_index) ||
            ghost_offset > std::numeric_limits<NodeID>::max() -
                               static_cast<NodeID>(ghost_index)) {
          local_plan_is_valid = false;
          continue;
        }
        auto const local_id = ghost_offset + static_cast<NodeID>(ghost_index);
        auto const mapping = graph.m_global_to_local_id.find(metadata.globalID);
        if (mapping == graph.m_global_to_local_id.end() ||
            mapping->second != local_id) {
          local_plan_is_valid = false;
          continue;
        }
        expected_segments[*source_index].push_back(metadata.globalID);
      }
      for (auto& expected : expected_segments) {
        std::ranges::sort(expected);
        if (std::ranges::adjacent_find(expected) != expected.end()) {
          local_plan_is_valid = false;
        }
      }

      auto outgoing = flat_segments{};
      auto expected = flat_segments{};
      if (local_plan_is_valid) {
        outgoing = flatten(outgoing_segments);
        expected = flatten(expected_segments);
      }

      if (!mpi::detail::collective_predicate(local_plan_is_valid,
                                             topology.view())) {
        semantic_failure = true;
      } else {
        result = std::unique_ptr<ghost_exchange_plan>{new ghost_exchange_plan{
            std::move(topology), std::move(outgoing.storage),
            std::move(outgoing.offsets), std::move(outgoing.counts),
            std::move(expected.storage), std::move(expected.offsets),
            std::move(expected.counts)}};
        if (!mpi::detail::collective_predicate(result != nullptr,
                                               result->topology().view())) {
          mpi::abort_on_programming_error(
              result->topology().native_handle(),
              "ghost exchange plan readiness diverged across ranks");
        }
      }
    } catch (...) {
      auto const affected = result == nullptr
                                ? topology.native_handle()
                                : result->topology().native_handle();
      mpi::abort_on_exception(affected,
                              "ghost exchange plan post-topology failure");
    }
  }

  if (semantic_failure) {
    mpi::throw_collectively_agreed_semantic_error(
        graph.m_communicator,
        "ghost exchange plan semantic validation failed");
  }
  return result;
}
}  // namespace parhip
