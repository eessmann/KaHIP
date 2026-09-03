/******************************************************************************
 * dspac.cpp
 * *
 * Source of KaHIP -- Karlsruhe High Quality Partitioning.
 * Author: Daniel Seemaier <daniel.seemaier@student.kit.edu>
 * Christian Schulz <christian.schulz.phone@gmail.com>
 *****************************************************************************/

#include "dspac.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <limits>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "communication/mpi_adapter.h"
#include "communication/mpi_fixed_reduction.h"
#include "communication/mpi_types.h"

namespace parhip {
namespace {
[[nodiscard]] auto is_monotone(std::span<NodeID const> values) noexcept
    -> bool {
  return std::ranges::adjacent_find(values, std::greater<>{}) == values.end();
}

void require_collectively(bool local_condition,
                          mpi::communicator_view communicator,
                          std::string_view diagnostic) noexcept {
  if (!mpi::detail::collective_predicate(local_condition, communicator)) {
    mpi::abort_on_programming_error(communicator.native_handle(), diagnostic);
  }
}

[[nodiscard]] auto arrays_agree_collectively(
    std::span<NodeID const> values,
    mpi::communicator_view communicator) -> bool {
  auto minimum = std::vector<NodeID>(values.size());
  auto maximum = std::vector<NodeID>(values.size());
  mpi::all_reduce_bounded(
      values, std::span<NodeID>{minimum}, mpi::reduction_kind::minimum,
      communicator, "MPI_Allreduce(DSPAC range minimum)");
  mpi::all_reduce_bounded(
      values, std::span<NodeID>{maximum}, mpi::reduction_kind::maximum,
      communicator, "MPI_Allreduce(DSPAC range maximum)");
  return minimum == maximum;
}

[[nodiscard]] auto checked_split_edge_count(EdgeID directed_edges,
                                            NodeID low_degree_vertices,
                                            EdgeID& result) noexcept -> bool {
  constexpr auto maximum = std::numeric_limits<EdgeID>::max();
  if (low_degree_vertices > directed_edges) {
    return false;
  }
  // 3m - 2c == m + 2(m - c), and every counted degree-one/two vertex
  // contributes at least one directed edge.  This form admits every
  // representable result without overflowing an intermediate expression.
  auto const additional_edges = directed_edges - low_degree_vertices;
  if (additional_edges > (maximum - directed_edges) / 2) {
    return false;
  }
  result = directed_edges + EdgeID{2} * additional_edges;
  return true;
}
}  // namespace

dspac::dspac(parallel_graph_access& graph,
             MPI_Comm comm,
             EdgeWeight infinity,
             mpi::collective_options collective_options)
    : m_comm(comm),
      m_infinity(infinity),
      m_input_graph(graph),
      m_collective_options(collective_options) {}

void dspac::construct(parallel_graph_access& split_graph) {
  auto operation_communicator =
      mpi::communicator{mpi::communicator_view{m_comm}};
  auto const communicator = operation_communicator.view();
  mpi::check_or_abort(MPI_Barrier(communicator.native_handle()),
                      communicator.native_handle(),
                      "MPI_Barrier(before DSPAC construction)");
  internal_construct(split_graph, communicator);
  mpi::check_or_abort(MPI_Barrier(communicator.native_handle()),
                      communicator.native_handle(),
                      "MPI_Barrier(after DSPAC construction)");
  assert(assert_sanity_checks(split_graph));
}

void dspac::internal_construct(parallel_graph_access& split_graph,
                               mpi::communicator_view communicator) {
  auto const size = communicator.size();
  auto const rank = communicator.rank();

  try {
    auto const n = m_input_graph.number_of_global_nodes();
    auto const global_input_edges = m_input_graph.number_of_global_edges();
    auto const local_input_nodes = m_input_graph.number_of_local_nodes();
    auto const local_input_edges = m_input_graph.number_of_local_edges();
    auto const range_count = static_cast<std::size_t>(size) + 1;
    auto const rank_index = static_cast<std::size_t>(rank);

    timer construction_timer;

    auto edge_range_array = m_input_graph.get_edge_range_array();
    auto node_range_array = m_input_graph.get_range_array();
    require_collectively(
        edge_range_array.size() == range_count &&
            node_range_array.size() == range_count,
        communicator, "DSPAC range arrays must contain one entry per rank boundary");

    // The legacy loader may leave only this terminal value stale.
    node_range_array.back() = n;
    auto const ranges_are_locally_valid =
        edge_range_array.front() == 0 &&
        edge_range_array.back() == global_input_edges &&
        node_range_array.front() == 0 && node_range_array.back() == n &&
        is_monotone(edge_range_array) && is_monotone(node_range_array) &&
        edge_range_array[rank_index + 1] - edge_range_array[rank_index] ==
            local_input_edges &&
        node_range_array[rank_index + 1] - node_range_array[rank_index] ==
            local_input_nodes &&
        std::ranges::all_of(node_range_array, [](NodeID value) {
          return std::in_range<std::size_t>(value);
        });
    require_collectively(ranges_are_locally_valid, communicator,
                         "DSPAC graph ranges are invalid");
    require_collectively(
        arrays_agree_collectively(edge_range_array, communicator) &&
            arrays_agree_collectively(node_range_array, communicator),
        communicator, "DSPAC graph ranges differ across ranks");

    auto const from = edge_range_array[rank_index];  // inclusive
    auto const to = edge_range_array[rank_index + 1];  // exclusive

    auto local_number_of_deg_1_or_2_vertices = NodeID{};
    for (NodeID vertex = 0; vertex < local_input_nodes; ++vertex) {
      auto const degree = m_input_graph.getNodeDegree(vertex);
      if (degree == 1 || degree == 2) {
        ++local_number_of_deg_1_or_2_vertices;
      }
    }
    auto const global_number_of_deg_1_or_2_vertices = mpi::all_reduce_sum(
        local_number_of_deg_1_or_2_vertices, communicator,
        "MPI_Allreduce(DSPAC degree-one-or-two count)");
    if (rank == 0) {
      std::cout << "[dspac::internal_construct()] Up to MPI_Allreduce() took "
                << construction_timer.elapsed() << std::endl;
      construction_timer.restart();
    }

    auto const local_number_of_split_nodes = local_input_edges;
    auto const global_number_of_split_nodes = global_input_edges;
    auto local_number_of_split_edges = EdgeID{};
    auto global_number_of_split_edges = EdgeID{};
    require_collectively(
        checked_split_edge_count(local_input_edges,
                                 local_number_of_deg_1_or_2_vertices,
                                 local_number_of_split_edges) &&
            checked_split_edge_count(global_input_edges,
                                     global_number_of_deg_1_or_2_vertices,
                                     global_number_of_split_edges),
        communicator, "DSPAC split-graph dimension arithmetic is invalid");

    auto outgoing_ranks = std::vector<int>{};
    auto adjacency_is_valid = true;
    for (NodeID vertex = 0; vertex < local_input_nodes; ++vertex) {
      auto previous_global_target = NodeID{};
      auto has_previous_target = false;
      for (auto edge = m_input_graph.get_first_edge(vertex),
                end = m_input_graph.get_first_invalid_edge(vertex);
           edge < end; ++edge) {
        auto const target = m_input_graph.getEdgeTarget(edge);
        auto const global_target = m_input_graph.getGlobalID(target);
        auto const owner = m_input_graph.is_local_node(target)
                               ? rank
                               : m_input_graph.getTargetPE(target);
        adjacency_is_valid =
            adjacency_is_valid && global_target < n && owner >= 0 &&
            owner < size &&
            (!has_previous_target || previous_global_target <= global_target);
        if (owner != rank && owner >= 0 && owner < size) {
          outgoing_ranks.push_back(owner);
        }
        previous_global_target = global_target;
        has_previous_target = true;
      }
    }
    require_collectively(adjacency_is_valid, communicator,
                         "DSPAC adjacency must be sorted and have valid owners");
    std::ranges::sort(outgoing_ranks);
    auto const unique_ranks = std::ranges::unique(outgoing_ranks);
    outgoing_ranks.erase(unique_ranks.begin(), unique_ranks.end());

    auto const local_node_count = static_cast<std::size_t>(local_input_nodes);
    auto first_split_node_on = std::vector<std::vector<NodeID>>(
        static_cast<std::size_t>(size));
    first_split_node_on[rank_index].resize(local_node_count);
    for (auto const destination : outgoing_ranks) {
      first_split_node_on[static_cast<std::size_t>(destination)].resize(
          local_node_count);
    }

    for (NodeID vertex = 0; vertex < local_input_nodes; ++vertex) {
      auto current_owner = PEID{-1};
      for (auto edge = m_input_graph.get_first_edge(vertex),
                end = m_input_graph.get_first_invalid_edge(vertex);
           edge < end; ++edge) {
        auto const target = m_input_graph.getEdgeTarget(edge);
        auto const owner = m_input_graph.is_local_node(target)
                               ? rank
                               : m_input_graph.getTargetPE(target);
        if (owner != current_owner) {
          first_split_node_on[static_cast<std::size_t>(owner)]
                             [static_cast<std::size_t>(vertex)] = from + edge;
          current_owner = owner;
        }
      }
    }

    if (rank == 0) {
      std::cout << "[dspac::internal_construct()] Preparation of "
                   "first_split_node_on[] took "
                << construction_timer.elapsed() << std::endl;
      construction_timer.restart();
    }

    auto first_split_node =
        std::vector<NodeID>(static_cast<std::size_t>(n));
    {
      auto topology = mpi::distributed_graph{communicator, outgoing_ranks};
      auto outgoing = std::vector<std::vector<NodeID>>{};
      outgoing.reserve(topology.destinations().size());
      for (auto const destination : topology.destinations()) {
        outgoing.push_back(
            first_split_node_on[static_cast<std::size_t>(destination)]);
      }
      auto received = mpi::neighbor_all_to_all_v(
          mpi::segmented_buffer<NodeID>::from_segments(outgoing), topology,
          m_collective_options);

      auto received_shape_is_valid =
          received.segment_count() == topology.sources().size();
      if (received_shape_is_valid) {
        for (std::size_t index = 0; index < topology.sources().size(); ++index) {
          auto const source = topology.sources()[index];
          auto const source_is_valid = source >= 0 && source < size;
          received_shape_is_valid =
              received_shape_is_valid && source_is_valid;
          if (!source_is_valid) {
            continue;
          }
          auto const source_index = static_cast<std::size_t>(source);
          auto const expected = node_range_array[source_index + 1] -
                                node_range_array[source_index];
          received_shape_is_valid =
              received_shape_is_valid &&
              std::in_range<std::size_t>(expected) &&
              received.segment(index).size() ==
                  static_cast<std::size_t>(expected);
        }
      }
      require_collectively(received_shape_is_valid, topology.view(),
                           "DSPAC first-split source segment extent mismatch");

      auto const own_offset =
          static_cast<std::size_t>(node_range_array[rank_index]);
      std::ranges::copy(first_split_node_on[rank_index],
                        first_split_node.begin() + own_offset);
      for (std::size_t index = 0; index < topology.sources().size(); ++index) {
        auto const source_index =
            static_cast<std::size_t>(topology.sources()[index]);
        auto const offset =
            static_cast<std::size_t>(node_range_array[source_index]);
        std::ranges::copy(received.segment(index),
                          first_split_node.begin() + offset);
      }
    }

    if (rank == 0) {
      std::cout << "[dspac::internal_construct()] first_split_node[] "
                   "communication took "
                << construction_timer.elapsed() << std::endl;
      construction_timer.restart();
    }

    first_split_node_on.clear();

    split_graph.start_construction(
        local_number_of_split_nodes, local_number_of_split_edges,
        global_number_of_split_nodes, global_number_of_split_edges);
    split_graph.set_range_array(edge_range_array);
    split_graph.set_range(from, from == to ? from : to - EdgeID{1});

    auto nodes_created = NodeID{};
    auto edges_created = EdgeID{};
    for (NodeID vertex = 0; vertex < local_input_nodes; ++vertex) {
      auto const degree = m_input_graph.getNodeDegree(vertex);
      if (degree == 0) {
        continue;
      }

      for (auto edge = m_input_graph.get_first_edge(vertex),
                end = m_input_graph.get_first_invalid_edge(vertex);
           edge < end; ++edge) {
        auto const target = m_input_graph.getEdgeTarget(edge);
        auto const global_target = m_input_graph.getGlobalID(target);
        if (!std::in_range<std::size_t>(global_target) ||
            static_cast<std::size_t>(global_target) >=
                first_split_node.size()) {
          mpi::abort_on_programming_error(
              communicator.native_handle(),
              "DSPAC dominant-edge target is outside the node domain");
        }

        ++nodes_created;
        auto const split_node = split_graph.new_node();
        if (split_node != edge) {
          mpi::abort_on_programming_error(
              communicator.native_handle(),
              "DSPAC split-node construction order diverged");
        }
        split_graph.setNodeWeight(split_node, 1);
        split_graph.setNodeLabel(split_node, from + split_node);
        split_graph.setSecondPartitionIndex(split_node, 0);

        auto& first_target =
            first_split_node[static_cast<std::size_t>(global_target)];
        if (first_target >= global_number_of_split_nodes) {
          mpi::abort_on_programming_error(
              communicator.native_handle(),
              "DSPAC reciprocal split-node mapping is invalid");
        }
        ++edges_created;
        auto const dominant_edge =
            split_graph.new_edge(split_node, first_target);
        ++first_target;
        split_graph.setEdgeWeight(dominant_edge, m_infinity);

        auto const first = edge == m_input_graph.get_first_edge(vertex);
        auto const last = edge + 1 == end;
        if (degree == 2) {
          auto const auxiliary_local =
              first ? split_node + NodeID{1} : split_node - NodeID{1};
          if (auxiliary_local >= local_number_of_split_nodes) {
            mpi::abort_on_programming_error(
                communicator.native_handle(),
                "DSPAC degree-two auxiliary edge is invalid");
          }
          ++edges_created;
          auto const auxiliary_edge =
              split_graph.new_edge(split_node, from + auxiliary_local);
          split_graph.setEdgeWeight(auxiliary_edge, 1);
        } else if (degree > 2) {
          auto const span = degree - EdgeID{1};
          auto const next_local =
              last ? split_node - span : split_node + NodeID{1};
          auto const previous_local =
              first ? split_node + span : split_node - NodeID{1};
          if (next_local >= local_number_of_split_nodes ||
              previous_local >= local_number_of_split_nodes) {
            mpi::abort_on_programming_error(
                communicator.native_handle(),
                "DSPAC cycle auxiliary edge is invalid");
          }
          ++edges_created;
          auto const next_edge =
              split_graph.new_edge(split_node, from + next_local);
          split_graph.setEdgeWeight(next_edge, 1);
          ++edges_created;
          auto const previous_edge =
              split_graph.new_edge(split_node, from + previous_local);
          split_graph.setEdgeWeight(previous_edge, 1);
        } else if (degree != 1) {
          mpi::abort_on_programming_error(
              communicator.native_handle(),
              "DSPAC encountered an invalid nonzero degree");
        }
      }
    }

    if (rank == 0) {
      std::cout << "[dspac::internal_construct()] Local construction took "
                << construction_timer.elapsed() << std::endl;
      construction_timer.restart();
    }
    if (nodes_created != local_number_of_split_nodes ||
        edges_created != local_number_of_split_edges) {
      mpi::abort_on_programming_error(
          communicator.native_handle(),
          "DSPAC local split-graph dimensions diverged");
    }
    split_graph.finish_construction();
  } catch (...) {
    mpi::abort_on_exception(communicator.native_handle(),
                            "DSPAC split-graph construction failed");
  }
}

/**
 * assert()'s some sanity checks on the split graph.
 * @return Pointless bool so that the method call can be used as expression.
 */
bool dspac::assert_sanity_checks(parallel_graph_access &split_graph) {
#ifndef NDEBUG
  assert(split_graph.number_of_local_nodes() == m_input_graph.number_of_local_edges());
  for (NodeID v = 0; v < split_graph.number_of_local_nodes(); ++v) {
    // isolated vertices should be removed for now
    assert(0 < split_graph.getNodeDegree(v) && split_graph.getNodeDegree(v) <= 3);

    // make sure that the edge weights are correct, i.e. auxiliary edges have edge weight 1 and
    // dominant edges have edge weight m_infinity
    EdgeID firstEdge = split_graph.get_first_edge(v);
    switch (split_graph.getNodeDegree(v)) {
      case 3: // fall through intended
        assert(split_graph.getEdgeWeight(firstEdge + 2) == 1);

      case 2:
        assert(split_graph.getEdgeWeight(firstEdge + 1) == 1);

      case 1:
        assert(split_graph.getEdgeWeight(firstEdge) == m_infinity);
      break;

      default:
        assert(false);
    }
  }

  // this part checks that the auxiliary edges are connected to the right nodes
  for (NodeID v = 0; v < m_input_graph.number_of_local_nodes(); ++v) {
    EdgeID deg = m_input_graph.getNodeDegree(v);
    if (deg == 0) { // explicitly skip isolated nodes
      continue;
    }

    for (EdgeID e = m_input_graph.get_first_edge(v); e < m_input_graph.get_first_invalid_edge(v); ++e) {
      bool first = (e == m_input_graph.get_first_edge(v));
      bool last = (e + 1 == m_input_graph.get_first_invalid_edge(v));

      if (deg == 1) {
        if (split_graph.get_first_edge(e) + 1 < split_graph.number_of_local_edges()) {
          // degree 1 node --> no auxiliary edges --> next edge must be a dominant edge of another node
          assert(split_graph.getEdgeWeight(split_graph.get_first_edge(e) + 1) == m_infinity);
        }
      } else if (deg == 2) {
        if (first) {
          // first split node --> auxiliary edge must target the second split node
          assert(split_graph.getEdgeTarget(split_graph.get_first_edge(e) + 1) == e + 1);
        } else if (last) {
          // second split node --> auxiliary edge must target the first split node
          assert(split_graph.getEdgeTarget(split_graph.get_first_edge(e) + 1) == e - 1);
        } else {
          assert(false);
        }

        // a dominant edge must follow a single auxiliary edge
        if (split_graph.get_first_edge(e) + 2 < split_graph.number_of_local_edges()) {
          assert(split_graph.getEdgeWeight(split_graph.get_first_edge(e) + 2) == m_infinity);
        }
      } else if (deg > 2) {
        if (first) {
          assert(split_graph.getEdgeTarget(split_graph.get_first_edge(e) + 1) == e + 1);
          assert(split_graph.getEdgeTarget(split_graph.get_first_edge(e) + 2) == e + (deg - 1));
        } else if (last) {
          assert(split_graph.getEdgeTarget(split_graph.get_first_edge(e) + 1) == e - (deg - 1));
          assert(split_graph.getEdgeTarget(split_graph.get_first_edge(e) + 2) == e - 1);
        } else {
          assert(split_graph.getEdgeTarget(split_graph.get_first_edge(e) + 1) == e + 1);
          assert(split_graph.getEdgeTarget(split_graph.get_first_edge(e) + 2) == e - 1);
        }

        // a dominant edge must follow after two auxiliary edges
        if (split_graph.get_first_edge(e) + 3 < split_graph.number_of_local_edges()) {
          assert(split_graph.getEdgeWeight(split_graph.get_first_edge(e) + 3) == m_infinity);
        }
      } else {
        assert(false);
      }
    }
  }
#endif
  return true;
}

std::vector<PartitionID> dspac::project_partition(
    parallel_graph_access& split_graph,
    std::vector<EdgeID> const& permutation) {
  auto operation_communicator =
      mpi::communicator{mpi::communicator_view{m_comm}};
  auto const communicator = operation_communicator.view();
  try {
    auto const local_edges = m_input_graph.number_of_local_edges();
    auto local_permutation_is_valid =
        std::in_range<std::size_t>(local_edges) &&
        split_graph.number_of_local_nodes() == local_edges;
    auto local_edge_count = std::size_t{};
    if (local_permutation_is_valid) {
      local_edge_count = static_cast<std::size_t>(local_edges);
      local_permutation_is_valid = permutation.size() == local_edge_count;
    }

    auto seen = std::vector<bool>{};
    if (local_permutation_is_valid) {
      seen.resize(local_edge_count);
      for (auto const target : permutation) {
        if (target >= local_edges ||
            seen[static_cast<std::size_t>(target)]) {
          local_permutation_is_valid = false;
          break;
        }
        seen[static_cast<std::size_t>(target)] = true;
      }
    }
    require_collectively(
        local_permutation_is_valid, communicator,
        "DSPAC projection permutation must be a bijection over local edges");

    auto edge_partition = std::vector<PartitionID>(local_edge_count);
    for (NodeID vertex = 0;
         vertex < m_input_graph.number_of_local_nodes(); ++vertex) {
      for (auto edge = m_input_graph.get_first_edge(vertex),
                end = m_input_graph.get_first_invalid_edge(vertex);
           edge < end; ++edge) {
        edge_partition[static_cast<std::size_t>(
            permutation[static_cast<std::size_t>(edge)])] =
            split_graph.getNodeLabel(edge);
      }
    }

    mpi::check_or_abort(MPI_Barrier(communicator.native_handle()),
                        communicator.native_handle(),
                        "MPI_Barrier(after DSPAC projection)");
    return edge_partition;
  } catch (...) {
    mpi::abort_on_exception(communicator.native_handle(),
                            "DSPAC partition projection failed");
  }
}

EdgeWeight dspac::calculate_vertex_cut(
    PartitionID k,
    std::vector<PartitionID> const& edge_partition) {
  mpi::require_live_intracommunicator(
      mpi::communicator_view{m_comm},
      "vertex cut validation requires a live intracommunicator");

  auto minimum_k = PartitionID{};
  auto maximum_k = PartitionID{};
  mpi::check_or_abort(
      MPI_Allreduce(&k, &minimum_k, 1, mpi::get_mpi_datatype<PartitionID>(),
                    MPI_MIN, m_comm),
      m_comm, "MPI_Allreduce(vertex cut k minimum)");
  mpi::check_or_abort(
      MPI_Allreduce(&k, &maximum_k, 1, mpi::get_mpi_datatype<PartitionID>(),
                    MPI_MAX, m_comm),
      m_comm, "MPI_Allreduce(vertex cut k maximum)");

  constexpr auto zero_k = PartitionID{1} << 0;
  constexpr auto unrepresentable_k = PartitionID{1} << 1;
  constexpr auto mismatched_k = PartitionID{1} << 2;
  constexpr auto mismatched_partition_extent = PartitionID{1} << 3;
  constexpr auto out_of_range_label = PartitionID{1} << 4;

  auto local_issues = PartitionID{};
  if (k == 0) {
    local_issues |= zero_k;
  }
  if (!std::in_range<std::size_t>(k)) {
    local_issues |= unrepresentable_k;
  }
  if (minimum_k != maximum_k) {
    local_issues |= mismatched_k;
  }
  auto const local_edge_count = m_input_graph.number_of_local_edges();
  if (!std::in_range<std::size_t>(local_edge_count) ||
      (std::in_range<std::size_t>(local_edge_count) &&
       edge_partition.size() != static_cast<std::size_t>(local_edge_count))) {
    local_issues |= mismatched_partition_extent;
  }
  if (std::ranges::any_of(edge_partition,
                          [k](PartitionID label) { return label >= k; })) {
    local_issues |= out_of_range_label;
  }

  auto global_issues = PartitionID{};
  mpi::check_or_abort(
      MPI_Allreduce(&local_issues, &global_issues, 1,
                    mpi::get_mpi_datatype<PartitionID>(), MPI_BOR, m_comm),
      m_comm, "MPI_Allreduce(vertex cut validation)");

  if ((global_issues & zero_k) != 0) {
    mpi::abort_on_programming_error(m_comm,
                                    "vertex cut requires k greater than zero");
  }
  if ((global_issues & unrepresentable_k) != 0) {
    mpi::abort_on_programming_error(
        m_comm, "vertex cut k exceeds local size_t capacity");
  }
  if ((global_issues & mismatched_k) != 0) {
    mpi::abort_on_programming_error(m_comm,
                                    "vertex cut k differs across communicator");
  }
  if ((global_issues & mismatched_partition_extent) != 0) {
    mpi::abort_on_programming_error(
        m_comm, "vertex cut partition extent does not match local edge count");
  }
  if ((global_issues & out_of_range_label) != 0) {
    mpi::abort_on_programming_error(
        m_comm, "vertex cut partition label is outside [0, k)");
  }

  auto local_cost = EdgeWeight{};
  auto counted = std::vector<bool>(static_cast<std::size_t>(k));

  for (NodeID v = 0; v < m_input_graph.number_of_local_nodes(); ++v) {
    if (m_input_graph.getNodeDegree(v) == 0) {
      continue;
    }

    auto distinct_blocks = EdgeWeight{};
    for (EdgeID e = m_input_graph.get_first_edge(v);
         e < m_input_graph.get_first_invalid_edge(v); ++e) {
      auto const p = edge_partition[static_cast<std::size_t>(e)];
      if (!counted[p]) {
        counted[p] = true;
        ++distinct_blocks;
      }
    }

    assert(distinct_blocks > 0);
    local_cost += distinct_blocks - 1;
    std::ranges::fill(counted, false);
  }

  return mpi::all_reduce_sum(local_cost, mpi::communicator_view{m_comm},
                             "MPI_Allreduce(vertex cut)");
}

void dspac::fix_cut_dominant_edges(parallel_graph_access &split_graph) {
  for (NodeID v = 0; v < split_graph.number_of_local_nodes(); ++v) {
    EdgeID e_vu = split_graph.get_first_edge(v);
    NodeID u = split_graph.getEdgeTarget(e_vu);

    PartitionID part_v = split_graph.getNodeLabel(v);
    PartitionID part_u = split_graph.getNodeLabel(u);
    if (part_v != part_u) {
      NodeWeight part_v_size = split_graph.getBlockSize(part_v);
      NodeWeight part_u_size = split_graph.getBlockSize(part_u);

      if (part_v_size < part_u_size) {
        split_graph.setNodeLabel(u, part_v);
        split_graph.setBlockSize(part_v, part_v_size + 1);
        split_graph.setBlockSize(part_u, part_u_size - 1);
      } else {
        split_graph.setNodeLabel(v, part_u);
        split_graph.setBlockSize(part_v, part_v_size - 1);
        split_graph.setBlockSize(part_u, part_u_size + 1);
      }
    }
  }
  split_graph.update_block_weights();
}
}  // namespace parhip
