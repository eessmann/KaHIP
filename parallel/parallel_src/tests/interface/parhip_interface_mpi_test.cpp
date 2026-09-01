#include <mpi.h>

#include <catch2/catch_all.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>
#include <vector>

#include "communication/mpi_handles.h"
#include "configuration.h"
#include "parhip_interface.h"

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

struct local_cycle final {
  std::vector<idxtype> distribution;
  std::vector<idxtype> offsets;
  std::vector<idxtype> neighbors;
  std::vector<idxtype> partition;
};

[[nodiscard]] auto make_cycle(int rank, int size, idxtype vertex_count)
    -> local_cycle {
  auto fixture = local_cycle{};
  fixture.distribution.resize(static_cast<std::size_t>(size) + 1);
  for (auto index = 0; index <= size; ++index) {
    fixture.distribution[static_cast<std::size_t>(index)] =
        static_cast<idxtype>(index) * vertex_count / static_cast<idxtype>(size);
  }

  auto const first = fixture.distribution[static_cast<std::size_t>(rank)];
  auto const last = fixture.distribution[static_cast<std::size_t>(rank) + 1];
  auto const local_count = last - first;
  fixture.offsets.reserve(static_cast<std::size_t>(local_count) + 1);
  fixture.offsets.push_back(0);
  fixture.neighbors.reserve(static_cast<std::size_t>(2 * local_count));
  for (auto global = first; global < last; ++global) {
    fixture.neighbors.push_back((global + vertex_count - 1) % vertex_count);
    fixture.neighbors.push_back((global + 1) % vertex_count);
    fixture.offsets.push_back(static_cast<idxtype>(fixture.neighbors.size()));
  }
  fixture.partition.resize(static_cast<std::size_t>(local_count));
  return fixture;
}

[[nodiscard]] auto require_valid_cycle_partition(local_cycle const& fixture,
                                                 int rank,
                                                 int size,
                                                 int edge_cut,
                                                 MPI_Comm communicator)
    -> std::vector<idxtype> {
  auto const vertex_count =
      static_cast<std::size_t>(fixture.distribution.back());
  constexpr auto block_count = idxtype{2};
  REQUIRE(std::ranges::all_of(
      fixture.partition, [](idxtype block) { return block < block_count; }));

  auto counts = std::vector<int>(static_cast<std::size_t>(size));
  auto displacements = std::vector<int>(static_cast<std::size_t>(size));
  for (auto index = 0; index < size; ++index) {
    auto const begin = fixture.distribution[static_cast<std::size_t>(index)];
    auto const end = fixture.distribution[static_cast<std::size_t>(index) + 1];
    REQUIRE(std::in_range<int>(end - begin));
    REQUIRE(std::in_range<int>(begin));
    counts[static_cast<std::size_t>(index)] = static_cast<int>(end - begin);
    displacements[static_cast<std::size_t>(index)] = static_cast<int>(begin);
  }
  auto global_partition = std::vector<idxtype>(vertex_count);
  auto const local_count = counts[static_cast<std::size_t>(rank)];
  REQUIRE(MPI_Allgatherv(fixture.partition.data(), local_count,
                         MPI_UNSIGNED_LONG_LONG, global_partition.data(),
                         counts.data(), displacements.data(),
                         MPI_UNSIGNED_LONG_LONG, communicator) == MPI_SUCCESS);

  auto block_weights = std::array<int, 2>{};
  for (auto const block : global_partition) {
    REQUIRE(block < block_count);
    ++block_weights[static_cast<std::size_t>(block)];
  }
  auto const upper_bound = static_cast<int>(vertex_count / 2);
  REQUIRE(block_weights[0] <= upper_bound);
  REQUIRE(block_weights[1] <= upper_bound);

  auto recomputed_cut = 0;
  for (auto vertex = std::size_t{0}; vertex < vertex_count; ++vertex) {
    auto const successor = (vertex + 1) % vertex_count;
    recomputed_cut +=
        global_partition[vertex] != global_partition[successor] ? 1 : 0;
  }
  REQUIRE(edge_cut == recomputed_cut);
  return global_partition;
}
}  // namespace

