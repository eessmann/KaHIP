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
#include <string_view>
#include <utility>
#include <vector>

#include "communication/mpi_collectives.h"
#include "kahip_mpi_capabilities.h"

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
struct neighbor_count_exchange {
  std::vector<std::size_t> counts;
  capacity_result capacity;
};

inline auto exchange_neighbor_counts(std::span<std::size_t const> send_counts,
                                     std::size_t indegree,
                                     communicator_view communicator)
    -> neighbor_count_exchange {
  static_assert(sizeof(std::size_t) <= sizeof(std::uint64_t));
  auto const outgoing =
      std::vector<std::uint64_t>(send_counts.begin(), send_counts.end());
  auto incoming = std::vector<std::uint64_t>(indegree);
  // Some MPI implementations validate the buffers before checking the degree.
  auto const ignored_send = std::uint64_t{0};
  auto ignored_receive = std::uint64_t{0};
  check_or_abort(
      MPI_Neighbor_alltoall(
          outgoing.empty() ? &ignored_send : outgoing.data(), 1, MPI_UINT64_T,
          incoming.empty() ? &ignored_receive : incoming.data(), 1,
          MPI_UINT64_T, communicator.native_handle()),
      communicator.native_handle(),
      "MPI_Neighbor_alltoall(exchange neighbor counts)");

  auto result = neighbor_count_exchange{
      .counts = std::vector<std::size_t>(incoming.size()),
      .capacity = {},
  };
  for (std::size_t index = 0; index < incoming.size(); ++index) {
    if (!std::in_range<std::size_t>(incoming[index])) {
      result.capacity = with_fatal_capacity_issue(
          result.capacity, capacity_issue::received_count_not_representable);
      continue;
    }
    result.counts[index] = static_cast<std::size_t>(incoming[index]);
  }
  return result;
}

struct neighbor_receive_layout final {
  std::vector<std::size_t> offsets;
  std::size_t element_count;
  capacity_result capacity;
};

inline auto canonical_neighbor_layout(std::vector<std::size_t> const& counts)
    -> neighbor_receive_layout {
  auto offsets = std::vector<std::size_t>(counts.size());
  auto capacity = capacity_result{};
  auto total = std::size_t{0};
  auto remains_representable = true;
  for (std::size_t index = 0; index < counts.size(); ++index) {
    if (!remains_representable) {
      continue;
    }
    offsets[index] = total;
    if (counts[index] > std::numeric_limits<std::size_t>::max() - total) {
      capacity = with_fatal_capacity_issue(
          capacity, capacity_issue::cumulative_offset_overflow);
      remains_representable = false;
      continue;
    }
    total += counts[index];
  }
  return neighbor_receive_layout{
      .offsets = std::move(offsets),
      .element_count = total,
      .capacity = capacity,
  };
}

template <typename T>
[[nodiscard]] constexpr auto neighbor_capacity_preflight(
    capacity_result local,
    std::size_t receive_element_count,
    bool direct_layout_is_representable) noexcept -> capacity_result {
  if (receive_element_count >
      std::numeric_limits<std::size_t>::max() / sizeof(T)) {
    local = with_fatal_capacity_issue(
        local, capacity_issue::storage_byte_size_overflow);
  }
  if (!direct_layout_is_representable) {
    local = with_bounded_capacity_issue(
        local, capacity_issue::direct_backend_not_representable);
  }
  return local;
}

[[nodiscard]] inline auto neighbor_mpi4_layout_is_representable_locally(
    std::span<std::size_t const> send_counts,
    std::span<std::size_t const> send_offsets,
    std::span<std::size_t const> receive_counts,
    std::span<std::size_t const> receive_offsets) noexcept -> bool {
  auto const count_is_representable = [](std::size_t value) noexcept {
    return std::in_range<MPI_Count>(value);
  };
  auto const offset_is_representable = [](std::size_t value) noexcept {
    return std::in_range<MPI_Aint>(value);
  };
  return std::ranges::all_of(send_counts, count_is_representable) &&
         std::ranges::all_of(receive_counts, count_is_representable) &&
         std::ranges::all_of(send_offsets, offset_is_representable) &&
         std::ranges::all_of(receive_offsets, offset_is_representable);
}

[[nodiscard]] inline auto neighbor_mpi3_layout_is_representable_locally(
    std::span<std::size_t const> send_counts,
    std::span<std::size_t const> send_offsets,
    std::span<std::size_t const> receive_counts,
    std::span<std::size_t const> receive_offsets,
    std::size_t ceiling) noexcept -> bool {
  auto const is_representable = [ceiling](std::size_t value) noexcept {
    return value <= ceiling;
  };
  return std::ranges::all_of(send_counts, is_representable) &&
         std::ranges::all_of(receive_counts, is_representable) &&
         std::ranges::all_of(send_offsets, is_representable) &&
         std::ranges::all_of(receive_offsets, is_representable);
}

