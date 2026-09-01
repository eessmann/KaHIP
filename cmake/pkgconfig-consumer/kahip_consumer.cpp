#include <kaHIP_interface.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <ranges>

auto main() -> int {
  if (kahip_sizeof_idx() != static_cast<int>(sizeof(kahip_idx))) {
    return 1;
  }

  auto vertex_count = 4;
  auto offsets = std::array<kahip_idx, 5>{0, 2, 4, 6, 8};
  auto neighbors = std::array<kahip_idx, 8>{1, 3, 0, 2, 1, 3, 0, 2};
  auto blocks = 2;
  auto imbalance = 0.03;
  auto edge_cut = kahip_idx{-1};
  auto partition = std::array<int, 4>{};

  kaffpa(&vertex_count, nullptr, offsets.data(), nullptr, neighbors.data(),
         &blocks, &imbalance, true, 1, FAST, &edge_cut, partition.data());

  auto block_weights = std::array<int, 2>{};
  for (auto const block : partition) {
    if (block < 0 || block >= blocks) {
      return 1;
    }
    ++block_weights[static_cast<std::size_t>(block)];
  }
  auto const recomputed_cut =
      static_cast<kahip_idx>(std::ranges::count_if(
          std::views::iota(std::size_t{0}, partition.size()),
          [&](std::size_t vertex) {
            return partition[vertex] !=
                   partition[(vertex + 1) % partition.size()];
          }));
  return std::ranges::all_of(block_weights,
                             [](int weight) { return weight <= 2; }) &&
                 edge_cut == recomputed_cut
             ? 0
             : 1;
}
