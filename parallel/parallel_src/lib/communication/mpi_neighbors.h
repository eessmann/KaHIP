#pragma once

#include <mpi.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "communication/mpi_collectives.h"

namespace parhip::mpi {
class distributed_graph {
 public:
  explicit distributed_graph(communicator_view communicator,
                             std::vector<int> outgoing_destinations);
  ~distributed_graph() noexcept;

  distributed_graph(distributed_graph const&) = delete;
  auto operator=(distributed_graph const&) -> distributed_graph& = delete;
  distributed_graph(distributed_graph&& other) noexcept;
  auto operator=(distributed_graph&& other) noexcept -> distributed_graph&;

  [[nodiscard]] auto native_handle() const noexcept -> MPI_Comm {
    return communicator_;
  }
  [[nodiscard]] auto view() const noexcept -> communicator_view {
    return communicator_view{communicator_};
  }
  [[nodiscard]] auto sources() const noexcept -> std::span<int const> {
    return sources_;
  }
  [[nodiscard]] auto destinations() const noexcept -> std::span<int const> {
    return destinations_;
  }
  [[nodiscard]] auto source_index(int rank) const noexcept
      -> std::optional<std::size_t> {
    return find_index(source_lookup_, rank);
  }
  [[nodiscard]] auto destination_index(int rank) const noexcept
      -> std::optional<std::size_t> {
    return find_index(destination_lookup_, rank);
  }

 private:
  using rank_index = std::pair<int, std::size_t>;

  [[nodiscard]] static auto make_lookup(std::span<int const> ranks)
      -> std::vector<rank_index>;
  [[nodiscard]] static auto find_index(std::span<rank_index const> lookup,
                                       int rank) noexcept
      -> std::optional<std::size_t>;
  void reset() noexcept;

