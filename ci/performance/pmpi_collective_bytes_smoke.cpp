#include <mpi.h>

#include <array>
#include <cstdlib>
#include <iostream>
#include <vector>

#ifndef KAHIP_PMPI_SMOKE_HAVE_LARGE_COUNTS
#define KAHIP_PMPI_SMOKE_HAVE_LARGE_COUNTS (MPI_VERSION >= 4)
#endif

#ifndef KAHIP_PMPI_SMOKE_HAVE_PERSISTENT
#define KAHIP_PMPI_SMOKE_HAVE_PERSISTENT (MPI_VERSION >= 4)
#endif

#ifndef KAHIP_PMPI_SMOKE_HAVE_PERSISTENT_C
#define KAHIP_PMPI_SMOKE_HAVE_PERSISTENT_C (MPI_VERSION >= 4)
#endif

namespace {

void require(int result, char const* operation) {
  if (result != MPI_SUCCESS) {
    std::cerr << operation << " failed with MPI status " << result << '\n';
    MPI_Abort(MPI_COMM_WORLD, result);
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  int supplied{};
  require(MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &supplied),
          "MPI_Init_thread");
  int size{};
  int rank{};
  require(MPI_Comm_size(MPI_COMM_WORLD, &size), "MPI_Comm_size");
  require(MPI_Comm_rank(MPI_COMM_WORLD, &rank), "MPI_Comm_rank");
  if (size <= 0) {
    MPI_Abort(MPI_COMM_WORLD, 2);
  }

  auto counts = std::vector<int>(static_cast<std::size_t>(size), 1);
  auto displacements = std::vector<int>(static_cast<std::size_t>(size));
  for (int index = 0; index < size; ++index) {
    displacements[static_cast<std::size_t>(index)] = index;
  }
  auto sends = std::vector<int>(static_cast<std::size_t>(size), rank);
  auto receives = std::vector<int>(static_cast<std::size_t>(size));
  require(MPI_Alltoall(sends.data(), 1, MPI_INT, receives.data(), 1, MPI_INT,
                       MPI_COMM_WORLD),
          "MPI_Alltoall");
  require(MPI_Alltoallv(sends.data(), counts.data(), displacements.data(),
                        MPI_INT, receives.data(), counts.data(),
                        displacements.data(), MPI_INT, MPI_COMM_WORLD),
          "MPI_Alltoallv");
  MPI_Request request{MPI_REQUEST_NULL};
  require(
      MPI_Ialltoallv(sends.data(), counts.data(), displacements.data(), MPI_INT,
                     receives.data(), counts.data(), displacements.data(),
                     MPI_INT, MPI_COMM_WORLD, &request),
      "MPI_Ialltoallv");
  require(MPI_Wait(&request, MPI_STATUS_IGNORE), "MPI_Wait(Ialltoallv)");

#if KAHIP_PMPI_SMOKE_HAVE_LARGE_COUNTS
  auto large_counts = std::vector<MPI_Count>(static_cast<std::size_t>(size), 1);
  auto large_displacements =
      std::vector<MPI_Aint>(static_cast<std::size_t>(size));
  for (int index = 0; index < size; ++index) {
    large_displacements[static_cast<std::size_t>(index)] = index;
  }
  require(MPI_Alltoallv_c(sends.data(), large_counts.data(),
                          large_displacements.data(), MPI_INT, receives.data(),
                          large_counts.data(), large_displacements.data(),
                          MPI_INT, MPI_COMM_WORLD),
          "MPI_Alltoallv_c");
  require(MPI_Ialltoallv_c(sends.data(), large_counts.data(),
                           large_displacements.data(), MPI_INT, receives.data(),
                           large_counts.data(), large_displacements.data(),
                           MPI_INT, MPI_COMM_WORLD, &request),
          "MPI_Ialltoallv_c");
  require(MPI_Wait(&request, MPI_STATUS_IGNORE), "MPI_Wait(Ialltoallv_c)");
#endif

  auto const peer = (rank + 1) % size;
  MPI_Comm neighborhood{MPI_COMM_NULL};
  require(MPI_Dist_graph_create_adjacent(
              MPI_COMM_WORLD, 1, &peer, MPI_UNWEIGHTED, 1, &peer,
              MPI_UNWEIGHTED, MPI_INFO_NULL, 0, &neighborhood),
          "MPI_Dist_graph_create_adjacent");
  auto neighbor_send = rank;
  auto neighbor_receive = -1;
  std::array<int, 1> neighbor_counts{1};
  std::array<int, 1> neighbor_displacements{0};
  require(MPI_Neighbor_alltoall(&neighbor_send, 1, MPI_INT, &neighbor_receive,
                                1, MPI_INT, neighborhood),
          "MPI_Neighbor_alltoall");
  require(MPI_Neighbor_alltoallv(&neighbor_send, neighbor_counts.data(),
                                 neighbor_displacements.data(), MPI_INT,
                                 &neighbor_receive, neighbor_counts.data(),
                                 neighbor_displacements.data(), MPI_INT,
                                 neighborhood),
          "MPI_Neighbor_alltoallv");
  require(MPI_Ineighbor_alltoallv(&neighbor_send, neighbor_counts.data(),
                                  neighbor_displacements.data(), MPI_INT,
                                  &neighbor_receive, neighbor_counts.data(),
                                  neighbor_displacements.data(), MPI_INT,
                                  neighborhood, &request),
          "MPI_Ineighbor_alltoallv");
  require(MPI_Wait(&request, MPI_STATUS_IGNORE),
          "MPI_Wait(Ineighbor_alltoallv)");

#if KAHIP_PMPI_SMOKE_HAVE_LARGE_COUNTS
  std::array<MPI_Count, 1> neighbor_large_counts{1};
  std::array<MPI_Aint, 1> neighbor_large_displacements{0};
  require(MPI_Neighbor_alltoallv_c(
              &neighbor_send, neighbor_large_counts.data(),
              neighbor_large_displacements.data(), MPI_INT, &neighbor_receive,
              neighbor_large_counts.data(), neighbor_large_displacements.data(),
              MPI_INT, neighborhood),
          "MPI_Neighbor_alltoallv_c");
  require(MPI_Ineighbor_alltoallv_c(
              &neighbor_send, neighbor_large_counts.data(),
              neighbor_large_displacements.data(), MPI_INT, &neighbor_receive,
              neighbor_large_counts.data(), neighbor_large_displacements.data(),
              MPI_INT, neighborhood, &request),
          "MPI_Ineighbor_alltoallv_c");
  require(MPI_Wait(&request, MPI_STATUS_IGNORE),
          "MPI_Wait(Ineighbor_alltoallv_c)");

#endif

#if KAHIP_PMPI_SMOKE_HAVE_PERSISTENT
  require(MPI_Neighbor_alltoallv_init(&neighbor_send, neighbor_counts.data(),
                                      neighbor_displacements.data(), MPI_INT,
                                      &neighbor_receive, neighbor_counts.data(),
                                      neighbor_displacements.data(), MPI_INT,
                                      neighborhood, MPI_INFO_NULL, &request),
          "MPI_Neighbor_alltoallv_init");
  for (int iteration = 0; iteration < 2; ++iteration) {
    require(MPI_Start(&request), "MPI_Start");
    require(MPI_Wait(&request, MPI_STATUS_IGNORE), "MPI_Wait(persistent)");
  }
  require(MPI_Request_free(&request), "MPI_Request_free");
#endif

#if KAHIP_PMPI_SMOKE_HAVE_PERSISTENT_C
  std::array<MPI_Count, 1> persistent_large_counts{1};
  std::array<MPI_Aint, 1> persistent_large_displacements{0};
  require(
      MPI_Neighbor_alltoallv_init_c(
          &neighbor_send, persistent_large_counts.data(),
          persistent_large_displacements.data(), MPI_INT, &neighbor_receive,
          persistent_large_counts.data(), persistent_large_displacements.data(),
          MPI_INT, neighborhood, MPI_INFO_NULL, &request),
      "MPI_Neighbor_alltoallv_init_c");
  for (int iteration = 0; iteration < 2; ++iteration) {
    require(MPI_Startall(1, &request), "MPI_Startall");
    require(MPI_Wait(&request, MPI_STATUS_IGNORE), "MPI_Wait(persistent_c)");
  }
  require(MPI_Request_free(&request), "MPI_Request_free(_c)");
#endif

  require(MPI_Comm_free(&neighborhood), "MPI_Comm_free");
  require(MPI_Finalize(), "MPI_Finalize");
  return EXIT_SUCCESS;
}
