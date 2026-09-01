#include <mpi.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>
#include <string_view>
#include <vector>

#include "data_structure/parallel_graph_access.h"
#include "definitions.h"
#include "partition_config.h"
#include "tools/distributed_quality_metrics.h"

// KAHIP_PMPI_CALLBACK_REGION_BEGIN
namespace quality_metrics_failure_probe {
enum class mode : unsigned char {
  backend,
  zero_k,
  invalid_label,
  invalid_second_label,
  invalid_load_label,
  mismatched_k,
  local_overflow,
  global_overflow,
  load_overflow,
  edge_cut_local_overflow,
  edge_cut_second_local_overflow,
  local_edge_cut_overflow,
  edge_cut_global_overflow,
  edge_cut_second_global_overflow,
  communication_volume_total_overflow,
  distribution_volume_global_overflow,
  null_partition_map,
};

inline bool active = false;
inline mode selected = mode::backend;
inline MPI_Comm expected_communicator = MPI_COMM_NULL;
inline int validations = 0;
inline int payload_attempts = 0;
inline int finalizations = 0;
inline bool callback_error = false;

void write_text(std::string_view text) noexcept {
  static_cast<void>(::write(STDERR_FILENO, text.data(), text.size()));
}

[[nodiscard]] auto valid_control(void const* send_buffer,
                                 void* receive_buffer,
                                 MPI_Count count,
                                 MPI_Datatype datatype,
                                 MPI_Op reduction,
                                 MPI_Comm communicator) noexcept -> bool {
  auto const domain_signature = count == 1 && datatype == MPI_UINT64_T &&
                                (reduction == MPI_MIN || reduction == MPI_MAX);
  auto const label_validation =
      count == 1 && datatype == MPI_INT && reduction == MPI_MIN;
  auto const reduction_signature =
      count == 7 && datatype == MPI_UINT64_T &&
      (reduction == MPI_MIN || reduction == MPI_MAX);
  return send_buffer != nullptr && receive_buffer != nullptr &&
         send_buffer != receive_buffer &&
         (domain_signature || label_validation || reduction_signature) &&
         communicator == expected_communicator;
}

[[nodiscard]] auto valid_payload(void const* send_buffer,
                                 void* receive_buffer,
                                 MPI_Count count,
                                 MPI_Datatype datatype,
                                 MPI_Op reduction,
                                 MPI_Comm communicator) noexcept -> bool {
  auto const scalar_payload =
      selected == mode::edge_cut_global_overflow ||
      selected == mode::edge_cut_second_global_overflow ||
      selected == mode::distribution_volume_global_overflow;
  auto const expected_count = scalar_payload ? MPI_Count{1} : MPI_Count{2};
  return send_buffer != nullptr && receive_buffer != nullptr &&
         send_buffer != receive_buffer && count == expected_count &&
         datatype == MPI_UNSIGNED_LONG_LONG && reduction == MPI_SUM &&
         communicator == expected_communicator;
}

void inject_capacity_boundary(void* receive_buffer) noexcept {
  if (selected == mode::communication_volume_total_overflow &&
      payload_attempts == 2) {
    auto* values = static_cast<unsigned long long*>(receive_buffer);
    values[0] = std::numeric_limits<unsigned long long>::max();
    values[1] = 1;
  } else if (selected == mode::distribution_volume_global_overflow &&
             payload_attempts == 1) {
    auto* values = static_cast<unsigned long long*>(receive_buffer);
    values[0] = std::numeric_limits<unsigned long long>::max();
  }
}

[[nodiscard]] auto expected_abort_state() noexcept -> bool {
  if (callback_error || finalizations != 0) {
    return false;
  }
  switch (selected) {
    case mode::backend:
      return validations == 6 && payload_attempts == 1;
    case mode::zero_k:
    case mode::mismatched_k:
      return validations == 2 && payload_attempts == 0;
    case mode::invalid_label:
    case mode::invalid_second_label:
    case mode::invalid_load_label:
      return validations == 4 && payload_attempts == 0;
    case mode::local_overflow:
    case mode::load_overflow:
      return validations == 3 && payload_attempts == 0;
    case mode::global_overflow:
      return validations == 8 && payload_attempts == 2;
    case mode::edge_cut_local_overflow:
    case mode::edge_cut_second_local_overflow:
      return validations == 1 && payload_attempts == 0;
    case mode::local_edge_cut_overflow:
    case mode::null_partition_map:
      return validations == 0 && payload_attempts == 0;
    case mode::edge_cut_global_overflow:
    case mode::edge_cut_second_global_overflow:
      return validations == 5 && payload_attempts == 2;
    case mode::communication_volume_total_overflow:
      return validations == 8 && payload_attempts == 2;
    case mode::distribution_volume_global_overflow:
      return validations == 6 && payload_attempts == 2;
  }
  return false;
}

[[noreturn]] void observed_abort(MPI_Comm communicator,
                                 int error_code) noexcept {
  int relation = MPI_UNEQUAL;
  if (error_code != EXIT_FAILURE ||
      PMPI_Comm_compare(communicator, expected_communicator, &relation) !=
          MPI_SUCCESS ||
      relation != MPI_IDENT || !expected_abort_state()) {
    write_text("observed quality-metrics MPI_Abort with unexpected state\n");
    std::_Exit(91);
  }
  write_text("observed quality-metrics MPI_Abort on affected communicator\n");
  if (PMPI_Barrier(MPI_COMM_WORLD) != MPI_SUCCESS) {
    std::_Exit(90);
  }
  std::_Exit(86);
}
}  // namespace quality_metrics_failure_probe

