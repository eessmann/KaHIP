#include "communication/mpi_failure.h"

#include <array>
#include <cstdlib>
#include <exception>
#include <optional>
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

void flush_diagnostics() noexcept {
  try {
    if (auto* logger = spdlog::default_logger_raw(); logger != nullptr) {
      logger->flush();
    }
  } catch (...) {
    // Diagnostic flushing must never replace fail-fast termination.
  }
}

[[nodiscard]] auto active_rank(MPI_Comm communicator) noexcept
    -> std::optional<int> {
  auto const affected =
      communicator == MPI_COMM_NULL ? MPI_COMM_WORLD : communicator;
  auto rank = 0;
  return MPI_Comm_rank(affected, &rank) == MPI_SUCCESS
             ? std::optional<int>{rank}
             : std::nullopt;
}

[[noreturn]] void abort_on_lifecycle_query_failure(std::string_view query,
                                                   int error_code) noexcept {
  try {
    spdlog::critical("MPI lifecycle query failure: {} returned raw error {}",
                     query, error_code);
  } catch (...) {
    // Runtime state is unknown, so no further MPI call is safe even when
    // diagnostic formatting fails.
  }
  flush_diagnostics();
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
                 std::exception_ptr failure,
                 std::optional<int> rank) noexcept {
  try {
    if (failure == nullptr) {
      if (rank.has_value()) {
        spdlog::critical("{}: unknown unrecoverable failure (rank {})",
                         boundary, *rank);
      } else {
        spdlog::critical("{}: unknown unrecoverable failure", boundary);
      }
      return;
    }
    try {
      std::rethrow_exception(failure);
    } catch (std::exception const& error) {
      if (rank.has_value()) {
        spdlog::critical("{}: {} (rank {})", boundary, error.what(), *rank);
      } else {
        spdlog::critical("{}: {}", boundary, error.what());
      }
    } catch (...) {
      if (rank.has_value()) {
        spdlog::critical("{}: unknown unrecoverable exception (rank {})",
                         boundary, *rank);
      } else {
        spdlog::critical("{}: unknown unrecoverable exception", boundary);
      }
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
                            std::source_location location,
                            std::optional<int> world_rank) noexcept {
  auto error_text = std::array<char, MPI_MAX_ERROR_STRING>{};
  auto error_text_length = 0;
  auto const formatter_result =
      MPI_Error_string(error_code, error_text.data(), &error_text_length);
  try {
    if (formatter_result != MPI_SUCCESS) {
      spdlog::critical(
          "MPI backend failure: {} at {}:{} (original raw code {}, "
          "MPI_Error_string secondary raw code {}, world rank {})",
          context, location.file_name(), location.line(), error_code,
          formatter_result, world_rank.value_or(-1));
      return;
    }
    if (error_text_length < 0 ||
        static_cast<std::size_t>(error_text_length) > error_text.size()) {
      spdlog::critical(
          "MPI backend failure: {} at {}:{} (original raw code {}, "
          "MPI_Error_string invalid length {}, world rank {})",
          context, location.file_name(), location.line(), error_code,
          error_text_length, world_rank.value_or(-1));
      return;
    }
    spdlog::critical(
        "MPI backend failure: {} at {}:{} (original raw code {}, MPI text: "
        "{}, world rank {})",
        context, location.file_name(), location.line(), error_code,
        std::string_view{error_text.data(),
                         static_cast<std::size_t>(error_text_length)},
        world_rank.value_or(-1));
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
  auto const active = runtime_is_active();
  log_failure(boundary, failure,
              active ? active_rank(communicator) : std::nullopt);
  flush_diagnostics();
  if (active) {
    auto const affected =
        communicator == MPI_COMM_NULL ? MPI_COMM_WORLD : communicator;
    MPI_Abort(affected, EXIT_FAILURE);
  }
  std::abort();
}

[[noreturn]] void abort_on_mpi_error(MPI_Comm communicator,
                                     int error_code,
                                     std::string_view context,
                                     std::source_location location) noexcept {
  auto const mpi_is_active = runtime_is_active();
  if (mpi_is_active) {
    log_active_mpi_failure(error_code, context, location,
                           active_rank(MPI_COMM_WORLD));
  } else {
    // MPI_Error_string is itself an MPI call and is not valid before
    // initialization or after finalization. Retain the raw code instead.
    log_raw_mpi_failure(error_code, context, location);
  }

  flush_diagnostics();
  if (mpi_is_active) {
    auto const affected =
        communicator == MPI_COMM_NULL ? MPI_COMM_WORLD : communicator;
    MPI_Abort(affected, EXIT_FAILURE);
  }
  std::abort();
}

[[noreturn]] void abort_on_backend_failure(MPI_Comm communicator,
                                           std::string_view context) noexcept {
  auto const active = runtime_is_active();
  try {
    if (auto const rank = active ? active_rank(communicator) : std::nullopt;
        rank.has_value()) {
      spdlog::critical("Distributed backend failure: {} (rank {})", context,
                       *rank);
    } else {
      spdlog::critical("Distributed backend failure: {}", context);
    }
  } catch (...) {
    // Diagnostic failures must not replace the distributed fail-fast path.
  }
  flush_diagnostics();
  if (active) {
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
  auto const active = runtime_is_active();
  try {
    if (auto const rank = active ? active_rank(communicator) : std::nullopt;
        rank.has_value()) {
      spdlog::critical("MPI adapter programming failure: {} (rank {})",
                       context, *rank);
    } else {
      spdlog::critical("MPI adapter programming failure: {}", context);
    }
  } catch (...) {
    // State misuse must still terminate collectively when logging fails.
  }
  flush_diagnostics();
  if (active) {
    auto const affected =
        communicator == MPI_COMM_NULL ? MPI_COMM_WORLD : communicator;
    MPI_Abort(affected, EXIT_FAILURE);
  }
  std::abort();
}

[[noreturn]] void abort_on_capacity_failure(
    MPI_Comm communicator,
    std::string_view boundary,
    std::string_view issue_diagnostic) noexcept {
  auto const active = runtime_is_active();
  try {
    if (auto const rank = active ? active_rank(communicator) : std::nullopt;
        rank.has_value()) {
      spdlog::critical("MPI adapter capacity failure: {}: {} (rank {})",
                       boundary, issue_diagnostic, *rank);
    } else {
      spdlog::critical("MPI adapter capacity failure: {}: {}", boundary,
                       issue_diagnostic);
    }
  } catch (...) {
    // Logging and formatting failures must not replace capacity termination.
  }
  flush_diagnostics();
  if (active) {
    auto const affected =
        communicator == MPI_COMM_NULL ? MPI_COMM_WORLD : communicator;
    MPI_Abort(affected, EXIT_FAILURE);
  }
  std::abort();
}

auto resolve_capacity_collectively(capacity_result local,
                                   MPI_Comm convergence_communicator,
                                   MPI_Comm abort_communicator,
                                   std::string_view boundary) noexcept
    -> capacity_route {
  if (!runtime_is_active()) {
    abort_on_inactive_mpi_ownership("capacity resolution");
  }
  if (convergence_communicator == MPI_COMM_NULL) {
    abort_on_programming_error(
        abort_communicator,
        "capacity resolution requires a live convergence intracommunicator");
  }

  auto is_intercommunicator = 0;
  check_or_abort(
      MPI_Comm_test_inter(convergence_communicator, &is_intercommunicator),
      convergence_communicator, "MPI_Comm_test_inter(capacity resolution)");
  if (is_intercommunicator != 0) {
    abort_on_programming_error(
        abort_communicator,
        "capacity resolution requires a live convergence intracommunicator");
  }

  auto const local_masks = std::array{
      local.fatal_issues,
      local.bounded_fallback_issues,
  };
  auto global_masks = std::array<std::uint64_t, 2>{};
  check_or_abort(MPI_Allreduce(local_masks.data(), global_masks.data(),
                               static_cast<int>(global_masks.size()),
                               MPI_UINT64_T, MPI_BOR, convergence_communicator),
                 convergence_communicator,
                 "MPI_Allreduce(capacity resolution)");

  auto const global = capacity_result{
      .fatal_issues = global_masks[0],
      .bounded_fallback_issues = global_masks[1],
  };
  if (auto const fatal_issue = first_fatal_capacity_issue(global);
      fatal_issue.has_value()) {
    abort_on_capacity_failure(abort_communicator, boundary,
                              capacity_issue_diagnostic(*fatal_issue));
  }
  return global.bounded_fallback_issues != 0 ? capacity_route::bounded
                                             : capacity_route::direct;
}

[[noreturn]] void abort_on_inactive_mpi_ownership(
    std::string_view context) noexcept {
  try {
    spdlog::critical(
        "MPI adapter ownership outlived the active MPI runtime: {}", context);
  } catch (...) {
    // No MPI call is valid on this raw termination path.
  }
  flush_diagnostics();
  std::abort();
}
}  // namespace parhip::mpi
