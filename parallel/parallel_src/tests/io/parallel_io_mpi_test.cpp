#include <mpi.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "data_structure/parallel_graph_access.h"
#include "io/parallel_graph_io.h"
#include "io/parallel_vector_io.h"
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
  auto const rank = communicator_rank(communicator);
  auto path = std::string{};
  if (rank == 0) {
    path = (std::filesystem::temp_directory_path() /
            ("kahip-parallel-io-" + std::to_string(::getpid()) + "-" +
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

template <typename Writer>
void write_fixture(MPI_Comm communicator,
                   std::filesystem::path const& path,
                   Writer&& writer) {
  auto const rank = communicator_rank(communicator);
  auto success = 1;
  if (rank == 0) {
    try {
      std::invoke(std::forward<Writer>(writer), path);
    } catch (...) {
      success = 0;
    }
  }
  REQUIRE(MPI_Bcast(&success, 1, MPI_INT, 0, communicator) == MPI_SUCCESS);
  REQUIRE(success == 1);
  REQUIRE(MPI_Barrier(communicator) == MPI_SUCCESS);
}

void remove_fixture(MPI_Comm communicator, std::filesystem::path const& path) {
  REQUIRE(MPI_Barrier(communicator) == MPI_SUCCESS);
  if (communicator_rank(communicator) == 0) {
    std::error_code error;
    static_cast<void>(std::filesystem::remove(path, error));
    REQUIRE_FALSE(error);
  }
  REQUIRE(MPI_Barrier(communicator) == MPI_SUCCESS);
}

void write_binary_graph(std::filesystem::path const& path,
                        NodeID nodes,
                        EdgeID edges,
                        std::span<ULONG const> offsets,
                        std::span<ULONG const> adjacency) {
  auto output = std::ofstream{path, std::ios::binary | std::ios::trunc};
  if (!output) {
    throw std::ios_base::failure{"unable to create binary graph fixture"};
  }
  auto const header = std::array<ULONG, 3>{3, nodes, edges};
  output.write(reinterpret_cast<char const*>(header.data()),
               static_cast<std::streamsize>(sizeof(header)));
  output.write(reinterpret_cast<char const*>(offsets.data()),
               static_cast<std::streamsize>(offsets.size_bytes()));
  if (!adjacency.empty()) {
    output.write(reinterpret_cast<char const*>(adjacency.data()),
                 static_cast<std::streamsize>(adjacency.size_bytes()));
  }
  if (!output) {
    throw std::ios_base::failure{"unable to write binary graph fixture"};
  }
}

void build_label_graph(parhip::parallel_graph_access& graph,
                       MPI_Comm communicator,
                       bool empty) {
  auto const rank = communicator_rank(communicator);
  auto const size = communicator_size(communicator);
  auto const global_nodes = empty ? NodeID{0} : NodeID{3};
  auto const local_nodes = empty       ? NodeID{0}
                           : size == 1 ? global_nodes
                           : rank == 1 ? global_nodes
                                       : NodeID{0};
  auto const from = empty || size == 1 || rank <= 1 ? NodeID{0} : global_nodes;

  graph.start_construction(local_nodes, 0, global_nodes, 0, false);
  graph.set_range(from, local_nodes == 0 ? from : from + local_nodes - 1);
  auto ranges =
      std::vector<NodeID>(static_cast<std::size_t>(size) + 1, global_nodes);
  ranges.front() = 0;
  if (!empty && size > 1) {
    ranges[1] = 0;
  }
  graph.set_range_array(ranges);
  for (NodeID local = 0; local < local_nodes; ++local) {
    auto const node = graph.new_node();
    graph.setNodeWeight(node, 1);
    graph.setNodeLabel(node, 101 + from + local);
    graph.setSecondPartitionIndex(node, 0);
  }
  graph.finish_construction();
}

void require_common(bool condition, MPI_Comm communicator) {
  auto local = condition ? 1 : 0;
  auto global = 0;
  REQUIRE(MPI_Allreduce(&local, &global, 1, MPI_INT, MPI_MIN, communicator) ==
          MPI_SUCCESS);
  REQUIRE(global == 1);
}
}  // namespace

TEST_CASE("binary graph ranges clamp to n when ranks outnumber vertices",
          "[mpi][parallel-io][graph][binary][zero-work]") {
  auto communicator = MPI_COMM_NULL;
  auto const world_rank = communicator_rank(MPI_COMM_WORLD);
  auto const world_size = communicator_size(MPI_COMM_WORLD);
  REQUIRE(MPI_Comm_split(MPI_COMM_WORLD, 0, world_size - world_rank,
                         &communicator) == MPI_SUCCESS);
  auto const rank = communicator_rank(communicator);
  auto const size = communicator_size(communicator);
  auto const path = shared_fixture_path(communicator, "small.bgf");
  write_fixture(communicator, path, [&](auto const& fixture) {
    auto const offsets = std::array<ULONG, 3>{48, 56, 64};
    auto const adjacency = std::array<ULONG, 2>{1, 0};
    write_binary_graph(fixture, 2, 2, offsets, adjacency);
  });

  {
    auto graph = parhip::parallel_graph_access{communicator};
    auto config = parhip::PPartitionConfig{};
    config.binary_io_window_size = 2;
    REQUIRE(parhip::parallel_graph_io::readGraphBinary(
                config, graph, path.string(), rank, size, communicator) == 0);

    auto const expected_from =
        size == 1 ? NodeID{0} : std::min<NodeID>(rank, NodeID{2});
    auto const expected_nodes = size == 1  ? NodeID{2}
                                : rank < 2 ? NodeID{1}
                                           : NodeID{0};
    auto const expected_to = expected_nodes == 0
                                 ? expected_from
                                 : expected_from + expected_nodes - 1;
    auto exact = graph.number_of_local_nodes() == expected_nodes &&
                 graph.get_from_range() == expected_from &&
                 graph.get_to_range() == expected_to &&
                 graph.get_range_array().back() == NodeID{2};
    for (auto boundary : graph.get_range_array()) {
      exact = exact && boundary <= NodeID{2};
    }
    for (NodeID local = 0; exact && local < expected_nodes; ++local) {
      auto const global = expected_from + local;
      auto const edge = graph.get_first_edge(local);
      exact =
          graph.getNodeDegree(local) == 1 &&
          graph.getGlobalID(graph.getEdgeTarget(edge)) == NodeID{1} - global &&
          graph.getEdgeWeight(edge) == 1;
    }
    require_common(exact, communicator);
  }
  remove_fixture(communicator, path);
  REQUIRE(MPI_Comm_free(&communicator) == MPI_SUCCESS);
}

TEST_CASE("empty text and binary graphs remain empty on every rank",
          "[mpi][parallel-io][graph][empty]") {
  auto const rank = communicator_rank(MPI_COMM_WORLD);
  auto const size = communicator_size(MPI_COMM_WORLD);
  auto const binary_path = shared_fixture_path(MPI_COMM_WORLD, "empty.bgf");
  auto const text_path = shared_fixture_path(MPI_COMM_WORLD, "empty.graph");
  write_fixture(MPI_COMM_WORLD, binary_path, [&](auto const& fixture) {
    auto const offsets = std::array<ULONG, 1>{32};
    write_binary_graph(fixture, 0, 0, offsets, {});
  });
  write_fixture(MPI_COMM_WORLD, text_path, [&](auto const& fixture) {
    auto output = std::ofstream{fixture, std::ios::trunc};
    output << "0 0\n";
    if (!output) {
      throw std::ios_base::failure{"unable to write empty text graph"};
    }
  });

  auto config = parhip::PPartitionConfig{};
  config.binary_io_window_size = 2;
  auto binary = parhip::parallel_graph_access{MPI_COMM_WORLD};
  REQUIRE(parhip::parallel_graph_io::readGraphBinary(
              config, binary, binary_path.string(), rank, size,
              MPI_COMM_WORLD) == 0);
  auto text = parhip::parallel_graph_access{MPI_COMM_WORLD};
  REQUIRE(parhip::parallel_graph_io::readGraphWeightedFlexible(
              text, text_path.string(), rank, size, MPI_COMM_WORLD) == 0);
  require_common(
      binary.number_of_local_nodes() == 0 &&
          binary.number_of_local_edges() == 0 && binary.get_from_range() == 0 &&
          binary.get_to_range() == 0 && binary.get_range_array().back() == 0 &&
          text.number_of_local_nodes() == 0 &&
          text.number_of_local_edges() == 0 && text.get_from_range() == 0 &&
          text.get_to_range() == 0 && text.get_range_array().back() == 0,
      MPI_COMM_WORLD);
  remove_fixture(MPI_COMM_WORLD, binary_path);
  remove_fixture(MPI_COMM_WORLD, text_path);
}

TEST_CASE("weighted METIS input preserves weights on uneven rank layouts",
          "[mpi][parallel-io][graph][weighted][zero-work]") {
  auto const rank = communicator_rank(MPI_COMM_WORLD);
  auto const size = communicator_size(MPI_COMM_WORLD);
  auto const path = shared_fixture_path(MPI_COMM_WORLD, "weighted.graph");
  write_fixture(MPI_COMM_WORLD, path, [&](auto const& fixture) {
    auto output = std::ofstream{fixture, std::ios::trunc};
    output << "4 3 11\n"
              "5 2 7\n"
              "6 1 7 3 9\n"
              "8 2 9 4 11\n"
              "10 3 11\n";
    if (!output) {
      throw std::ios_base::failure{"unable to write weighted text graph"};
    }
  });

  auto graph = parhip::parallel_graph_access{MPI_COMM_WORLD};
  REQUIRE(parhip::parallel_graph_io::readGraphWeightedFlexible(
              graph, path.string(), rank, size, MPI_COMM_WORLD) == 0);

  constexpr auto weights = std::array<ULONG, 4>{5, 6, 8, 10};
  constexpr auto targets = std::array<std::array<NodeID, 2>, 4>{
      std::array<NodeID, 2>{1, 0}, std::array<NodeID, 2>{0, 2},
      std::array<NodeID, 2>{1, 3}, std::array<NodeID, 2>{2, 0}};
  constexpr auto edge_weights = std::array<std::array<ULONG, 2>, 4>{
      std::array<ULONG, 2>{7, 0}, std::array<ULONG, 2>{7, 9},
      std::array<ULONG, 2>{9, 11}, std::array<ULONG, 2>{11, 0}};
  constexpr auto degrees = std::array<EdgeID, 4>{1, 2, 2, 1};

  auto exact = graph.number_of_global_nodes() == 4 &&
               graph.number_of_global_edges() == 6 &&
               graph.get_range_array().back() == 4;
  for (auto boundary : graph.get_range_array()) {
    exact = exact && boundary <= NodeID{4};
  }
  for (NodeID local = 0; exact && local < graph.number_of_local_nodes();
       ++local) {
    auto const global = graph.get_from_range() + local;
    exact = global < 4 && graph.getNodeWeight(local) == weights[global] &&
            graph.getNodeDegree(local) == degrees[global];
    auto edge = graph.get_first_edge(local);
    for (EdgeID offset = 0; exact && offset < degrees[global]; ++offset) {
      exact =
          graph.getGlobalID(graph.getEdgeTarget(edge + offset)) ==
              targets[global][offset] &&
          graph.getEdgeWeight(edge + offset) == edge_weights[global][offset];
    }
  }
  require_common(exact, MPI_COMM_WORLD);
  remove_fixture(MPI_COMM_WORLD, path);
}

TEST_CASE("empty binary partitions truncate stale data with a zero window",
          "[mpi][parallel-io][vector][empty][zero-window]") {
  auto const path = shared_fixture_path(MPI_COMM_WORLD, "empty.binp");
  write_fixture(MPI_COMM_WORLD, path, [&](auto const& fixture) {
    auto output = std::ofstream{fixture, std::ios::binary | std::ios::trunc};
    auto stale = std::array<std::byte, 128>{};
    output.write(reinterpret_cast<char const*>(stale.data()),
                 static_cast<std::streamsize>(stale.size()));
  });
  auto graph = parhip::parallel_graph_access{MPI_COMM_WORLD};
  build_label_graph(graph, MPI_COMM_WORLD, true);
  auto config = parhip::PPartitionConfig{};
  config.binary_io_window_size = 0;
  auto io = parhip::parallel_vector_io{};
  io.writePartitionBinaryParallelPosix(config, graph, path.string());
  io.readPartitionBinaryParallel(config, graph, path.string());

  auto exact_size = 0;
  if (communicator_rank(MPI_COMM_WORLD) == 0) {
    std::error_code error;
    exact_size =
        std::filesystem::file_size(path, error) == 2 * sizeof(ULONG) && !error
            ? 1
            : 0;
  }
  REQUIRE(MPI_Bcast(&exact_size, 1, MPI_INT, 0, MPI_COMM_WORLD) == MPI_SUCCESS);
  require_common(exact_size == 1 && graph.number_of_local_nodes() == 0,
                 MPI_COMM_WORLD);
  remove_fixture(MPI_COMM_WORLD, path);
}

TEST_CASE(
    "binary partition roundtrip supports leading and trailing zero-work "
    "ranks",
    "[mpi][parallel-io][vector][binary][zero-work]") {
  auto const path = shared_fixture_path(MPI_COMM_WORLD, "labels.binp");
  auto graph = parhip::parallel_graph_access{MPI_COMM_WORLD};
  build_label_graph(graph, MPI_COMM_WORLD, false);
  auto config = parhip::PPartitionConfig{};
  config.binary_io_window_size = 2;
  auto io = parhip::parallel_vector_io{};
  io.writePartitionBinaryParallelPosix(config, graph, path.string());
  for (NodeID node = 0; node < graph.number_of_local_nodes(); ++node) {
    graph.setNodeLabel(node, 0);
  }
  io.readPartitionBinaryParallel(config, graph, path.string());
  auto exact = true;
  for (NodeID node = 0; node < graph.number_of_local_nodes(); ++node) {
    exact = exact &&
            graph.getNodeLabel(node) == 101 + graph.get_from_range() + node;
  }
  require_common(exact, MPI_COMM_WORLD);
  remove_fixture(MPI_COMM_WORLD, path);
}

TEST_CASE("text partition ordering follows the graph communicator",
          "[mpi][parallel-io][vector][text][communicator]") {
  auto const world_rank = communicator_rank(MPI_COMM_WORLD);
  auto const world_size = communicator_size(MPI_COMM_WORLD);
  auto communicator = MPI_COMM_NULL;
  REQUIRE(MPI_Comm_split(MPI_COMM_WORLD, 0, world_size - world_rank,
                         &communicator) == MPI_SUCCESS);
  auto const rank = communicator_rank(communicator);
  auto const size = communicator_size(communicator);
  auto const path = shared_fixture_path(communicator, "labels.txtp");

  {
    auto graph = parhip::parallel_graph_access{communicator};
    graph.start_construction(1, 0, static_cast<NodeID>(size), 0, false);
    graph.set_range(static_cast<NodeID>(rank), static_cast<NodeID>(rank));
    auto ranges = std::vector<NodeID>(static_cast<std::size_t>(size) + 1);
    for (int pe = 0; pe <= size; ++pe) {
      ranges[static_cast<std::size_t>(pe)] = static_cast<NodeID>(pe);
    }
    graph.set_range_array(ranges);
    auto const node = graph.new_node();
    graph.setNodeWeight(node, 1);
    graph.setNodeLabel(node, 501 + static_cast<NodeID>(rank));
    graph.setSecondPartitionIndex(node, 0);
    graph.finish_construction();

    auto io = parhip::parallel_vector_io{};
    io.writePartitionSimpleParallel(graph, path.string());
    graph.setNodeLabel(0, 0);
    io.readPartitionSimpleParallel(graph, path.string());
    require_common(graph.getNodeLabel(0) == 501 + static_cast<NodeID>(rank),
                   communicator);
  }
  remove_fixture(communicator, path);
  REQUIRE(MPI_Comm_free(&communicator) == MPI_SUCCESS);
}
