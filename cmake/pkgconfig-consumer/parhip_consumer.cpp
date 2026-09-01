#include <parhip_interface.h>

#include <algorithm>
#include <array>

auto main(int argc, char** argv) -> int {
  if (MPI_Init(&argc, &argv) != MPI_SUCCESS) {
    return 1;
  }

  std::array<idxtype, 2> vertex_distribution{0, 4};
  std::array<idxtype, 5> edge_offsets{0, 2, 4, 6, 8};
  std::array<idxtype, 8> neighbors{1, 3, 0, 2, 1, 3, 0, 2};
  std::array<idxtype, 4> partition{};
  auto block_count = 2;
  auto imbalance = 0.03;
  auto edge_cut = -1;
  auto communicator = MPI_COMM_WORLD;

  ParHIPPartitionKWay(vertex_distribution.data(), edge_offsets.data(),
                      neighbors.data(), nullptr, nullptr, &block_count,
                      &imbalance, true, 1, FASTMESH, &edge_cut,
                      partition.data(), &communicator);

  auto const valid_partition = [](idxtype block) { return block < 2; };
  auto const partition_is_valid =
      edge_cut >= 0 && std::ranges::all_of(partition, valid_partition);
  auto const finalize_succeeded = MPI_Finalize() == MPI_SUCCESS;
  return partition_is_valid && finalize_succeeded ? 0 : 1;
}
