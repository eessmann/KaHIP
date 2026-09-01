#include <argtable3.h>
#include <mpi.h>

#include <cstdio>
#include <cstdlib>
#include <iostream>

#include "communication/mpi_handles.h"
#include "parse_dspac_parameters.h"
#include "parse_parameters.h"

namespace {
auto reject_world_queries = false;

[[noreturn]] void reject_world(char const* operation) noexcept {
  std::fprintf(stderr, "%s queried MPI_COMM_WORLD inside parser\n", operation);
  std::_Exit(87);
}
}  // namespace

extern "C" int MPI_Comm_rank(MPI_Comm communicator, int* rank) {
  if (reject_world_queries && communicator == MPI_COMM_WORLD) {
    reject_world("MPI_Comm_rank");
  }
  return PMPI_Comm_rank(communicator, rank);
}

extern "C" int MPI_Comm_size(MPI_Comm communicator, int* size) {
  if (reject_world_queries && communicator == MPI_COMM_WORLD) {
    reject_world("MPI_Comm_size");
  }
  return PMPI_Comm_size(communicator, size);
}

int main(int argc, char** argv) {
  if (PMPI_Init(&argc, &argv) != MPI_SUCCESS) {
    std::fputs("MPI_Init failed\n", stderr);
    return 70;
  }

  auto world_rank = 0;
  PMPI_Comm_rank(MPI_COMM_WORLD, &world_rank);
  auto reversed = MPI_COMM_NULL;
  if (PMPI_Comm_split(MPI_COMM_WORLD, 0, -world_rank, &reversed) !=
      MPI_SUCCESS) {
    std::fputs("MPI_Comm_split failed\n", stderr);
    PMPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
  }
  PMPI_Comm_set_errhandler(reversed, MPI_ERRORS_RETURN);
  auto const communicator = parhip::mpi::communicator_view{reversed};

  char parhip_program[] = "parser-test";
  char parhip_graph[] = "graph.graph";
  char parhip_k[] = "--k=4";
  char parhip_preconfiguration[] = "--preconfiguration=ecosocial";
  char* parhip_arguments[]{parhip_program, parhip_graph, parhip_k,
                           parhip_preconfiguration};
  auto parhip_config = parhip::PPartitionConfig{};
  auto graph_filename = std::string{};

  reject_world_queries = true;
  auto const parhip_result = parhip::parse_parameters(
      static_cast<int>(std::size(parhip_arguments)), parhip_arguments,
      parhip_config, graph_filename, communicator);
  reject_world_queries = false;

  char dspac_program[] = "dspac-parser-test";
  char dspac_graph[] = "graph.graph";
  char dspac_k[] = "--k=4";
  char dspac_preconfiguration[] = "--preconfiguration=ecosocial";
  char* dspac_arguments[]{dspac_program, dspac_graph, dspac_k,
                          dspac_preconfiguration};
  auto dspac_partition_config = parhip::PPartitionConfig{};
  auto dspac_config = parhip::DspacConfig{};
  auto dspac_graph_filename = std::string{};
  auto dspac_partition_filename = std::string{};

  reject_world_queries = true;
  auto const dspac_result = parhip::parse_dspac_parameters(
      static_cast<int>(std::size(dspac_arguments)), dspac_arguments,
      dspac_partition_config, dspac_config, dspac_graph_filename,
      dspac_partition_filename, communicator);
  reject_world_queries = false;

  auto process_count = 0;
  PMPI_Comm_size(reversed, &process_count);
  auto const valid =
      parhip_result == parhip::parse_outcome::continue_execution &&
      graph_filename == "graph.graph" &&
      parhip_config.k == 4 &&
      parhip_config.evolutionary_time_limit == 2048 / process_count &&
      dspac_result == parhip::parse_outcome::continue_execution &&
      dspac_graph_filename == "graph.graph" &&
      dspac_partition_config.k == 4 && dspac_config.k == 4;

  PMPI_Comm_free(&reversed);
  PMPI_Finalize();
  return valid ? EXIT_SUCCESS : EXIT_FAILURE;
}
