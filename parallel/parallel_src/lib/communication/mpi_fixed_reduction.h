#pragma once

#include <mpi.h>

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "communication/mpi_failure.h"
#include "communication/mpi_handles.h"
#include "communication/mpi_types.h"
#include "kahip_mpi_capabilities.h"

namespace parhip::mpi {
enum class reduction_kind : std::uint8_t {
  sum,
  minimum,
  maximum,
};

struct reduction_options final {
  std::size_t mpi3_round_ceiling =
      static_cast<std::size_t>(std::numeric_limits<int>::max());
  bool force_mpi3 = false;
};

template <typename T>
concept mpi_integral_reduction_datatype =
    mpi_native_datatype<T> &&
    std::integral<std::remove_cv_t<std::remove_reference_t<T>>> &&
    (!std::is_same_v<std::remove_cv_t<std::remove_reference_t<T>>, bool>);

namespace detail {
[[nodiscard]] constexpr auto reduction_operation(reduction_kind kind) noexcept
    -> MPI_Op {
  switch (kind) {
    case reduction_kind::sum:
      return MPI_SUM;
    case reduction_kind::minimum:
      return MPI_MIN;
    case reduction_kind::maximum:
      return MPI_MAX;
  }
  return MPI_OP_NULL;
}

inline void validate_reduction_parameters(std::size_t send_count,
                                          std::size_t receive_count,
                                          bool buffers_are_distinct,
                                          reduction_kind kind,
                                          std::optional<int> root,
                                          communicator_view communicator,
                                          reduction_options options) noexcept {
  static_assert(sizeof(std::size_t) <= sizeof(std::uint64_t));
  constexpr auto all_reduce_root = std::numeric_limits<std::uint64_t>::max();
  constexpr auto invalid_reduce_root = all_reduce_root - 1;
  auto const encoded_root = !root.has_value() ? all_reduce_root
                            : *root >= 0 ? static_cast<std::uint64_t>(*root)
                                         : invalid_reduce_root;
  auto const local = std::array<std::uint64_t, 7>{
      static_cast<std::uint64_t>(send_count),
      static_cast<std::uint64_t>(receive_count),
      static_cast<std::uint64_t>(options.mpi3_round_ceiling),
      options.force_mpi3 ? std::uint64_t{1} : std::uint64_t{0},
      static_cast<std::uint64_t>(kind),
      encoded_root,
      buffers_are_distinct ? std::uint64_t{1} : std::uint64_t{0}};
  auto minimum = std::array<std::uint64_t, local.size()>{};
  auto maximum = std::array<std::uint64_t, local.size()>{};
  check_or_abort(MPI_Allreduce(local.data(), minimum.data(),
                               static_cast<int>(local.size()), MPI_UINT64_T,
                               MPI_MIN, communicator.native_handle()),
                 communicator.native_handle(),
                 "MPI_Allreduce(bounded reduction signature minimum)");
  check_or_abort(MPI_Allreduce(local.data(), maximum.data(),
                               static_cast<int>(local.size()), MPI_UINT64_T,
                               MPI_MAX, communicator.native_handle()),
                 communicator.native_handle(),
                 "MPI_Allreduce(bounded reduction signature maximum)");

  if (minimum != maximum) {
    abort_on_programming_error(
        communicator.native_handle(),
        "bounded reduction arguments differ across communicator");
  }
  auto const encoded_kind = static_cast<std::uint64_t>(kind);
  auto const root_is_valid =
      !root.has_value() || (*root >= 0 && *root < communicator.size());
  if (send_count != receive_count || !buffers_are_distinct ||
      options.mpi3_round_ceiling == 0 ||
      encoded_kind > static_cast<std::uint64_t>(reduction_kind::maximum) ||
      !root_is_valid) {
    abort_on_programming_error(communicator.native_handle(),
                               "bounded reduction arguments are invalid");
  }
}

template <mpi_integral_reduction_datatype T,
          typename LargeCountCollective,
          typename LegacyCollective>
void reduce_bounded_impl(std::span<T const> local_values,
                         std::span<T> reduced_values,
                         reduction_kind kind,
                         std::optional<int> root,
                         communicator_view communicator,
                         std::string_view context,
                         reduction_options options,
                         bool large_count_available,
                         LargeCountCollective&& large_count_collective,
                         LegacyCollective&& legacy_collective) noexcept {
  require_live_intracommunicator(
      communicator, "bounded reduction requires a live intracommunicator");
  auto const buffers_are_distinct =
      local_values.empty() || local_values.data() != reduced_values.data();
  validate_reduction_parameters(local_values.size(), reduced_values.size(),
                                buffers_are_distinct, kind, root, communicator,
                                options);
  auto ignored_local_value = T{};
  auto ignored_reduced_value = T{};
  auto const* local_data =
      local_values.empty() ? &ignored_local_value : local_values.data();
  auto* reduced_data =
      reduced_values.empty() ? &ignored_reduced_value : reduced_values.data();
  auto const operation = reduction_operation(kind);
  if (large_count_available && !options.force_mpi3 &&
      std::in_range<MPI_Count>(local_values.size())) {
    check_or_abort(std::invoke(large_count_collective, local_data, reduced_data,
                               static_cast<MPI_Count>(local_values.size()),
                               operation, communicator.native_handle()),
                   communicator.native_handle(), context);
    return;
  }

  auto const ceiling =
      std::min(options.mpi3_round_ceiling,
               static_cast<std::size_t>(std::numeric_limits<int>::max()));
  if (local_values.empty()) {
    check_or_abort(std::invoke(legacy_collective, local_data, reduced_data, 0,
                               operation, communicator.native_handle()),
                   communicator.native_handle(), context);
    return;
  }
  for (std::size_t offset = 0; offset < local_values.size();) {
    auto const count = std::min(ceiling, local_values.size() - offset);
    check_or_abort(std::invoke(legacy_collective, local_data + offset,
                               reduced_data + offset, static_cast<int>(count),
                               operation, communicator.native_handle()),
                   communicator.native_handle(), context);
    offset += count;
  }
}
}  // namespace detail

template <mpi_integral_reduction_datatype T,
          std::size_t LocalExtent,
          std::size_t GlobalExtent>
void all_reduce_bounded(std::span<T const, LocalExtent> local_values,
                        std::span<T, GlobalExtent> global_values,
                        reduction_kind kind,
                        communicator_view communicator,
                        std::string_view context,
                        reduction_options options = {}) noexcept {
#if KAHIP_HAVE_MPI_ALLREDUCE_C
  constexpr auto large_count_available = true;
  auto const large_count_collective =
      [](void const* send_buffer, void* receive_buffer, MPI_Count count,
         MPI_Op operation, MPI_Comm native_communicator) noexcept {
        return MPI_Allreduce_c(send_buffer, receive_buffer, count,
                               get_mpi_datatype<T>(), operation,
                               native_communicator);
      };
#else
  constexpr auto large_count_available = false;
  auto const large_count_collective = [](void const*, void*, MPI_Count, MPI_Op,
                                         MPI_Comm) noexcept {
    return MPI_ERR_OTHER;
  };
#endif
  auto const legacy_collective = [](void const* send_buffer,
                                    void* receive_buffer, int count,
                                    MPI_Op operation,
                                    MPI_Comm native_communicator) noexcept {
    return MPI_Allreduce(send_buffer, receive_buffer, count,
                         get_mpi_datatype<T>(), operation, native_communicator);
  };
  detail::reduce_bounded_impl(
      std::span<T const>{local_values.data(), local_values.size()},
      std::span<T>{global_values.data(), global_values.size()}, kind,
      std::nullopt, communicator, context, options, large_count_available,
      large_count_collective, legacy_collective);
}

template <mpi_integral_reduction_datatype T,
          std::size_t LocalExtent,
          std::size_t GlobalExtent>
  requires std::unsigned_integral<std::remove_cv_t<T>>
void all_reduce_checked_sum(std::span<T const, LocalExtent> local_values,
                            std::span<T, GlobalExtent> global_values,
                            communicator_view communicator,
                            std::string_view context,
                            std::string_view overflow_boundary,
                            std::string_view overflow_diagnostic,
                            reduction_options options = {}) noexcept {
  require_live_intracommunicator(
      communicator, "checked sum reduction requires a live intracommunicator");
  auto const process_count = communicator.size();
  using value_type = std::remove_cv_t<T>;
  constexpr auto maximum = std::numeric_limits<value_type>::max();
  if (process_count <= 0 || !std::in_range<value_type>(process_count)) {
    abort_on_capacity_failure(
        communicator.native_handle(), overflow_boundary,
        "communicator size exceeds checked-sum arithmetic capacity");
  }
  auto const radix = static_cast<value_type>(process_count);
  if (radix > maximum / radix) {
    abort_on_capacity_failure(
        communicator.native_handle(), overflow_boundary,
        "communicator size squared exceeds checked-sum arithmetic capacity");
  }

  try {
    auto local_quotients = std::vector<value_type>(local_values.size());
    auto local_remainders = std::vector<value_type>(local_values.size());
    std::ranges::transform(local_values, local_quotients.begin(),
                           [radix](value_type value) { return value / radix; });
    std::ranges::transform(local_values, local_remainders.begin(),
                           [radix](value_type value) { return value % radix; });

    auto global_quotients = std::vector<value_type>(global_values.size());
    auto global_remainders = std::vector<value_type>(global_values.size());
    all_reduce_bounded(std::span<value_type const>{local_quotients},
                       std::span<value_type>{global_quotients},
                       reduction_kind::sum, communicator, context, options);
    all_reduce_bounded(std::span<value_type const>{local_remainders},
                       std::span<value_type>{global_remainders},
                       reduction_kind::sum, communicator, context, options);

    // For P ranks and radix P, every quotient sum is at most
    // P*floor(max/P), and every remainder sum is below P^2.  Both component
    // reductions therefore remain representable.  Only the exact
    // reconstruction can overflow the destination type.
    auto result = std::vector<value_type>(global_values.size());
    for (std::size_t index = 0; index < result.size(); ++index) {
      auto const quotient = global_quotients[index];
      auto const remainder = global_remainders[index];
      if (quotient > (maximum - remainder) / radix) {
        abort_on_capacity_failure(communicator.native_handle(),
                                  overflow_boundary, overflow_diagnostic);
      }
      result[index] = quotient * radix + remainder;
    }
    std::ranges::copy(result, global_values.begin());
  } catch (...) {
    abort_on_exception(communicator.native_handle(), context);
  }
}

template <mpi_integral_reduction_datatype T,
          std::size_t LocalExtent,
          std::size_t RootExtent>
void reduce_bounded(std::span<T const, LocalExtent> local_values,
                    std::span<T, RootExtent> root_values,
                    reduction_kind kind,
                    int root,
                    communicator_view communicator,
                    std::string_view context,
                    reduction_options options = {}) noexcept {
#if KAHIP_HAVE_MPI_REDUCE_C
  constexpr auto large_count_available = true;
  auto const large_count_collective =
      [root](void const* send_buffer, void* receive_buffer, MPI_Count count,
             MPI_Op operation, MPI_Comm native_communicator) noexcept {
        return MPI_Reduce_c(send_buffer, receive_buffer, count,
                            get_mpi_datatype<T>(), operation, root,
                            native_communicator);
      };
#else
  constexpr auto large_count_available = false;
  auto const large_count_collective = [](void const*, void*, MPI_Count, MPI_Op,
                                         MPI_Comm) noexcept {
    return MPI_ERR_OTHER;
  };
#endif
  auto const legacy_collective = [root](void const* send_buffer,
                                        void* receive_buffer, int count,
                                        MPI_Op operation,
                                        MPI_Comm native_communicator) noexcept {
    return MPI_Reduce(send_buffer, receive_buffer, count, get_mpi_datatype<T>(),
                      operation, root, native_communicator);
  };
  detail::reduce_bounded_impl(
      std::span<T const>{local_values.data(), local_values.size()},
      std::span<T>{root_values.data(), root_values.size()}, kind, root,
      communicator, context, options, large_count_available,
      large_count_collective, legacy_collective);
}

template <mpi_native_datatype T>
  requires(!std::is_same_v<std::remove_cv_t<T>, bool>)
[[nodiscard]] auto all_reduce_sum(T local_value,
                                  communicator_view communicator,
                                  std::string_view context) noexcept -> T {
  require_live_intracommunicator(
      communicator, "fixed reduction requires a live intracommunicator");
  auto global_value = T{};
  check_or_abort(
      MPI_Allreduce(&local_value, &global_value, 1, get_mpi_datatype<T>(),
                    MPI_SUM, communicator.native_handle()),
      communicator.native_handle(), context);
  return global_value;
}
}  // namespace parhip::mpi
