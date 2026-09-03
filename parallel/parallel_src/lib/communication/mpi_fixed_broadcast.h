#pragma once

#include <mpi.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <utility>

#include "communication/mpi_failure.h"
#include "communication/mpi_handles.h"
#include "communication/mpi_types.h"

namespace parhip::mpi {
struct broadcast_options final {
  std::size_t mpi3_round_ceiling =
      static_cast<std::size_t>(std::numeric_limits<int>::max());
  bool force_mpi3 = false;
};

namespace detail {
inline void validate_broadcast_parameters(std::size_t count,
                                          int root,
                                          communicator_view communicator,
                                          broadcast_options options) noexcept {
  static_assert(sizeof(std::size_t) <= sizeof(std::uint64_t));
  auto const encoded_root = root >= 0
                                ? static_cast<std::uint64_t>(root)
                                : std::numeric_limits<std::uint64_t>::max();
  auto const local = std::array<std::uint64_t, 4>{
      static_cast<std::uint64_t>(count),
      static_cast<std::uint64_t>(options.mpi3_round_ceiling),
      options.force_mpi3 ? std::uint64_t{1} : std::uint64_t{0}, encoded_root};
  auto minimum = std::array<std::uint64_t, 4>{};
  auto maximum = std::array<std::uint64_t, 4>{};
  check_or_abort(MPI_Allreduce(local.data(), minimum.data(),
                               static_cast<int>(local.size()), MPI_UINT64_T,
                               MPI_MIN, communicator.native_handle()),
                 communicator.native_handle(),
                 "MPI_Allreduce(bounded broadcast signature minimum)");
  check_or_abort(MPI_Allreduce(local.data(), maximum.data(),
                               static_cast<int>(local.size()), MPI_UINT64_T,
                               MPI_MAX, communicator.native_handle()),
                 communicator.native_handle(),
                 "MPI_Allreduce(bounded broadcast signature maximum)");

  if (minimum != maximum) {
    abort_on_programming_error(
        communicator.native_handle(),
        "bounded broadcast arguments differ across communicator");
  }
  if (options.mpi3_round_ceiling == 0 || root < 0 ||
      root >= communicator.size()) {
    abort_on_programming_error(communicator.native_handle(),
                               "bounded broadcast arguments are invalid");
  }
}
}  // namespace detail

template <mpi_native_datatype T, std::size_t Extent>
  requires(Extent != std::dynamic_extent &&
           Extent <= static_cast<std::size_t>(std::numeric_limits<int>::max()))
void broadcast_fixed(std::span<T, Extent> values,
                     int root,
                     communicator_view communicator,
                     std::string_view context) noexcept {
  check_or_abort(
      MPI_Bcast(values.data(), static_cast<int>(Extent), get_mpi_datatype<T>(),
                root, communicator.native_handle()),
      communicator.native_handle(), context);
}

template <mpi_native_datatype T>
void broadcast_fixed(T& value,
                     int root,
                     communicator_view communicator,
                     std::string_view context) noexcept {
  broadcast_fixed(std::span<T, 1>{&value, 1}, root, communicator, context);
}

template <mpi_native_datatype T>
void broadcast_bounded(std::span<T> values,
                       int root,
                       communicator_view communicator,
                       std::string_view context,
                       broadcast_options options = {}) noexcept {
  require_live_intracommunicator(
      communicator, "bounded broadcast requires a live intracommunicator");
  detail::validate_broadcast_parameters(values.size(), root, communicator,
                                        options);
#if KAHIP_HAVE_MPI_BCAST_C
  if (!options.force_mpi3 && std::in_range<MPI_Count>(values.size())) {
    check_or_abort(
        MPI_Bcast_c(values.data(), static_cast<MPI_Count>(values.size()),
                    get_mpi_datatype<T>(), root, communicator.native_handle()),
        communicator.native_handle(), context);
    return;
  }
#endif

  auto const ceiling =
      std::min(options.mpi3_round_ceiling,
               static_cast<std::size_t>(std::numeric_limits<int>::max()));
  if (values.empty()) {
    check_or_abort(MPI_Bcast(values.data(), 0, get_mpi_datatype<T>(), root,
                             communicator.native_handle()),
                   communicator.native_handle(), context);
    return;
  }
  for (std::size_t offset = 0; offset < values.size();) {
    auto const chunk = std::min(ceiling, values.size() - offset);
    check_or_abort(
        MPI_Bcast(values.data() + offset, static_cast<int>(chunk),
                  get_mpi_datatype<T>(), root, communicator.native_handle()),
        communicator.native_handle(), context);
    offset += chunk;
  }
}

template <mpi_native_datatype EdgeWeight, mpi_native_datatype NodeWeight>
void broadcast_vcycle_state(std::span<int> partition_map,
                            EdgeWeight& previous_cut,
                            NodeWeight& previous_maximum_block_weight,
                            int root,
                            communicator_view communicator,
                            broadcast_options options = {}) noexcept {
  broadcast_bounded(partition_map, root, communicator,
                    "MPI_Bcast(previous partition map)", options);
  broadcast_fixed(previous_cut, root, communicator,
                  "MPI_Bcast(previous edge cut)");
  broadcast_fixed(previous_maximum_block_weight, root, communicator,
                  "MPI_Bcast(previous maximum block weight)");
}
}  // namespace parhip::mpi
