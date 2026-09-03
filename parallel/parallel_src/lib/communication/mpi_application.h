#pragma once

#include <mpi.h>

#include <concepts>
#include <exception>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "communication/mpi_failure.h"
#include "communication/mpi_handles.h"

namespace parhip::mpi {
// Executables own the MPI runtime. Libraries only receive the duplicated
// operation communicator created by execute(), and all objects using that
// communicator are destroyed before this runtime finalizes MPI.
class application_runtime final {
 public:
  application_runtime(int& argument_count,
                      char**& argument_values,
                      std::string_view boundary);
  ~application_runtime() noexcept;

  application_runtime(application_runtime const&) = delete;
  auto operator=(application_runtime const&) -> application_runtime& = delete;
  application_runtime(application_runtime&&) = delete;
  auto operator=(application_runtime&&) -> application_runtime& = delete;

  template <typename Operation>
    requires std::invocable<Operation&&, communicator_view> &&
             std::same_as<std::invoke_result_t<Operation&&, communicator_view>,
                          int>
  [[nodiscard]] auto execute(Operation&& operation) noexcept -> int {
    communicator operation_communicator{communicator_view{MPI_COMM_WORLD}};
    try {
      return std::invoke(std::forward<Operation>(operation),
                         operation_communicator.view());
    } catch (...) {
      abort_on_exception(operation_communicator.native_handle(), boundary_,
                         std::current_exception());
    }
  }

 private:
  std::string boundary_;
  int rank_ = -1;
};
}  // namespace parhip::mpi
