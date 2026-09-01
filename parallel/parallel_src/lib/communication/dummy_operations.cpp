/******************************************************************************
 * dummy_operations.cpp
 * *
 * Source of KaHIP -- Karlsruhe High Quality Partitioning.
 * Christian Schulz <christian.schulz.phone@gmail.com>
 *****************************************************************************/

#include <mpi.h>

#include <algorithm>
#include <numeric>
#include <vector>

#include "communication/mpi_failure.h"
#include "dummy_operations.h"
namespace parhip {
dummy_operations::dummy_operations() {
                
}

dummy_operations::~dummy_operations() {
                        
}

void dummy_operations::run_collective_dummy_operations(
    mpi::communicator_view communicator) {
  auto const rank = communicator.rank();
  auto const size = communicator.size();
  auto const native_communicator = communicator.native_handle();

  // Run Broadcast
  {
    auto x = rank;
    mpi::check_or_abort(MPI_Bcast(&x, 1, MPI_INT, 0, native_communicator),
                        native_communicator,
                        "MPI_Bcast(application warm-up)");
  }
  // Run Allgather.
  {
    auto received = std::vector<int>(static_cast<std::size_t>(size));
    mpi::check_or_abort(
        MPI_Allgather(&rank, 1, MPI_INT, received.data(), 1, MPI_INT,
                      native_communicator),
        native_communicator, "MPI_Allgather(application warm-up)");
  }

  // Run Allreduce.
  {
    int y = 0;
    mpi::check_or_abort(
        MPI_Allreduce(&rank, &y, 1, MPI_INT, MPI_SUM, native_communicator),
        native_communicator, "MPI_Allreduce(application warm-up)");
  }

  // Dummy Prefix Sum
  {
    int x  = 1;
    int y  = 0;

    mpi::check_or_abort(
        MPI_Scan(&x, &y, 1, MPI_INT, MPI_SUM, native_communicator),
        native_communicator, "MPI_Scan(application warm-up)");
  }

  // Run Alltoallv.
  {
    auto const process_count = static_cast<std::size_t>(size);
    auto sent = std::vector<int>(process_count);
    auto received = std::vector<int>(process_count);
    auto send_counts = std::vector<int>(process_count, 1);
    auto receive_counts = std::vector<int>(process_count, 1);
    auto send_displacements = std::vector<int>(process_count);
    auto receive_displacements = std::vector<int>(process_count);
    std::iota(send_displacements.begin(), send_displacements.end(), 0);
    std::ranges::copy(send_displacements, receive_displacements.begin());
    mpi::check_or_abort(
        MPI_Alltoallv(sent.data(), send_counts.data(), send_displacements.data(),
                      MPI_INT, received.data(), receive_counts.data(),
                      receive_displacements.data(), MPI_INT,
                      native_communicator),
        native_communicator, "MPI_Alltoallv(application warm-up)");
  }
        

}
}
