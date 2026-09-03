/******************************************************************************
 * edge_list_to_metis_graph.cpp
 * *
 * Source of KaHIP -- Karlsruhe High Quality Partitioning.
 * Christian Schulz <christian.schulz.phone@gmail.com>
 *****************************************************************************/

#include <mpi.h>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "application_math.h"
#include "communication/mpi_application.h"
#include "data_structure/hashed_graph.h"
#include "data_structure/parallel_graph_access.h"
#include "io/parallel_graph_io.h"

namespace {
namespace fs = std::filesystem;

template <typename Value>
[[nodiscard]] auto parse_number(std::string_view text) -> std::optional<Value> {
  auto value = Value{};
  auto const result =
      std::from_chars(text.data(), text.data() + text.size(), value);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size()
             ? std::optional<Value>{value}
             : std::nullopt;
}
}  // namespace

int main(int argument_count, char** argument_values) {
  using namespace parhip;
  mpi::application_runtime runtime{argument_count, argument_values,
                                   "edge-list converter executable"};
  return runtime.execute([&](mpi::communicator_view communicator) -> int {
    auto const rank = communicator.rank();
    if (argument_count != 2) {
      if (rank == ROOT) {
        std::cout << "usage: edge_list_to_metis inputfilename\n";
      }
      return EXIT_FAILURE;
    }
    if (rank != ROOT) {
      return EXIT_SUCCESS;
    }

    auto const graph_filename = fs::path{argument_values[1]};
    if (!fs::exists(graph_filename)) {
      std::cerr << "Error: File '" << graph_filename.string()
                << "' does not exist.\n";
      return EXIT_FAILURE;
    }
    auto input = std::ifstream{graph_filename};
    if (!input.is_open()) {
      std::cerr << "Error: Could not open file '" << graph_filename.string()
                << "'.\n";
      return EXIT_FAILURE;
    }

    std::cout << "Starting IO...\n";
    auto source_targets =
        std::unordered_map<NodeID, std::unordered_map<NodeID, EdgeID>>{};
    auto self_loops = EdgeID{0};
    auto line = std::string{};
    while (std::getline(input, line)) {
      if (line.empty()) {
        continue;
      }
      auto const line_view = std::string_view{line};
      auto const comma = line_view.find(',');
      if (comma == std::string_view::npos) {
        std::cerr << "Malformed line (missing comma): '" << line << "'\n";
        continue;
      }

      auto const source = parse_number<NodeID>(line_view.substr(0, comma));
      auto const target = parse_number<NodeID>(line_view.substr(comma + 1));
      if (!source.has_value() || !target.has_value()) {
        std::cerr << "Error parsing line '" << line
                  << "': invalid number format.\n";
        continue;
      }
      if (*source == *target) {
        if (!application::checked_add(self_loops, EdgeID{1})) {
          mpi::abort_on_capacity_failure(
              communicator.native_handle(), "edge-list converter executable",
              "self-loop count exceeds the edge domain");
        }
        continue;
      }
      auto& forward = source_targets[*source][*target];
      auto& reverse = source_targets[*target][*source];
      if (!application::checked_add(forward, EdgeID{1}) ||
          !application::checked_add(reverse, EdgeID{1})) {
        mpi::abort_on_capacity_failure(
            communicator.native_handle(), "edge-list converter executable",
            "parallel-edge multiplicity exceeds the edge domain");
      }
    }
    std::cout << "Self-loops detected: " << self_loops << "\nIO completed.\n";

    auto node_ids = std::vector<NodeID>{};
    node_ids.reserve(source_targets.size());
    std::ranges::transform(source_targets, std::back_inserter(node_ids),
                           [](auto const& entry) { return entry.first; });
    std::ranges::sort(node_ids);

    auto node_mapping = std::unordered_map<NodeID, NodeID>{};
    node_mapping.reserve(node_ids.size());
    for (auto const index :
         std::views::iota(std::size_t{0}, node_ids.size())) {
      node_mapping.emplace(node_ids[index], static_cast<NodeID>(index));
    }

    auto edge_count = EdgeID{0};
    for (auto const& [node_id, targets] : source_targets) {
      static_cast<void>(node_id);
      if (!std::in_range<EdgeID>(targets.size()) ||
          !application::checked_add(
              edge_count, static_cast<EdgeID>(targets.size()))) {
        mpi::abort_on_capacity_failure(
            communicator.native_handle(), "edge-list converter executable",
            "converted edge count exceeds the edge domain");
      }
    }
    if (!std::in_range<NodeID>(node_ids.size())) {
      mpi::abort_on_capacity_failure(
          communicator.native_handle(), "edge-list converter executable",
          "converted node count exceeds the node domain");
    }
    auto const node_count = static_cast<NodeID>(node_ids.size());

    std::cout << "Starting graph construction...\n";
    auto graph = complete_graph_access{};
    graph.start_construction(node_count, edge_count, node_count, edge_count);
    graph.set_range(0, node_count);
    auto total_edge_weight = EdgeID{0};
    for (auto const node_id : node_ids) {
      auto const new_node = graph.new_node();
      auto targets = std::vector<std::pair<NodeID, EdgeID>>{};
      targets.reserve(source_targets.at(node_id).size());
      std::ranges::copy(source_targets.at(node_id), std::back_inserter(targets));
      std::ranges::sort(targets, {}, &std::pair<NodeID, EdgeID>::first);
      for (auto const& [target_id, multiplicity] : targets) {
        graph.new_edge(new_node, node_mapping.at(target_id));
        if (!application::checked_add(total_edge_weight, multiplicity)) {
          mpi::abort_on_capacity_failure(
              communicator.native_handle(), "edge-list converter executable",
              "total edge weight exceeds the edge domain");
        }
      }
    }
    graph.finish_construction();

    std::cout << "Total edge weight: " << total_edge_weight << '\n';
    std::cout << "Adjusted edge count (accounting for self-loops): "
              << total_edge_weight / 2 + self_loops << '\n';
    auto output_filename = graph_filename;
    output_filename.replace_extension(".graph");
    auto const write_status = parallel_graph_io::writeGraphSequentially(
        graph, output_filename.string());
    if (write_status != 0) {
      std::cerr << "Error writing graph to '" << output_filename.string()
                << "'.\n";
      return EXIT_FAILURE;
    }
    std::cout << "Graph successfully written to '" << output_filename.string()
              << "'.\n";
    return EXIT_SUCCESS;
  });
}
