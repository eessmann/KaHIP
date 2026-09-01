#pragma once

#include <mpi.h>

#include <algorithm>
#include <cstdlib>

#include <spdlog/spdlog.h>

namespace kahip::parallel_mh {
[[nodiscard]] constexpr auto clamp_population_size(
    int estimate,
    bool easy_construction) noexcept -> int {
  return std::clamp(estimate, 3, easy_construction ? 50 : 100);
}

[[noreturn]] inline void abort_population_size_broadcast(
    MPI_Comm communicator,
    int error_code) noexcept {
  try {
    spdlog::critical(
        "MPI backend failure: MPI_Bcast(population size) returned raw error {}",
        error_code);
    if (auto* logger = spdlog::default_logger_raw(); logger != nullptr) {
      logger->flush();
    }
  } catch (...) {
    // Diagnostic failures must not replace communicator-scoped termination.
  }
  MPI_Abort(communicator, EXIT_FAILURE);
  std::abort();
}

[[nodiscard]] inline auto broadcast_population_size(
    MPI_Comm communicator,
    int root_estimate,
    bool easy_construction) noexcept -> int {
  auto population_size = root_estimate;
  auto const result = MPI_Bcast(&population_size, 1, MPI_INT, 0, communicator);
  if (result != MPI_SUCCESS) {
    abort_population_size_broadcast(communicator, result);
  }
  return clamp_population_size(population_size, easy_construction);
}
}  // namespace kahip::parallel_mh