inline auto bounded_round_count(std::size_t count, std::size_t ceiling) noexcept
    -> std::size_t {
  return count == 0 ? std::size_t{0} : (count - 1) / ceiling + std::size_t{1};
}

inline auto product_is_representable(std::size_t lhs,
                                     std::size_t rhs,
                                     std::size_t& result) noexcept -> bool {
  if (lhs != 0 && rhs > std::numeric_limits<std::size_t>::max() / lhs) {
    result = 0;
    return false;
  }
  result = lhs * rhs;
  return true;
}

inline auto sum_is_representable(std::size_t lhs,
                                 std::size_t rhs,
                                 std::size_t& result) noexcept -> bool {
  if (rhs > std::numeric_limits<std::size_t>::max() - lhs) {
    result = 0;
    return false;
  }
  result = lhs + rhs;
  return true;
}

struct mpi3_bounded_neighbor_phase final {
  std::optional<std::size_t> destination_index;
  std::optional<std::size_t> source_index;
  std::size_t send_total = 0;
  std::size_t receive_total = 0;
  std::size_t round_count = 0;
};

struct mpi3_bounded_neighbor_plan final {
  std::size_t ceiling = 0;
  std::vector<mpi3_bounded_neighbor_phase> phases;
};

[[nodiscard]] inline auto make_mpi3_bounded_neighbor_plan(
    std::span<std::size_t const> send_counts,
    std::span<std::size_t const> send_offsets,
    std::span<std::size_t const> receive_counts,
    std::span<std::size_t const> receive_offsets,
    std::size_t ceiling,
    distributed_graph const& graph,
    communicator_view communicator) -> mpi3_bounded_neighbor_plan {
  auto const rank = static_cast<std::size_t>(communicator.rank());
  auto const size = static_cast<std::size_t>(communicator.size());
  auto result = mpi3_bounded_neighbor_plan{
      .ceiling = ceiling,
      .phases = std::vector<mpi3_bounded_neighbor_phase>(size),
  };
  auto local_capacity = capacity_result{};

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
                                ? send_counts[*destination_index]
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
      local_capacity = with_fatal_capacity_issue(
          local_capacity, capacity_issue::bounded_round_arithmetic_overflow);
      continue;
    }
    auto const phase_rounds = static_cast<std::size_t>(phase_rounds_u64);
    result.phases[phase] = mpi3_bounded_neighbor_phase{
        .destination_index = destination_index,
        .source_index = source_index,
        .send_total = send_total,
        .receive_total = receive_total,
        .round_count = phase_rounds,
    };

    auto ignored = std::size_t{0};
    if (phase_rounds != 0 &&
        !product_is_representable(phase_rounds - 1, ceiling, ignored)) {
      local_capacity = with_fatal_capacity_issue(
          local_capacity, capacity_issue::bounded_round_arithmetic_overflow);
    }
    if (destination_index.has_value() && send_total != 0) {
      auto last_chunk_offset = std::size_t{0};
      auto last_storage_offset = std::size_t{0};
      auto const local_send_rounds = bounded_round_count(send_total, ceiling);
      if (!product_is_representable(local_send_rounds - 1, ceiling,
                                    last_chunk_offset) ||
          !sum_is_representable(send_offsets[*destination_index],
                                last_chunk_offset, last_storage_offset)) {
        local_capacity = with_fatal_capacity_issue(
            local_capacity, capacity_issue::bounded_round_arithmetic_overflow);
      }
    }
    if (source_index.has_value() && receive_total != 0) {
      auto last_chunk_offset = std::size_t{0};
      auto last_storage_offset = std::size_t{0};
      auto const local_receive_rounds =
          bounded_round_count(receive_total, ceiling);
      if (!product_is_representable(local_receive_rounds - 1, ceiling,
                                    last_chunk_offset) ||
          !sum_is_representable(receive_offsets[*source_index],
                                last_chunk_offset, last_storage_offset)) {
        local_capacity = with_fatal_capacity_issue(
            local_capacity, capacity_issue::bounded_round_arithmetic_overflow);
      }
    }
  }

  static_cast<void>(resolve_capacity_collectively(
      local_capacity, communicator.native_handle(), graph.native_handle(),
      "neighbor_all_to_all_v bounded MPI-3 plan"));
  return result;
}

struct mpi3_bounded_neighbor_round final {
  std::optional<std::size_t> destination_index;
  std::optional<std::size_t> source_index;
  std::optional<std::size_t> send_storage_offset;
  std::optional<std::size_t> receive_storage_offset;
  int send_count = 0;
  int receive_count = 0;
};

