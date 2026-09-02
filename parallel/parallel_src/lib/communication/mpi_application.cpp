#include "communication/mpi_application.h"

#include <cstdlib>
#include <string_view>

#include "tools/fatal_diagnostics.h"

namespace parhip::mpi {
namespace {
[[noreturn]] void abort_on_unusable_runtime(int error_code,
                                            std::string_view boundary,
                                            std::string_view operation,
                                            int rank) noexcept {
  if (rank >= 0) {
    kahip::diagnostics::critical(
        "MPI lifecycle failure: ", boundary, ": ", operation,
        " returned raw error ", error_code, " on rank ", rank);
  } else {
    kahip::diagnostics::critical(
        "MPI lifecycle failure: ", boundary, ": ", operation,
        " returned raw error ", error_code, " (rank unavailable)");
  }
  std::abort();
}
}  // namespace

application_runtime::application_runtime(int& argument_count,
                                         char**& argument_values,
                                         std::string_view boundary)
    : boundary_(boundary) {
  auto const result = MPI_Init(&argument_count, &argument_values);
  if (result != MPI_SUCCESS) {
    abort_on_unusable_runtime(result, boundary_, "MPI_Init", -1);
  }
  check_or_abort(MPI_Comm_rank(MPI_COMM_WORLD, &rank_), MPI_COMM_WORLD,
                 "MPI_Comm_rank(application runtime)");
}

application_runtime::~application_runtime() noexcept {
  auto const result = MPI_Finalize();
  if (result != MPI_SUCCESS) {
    // MPI_Finalize may have partially dismantled the runtime. Issuing any
    // further MPI call, including MPI_Abort, is not portable here.
    abort_on_unusable_runtime(result, boundary_, "MPI_Finalize", rank_);
  }
}
}  // namespace parhip::mpi
