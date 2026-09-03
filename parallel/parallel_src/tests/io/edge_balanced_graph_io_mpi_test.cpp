#include <mpi.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "communication/mpi_handles.h"
#include "data_structure/parallel_graph_access.h"
#include "dspac/edge_balanced_graph_io.h"
#include "partition_config.h"

namespace {
using parhip::EdgeID;
using parhip::NodeID;
using parhip::ULONG;

[[nodiscard]] auto communicator_rank(MPI_Comm communicator) -> int {
  auto rank = -1;
  REQUIRE(MPI_Comm_rank(communicator, &rank) == MPI_SUCCESS);
  return rank;
}

[[nodiscard]] auto communicator_size(MPI_Comm communicator) -> int {
  auto size = 0;
  REQUIRE(MPI_Comm_size(communicator, &size) == MPI_SUCCESS);
  return size;
}

[[nodiscard]] auto shared_fixture_path(MPI_Comm communicator,
                                       std::string_view suffix)
    -> std::filesystem::path {
  auto path = std::string{};
  if (communicator_rank(communicator) == 0) {
    path = (std::filesystem::temp_directory_path() /
            ("kahip-edge-balanced-" + std::to_string(::getpid()) + "-" +
             std::string{suffix}))
               .string();
  }
  auto length = static_cast<std::uint64_t>(path.size());
  REQUIRE(MPI_Bcast(&length, 1, MPI_UINT64_T, 0, communicator) == MPI_SUCCESS);
  REQUIRE(length <=
          static_cast<std::uint64_t>(std::numeric_limits<int>::max()));
  path.resize(static_cast<std::size_t>(length));
  REQUIRE(MPI_Bcast(path.data(), static_cast<int>(length), MPI_CHAR, 0,
                    communicator) == MPI_SUCCESS);
  return path;
}

void write_binary_graph(MPI_Comm communicator,
                        std::filesystem::path const& path,
                        NodeID nodes,
                        EdgeID edges,
                        std::span<ULONG const> offsets,
                        std::span<ULONG const> adjacency) {
  auto success = 1;
  if (communicator_rank(communicator) == 0) {
    auto output = std::ofstream{path, std::ios::binary | std::ios::trunc};
    auto const header = std::array<ULONG, 3>{3, nodes, edges};
    output.write(reinterpret_cast<char const*>(header.data()),
                 static_cast<std::streamsize>(sizeof(header)));
    output.write(reinterpret_cast<char const*>(offsets.data()),
                 static_cast<std::streamsize>(offsets.size_bytes()));
    output.write(reinterpret_cast<char const*>(adjacency.data()),
                 static_cast<std::streamsize>(adjacency.size_bytes()));
    success = output ? 1 : 0;
  }
  REQUIRE(MPI_Bcast(&success, 1, MPI_INT, 0, communicator) == MPI_SUCCESS);
  REQUIRE(success == 1);
  REQUIRE(MPI_Barrier(communicator) == MPI_SUCCESS);
}

void remove_fixture(MPI_Comm communicator, std::filesystem::path const& path) {
  REQUIRE(MPI_Barrier(communicator) == MPI_SUCCESS);
  if (communicator_rank(communicator) == 0) {
    auto error = std::error_code{};
    static_cast<void>(std::filesystem::remove(path, error));
    REQUIRE_FALSE(error);
  }
  REQUIRE(MPI_Barrier(communicator) == MPI_SUCCESS);
}

void require_common(bool condition, MPI_Comm communicator) {
  auto const local = condition ? 1 : 0;
  auto global = 0;
  REQUIRE(MPI_Allreduce(&local, &global, 1, MPI_INT, MPI_MIN, communicator) ==
          MPI_SUCCESS);
  REQUIRE(global == 1);
}

[[nodiscard]] auto local_targets(parhip::parallel_graph_access& graph)
    -> std::vector<NodeID> {
  auto result = std::vector<NodeID>{};
  for (auto local = NodeID{0}; local < graph.number_of_local_nodes(); ++local) {
    for (auto edge = graph.get_first_edge(local);
         edge < graph.get_first_invalid_edge(local); ++edge) {
      result.push_back(graph.getGlobalID(graph.getEdgeTarget(edge)));
    }
  }
  return result;
}
}  // namespace

