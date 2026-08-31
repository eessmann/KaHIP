#pragma once

#include <mpi.h>

#include <exception>
#include <functional>
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
}  // namespace parhip::mpi
