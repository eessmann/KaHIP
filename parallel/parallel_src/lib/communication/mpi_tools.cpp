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
#include <cstdlib>
#include <limits>
#include <numeric>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "communication/mpi_fixed_broadcast.h"
#include "tools/fatal_diagnostics.h"

namespace parhip {
namespace {
using graph_node_record = mpi_tools_detail::complete_graph_node_record;
using graph_edge_record = mpi_tools_detail::complete_graph_edge_record;
using serial_kernel_profile = kahip::serial_kernel::serial_kernel_profile;
using serial_profile_input = kahip::serial_kernel::profile_input;
using serial_profile_limits = kahip::serial_kernel::profile_limits;
using serial_profile_reason = kahip::serial_kernel::profile_reason;

constexpr auto serial_profile_fields = std::size_t{12};

struct local_serial_observation final {
  std::uint64_t local_nodes{};
  std::uint64_t local_edges{};
  std::uint64_t total_node_weight{};
  std::uint64_t maximum_node_weight{};
  std::uint64_t total_edge_weight{};
  std::uint64_t maximum_edge_weight{};
  std::uint64_t reported_global_nodes{};
  std::uint64_t reported_global_edges{};
  std::uint64_t block_count{};
  std::uint64_t absolute_bound{};
  std::uint64_t bank_factor_twice{};
  std::uint64_t flags{};
};

constexpr auto csr_offsets_are_valid = std::uint64_t{1} << 0;
constexpr auto targets_are_valid = std::uint64_t{1} << 1;
constexpr auto labels_are_valid = std::uint64_t{1} << 2;
constexpr auto sums_are_valid = std::uint64_t{1} << 3;
constexpr auto social_mode = std::uint64_t{1} << 4;
constexpr auto all_local_observation_flags = csr_offsets_are_valid |
                                            targets_are_valid |
                                            labels_are_valid |
                                            sums_are_valid;

[[nodiscard]] auto native_serial_profile_limits() -> serial_profile_limits {
  auto result = serial_profile_limits::native();
  result.int_elements = std::vector<int>{}.max_size();
  result.wire_node_elements = std::vector<graph_node_record>{}.max_size();
  result.wire_edge_elements = std::vector<graph_edge_record>{}.max_size();
  result.complete_node_elements = std::vector<Node>{}.max_size();
  result.complete_node_data_elements = std::vector<NodeData>{}.max_size();
  result.complete_edge_elements = std::vector<Edge>{}.max_size();
  result.wire_node_bytes = sizeof(graph_node_record);
  result.wire_edge_bytes = sizeof(graph_edge_record);
  result.complete_node_bytes = sizeof(Node);
  result.complete_node_data_bytes = sizeof(NodeData);
  result.complete_edge_bytes = sizeof(Edge);
  return result;
}

[[nodiscard]] auto observe_serial_kernel(parallel_graph_access& graph,
                                         PPartitionConfig const& config)
    -> local_serial_observation {
  auto result = local_serial_observation{
      .local_nodes = static_cast<std::uint64_t>(graph.number_of_local_nodes()),
      .local_edges = static_cast<std::uint64_t>(graph.number_of_local_edges()),
      .reported_global_nodes =
          static_cast<std::uint64_t>(graph.number_of_global_nodes()),
      .reported_global_edges =
          static_cast<std::uint64_t>(graph.number_of_global_edges()),
      .block_count = static_cast<std::uint64_t>(config.k),
      .absolute_bound = static_cast<std::uint64_t>(config.upper_bound_partition),
      .bank_factor_twice =
          (config.initial_partitioning_algorithm ==
               InitialPartitioningAlgorithm::KAFFPAESTRONG ||
           config.initial_partitioning_algorithm ==
               InitialPartitioningAlgorithm::KAFFPAESTRONGSNW)
              ? std::uint64_t{6}
              : (config.initial_partitioning_algorithm ==
                     InitialPartitioningAlgorithm::KAFFPAEFAST
                     ? std::uint64_t{2}
                     : std::uint64_t{3}),
      .flags = all_local_observation_flags,
  };
  if (config.initial_partitioning_algorithm ==
          InitialPartitioningAlgorithm::KAFFPAEULTRAFASTSNW ||
      config.initial_partitioning_algorithm ==
          InitialPartitioningAlgorithm::KAFFPAEFASTSNW ||
      config.initial_partitioning_algorithm ==
          InitialPartitioningAlgorithm::KAFFPAEECOSNW ||
      config.initial_partitioning_algorithm ==
          InitialPartitioningAlgorithm::KAFFPAESTRONGSNW) {
    result.flags |= social_mode;
  }
  auto expected_edge = EdgeID{0};
  for (NodeID node = 0; node < graph.number_of_local_nodes(); ++node) {
    auto const first = graph.get_first_edge(node);
    auto const end = graph.get_first_invalid_edge(node);
    if (first != expected_edge || end < first ||
        end > graph.number_of_local_edges() ||
        end > static_cast<EdgeID>(std::numeric_limits<int>::max())) {
      result.flags &= ~csr_offsets_are_valid;
    }
    expected_edge = end;
    auto const node_weight = graph.getNodeWeight(node);
    result.maximum_node_weight = std::max(
        result.maximum_node_weight, static_cast<std::uint64_t>(node_weight));
    if (!kahip::serial_kernel::checked_add(
            result.total_node_weight, static_cast<std::uint64_t>(node_weight),
            std::numeric_limits<std::uint64_t>::max(), result.total_node_weight)) {
      result.flags &= ~sums_are_valid;
    }
    if (config.vcycle &&
        (config.k == 0 || graph.getSecondPartitionIndex(node) >= config.k)) {
      result.flags &= ~labels_are_valid;
    }
    for (EdgeID edge = first; edge < end && edge < graph.number_of_local_edges();
         ++edge) {
      auto const target = graph.getGlobalID(graph.getEdgeTarget(edge));
      if (target > static_cast<NodeID>(std::numeric_limits<int>::max())) {
        result.flags &= ~targets_are_valid;
      }
      auto const edge_weight = graph.getEdgeWeight(edge);
      result.maximum_edge_weight = std::max(
          result.maximum_edge_weight, static_cast<std::uint64_t>(edge_weight));
      if (!kahip::serial_kernel::checked_add(
              result.total_edge_weight,
              static_cast<std::uint64_t>(edge_weight),
              std::numeric_limits<std::uint64_t>::max(),
              result.total_edge_weight)) {
        result.flags &= ~sums_are_valid;
      }
    }
  }
  if (expected_edge != graph.number_of_local_edges()) {
    result.flags &= ~csr_offsets_are_valid;
  }
  return result;
}

[[nodiscard]] auto serial_profile_from_observations(
    std::span<local_serial_observation const> observations)
    -> serial_kernel_profile {
  auto input = serial_profile_input{};
  auto node_sum_ok = true;
  auto edge_sum_ok = true;
  auto node_weight_sum_ok = true;
  auto edge_weight_sum_ok = true;
  auto common_metadata = !observations.empty();
  auto const first = common_metadata ? observations.front()
                                     : local_serial_observation{};
  input.social_mode = (first.flags & social_mode) != 0;
  input.bank_factor_twice = first.bank_factor_twice;
  for (auto const& observation : observations) {
    node_sum_ok = node_sum_ok && kahip::serial_kernel::checked_add(
        input.global_nodes, observation.local_nodes,
        std::numeric_limits<std::uint64_t>::max(), input.global_nodes);
    edge_sum_ok = edge_sum_ok && kahip::serial_kernel::checked_add(
        input.global_directed_edges, observation.local_edges,
        std::numeric_limits<std::uint64_t>::max(), input.global_directed_edges);
    node_weight_sum_ok =
        node_weight_sum_ok && kahip::serial_kernel::checked_add(
                                  input.total_node_weight,
                                  observation.total_node_weight,
                                  std::numeric_limits<std::uint64_t>::max(),
                                  input.total_node_weight);
    edge_weight_sum_ok =
        edge_weight_sum_ok && kahip::serial_kernel::checked_add(
                                  input.total_directed_edge_weight,
                                  observation.total_edge_weight,
                                  std::numeric_limits<std::uint64_t>::max(),
                                  input.total_directed_edge_weight);
    input.maximum_node_weight =
        std::max(input.maximum_node_weight, observation.maximum_node_weight);
    input.maximum_directed_edge_weight = std::max(
        input.maximum_directed_edge_weight, observation.maximum_edge_weight);
    input.csr_offsets_are_valid =
        input.csr_offsets_are_valid &&
        (observation.flags & csr_offsets_are_valid) != 0;
    input.targets_are_valid = input.targets_are_valid &&
                              (observation.flags & targets_are_valid) != 0;
    input.labels_are_valid = input.labels_are_valid &&
                             (observation.flags & labels_are_valid) != 0;
    common_metadata = common_metadata &&
                      observation.reported_global_nodes == first.reported_global_nodes &&
                      observation.reported_global_edges == first.reported_global_edges &&
                      observation.block_count == first.block_count &&
                      observation.absolute_bound == first.absolute_bound &&
                      observation.bank_factor_twice == first.bank_factor_twice &&
                      (observation.flags & social_mode) ==
                          (first.flags & social_mode);
    node_weight_sum_ok = node_weight_sum_ok &&
                         (observation.flags & sums_are_valid) != 0;
    edge_weight_sum_ok = edge_weight_sum_ok &&
                         (observation.flags & sums_are_valid) != 0;
  }
  input.block_count = first.block_count;
  input.absolute_bound = first.absolute_bound;
  auto profile = kahip::serial_kernel::make_profile(input,
                                                     native_serial_profile_limits());
  if (!node_sum_ok || !edge_sum_ok || !node_weight_sum_ok || !edge_weight_sum_ok) {
    profile.reason = serial_profile_reason::collective_aggregate_overflow;
  } else if (!common_metadata) {
    profile.reason = serial_profile_reason::collective_configuration_mismatch;
  } else if (input.global_nodes != first.reported_global_nodes) {
    profile.reason = serial_profile_reason::global_node_count_mismatch;
  } else if (input.global_directed_edges != first.reported_global_edges) {
    profile.reason = serial_profile_reason::global_directed_edge_count_mismatch;
  }
  return profile;
}

[[noreturn]] void abort_unsafe_serial_profile(
    MPI_Comm communicator,
    int rank,
    serial_kernel_profile const& profile) noexcept {
  if (rank == static_cast<int>(ROOT)) {
    kahip::diagnostics::critical(
        "ParHIP serial kernel profile failure: reason=",
        kahip::serial_kernel::reason_name(profile.reason),
        ", global nodes=", profile.global_nodes,
        ", global directed edges=", profile.global_directed_edges,
        ", total node weight=", profile.total_node_weight,
        ", maximum node weight=", profile.maximum_node_weight,
        ", total directed edge weight=", profile.total_directed_edge_weight,
        ", maximum directed edge weight=", profile.maximum_directed_edge_weight,
        ", block count=", profile.block_count,
        ", absolute bound=", profile.absolute_bound,
        ", wire record bytes=", profile.wire_record_bytes,
        ", CSR bytes=", profile.csr_bytes,
        ", partition bytes=", profile.partition_bytes,
        ", serial input bytes=", profile.serial_input_bytes,
        ", complete graph bytes=", profile.complete_graph_bytes,
        ", base memory bytes=", profile.base_memory_bytes);
  }
  static_cast<void>(MPI_Abort(communicator, EXIT_FAILURE));
  std::abort();
}

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

auto mpi_tools::preflight_serial_kernel(
    MPI_Comm communicator,
    PPartitionConfig const& config,
    parallel_graph_access& graph) const -> serial_kernel_profile {
  auto const collective = mpi::communicator_view{communicator};
  mpi::require_live_intracommunicator(
      collective, "serial kernel profile requires a live intracommunicator");
  auto const local = observe_serial_kernel(graph, config);
  auto const rank = collective.rank();
  auto const size = static_cast<std::size_t>(collective.size());
  auto received = std::vector<local_serial_observation>(size);
  static_assert(sizeof(local_serial_observation) ==
                serial_profile_fields * sizeof(std::uint64_t));
  mpi::check_or_abort(
      MPI_Allgather(&local, static_cast<int>(serial_profile_fields), MPI_UINT64_T,
                    received.data(), static_cast<int>(serial_profile_fields),
                    MPI_UINT64_T, communicator),
      communicator, "MPI_Allgather(serial kernel profile)");

  auto profile = serial_kernel_profile{};
  if (rank == static_cast<int>(ROOT)) {
    profile = serial_profile_from_observations(received);
  }
  auto packed = std::array<std::uint64_t, 15>{
      profile.global_nodes,
      profile.global_directed_edges,
      profile.total_node_weight,
      profile.maximum_node_weight,
      profile.total_directed_edge_weight,
      profile.maximum_directed_edge_weight,
      profile.block_count,
      profile.absolute_bound,
      profile.wire_record_bytes,
      profile.csr_bytes,
      profile.partition_bytes,
      profile.serial_input_bytes,
      profile.complete_graph_bytes,
      profile.base_memory_bytes,
      static_cast<std::uint64_t>(profile.reason),
  };
  mpi::check_or_abort(
      MPI_Bcast(packed.data(), static_cast<int>(packed.size()), MPI_UINT64_T,
                ROOT, communicator),
      communicator, "MPI_Bcast(serial kernel profile)");
  profile = serial_kernel_profile{
      .global_nodes = packed[0],
      .global_directed_edges = packed[1],
      .total_node_weight = packed[2],
      .maximum_node_weight = packed[3],
      .total_directed_edge_weight = packed[4],
      .maximum_directed_edge_weight = packed[5],
      .block_count = packed[6],
      .absolute_bound = packed[7],
      .wire_record_bytes = packed[8],
      .csr_bytes = packed[9],
      .partition_bytes = packed[10],
      .serial_input_bytes = packed[11],
      .complete_graph_bytes = packed[12],
      .base_memory_bytes = packed[13],
      .reason = static_cast<serial_profile_reason>(packed[14]),
  };
  if (!profile.safe()) {
    abort_unsafe_serial_profile(communicator, rank, profile);
  }
  return profile;
}

void mpi_tools::collect_parallel_graph_to_checked_serial_graph(
    MPI_Comm communicator,
    PPartitionConfig const& config,
    parallel_graph_access& distributed,
    complete_graph_access& complete) {
  static_cast<void>(preflight_serial_kernel(communicator, config, distributed));
  collect_parallel_graph_to_local_graph(communicator, config, distributed,
                                        complete);
}

void mpi_tools::collect_parallel_graph_to_local_graph(
    MPI_Comm communicator,
    PPartitionConfig const&,
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
