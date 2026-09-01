/******************************************************************************
 * graph2binary_external.cpp
 * *
 * Source of KaHIP -- Karlsruhe High Quality Partitioning.
 * Christian Schulz <christian.schulz.phone@gmail.com>
 *****************************************************************************/

#include <cstdlib>
#include <iostream>
#include <string>

#include "communication/mpi_application.h"
#include "io/parallel_graph_io.h"

int main(int argument_count, char** argument_values) {
  using namespace parhip;
  mpi::application_runtime runtime{argument_count, argument_values,
                                   "graph2binary_external executable"};
  return runtime.execute([&](mpi::communicator_view communicator) -> int {
    auto const rank = communicator.rank();
    auto const size = communicator.size();
    if (rank == ROOT) {
      std::cout << "program converts a METIS graph file into a binary "
                   "(distributed graph format) file.\n";
    }
    if (argument_count != 3) {
      if (rank == ROOT) {
        std::cout
            << "usage: graph2binary_external metisfile outputfilename\n";
      }
      return EXIT_SUCCESS;
    }
    if (size != 1) {
      if (rank == ROOT) {
        std::cout << "currently only one process supported.\n";
      }
      return EXIT_SUCCESS;
    }

    auto const graph_filename = std::string{argument_values[1]};
    auto const output_filename = std::string{argument_values[2]};
    std::cout << "Reading and writing graph " << graph_filename << '\n';
    parallel_graph_io::writeGraphExternallyBinary(graph_filename,
                                                  output_filename);
    return EXIT_SUCCESS;
  });
}
