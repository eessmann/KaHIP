#include "mpi_application_runtime.h"

#include <cstdlib>
#include <exception>
#include <string_view>

#include "tools/fatal_diagnostics.h"

namespace kahip::mpi {
namespace {
[[noreturn]] void abort_without_mpi(int error_code,
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

[[noreturn]] void abort_backend(MPI_Comm communicator,
                                int error_code,
                                std::string_view boundary,
                                std::string_view operation,
                                int rank) noexcept {
  if (rank >= 0) {
    kahip::diagnostics::critical(
        "MPI backend failure: ", boundary, ": ", operation,
        " returned raw error ", error_code, " on rank ", rank);
  } else {
    kahip::diagnostics::critical(
        "MPI backend failure: ", boundary, ": ", operation,
        " returned raw error ", error_code, " (rank unavailable)");
  }
  static_cast<void>(MPI_Abort(communicator, EXIT_FAILURE));
  std::abort();
}
}  // namespace

void check_or_abort(int result,
                    MPI_Comm communicator,
                    std::string_view boundary,
                    std::string_view operation) noexcept {
  if (result == MPI_SUCCESS) {
    return;
  }
  auto rank = -1;
  static_cast<void>(PMPI_Comm_rank(communicator, &rank));
  abort_backend(communicator, result, boundary, operation, rank);
}

application_runtime::application_runtime(int& argument_count,
                                         char**& argument_values,
                                         std::string_view boundary)
    : boundary_(boundary) {
  auto const result = MPI_Init(&argument_count, &argument_values);
  if (result != MPI_SUCCESS) {
    abort_without_mpi(result, boundary_, "MPI_Init", -1);
  }
  auto const rank_result = MPI_Comm_rank(MPI_COMM_WORLD, &rank_);
  if (rank_result != MPI_SUCCESS) {
    abort_backend(MPI_COMM_WORLD, rank_result, boundary_,
                  "MPI_Comm_rank(application runtime)", -1);
  }
}

application_runtime::~application_runtime() noexcept {
  auto const result = MPI_Finalize();
  if (result != MPI_SUCCESS) {
    abort_without_mpi(result, boundary_, "MPI_Finalize", rank_);
  }
}

auto application_runtime::duplicate_operation_communicator() const noexcept
    -> MPI_Comm {
  auto communicator = MPI_COMM_NULL;
  auto const duplicate_result = MPI_Comm_dup(MPI_COMM_WORLD, &communicator);
  if (duplicate_result != MPI_SUCCESS) {
    abort_backend(MPI_COMM_WORLD, duplicate_result, boundary_,
                  "MPI_Comm_dup(application operation communicator)", rank_);
  }
  auto const handler_result =
      MPI_Comm_set_errhandler(communicator, MPI_ERRORS_RETURN);
  if (handler_result != MPI_SUCCESS) {
    abort_backend(
        communicator, handler_result, boundary_,
        "MPI_Comm_set_errhandler(application operation communicator)", rank_);
  }
  return communicator;
}

void application_runtime::free_operation_communicator(
    MPI_Comm communicator) const noexcept {
  auto owned = communicator;
  auto const result = MPI_Comm_free(&owned);
  if (result != MPI_SUCCESS) {
    abort_backend(MPI_COMM_WORLD, result, boundary_,
                  "MPI_Comm_free(application operation communicator)", rank_);
  }
}

[[noreturn]] void application_runtime::abort_on_exception(
    MPI_Comm communicator,
    std::exception_ptr exception) const noexcept {
  auto diagnostic = std::string{"unknown operation failure"};
  if (exception != nullptr) {
    try {
      std::rethrow_exception(exception);
    } catch (std::exception const& error) {
      diagnostic = error.what();
    } catch (...) {
      diagnostic = "non-standard operation exception";
    }
  }
  kahip::diagnostics::critical(boundary_, ": ", diagnostic, " (rank ", rank_,
                               ")");
  static_cast<void>(MPI_Abort(communicator, EXIT_FAILURE));
  std::abort();
}

}  // namespace kahip::mpi
