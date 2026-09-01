/******************************************************************************
 * dspac.cpp
 * *
 * Source of KaHIP -- Karlsruhe High Quality Partitioning.
 * Author: Daniel Seemaier <daniel.seemaier@student.kit.edu>
 * Christian Schulz <christian.schulz.phone@gmail.com>
 *****************************************************************************/

#include <argtable3.h>

#include <array>
#include <cstdlib>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "application_math.h"
#include "communication/dummy_operations.h"
#include "communication/mpi_application.h"
#include "communication/mpi_fixed_reduction.h"
#include "data_structure/parallel_graph_access.h"
#include "distributed_partitioning/distributed_partitioner.h"
#include "dspac/dspac.h"
#include "dspac/edge_balanced_graph_io.h"
#include "io/parallel_vector_io.h"
#include "macros_assertions.h"
#include "parse_dspac_parameters.h"
#include "partition_config.h"
#include "random_functions.h"
#include "timer.h"
#include "tools/distributed_quality_metrics.h"

namespace parhip {
namespace {
void require_local_add(NodeWeight& accumulator,
                       NodeWeight value,
                       mpi::communicator_view communicator,
                       std::string_view diagnostic) {
  if (!application::checked_add(accumulator, value)) {
    mpi::abort_on_capacity_failure(communicator.native_handle(),
                                   "DSPAC executable", diagnostic);
  }
}

[[nodiscard]] auto checked_global_sum(NodeWeight local,
                                      mpi::communicator_view communicator,
                                      std::string_view diagnostic)
    -> NodeWeight {
  auto const local_values = std::array<NodeWeight, 1>{local};
  auto global_values = std::array<NodeWeight, 1>{};
  mpi::all_reduce_checked_sum(
      std::span<NodeWeight const>{local_values},
      std::span<NodeWeight>{global_values}, communicator,
      "MPI_Allreduce(DSPAC application checked sum)", "DSPAC executable",
      diagnostic);
  return global_values.front();
}

[[nodiscard]] auto partition_upper_bound(NodeWeight total_weight,
                                         PPartitionConfig const& config,
                                         mpi::communicator_view communicator)
    -> NodeWeight {
  auto const result = application::exact_partition_upper_bound(
      total_weight, config.k, config.inbalance);
  if (!result.has_value()) {
    mpi::abort_on_capacity_failure(
        communicator.native_handle(), "DSPAC executable",
        "partition upper bound exceeds the graph-weight domain");
  }
  return *result;
}

[[nodiscard]] auto edge_fraction(EdgeWeight edge_count,
                                 EdgeWeight global_edge_count) noexcept
    -> double {
  return global_edge_count == 0
             ? 0.0
             : static_cast<double>(edge_count) /
                   static_cast<double>(global_edge_count);
}

void execute_parhip(parallel_graph_access& graph,
                    PPartitionConfig& config,
                    mpi::communicator_view communicator) {
  auto const rank = communicator.rank();
  auto const size = communicator.size();
  auto const native_communicator = communicator.native_handle();

  if (rank == ROOT) {
    PRINT(std::cout << "log> cluster coarsening factor is set to "
                    << config.cluster_coarsening_factor << '\n';)
  }

  config.stop_factor /= static_cast<int>(config.k);
  auto const seed = application::rank_seed(config.seed, size, rank);
  if (!seed.has_value()) {
    mpi::abort_on_programming_error(native_communicator,
                                    "invalid rank-specific PRNG seed input");
  }
  config.seed = *seed;
  std::srand(static_cast<unsigned int>(config.seed));
  random_functions::setSeed(config.seed);

  auto const process_count = static_cast<ULONG>(size);
  parallel_graph_access::set_comm_rounds(config.comm_rounds / process_count);
  parallel_graph_access::set_comm_rounds_up(config.comm_rounds /
                                            process_count);
  distributed_partitioner::generate_random_choices(config, communicator);
  graph.printMemoryUsage(std::cout);

  auto local_inter_edges = EdgeWeight{0};
  auto local_intra_edges = EdgeWeight{0};
  auto local_weight = NodeWeight{0};
  forall_local_nodes(graph, node) {
    require_local_add(local_weight, graph.getNodeWeight(node), communicator,
                      "local split-graph vertex-weight sum overflow");
    forall_out_edges(graph, edge, node) {
      auto const target = graph.getEdgeTarget(edge);
      auto& count = graph.is_local_node(target) ? local_intra_edges
                                                : local_inter_edges;
      require_local_add(count, EdgeWeight{1}, communicator,
                        "local split-graph edge-count overflow");
    }
    endfor
  }
  endfor

  auto const local_statistics =
      std::array<NodeWeight, 2>{local_inter_edges, local_intra_edges};
  auto global_statistics = std::array<NodeWeight, 2>{};
  mpi::all_reduce_checked_sum(
      std::span<NodeWeight const>{local_statistics},
      std::span<NodeWeight>{global_statistics}, communicator,
      "MPI_Allreduce(DSPAC application statistics)", "DSPAC executable",
      "global split-graph edge count exceeds the KaHIP weight domain");
  auto const [global_inter_edges, global_intra_edges] = global_statistics;
  if (rank == ROOT) {
    std::cout << "log> ghost edges "
              << edge_fraction(global_inter_edges,
                               graph.number_of_global_edges())
              << '\n';
    std::cout << "log> local edges "
              << edge_fraction(global_intra_edges,
                               graph.number_of_global_edges())
              << '\n';
  }

  if (config.vertex_degree_weights) {
    throw std::logic_error{"DSPAC cannot overwrite split-graph vertex weights"};
  }
  config.number_of_overall_nodes = graph.number_of_global_nodes();
  auto const global_weight = checked_global_sum(
      local_weight, communicator,
      "global split-graph vertex-weight sum exceeds the graph-weight domain");
  config.upper_bound_partition =
      partition_upper_bound(global_weight, config, communicator);

  auto clock = timer{};
  auto partitioner = distributed_partitioner{};
  partitioner.perform_partitioning(native_communicator, config, graph);
  mpi::check_or_abort(MPI_Barrier(native_communicator), native_communicator,
                      "MPI_Barrier(DSPAC partition completion)");

  auto const running_time = clock.elapsed();
  auto quality = distributed_quality_metrics{};
  auto const edge_cut = quality.edge_cut(graph, native_communicator);
  auto const balance = quality.balance(config, graph, native_communicator);
  PRINT(auto const balance_load =
            quality.balance_load(config, graph, native_communicator);)
  PRINT(auto const balance_load_dist =
            quality.balance_load_dist(config, graph, native_communicator);)

  if (rank == ROOT) {
    std::cout << "log>=====================================\n";
    std::cout << "log>============AND WE R DONE============\n";
    std::cout << "log>=====================================\n";
    std::cout << "log>total partitioning time elapsed " << running_time << '\n';
    std::cout << "log>final edge cut " << edge_cut << '\n';
    std::cout << "log>final balance " << balance << '\n';
    PRINT(std::cout << "log>final balance load " << balance_load << '\n';)
    PRINT(std::cout << "log>final balance load dist " << balance_load_dist
                    << '\n';)
  }
  PRINT(quality.comm_vol(config, graph, native_communicator);)
  PRINT(quality.comm_vol_dist(graph, native_communicator);)
}
}  // namespace
}  // namespace parhip

