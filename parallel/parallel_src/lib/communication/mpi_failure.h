#pragma once

#include <mpi.h>

#include <exception>
#include <functional>
#include <source_location>
#include <string_view>
#include <utility>

namespace parhip::mpi {
[[nodiscard]] auto runtime_is_active() noexcept -> bool;

template <typename Operation, typename OnFailure>
void run_with_exception_barrier(Operation&& operation,
                                OnFailure&& on_failure) noexcept {
  try {
    std::invoke(std::forward<Operation>(operation));
  } catch (...) {
    std::invoke(std::forward<OnFailure>(on_failure),
                std::current_exception());
  }
}

[[noreturn]] void abort_on_exception(
    MPI_Comm communicator,
    std::string_view boundary,
    std::exception_ptr failure = std::current_exception()) noexcept;

[[noreturn]] void abort_on_mpi_error(
    MPI_Comm communicator,
    int error_code,
    std::string_view context,
    std::source_location location = std::source_location::current()) noexcept;

[[noreturn]] void abort_on_programming_error(
    MPI_Comm communicator,
    std::string_view context) noexcept;

[[noreturn]] void abort_on_inactive_mpi_ownership(
    std::string_view context) noexcept;

inline void check_or_abort(
    int error_code,
    MPI_Comm communicator,
    std::string_view context,
    std::source_location location = std::source_location::current()) noexcept {
  if (error_code != MPI_SUCCESS) {
    abort_on_mpi_error(communicator, error_code, context, location);
  }
}
}  // namespace parhip::mpi
