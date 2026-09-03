#pragma once

#include <cstddef>
#include <span>
#include <utility>

namespace parhip::detail {
template <typename Weight, std::size_t Extent>
[[nodiscard]] constexpr auto lowest_id_heaviest_block(
    std::span<Weight const, Extent> block_weights) noexcept
    -> std::pair<std::size_t, Weight> {
  auto heaviest_block = std::size_t{0};
  auto heaviest_weight = block_weights.front();
  for (auto block = std::size_t{1}; block < block_weights.size(); ++block) {
    if (block_weights[block] > heaviest_weight) {
      heaviest_block = block;
      heaviest_weight = block_weights[block];
    }
  }
  return {heaviest_block, heaviest_weight};
}
}  // namespace parhip::detail