[[nodiscard]] inline auto make_mpi3_bounded_neighbor_round(
    mpi3_bounded_neighbor_plan const& plan,
    std::size_t phase_index,
    std::size_t round_index,
    std::span<std::size_t const> send_offsets,
    std::span<std::size_t const> receive_offsets) noexcept
    -> mpi3_bounded_neighbor_round {
  auto const& phase = plan.phases[phase_index];
  auto const chunk_offset = round_index * plan.ceiling;
  auto const send_chunk =
      chunk_offset < phase.send_total
          ? std::min(plan.ceiling, phase.send_total - chunk_offset)
          : std::size_t{0};
  auto const receive_chunk =
      chunk_offset < phase.receive_total
          ? std::min(plan.ceiling, phase.receive_total - chunk_offset)
          : std::size_t{0};
  return mpi3_bounded_neighbor_round{
      .destination_index = phase.destination_index,
      .source_index = phase.source_index,
      .send_storage_offset =
          send_chunk == 0
              ? std::nullopt
              : std::optional<std::size_t>{
                    send_offsets[*phase.destination_index] + chunk_offset},
      .receive_storage_offset =
          receive_chunk == 0
              ? std::nullopt
              : std::optional<std::size_t>{
                    receive_offsets[*phase.source_index] + chunk_offset},
      .send_count = static_cast<int>(send_chunk),
      .receive_count = static_cast<int>(receive_chunk),
  };
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
  auto send_counts = std::vector<int>(graph.destinations().size(), 0);
  auto receive_counts_i = std::vector<int>(graph.sources().size(), 0);
  auto send_displacements = std::vector<int>(graph.destinations().size(), 0);
  auto receive_displacements = std::vector<int>(graph.sources().size(), 0);
  auto const plan = make_mpi3_bounded_neighbor_plan(
      sends.counts(), sends.offsets(), receive_counts, receive_offsets, ceiling,
      graph, communicator);

  for (std::size_t phase = 0; phase < plan.phases.size(); ++phase) {
    for (std::size_t round = 0; round < plan.phases[phase].round_count;
         ++round) {
      std::ranges::fill(send_counts, 0);
      std::ranges::fill(receive_counts_i, 0);
      auto const layout = make_mpi3_bounded_neighbor_round(
          plan, phase, round, sends.offsets(), receive_offsets);

      if (layout.destination_index.has_value()) {
        send_counts[*layout.destination_index] = layout.send_count;
      }
      if (layout.source_index.has_value()) {
        receive_counts_i[*layout.source_index] = layout.receive_count;
      }

      auto const* send_buffer =
          layout.send_storage_offset.has_value()
              ? static_cast<void const*>(
                    sends.storage().data() + *layout.send_storage_offset)
              : static_cast<void const*>(send_counts.data());
      auto* receive_buffer =
          layout.receive_storage_offset.has_value()
              ? static_cast<void*>(receive_storage.data() +
                                   *layout.receive_storage_offset)
              : static_cast<void*>(receive_counts_i.data());

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
          auto receive_layout =
              detail::canonical_neighbor_layout(receive_count_exchange.counts);
          auto local_capacity = detail::combine_capacity_results(
              receive_count_exchange.capacity, receive_layout.capacity);
          auto const mpi4_is_candidate =
              capabilities::has_neighbor_alltoallv_c && !options.force_mpi3;
          auto const direct_layout_is_representable =
              mpi4_is_candidate
                  ? detail::neighbor_mpi4_layout_is_representable_locally(
                        sends.counts(), sends.offsets(),
                        receive_count_exchange.counts, receive_layout.offsets)
                  : detail::neighbor_mpi3_layout_is_representable_locally(
                        sends.counts(), sends.offsets(),
                        receive_count_exchange.counts, receive_layout.offsets,
                        *mpi3_ceiling);
          local_capacity = detail::neighbor_capacity_preflight<T>(
              local_capacity, receive_layout.element_count,
              direct_layout_is_representable);
          auto const route = resolve_capacity_collectively(
              local_capacity, collective_communicator.native_handle(),
              graph.native_handle(), "neighbor_all_to_all_v");

          auto received = segmented_buffer<T>::uninitialized(
              receive_layout.element_count,
              std::move(receive_count_exchange.counts),
              std::move(receive_layout.offsets));
          auto datatype =
              make_mpi_datatype<T>(collective_communicator.native_handle());
          auto payload_complete = false;

#if KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_C
          if (route == capacity_route::direct && mpi4_is_candidate) {
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

          if (!payload_complete && route == capacity_route::bounded) {
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
    } catch (...) {
      abort_on_exception(collective_communicator.native_handle(),
                         "neighbor_all_to_all_v local failure");
    }
  }

  // KAHIP_SEMANTIC_EXIT_BEGIN(sync-neighbor)
  if (!semantic_failure.empty()) {
    throw_collectively_agreed_semantic_error(graph.native_handle(),
                                             semantic_failure);
  }
  // KAHIP_SEMANTIC_EXIT_END(sync-neighbor)
  return std::move(*result);
}
}  // namespace parhip::mpi
