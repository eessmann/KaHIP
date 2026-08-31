#include "communication/mpi_failure.h"

#include <cstdlib>
#include <exception>

#include <spdlog/spdlog.h>

namespace parhip::mpi {
auto runtime_is_active() noexcept -> bool {
  int initialized = 0;
  int finalized = 0;
  if (MPI_Initialized(&initialized) != MPI_SUCCESS || initialized == 0) {
    return false;
  }
  if (MPI_Finalized(&finalized) != MPI_SUCCESS) {
    return false;
  }
  return finalized == 0;
}

namespace {
void log_failure(std::string_view boundary,
                 std::exception_ptr failure) noexcept {
  try {
    if (failure == nullptr) {
      spdlog::critical("{}: unknown unrecoverable failure", boundary);
      return;
    }
    try {
      std::rethrow_exception(failure);
    } catch (std::exception const& error) {
      spdlog::critical("{}: {}", boundary, error.what());
    } catch (...) {
      spdlog::critical("{}: unknown unrecoverable exception", boundary);
    }
  } catch (...) {
    // Failure reporting must never replace the original termination path.
  }
}
}  // namespace

[[noreturn]] void abort_on_exception(MPI_Comm communicator,
                                     std::string_view boundary,
                                     std::exception_ptr failure) noexcept {
  log_failure(boundary, failure);
  if (runtime_is_active()) {
    auto const affected =
        communicator == MPI_COMM_NULL ? MPI_COMM_WORLD : communicator;
    MPI_Abort(affected, EXIT_FAILURE);
  }
  std::abort();
}
}  // namespace parhip::mpi
