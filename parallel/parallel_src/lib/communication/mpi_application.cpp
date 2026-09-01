#include "communication/mpi_application.h"

#include <cstdlib>
#include <string_view>

#include <spdlog/spdlog.h>

namespace parhip::mpi {
namespace {
void flush_diagnostics() noexcept {
  try {
    if (auto* logger = spdlog::default_logger_raw(); logger != nullptr) {
      logger->flush();
    }
  } catch (...) {
    // Failure logging must not replace fail-fast termination.
  }
}

[[noreturn]] void abort_on_unusable_runtime(int error_code,
                                            std::string_view boundary,
                                            std::string_view operation,
                                            int rank) noexcept {
  try {
    if (rank >= 0) {
      spdlog::critical(
          "MPI lifecycle failure: {}: {} returned raw error {} on rank {}",
          boundary, operation, error_code, rank);
    } else {
      spdlog::critical(
          "MPI lifecycle failure: {}: {} returned raw error {} (rank "
          "unavailable)",
          boundary, operation, error_code);
    }
  } catch (...) {
    // The MPI runtime is unavailable or has an indeterminate state. Logging
    // must not trigger another MPI call or replace process termination.
  }
  flush_diagnostics();
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
