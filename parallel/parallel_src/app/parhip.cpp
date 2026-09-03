/******************************************************************************
 * parhip.cpp
 * *
 * Source of KaHIP -- Karlsruhe High Quality Partitioning.
 * Christian Schulz <christian.schulz.phone@gmail.com>
 *****************************************************************************/

#include <argtable3.h>

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <span>
#include <string>

#include "application_math.h"
#include "communication/dummy_operations.h"
#include "communication/mpi_application.h"
#include "communication/mpi_fixed_reduction.h"
#include "communication/mpi_trace.h"
#include "data_structure/parallel_graph_access.h"
#include "distributed_partitioning/distributed_partitioner.h"
#include "io/parallel_graph_io.h"
#include "io/parallel_vector_io.h"
#include "macros_assertions.h"
#include "parse_parameters.h"
#include "partition_config.h"
#include "random_functions.h"
#include "timer.h"
#include "tools/distributed_quality_metrics.h"

namespace parhip {
namespace {
[[nodiscard]] auto checked_global_sum(NodeWeight local,
                                      mpi::communicator_view communicator,
                                      std::string_view diagnostic)
    -> NodeWeight {
  auto const local_values = std::array<NodeWeight, 1>{local};
  auto global_values = std::array<NodeWeight, 1>{};
  mpi::all_reduce_checked_sum(
      std::span<NodeWeight const>{local_values},
      std::span<NodeWeight>{global_values}, communicator,
      "MPI_Allreduce(ParHIP application checked sum)", "parhip executable",
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
        communicator.native_handle(), "parhip executable",
        "partition upper bound exceeds the graph-weight domain");
  }
  return *result;
}

void require_local_add(NodeWeight& accumulator,
                       NodeWeight value,
                       mpi::communicator_view communicator,
                       std::string_view diagnostic) {
  if (!application::checked_add(accumulator, value)) {
    mpi::abort_on_capacity_failure(communicator.native_handle(),
                                   "parhip executable", diagnostic);
  }
}

[[nodiscard]] auto edge_fraction(EdgeWeight edge_count,
                                 EdgeWeight global_edge_count) noexcept
    -> double {
  return global_edge_count == 0
             ? 0.0
             : static_cast<double>(edge_count) /
                   static_cast<double>(global_edge_count);
}
}  // namespace
}  // namespace parhip

