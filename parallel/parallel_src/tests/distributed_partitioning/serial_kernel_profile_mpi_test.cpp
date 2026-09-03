#include <mpi.h>

#include <array>
#include <cstdint>
#include <vector>

#include <catch2/catch_all.hpp>

#include "communication/mpi_tools.h"
#include "data_structure/parallel_graph_access.h"
#include "distributed_partitioning/initial_partitioning/distributed_evolutionary_partitioning.h"
#include "partition_config.h"
#include "tools/random_functions.h"

namespace {
void build_zero_work_quotient(parhip::parallel_graph_access& graph,
                              MPI_Comm communicator) {
  auto rank = 0;
  auto size = 0;
  REQUIRE(MPI_Comm_rank(communicator, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(communicator, &size) == MPI_SUCCESS);
  auto ranges = std::vector<parhip::NodeID>(static_cast<std::size_t>(size) + 1,
                                             parhip::NodeID{2});
  ranges.front() = 0;
  auto const first = ranges[static_cast<std::size_t>(rank)];
  auto const end = ranges[static_cast<std::size_t>(rank + 1)];
  graph.start_construction(end - first, 0, 2, 0, false);
  graph.set_range(first, first == end ? first : end - 1);
  graph.set_range_array(ranges);
  for (auto global = first; global < end; ++global) {
    auto const node = graph.new_node();
    graph.setNodeWeight(node, 1);
    graph.setSecondPartitionIndex(node, 0);
  }
  graph.finish_construction();
}

[[nodiscard]] auto fields(
    kahip::serial_kernel::serial_kernel_profile const& profile)
    -> std::array<std::uint64_t, 17> {
  return {profile.global_nodes,
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
          profile.structural_validation_bytes,
          profile.base_memory_bytes,
          profile.flat_payload_elements,
          static_cast<std::uint64_t>(profile.reason)};
}
}  // namespace

TEST_CASE("serial-kernel profile agrees exactly with zero-local-work ranks",
          "[mpi][serial-kernel][profile]") {
  auto graph = parhip::parallel_graph_access{MPI_COMM_WORLD};
  build_zero_work_quotient(graph, MPI_COMM_WORLD);
  auto config = parhip::PPartitionConfig{};
  config.k = 1;
  config.upper_bound_partition = 2;

  auto const profile = parhip::mpi_tools{}.preflight_serial_kernel(
      MPI_COMM_WORLD, config, graph);
  REQUIRE(profile.safe());
  REQUIRE(profile.global_nodes == 2);
  REQUIRE(profile.global_directed_edges == 0);

  auto local = fields(profile);
  REQUIRE(local == std::array<std::uint64_t, 17>{
                       2, 0, 2, 1, 0, 0, 1, 2, 64, 20, 8, 28, 120, 0,
                       184, 5,
                       static_cast<std::uint64_t>(
                           kahip::serial_kernel::profile_reason::none)});
  auto all = std::array<std::uint64_t, 17>{};
  REQUIRE(MPI_Allreduce(local.data(), all.data(),
                        static_cast<int>(local.size()), MPI_UINT64_T, MPI_MIN,
                        MPI_COMM_WORLD) == MPI_SUCCESS);
  REQUIRE(all == local);
}

TEST_CASE("single-block distributed bridge never enters the generic kernel",
          "[mpi][serial-kernel][bridge]") {
  auto graph = parhip::parallel_graph_access{MPI_COMM_WORLD};
  build_zero_work_quotient(graph, MPI_COMM_WORLD);
  auto config = parhip::PPartitionConfig{};
  config.k = 1;
  config.upper_bound_partition = 2;
  config.seed = 73;
  parhip::random_functions::setSeed(config.seed);
  auto const expected_next = parhip::random_functions::nextInt(0, 1'000'000);
  parhip::random_functions::setSeed(config.seed);
  parhip::distributed_evolutionary_partitioning{}.perform_partitioning(
      MPI_COMM_WORLD, config, graph);
  for (parhip::NodeID node = 0; node < graph.number_of_local_nodes(); ++node) {
    CHECK(graph.getNodeLabel(node) == 0);
  }
  CHECK(parhip::random_functions::nextInt(0, 1'000'000) == expected_next);
}
