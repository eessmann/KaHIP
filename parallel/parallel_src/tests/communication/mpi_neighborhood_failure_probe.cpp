#include <mpi.h>

#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <string_view>

#include "communication/mpi_adapter.h"

namespace {
enum class failure_mode {
  create,
  query,
  free,
};

auto selected_mode = failure_mode::create;
auto graph_communicator = MPI_COMM_NULL;

[[nodiscard]] auto mode_name() noexcept -> std::string_view {
  switch (selected_mode) {
    case failure_mode::create:
      return "create";
    case failure_mode::query:
      return "query";
    case failure_mode::free:
      return "free";
  }
  return "unknown";
}

[[nodiscard]] auto affected_name(MPI_Comm communicator) noexcept
    -> std::string_view {
  if (communicator == MPI_COMM_NULL) {
    return "null";
  }
  if (communicator == graph_communicator) {
    return "graph";
  }
  if (communicator == MPI_COMM_WORLD) {
    return "world";
  }
  return "internal";
}
}  // namespace

extern "C" int MPI_Dist_graph_create(MPI_Comm old_communicator,
                                     int source_count,
                                     int const sources[],
                                     int const degrees[],
                                     int const destinations[],
                                     int const weights[],
                                     MPI_Info info,
                                     int reorder,
                                     MPI_Comm* result_communicator) {
  if (selected_mode == failure_mode::create) {
    return MPI_ERR_OTHER;
  }
  auto const result = PMPI_Dist_graph_create(
      old_communicator, source_count, sources, degrees, destinations, weights,
      info, reorder, result_communicator);
  if (result == MPI_SUCCESS) {
    graph_communicator = *result_communicator;
  }
  return result;
}

extern "C" int MPI_Dist_graph_neighbors_count(MPI_Comm communicator,
                                              int* indegree,
                                              int* outdegree,
                                              int* weighted) {
  if (selected_mode == failure_mode::query &&
      communicator == graph_communicator) {
    return MPI_ERR_OTHER;
  }
  return PMPI_Dist_graph_neighbors_count(communicator, indegree, outdegree,
                                         weighted);
}

extern "C" int MPI_Comm_free(MPI_Comm* communicator) {
  if (selected_mode == failure_mode::free &&
      *communicator == graph_communicator) {
    return MPI_ERR_OTHER;
  }
  return PMPI_Comm_free(communicator);
}

extern "C" int MPI_Abort(MPI_Comm communicator, int) {
  auto const mode = mode_name();
  auto const affected = affected_name(communicator);
  std::fprintf(stderr,
               "observed MPI_Abort from neighborhood-%.*s failure on %.*s "
               "communicator\n",
               static_cast<int>(mode.size()), mode.data(),
               static_cast<int>(affected.size()), affected.data());
  std::_Exit(86);
}

int main(int argc, char* argv[]) {
  if (argc != 2) {
    std::fputs("usage: mpi_neighborhood_failure_probe MODE\n", stderr);
    return 64;
  }
  auto const mode = std::string_view{argv[1]};
  if (mode == "create") {
    selected_mode = failure_mode::create;
  } else if (mode == "query") {
    selected_mode = failure_mode::query;
  } else if (mode == "free") {
    selected_mode = failure_mode::free;
  } else {
    std::fprintf(stderr, "unknown neighborhood failure mode: %s\n", argv[1]);
    return 64;
  }

  auto const init_result = MPI_Init(&argc, &argv);
  if (init_result != MPI_SUCCESS) {
    std::fprintf(stderr, "MPI_Init returned raw error %d\n", init_result);
    return 70;
  }
  {
    parhip::mpi::communicator_view const world{MPI_COMM_WORLD};
    parhip::mpi::distributed_graph graph{world, {world.rank()}};
  }

  std::fprintf(stderr, "neighborhood-%.*s failure did not abort\n",
               static_cast<int>(mode.size()), mode.data());
  static_cast<void>(MPI_Finalize());
  return 2;
}
