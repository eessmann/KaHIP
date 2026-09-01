/******************************************************************************
 * mpi_tools.cpp
 *
 * Source of KaHIP -- Karlsruhe High Quality Partitioning.
 * Christian Schulz <christian.schulz.phone@gmail.com>
 *****************************************************************************/

#include "communication/mpi_tools.h"

#include <mpi.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "communication/mpi_fixed_broadcast.h"

namespace parhip {
namespace {
using graph_node_record = mpi_tools_detail::complete_graph_node_record;
using graph_edge_record = mpi_tools_detail::complete_graph_edge_record;

static_assert(std::numeric_limits<NodeID>::digits <=
              std::numeric_limits<std::uint64_t>::digits);
static_assert(std::numeric_limits<EdgeID>::digits <=
              std::numeric_limits<std::uint64_t>::digits);
static_assert(std::numeric_limits<NodeWeight>::digits <=
              std::numeric_limits<std::uint64_t>::digits);
static_assert(std::numeric_limits<EdgeWeight>::digits <=
              std::numeric_limits<std::uint64_t>::digits);

[[nodiscard]] auto packed_graph_capacity(parallel_graph_access& graph) noexcept
    -> mpi::capacity_result {
  auto result = mpi::capacity_result{};
  auto const local_nodes = graph.number_of_local_nodes();
  auto const local_edges = graph.number_of_local_edges();
  if (!std::in_range<std::size_t>(local_nodes) ||
      !std::in_range<std::size_t>(local_edges)) {
    return mpi::with_fatal_capacity_issue(
        result, mpi::capacity_issue::storage_byte_size_overflow);
  }
  auto const nodes = static_cast<std::size_t>(local_nodes);
  auto const edges = static_cast<std::size_t>(local_edges);
  if (nodes >
          std::numeric_limits<std::size_t>::max() / sizeof(graph_node_record) ||
      edges >
          std::numeric_limits<std::size_t>::max() / sizeof(graph_edge_record)) {
    return mpi::with_fatal_capacity_issue(
        result, mpi::capacity_issue::storage_byte_size_overflow);
  }
  return result;
}

struct packed_local_graph final {
  std::vector<graph_node_record> nodes;
  std::vector<graph_edge_record> edges;
};

[[nodiscard]] auto pack_local_graph(parallel_graph_access& graph)
    -> packed_local_graph {
  auto const local_nodes = graph.number_of_local_nodes();
  auto const local_edges = graph.number_of_local_edges();
  auto result = packed_local_graph{};
  result.nodes.reserve(static_cast<std::size_t>(local_nodes));
  result.edges.reserve(static_cast<std::size_t>(local_edges));

  for (NodeID node = 0; node < local_nodes; ++node) {
    auto const global = graph.getGlobalID(node);
    auto const degree = graph.getNodeDegree(node);
    result.nodes.push_back(graph_node_record{
        .global_id = static_cast<std::uint64_t>(global),
        .second_partition =
            static_cast<std::uint64_t>(graph.getSecondPartitionIndex(node)),
        .weight = static_cast<std::uint64_t>(graph.getNodeWeight(node)),
        .degree = static_cast<std::uint64_t>(degree),
    });
    auto const first_edge = graph.get_first_edge(node);
    auto const edge_end = graph.get_first_invalid_edge(node);
    for (auto edge = first_edge; edge < edge_end; ++edge) {
      auto const target = graph.getGlobalID(graph.getEdgeTarget(edge));
      result.edges.push_back(graph_edge_record{
          .target_global_id = static_cast<std::uint64_t>(target),
          .weight = static_cast<std::uint64_t>(graph.getEdgeWeight(edge)),
      });
    }
  }
  return result;
}

template <mpi::mpi_datatype Record>
[[nodiscard]] auto make_root_exchange(std::vector<Record> records,
                                      std::size_t communicator_size)
    -> mpi::segmented_buffer<Record> {
  auto counts = std::vector<std::size_t>(communicator_size, 0);
  counts[static_cast<std::size_t>(ROOT)] = records.size();
  auto offsets = std::vector<std::size_t>(communicator_size);
  std::exclusive_scan(counts.begin(), counts.end(), offsets.begin(),
                      std::size_t{0});
  return mpi::segmented_buffer<Record>{std::move(records), std::move(counts),
                                       std::move(offsets)};
}

[[nodiscard]] auto complete_graph_payload_is_valid(
    mpi::segmented_buffer<graph_node_record> const& received_nodes,
    mpi::segmented_buffer<graph_edge_record> const& received_edges,
    NodeID global_nodes,
    EdgeID global_edges,
    std::size_t communicator_size) -> bool {
  if (received_nodes.segment_count() != communicator_size ||
      received_edges.segment_count() != communicator_size) {
    return false;
  }

  auto next_global_node = std::uint64_t{0};
  auto total_received_edges = std::uint64_t{0};
  for (std::size_t source = 0; source < communicator_size; ++source) {
    auto const node_segment = received_nodes.segment(source);
    auto const edge_segment = received_edges.segment(source);
    auto parsed_edges = std::size_t{0};
    for (auto const& node : node_segment) {
      if (node.global_id != next_global_node ||
          !std::in_range<std::size_t>(node.degree)) {
        return false;
      }
      auto const degree = static_cast<std::size_t>(node.degree);
      if (degree > edge_segment.size() - parsed_edges) {
        return false;
      }
      for (std::size_t edge_index = 0; edge_index < degree; ++edge_index) {
        auto const& edge = edge_segment[parsed_edges + edge_index];
        if (edge.target_global_id >= static_cast<std::uint64_t>(global_nodes)) {
          return false;
        }
      }
      parsed_edges += degree;
      ++next_global_node;
    }
    if (parsed_edges != edge_segment.size() ||
        !std::in_range<std::uint64_t>(edge_segment.size())) {
      return false;
    }
    auto const received_edge_count =
        static_cast<std::uint64_t>(edge_segment.size());
    if (total_received_edges >
        std::numeric_limits<std::uint64_t>::max() - received_edge_count) {
      return false;
    }
    total_received_edges += received_edge_count;
  }
  return next_global_node == static_cast<std::uint64_t>(global_nodes) &&
         total_received_edges == static_cast<std::uint64_t>(global_edges);
}

void construct_complete_graph(
    complete_graph_access& complete,
    mpi::segmented_buffer<graph_node_record> const& received_nodes,
    mpi::segmented_buffer<graph_edge_record> const& received_edges,
    NodeID global_nodes,
    EdgeID global_edges) {
  complete.start_construction(global_nodes, global_edges, global_nodes,
                              global_edges, false);
  complete.set_range(0, global_nodes);
  for (std::size_t source = 0; source < received_nodes.segment_count();
       ++source) {
    auto const node_segment = received_nodes.segment(source);
    auto const edge_segment = received_edges.segment(source);
    auto edge_position = std::size_t{0};
    for (auto const& node_record : node_segment) {
      auto const node = complete.new_node();
      complete.setSecondPartitionIndex(
          node, static_cast<NodeID>(node_record.second_partition));
      complete.setNodeWeight(node, static_cast<NodeWeight>(node_record.weight));
      auto const degree = static_cast<std::size_t>(node_record.degree);
      for (std::size_t edge_index = 0; edge_index < degree; ++edge_index) {
        auto const& edge_record = edge_segment[edge_position++];
        auto const edge = complete.new_edge(
            node, static_cast<NodeID>(edge_record.target_global_id));
        complete.setEdgeWeight(edge,
                               static_cast<EdgeWeight>(edge_record.weight));
      }
    }
  }
  complete.finish_construction();
}

enum class distribution_status : std::uint64_t {
  valid,
  incomplete_graph,
  serial_capacity_exceeded,
};

[[nodiscard]] auto root_distribution_status(complete_graph_access& graph)
    -> distribution_status {
  auto const global_nodes = graph.number_of_global_nodes();
  auto const global_edges = graph.number_of_global_edges();
  if (graph.number_of_local_nodes() != global_nodes ||
      graph.number_of_local_edges() != global_edges) {
    return distribution_status::incomplete_graph;
  }
  if (!std::in_range<int>(global_nodes) || !std::in_range<int>(global_edges)) {
    return distribution_status::serial_capacity_exceeded;
  }

  auto expected_edge = EdgeID{0};
  for (NodeID node = 0; node < global_nodes; ++node) {
    auto const first_edge = graph.get_first_edge(node);
    auto const edge_end = graph.get_first_invalid_edge(node);
    if (first_edge != expected_edge || edge_end < first_edge ||
        edge_end > global_edges) {
      return distribution_status::incomplete_graph;
    }
    if (!std::in_range<int>(graph.getNodeWeight(node))) {
      return distribution_status::serial_capacity_exceeded;
    }
    for (auto edge = first_edge; edge < edge_end; ++edge) {
      if (graph.getEdgeTarget(edge) >= global_nodes) {
        return distribution_status::incomplete_graph;
      }
      if (!std::in_range<int>(graph.getEdgeTarget(edge)) ||
          !std::in_range<int>(graph.getEdgeWeight(edge))) {
        return distribution_status::serial_capacity_exceeded;
      }
    }
    expected_edge = edge_end;
  }
  return expected_edge == global_edges ? distribution_status::valid
                                       : distribution_status::incomplete_graph;
}

[[nodiscard]] auto checked_payload_size(std::size_t nodes,
                                        std::size_t edges,
                                        MPI_Comm communicator) noexcept
    -> std::size_t {
  auto const maximum = std::numeric_limits<std::size_t>::max();
  auto total = nodes;
  auto const add = [&](std::size_t count) noexcept {
    if (count > maximum - total) {
      mpi::abort_on_capacity_failure(
          communicator, "complete graph distribution",
          "serial graph payload size exceeds local size_t capacity");
    }
    total += count;
  };
  add(std::size_t{1});
  add(edges);
  add(nodes);
  add(edges);
  return total;
}
}  // namespace

void mpi_tools::collect_parallel_graph_to_local_graph(
    MPI_Comm communicator,
    PPartitionConfig&,
    parallel_graph_access& distributed,
    complete_graph_access& complete) {
  auto const communicator_view = mpi::communicator_view{communicator};
  mpi::require_live_intracommunicator(
      communicator_view,
      "complete graph collection requires a live intracommunicator");
  auto const rank = communicator_view.rank();
  auto const communicator_size =
      static_cast<std::size_t>(communicator_view.size());

  static_cast<void>(mpi::resolve_capacity_collectively(
      packed_graph_capacity(distributed), communicator, communicator,
      "complete graph collection"));

  auto global_nodes = std::uint64_t{};
  auto global_edges = std::uint64_t{};
  try {
    global_nodes = mpi::agree_collectively(
        static_cast<std::uint64_t>(distributed.number_of_global_nodes()),
        communicator_view,
        "complete graph global node count differs across ranks");
    global_edges = mpi::agree_collectively(
        static_cast<std::uint64_t>(distributed.number_of_global_edges()),
        communicator_view,
        "complete graph global edge count differs across ranks");
  } catch (...) {
    mpi::abort_on_exception(communicator,
                            "complete graph collection metadata failure");
  }

  auto received_nodes =
      std::optional<mpi::segmented_buffer<graph_node_record>>{};
  auto received_edges =
      std::optional<mpi::segmented_buffer<graph_edge_record>>{};
  try {
    auto records = pack_local_graph(distributed);
    received_nodes.emplace(mpi::all_to_all_v(
        make_root_exchange(std::move(records.nodes), communicator_size),
        communicator_view));
    received_edges.emplace(mpi::all_to_all_v(
        make_root_exchange(std::move(records.edges), communicator_size),
        communicator_view));
  } catch (...) {
    mpi::abort_on_exception(communicator,
                            "complete graph collection local failure");
  }

  auto payload_is_valid = rank != static_cast<int>(ROOT);
  if (rank == static_cast<int>(ROOT)) {
    try {
      payload_is_valid = complete_graph_payload_is_valid(
          *received_nodes, *received_edges, static_cast<NodeID>(global_nodes),
          static_cast<EdgeID>(global_edges), communicator_size);
    } catch (...) {
      mpi::abort_on_exception(
          communicator, "complete graph collection payload inspection failed");
    }
  }
  try {
    mpi::validate_collectively(payload_is_valid, communicator_view,
                               "complete graph collection payload is invalid");
  } catch (...) {
    mpi::abort_on_exception(communicator,
                            "complete graph collection validation failure");
  }

  if (rank == static_cast<int>(ROOT)) {
    try {
      construct_complete_graph(complete, *received_nodes, *received_edges,
                               static_cast<NodeID>(global_nodes),
                               static_cast<EdgeID>(global_edges));
    } catch (...) {
      mpi::abort_on_exception(communicator,
                              "complete graph collection construction failed");
    }
  }
}

void mpi_tools::distribute_local_graph(MPI_Comm communicator,
                                       PPartitionConfig&,
                                       complete_graph_access& graph) {
  auto owned_communicator =
      mpi::communicator{mpi::communicator_view{communicator}};
  auto const collective = owned_communicator.view();
  auto const rank = collective.rank();

  auto header = std::array<std::uint64_t, 3>{};
  if (rank == static_cast<int>(ROOT)) {
    try {
      header = {
          static_cast<std::uint64_t>(graph.number_of_global_nodes()),
          static_cast<std::uint64_t>(graph.number_of_global_edges()),
          static_cast<std::uint64_t>(root_distribution_status(graph)),
      };
    } catch (...) {
      mpi::abort_on_exception(
          collective.native_handle(),
          "complete graph distribution root inspection failed");
    }
  }
  mpi::broadcast_fixed(std::span{header}, ROOT, collective,
                       "MPI_Bcast(complete graph header)");

  auto const status = static_cast<distribution_status>(header[2]);
  if (status == distribution_status::incomplete_graph) {
    mpi::abort_on_programming_error(
        collective.native_handle(),
        "complete graph distribution requires a complete valid root graph");
  }
  if (status != distribution_status::valid) {
    mpi::abort_on_capacity_failure(
        collective.native_handle(), "complete graph distribution",
        "serial graph representation exceeds int capacity");
  }
  if (!std::in_range<std::size_t>(header[0]) ||
      !std::in_range<std::size_t>(header[1])) {
    mpi::abort_on_capacity_failure(collective.native_handle(),
                                   "complete graph distribution",
                                   "graph counts exceed local size_t capacity");
  }

  auto const nodes = static_cast<std::size_t>(header[0]);
  auto const edges = static_cast<std::size_t>(header[1]);
  auto const xadj_offset = std::size_t{0};
  auto const adjncy_offset = nodes + std::size_t{1};
  auto const node_weight_offset = adjncy_offset + edges;
  auto const edge_weight_offset = node_weight_offset + nodes;
  auto const payload_size =
      checked_payload_size(nodes, edges, collective.native_handle());

  auto payload = std::vector<int>{};
  try {
    payload.resize(payload_size);
  } catch (...) {
    mpi::abort_on_exception(collective.native_handle(),
                            "complete graph distribution allocation failure");
  }

  if (rank == static_cast<int>(ROOT)) {
    for (std::size_t node = 0; node < nodes; ++node) {
      auto const node_id = static_cast<NodeID>(node);
      payload[xadj_offset + node] =
          static_cast<int>(graph.get_first_edge(node_id));
      payload[node_weight_offset + node] =
          static_cast<int>(graph.getNodeWeight(node_id));
    }
    payload[xadj_offset + nodes] = static_cast<int>(edges);
    for (std::size_t edge = 0; edge < edges; ++edge) {
      auto const edge_id = static_cast<EdgeID>(edge);
      payload[adjncy_offset + edge] =
          static_cast<int>(graph.getEdgeTarget(edge_id));
      payload[edge_weight_offset + edge] =
          static_cast<int>(graph.getEdgeWeight(edge_id));
    }
  }

  mpi::broadcast_bounded(std::span{payload}, ROOT, collective,
                         "MPI_Bcast(complete graph payload)");

  if (rank != static_cast<int>(ROOT)) {
    graph.build_from_metis_weighted(
        static_cast<int>(nodes), payload.data() + xadj_offset,
        payload.data() + adjncy_offset, payload.data() + node_weight_offset,
        payload.data() + edge_weight_offset);
  }
}
}  // namespace parhip
