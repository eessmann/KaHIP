#include <mpi.h>

#include <catch2/catch_all.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <vector>

#include "data_structure/parallel_graph_access.h"
#include "communication/mpi_handles.h"
#include "distributed_partitioning/distributed_partitioner.h"
#include "distributed_partitioning/initial_partitioning/random_initial_partitioning.h"
#include "partition_config.h"
#include "tools/random_functions.h"

namespace {
using parhip::NodeID;
using parhip::NodeWeight;
using parhip::PPartitionConfig;
using parhip::parallel_graph_access;

class scoped_communicator final {
 public:
  explicit scoped_communicator(MPI_Comm communicator) noexcept
      : communicator_(communicator) {}

  ~scoped_communicator() {
    if (communicator_ != MPI_COMM_NULL) {
      REQUIRE(MPI_Comm_free(&communicator_) == MPI_SUCCESS);
    }
  }

  scoped_communicator(scoped_communicator const&) = delete;
  auto operator=(scoped_communicator const&) -> scoped_communicator& = delete;

  [[nodiscard]] auto get() const noexcept -> MPI_Comm { return communicator_; }

 private:
  MPI_Comm communicator_ = MPI_COMM_NULL;
};

[[nodiscard]] auto reversed_world() -> scoped_communicator {
  auto world_rank = 0;
  auto world_size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &world_rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &world_size) == MPI_SUCCESS);
  auto result = MPI_COMM_NULL;
  REQUIRE(MPI_Comm_split(MPI_COMM_WORLD, 0, world_size - world_rank, &result) ==
          MPI_SUCCESS);
  return scoped_communicator{result};
}

void build_weighted_isolates(parallel_graph_access& graph,
                             MPI_Comm communicator) {
  auto rank = 0;
  auto size = 0;
  REQUIRE(MPI_Comm_rank(communicator, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(communicator, &size) == MPI_SUCCESS);

  constexpr auto global_nodes = NodeID{4};
  auto ranges = std::vector<NodeID>(static_cast<std::size_t>(size) + 1,
                                    global_nodes);
  ranges.front() = 0;
  auto const first = ranges[static_cast<std::size_t>(rank)];
  auto const end = ranges[static_cast<std::size_t>(rank + 1)];
  graph.start_construction(end - first, 0, global_nodes, 0, false);
  graph.set_range(first, first == end ? first : end - 1);
  graph.set_range_array(ranges);

  constexpr auto weights = std::array<NodeWeight, 4>{2, 3, 5, 7};
  for (auto global = first; global < end; ++global) {
    auto const local = graph.new_node();
    graph.setNodeWeight(local, weights[static_cast<std::size_t>(global)]);
    graph.setNodeLabel(local, NodeID{99});
    graph.setSecondPartitionIndex(local, 0);
  }
  graph.finish_construction();
}

[[nodiscard]] auto local_labels(parallel_graph_access& graph)
    -> std::vector<NodeID> {
  auto result = std::vector<NodeID>(graph.number_of_local_nodes());
  std::ranges::transform(
      std::views::iota(NodeID{0}, graph.number_of_local_nodes()),
      result.begin(), [&](NodeID node) { return graph.getNodeLabel(node); });
  return result;
}

[[nodiscard]] auto global_block_weights(parallel_graph_access& graph,
                                        MPI_Comm communicator)
    -> std::array<NodeWeight, 3> {
  auto local = std::array<NodeWeight, 3>{};
  for (NodeID node = 0; node < graph.number_of_local_nodes(); ++node) {
    auto const block = graph.getNodeLabel(node);
    REQUIRE(block < local.size());
    local[static_cast<std::size_t>(block)] += graph.getNodeWeight(node);
  }
  auto global = std::array<NodeWeight, 3>{};
  REQUIRE(MPI_Allreduce(local.data(), global.data(),
                        static_cast<int>(global.size()),
                        MPI_UNSIGNED_LONG_LONG, MPI_SUM, communicator) ==
          MPI_SUCCESS);
  return global;
}
}  // namespace

TEST_CASE("random initial partitioning preserves deterministic weighted labels "
          "with zero-local-work ranks",
          "[unit][mpi][distributed-partitioner][random]") {
  auto communicator = reversed_world();
  auto graph = parallel_graph_access{communicator.get()};
  build_weighted_isolates(graph, communicator.get());

  auto config = PPartitionConfig{};
  config.k = 3;
  auto partitioner = parhip::random_initial_partitioning{};

  for (auto repetition = 0; repetition < 2; ++repetition) {
    parhip::random_functions::setSeed(23);
    partitioner.perform_partitioning(
        parhip::mpi::communicator_view{communicator.get()}, config, graph);

    auto rank = 0;
    REQUIRE(MPI_Comm_rank(communicator.get(), &rank) == MPI_SUCCESS);
    if (rank == 0) {
      REQUIRE(local_labels(graph) == std::vector<NodeID>{1, 2, 2, 0});
    } else {
      REQUIRE(local_labels(graph).empty());
    }
    REQUIRE(global_block_weights(graph, communicator.get()) ==
            std::array<NodeWeight, 3>{7, 2, 8});
  }
}

TEST_CASE("random-choice generation preserves the upstream draw stream across "
          "repeated calls",
          "[unit][mpi][distributed-partitioner][random]") {
  auto config = PPartitionConfig{};
  config.num_tries = 2;
  config.num_vcycles = 3;

  parhip::random_functions::setSeed(17);
  parhip::distributed_partitioner::generate_random_choices(
      config, parhip::mpi::communicator_view{MPI_COMM_WORLD});
  REQUIRE(parhip::random_functions::nextInt(0ULL, 1000000ULL) == 637521);
  REQUIRE(std::bit_cast<std::uint64_t>(
              parhip::random_functions::nextDouble(0.0, 1.0)) ==
          4592438611616939308ULL);

  parhip::random_functions::setSeed(17);
  parhip::distributed_partitioner::generate_random_choices(
      config, parhip::mpi::communicator_view{MPI_COMM_WORLD});
  parhip::distributed_partitioner::generate_random_choices(
      config, parhip::mpi::communicator_view{MPI_COMM_WORLD});
  REQUIRE(parhip::random_functions::nextInt(0ULL, 1000000ULL) == 864042);
  REQUIRE(std::bit_cast<std::uint64_t>(
              parhip::random_functions::nextDouble(0.0, 1.0)) ==
          4598724536360494900ULL);
}
