/******************************************************************************
 * toolbox.cpp
 * *
 * Source of KaHIP -- Karlsruhe High Quality Partitioning.
 * Christian Schulz <christian.schulz.phone@gmail.com>
 *****************************************************************************/

#include <argtable3.h>

#include <cstdlib>
#include <iostream>
#include <string>

#include "application_math.h"
#include "communication/mpi_application.h"
#include "data_structure/parallel_graph_access.h"
#include "io/parallel_graph_io.h"
#include "io/parallel_vector_io.h"
#include "parse_parameters.h"
#include "partition_config.h"
#include "tools/distributed_quality_metrics.h"

int main(int argument_count, char** argument_values) {
  using namespace parhip;
  mpi::application_runtime runtime{argument_count, argument_values,
                                   "ParHIP toolbox executable"};
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
    partition_config.stop_factor /= static_cast<int>(partition_config.k);
    auto const seed = application::rank_seed(partition_config.seed, size, rank);
    if (!seed.has_value()) {
      mpi::abort_on_programming_error(native_communicator,
                                      "invalid rank-specific PRNG seed input");
    }
    partition_config.seed = *seed;
    std::srand(static_cast<unsigned int>(partition_config.seed));

    auto graph = parallel_graph_access{native_communicator};
    parallel_graph_io::readGraphWeighted(partition_config, graph,
                                         graph_filename, rank, size,
                                         native_communicator);
    auto partition_io = parallel_vector_io{};
    partition_io.readPartition(partition_config, graph,
                               partition_config.input_partition_filename);
    graph.printMemoryUsage(std::cout);

    mpi::check_or_abort(MPI_Barrier(native_communicator), native_communicator,
                        "MPI_Barrier(toolbox input completion)");
    if (partition_config.converter_evaluate) {
      auto quality = distributed_quality_metrics{};
      auto const edge_cut = quality.edge_cut(graph, native_communicator);
      auto const balance =
          quality.balance(partition_config, graph, native_communicator);
      auto const balance_load =
          quality.balance_load(partition_config, graph, native_communicator);
      auto const balance_load_dist = quality.balance_load_dist(
          partition_config, graph, native_communicator);

      if (rank == ROOT) {
        std::cout << "log>=====================================\n";
        std::cout << "log>============Evaluation Result========\n";
        std::cout << "log>=====================================\n";
        std::cout << "log>final edge cut " << edge_cut << '\n';
        std::cout << "log>final balance " << balance << '\n';
        std::cout << "log>final balance load " << balance_load << '\n';
        std::cout << "log>final balance load dist " << balance_load_dist
                  << '\n';
      }
      quality.comm_vol(partition_config, graph, native_communicator);
    }

    if (partition_config.save_partition) {
      if (rank == ROOT) {
        std::cout << "saving text partition\n";
      }
      partition_io.writePartitionSimpleParallel(graph, "tmppartition.txtp");
    }
    if (partition_config.save_partition_binary) {
      if (rank == ROOT) {
        std::cout << "saving binary partition\n";
      }
      partition_io.writePartitionBinaryParallelPosix(
          partition_config, graph, "tmppartition.binp");
    }
    mpi::check_or_abort(MPI_Barrier(native_communicator), native_communicator,
                        "MPI_Barrier(toolbox completion)");
    return EXIT_SUCCESS;
  });
}
