#include <mpi.h>

#include <catch2/catch_all.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <span>
#include <vector>

#include "communication/mpi_fixed_reduction.h"
#include "data_structure/balance_management_refinement.h"
#include "data_structure/parallel_graph_access.h"
#include "definitions.h"
#include "partition_config.h"
#include "tools/distributed_quality_metrics.h"

// KAHIP_PMPI_CALLBACK_REGION_BEGIN
namespace quality_metrics_probe {
enum class backend : unsigned char { legacy, large_count };
enum class collective : unsigned char { all_reduce, reduce };

struct operation final {
  collective selected_collective = collective::all_reduce;
  backend selected_backend = backend::legacy;
  MPI_Count count = 0;
  MPI_Datatype datatype = MPI_DATATYPE_NULL;
  MPI_Op reduction = MPI_OP_NULL;
  int root = -1;
  MPI_Comm communicator = MPI_COMM_NULL;
  bool send_buffer_is_null = false;
  bool receive_buffer_is_null = false;
};

struct observations final {
  std::array<operation, 128> operations{};
  int operation_count = 0;
  bool overflow = false;
};

inline bool active = false;
inline observations observed{};

void reset() noexcept {
  observed = {};
}

void record(collective selected_collective,
            backend selected_backend,
            MPI_Count count,
            MPI_Datatype datatype,
            MPI_Op reduction,
            int root,
            MPI_Comm communicator,
            void const* send_buffer,
            void const* receive_buffer) noexcept {
  if (!active) {
    return;
  }
  if (observed.operation_count >=
      static_cast<int>(observed.operations.size())) {
    observed.overflow = true;
    return;
  }
  observed.operations[static_cast<std::size_t>(observed.operation_count++)] = {
      .selected_collective = selected_collective,
      .selected_backend = selected_backend,
      .count = count,
      .datatype = datatype,
      .reduction = reduction,
      .root = root,
      .communicator = communicator,
      .send_buffer_is_null = send_buffer == nullptr,
      .receive_buffer_is_null = receive_buffer == nullptr};
}

class activation final {
 public:
  activation() noexcept {
    reset();
    active = true;
  }

  ~activation() noexcept { active = false; }

  activation(activation const&) = delete;
  auto operator=(activation const&) -> activation& = delete;
};
}  // namespace quality_metrics_probe

static_assert(noexcept(quality_metrics_probe::reset()));
static_assert(noexcept(
    quality_metrics_probe::record(quality_metrics_probe::collective::all_reduce,
                                  quality_metrics_probe::backend::legacy,
                                  0,
                                  MPI_DATATYPE_NULL,
                                  MPI_OP_NULL,
                                  -1,
                                  MPI_COMM_NULL,
                                  nullptr,
                                  nullptr)));

extern "C" int MPI_Allreduce(void const* send_buffer,
                             void* receive_buffer,
                             int count,
                             MPI_Datatype datatype,
                             MPI_Op reduction,
                             MPI_Comm communicator) {
  quality_metrics_probe::record(quality_metrics_probe::collective::all_reduce,
                                quality_metrics_probe::backend::legacy, count,
                                datatype, reduction, -1, communicator,
                                send_buffer, receive_buffer);
  return PMPI_Allreduce(send_buffer, receive_buffer, count, datatype, reduction,
                        communicator);
}

#if KAHIP_HAVE_MPI_ALLREDUCE_C
extern "C" int MPI_Allreduce_c(void const* send_buffer,
                               void* receive_buffer,
                               MPI_Count count,
                               MPI_Datatype datatype,
                               MPI_Op reduction,
                               MPI_Comm communicator) {
  quality_metrics_probe::record(quality_metrics_probe::collective::all_reduce,
                                quality_metrics_probe::backend::large_count,
                                count, datatype, reduction, -1, communicator,
                                send_buffer, receive_buffer);
  return PMPI_Allreduce_c(send_buffer, receive_buffer, count, datatype,
                          reduction, communicator);
}
#endif

extern "C" int MPI_Reduce(void const* send_buffer,
                          void* receive_buffer,
                          int count,
                          MPI_Datatype datatype,
                          MPI_Op reduction,
                          int root,
                          MPI_Comm communicator) {
  quality_metrics_probe::record(quality_metrics_probe::collective::reduce,
                                quality_metrics_probe::backend::legacy, count,
                                datatype, reduction, root, communicator,
                                send_buffer, receive_buffer);
  return PMPI_Reduce(send_buffer, receive_buffer, count, datatype, reduction,
                     root, communicator);
}

