/******************************************************************************
 * kaffpaE.cpp
 * *
 * Source of KaHIP -- Karlsruhe High Quality Partitioning.
 * Christian Schulz <christian.schulz.phone@gmail.com>
 *****************************************************************************/

#include <argtable3.h>
#include <omp.h>

#include <cstdlib>
#include <iostream>
#include <limits>
#include <queue>
#include <sstream>
#include <string>
#include <vector>

#include "algorithms/cycle_search.h"
#include "balance_configuration.h"
#include "data_structure/graph_access.h"
#include "graph_io.h"
#include "mpi_application_runtime.h"
#include "parallel_mh/parallel_mh_async.h"
#include "parse_parameters.h"
#include "partition/graph_partitioner.h"
#include "partition/partition_config.h"
#include "quality_metrics.h"
#include "timer.h"

int main(int argument_count, char** argument_values) {
  kahip::mpi::application_runtime runtime{argument_count, argument_values,
                                          "kaffpaE executable"};
  return runtime.execute([&](MPI_Comm communicator) -> int {
        auto partition_config = PartitionConfig{};
        auto graph_filename = std::string{};
        auto is_graph_weighted = false;
        auto suppress_output = false;
        auto recursive = false;
        auto early_exit = false;
        if (parse_parameters(argument_count, argument_values, partition_config,
                             graph_filename, is_graph_weighted,
                             suppress_output, recursive, &early_exit) != 0) {
          return early_exit ? EXIT_SUCCESS : EXIT_FAILURE;
        }

        auto rank = 0;
        kahip::mpi::check_or_abort(MPI_Comm_rank(communicator, &rank),
                                   communicator, "kaffpaE executable",
                                   "MPI_Comm_rank(kaffpaE operation)");
        if (partition_config.k == 0 ||
            partition_config.k >
                static_cast<PartitionID>(std::numeric_limits<int>::max())) {
          if (rank == ROOT) {
            std::cerr << "Number of blocks must be a positive int.\n";
          }
          return EXIT_FAILURE;
        }

        partition_config.LogDump(stdout);
        partition_config.graph_filename =
            graph_filename.substr(graph_filename.find_last_of('/') + 1);
        auto graph = graph_access{};
        auto clock = timer{};
        graph_io::readGraphWeighted(graph, graph_filename);
        std::cout << "io time: " << clock.elapsed() << '\n';

        if (partition_config.connected_blocks && graph.number_of_nodes() > 0) {
          auto visited = std::vector<bool>(graph.number_of_nodes(), false);
          auto pending = std::queue<NodeID>{};
          visited.front() = true;
          pending.push(0);
          auto visited_count = NodeID{1};
          while (!pending.empty()) {
            auto const vertex = pending.front();
            pending.pop();
            forall_out_edges(graph, edge, vertex) {
              auto const target = graph.getEdgeTarget(edge);
              if (!visited[target]) {
                visited[target] = true;
                ++visited_count;
                pending.push(target);
              }
            }
            endfor
          }
          if (visited_count < graph.number_of_nodes()) {
            std::cout << "WARNING: input graph is disconnected, connected "
                         "blocks cannot be guaranteed.\n";
          }
        }

        omp_set_num_threads(1);
        graph.set_partition_count(partition_config.k);
        partition_config.kaffpaE = true;
        if (partition_config.imbalance < 1) {
          partition_config.kabapE = true;
        }
        auto balance = balance_configuration{};
        balance.configurate_balance(partition_config, graph);

        auto input_partition = std::vector<PartitionID>{};
        if (!partition_config.input_partition.empty()) {
          std::cout << "reading input partition\n";
          graph_io::readPartition(graph, partition_config.input_partition);
          partition_config.graph_allready_partitioned = true;
          input_partition.resize(graph.number_of_nodes());
          forall_nodes(graph, node) {
            input_partition[node] = graph.getPartitionIndex(node);
          }
          endfor
        }

        clock.restart();
        auto metaheuristic =
            parallel_mh_async{communicator};
        metaheuristic.perform_partitioning(partition_config, graph);
        auto const elapsed = clock.elapsed();

        if (rank == ROOT) {
          std::cout << "time spent for partitioning " << elapsed << '\n';
          std::cout << "time spent in neg. cycle detection "
                    << cycle_search::total_time << '\n';
          auto const relative_cycle_time =
              elapsed > 0.0 ? cycle_search::total_time / elapsed * 100.0 : 0.0;
          std::cout << "time spent in neg. cycle detection (rel) "
                    << relative_cycle_time << '\n';

          auto quality = quality_metrics{};
          auto const cut = quality.edge_cut(graph);
          std::cout << "cut \t\t" << cut << '\n';
          std::cout << "finalobjective  " << cut << '\n';
          std::cout << "bnd \t\t" << quality.boundary_nodes(graph) << '\n';
          std::cout << "balance \t" << quality.balance(graph) << '\n';
          std::cout << "max_comm_vol \t"
                    << quality.max_communication_volume(graph) << '\n';

          auto filename = std::stringstream{};
          if (partition_config.filename_output.empty()) {
            filename << "tmppartition" << partition_config.k;
          } else {
            filename << partition_config.filename_output;
          }
          graph_io::writePartition(graph, filename.str());
        }
        return EXIT_SUCCESS;
      });
}