static_assert(noexcept(quality_metrics_failure_probe::write_text({})));
static_assert(
    noexcept(quality_metrics_failure_probe::valid_control(nullptr,
                                                          nullptr,
                                                          0,
                                                          MPI_DATATYPE_NULL,
                                                          MPI_OP_NULL,
                                                          MPI_COMM_NULL)));
static_assert(
    noexcept(quality_metrics_failure_probe::valid_payload(nullptr,
                                                          nullptr,
                                                          0,
                                                          MPI_DATATYPE_NULL,
                                                          MPI_OP_NULL,
                                                          MPI_COMM_NULL)));
static_assert(
    noexcept(quality_metrics_failure_probe::inject_capacity_boundary(nullptr)));
static_assert(noexcept(quality_metrics_failure_probe::expected_abort_state()));
static_assert(
    noexcept(quality_metrics_failure_probe::observed_abort(MPI_COMM_NULL, 0)));

extern "C" int MPI_Allreduce(void const* send_buffer,
                             void* receive_buffer,
                             int count,
                             MPI_Datatype datatype,
                             MPI_Op reduction,
                             MPI_Comm communicator) {
  using namespace quality_metrics_failure_probe;
  if (!active) {
    return PMPI_Allreduce(send_buffer, receive_buffer, count, datatype,
                          reduction, communicator);
  }
  auto const is_payload = reduction == MPI_SUM &&
                          datatype == MPI_UNSIGNED_LONG_LONG &&
                          valid_payload(send_buffer, receive_buffer, count,
                                        datatype, reduction, communicator);
  if (!is_payload) {
    ++validations;
    if (!valid_control(send_buffer, receive_buffer, count, datatype, reduction,
                       communicator)) {
      callback_error = true;
      return MPI_ERR_OTHER;
    }
    return PMPI_Allreduce(send_buffer, receive_buffer, count, datatype,
                          reduction, communicator);
  }
  ++payload_attempts;
  if (!valid_payload(send_buffer, receive_buffer, count, datatype, reduction,
                     communicator)) {
    callback_error = true;
  }
  if (selected == mode::backend) {
    return MPI_ERR_OTHER;
  }
  auto const result = PMPI_Allreduce(send_buffer, receive_buffer, count,
                                     datatype, reduction, communicator);
  if (result == MPI_SUCCESS) {
    inject_capacity_boundary(receive_buffer);
  }
  return result;
}

