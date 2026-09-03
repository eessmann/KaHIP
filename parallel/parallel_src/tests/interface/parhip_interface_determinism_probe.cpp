#include <mpi.h>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <utility>
#include <vector>

#include "fixtures/cube_graph.h"
#include "parhip_interface.h"

namespace {
struct local_graph final {
  std::vector<idxtype> distribution;
  std::vector<idxtype> offsets;
  std::vector<idxtype> neighbors;
};

[[nodiscard]] auto make_cube(int rank, int size) -> local_graph {
  auto const cube = parhip::testing::cube_graph{{10, 10, 10}};
  auto graph = local_graph{};
  graph.distribution.resize(static_cast<std::size_t>(size) + 1);
  for (auto index = 0; index <= size; ++index) {
    graph.distribution[static_cast<std::size_t>(index)] =
        static_cast<idxtype>(index) * cube.vertex_count() /
        static_cast<idxtype>(size);
  }

  auto const first = graph.distribution[static_cast<std::size_t>(rank)];
  auto const last = graph.distribution[static_cast<std::size_t>(rank) + 1];
  graph.offsets.reserve(static_cast<std::size_t>(last - first) + 1);
  graph.offsets.push_back(0);
  for (auto vertex = first; vertex < last; ++vertex) {
    auto const adjacent = cube.neighbors(vertex);
    graph.neighbors.insert(graph.neighbors.end(), adjacent.begin(),
                           adjacent.end());
    graph.offsets.push_back(static_cast<idxtype>(graph.neighbors.size()));
  }
  return graph;
}

struct partition_result final {
  int edge_cut;
  std::vector<idxtype> global_partition;
};

[[nodiscard]] auto partition_cube(local_graph& graph,
                                  int rank,
                                  int size,
                                  int seed,
                                  int mode,
                                  MPI_Comm communicator) -> partition_result {
  auto const local_count =
      graph.distribution[static_cast<std::size_t>(rank) + 1] -
      graph.distribution[static_cast<std::size_t>(rank)];
  auto local_partition =
      std::vector<idxtype>(static_cast<std::size_t>(local_count));
  auto blocks = 4;
  auto imbalance = 0.03;
  auto edge_cut = -1;
  auto mutable_communicator = communicator;
  ParHIPPartitionKWay(graph.distribution.data(), graph.offsets.data(),
                      graph.neighbors.data(), nullptr, nullptr, &blocks,
                      &imbalance, true, seed, mode, &edge_cut,
                      local_partition.data(), &mutable_communicator);

  auto local_valid =
      edge_cut >= 0 && std::ranges::all_of(local_partition, [](idxtype block) {
        return block < idxtype{4};
      });
  auto all_valid = 0;
  auto const encoded_valid = local_valid ? 1 : 0;
  if (MPI_Allreduce(&encoded_valid, &all_valid, 1, MPI_INT, MPI_MIN,
                    communicator) != MPI_SUCCESS ||
      all_valid == 0) {
    std::_Exit(11);
  }

  auto counts = std::vector<int>(static_cast<std::size_t>(size));
  auto displacements = std::vector<int>(static_cast<std::size_t>(size));
  for (auto index = 0; index < size; ++index) {
    auto const begin = graph.distribution[static_cast<std::size_t>(index)];
    auto const end = graph.distribution[static_cast<std::size_t>(index) + 1];
    if (!std::in_range<int>(begin) || !std::in_range<int>(end - begin)) {
      std::_Exit(12);
    }
    counts[static_cast<std::size_t>(index)] = static_cast<int>(end - begin);
    displacements[static_cast<std::size_t>(index)] = static_cast<int>(begin);
  }

  auto global_partition =
      std::vector<idxtype>(static_cast<std::size_t>(graph.distribution.back()));
  if (MPI_Allgatherv(local_partition.data(), static_cast<int>(local_count),
                     MPI_UNSIGNED_LONG_LONG, global_partition.data(),
                     counts.data(), displacements.data(),
                     MPI_UNSIGNED_LONG_LONG, communicator) != MPI_SUCCESS) {
    std::_Exit(13);
  }
  return {.edge_cut = edge_cut,
          .global_partition = std::move(global_partition)};
}

void print_target(partition_result const& result) {
  std::cout << "PARHIP_TARGET " << result.edge_cut;
  for (auto const block : result.global_partition) {
    std::cout << ' ' << block;
  }
  std::cout << '\n';
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
      world_size != 2) {
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

  auto graph = make_cube(rank, size);
  auto const selected = std::string_view{argv[1]};
  if (selected == "fresh") {
    auto const target =
        partition_cube(graph, rank, size, 1, FASTMESH, communicator);
    if (rank == 0)
      print_target(target);
  } else if (selected == "contaminated") {
    static_cast<void>(
        partition_cube(graph, rank, size, 7919, ULTRAFASTMESH, communicator));
    auto const first_target =
        partition_cube(graph, rank, size, 1, FASTMESH, communicator);
    auto const second_target =
        partition_cube(graph, rank, size, 1, FASTMESH, communicator);
    if (rank == 0) {
      print_target(first_target);
      print_target(second_target);
    }
  } else if (selected == "wrapped") {
    constexpr auto wrapped_seed = 536870912;
    auto const first_target = partition_cube(
        graph, rank, size, wrapped_seed, FASTMESH, communicator);
    auto const second_target = partition_cube(
        graph, rank, size, wrapped_seed, FASTMESH, communicator);
    if (rank == 0) {
      print_target(first_target);
      print_target(second_target);
    }
  } else {
    return 6;
  }

  if (MPI_Comm_free(&communicator) != MPI_SUCCESS ||
      MPI_Finalize() != MPI_SUCCESS) {
    return 7;
  }
  return 0;
}