#if KAHIP_HAVE_MPI_REDUCE_C
extern "C" int MPI_Reduce_c(void const* send_buffer,
                            void* receive_buffer,
                            MPI_Count count,
                            MPI_Datatype datatype,
                            MPI_Op reduction,
                            int root,
                            MPI_Comm communicator) {
  quality_metrics_probe::record(quality_metrics_probe::collective::reduce,
                                quality_metrics_probe::backend::large_count,
                                count, datatype, reduction, root, communicator,
                                send_buffer, receive_buffer);
  return PMPI_Reduce_c(send_buffer, receive_buffer, count, datatype, reduction,
                       root, communicator);
}
#endif
// KAHIP_PMPI_CALLBACK_REGION_END

namespace {
[[nodiscard]] auto reversed_world() -> MPI_Comm {
  auto world_rank = -1;
  auto world_size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &world_rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &world_size) == MPI_SUCCESS);
  REQUIRE(world_size >= 1);
  REQUIRE(world_size <= 5);

  auto communicator = MPI_COMM_NULL;
  REQUIRE(MPI_Comm_split(MPI_COMM_WORLD, 0, world_size - world_rank,
                         &communicator) == MPI_SUCCESS);
  REQUIRE(communicator != MPI_COMM_NULL);
  return communicator;
}

void require_operation(quality_metrics_probe::operation const& operation,
                       quality_metrics_probe::backend backend,
                       MPI_Count count,
                       MPI_Datatype datatype,
                       MPI_Op reduction,
                       MPI_Comm communicator) {
  REQUIRE(operation.selected_backend == backend);
  REQUIRE(operation.selected_collective ==
          quality_metrics_probe::collective::all_reduce);
  REQUIRE(operation.count == count);
  REQUIRE(operation.datatype == datatype);
  REQUIRE(operation.reduction == reduction);
  REQUIRE(operation.root == -1);
  REQUIRE(operation.communicator == communicator);
  REQUIRE_FALSE(operation.send_buffer_is_null);
  REQUIRE_FALSE(operation.receive_buffer_is_null);
}

void require_reduce_operation(quality_metrics_probe::operation const& operation,
                              quality_metrics_probe::backend backend,
                              MPI_Count count,
                              MPI_Datatype datatype,
                              MPI_Op reduction,
                              int root,
                              MPI_Comm communicator) {
  REQUIRE(operation.selected_collective ==
          quality_metrics_probe::collective::reduce);
  REQUIRE(operation.selected_backend == backend);
  REQUIRE(operation.count == count);
  REQUIRE(operation.datatype == datatype);
  REQUIRE(operation.reduction == reduction);
  REQUIRE(operation.root == root);
  REQUIRE(operation.communicator == communicator);
  REQUIRE_FALSE(operation.send_buffer_is_null);
  REQUIRE_FALSE(operation.receive_buffer_is_null);
}

#if KAHIP_HAVE_MPI_ALLREDUCE_C
constexpr auto expected_all_reduce_payload_backend =
    quality_metrics_probe::backend::large_count;
#else
constexpr auto expected_all_reduce_payload_backend =
    quality_metrics_probe::backend::legacy;
#endif

#if KAHIP_HAVE_MPI_REDUCE_C
constexpr auto expected_reduce_payload_backend =
    quality_metrics_probe::backend::large_count;
#else
constexpr auto expected_reduce_payload_backend =
    quality_metrics_probe::backend::legacy;
#endif

[[nodiscard]] auto active_rank_count(int size) noexcept -> int {
  return size == 1 ? 1 : size - 1;
}

[[nodiscard]] auto active_rank_factor_sum(int size) noexcept
    -> parhip::NodeWeight {
  if (size == 1) {
    return 1;
  }
  return static_cast<parhip::NodeWeight>(size * (size + 1) / 2 - 1);
}