#if KAHIP_HAVE_MPI_ALLREDUCE_C
extern "C" int MPI_Allreduce_c(void const* send_buffer,
                               void* receive_buffer,
                               MPI_Count count,
                               MPI_Datatype datatype,
                               MPI_Op reduction,
                               MPI_Comm communicator) {
  using namespace quality_metrics_failure_probe;
  if (!active) {
    return PMPI_Allreduce_c(send_buffer, receive_buffer, count, datatype,
                            reduction, communicator);
  }
  ++payload_attempts;
  if (!valid_payload(send_buffer, receive_buffer, count, datatype, reduction,
                     communicator)) {
    callback_error = true;
  }
  if (selected == mode::backend) {
    return MPI_ERR_OTHER;
  }
  auto const result = PMPI_Allreduce_c(send_buffer, receive_buffer, count,
                                       datatype, reduction, communicator);
  if (result == MPI_SUCCESS) {
    inject_capacity_boundary(receive_buffer);
  }
  return result;
}
#endif

extern "C" int MPI_Finalize() {
  if (quality_metrics_failure_probe::active) {
    ++quality_metrics_failure_probe::finalizations;
    return MPI_SUCCESS;
  }
  return PMPI_Finalize();
}

extern "C" int MPI_Abort(MPI_Comm communicator, int error_code) {
  quality_metrics_failure_probe::observed_abort(communicator, error_code);
}
// KAHIP_PMPI_CALLBACK_REGION_END

