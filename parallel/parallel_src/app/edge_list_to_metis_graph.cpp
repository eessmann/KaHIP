/******************************************************************************
 * edge_list_to_metis_graph.cpp
 * *
 * Source of KaHIP -- Karlsruhe High Quality Partitioning.
 * Christian Schulz <christian.schulz.phone@gmail.com>
 *****************************************************************************/
#include <argtable3.h>
#include <mpi.h>
#include <charconv>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "data_structure/hashed_graph.h"
#include "data_structure/parallel_graph_access.h"
#include "io/parallel_graph_io.h"
#include "parse_parameters.h"
#include "partition_config.h"

namespace fs = std::filesystem;

int main(int argc, char** argv) {
  using namespace parhip;
  MPI_Init(&argc, &argv);

  int rank = 0;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);

  PPartitionConfig partition_config{};
  std::string graph_filename;

  if (argc != 2) {
    if (rank == ROOT) {
      std::cout << "usage: ";
      std::cout << "edge_list_to_metis inputfilename" << std::endl;
    }
    MPI_Finalize();
    return EXIT_FAILURE;
  }
  graph_filename = argv[1];

  if (!fs::exists(graph_filename)) {
    if (rank == 0) {
      for (int i = 0; i < argc; ++i) {
        std::cout << argv[i] << std::endl;
      }
      std::cerr << std::format("Error: File '{}' does not exist.\n",
                               graph_filename);
    }
    MPI_Finalize();
    return EXIT_FAILURE;
  }

  std::ifstream in_file(graph_filename);
  if (!in_file.is_open()) {
    if (rank == 0) {
      std::cerr << std::format("Error: Could not open file '{}'.\n",
                               graph_filename);
    }
    MPI_Finalize();
    return EXIT_FAILURE;
  }

  if (rank == 0) {
    std::cout << "Starting IO...\n";
  }

  std::unordered_map<NodeID, std::unordered_map<NodeID, int>> source_targets;
  EdgeID selfloops = 0;

  std::string line;
  while (std::getline(in_file, line)) {
    if (line.empty()) {
      continue;
    }

    std::string_view const line_view(line);
    auto comma_pos = line_view.find(',');

    if (comma_pos == std::string_view::npos) {
      if (rank == 0) {
        std::cerr << std::format("Malformed line (missing comma): '{}'\n",
                                 line);
      }
      continue;
    }

    std::string_view const source_str = line_view.substr(0, comma_pos);
    std::string_view const target_str = line_view.substr(comma_pos + 1);

    NodeID source = 0;
    NodeID target = 0;

    auto source_result = std::from_chars(
        source_str.data(), source_str.data() + source_str.size(), source);
    auto target_result = std::from_chars(
        target_str.data(), target_str.data() + target_str.size(), target);

    if (source_result.ec != std::errc{} || target_result.ec != std::errc{}) {
      if (rank == 0) {
        std::cerr << std::format(
            "Error parsing line '{}': invalid number format.\n", line);
      }
      continue;
    }

    if (source == target) {
      ++selfloops;
      continue;
    }

    ++source_targets[source][target];
    ++source_targets[target][source];
  }

  if (rank == 0) {
    std::cout << std::format("Self-loops detected: {}\n", selfloops);
    std::cout << "IO completed.\n";
  }

  // Map original node IDs to consecutive IDs
  std::unordered_map<NodeID, NodeID> node_id_mapping;
  NodeID consecutive_id = 0;

  for (auto const& [node_id, _] : source_targets) {
    node_id_mapping[node_id] = consecutive_id++;
  }

  EdgeID edge_counter = 0;
  for (auto const& [_, targets] : source_targets) {
    edge_counter += targets.size();
  }

  if (rank == 0) {
    std::cout << "Starting graph construction...\n";
  }

  complete_graph_access G;
  G.start_construction(consecutive_id, edge_counter, consecutive_id,
                       edge_counter);
  G.set_range(0, consecutive_id);

  EdgeID total_edge_weight = 0;

  for (auto const& [node_id, targets] : source_targets) {
    NodeID const new_node = G.new_node();

    for (auto const& [target_id, weight] : targets) {
      G.new_edge(new_node, node_id_mapping[target_id]);
      total_edge_weight += weight;
    }
  }

  G.finish_construction();

  if (rank == 0) {
    std::cout << std::format("Total edge weight: {}\n", total_edge_weight);
    std::cout << std::format(
        "Adjusted edge count (accounting for self-loops): {}\n",
        ((total_edge_weight / 2) + selfloops));
  }

  // Generate output filename by replacing the input file's extension with
  // ".graph"
  fs::path input_path(graph_filename);
  input_path.replace_extension(".graph");
  std::string output_filename = input_path.string();

  if (rank == 0) {
    int const write_status = parallel_graph_io::writeGraphSequentially(G, output_filename);
    if (write_status != 0) {
      std::cerr << std::format("Error writing graph to '{}'.\n",
                               output_filename);
      MPI_Finalize();
      return EXIT_FAILURE;
    } else {
      std::cout << std::format("Graph successfully written to '{}'.\n",
                               output_filename);
    }
  }

  MPI_Finalize();
  return EXIT_SUCCESS;
}
