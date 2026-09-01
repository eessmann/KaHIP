#include <mpi.h>
#include <unistd.h>

#include <cstdlib>
#include <string_view>

#include "parallel_mh/population_size_broadcast.h"

// KAHIP_PMPI_CALLBACK_REGION_BEGIN
namespace population_failure_probe {
inline bool active = false;
inline MPI_Comm expected_communicator = MPI_COMM_NULL;
inline int broadcasts = 0;
inline int point_to_point_calls = 0;
inline int barriers = 0;
inline bool callback_error = false;

void write_text(std::string_view text) noexcept {
  static_cast<void>(::write(STDERR_FILENO, text.data(), text.size()));
}

[[noreturn]] void observed_abort(MPI_Comm communicator,
                                 int error_code) noexcept {
  int relation = MPI_UNEQUAL;
  if (expected_communicator == MPI_COMM_NULL || error_code != EXIT_FAILURE ||
      PMPI_Comm_compare(communicator, expected_communicator, &relation) !=
          MPI_SUCCESS ||
      relation != MPI_IDENT || broadcasts != 1 || point_to_point_calls != 0 ||
      barriers != 0 || callback_error) {
    write_text("observed population-size MPI_Abort with unexpected state\n");
    std::_Exit(91);
  }
  write_text(
      "observed population-size MPI_Abort on the affected subcommunicator\n");
  std::_Exit(86);
}
}  // namespace population_failure_probe

static_assert(noexcept(population_failure_probe::write_text({})));
static_assert(noexcept(population_failure_probe::observed_abort(MPI_COMM_NULL,
                                                                0)));

extern "C" int MPI_Bcast(void* buffer,
                         int count,
                         MPI_Datatype datatype,
                         int root,
                         MPI_Comm communicator) {
  if (!population_failure_probe::active) {
    return PMPI_Bcast(buffer, count, datatype, root, communicator);
  }
  ++population_failure_probe::broadcasts;
  int relation = MPI_UNEQUAL;
  if (count != 1 || datatype != MPI_INT || root != 0 ||
      PMPI_Comm_compare(communicator,
                        population_failure_probe::expected_communicator,
                        &relation) != MPI_SUCCESS ||
      relation != MPI_IDENT) {
    population_failure_probe::callback_error = true;
  }
  return MPI_ERR_OTHER;
}

extern "C" int MPI_Isend(void const* buffer,
                         int count,
                         MPI_Datatype datatype,
                         int destination,
                         int tag,
                         MPI_Comm communicator,
                         MPI_Request* request) {
  if (population_failure_probe::active) {
    ++population_failure_probe::point_to_point_calls;
  }
  return PMPI_Isend(buffer, count, datatype, destination, tag, communicator,
                    request);
}

extern "C" int MPI_Recv(void* buffer,
                        int count,
                        MPI_Datatype datatype,
                        int source,
                        int tag,
                        MPI_Comm communicator,
                        MPI_Status* status) {
  if (population_failure_probe::active) {
    ++population_failure_probe::point_to_point_calls;
  }
  return PMPI_Recv(buffer, count, datatype, source, tag, communicator, status);
}

extern "C" int MPI_Barrier(MPI_Comm communicator) {
  if (population_failure_probe::active) {
    ++population_failure_probe::barriers;
  }
  return PMPI_Barrier(communicator);
}

extern "C" int MPI_Abort(MPI_Comm communicator, int error_code) {
  population_failure_probe::observed_abort(communicator, error_code);
}
// KAHIP_PMPI_CALLBACK_REGION_END

int main(int argc, char** argv) {
  if (MPI_Init(&argc, &argv) != MPI_SUCCESS) {
    return 2;
  }
  auto world_rank = 0;
  auto world_size = 0;
  if (MPI_Comm_rank(MPI_COMM_WORLD, &world_rank) != MPI_SUCCESS ||
      MPI_Comm_size(MPI_COMM_WORLD, &world_size) != MPI_SUCCESS) {
    return 3;
  }

  auto communicator = MPI_COMM_NULL;
  if (MPI_Comm_split(MPI_COMM_WORLD, 0, world_size - world_rank,
                     &communicator) != MPI_SUCCESS ||
      communicator == MPI_COMM_NULL) {
    return 4;
  }
  if (MPI_Comm_set_errhandler(communicator, MPI_ERRORS_RETURN) != MPI_SUCCESS) {
    return 5;
  }
  population_failure_probe::expected_communicator = communicator;
  population_failure_probe::active = true;

  static_cast<void>(kahip::parallel_mh::broadcast_population_size(
      communicator, world_rank == world_size - 1 ? 17 : -1, false));
  population_failure_probe::write_text(
      "population-size broadcast returned without fail-fast\n");
  std::_Exit(92);
}
