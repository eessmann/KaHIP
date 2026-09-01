#pragma once

#include <mpi.h>

#include <concepts>
#include <exception>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace kahip::mpi {

void check_or_abort(int result,
                    MPI_Comm communicator,
                    std::string_view boundary,
                    std::string_view operation) noexcept;

// Root KaHIP executables own their MPI lifecycle. The duplicated operation
// communicator and every object created from it are destroyed before Finalize.
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
    requires std::invocable<Operation&&, MPI_Comm> &&
             std::same_as<std::invoke_result_t<Operation&&, MPI_Comm>, int>
  [[nodiscard]] auto execute(Operation&& operation) noexcept -> int {
    auto operation_communicator = duplicate_operation_communicator();
    try {
      auto const result =
          std::invoke(std::forward<Operation>(operation),
                      operation_communicator);
      free_operation_communicator(operation_communicator);
      return result;
    } catch (...) {
      abort_on_exception(operation_communicator, std::current_exception());
    }
  }

 private:
  [[nodiscard]] auto duplicate_operation_communicator() const noexcept
      -> MPI_Comm;
  void free_operation_communicator(MPI_Comm communicator) const noexcept;
  [[noreturn]] void abort_on_exception(
      MPI_Comm communicator,
      std::exception_ptr exception) const noexcept;

  std::string boundary_;
  int rank_ = -1;
};

}  // namespace kahip::mpi