int main(int argument_count, char** argument_values) {
  using namespace parhip;
  mpi::application_runtime runtime{argument_count, argument_values,
                                   "parhip executable"};
  return runtime.execute([&](mpi::communicator_view communicator) -> int {
    auto partition_config = PPartitionConfig{};
    auto graph_filename = std::string{};
    auto const parse_result = parse_parameters(
        argument_count, argument_values, partition_config, graph_filename,
        communicator);
    if (parse_result != parse_outcome::continue_execution) {
      return parse_result == parse_outcome::early_success ? EXIT_SUCCESS
                                                          : EXIT_FAILURE;
    }

    auto const rank = communicator.rank();
    auto const size = communicator.size();
    auto const native_communicator = communicator.native_handle();
    auto clock = timer{};

    mpi::check_or_abort(MPI_Barrier(native_communicator), native_communicator,
                        "MPI_Barrier(before ParHIP warm-up)");
    clock.restart();
    if (rank == ROOT) {
      std::cout << "running collective dummy operations ";
    }
    auto warm_up = dummy_operations{};
    warm_up.run_collective_dummy_operations(communicator);
    mpi::check_or_abort(MPI_Barrier(native_communicator), native_communicator,
                        "MPI_Barrier(after ParHIP warm-up)");
    if (rank == ROOT) {
      std::cout << "took " << clock.elapsed() << '\n';
    }

    if (rank == ROOT) {
      PRINT(std::cout << "log> cluster coarsening factor is set to "
                      << partition_config.cluster_coarsening_factor << '\n';)
    }

    partition_config.stop_factor /= static_cast<int>(partition_config.k);
    auto const seed = application::rank_seed(partition_config.seed, size, rank);
    if (!seed.has_value()) {
      mpi::abort_on_programming_error(native_communicator,
                                      "invalid rank-specific PRNG seed input");
    }
    partition_config.seed = *seed;
    std::srand(static_cast<unsigned int>(partition_config.seed));

    auto graph = parallel_graph_access{native_communicator};
    clock.restart();
    parallel_graph_io::readGraphWeighted(partition_config, graph,
                                         graph_filename, rank, size,
                                         native_communicator);
    KAHIP_MPI_TRACE_SET_HIERARCHY(0, 0, mpi::trace::epoch::input);
    forall_local_nodes(graph, node) {
      KAHIP_MPI_TRACE(mpi::trace::graph_distribution_node(
          mpi::trace::current_hierarchy(), graph.getGlobalID(node), rank,
          graph.getNodeWeight(node)));
      forall_out_edges(graph, edge, node) {
        auto const target = graph.getEdgeTarget(edge);
        KAHIP_MPI_TRACE(mpi::trace::graph_distribution_edge(
            mpi::trace::current_hierarchy(), graph.getGlobalID(node), rank,
            graph.getGlobalID(target), graph.getEdgeWeight(edge)));
      }
      endfor
    }
    endfor
    if (rank == ROOT) {
      std::cout << "took " << clock.elapsed() << '\n';
      std::cout << "n:" << graph.number_of_global_nodes()
                << " m: " << graph.number_of_global_edges() << '\n';
    }

    random_functions::setSeed(partition_config.seed);
    auto const process_count = static_cast<ULONG>(size);
    parallel_graph_access::set_comm_rounds(partition_config.comm_rounds /
                                           process_count);
    parallel_graph_access::set_comm_rounds_up(partition_config.comm_rounds /
                                              process_count);
    distributed_partitioner::generate_random_choices(partition_config,
                                                      communicator);
    graph.printMemoryUsage(std::cout);

    auto local_inter_edges = EdgeWeight{0};
    auto local_intra_edges = EdgeWeight{0};
    auto local_weight = NodeWeight{0};
    forall_local_nodes(graph, node) {
      require_local_add(local_weight, graph.getNodeWeight(node), communicator,
                        "local graph vertex-weight sum overflow");
      forall_out_edges(graph, edge, node) {
        auto const target = graph.getEdgeTarget(edge);
        auto& count = graph.is_local_node(target) ? local_intra_edges
                                                  : local_inter_edges;
        require_local_add(count, EdgeWeight{1}, communicator,
                          "local graph edge-count overflow");
      }
      endfor
    }
    endfor

    auto const local_statistics =
        std::array<NodeWeight, 3>{local_inter_edges, local_intra_edges,
                                  local_weight};
    auto global_statistics = std::array<NodeWeight, 3>{};
    mpi::all_reduce_checked_sum(
        std::span<NodeWeight const>{local_statistics},
        std::span<NodeWeight>{global_statistics}, communicator,
        "MPI_Allreduce(ParHIP application statistics)", "parhip executable",
        "global graph statistic exceeds the KaHIP weight domain");
    auto const [global_inter_edges, global_intra_edges, global_weight] =
        global_statistics;

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

    clock.restart();
    partition_config.number_of_overall_nodes = graph.number_of_global_nodes();
    if (partition_config.vertex_degree_weights) {
      auto local_total_load = NodeWeight{0};
      forall_local_nodes(graph, node) {
        auto const degree = graph.getNodeDegree(node);
        if (degree == std::numeric_limits<NodeWeight>::max()) {
          mpi::abort_on_capacity_failure(
              native_communicator, "parhip executable",
              "degree-plus-one vertex weight exceeds the weight domain");
        }
        auto const weight = static_cast<NodeWeight>(degree + 1);
        graph.setNodeWeight(node, weight);
        require_local_add(local_total_load, weight, communicator,
                          "local degree-weight sum overflow");
      }
      endfor
      auto const total_load = checked_global_sum(
          local_total_load, communicator,
          "global degree-weight sum exceeds the graph-weight domain");
      partition_config.upper_bound_partition =
          partition_upper_bound(total_load, partition_config, communicator);
    } else {
      partition_config.upper_bound_partition =
          partition_upper_bound(global_weight, partition_config, communicator);
      if (rank == ROOT) {
        std::cout << "upper bound on blocks "
                  << partition_config.upper_bound_partition << '\n';
      }
    }

    auto partitioner = distributed_partitioner{};
    partitioner.perform_partitioning(native_communicator, partition_config,
                                     graph);
    mpi::check_or_abort(MPI_Barrier(native_communicator), native_communicator,
                        "MPI_Barrier(ParHIP partition completion)");

    KAHIP_MPI_TRACE_SET_HIERARCHY(
        partition_config.num_vcycles == 0 ? 0
                                          : partition_config.num_vcycles - 1,
        0, mpi::trace::epoch::final_partition);
    forall_local_nodes(graph, node) {
      KAHIP_MPI_TRACE(mpi::trace::final_partition(
          mpi::trace::current_hierarchy(), graph.getGlobalID(node), rank,
          graph.getNodeLabel(node)));
    }
    endfor
    mpi::trace::write_rank_file_if_requested(native_communicator);

    auto const running_time = clock.elapsed();
    auto quality = distributed_quality_metrics{};
    auto const edge_cut = quality.edge_cut(graph, native_communicator);
    auto const balance =
        quality.balance(partition_config, graph, native_communicator);
    PRINT(auto const balance_load =
              quality.balance_load(partition_config, graph,
                                   native_communicator);)
    PRINT(auto const balance_load_dist =
              quality.balance_load_dist(partition_config, graph,
                                        native_communicator);)

    if (rank == ROOT) {
      std::cout << "log>=====================================\n";
      std::cout << "log>============AND WE R DONE============\n";
      std::cout << "log>=====================================\n";
      std::cout << "log>total partitioning time elapsed " << running_time
                << '\n';
      std::cout << "log>final edge cut " << edge_cut << '\n';
      std::cout << "log>final balance " << balance << '\n';
      PRINT(std::cout << "log>final balance load " << balance_load << '\n';)
      PRINT(std::cout << "log>final balance load dist " << balance_load_dist
                      << '\n';)
    }
    PRINT(quality.comm_vol(partition_config, graph, native_communicator);)
    PRINT(quality.comm_vol_dist(graph, native_communicator);)

    if (partition_config.save_partition) {
      auto output = parallel_vector_io{};
      output.writePartitionSimpleParallel(graph, "tmppartition.txtp");
    }
    if (partition_config.save_partition_binary) {
      auto output = parallel_vector_io{};
      output.writePartitionBinaryParallelPosix(partition_config, graph,
                                               "tmppartition.binp");
    }
    return EXIT_SUCCESS;
  });
}
