#pragma once

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

#include "../../parallel/shared/random_state.h"

namespace kahip::parallel_mh {
template <std::unsigned_integral Weight, typename Graph>
[[nodiscard]] auto maximum_block_weight(Graph& graph)
    -> std::optional<Weight> {
  auto const raw_block_count = graph.get_partition_count();
  if (!std::in_range<std::size_t>(raw_block_count) || raw_block_count == 0) {
    return std::nullopt;
  }
  auto const block_count = static_cast<std::size_t>(raw_block_count);
  auto block_weights = std::vector<Weight>(block_count, Weight{0});

  using node_type = decltype(graph.number_of_nodes());
  for (auto node = node_type{0}; node < graph.number_of_nodes(); ++node) {
    auto const raw_block = graph.getPartitionIndex(node);
    if (!std::in_range<std::size_t>(raw_block) ||
        static_cast<std::size_t>(raw_block) >= block_count) {
      return std::nullopt;
    }
    auto const weight =
        random_compat::checked_narrow<Weight>(graph.getNodeWeight(node));
    if (!weight.has_value()) {
      return std::nullopt;
    }
    auto& block_weight = block_weights[static_cast<std::size_t>(raw_block)];
    if (!random_compat::checked_add(block_weight, *weight)) {
      return std::nullopt;
    }
  }

  return *std::ranges::max_element(block_weights);
}
}  // namespace kahip::parallel_mh
