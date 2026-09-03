#include "fixtures/cube_partition.h"

#include <algorithm>
#include <cstddef>
#include <istream>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <string>
#include <utility>

namespace parhip::testing {
namespace {
[[nodiscard]] auto checked_add(std::uint64_t left,
                               std::uint64_t right,
                               char const* context) -> std::uint64_t {
  if (right > std::numeric_limits<std::uint64_t>::max() - left) {
    throw std::overflow_error{context};
  }
  return left + right;
}

[[nodiscard]] auto checked_multiply(std::uint64_t left,
                                    std::uint64_t right,
                                    char const* context) -> std::uint64_t {
  if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left) {
    throw std::overflow_error{context};
  }
  return left * right;
}
}  // namespace

auto evaluate_cube_partition(cube_graph const& graph,
                             std::span<std::uint64_t const> partition,
                             std::uint64_t block_count,
                             std::uint64_t imbalance_percent)
    -> cube_partition_metrics {
  if (block_count == 0 || !std::in_range<std::size_t>(graph.vertex_count()) ||
      !std::in_range<std::size_t>(block_count)) {
    throw std::invalid_argument{"partition dimensions are not representable"};
  }
  if (!std::cmp_equal(partition.size(), graph.vertex_count())) {
    throw std::invalid_argument{
        "partition must contain exactly one block per cube vertex"};
  }

  auto metrics = cube_partition_metrics{
      .block_weights =
          std::vector<std::uint64_t>(static_cast<std::size_t>(block_count), 0),
      .maximum_block_weight = 0,
      .weighted_cut = 0,
  };
  for (auto const block : partition) {
    if (block >= block_count) {
      throw std::runtime_error{"partition contains an invalid block"};
    }
    ++metrics.block_weights[static_cast<std::size_t>(block)];
  }

  auto const ideal_block_weight =
      graph.vertex_count() / block_count +
      static_cast<std::uint64_t>(graph.vertex_count() % block_count != 0);
  auto const percent_scale =
      checked_add(100, imbalance_percent, "partition imbalance scale overflow");
  metrics.maximum_block_weight =
      checked_multiply(ideal_block_weight, percent_scale,
                       "partition balance bound overflow") /
      100;
  if (std::ranges::any_of(metrics.block_weights, [&](auto weight) {
        return weight > metrics.maximum_block_weight;
      })) {
    throw std::runtime_error{"partition violates the block-weight bound"};
  }

  for (auto vertex = cube_graph::vertex_id{0}; vertex < graph.vertex_count();
       ++vertex) {
    auto const source_block = partition[static_cast<std::size_t>(vertex)];
    for (auto const neighbor : graph.neighbors(vertex)) {
      if (neighbor > vertex &&
          partition[static_cast<std::size_t>(neighbor)] != source_block) {
        ++metrics.weighted_cut;
      }
    }
  }
  return metrics;
}

auto read_text_partition(std::istream& input, std::uint64_t expected_vertices)
    -> std::vector<std::uint64_t> {
  if (!std::in_range<std::size_t>(expected_vertices)) {
    throw std::overflow_error{"partition length is not locally representable"};
  }

  auto partition = std::vector<std::uint64_t>{};
  partition.reserve(static_cast<std::size_t>(expected_vertices));
  for (auto index = std::uint64_t{0}; index < expected_vertices; ++index) {
    auto block = std::uint64_t{};
    if (!(input >> block)) {
      throw std::runtime_error{"partition is missing or has a malformed block"};
    }
    partition.push_back(block);
  }

  auto trailing = std::string{};
  if (input >> trailing) {
    throw std::runtime_error{"partition has more blocks than vertices"};
  }
  if (!input.eof()) {
    throw std::runtime_error{"partition has malformed trailing data"};
  }
  return partition;
}
}  // namespace parhip::testing
