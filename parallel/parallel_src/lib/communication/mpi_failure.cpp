#include "communication/mpi_failure.h"

#include <array>
#include <cstdlib>
#include <exception>
#include <optional>
#include <string>

#include "communication/mpi_error.h"
#include "tools/fatal_diagnostics.h"

namespace parhip::mpi {
namespace {
enum class runtime_state {
  before_initialization,
  active,
  finalized,
};

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
  kahip::diagnostics::critical(
      "MPI lifecycle query failure: ", query, " returned raw error ",
      error_code);
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
  if (failure == nullptr) {
    if (rank.has_value()) {
      kahip::diagnostics::critical(
          boundary, ": unknown unrecoverable failure (rank ", *rank, ")");
    } else {
      kahip::diagnostics::critical(boundary,
                                   ": unknown unrecoverable failure");
    }
    return;
  }
  try {
    std::rethrow_exception(failure);
  } catch (std::exception const& error) {
    if (rank.has_value()) {
      kahip::diagnostics::critical(boundary, ": ", error.what(), " (rank ",
                                   *rank, ")");
    } else {
      kahip::diagnostics::critical(boundary, ": ", error.what());
    }
  } catch (...) {
    if (rank.has_value()) {
      kahip::diagnostics::critical(
          boundary, ": unknown unrecoverable exception (rank ", *rank, ")");
    } else {
      kahip::diagnostics::critical(boundary,
                                   ": unknown unrecoverable exception");
    }
  }
}

void log_raw_mpi_failure(int error_code,
                         std::string_view context,
                         std::source_location location) noexcept {
  kahip::diagnostics::critical(
      "MPI backend failure: ", context, " at ", location.file_name(), ":",
      location.line(), " (original raw code ", error_code, ")");
}

void log_active_mpi_failure(int error_code,
                            std::string_view context,
                            std::source_location location,
                            std::optional<int> world_rank) noexcept {
  auto error_text = std::array<char, MPI_MAX_ERROR_STRING>{};
  auto error_text_length = 0;
  auto const formatter_result =
      MPI_Error_string(error_code, error_text.data(), &error_text_length);
  if (formatter_result != MPI_SUCCESS) {
    kahip::diagnostics::critical(
        "MPI backend failure: ", context, " at ", location.file_name(), ":",
        location.line(), " (original raw code ", error_code,
        ", MPI_Error_string secondary raw code ", formatter_result,
        ", world rank ", world_rank.value_or(-1), ")");
    return;
  }
  if (error_text_length < 0 ||
      static_cast<std::size_t>(error_text_length) > error_text.size()) {
    kahip::diagnostics::critical(
        "MPI backend failure: ", context, " at ", location.file_name(), ":",
        location.line(), " (original raw code ", error_code,
        ", MPI_Error_string invalid length ", error_text_length,
        ", world rank ", world_rank.value_or(-1), ")");
    return;
  }
  kahip::diagnostics::critical(
      "MPI backend failure: ", context, " at ", location.file_name(), ":",
      location.line(), " (original raw code ", error_code, ", MPI text: ",
      std::string_view{error_text.data(),
                       static_cast<std::size_t>(error_text_length)},
      ", world rank ", world_rank.value_or(-1), ")");
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
  if (auto const rank = active ? active_rank(communicator) : std::nullopt;
      rank.has_value()) {
    kahip::diagnostics::critical(
        "Distributed backend failure: ", context, " (rank ", *rank, ")");
  } else {
    kahip::diagnostics::critical("Distributed backend failure: ", context);
  }
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
  if (auto const rank = active ? active_rank(communicator) : std::nullopt;
      rank.has_value()) {
    kahip::diagnostics::critical(
        "MPI adapter programming failure: ", context, " (rank ", *rank,
        ")");
  } else {
    kahip::diagnostics::critical("MPI adapter programming failure: ",
                                 context);
  }
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
  if (auto const rank = active ? active_rank(communicator) : std::nullopt;
      rank.has_value()) {
    kahip::diagnostics::critical(
        "MPI adapter capacity failure: ", boundary, ": ", issue_diagnostic,
        " (rank ", *rank, ")");
  } else {
    kahip::diagnostics::critical(
        "MPI adapter capacity failure: ", boundary, ": ", issue_diagnostic);
  }
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
  kahip::diagnostics::critical(
      "MPI adapter ownership outlived the active MPI runtime: ", context);
  std::abort();
}
}  // namespace parhip::mpi
