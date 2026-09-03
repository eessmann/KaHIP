#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "fixtures/cube_partition.h"

namespace {
using parhip::testing::cube_dimensions;
using parhip::testing::cube_graph;
using parhip::testing::evaluate_cube_partition;
using parhip::testing::read_text_partition;
}  // namespace

TEST_CASE("cube partition verifier recomputes block weights and cut",
          "[cube][partition][verifier]") {
  auto const graph = cube_graph{cube_dimensions{.nx = 2, .ny = 2, .nz = 1}};
  auto const partition = std::vector<std::uint64_t>{0, 0, 1, 1};

  auto const metrics = evaluate_cube_partition(graph, partition, 2, 0);

  REQUIRE(metrics.block_weights == std::vector<std::uint64_t>{2, 2});
  REQUIRE(metrics.maximum_block_weight == 2);
  REQUIRE(metrics.weighted_cut == 2);
}

TEST_CASE("cube partition verifier applies the exact percent balance bound",
          "[cube][partition][verifier]") {
  auto const graph = cube_graph{cube_dimensions{.nx = 2, .ny = 2, .nz = 1}};
  auto const partition = std::vector<std::uint64_t>{0, 0, 0, 1};

  REQUIRE_THROWS_AS(evaluate_cube_partition(graph, partition, 2, 0),
                    std::runtime_error);
  auto const metrics = evaluate_cube_partition(graph, partition, 2, 50);
  REQUIRE(metrics.maximum_block_weight == 3);
  REQUIRE(metrics.block_weights == std::vector<std::uint64_t>{3, 1});
}

TEST_CASE("cube partition verifier rejects missing, extra, and invalid blocks",
          "[cube][partition][verifier]") {
  auto const graph = cube_graph{cube_dimensions{.nx = 2, .ny = 2, .nz = 1}};

  REQUIRE_THROWS_AS(
      evaluate_cube_partition(graph, std::vector<std::uint64_t>{0, 1, 0}, 2, 0),
      std::invalid_argument);
  REQUIRE_THROWS_AS(evaluate_cube_partition(
                        graph, std::vector<std::uint64_t>{0, 1, 0, 1, 0}, 2, 0),
                    std::invalid_argument);
  REQUIRE_THROWS_AS(evaluate_cube_partition(
                        graph, std::vector<std::uint64_t>{0, 1, 2, 0}, 2, 50),
                    std::runtime_error);
}

TEST_CASE("text partition reader requires exactly one block per vertex",
          "[cube][partition][verifier]") {
  auto valid = std::istringstream{"0\n1\n 0  \n1\n"};
  REQUIRE(read_text_partition(valid, 4) ==
          std::vector<std::uint64_t>{0, 1, 0, 1});

  auto missing = std::istringstream{"0\n1\n0\n"};
  REQUIRE_THROWS_AS(read_text_partition(missing, 4), std::runtime_error);

  auto extra = std::istringstream{"0\n1\n0\n1\n0\n"};
  REQUIRE_THROWS_AS(read_text_partition(extra, 4), std::runtime_error);

  auto malformed = std::istringstream{"0\n1\nnot-a-block\n1\n"};
  REQUIRE_THROWS_AS(read_text_partition(malformed, 4), std::runtime_error);
}
