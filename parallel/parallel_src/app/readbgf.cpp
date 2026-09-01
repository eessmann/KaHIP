/******************************************************************************
 * readbgf.cpp
 * *
 * Source of KaHIP -- Karlsruhe High Quality Partitioning.
 * Christian Schulz <christian.schulz.phone@gmail.com>
 *****************************************************************************/

#include <cstdlib>
#include <iostream>
#include <string>

#include "communication/mpi_application.h"
#include "configuration.h"
#include "io/parallel_graph_io.h"
#include "partition_config.h"

int main(int argument_count, char** argument_values) {
  using namespace parhip;
  mpi::application_runtime runtime{argument_count, argument_values,
                                   "readbgf executable"};
  return runtime.execute([&](mpi::communicator_view communicator) -> int {
    auto const rank = communicator.rank();
    auto const size = communicator.size();
    if (argument_count != 2) {
      if (rank == ROOT) {
        std::cout << "usage: readbgf bgf_file\n";
      }
      return EXIT_SUCCESS;
    }
    if (rank == ROOT) {
      std::cout << "program reads a BGF (binary graph format) file and prints "
                   "it into dummy.\n";
    }

    auto config = PPartitionConfig{};
    auto presets = configuration{};
    presets.standard(config);
    auto graph = parallel_graph_access{communicator.native_handle()};
    auto graph_io = parallel_graph_io{};
    graph_io.readGraphBinary(config, graph, argument_values[1], rank, size);
    parallel_graph_io::writeGraphParallelSimple(graph, "dummy");
    return EXIT_SUCCESS;
  });
}
