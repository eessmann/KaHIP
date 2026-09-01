#include <cstdint>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "fixtures/cube_graph.h"

namespace {
using parhip::testing::cube_dimensions;
using parhip::testing::cube_graph;

[[nodiscard]] auto neighbor_ids(cube_graph const& graph, std::uint64_t vertex) {
  auto const neighbors = graph.neighbors(vertex);
  return std::vector<std::uint64_t>{neighbors.begin(), neighbors.end()};
}
}  // namespace

TEST_CASE("cube graph uses the documented linear cell identifier",
          "[cube][fixture]") {
  auto const graph = cube_graph{cube_dimensions{.nx = 3, .ny = 2, .nz = 2}};

  REQUIRE(graph.cell_id(0, 0, 0) == 0);
  REQUIRE(graph.cell_id(2, 0, 0) == 2);
  REQUIRE(graph.cell_id(0, 1, 0) == 3);
  REQUIRE(graph.cell_id(1, 1, 1) == 10);
  REQUIRE(graph.cell_id(2, 1, 1) == 11);
}

TEST_CASE("cube graph adjacency is the sorted six-face neighborhood",
          "[cube][fixture]") {
  auto const graph = cube_graph{cube_dimensions{.nx = 3, .ny = 2, .nz = 2}};

  REQUIRE(neighbor_ids(graph, 0) == std::vector<std::uint64_t>{1, 3, 6});
  REQUIRE(neighbor_ids(graph, 4) == std::vector<std::uint64_t>{1, 3, 5, 10});
  REQUIRE(neighbor_ids(graph, 11) == std::vector<std::uint64_t>{5, 8, 10});
}

TEST_CASE("cube graph counts match the CI and large debugging fixtures",
          "[cube][fixture]") {
  auto const four = cube_graph{cube_dimensions{.nx = 4, .ny = 4, .nz = 4}};
  auto const ten = cube_graph{cube_dimensions{.nx = 10, .ny = 10, .nz = 10}};
  auto const hundred =
      cube_graph{cube_dimensions{.nx = 100, .ny = 100, .nz = 100}};

  REQUIRE(four.vertex_count() == 64);
  REQUIRE(four.undirected_edge_count() == 144);
  REQUIRE(ten.vertex_count() == 1'000);
  REQUIRE(ten.undirected_edge_count() == 2'700);
  REQUIRE(hundred.vertex_count() == 1'000'000);
  REQUIRE(hundred.undirected_edge_count() == 2'970'000);
}

TEST_CASE("cube graph streams canonical one-based METIS adjacency",
          "[cube][fixture]") {
  auto const graph = cube_graph{cube_dimensions{.nx = 2, .ny = 1, .nz = 1}};
  auto output = std::ostringstream{};

  graph.write_metis(output);

  REQUIRE(output.str() == "2 1\n2\n1\n");
}

TEST_CASE("cube dimensions reject empty and unrepresentable graphs",
          "[cube][fixture]") {
  REQUIRE_THROWS_AS(cube_graph(cube_dimensions{.nx = 0, .ny = 1, .nz = 1}),
                    std::invalid_argument);
  REQUIRE_THROWS_AS(cube_graph(cube_dimensions{
                        .nx = std::numeric_limits<std::uint64_t>::max(),
                        .ny = 2,
                        .nz = 1,
                    }),
                    std::overflow_error);
}
