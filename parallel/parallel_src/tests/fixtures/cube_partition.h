#pragma once

#include <cstdint>
#include <iosfwd>
#include <span>
#include <vector>

#include "fixtures/cube_graph.h"

namespace parhip::testing {
struct cube_partition_metrics final {
  std::vector<std::uint64_t> block_weights;
  std::uint64_t maximum_block_weight;
  std::uint64_t weighted_cut;
};

[[nodiscard]] auto evaluate_cube_partition(
    cube_graph const& graph,
    std::span<std::uint64_t const> partition,
    std::uint64_t block_count,
    std::uint64_t imbalance_percent) -> cube_partition_metrics;

[[nodiscard]] auto read_text_partition(std::istream& input,
                                       std::uint64_t expected_vertices)
    -> std::vector<std::uint64_t>;
}  // namespace parhip::testing
