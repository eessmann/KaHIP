#pragma once

#include <mpi.h>

#include <concepts>
#include <exception>
#include <functional>
#include <source_location>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "communication/mpi_error.h"

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

namespace detail {
template <typename Factory>
concept semantic_error_factory =
    std::invocable<Factory&&> &&
    std::same_as<std::invoke_result_t<Factory&&>, mpi_error>;

template <semantic_error_factory Factory>
[[noreturn]] void throw_collectively_agreed_semantic_error_from(
    MPI_Comm communicator,
    Factory&& factory) {
  auto structured_error = std::exception_ptr{};
  try {
    structured_error =
        std::make_exception_ptr(std::invoke(std::forward<Factory>(factory)));
    if (structured_error == nullptr) {
      abort_on_exception(communicator, "MPI semantic error construction", {});
    }
    try {
      std::rethrow_exception(structured_error);
    } catch (mpi_error const&) {
      // make_exception_ptr is noexcept and may store a copy/allocation failure
      // instead of the requested type. Only the intended structured error may
      // leave this construction barrier.
    } catch (...) {
      abort_on_exception(communicator, "MPI semantic error construction",
                         std::current_exception());
    }
  } catch (...) {
    abort_on_exception(communicator, "MPI semantic error construction",
                       std::current_exception());
  }
  std::rethrow_exception(structured_error);
}
}  // namespace detail

// Precondition: every rank in communicator has already reached the same
// invalid semantic decision and no payload or externally visible state has
// been mutated. This helper performs no hidden collective.
[[noreturn]] void throw_collectively_agreed_semantic_error(
    MPI_Comm communicator,
    std::string_view context,
    std::source_location location = std::source_location::current());

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
