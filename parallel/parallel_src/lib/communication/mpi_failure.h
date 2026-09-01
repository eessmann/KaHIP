#pragma once

#include <mpi.h>

#include <concepts>
#include <cstdint>
#include <exception>
#include <functional>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "communication/mpi_error.h"

namespace parhip::mpi {
enum class capacity_issue : std::uint64_t {
  received_count_not_representable = std::uint64_t{1} << 0,
  cumulative_offset_overflow = std::uint64_t{1} << 1,
  storage_byte_size_overflow = std::uint64_t{1} << 2,
  topology_degree_not_representable = std::uint64_t{1} << 3,
  collective_layout_not_representable = std::uint64_t{1} << 4,
  direct_backend_not_representable = std::uint64_t{1} << 5,
  bounded_round_arithmetic_overflow = std::uint64_t{1} << 6,
};

enum class capacity_route : std::uint8_t {
  direct,
  bounded,
};

struct capacity_result final {
  std::uint64_t fatal_issues = 0;
  std::uint64_t bounded_fallback_issues = 0;

  auto operator==(capacity_result const&) const -> bool = default;
};

static_assert(std::is_trivially_copyable_v<capacity_result>);
static_assert(std::is_standard_layout_v<capacity_result>);

[[nodiscard]] constexpr auto capacity_issue_mask(capacity_issue issue) noexcept
    -> std::uint64_t {
  return static_cast<std::uint64_t>(issue);
}

[[nodiscard]] constexpr auto capacity_issue_diagnostic(
    capacity_issue issue) noexcept -> std::string_view {
  switch (issue) {
    case capacity_issue::received_count_not_representable:
      return "received element count exceeds local size_t capacity";
    case capacity_issue::cumulative_offset_overflow:
      return "cumulative element offset exceeds local size_t capacity";
    case capacity_issue::storage_byte_size_overflow:
      return "element storage byte size exceeds local size_t capacity";
    case capacity_issue::topology_degree_not_representable:
      return "distributed graph outdegree exceeds MPI int capacity";
    case capacity_issue::collective_layout_not_representable:
      return "collective payload layout has no representable MPI backend";
    case capacity_issue::direct_backend_not_representable:
      return "direct neighborhood payload has no representable MPI backend";
    case capacity_issue::bounded_round_arithmetic_overflow:
      return "bounded MPI-3 chunk arithmetic exceeds local size_t capacity";
  }
  return "unknown capacity issue";
}

[[nodiscard]] constexpr auto with_fatal_capacity_issue(
    capacity_result result,
    capacity_issue issue) noexcept -> capacity_result {
  result.fatal_issues |= capacity_issue_mask(issue);
  return result;
}

[[nodiscard]] constexpr auto with_bounded_capacity_issue(
    capacity_result result,
    capacity_issue issue) noexcept -> capacity_result {
  result.bounded_fallback_issues |= capacity_issue_mask(issue);
  return result;
}

[[nodiscard]] constexpr auto has_fatal_capacity_issue(
    capacity_result result,
    capacity_issue issue) noexcept -> bool {
  return (result.fatal_issues & capacity_issue_mask(issue)) != 0;
}

[[nodiscard]] constexpr auto has_bounded_capacity_issue(
    capacity_result result,
    capacity_issue issue) noexcept -> bool {
  return (result.bounded_fallback_issues & capacity_issue_mask(issue)) != 0;
}

[[nodiscard]] constexpr auto first_fatal_capacity_issue(
    capacity_result result) noexcept -> std::optional<capacity_issue> {
  if (result.fatal_issues == 0) {
    return std::nullopt;
  }
  auto const lowest_issue =
      result.fatal_issues & (~result.fatal_issues + std::uint64_t{1});
  return static_cast<capacity_issue>(lowest_issue);
}

[[nodiscard]] constexpr auto capacity_route_for(capacity_result result) noexcept
    -> std::optional<capacity_route> {
  if (result.fatal_issues != 0) {
    return std::nullopt;
  }
  return result.bounded_fallback_issues != 0 ? capacity_route::bounded
                                             : capacity_route::direct;
}

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

[[noreturn]] void abort_on_capacity_failure(
    MPI_Comm communicator,
    std::string_view boundary,
    std::string_view issue_diagnostic) noexcept;

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

// convergence_communicator must be a live intracommunicator. Invalid runtime,
// null, or intercommunicator use terminates before the payload-free reduction.
// Every valid caller performs exactly one two-mask MPI_BOR reduction.
[[nodiscard]] auto resolve_capacity_collectively(
    capacity_result local,
    MPI_Comm convergence_communicator,
    MPI_Comm abort_communicator,
    std::string_view boundary) noexcept -> capacity_route;

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