void build_metric_fixture(parhip::parallel_graph_access& graph,
                          int rank,
                          int size,
                          std::vector<int>& partition_map) {
  auto const active = size == 1 || rank != 0;
  auto const local_nodes = active ? parhip::NodeID{2} : parhip::NodeID{0};
  auto const local_edges = active ? parhip::EdgeID{2} : parhip::EdgeID{0};
  auto const active_ranks = active_rank_count(size);
  auto const global_nodes = static_cast<parhip::NodeID>(2 * active_ranks);
  auto const global_edges = static_cast<parhip::EdgeID>(2 * active_ranks);
  graph.start_construction(local_nodes, local_edges, global_nodes, global_edges,
                           false);

  auto ranges = std::vector<parhip::NodeID>(static_cast<std::size_t>(size) + 1);
  for (int index = 0; index <= size; ++index) {
    auto const active_before = size == 1 ? index : std::max(0, index - 1);
    ranges[static_cast<std::size_t>(index)] =
        static_cast<parhip::NodeID>(2 * active_before);
  }
  auto const from = ranges[static_cast<std::size_t>(rank)];
  auto const to =
      local_nodes == 0 ? from : ranges[static_cast<std::size_t>(rank) + 1] - 1;
  graph.set_range(from, to);
  graph.set_range_array(ranges);

  if (active) {
    auto const factor = static_cast<parhip::NodeWeight>(rank + 1);
    auto const first = graph.new_node();
    graph.setNodeWeight(first, factor);
    graph.setNodeLabel(first, 0);
    graph.setSecondPartitionIndex(first, 0);
    auto const first_edge = graph.new_edge(first, from + 1);
    graph.setEdgeWeight(first_edge, factor);

    auto const second = graph.new_node();
    graph.setNodeWeight(second, 2 * factor);
    graph.setNodeLabel(second, 1);
    graph.setSecondPartitionIndex(second, 1);
    auto const second_edge = graph.new_edge(second, from);
    graph.setEdgeWeight(second_edge, factor);
    partition_map = {0, 1};
  }
  graph.finish_construction();
}

void build_zero_weight_fixture(parhip::parallel_graph_access& graph,
                               int rank,
                               int size,
                               bool include_nodes) {
  auto const active = include_nodes && (size == 1 || rank != 0);
  auto const local_nodes = active ? parhip::NodeID{1} : parhip::NodeID{0};
  auto const active_ranks = include_nodes ? active_rank_count(size) : 0;
  auto const global_nodes = static_cast<parhip::NodeID>(active_ranks);
  graph.start_construction(local_nodes, 0, global_nodes, 0, false);

  auto ranges = std::vector<parhip::NodeID>(static_cast<std::size_t>(size) + 1);
  for (int index = 0; index <= size; ++index) {
    auto const nodes_before =
        include_nodes ? (size == 1 ? index : std::max(0, index - 1)) : 0;
    ranges[static_cast<std::size_t>(index)] =
        static_cast<parhip::NodeID>(nodes_before);
  }
  auto const from = ranges[static_cast<std::size_t>(rank)];
  graph.set_range(from, from);
  graph.set_range_array(ranges);
  if (active) {
    auto const node = graph.new_node();
    graph.setNodeWeight(node, 0);
    graph.setNodeLabel(node, static_cast<parhip::PartitionID>(rank % 3));
    graph.setSecondPartitionIndex(
        node, static_cast<parhip::PartitionID>((rank + 1) % 3));
  }
  graph.finish_construction();
}

void build_large_exact_fixture(parhip::parallel_graph_access& graph,
                               int rank,
                               int size,
                               parhip::NodeWeight weight) {
  auto const active = rank + 1 == size;
  auto const local_nodes = active ? parhip::NodeID{1} : parhip::NodeID{0};
  graph.start_construction(local_nodes, 0, 1, 0, false);
  auto ranges = std::vector<parhip::NodeID>(static_cast<std::size_t>(size) + 1);
  ranges.back() = 1;
  graph.set_range(0, 0);
  graph.set_range_array(ranges);
  if (active) {
    auto const node = graph.new_node();
    graph.setNodeWeight(node, weight);
    graph.setNodeLabel(node, 0);
    graph.setSecondPartitionIndex(node, 0);
  }
  graph.finish_construction();
}
}  // namespace