TEST_CASE("ParHIP ECO configuration follows the operation communicator") {
  auto world_rank = -1;
  auto world_size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &world_rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &world_size) == MPI_SUCCESS);

  auto subset = MPI_COMM_NULL;
  REQUIRE(MPI_Comm_split(MPI_COMM_WORLD, world_rank % 2, world_rank, &subset) ==
          MPI_SUCCESS);
  auto subset_size = 0;
  REQUIRE(MPI_Comm_size(subset, &subset_size) == MPI_SUCCESS);

  auto config = parhip::PPartitionConfig{};
  auto defaults = parhip::configuration{};
  defaults.standard(config);
  defaults.eco(config, parhip::mpi::communicator_view{subset});
  CHECK(config.evolutionary_time_limit == 2048 / subset_size);
  if (subset_size != world_size) {
    CHECK(config.evolutionary_time_limit != 2048 / world_size);
  }
  REQUIRE(MPI_Comm_free(&subset) == MPI_SUCCESS);
}

TEST_CASE("ParHIP partitions a cycle on ranks one through five") {
  auto communicator = reversed_world();
  auto rank = -1;
  auto size = 0;
  REQUIRE(MPI_Comm_rank(communicator, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(communicator, &size) == MPI_SUCCESS);

  auto fixture = make_cycle(rank, size, 32);

  auto blocks = 2;
  auto imbalance = 0.03;
  auto edge_cut = -1;
  ParHIPPartitionKWay(fixture.distribution.data(), fixture.offsets.data(),
                      fixture.neighbors.data(), nullptr, nullptr, &blocks,
                      &imbalance, true, 1, FASTMESH, &edge_cut,
                      fixture.partition.data(), &communicator);

  auto const first_local_partition = fixture.partition;
  auto const first_global_partition = require_valid_cycle_partition(
      fixture, rank, size, edge_cut, communicator);
  auto const first_edge_cut = edge_cut;

  std::ranges::fill(fixture.partition, std::numeric_limits<idxtype>::max());
  edge_cut = -1;
  ParHIPPartitionKWay(fixture.distribution.data(), fixture.offsets.data(),
                      fixture.neighbors.data(), nullptr, nullptr, &blocks,
                      &imbalance, true, 1, FASTMESH, &edge_cut,
                      fixture.partition.data(), &communicator);

  auto const second_global_partition = require_valid_cycle_partition(
      fixture, rank, size, edge_cut, communicator);
  REQUIRE(fixture.partition == first_local_partition);
  REQUIRE(second_global_partition == first_global_partition);
  REQUIRE(edge_cut == first_edge_cut);
  REQUIRE(MPI_Comm_free(&communicator) == MPI_SUCCESS);
}

TEST_CASE("ParHIP does not coarsen a feasible block into an overweight cluster") {
  auto communicator = reversed_world();
  auto rank = -1;
  auto size = 0;
  REQUIRE(MPI_Comm_rank(communicator, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(communicator, &size) == MPI_SUCCESS);

  auto fixture = make_cycle(rank, size, 4);
  auto blocks = 2;
  auto imbalance = 0.03;
  auto edge_cut = -1;

  ParHIPPartitionKWay(fixture.distribution.data(), fixture.offsets.data(),
                      fixture.neighbors.data(), nullptr, nullptr, &blocks,
                      &imbalance, true, 1, FASTMESH, &edge_cut,
                      fixture.partition.data(), &communicator);

  static_cast<void>(require_valid_cycle_partition(
      fixture, rank, size, edge_cut, communicator));
  REQUIRE(MPI_Comm_free(&communicator) == MPI_SUCCESS);
}

TEST_CASE("ParHIP accepts a leading zero-work rank") {
  auto communicator = reversed_world();
  auto rank = -1;
  auto size = 0;
  REQUIRE(MPI_Comm_rank(communicator, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(communicator, &size) == MPI_SUCCESS);
  if (size != 5) {
    REQUIRE(MPI_Comm_free(&communicator) == MPI_SUCCESS);
    return;
  }

  auto fixture = make_cycle(rank, size, 4);
  if (rank == 0) {
    REQUIRE(fixture.partition.empty());
    REQUIRE(fixture.neighbors.empty());
  }
  auto blocks = 1;
  auto imbalance = 0.03;
  auto edge_cut = -1;
  ParHIPPartitionKWay(fixture.distribution.data(), fixture.offsets.data(),
                      fixture.neighbors.data(), nullptr, nullptr, &blocks,
                      &imbalance, true, 1, FASTMESH, &edge_cut,
                      fixture.partition.data(), &communicator);

  REQUIRE(std::ranges::all_of(fixture.partition,
                              [](idxtype block) { return block == 0; }));
  REQUIRE(edge_cut == 0);
  REQUIRE(MPI_Comm_free(&communicator) == MPI_SUCCESS);
}