namespace {
[[nodiscard]] auto parse_mode(std::string_view value)
    -> quality_metrics_failure_probe::mode {
  using mode = quality_metrics_failure_probe::mode;
  if (value == "backend") {
    return mode::backend;
  }
  if (value == "zero-k") {
    return mode::zero_k;
  }
  if (value == "invalid-label") {
    return mode::invalid_label;
  }
  if (value == "invalid-second-label") {
    return mode::invalid_second_label;
  }
  if (value == "invalid-load-label") {
    return mode::invalid_load_label;
  }
  if (value == "mismatched-k") {
    return mode::mismatched_k;
  }
  if (value == "local-overflow") {
    return mode::local_overflow;
  }
  if (value == "global-overflow") {
    return mode::global_overflow;
  }
  if (value == "load-overflow") {
    return mode::load_overflow;
  }
  if (value == "edge-cut-local-overflow") {
    return mode::edge_cut_local_overflow;
  }
  if (value == "edge-cut-second-local-overflow") {
    return mode::edge_cut_second_local_overflow;
  }
  if (value == "local-edge-cut-overflow") {
    return mode::local_edge_cut_overflow;
  }
  if (value == "edge-cut-global-overflow") {
    return mode::edge_cut_global_overflow;
  }
  if (value == "edge-cut-second-global-overflow") {
    return mode::edge_cut_second_global_overflow;
  }
  if (value == "communication-volume-total-overflow") {
    return mode::communication_volume_total_overflow;
  }
  if (value == "distribution-volume-global-overflow") {
    return mode::distribution_volume_global_overflow;
  }
  if (value == "null-partition-map") {
    return mode::null_partition_map;
  }
  quality_metrics_failure_probe::write_text(
      "unknown quality-metrics probe mode\n");
  std::_Exit(2);
}

void build_fixture(parhip::parallel_graph_access& graph,
                   int rank,
                   int size,
                   quality_metrics_failure_probe::mode selected) {
  using mode = quality_metrics_failure_probe::mode;
  auto const all_ranks_active = selected == mode::null_partition_map;
  auto const all_ranks_have_edge_work =
      selected == mode::local_edge_cut_overflow;
  auto const local_nodes = all_ranks_active           ? parhip::NodeID{1}
                           : all_ranks_have_edge_work ? parhip::NodeID{2}
                           : rank == 0                ? parhip::NodeID{0}
                                                      : parhip::NodeID{2};
  auto const global_nodes = all_ranks_active ? static_cast<parhip::NodeID>(size)
                            : all_ranks_have_edge_work
                                ? static_cast<parhip::NodeID>(2 * size)
                                : static_cast<parhip::NodeID>(2 * (size - 1));
  auto const needs_edges =
      selected == mode::load_overflow ||
      selected == mode::edge_cut_local_overflow ||
      selected == mode::edge_cut_second_local_overflow ||
      selected == mode::local_edge_cut_overflow ||
      selected == mode::edge_cut_global_overflow ||
      selected == mode::edge_cut_second_global_overflow ||
      selected == mode::communication_volume_total_overflow ||
      selected == mode::distribution_volume_global_overflow;
  auto const local_edges =
      needs_edges && (rank != 0 || all_ranks_have_edge_work)
          ? parhip::EdgeID{2}
          : parhip::EdgeID{0};
  auto const global_edges =
      all_ranks_have_edge_work ? static_cast<parhip::EdgeID>(2 * size)
      : needs_edges            ? static_cast<parhip::EdgeID>(2 * (size - 1))
                               : parhip::EdgeID{0};
  graph.start_construction(local_nodes, local_edges, global_nodes, global_edges,
                           false);

  auto ranges = std::vector<parhip::NodeID>(static_cast<std::size_t>(size) + 1);
  for (int index = 0; index <= size; ++index) {
    ranges[static_cast<std::size_t>(index)] =
        all_ranks_active ? static_cast<parhip::NodeID>(index)
        : all_ranks_have_edge_work
            ? static_cast<parhip::NodeID>(2 * index)
            : static_cast<parhip::NodeID>(2 * std::max(0, index - 1));
  }
  auto const from = ranges[static_cast<std::size_t>(rank)];
  auto const to =
      local_nodes == 0 ? from : ranges[static_cast<std::size_t>(rank) + 1] - 1;
  graph.set_range(from, to);
  graph.set_range_array(ranges);
  if (all_ranks_active) {
    auto const node = graph.new_node();
    graph.setNodeWeight(node, 1);
    graph.setNodeLabel(node, 0);
    graph.setSecondPartitionIndex(node, 0);
  } else if (rank != 0 || all_ranks_have_edge_work) {
    auto const first = graph.new_node();
    graph.setNodeWeight(first, 1);
    graph.setNodeLabel(first, 0);
    graph.setSecondPartitionIndex(first, 0);
    if (needs_edges) {
      auto const edge = graph.new_edge(first, from + 1);
      if (selected == mode::edge_cut_local_overflow ||
          selected == mode::edge_cut_second_local_overflow ||
          selected == mode::local_edge_cut_overflow) {
        graph.setEdgeWeight(edge,
                            std::numeric_limits<parhip::EdgeWeight>::max());
      } else if (selected == mode::edge_cut_global_overflow ||
                 selected == mode::edge_cut_second_global_overflow) {
        graph.setEdgeWeight(
            edge, std::numeric_limits<parhip::EdgeWeight>::max() / 2 + 1);
      } else {
        graph.setEdgeWeight(edge, 1);
      }
    }
    auto const second = graph.new_node();
    graph.setNodeWeight(second, 2);
    graph.setNodeLabel(second, 1);
    graph.setSecondPartitionIndex(second, 1);
    if (needs_edges) {
      auto const edge = graph.new_edge(second, from);
      graph.setEdgeWeight(
          edge, selected == mode::edge_cut_local_overflow ||
                        selected == mode::edge_cut_second_local_overflow ||
                        selected == mode::local_edge_cut_overflow
                    ? parhip::EdgeWeight{1}
                    : parhip::EdgeWeight{0});
    }
  }
  graph.finish_construction();
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 2 || MPI_Init(&argc, &argv) != MPI_SUCCESS) {
    return 2;
  }
  auto world_rank = -1;
  auto world_size = 0;
  if (MPI_Comm_rank(MPI_COMM_WORLD, &world_rank) != MPI_SUCCESS ||
      MPI_Comm_size(MPI_COMM_WORLD, &world_size) != MPI_SUCCESS ||
      world_size != 3) {
    return 3;
  }

  auto communicator = MPI_COMM_NULL;
  if (MPI_Comm_split(MPI_COMM_WORLD, 0, world_size - world_rank,
                     &communicator) != MPI_SUCCESS ||
      communicator == MPI_COMM_NULL ||
      MPI_Comm_set_errhandler(communicator, MPI_ERRORS_RETURN) != MPI_SUCCESS) {
    return 4;
  }
  auto rank = -1;
  auto size = 0;
  if (MPI_Comm_rank(communicator, &rank) != MPI_SUCCESS ||
      MPI_Comm_size(communicator, &size) != MPI_SUCCESS) {
    return 5;
  }

