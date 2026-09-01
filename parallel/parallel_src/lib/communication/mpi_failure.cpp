#include "communication/mpi_failure.h"

#include <array>
#include <cstdlib>
#include <exception>
#include <string>

#include <spdlog/spdlog.h>

#include "communication/mpi_error.h"

namespace parhip::mpi {
namespace {
enum class runtime_state {
  before_initialization,
  active,
  finalized,
};

[[noreturn]] void abort_on_lifecycle_query_failure(
    std::string_view query,
    int error_code) noexcept {
  try {
    spdlog::critical(
        "MPI lifecycle query failure: {} returned raw error {}",
        query,
        error_code);
  } catch (...) {
    // Runtime state is unknown, so no further MPI call is safe even when
    // diagnostic formatting fails.
  }
  std::abort();
}

[[nodiscard]] auto query_runtime_state() noexcept -> runtime_state {
  int initialized = 0;
  int finalized = 0;
  auto const initialized_result = MPI_Initialized(&initialized);
  if (initialized_result != MPI_SUCCESS) {
    abort_on_lifecycle_query_failure("MPI_Initialized", initialized_result);
  }
  if (initialized == 0) {
    return runtime_state::before_initialization;
  }
  auto const finalized_result = MPI_Finalized(&finalized);
  if (finalized_result != MPI_SUCCESS) {
    abort_on_lifecycle_query_failure("MPI_Finalized", finalized_result);
  }
  return finalized == 0 ? runtime_state::active : runtime_state::finalized;
}

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

void log_raw_mpi_failure(int error_code,
                         std::string_view context,
                         std::source_location location) noexcept {
  try {
    spdlog::critical("MPI backend failure: {} at {}:{} (original raw code {})",
                     context, location.file_name(), location.line(),
                     error_code);
  } catch (...) {
    // Failure reporting must never replace the original termination path.
  }
}

void log_active_mpi_failure(int error_code,
                            std::string_view context,
                            std::source_location location) noexcept {
  auto error_text = std::array<char, MPI_MAX_ERROR_STRING>{};
  auto error_text_length = 0;
  auto const formatter_result =
      MPI_Error_string(error_code, error_text.data(), &error_text_length);
  try {
    if (formatter_result != MPI_SUCCESS) {
      spdlog::critical(
          "MPI backend failure: {} at {}:{} (original raw code {}, "
          "MPI_Error_string secondary raw code {})",
          context, location.file_name(), location.line(), error_code,
          formatter_result);
      return;
    }
    if (error_text_length < 0 ||
        static_cast<std::size_t>(error_text_length) > error_text.size()) {
      spdlog::critical(
          "MPI backend failure: {} at {}:{} (original raw code {}, "
          "MPI_Error_string invalid length {})",
          context, location.file_name(), location.line(), error_code,
          error_text_length);
      return;
    }
    spdlog::critical(
        "MPI backend failure: {} at {}:{} (original raw code {}, MPI text: "
        "{})",
        context, location.file_name(), location.line(), error_code,
        std::string_view{error_text.data(),
                         static_cast<std::size_t>(error_text_length)});
  } catch (...) {
    // Failure reporting must never replace the original termination path.
  }
}
}  // namespace

auto runtime_is_active() noexcept -> bool {
  return query_runtime_state() == runtime_state::active;
}

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

[[noreturn]] void abort_on_mpi_error(
    MPI_Comm communicator,
    int error_code,
    std::string_view context,
    std::source_location location) noexcept {
  auto const mpi_is_active = runtime_is_active();
  if (mpi_is_active) {
    log_active_mpi_failure(error_code, context, location);
  } else {
    // MPI_Error_string is itself an MPI call and is not valid before
    // initialization or after finalization. Retain the raw code instead.
    log_raw_mpi_failure(error_code, context, location);
  }

  if (mpi_is_active) {
    auto const affected =
        communicator == MPI_COMM_NULL ? MPI_COMM_WORLD : communicator;
    MPI_Abort(affected, EXIT_FAILURE);
  }
  std::abort();
}

[[noreturn]] void throw_collectively_agreed_semantic_error(
    MPI_Comm communicator,
    std::string_view context,
    std::source_location location) {
  detail::throw_collectively_agreed_semantic_error_from(
      communicator, [context, location]() -> mpi_error {
        return mpi_error{MPI_ERR_ARG, std::string{context}, location};
      });
}

[[noreturn]] void abort_on_programming_error(
    MPI_Comm communicator,
    std::string_view context) noexcept {
  try {
    spdlog::critical("MPI adapter programming failure: {}", context);
  } catch (...) {
    // State misuse must still terminate collectively when logging fails.
  }
  if (runtime_is_active()) {
    auto const affected =
        communicator == MPI_COMM_NULL ? MPI_COMM_WORLD : communicator;
    MPI_Abort(affected, EXIT_FAILURE);
  }
  std::abort();
}

[[noreturn]] void abort_on_inactive_mpi_ownership(
    std::string_view context) noexcept {
  try {
    spdlog::critical(
        "MPI adapter ownership outlived the active MPI runtime: {}", context);
  } catch (...) {
    // No MPI call is valid on this raw termination path.
  }
  std::abort();
}
}  // namespace parhip::mpi