int main(int argument_count, char** argument_values) {
  using namespace parhip;
  mpi::application_runtime runtime{argument_count, argument_values,
                                   "DSPAC executable"};
  return runtime.execute([&](mpi::communicator_view communicator) -> int {
    auto const rank = communicator.rank();
    auto const size = communicator.size();
    auto const native_communicator = communicator.native_handle();

    auto partition_config = PPartitionConfig{};
    auto dspac_config = DspacConfig{};
    auto graph_filename = std::string{};
    auto partition_filename = std::string{};
    auto const parse_result = parse_dspac_parameters(
        argument_count, argument_values, partition_config, dspac_config,
        graph_filename, partition_filename, communicator);
    if (parse_result != parse_outcome::continue_execution) {
      return parse_result == parse_outcome::early_success ? EXIT_SUCCESS
                                                          : EXIT_FAILURE;
    }

    if (rank == ROOT) {
      std::cout << "graph: " << graph_filename << '\n'
                << "infinity edge weight: " << dspac_config.infinity << '\n'
                << "seed: " << partition_config.seed << '\n'
                << "k: " << partition_config.k << '\n'
                << "ncores: " << size << '\n';
    }

    auto clock = timer{};
    mpi::check_or_abort(MPI_Barrier(native_communicator), native_communicator,
                        "MPI_Barrier(before DSPAC warm-up)");
    clock.restart();
    if (rank == ROOT) {
      std::cout << "running collective dummy operations ";
    }
    auto warm_up = dummy_operations{};
    warm_up.run_collective_dummy_operations(communicator);
    mpi::check_or_abort(MPI_Barrier(native_communicator), native_communicator,
                        "MPI_Barrier(after DSPAC warm-up)");
    if (rank == ROOT) {
      std::cout << "took " << clock.elapsed() << '\n';
    }

    auto edge_permutation = std::vector<EdgeID>{};
    clock.restart();
    auto input_graph = parallel_graph_access{native_communicator};
    edge_balanced_graph_io::read_binary_graph_edge_balanced(
        input_graph, graph_filename, partition_config, edge_permutation,
        communicator);
    if (rank == ROOT) {
      std::cout << "input IO took " << clock.elapsed() << '\n'
                << "n(input): " << input_graph.number_of_global_nodes() << '\n'
                << "m(input): " << input_graph.number_of_global_edges() << '\n';
    }
    mpi::check_or_abort(MPI_Barrier(native_communicator), native_communicator,
                        "MPI_Barrier(after DSPAC input)");

    clock.restart();
    auto split_graph = parallel_graph_access{native_communicator};
    auto splitter = dspac{input_graph, native_communicator,
                          dspac_config.infinity};
    splitter.construct(split_graph);
    if (rank == ROOT) {
      std::cout << "split graph construction took " << clock.elapsed() << '\n'
                << "n(split): " << split_graph.number_of_global_nodes() << '\n'
                << "m(split): " << split_graph.number_of_global_edges() << '\n';
    }

    clock.restart();
    execute_parhip(split_graph, partition_config, communicator);
    if (rank == ROOT) {
      std::cout << "parhip took " << clock.elapsed() << '\n';
    }

    clock.restart();
    splitter.fix_cut_dominant_edges(split_graph);
    auto edge_partition =
        splitter.project_partition(split_graph, edge_permutation);
    auto const vertex_cut =
        splitter.calculate_vertex_cut(partition_config.k, edge_partition);
    if (rank == ROOT) {
      std::cout << "evaluation took " << clock.elapsed() << '\n'
                << "vertex cut: " << vertex_cut << '\n';
    }

    if (partition_config.save_partition ||
        partition_config.save_partition_binary) {
      for (NodeID node = 0; node < split_graph.number_of_local_nodes(); ++node) {
        split_graph.setNodeLabel(node, edge_partition[node]);
      }
    }
    if (partition_config.save_partition) {
      auto output = parallel_vector_io{};
      auto const filename = partition_filename.empty()
                                ? std::string{"tmpedgepartition.txtp"}
                                : partition_filename;
      output.writePartitionSimpleParallel(split_graph, filename);
    }
    if (partition_config.save_partition_binary) {
      auto output = parallel_vector_io{};
      auto const filename = partition_filename.empty()
                                ? std::string{"tmpedgepartition.binp"}
                                : partition_filename;
      output.writePartitionBinaryParallelPosix(partition_config, split_graph,
                                               filename);
    }
    mpi::check_or_abort(MPI_Barrier(native_communicator), native_communicator,
                        "MPI_Barrier(DSPAC executable completion)");
    return EXIT_SUCCESS;
  });
}