  auto const selected = parse_mode(argv[1]);
  auto graph = parhip::parallel_graph_access{communicator};
  build_fixture(graph, rank, size, selected);
  if ((selected == quality_metrics_failure_probe::mode::invalid_label ||
       selected == quality_metrics_failure_probe::mode::invalid_load_label) &&
      rank != 0) {
    graph.setNodeLabel(1, 2);
  }
  if (selected == quality_metrics_failure_probe::mode::invalid_second_label &&
      rank != 0) {
    graph.setSecondPartitionIndex(1, 2);
  }
  if (selected == quality_metrics_failure_probe::mode::local_overflow &&
      rank != 0) {
    graph.setNodeWeight(0, std::numeric_limits<parhip::NodeWeight>::max());
    graph.setNodeWeight(1, 1);
  }
  if (selected == quality_metrics_failure_probe::mode::global_overflow &&
      rank != 0) {
    graph.setNodeWeight(0,
                        std::numeric_limits<parhip::NodeWeight>::max() / 2 + 1);
    graph.setNodeWeight(1, 0);
  }
  if (selected == quality_metrics_failure_probe::mode::load_overflow &&
      rank != 0) {
    graph.setNodeWeight(0, std::numeric_limits<parhip::NodeWeight>::max());
    graph.setNodeWeight(1, 0);
  }
  auto config = parhip::PPartitionConfig{};
  if (selected == quality_metrics_failure_probe::mode::zero_k) {
    config.k = 0;
  } else if (selected == quality_metrics_failure_probe::mode::mismatched_k) {
    config.k = static_cast<parhip::PartitionID>(rank + 2);
  } else {
    config.k = 2;
  }
  auto metrics = parhip::distributed_quality_metrics{};
  auto partition_map = std::array<int, 2>{0, 1};
  quality_metrics_failure_probe::selected = selected;
  quality_metrics_failure_probe::expected_communicator = communicator;
  quality_metrics_failure_probe::active = true;
  if (selected == quality_metrics_failure_probe::mode::invalid_second_label) {
    static_cast<void>(metrics.balance_second(config, graph, communicator));
  } else if (selected ==
             quality_metrics_failure_probe::mode::invalid_load_label) {
    static_cast<void>(metrics.balance_load(config, graph, communicator));
  } else if (selected == quality_metrics_failure_probe::mode::load_overflow) {
    static_cast<void>(metrics.balance_load(config, graph, communicator));
  } else if (selected ==
             quality_metrics_failure_probe::mode::edge_cut_local_overflow) {
    static_cast<void>(metrics.edge_cut(graph, communicator));
  } else if (selected == quality_metrics_failure_probe::mode::
                             edge_cut_second_local_overflow) {
    static_cast<void>(metrics.edge_cut_second(graph, communicator));
  } else if (selected ==
             quality_metrics_failure_probe::mode::local_edge_cut_overflow) {
    static_cast<void>(
        metrics.local_edge_cut(graph, partition_map.data(), communicator));
  } else if (selected ==
             quality_metrics_failure_probe::mode::edge_cut_global_overflow) {
    static_cast<void>(metrics.edge_cut(graph, communicator));
  } else if (selected == quality_metrics_failure_probe::mode::
                             edge_cut_second_global_overflow) {
    static_cast<void>(metrics.edge_cut_second(graph, communicator));
  } else if (selected == quality_metrics_failure_probe::mode::
                             communication_volume_total_overflow) {
    static_cast<void>(metrics.comm_vol(config, graph, communicator));
  } else if (selected == quality_metrics_failure_probe::mode::
                             distribution_volume_global_overflow) {
    static_cast<void>(metrics.comm_vol_dist(graph, communicator));
  } else if (selected ==
             quality_metrics_failure_probe::mode::null_partition_map) {
    static_cast<void>(
        metrics.local_max_block_weight(config, graph, nullptr, communicator));
  } else {
    static_cast<void>(metrics.balance(config, graph, communicator));
  }

  quality_metrics_failure_probe::write_text(
      "quality-metrics operation returned without fail-fast\n");
  std::_Exit(92);
}
