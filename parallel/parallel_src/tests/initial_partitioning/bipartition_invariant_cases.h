#pragma once

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <functional>
#include <numeric>
#include <span>
#include <utility>
#include <vector>

namespace kahip::test {

enum class bipartition_growth { bfs, fm };

struct growth_result final {
  std::vector<unsigned> labels;
  std::array<unsigned, 2> node_counts{};
  std::array<unsigned, 2> block_weights{};

  friend auto operator==(growth_result const&, growth_result const&)
      -> bool = default;
};

template <typename Adapter>
[[nodiscard]] auto run_growth(bipartition_growth algorithm,
                              std::span<unsigned const> weights,
                              std::span<std::vector<unsigned> const> adjacency,
                              unsigned grow_target) -> growth_result {
  REQUIRE(weights.size() == adjacency.size());

  auto graph = typename Adapter::graph_type{};
  auto const edge_count = std::transform_reduce(
      adjacency.begin(), adjacency.end(), std::size_t{0}, std::plus<>{},
      [](auto const& neighbors) { return neighbors.size(); });
  graph.start_construction(static_cast<unsigned>(weights.size()),
                           static_cast<unsigned>(edge_count));
  for (auto source = std::size_t{0}; source < weights.size(); ++source) {
    auto const node = graph.new_node();
    REQUIRE(node == source);
    graph.setNodeWeight(node, weights[source]);
    for (auto const target : adjacency[source]) {
      auto const edge = graph.new_edge(node, target);
      graph.setEdgeWeight(edge, 1);
    }
  }
  graph.finish_construction();

  auto config = typename Adapter::config_type{};
  Adapter::configure(config, algorithm, grow_target);
  Adapter::reset_seed(0);
  auto partitioner = typename Adapter::partitioner_type{};
  Adapter::grow(partitioner, algorithm, config, graph);

  auto result = growth_result{};
  result.labels.reserve(weights.size());
  for (auto node = std::size_t{0}; node < weights.size(); ++node) {
    auto const block = graph.getPartitionIndex(static_cast<unsigned>(node));
    REQUIRE(block < 2);
    result.labels.push_back(block);
    ++result.node_counts[block];
    result.block_weights[block] += weights[node];
  }
  return result;
}

template <typename Adapter>
[[nodiscard]] auto require_deterministic_growth(
    bipartition_growth algorithm,
    std::span<unsigned const> weights,
    std::span<std::vector<unsigned> const> adjacency,
    unsigned grow_target) -> growth_result {
  auto const first =
      run_growth<Adapter>(algorithm, weights, adjacency, grow_target);
  auto const second =
      run_growth<Adapter>(algorithm, weights, adjacency, grow_target);
  REQUIRE(second == first);
  return first;
}

template <typename Adapter>
void require_singleton_rhs(bipartition_growth algorithm) {
  auto const weights = std::array{5U};
  auto const adjacency = std::array{std::vector<unsigned>{}};

  auto const result =
      require_deterministic_growth<Adapter>(algorithm, weights, adjacency, 5U);

  REQUIRE(result.labels == std::vector<unsigned>{1U});
  REQUIRE(result.node_counts == std::array{0U, 1U});
  REQUIRE(result.block_weights == std::array{0U, 5U});
}

template <typename Adapter>
void require_weighted_pair_exact_target(bipartition_growth algorithm) {
  auto const weights = std::array{7U, 7U};
  auto const adjacency =
      std::array{std::vector<unsigned>{1U}, std::vector<unsigned>{0U}};

  auto const result =
      require_deterministic_growth<Adapter>(algorithm, weights, adjacency, 7U);

  REQUIRE(result.node_counts == std::array{1U, 1U});
  REQUIRE(result.block_weights == std::array{7U, 7U});
}

template <typename Adapter>
void require_two_rhs_vertices_at_target(bipartition_growth algorithm) {
  auto const weights = std::array{1U, 1U, 1U, 1U, 1U};
  auto const adjacency =
      std::array{std::vector<unsigned>{1U}, std::vector<unsigned>{0U, 2U},
                 std::vector<unsigned>{1U, 3U}, std::vector<unsigned>{2U, 4U},
                 std::vector<unsigned>{3U}};

  auto const result =
      require_deterministic_growth<Adapter>(algorithm, weights, adjacency, 3U);

  REQUIRE(result.node_counts == std::array{3U, 2U});
  REQUIRE(result.block_weights == std::array{3U, 2U});
}

template <typename Adapter>
void require_disconnected_restart_reaches_target(bipartition_growth algorithm) {
  auto const weights = std::array{1U, 1U, 1U, 1U};
  auto const adjacency =
      std::array{std::vector<unsigned>{1U}, std::vector<unsigned>{0U},
                 std::vector<unsigned>{3U}, std::vector<unsigned>{2U}};

  auto const result =
      require_deterministic_growth<Adapter>(algorithm, weights, adjacency, 3U);

  REQUIRE(result.node_counts == std::array{3U, 1U});
  REQUIRE(result.block_weights == std::array{3U, 1U});
}

}  // namespace kahip::test