TEST_CASE("edge-balanced binary input supports empty and edgeless graphs",
          "[mpi][parallel-io][edge-balanced][zero-work]") {
  auto communicator = MPI_COMM_NULL;
  auto const world_rank = communicator_rank(MPI_COMM_WORLD);
  auto const world_size = communicator_size(MPI_COMM_WORLD);
  REQUIRE(world_size == 5);
  REQUIRE(MPI_Comm_split(MPI_COMM_WORLD, 0, world_size - world_rank,
                         &communicator) == MPI_SUCCESS);
  auto const rank = communicator_rank(communicator);

  auto config = parhip::PPartitionConfig{};
  config.binary_io_window_size = 2;

  auto const empty_path = shared_fixture_path(communicator, "empty.bgf");
  auto const empty_offsets = std::array<ULONG, 1>{32};
  write_binary_graph(communicator, empty_path, 0, 0, empty_offsets, {});
  {
    auto graph = parhip::parallel_graph_access{communicator};
    auto permutation = std::vector<EdgeID>{17};
    parhip::edge_balanced_graph_io::read_binary_graph_edge_balanced(
        graph, empty_path.string(), config, permutation,
        parhip::mpi::communicator_view{communicator});
    require_common(
        graph.number_of_local_nodes() == 0 &&
            graph.number_of_local_edges() == 0 && graph.get_from_range() == 0 &&
            graph.get_to_range() == 0 && permutation.empty() &&
            graph.get_range_array() == std::vector<NodeID>{0, 0, 0, 0, 0, 0} &&
            graph.get_edge_range_array() ==
                std::vector<EdgeID>{0, 0, 0, 0, 0, 0},
        communicator);
  }
  remove_fixture(communicator, empty_path);

  auto const edgeless_path = shared_fixture_path(communicator, "edgeless.bgf");
  auto const edgeless_offsets = std::array<ULONG, 4>{56, 56, 56, 56};
  write_binary_graph(communicator, edgeless_path, 3, 0, edgeless_offsets, {});
  {
    auto graph = parhip::parallel_graph_access{communicator};
    auto permutation = std::vector<EdgeID>{};
    parhip::edge_balanced_graph_io::read_binary_graph_edge_balanced(
        graph, edgeless_path.string(), config, permutation,
        parhip::mpi::communicator_view{communicator});
    auto const expected_ranges = std::vector<NodeID>{0, 1, 2, 3, 3, 3};
    auto const expected_nodes = rank < 3 ? NodeID{1} : NodeID{0};
    auto const expected_from = expected_ranges[static_cast<std::size_t>(rank)];
    require_common(graph.number_of_local_nodes() == expected_nodes &&
                       graph.number_of_local_edges() == 0 &&
                       graph.get_from_range() == expected_from &&
                       graph.get_to_range() == expected_from &&
                       permutation.empty() &&
                       graph.get_range_array() == expected_ranges &&
                       graph.get_edge_range_array() ==
                           std::vector<EdgeID>{0, 0, 0, 0, 0, 0},
                   communicator);
  }
  remove_fixture(communicator, edgeless_path);
  REQUIRE(MPI_Comm_free(&communicator) == MPI_SUCCESS);
}

TEST_CASE("edge-balanced binary input preserves exact reversed-subcomm order",
          "[mpi][parallel-io][edge-balanced][determinism]") {
  auto communicator = MPI_COMM_NULL;
  auto const world_rank = communicator_rank(MPI_COMM_WORLD);
  auto const world_size = communicator_size(MPI_COMM_WORLD);
  REQUIRE(world_size == 5);
  REQUIRE(MPI_Comm_split(MPI_COMM_WORLD, 0, world_size - world_rank,
                         &communicator) == MPI_SUCCESS);
  auto const rank = communicator_rank(communicator);

  auto const path = shared_fixture_path(communicator, "three-vertices.bgf");
  auto const offsets = std::array<ULONG, 4>{56, 72, 80, 88};
  auto const adjacency = std::array<ULONG, 4>{2, 1, 0, 1};
  write_binary_graph(communicator, path, 3, 4, offsets, adjacency);

  {
    auto graph = parhip::parallel_graph_access{communicator};
    auto config = parhip::PPartitionConfig{};
    config.binary_io_window_size = 2;
    auto permutation = std::vector<EdgeID>{};
    parhip::edge_balanced_graph_io::read_binary_graph_edge_balanced(
        graph, path.string(), config, permutation,
        parhip::mpi::communicator_view{communicator});

    auto const expected_ranges = std::vector<NodeID>{0, 1, 1, 2, 3, 3};
    auto const expected_edge_ranges = std::vector<EdgeID>{0, 2, 2, 3, 4, 4};
    auto const expected_nodes = std::array<NodeID, 5>{1, 0, 1, 1, 0};
    auto const expected_edges = std::array<EdgeID, 5>{2, 0, 1, 1, 0};
    auto const expected_targets =
        std::array<std::vector<NodeID>, 5>{std::vector<NodeID>{1, 2},
                                           {},
                                           std::vector<NodeID>{0},
                                           std::vector<NodeID>{1},
                                           {}};
    auto const expected_permutations =
        std::array<std::vector<EdgeID>, 5>{std::vector<EdgeID>{1, 0},
                                           {},
                                           std::vector<EdgeID>{0},
                                           std::vector<EdgeID>{0},
                                           {}};
    require_common(
        graph.number_of_local_nodes() ==
                expected_nodes[static_cast<std::size_t>(rank)] &&
            graph.number_of_local_edges() ==
                expected_edges[static_cast<std::size_t>(rank)] &&
            graph.get_range_array() == expected_ranges &&
            graph.get_edge_range_array() == expected_edge_ranges &&
            local_targets(graph) ==
                expected_targets[static_cast<std::size_t>(rank)] &&
            permutation ==
                expected_permutations[static_cast<std::size_t>(rank)],
        communicator);
  }
  remove_fixture(communicator, path);
  REQUIRE(MPI_Comm_free(&communicator) == MPI_SUCCESS);
}