  MPI_Comm communicator_ = MPI_COMM_NULL;
  std::vector<int> sources_;
  std::vector<int> destinations_;
  std::vector<rank_index> source_lookup_;
  std::vector<rank_index> destination_lookup_;
};

namespace detail {
inline auto neighbor_offsets(std::vector<std::size_t> const& counts)
    -> std::optional<std::vector<std::size_t>> {
  auto offsets = std::vector<std::size_t>(counts.size());
  auto total = std::size_t{0};
  for (std::size_t index = 0; index < counts.size(); ++index) {
    if (counts[index] > std::numeric_limits<std::size_t>::max() - total) {
      return std::nullopt;
    }
    offsets[index] = total;
    total += counts[index];
  }
  return offsets;
}

inline auto neighbor_storage_size(
    std::vector<std::size_t> const& counts,
    std::vector<std::size_t> const& offsets) noexcept -> std::size_t {
  return counts.empty() ? std::size_t{0} : offsets.back() + counts.back();
}

struct neighbor_count_exchange {
  std::vector<std::size_t> counts;
  bool representable = true;
};

inline auto exchange_neighbor_counts(
    std::vector<std::size_t> const& send_counts,
    std::size_t indegree,
    communicator_view communicator) -> neighbor_count_exchange {
  static_assert(sizeof(std::size_t) <= sizeof(std::uint64_t));
  auto const outgoing =
      std::vector<std::uint64_t>(send_counts.begin(), send_counts.end());
  auto incoming = std::vector<std::uint64_t>(indegree);
  check_or_abort(
      MPI_Neighbor_alltoall(outgoing.data(), 1, MPI_UINT64_T, incoming.data(),
                            1, MPI_UINT64_T, communicator.native_handle()),
      communicator.native_handle(),
      "MPI_Neighbor_alltoall(exchange neighbor counts)");

  auto result = neighbor_count_exchange{};
  result.counts.reserve(incoming.size());
  for (auto const count : incoming) {
    if (!std::in_range<std::size_t>(count)) {
      result.representable = false;
      result.counts.push_back(0);
    } else {
      result.counts.push_back(static_cast<std::size_t>(count));
    }
  }
  return result;
}

#if KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_C
template <typename T>
[[nodiscard]] auto neighbor_mpi4_layout_is_representable(
    segmented_buffer<T> const& sends,
    segmented_buffer<T> const& received,
    communicator_view communicator) -> bool {
  auto local_is_representable = true;
  for (std::size_t index = 0; index < sends.segment_count(); ++index) {
    local_is_representable = local_is_representable &&
                             std::in_range<MPI_Count>(sends.counts()[index]) &&
                             std::in_range<MPI_Aint>(sends.offsets()[index]);
  }
  for (std::size_t index = 0; index < received.segment_count(); ++index) {
    local_is_representable =
        local_is_representable &&
        std::in_range<MPI_Count>(received.counts()[index]) &&
        std::in_range<MPI_Aint>(received.offsets()[index]);
  }
  return collective_predicate(local_is_representable, communicator);
}
#endif

template <typename T>
[[nodiscard]] auto neighbor_needs_bounded_rounds(
    segmented_buffer<T> const& sends,
    std::vector<std::size_t> const& receive_counts,
    std::vector<std::size_t> const& receive_offsets,
    std::size_t ceiling,
    communicator_view communicator) -> bool {
  auto local_needs_rounds = std::ranges::any_of(
      std::views::iota(std::size_t{0}, sends.segment_count()),
      [&](auto const index) {
        return sends.counts()[index] > ceiling ||
               sends.offsets()[index] > ceiling;
      });
  local_needs_rounds =
      local_needs_rounds ||
      std::ranges::any_of(
          std::views::iota(std::size_t{0}, receive_counts.size()),
          [&](auto const index) {
            return receive_counts[index] > ceiling ||
                   receive_offsets[index] > ceiling;
          });
  auto local = local_needs_rounds ? 1 : 0;
  auto global = 0;
  check_or_abort(MPI_Allreduce(&local, &global, 1, MPI_INT, MPI_MAX,
                               communicator.native_handle()),
                 communicator.native_handle(),
                 "MPI_Allreduce(select MPI-3 neighborhood path)");
  return global != 0;
}

inline auto bounded_round_count(std::size_t count, std::size_t ceiling) noexcept
    -> std::size_t {
  return count == 0 ? std::size_t{0} : (count - 1) / ceiling + std::size_t{1};
}

inline auto checked_product(std::size_t lhs,
                            std::size_t rhs,
                            std::string_view context) -> std::size_t {
  if (lhs != 0 && rhs > std::numeric_limits<std::size_t>::max() / lhs) {
    throw mpi_error{MPI_ERR_COUNT, std::string{context}};
  }
  return lhs * rhs;
}

inline auto checked_sum(std::size_t lhs,
                        std::size_t rhs,
                        std::string_view context) -> std::size_t {
  if (rhs > std::numeric_limits<std::size_t>::max() - lhs) {
    throw mpi_error{MPI_ERR_COUNT, std::string{context}};
  }
  return lhs + rhs;
}

template <typename T>
void mpi3_bounded_neighbor_all_to_all_v(
    segmented_buffer<T> const& sends,
    std::span<T> receive_storage,
    std::vector<std::size_t> const& receive_counts,
    std::vector<std::size_t> const& receive_offsets,
    MPI_Datatype datatype,
    std::size_t ceiling,
    distributed_graph const& graph,
    communicator_view communicator) {
  auto const rank = static_cast<std::size_t>(communicator.rank());
  auto const size = static_cast<std::size_t>(communicator.size());
  auto send_counts = std::vector<int>(graph.destinations().size(), 0);
  auto receive_counts_i = std::vector<int>(graph.sources().size(), 0);
  auto send_displacements = std::vector<int>(graph.destinations().size(), 0);
  auto receive_displacements = std::vector<int>(graph.sources().size(), 0);

  for (std::size_t phase = 0; phase < size; ++phase) {
    auto const distance_to_wrap = size - rank;
    auto const destination_rank =
        phase >= distance_to_wrap ? phase - distance_to_wrap : rank + phase;
    auto const source_rank =
        rank >= phase ? rank - phase : size - (phase - rank);
    auto const destination_index =
        graph.destination_index(static_cast<int>(destination_rank));
    auto const source_index = graph.source_index(static_cast<int>(source_rank));
    auto const send_total = destination_index.has_value()
                                ? sends.counts()[*destination_index]
                                : std::size_t{0};
    auto const receive_total = source_index.has_value()
                                   ? receive_counts[*source_index]
                                   : std::size_t{0};
    auto const local_rounds =
        std::max(bounded_round_count(send_total, ceiling),
                 bounded_round_count(receive_total, ceiling));
    auto const local_rounds_u64 = static_cast<std::uint64_t>(local_rounds);
    auto phase_rounds_u64 = std::uint64_t{0};
    check_or_abort(
        MPI_Allreduce(&local_rounds_u64, &phase_rounds_u64, 1, MPI_UINT64_T,
                      MPI_MAX, communicator.native_handle()),
        communicator.native_handle(),
        "MPI_Allreduce(MPI-3 bounded neighbor phase rounds)");
    if (phase_rounds_u64 > std::numeric_limits<std::size_t>::max()) {
      throw mpi_error{MPI_ERR_COUNT,
                      "bounded neighbor phase count exceeds size_t"};
    }
    auto const phase_rounds = static_cast<std::size_t>(phase_rounds_u64);

    for (std::size_t round = 0; round < phase_rounds; ++round) {
      std::ranges::fill(send_counts, 0);
      std::ranges::fill(receive_counts_i, 0);
      auto const chunk_offset = checked_product(
          round, ceiling, "bounded neighbor chunk offset overflow");
      auto const send_chunk = chunk_offset < send_total
                                  ? std::min(ceiling, send_total - chunk_offset)
                                  : std::size_t{0};
      auto const receive_chunk =
          chunk_offset < receive_total
              ? std::min(ceiling, receive_total - chunk_offset)
              : std::size_t{0};

      if (destination_index.has_value()) {
        send_counts[*destination_index] =
            checked_int(send_chunk, "MPI-3 bounded neighbor send chunk");
      }
      if (source_index.has_value()) {
        receive_counts_i[*source_index] =
            checked_int(receive_chunk, "MPI-3 bounded neighbor receive chunk");
      }

      auto const* send_buffer = sends.storage().data();
      if (send_chunk != 0) {
        auto const offset =
            checked_sum(sends.offsets()[*destination_index], chunk_offset,
                        "bounded neighbor send pointer overflow");
        send_buffer += offset;
      }
      auto* receive_buffer = receive_storage.data();
      if (receive_chunk != 0) {
        auto const offset =
            checked_sum(receive_offsets[*source_index], chunk_offset,
                        "bounded neighbor receive pointer overflow");
        receive_buffer += offset;
      }

      check_or_abort(MPI_Neighbor_alltoallv(
                         send_buffer, send_counts.data(),
                         send_displacements.data(), datatype, receive_buffer,
                         receive_counts_i.data(), receive_displacements.data(),
                         datatype, communicator.native_handle()),
                     communicator.native_handle(),
                     "MPI_Neighbor_alltoallv(MPI-3 bounded neighbor round)");
    }
  }
}
}  // namespace detail

template <mpi_datatype T>
[[nodiscard]] auto neighbor_all_to_all_v(segmented_buffer<T> sends,
                                         distributed_graph const& graph,
                                         collective_options options = {})
    -> segmented_buffer<T> {
  auto semantic_failure = std::string_view{};
  auto result = std::optional<segmented_buffer<T>>{};
  {
    auto owned_communicator = communicator{graph.view()};
    auto const collective_communicator = owned_communicator.view();
    try {
      auto const layout_is_valid = detail::collective_predicate(
          sends.has_canonical_layout(graph.destinations().size()),
          collective_communicator);
      if (!layout_is_valid) {
        semantic_failure =
            "neighbor_all_to_all_v collective input validation failed";
      } else {
        auto const mpi3_ceiling = detail::validate_collective_options(
            options, collective_communicator);
        if (!mpi3_ceiling.has_value()) {
          semantic_failure =
              "neighbor_all_to_all_v collective options must match and use "
              "a nonzero MPI-3 ceiling";
        } else {
          auto receive_count_exchange = detail::exchange_neighbor_counts(
              sends.counts(), graph.sources().size(), collective_communicator);
          auto receive_offsets =
              receive_count_exchange.representable
                  ? detail::neighbor_offsets(receive_count_exchange.counts)
                  : std::nullopt;
          auto receive_storage_size = std::optional<std::size_t>{};
          if (receive_offsets.has_value()) {
            auto const element_count = detail::neighbor_storage_size(
                receive_count_exchange.counts, *receive_offsets);
            if (element_count <=
                std::numeric_limits<std::size_t>::max() / sizeof(T)) {
              receive_storage_size = element_count;
            }
          }
          auto const receive_layout_is_valid = detail::collective_predicate(
              receive_count_exchange.representable &&
                  receive_offsets.has_value() &&
                  receive_storage_size.has_value(),
              collective_communicator);
          if (!receive_layout_is_valid) {
            semantic_failure =
                "neighbor_all_to_all_v receive layout validation failed";
          } else {
            auto received = segmented_buffer<T>::uninitialized(
                *receive_storage_size,
                std::move(receive_count_exchange.counts),
                std::move(*receive_offsets));
            auto datatype =
                make_mpi_datatype<T>(collective_communicator.native_handle());
            auto payload_complete = false;
            auto mpi4_requires_bounded_rounds = false;

#if KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_C
            auto const mpi4_layout_is_representable =
                detail::neighbor_mpi4_layout_is_representable(
                    sends, received, collective_communicator);
            mpi4_requires_bounded_rounds = !mpi4_layout_is_representable;
            if (!options.force_mpi3 && mpi4_layout_is_representable) {
              auto send_counts = std::vector<MPI_Count>{};
              auto receive_counts_c = std::vector<MPI_Count>{};
              auto send_offsets = std::vector<MPI_Aint>{};
              auto receive_offsets_c = std::vector<MPI_Aint>{};
              send_counts.reserve(sends.segment_count());
              send_offsets.reserve(sends.segment_count());
              receive_counts_c.reserve(received.segment_count());
              receive_offsets_c.reserve(received.segment_count());
              for (std::size_t index = 0; index < sends.segment_count();
                   ++index) {
                send_counts.push_back(detail::checked_mpi_count(
                    sends.counts()[index], "MPI neighbor send count"));
                send_offsets.push_back(detail::checked_mpi_aint(
                    sends.offsets()[index], "MPI neighbor send offset"));
              }
              for (std::size_t index = 0; index < received.segment_count();
                   ++index) {
                receive_counts_c.push_back(detail::checked_mpi_count(
                    received.counts()[index], "MPI neighbor receive count"));
                receive_offsets_c.push_back(detail::checked_mpi_aint(
                    received.offsets()[index], "MPI neighbor receive offset"));
              }
              check_or_abort(
                  MPI_Neighbor_alltoallv_c(
                      sends.storage().data(), send_counts.data(),
                      send_offsets.data(), datatype.native_handle(),
                      received.storage().data(), receive_counts_c.data(),
                      receive_offsets_c.data(), datatype.native_handle(),
                      collective_communicator.native_handle()),
                  collective_communicator.native_handle(),
                  "MPI_Neighbor_alltoallv_c(neighbor exchange)");
              payload_complete = true;
            }
#endif

            if (!payload_complete &&
                (mpi4_requires_bounded_rounds ||
                 detail::neighbor_needs_bounded_rounds(
                     sends, received.counts(), received.offsets(),
                     *mpi3_ceiling, collective_communicator))) {
              detail::mpi3_bounded_neighbor_all_to_all_v(
                  sends, received.storage(), received.counts(),
                  received.offsets(), datatype.native_handle(), *mpi3_ceiling,
                  graph, collective_communicator);
              payload_complete = true;
            }
            if (!payload_complete) {
              auto send_counts = std::vector<int>{};
              auto receive_counts_i = std::vector<int>{};
              auto send_offsets = std::vector<int>{};
              auto receive_offsets_i = std::vector<int>{};
              send_counts.reserve(sends.segment_count());
              send_offsets.reserve(sends.segment_count());
              receive_counts_i.reserve(received.segment_count());
              receive_offsets_i.reserve(received.segment_count());
              for (std::size_t index = 0; index < sends.segment_count();
                   ++index) {
                send_counts.push_back(detail::checked_int(
                    sends.counts()[index], "MPI neighbor send count"));
                send_offsets.push_back(detail::checked_int(
                    sends.offsets()[index], "MPI neighbor send offset"));
              }
              for (std::size_t index = 0; index < received.segment_count();
                   ++index) {
                receive_counts_i.push_back(detail::checked_int(
                    received.counts()[index], "MPI neighbor receive count"));
                receive_offsets_i.push_back(detail::checked_int(
                    received.offsets()[index], "MPI neighbor receive offset"));
              }
              check_or_abort(
                  MPI_Neighbor_alltoallv(
                      sends.storage().data(), send_counts.data(),
                      send_offsets.data(), datatype.native_handle(),
                      received.storage().data(), receive_counts_i.data(),
                      receive_offsets_i.data(), datatype.native_handle(),
                      collective_communicator.native_handle()),
                  collective_communicator.native_handle(),
                  "MPI_Neighbor_alltoallv(neighbor exchange)");
            }
            result.emplace(std::move(received));
          }
        }
      }
    } catch (...) {
      abort_on_exception(collective_communicator.native_handle(),
                         "neighbor_all_to_all_v local failure");
    }
  }

  if (!semantic_failure.empty()) {
    throw mpi_error{MPI_ERR_ARG, std::string{semantic_failure}};
  }
  return std::move(*result);
}
}  // namespace parhip::mpi
