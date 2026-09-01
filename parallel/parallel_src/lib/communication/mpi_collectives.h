#pragma once

#include <mpi.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "communication/mpi_error.h"
#include "communication/mpi_handles.h"
#include "communication/mpi_types.h"
#include "communication/segmented_buffer.h"
#include "kahip_mpi_capabilities.h"

namespace parhip::mpi {
namespace capabilities {
inline constexpr bool has_alltoallv_c = KAHIP_HAVE_MPI_ALLTOALLV_C != 0;
}  // namespace capabilities

struct collective_options {
  std::size_t mpi3_round_ceiling =
      static_cast<std::size_t>(std::numeric_limits<int>::max());
  bool force_mpi3 = false;
};

namespace detail {
inline auto collective_predicate(bool local_is_valid,
                                 communicator_view communicator) noexcept
    -> bool {
  int local_valid = local_is_valid ? 1 : 0;
  int all_valid = 0;
  check_or_abort(MPI_Allreduce(&local_valid,
                               &all_valid,
                               1,
                               MPI_INT,
                               MPI_MIN,
                               communicator.native_handle()),
                 communicator.native_handle(),
                 "MPI_Allreduce(collective validation)");
  return all_valid != 0;
}

inline auto validate_collective_options(collective_options options,
                                        communicator_view communicator)
    -> std::optional<std::size_t> {
  static_assert(sizeof(std::size_t) <= sizeof(std::uint64_t));
  auto const local = std::array<std::uint64_t, 2>{
      static_cast<std::uint64_t>(options.mpi3_round_ceiling),
      options.force_mpi3 ? std::uint64_t{1} : std::uint64_t{0}};
  std::array<std::uint64_t, 2> minimum{};
  std::array<std::uint64_t, 2> maximum{};
  check_or_abort(MPI_Allreduce(local.data(),
                               minimum.data(),
                               2,
                               MPI_UINT64_T,
                               MPI_MIN,
                               communicator.native_handle()),
                 communicator.native_handle(),
                 "MPI_Allreduce(dense collective option minimum)");
  check_or_abort(MPI_Allreduce(local.data(),
                               maximum.data(),
                               2,
                               MPI_UINT64_T,
                               MPI_MAX,
                               communicator.native_handle()),
                 communicator.native_handle(),
                 "MPI_Allreduce(dense collective option maximum)");
  if (minimum != maximum || options.mpi3_round_ceiling == 0) {
    return std::nullopt;
  }
  return std::min(options.mpi3_round_ceiling,
                  static_cast<std::size_t>(std::numeric_limits<int>::max()));
}

inline auto exchange_counts(std::vector<std::size_t> const& send_counts,
                            communicator_view communicator)
    -> std::vector<std::size_t> {
  static_assert(sizeof(std::size_t) <= sizeof(std::uint64_t));
  std::vector<std::uint64_t> send(send_counts.begin(), send_counts.end());
  std::vector<std::uint64_t> receive(send_counts.size());
  check_or_abort(MPI_Alltoall(send.data(),
                              1,
                              MPI_UINT64_T,
                              receive.data(),
                              1,
                              MPI_UINT64_T,
                              communicator.native_handle()),
                 communicator.native_handle(),
                 "MPI_Alltoall(exchange dense counts)");

  std::vector<std::size_t> result;
  result.reserve(receive.size());
  for (auto count : receive) {
    if (count > std::numeric_limits<std::size_t>::max()) {
      throw mpi_error{MPI_ERR_COUNT, "received dense count exceeds size_t"};
    }
    result.push_back(static_cast<std::size_t>(count));
  }
  return result;
}

inline auto canonical_offsets(std::vector<std::size_t> const& counts)
    -> std::vector<std::size_t> {
  std::vector<std::size_t> offsets(counts.size());
  std::size_t total = 0;
  for (std::size_t index = 0; index < counts.size(); ++index) {
    if (counts[index] > std::numeric_limits<std::size_t>::max() - total) {
      throw mpi_error{MPI_ERR_COUNT, "dense receive size exceeds size_t"};
    }
    offsets[index] = total;
    total += counts[index];
  }
  return offsets;
}

inline auto checked_int(std::size_t value, std::string_view context) -> int {
  if (value > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    throw mpi_error{MPI_ERR_COUNT, std::string{context}};
  }
  return static_cast<int>(value);
}

template <typename T>
inline auto needs_bounded_rounds(
    segmented_buffer<T> const& sends,
    std::vector<std::size_t> const& receive_counts,
    std::vector<std::size_t> const& receive_offsets,
    std::size_t ceiling,
    communicator_view communicator) -> bool {
  auto local_needs_rounds = false;
  for (std::size_t index = 0; index < sends.segment_count(); ++index) {
    local_needs_rounds = local_needs_rounds ||
                         sends.counts()[index] > ceiling ||
                         sends.offsets()[index] > ceiling ||
                         receive_counts[index] > ceiling ||
                         receive_offsets[index] > ceiling;
  }
  int local = local_needs_rounds ? 1 : 0;
  int global = 0;
  check_or_abort(MPI_Allreduce(&local,
                               &global,
                               1,
                               MPI_INT,
                               MPI_MAX,
                               communicator.native_handle()),
                 communicator.native_handle(),
                 "MPI_Allreduce(select MPI-3 collective path)");
  return global != 0;
}

template <typename T>
void mpi3_bounded_all_to_all_v(
    segmented_buffer<T> const& sends,
    std::span<T> receive_storage,
    std::vector<std::size_t> const& receive_counts,
    std::vector<std::size_t> const& receive_offsets,
    MPI_Datatype datatype,
    std::size_t ceiling,
    communicator_view communicator) {
  auto const rank = communicator.rank();
  auto const size = communicator.size();
  std::vector<int> send_counts(static_cast<std::size_t>(size), 0);
  std::vector<int> receive_counts_i(static_cast<std::size_t>(size), 0);
  std::vector<int> displacements(static_cast<std::size_t>(size), 0);

  for (int phase = 0; phase < size; ++phase) {
    auto const destination = (rank + phase) % size;
    auto const source = (rank - phase + size) % size;
    auto const destination_index = static_cast<std::size_t>(destination);
    auto const source_index = static_cast<std::size_t>(source);
    auto const send_total = sends.counts()[destination_index];
    auto const local_rounds =
        send_total == 0 ? std::size_t{0}
                        : (send_total - 1) / ceiling + std::size_t{1};
    auto local_rounds_u64 = static_cast<std::uint64_t>(local_rounds);
    std::uint64_t phase_rounds_u64 = 0;
    check_or_abort(MPI_Allreduce(&local_rounds_u64,
                                 &phase_rounds_u64,
                                 1,
                                 MPI_UINT64_T,
                                 MPI_MAX,
                                 communicator.native_handle()),
                   communicator.native_handle(),
                   "MPI_Allreduce(MPI-3 bounded phase rounds)");
    auto const phase_rounds = static_cast<std::size_t>(phase_rounds_u64);

    for (std::size_t round = 0; round < phase_rounds; ++round) {
      std::ranges::fill(send_counts, 0);
      std::ranges::fill(receive_counts_i, 0);
      auto const chunk_offset = round * ceiling;
      auto const send_chunk =
          chunk_offset < send_total
              ? std::min(ceiling, send_total - chunk_offset)
              : std::size_t{0};
      auto const receive_total = receive_counts[source_index];
      auto const receive_chunk =
          chunk_offset < receive_total
              ? std::min(ceiling, receive_total - chunk_offset)
              : std::size_t{0};
      send_counts[destination_index] =
          checked_int(send_chunk, "MPI-3 bounded send chunk");
      receive_counts_i[source_index] =
          checked_int(receive_chunk, "MPI-3 bounded receive chunk");

      auto const* send_buffer = sends.storage().data();
      if (send_chunk != 0) {
        send_buffer += sends.offsets()[destination_index] + chunk_offset;
      }
      auto* receive_buffer = receive_storage.data();
      if (receive_chunk != 0) {
        receive_buffer += receive_offsets[source_index] + chunk_offset;
      }
      check_or_abort(MPI_Alltoallv(send_buffer,
                                   send_counts.data(),
                                   displacements.data(),
                                   datatype,
                                   receive_buffer,
                                   receive_counts_i.data(),
                                   displacements.data(),
                                   datatype,
                                   communicator.native_handle()),
                     communicator.native_handle(),
                     "MPI_Alltoallv(MPI-3 bounded dense round)");
    }
  }
}

#if KAHIP_HAVE_MPI_ALLTOALLV_C
inline auto checked_mpi_count(std::size_t value, std::string_view context)
    -> MPI_Count {
  if (value > static_cast<std::size_t>(std::numeric_limits<MPI_Count>::max())) {
    throw mpi_error{MPI_ERR_COUNT, std::string{context}};
  }
  return static_cast<MPI_Count>(value);
}

inline auto checked_mpi_aint(std::size_t value, std::string_view context)
    -> MPI_Aint {
  if (value > static_cast<std::size_t>(std::numeric_limits<MPI_Aint>::max())) {
    throw mpi_error{MPI_ERR_COUNT, std::string{context}};
  }
  return static_cast<MPI_Aint>(value);
}
#endif
}  // namespace detail

inline void validate_collectively(bool local_is_valid,
                                  communicator_view communicator,
                                  std::string_view context) {
  auto all_valid = false;
  {
    auto owned_communicator = parhip::mpi::communicator{communicator};
    all_valid = detail::collective_predicate(
        local_is_valid, owned_communicator.view());
  }
  if (!all_valid) {
    throw mpi_error{MPI_ERR_ARG, std::string{context}};
  }
}

template <mpi_native_datatype T>
[[nodiscard]] auto agree_collectively(T local_value,
                                      communicator_view communicator,
                                      std::string_view context) -> T {
  auto minimum = T{};
  auto maximum = T{};
  {
    auto owned_communicator = parhip::mpi::communicator{communicator};
    auto const collective_communicator = owned_communicator.view();
    auto const datatype = get_mpi_datatype<T>();
    check_or_abort(MPI_Allreduce(&local_value,
                                 &minimum,
                                 1,
                                 datatype,
                                 MPI_MIN,
                                 collective_communicator.native_handle()),
                   collective_communicator.native_handle(),
                   "MPI_Allreduce(common value minimum)");
    check_or_abort(MPI_Allreduce(&local_value,
                                 &maximum,
                                 1,
                                 datatype,
                                 MPI_MAX,
                                 collective_communicator.native_handle()),
                   collective_communicator.native_handle(),
                   "MPI_Allreduce(common value maximum)");
  }
  if (minimum != maximum) {
    throw mpi_error{MPI_ERR_ARG, std::string{context}};
  }
  return minimum;
}

template <mpi_datatype T>
[[nodiscard]] auto all_to_all_v(
    segmented_buffer<T> sends,
    communicator_view communicator,
    collective_options options = {})
    -> segmented_buffer<T> {
  auto semantic_failure = std::string_view{};
  auto result = std::optional<segmented_buffer<T>>{};
  {
    auto owned_communicator = parhip::mpi::communicator{communicator};
    auto const collective_communicator = owned_communicator.view();
    try {
      auto const communicator_size =
          static_cast<std::size_t>(collective_communicator.size());
      auto const layout_is_valid = detail::collective_predicate(
          sends.has_canonical_layout(communicator_size),
          collective_communicator);
      if (!layout_is_valid) {
        semantic_failure =
            "all_to_all_v collective input validation failed";
      } else {
        auto const mpi3_ceiling =
            detail::validate_collective_options(options,
                                                collective_communicator);
        if (!mpi3_ceiling.has_value()) {
          semantic_failure =
              "all_to_all_v collective options must match and use a "
              "nonzero MPI-3 ceiling";
        } else {
          auto receive_counts =
              detail::exchange_counts(sends.counts(), collective_communicator);
          auto receive_offsets = detail::canonical_offsets(receive_counts);
          auto const receive_size =
              receive_counts.empty()
                  ? std::size_t{0}
                  : receive_offsets.back() + receive_counts.back();
          auto received = segmented_buffer<T>::uninitialized(
              receive_size,
              std::move(receive_counts),
              std::move(receive_offsets));
          auto datatype = make_mpi_datatype<T>(
              collective_communicator.native_handle());
          auto payload_complete = false;

#if KAHIP_HAVE_MPI_ALLTOALLV_C
          if (!options.force_mpi3) {
            std::vector<MPI_Count> send_counts;
            std::vector<MPI_Count> receive_counts_c;
            std::vector<MPI_Aint> send_offsets;
            std::vector<MPI_Aint> receive_offsets_c;
            send_counts.reserve(communicator_size);
            receive_counts_c.reserve(communicator_size);
            send_offsets.reserve(communicator_size);
            receive_offsets_c.reserve(communicator_size);
            for (std::size_t index = 0; index < communicator_size; ++index) {
              send_counts.push_back(detail::checked_mpi_count(
                  sends.counts()[index], "MPI send count"));
              receive_counts_c.push_back(detail::checked_mpi_count(
                  received.counts()[index], "MPI receive count"));
              send_offsets.push_back(detail::checked_mpi_aint(
                  sends.offsets()[index], "MPI send offset"));
              receive_offsets_c.push_back(detail::checked_mpi_aint(
                  received.offsets()[index], "MPI receive offset"));
            }
            check_or_abort(
                MPI_Alltoallv_c(sends.storage().data(),
                                send_counts.data(),
                                send_offsets.data(),
                                datatype.native_handle(),
                                received.storage().data(),
                                receive_counts_c.data(),
                                receive_offsets_c.data(),
                                datatype.native_handle(),
                                collective_communicator.native_handle()),
                collective_communicator.native_handle(),
                "MPI_Alltoallv_c(dense exchange)");
            payload_complete = true;
          }
#endif

          if (!payload_complete &&
              detail::needs_bounded_rounds(sends,
                                            received.counts(),
                                            received.offsets(),
                                            *mpi3_ceiling,
                                            collective_communicator)) {
            detail::mpi3_bounded_all_to_all_v(sends,
                                              received.storage(),
                                              received.counts(),
                                              received.offsets(),
                                              datatype.native_handle(),
                                              *mpi3_ceiling,
                                              collective_communicator);
            payload_complete = true;
          }
          if (!payload_complete) {
            std::vector<int> send_counts;
            std::vector<int> receive_counts_i;
            std::vector<int> send_offsets;
            std::vector<int> receive_offsets_i;
            send_counts.reserve(communicator_size);
            receive_counts_i.reserve(communicator_size);
            send_offsets.reserve(communicator_size);
            receive_offsets_i.reserve(communicator_size);
            for (std::size_t index = 0; index < communicator_size; ++index) {
              send_counts.push_back(detail::checked_int(
                  sends.counts()[index], "MPI send count"));
              receive_counts_i.push_back(detail::checked_int(
                  received.counts()[index], "MPI receive count"));
              send_offsets.push_back(detail::checked_int(
                  sends.offsets()[index], "MPI send offset"));
              receive_offsets_i.push_back(detail::checked_int(
                  received.offsets()[index], "MPI receive offset"));
            }
            check_or_abort(
                MPI_Alltoallv(sends.storage().data(),
                              send_counts.data(),
                              send_offsets.data(),
                              datatype.native_handle(),
                              received.storage().data(),
                              receive_counts_i.data(),
                              receive_offsets_i.data(),
                              datatype.native_handle(),
                              collective_communicator.native_handle()),
                collective_communicator.native_handle(),
                "MPI_Alltoallv(dense exchange)");
          }
          result.emplace(std::move(received));
        }
      }
    } catch (...) {
      abort_on_exception(collective_communicator.native_handle(),
                         "all_to_all_v local failure");
    }
  }

  if (!semantic_failure.empty()) {
    throw mpi_error{MPI_ERR_ARG, std::string{semantic_failure}};
  }
  return std::move(*result);
}
}  // namespace parhip::mpi