TEST_CASE("default integral reductions select the detected payload backends") {
  auto communicator = reversed_world();
  auto rank = -1;
  auto size = 0;
  REQUIRE(MPI_Comm_rank(communicator, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(communicator, &size) == MPI_SUCCESS);

  auto const local = std::array<parhip::NodeWeight, 2>{
      static_cast<parhip::NodeWeight>(rank + 1),
      static_cast<parhip::NodeWeight>(2 * (rank + 1))};
  auto global = std::array<parhip::NodeWeight, 2>{};
  auto root = std::array<parhip::NodeWeight, 2>{};
  quality_metrics_probe::observations observed{};
  {
    quality_metrics_probe::activation const probe;
    parhip::mpi::all_reduce_bounded(
        std::span<parhip::NodeWeight const>{local}, std::span{global},
        parhip::mpi::reduction_kind::sum,
        parhip::mpi::communicator_view{communicator},
        "MPI_Allreduce(test default backend)");
    parhip::mpi::reduce_bounded(
        std::span<parhip::NodeWeight const>{local}, std::span{root},
        parhip::mpi::reduction_kind::sum, 0,
        parhip::mpi::communicator_view{communicator},
        "MPI_Reduce(test default backend)");
    observed = quality_metrics_probe::observed;
  }

  auto const rank_sum = static_cast<parhip::NodeWeight>(size * (size + 1) / 2);
  REQUIRE(global == std::array<parhip::NodeWeight, 2>{rank_sum, 2 * rank_sum});
  if (rank == 0) {
    REQUIRE(root == std::array<parhip::NodeWeight, 2>{rank_sum, 2 * rank_sum});
  }
  REQUIRE_FALSE(observed.overflow);
  REQUIRE(observed.operation_count == 6);
  require_operation(observed.operations[0],
                    quality_metrics_probe::backend::legacy, 7, MPI_UINT64_T,
                    MPI_MIN, communicator);
  require_operation(observed.operations[1],
                    quality_metrics_probe::backend::legacy, 7, MPI_UINT64_T,
                    MPI_MAX, communicator);
  require_operation(observed.operations[2], expected_all_reduce_payload_backend,
                    2, MPI_UNSIGNED_LONG_LONG, MPI_SUM, communicator);
  require_operation(observed.operations[3],
                    quality_metrics_probe::backend::legacy, 7, MPI_UINT64_T,
                    MPI_MIN, communicator);
  require_operation(observed.operations[4],
                    quality_metrics_probe::backend::legacy, 7, MPI_UINT64_T,
                    MPI_MAX, communicator);
  require_reduce_operation(observed.operations[5],
                           expected_reduce_payload_backend, 2,
                           MPI_UNSIGNED_LONG_LONG, MPI_SUM, 0, communicator);

  REQUIRE(MPI_Comm_free(&communicator) == MPI_SUCCESS);
}

TEST_CASE("integral all-reduction uses deterministic bounded MPI-3 rounds") {
  auto communicator = reversed_world();
  auto rank = -1;
  auto size = 0;
  REQUIRE(MPI_Comm_rank(communicator, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(communicator, &size) == MPI_SUCCESS);

  auto local = std::array<parhip::NodeWeight, 5>{};
  for (std::size_t index = 0; index < local.size(); ++index) {
    local[index] = static_cast<parhip::NodeWeight>((rank + 1) * (index + 1));
  }
  auto global = std::array<parhip::NodeWeight, 5>{};
  quality_metrics_probe::observations observed{};
  {
    quality_metrics_probe::activation const probe;
    parhip::mpi::all_reduce_bounded(
        std::span<parhip::NodeWeight const>{local}, std::span{global},
        parhip::mpi::reduction_kind::sum,
        parhip::mpi::communicator_view{communicator},
        "MPI_Allreduce(test bounded sum)",
        {.mpi3_round_ceiling = 2, .force_mpi3 = true});
    observed = quality_metrics_probe::observed;
  }

  auto const rank_sum = static_cast<parhip::NodeWeight>(size * (size + 1) / 2);
  for (std::size_t index = 0; index < global.size(); ++index) {
    REQUIRE(global[index] == rank_sum * (index + 1));
  }
  REQUIRE_FALSE(observed.overflow);
  REQUIRE(observed.operation_count == 5);
  require_operation(observed.operations[0],
                    quality_metrics_probe::backend::legacy, 7, MPI_UINT64_T,
                    MPI_MIN, communicator);
  require_operation(observed.operations[1],
                    quality_metrics_probe::backend::legacy, 7, MPI_UINT64_T,
                    MPI_MAX, communicator);
  require_operation(observed.operations[2],
                    quality_metrics_probe::backend::legacy, 2,
                    MPI_UNSIGNED_LONG_LONG, MPI_SUM, communicator);
  require_operation(observed.operations[3],
                    quality_metrics_probe::backend::legacy, 2,
                    MPI_UNSIGNED_LONG_LONG, MPI_SUM, communicator);
  require_operation(observed.operations[4],
                    quality_metrics_probe::backend::legacy, 1,
                    MPI_UNSIGNED_LONG_LONG, MPI_SUM, communicator);

  REQUIRE(MPI_Comm_free(&communicator) == MPI_SUCCESS);
}

TEST_CASE("integral all-reduction retains an empty payload collective") {
  auto communicator = reversed_world();
  quality_metrics_probe::observations observed{};
  {
    quality_metrics_probe::activation const probe;
    parhip::mpi::all_reduce_bounded(
        std::span<parhip::NodeWeight const>{}, std::span<parhip::NodeWeight>{},
        parhip::mpi::reduction_kind::sum,
        parhip::mpi::communicator_view{communicator},
        "MPI_Allreduce(test empty sum)",
        {.mpi3_round_ceiling = 2, .force_mpi3 = true});
    observed = quality_metrics_probe::observed;
  }

  REQUIRE_FALSE(observed.overflow);
  REQUIRE(observed.operation_count == 3);
  require_operation(observed.operations[2],
                    quality_metrics_probe::backend::legacy, 0,
                    MPI_UNSIGNED_LONG_LONG, MPI_SUM, communicator);
  REQUIRE(MPI_Comm_free(&communicator) == MPI_SUCCESS);
}

TEST_CASE("integral root reduction uses deterministic bounded MPI-3 rounds") {
  auto communicator = reversed_world();
  auto rank = -1;
  auto size = 0;
  REQUIRE(MPI_Comm_rank(communicator, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(communicator, &size) == MPI_SUCCESS);

  auto local = std::array<parhip::NodeWeight, 5>{};
  for (std::size_t index = 0; index < local.size(); ++index) {
    local[index] = static_cast<parhip::NodeWeight>((rank + 1) * (index + 1));
  }
  auto reduced = std::array<parhip::NodeWeight, 5>{};
  quality_metrics_probe::observations observed{};
  {
    quality_metrics_probe::activation const probe;
    parhip::mpi::reduce_bounded(std::span<parhip::NodeWeight const>{local},
                                std::span{reduced},
                                parhip::mpi::reduction_kind::maximum, 0,
                                parhip::mpi::communicator_view{communicator},
                                "MPI_Reduce(test bounded maximum)",
                                {.mpi3_round_ceiling = 2, .force_mpi3 = true});
    observed = quality_metrics_probe::observed;
  }

  if (rank == 0) {
    for (std::size_t index = 0; index < reduced.size(); ++index) {
      REQUIRE(reduced[index] ==
              static_cast<parhip::NodeWeight>(size * (index + 1)));
    }
  }
  REQUIRE_FALSE(observed.overflow);
  REQUIRE(observed.operation_count == 5);
  require_reduce_operation(observed.operations[2],
                           quality_metrics_probe::backend::legacy, 2,
                           MPI_UNSIGNED_LONG_LONG, MPI_MAX, 0, communicator);
  require_reduce_operation(observed.operations[3],
                           quality_metrics_probe::backend::legacy, 2,
                           MPI_UNSIGNED_LONG_LONG, MPI_MAX, 0, communicator);
  require_reduce_operation(observed.operations[4],
                           quality_metrics_probe::backend::legacy, 1,
                           MPI_UNSIGNED_LONG_LONG, MPI_MAX, 0, communicator);
  REQUIRE(MPI_Comm_free(&communicator) == MPI_SUCCESS);
}

TEST_CASE("integral root reduction retains an empty payload collective") {
  auto communicator = reversed_world();
  quality_metrics_probe::observations observed{};
  {
    quality_metrics_probe::activation const probe;
    parhip::mpi::reduce_bounded(std::span<parhip::NodeWeight const>{},
                                std::span<parhip::NodeWeight>{},
                                parhip::mpi::reduction_kind::sum, 0,
                                parhip::mpi::communicator_view{communicator},
                                "MPI_Reduce(test empty sum)",
                                {.mpi3_round_ceiling = 2, .force_mpi3 = true});
    observed = quality_metrics_probe::observed;
  }

  REQUIRE_FALSE(observed.overflow);
  REQUIRE(observed.operation_count == 3);
  require_reduce_operation(observed.operations[2],
                           quality_metrics_probe::backend::legacy, 0,
                           MPI_UNSIGNED_LONG_LONG, MPI_SUM, 0, communicator);
  REQUIRE(MPI_Comm_free(&communicator) == MPI_SUCCESS);
}

TEST_CASE("local maximum block weight remains root-local") {
  auto communicator = reversed_world();
  auto rank = -1;
  auto size = 0;
  REQUIRE(MPI_Comm_rank(communicator, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(communicator, &size) == MPI_SUCCESS);

  auto graph = parhip::parallel_graph_access{communicator};
  auto partition_map = std::vector<int>{};
  build_metric_fixture(graph, rank, size, partition_map);
  auto config = parhip::PPartitionConfig{};
  config.k = 2;
  auto metrics = parhip::distributed_quality_metrics{};

  if (rank == 0) {
    auto const expected =
        size == 1 ? parhip::NodeWeight{2} : parhip::NodeWeight{0};
    REQUIRE(metrics.local_max_block_weight(config, graph, partition_map.data(),
                                           communicator) == expected);
  }
  REQUIRE(MPI_Barrier(communicator) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_free(&communicator) == MPI_SUCCESS);
}

TEST_CASE("distributed quality metrics preserve exact integral objectives") {
  auto communicator = reversed_world();
  auto rank = -1;
  auto size = 0;
  REQUIRE(MPI_Comm_rank(communicator, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(communicator, &size) == MPI_SUCCESS);

  auto graph = parhip::parallel_graph_access{communicator};
  auto partition_map = std::vector<int>{};
  build_metric_fixture(graph, rank, size, partition_map);
  auto config = parhip::PPartitionConfig{};
  config.k = 2;
  auto metrics = parhip::distributed_quality_metrics{};

  auto edge_cut_second = parhip::EdgeWeight{};
  auto edge_cut = parhip::EdgeWeight{};
  auto balance = 0.0;
  auto balance_second = 0.0;
  auto balance_load = 0.0;
  auto balance_load_dist = 0.0;
  auto communication_volume = parhip::EdgeWeight{};
  auto distribution_communication_volume = parhip::EdgeWeight{};
  quality_metrics_probe::observations observed{};
  {
    quality_metrics_probe::activation const probe;
    edge_cut_second = metrics.edge_cut_second(graph, communicator);
    edge_cut = metrics.edge_cut(graph, communicator);
    balance = metrics.balance(config, graph, communicator);
    balance_second = metrics.balance_second(config, graph, communicator);
    balance_load = metrics.balance_load(config, graph, communicator);
    balance_load_dist = metrics.balance_load_dist(config, graph, communicator);
    communication_volume = metrics.comm_vol(config, graph, communicator);
    distribution_communication_volume =
        metrics.comm_vol_dist(graph, communicator);
    observed = quality_metrics_probe::observed;
  }

  auto const factor_sum = active_rank_factor_sum(size);
  auto const active_ranks =
      static_cast<parhip::NodeWeight>(active_rank_count(size));
  REQUIRE(edge_cut_second == factor_sum);
  REQUIRE(edge_cut == factor_sum);
  REQUIRE(balance ==
          Catch::Approx(static_cast<double>(2 * factor_sum) /
                        std::ceil(static_cast<double>(3 * factor_sum) / 2.0)));
  REQUIRE(balance_second == balance);
  REQUIRE(balance_load ==
          Catch::Approx(
              static_cast<double>(2 * factor_sum + active_ranks) /
              std::ceil(static_cast<double>(3 * factor_sum + 2 * active_ranks) /
                        2.0)));
  auto const largest_local_load =
      size == 1 ? parhip::NodeWeight{5}
                : static_cast<parhip::NodeWeight>(3 * size + 2);
  REQUIRE(balance_load_dist ==
          Catch::Approx(
              static_cast<double>(largest_local_load) /
              std::ceil(static_cast<double>(3 * factor_sum + 2 * active_ranks) /
                        static_cast<double>(size))));
  REQUIRE(communication_volume == 2 * active_ranks);
  REQUIRE(distribution_communication_volume == 0);
  REQUIRE_FALSE(observed.overflow);
  REQUIRE(observed.operation_count == 75);
  for (auto index = 0; index < observed.operation_count; ++index) {
    REQUIRE(observed.operations[static_cast<std::size_t>(index)].communicator ==
            communicator);
  }
  if (size > 1) {
    REQUIRE((rank == 0 ? graph.number_of_local_nodes() == 0
                       : graph.number_of_local_nodes() == 2));
  }

  REQUIRE(MPI_Comm_free(&communicator) == MPI_SUCCESS);
}

TEST_CASE("zero-total distributed balances use the neutral value") {
  auto communicator = reversed_world();
  auto rank = -1;
  auto size = 0;
  REQUIRE(MPI_Comm_rank(communicator, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(communicator, &size) == MPI_SUCCESS);

  auto config = parhip::PPartitionConfig{};
  config.k = 3;
  auto metrics = parhip::distributed_quality_metrics{};
  for (auto const include_nodes : {false, true}) {
    auto graph = parhip::parallel_graph_access{communicator};
    build_zero_weight_fixture(graph, rank, size, include_nodes);
    auto const balances = std::array{
        metrics.balance(config, graph, communicator),
        metrics.balance_second(config, graph, communicator),
        metrics.balance_load(config, graph, communicator),
        metrics.balance_load_dist(config, graph, communicator),
    };
    for (auto const value : balances) {
      REQUIRE(std::isfinite(value));
      REQUIRE(value == 1.0);
    }
  }

  REQUIRE(MPI_Comm_free(&communicator) == MPI_SUCCESS);
}

TEST_CASE("distributed balances form the ceiling in the integer domain") {
  auto communicator = reversed_world();
  auto rank = -1;
  auto size = 0;
  REQUIRE(MPI_Comm_rank(communicator, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(communicator, &size) == MPI_SUCCESS);

  constexpr auto total = parhip::NodeWeight{9'007'199'254'740'993ULL};
  auto graph = parhip::parallel_graph_access{communicator};
  build_large_exact_fixture(graph, rank, size, total);
  auto config = parhip::PPartitionConfig{};
  config.k = 2;
  auto metrics = parhip::distributed_quality_metrics{};

  auto const block_denominator = total / config.k + (total % config.k != 0);
  auto const block_balance =
      static_cast<double>(total) / static_cast<double>(block_denominator);
  REQUIRE(metrics.balance(config, graph, communicator) == block_balance);
  REQUIRE(metrics.balance_second(config, graph, communicator) == block_balance);
  REQUIRE(metrics.balance_load(config, graph, communicator) == block_balance);

  auto const rank_count = static_cast<parhip::NodeWeight>(size);
  auto const rank_denominator = total / rank_count + (total % rank_count != 0);
  auto const rank_balance =
      static_cast<double>(total) / static_cast<double>(rank_denominator);
  REQUIRE(metrics.balance_load_dist(config, graph, communicator) ==
          rank_balance);

  REQUIRE(MPI_Comm_free(&communicator) == MPI_SUCCESS);
}

TEST_CASE("refinement balance management preserves weighted global blocks") {
  auto communicator = reversed_world();
  auto rank = -1;
  auto size = 0;
  REQUIRE(MPI_Comm_rank(communicator, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(communicator, &size) == MPI_SUCCESS);

  auto graph = parhip::parallel_graph_access{communicator};
  auto partition_map = std::vector<int>{};
  build_metric_fixture(graph, rank, size, partition_map);
  auto manager = parhip::balance_management_refinement{&graph, 2};
  auto const factor_sum = active_rank_factor_sum(size);
  REQUIRE(manager.getBlockSize(0) == factor_sum);
  REQUIRE(manager.getBlockSize(1) == 2 * factor_sum);

  REQUIRE(MPI_Comm_free(&communicator) == MPI_SUCCESS);
}
