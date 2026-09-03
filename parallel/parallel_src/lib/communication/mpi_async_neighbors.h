#pragma once

#include <mpi.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "communication/mpi_neighbors.h"
#include "kahip_mpi_capabilities.h"

namespace parhip::mpi {
enum class persistence_policy : std::uint8_t {
  disabled,
  prefer,
  required,
};

struct context_options {
  collective_options collective{};
  persistence_policy persistence = persistence_policy::disabled;
};

namespace detail {
enum class neighbor_direct_backend : std::uint8_t {
  bounded_legacy,
  immediate_legacy,
  immediate_large_count,
  persistent_legacy,
  persistent_large_count,
};

[[nodiscard]] inline auto is_persistent(
    neighbor_direct_backend backend) noexcept -> bool {
  return backend == neighbor_direct_backend::persistent_legacy ||
         backend == neighbor_direct_backend::persistent_large_count;
}

[[nodiscard]] inline auto uses_large_count(
    neighbor_direct_backend backend) noexcept -> bool {
  return backend == neighbor_direct_backend::immediate_large_count ||
         backend == neighbor_direct_backend::persistent_large_count;
}

inline auto validate_persistence_policy(persistence_policy policy,
                                        communicator_view communicator)
    -> std::optional<persistence_policy> {
  auto const encoded = static_cast<std::uint8_t>(policy);
  auto const locally_valid =
      encoded <= static_cast<std::uint8_t>(persistence_policy::required);
  auto const local = static_cast<std::uint64_t>(encoded);
  auto minimum = std::uint64_t{0};
  auto maximum = std::uint64_t{0};
  check_or_abort(MPI_Allreduce(&local, &minimum, 1, MPI_UINT64_T, MPI_MIN,
                               communicator.native_handle()),
                 communicator.native_handle(),
                 "MPI_Allreduce(neighbor persistence policy minimum)");
  check_or_abort(MPI_Allreduce(&local, &maximum, 1, MPI_UINT64_T, MPI_MAX,
                               communicator.native_handle()),
                 communicator.native_handle(),
                 "MPI_Allreduce(neighbor persistence policy maximum)");
  if (!collective_predicate(locally_valid, communicator) ||
      minimum != maximum) {
    return std::nullopt;
  }
  return policy;
}

using neighbor_backend_mask = std::uint64_t;

[[nodiscard]] constexpr auto backend_bit(
    neighbor_direct_backend backend) noexcept -> neighbor_backend_mask {
  return neighbor_backend_mask{1} << static_cast<std::uint8_t>(backend);
}

struct neighbor_backend_masks final {
  neighbor_backend_mask allowed = 0;
  neighbor_backend_mask physical = 0;

  auto operator==(neighbor_backend_masks const&) const -> bool = default;
};

[[nodiscard]] constexpr auto compiled_backend_mask(bool force_mpi3) noexcept
    -> neighbor_backend_mask {
  auto result = neighbor_backend_mask{0};
  if constexpr (KAHIP_HAVE_MPI_INEIGHBOR_ALLTOALLV != 0) {
    result |= backend_bit(neighbor_direct_backend::immediate_legacy);
  }
  if (!force_mpi3) {
    if constexpr (KAHIP_HAVE_MPI_INEIGHBOR_ALLTOALLV_C != 0) {
      result |= backend_bit(neighbor_direct_backend::immediate_large_count);
    }
    if constexpr (KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_INIT != 0) {
      result |= backend_bit(neighbor_direct_backend::persistent_legacy);
    }
    if constexpr (KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_INIT_C != 0) {
      result |= backend_bit(neighbor_direct_backend::persistent_large_count);
    }
  }
  return result;
}

[[nodiscard]] constexpr auto policy_backend_mask(
    persistence_policy policy) noexcept -> neighbor_backend_mask {
  constexpr auto immediate =
      backend_bit(neighbor_direct_backend::immediate_legacy) |
      backend_bit(neighbor_direct_backend::immediate_large_count);
  constexpr auto persistent =
      backend_bit(neighbor_direct_backend::persistent_legacy) |
      backend_bit(neighbor_direct_backend::persistent_large_count);
  switch (policy) {
    case persistence_policy::disabled:
      return immediate;
    case persistence_policy::prefer:
      return persistent | immediate;
    case persistence_policy::required:
      return persistent;
  }
  return 0;
}

[[nodiscard]] constexpr auto choose_direct_backend(
    neighbor_backend_mask common_allowed) noexcept
    -> std::optional<neighbor_direct_backend> {
  constexpr auto precedence = std::array{
      neighbor_direct_backend::persistent_large_count,
      neighbor_direct_backend::persistent_legacy,
      neighbor_direct_backend::immediate_large_count,
      neighbor_direct_backend::immediate_legacy,
  };
  auto const selected = std::ranges::find_if(precedence, [&](auto backend) {
    return (common_allowed & backend_bit(backend)) != 0;
  });
  return selected == precedence.end()
             ? std::nullopt
             : std::optional<neighbor_direct_backend>{*selected};
}

[[nodiscard]] inline auto agree_neighbor_backend_masks(
    neighbor_backend_masks local,
    communicator_view communicator) -> neighbor_backend_masks {
  auto const local_masks = std::array{local.allowed, local.physical};
  auto common_masks = std::array<neighbor_backend_mask, 2>{};
  check_or_abort(
      MPI_Allreduce(local_masks.data(), common_masks.data(),
                    static_cast<int>(common_masks.size()), MPI_UINT64_T,
                    MPI_BAND, communicator.native_handle()),
      communicator.native_handle(),
      "MPI_Allreduce(direct neighborhood backend agreement)");
  return neighbor_backend_masks{
      .allowed = common_masks[0],
      .physical = common_masks[1],
  };
}

template <typename T>
struct pending_neighbor_sends final {
  std::optional<segmented_buffer<T>> materialized;
  std::vector<std::size_t> fixed_counts;
  std::vector<std::size_t> fixed_offsets;
  std::size_t fixed_element_count = 0;
  capacity_result capacity;

  [[nodiscard]] static auto one_shot(segmented_buffer<T> sends)
      -> pending_neighbor_sends {
    auto result = pending_neighbor_sends{};
    result.materialized.emplace(std::move(sends));
    return result;
  }

  [[nodiscard]] static auto fixed(std::vector<std::size_t> counts)
      -> pending_neighbor_sends {
    auto result = pending_neighbor_sends{};
    result.fixed_counts = std::move(counts);
    return result;
  }

  [[nodiscard]] auto is_fixed() const noexcept -> bool {
    return !materialized.has_value();
  }

  [[nodiscard]] auto locally_valid(std::size_t expected_segments) const noexcept
      -> bool {
    return is_fixed() ? fixed_counts.size() == expected_segments
                      : materialized->has_canonical_layout(expected_segments);
  }

  void prepare_fixed_layout() {
    if (!is_fixed()) {
      return;
    }
    auto layout = canonical_neighbor_layout(fixed_counts);
    fixed_offsets = std::move(layout.offsets);
    fixed_element_count = layout.element_count;
    capacity = layout.capacity;
    if (fixed_element_count >
        std::numeric_limits<std::size_t>::max() / sizeof(T)) {
      capacity = with_fatal_capacity_issue(
          capacity, capacity_issue::storage_byte_size_overflow);
    }
  }

  [[nodiscard]] auto counts() const noexcept -> std::span<std::size_t const> {
    return is_fixed() ? std::span<std::size_t const>{fixed_counts}
                      : std::span<std::size_t const>{materialized->counts()};
  }

  [[nodiscard]] auto offsets() const noexcept -> std::span<std::size_t const> {
    return is_fixed() ? std::span<std::size_t const>{fixed_offsets}
                      : std::span<std::size_t const>{materialized->offsets()};
  }

  [[nodiscard]] auto materialize() -> segmented_buffer<T> {
    if (materialized.has_value()) {
      return std::move(*materialized);
    }
    return segmented_buffer<T>::uninitialized(
        fixed_element_count, std::move(fixed_counts), std::move(fixed_offsets));
  }
};

struct legacy_neighbor_layout final {
  std::vector<int> send_counts;
  std::vector<int> send_offsets;
  std::vector<int> receive_counts;
  std::vector<int> receive_offsets;
};

struct large_count_neighbor_layout final {
  std::vector<MPI_Count> send_counts;
  std::vector<MPI_Aint> send_offsets;
  std::vector<MPI_Count> receive_counts;
  std::vector<MPI_Aint> receive_offsets;
};

using direct_neighbor_layout =
    std::variant<std::monostate, legacy_neighbor_layout,
                 large_count_neighbor_layout>;

static_assert(std::is_nothrow_move_constructible_v<direct_neighbor_layout>);
static_assert(std::is_nothrow_move_assignable_v<direct_neighbor_layout>);

[[nodiscard]] inline auto checked_neighbor_mpi_count(std::size_t value,
                                                     std::string_view context)
    -> MPI_Count {
  if (!std::in_range<MPI_Count>(value)) {
    throw mpi_error{MPI_ERR_COUNT, std::string{context}};
  }
  return static_cast<MPI_Count>(value);
}

[[nodiscard]] inline auto checked_neighbor_mpi_aint(std::size_t value,
                                                    std::string_view context)
    -> MPI_Aint {
  if (!std::in_range<MPI_Aint>(value)) {
    throw mpi_error{MPI_ERR_COUNT, std::string{context}};
  }
  return static_cast<MPI_Aint>(value);
}

template <typename Result, typename Conversion>
[[nodiscard]] auto convert_neighbor_layout(std::span<std::size_t const> values,
                                           Conversion conversion)
    -> std::vector<Result> {
  auto result = std::vector<Result>{};
  result.reserve(values.size());
  std::ranges::transform(values, std::back_inserter(result), conversion);
  return result;
}

[[nodiscard]] inline auto build_direct_neighbor_layout(
    neighbor_direct_backend backend,
    std::span<std::size_t const> send_counts,
    std::span<std::size_t const> send_offsets,
    std::span<std::size_t const> receive_counts,
    std::span<std::size_t const> receive_offsets) -> direct_neighbor_layout {
  if (uses_large_count(backend)) {
    return large_count_neighbor_layout{
        .send_counts = convert_neighbor_layout<MPI_Count>(
            send_counts,
            [](auto value) {
              return checked_neighbor_mpi_count(value, "neighbor send count");
            }),
        .send_offsets = convert_neighbor_layout<MPI_Aint>(
            send_offsets,
            [](auto value) {
              return checked_neighbor_mpi_aint(value, "neighbor send offset");
            }),
        .receive_counts = convert_neighbor_layout<MPI_Count>(
            receive_counts,
            [](auto value) {
              return checked_neighbor_mpi_count(value,
                                                "neighbor receive count");
            }),
        .receive_offsets = convert_neighbor_layout<MPI_Aint>(
            receive_offsets,
            [](auto value) {
              return checked_neighbor_mpi_aint(value,
                                               "neighbor receive offset");
            }),
    };
  }
  return legacy_neighbor_layout{
      .send_counts = convert_neighbor_layout<int>(
          send_counts,
          [](auto value) { return checked_int(value, "neighbor send count"); }),
      .send_offsets = convert_neighbor_layout<int>(
          send_offsets,
          [](auto value) {
            return checked_int(value, "neighbor send offset");
          }),
      .receive_counts = convert_neighbor_layout<int>(
          receive_counts,
          [](auto value) {
            return checked_int(value, "neighbor receive count");
          }),
      .receive_offsets = convert_neighbor_layout<int>(
          receive_offsets,
          [](auto value) {
            return checked_int(value, "neighbor receive offset");
          }),
  };
}

template <typename T>
struct direct_neighbor_storage {
  direct_neighbor_storage(communicator operation_communicator,
                          datatype operation_datatype,
                          segmented_buffer<T> send_buffer,
                          segmented_buffer<T> receive_buffer,
                          direct_neighbor_layout layout,
                          neighbor_direct_backend selected_backend,
                          std::optional<mpi3_bounded_neighbor_plan>
                              bounded_plan = std::nullopt)
      : communicator_(std::move(operation_communicator)),
        datatype_(std::move(operation_datatype)),
        sends_(std::move(send_buffer)),
        received_(std::move(receive_buffer)),
        layout_(std::move(layout)),
        backend_(selected_backend),
        bounded_plan_(std::move(bounded_plan)),
        bounded_send_counts_(sends_.segment_count(), 0),
        bounded_receive_counts_(received_.segment_count(), 0),
        bounded_send_displacements_(sends_.segment_count(), 0),
        bounded_receive_displacements_(received_.segment_count(), 0) {}

  direct_neighbor_storage(direct_neighbor_storage const&) = delete;
  auto operator=(direct_neighbor_storage const&)
      -> direct_neighbor_storage& = delete;
  direct_neighbor_storage(direct_neighbor_storage&&) = delete;
  auto operator=(direct_neighbor_storage&&)
      -> direct_neighbor_storage& = delete;

  [[nodiscard]] auto view() const noexcept -> communicator_view {
    return communicator_.view();
  }

  [[nodiscard]] auto send_buffer() const noexcept -> void const* {
    auto const storage = sends_.storage();
    return storage.empty()
               ? static_cast<void const*>(std::addressof(ignored_send_byte_))
               : static_cast<void const*>(storage.data());
  }

  [[nodiscard]] auto receive_buffer() noexcept -> void* {
    auto storage = received_.storage();
    return storage.empty()
               ? static_cast<void*>(std::addressof(ignored_receive_byte_))
               : static_cast<void*>(storage.data());
  }

  template <typename Value>
  [[nodiscard]] static auto data_or_ignored(std::vector<Value> const& values,
                                            Value const& ignored) noexcept
      -> Value const* {
    return values.empty() ? std::addressof(ignored) : values.data();
  }

  [[nodiscard]] auto legacy_layout() const noexcept
      -> legacy_neighbor_layout const* {
    return std::get_if<legacy_neighbor_layout>(&layout_);
  }

  [[nodiscard]] auto large_count_layout() const noexcept
      -> large_count_neighbor_layout const* {
    return std::get_if<large_count_neighbor_layout>(&layout_);
  }

  [[nodiscard]] auto is_bounded() const noexcept -> bool {
    return backend_ == neighbor_direct_backend::bounded_legacy;
  }

  void seek_next_bounded_round() noexcept {
    while (bounded_phase_ < bounded_plan_->phases.size() &&
           bounded_round_ >=
               bounded_plan_->phases[bounded_phase_].round_count) {
      ++bounded_phase_;
      bounded_round_ = 0;
    }
  }

  [[nodiscard]] auto has_bounded_round() const noexcept -> bool {
    return bounded_phase_ < bounded_plan_->phases.size();
  }

  void launch_bounded_round() noexcept {
    std::ranges::fill(bounded_send_counts_, 0);
    std::ranges::fill(bounded_receive_counts_, 0);
    auto const round = make_mpi3_bounded_neighbor_round(
        *bounded_plan_, bounded_phase_, bounded_round_, sends_.offsets(),
        received_.offsets());
    if (round.destination_index.has_value()) {
      bounded_send_counts_[*round.destination_index] = round.send_count;
    }
    if (round.source_index.has_value()) {
      bounded_receive_counts_[*round.source_index] = round.receive_count;
    }
    auto const* round_send_buffer =
        round.send_storage_offset.has_value()
            ? static_cast<void const*>(sends_.storage().data() +
                                       *round.send_storage_offset)
            : send_buffer();
    auto* round_receive_buffer =
        round.receive_storage_offset.has_value()
            ? static_cast<void*>(received_.storage().data() +
                                 *round.receive_storage_offset)
            : receive_buffer();
    check_or_abort(
        MPI_Ineighbor_alltoallv(
            round_send_buffer,
            data_or_ignored(bounded_send_counts_, ignored_int_),
            data_or_ignored(bounded_send_displacements_, ignored_int_),
            datatype_.native_handle(), round_receive_buffer,
            data_or_ignored(bounded_receive_counts_, ignored_int_),
            data_or_ignored(bounded_receive_displacements_, ignored_int_),
            datatype_.native_handle(), communicator_.native_handle(),
            &request_),
        communicator_.native_handle(),
        "MPI_Ineighbor_alltoallv(MPI-3 bounded neighborhood round)");
  }

  void begin_bounded_generation() noexcept {
    if (!bounded_plan_.has_value()) {
      abort_on_programming_error(
          communicator_.native_handle(),
          "bounded neighborhood backend requires a bounded MPI-3 plan");
    }
    bounded_phase_ = 0;
    bounded_round_ = 0;
    seek_next_bounded_round();
    if (!has_bounded_round()) {
      active_ = false;
      receive_ready_ = true;
      return;
    }
    launch_bounded_round();
  }

  void advance_bounded_round() noexcept {
    ++bounded_round_;
    seek_next_bounded_round();
    if (has_bounded_round()) {
      launch_bounded_round();
      return;
    }
    active_ = false;
    receive_ready_ = true;
  }

  [[nodiscard]] auto test_bounded_generation() noexcept -> bool {
    auto complete = 0;
    check_or_abort(MPI_Test(&request_, &complete, MPI_STATUS_IGNORE),
                   communicator_.native_handle(),
                   "MPI_Test(MPI-3 bounded neighborhood round)");
    if (complete == 0) {
      return false;
    }
    advance_bounded_round();
    return !active_;
  }

  void wait_bounded_generation() noexcept {
    while (active_) {
      check_or_abort(MPI_Wait(&request_, MPI_STATUS_IGNORE),
                     communicator_.native_handle(),
                     "MPI_Wait(MPI-3 bounded neighborhood round)");
      advance_bounded_round();
    }
  }

  void initiate_immediate() noexcept {
    active_ = true;
    receive_ready_ = false;
    if (is_bounded()) {
      begin_bounded_generation();
      return;
    }
    if (backend_ == neighbor_direct_backend::immediate_legacy) {
      auto const* layout = legacy_layout();
      if (layout == nullptr) {
        abort_on_programming_error(
            communicator_.native_handle(),
            "legacy neighborhood backend requires a legacy MPI layout");
      }
      check_or_abort(
          MPI_Ineighbor_alltoallv(
              send_buffer(), data_or_ignored(layout->send_counts, ignored_int_),
              data_or_ignored(layout->send_offsets, ignored_int_),
              datatype_.native_handle(), receive_buffer(),
              data_or_ignored(layout->receive_counts, ignored_int_),
              data_or_ignored(layout->receive_offsets, ignored_int_),
              datatype_.native_handle(), communicator_.native_handle(),
              &request_),
          communicator_.native_handle(),
          "MPI_Ineighbor_alltoallv(immediate neighborhood exchange)");
      return;
    }
#if KAHIP_HAVE_MPI_INEIGHBOR_ALLTOALLV_C
    if (backend_ == neighbor_direct_backend::immediate_large_count) {
      auto const* layout = large_count_layout();
      if (layout == nullptr) {
        abort_on_programming_error(
            communicator_.native_handle(),
            "large-count neighborhood backend requires a large-count MPI "
            "layout");
      }
      check_or_abort(
          MPI_Ineighbor_alltoallv_c(
              send_buffer(),
              data_or_ignored(layout->send_counts, ignored_count_),
              data_or_ignored(layout->send_offsets, ignored_aint_),
              datatype_.native_handle(), receive_buffer(),
              data_or_ignored(layout->receive_counts, ignored_count_),
              data_or_ignored(layout->receive_offsets, ignored_aint_),
              datatype_.native_handle(), communicator_.native_handle(),
              &request_),
          communicator_.native_handle(),
          "MPI_Ineighbor_alltoallv_c(immediate neighborhood exchange)");
      return;
    }
#endif
    abort_on_programming_error(communicator_.native_handle(),
                               "immediate initiation selected a persistent "
                               "neighborhood backend");
  }

  void initialize_persistent() noexcept {
    if (backend_ == neighbor_direct_backend::persistent_legacy) {
#if KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_INIT
      auto const* layout = legacy_layout();
      if (layout == nullptr) {
        abort_on_programming_error(
            communicator_.native_handle(),
            "legacy neighborhood backend requires a legacy MPI layout");
      }
      check_or_abort(
          MPI_Neighbor_alltoallv_init(
              send_buffer(), data_or_ignored(layout->send_counts, ignored_int_),
              data_or_ignored(layout->send_offsets, ignored_int_),
              datatype_.native_handle(), receive_buffer(),
              data_or_ignored(layout->receive_counts, ignored_int_),
              data_or_ignored(layout->receive_offsets, ignored_int_),
              datatype_.native_handle(), communicator_.native_handle(),
              MPI_INFO_NULL, &request_),
          communicator_.native_handle(),
          "MPI_Neighbor_alltoallv_init(persistent neighborhood exchange)");
      return;
#endif
    }
#if KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_INIT_C
    if (backend_ == neighbor_direct_backend::persistent_large_count) {
      auto const* layout = large_count_layout();
      if (layout == nullptr) {
        abort_on_programming_error(
            communicator_.native_handle(),
            "large-count neighborhood backend requires a large-count MPI "
            "layout");
      }
      check_or_abort(
          MPI_Neighbor_alltoallv_init_c(
              send_buffer(),
              data_or_ignored(layout->send_counts, ignored_count_),
              data_or_ignored(layout->send_offsets, ignored_aint_),
              datatype_.native_handle(), receive_buffer(),
              data_or_ignored(layout->receive_counts, ignored_count_),
              data_or_ignored(layout->receive_offsets, ignored_aint_),
              datatype_.native_handle(), communicator_.native_handle(),
              MPI_INFO_NULL, &request_),
          communicator_.native_handle(),
          "MPI_Neighbor_alltoallv_init_c(persistent neighborhood exchange)");
      return;
    }
#endif
    abort_on_programming_error(communicator_.native_handle(),
                               "persistent initialization selected an "
                               "unavailable neighborhood backend");
  }

  void start_generation() noexcept {
    require_active_runtime("neighborhood operation start");
    if (active_) {
      abort_on_programming_error(communicator_.native_handle(),
                                 "neighbor context start requires an "
                                 "inactive generation");
    }
    receive_ready_ = false;
    active_ = true;
    if (is_persistent(backend_)) {
      check_or_abort(MPI_Start(&request_), communicator_.native_handle(),
                     "MPI_Start(persistent neighborhood exchange)");
      return;
    }
    initiate_immediate();
  }

  [[nodiscard]] auto test_generation() noexcept -> bool {
    require_active_runtime("neighborhood operation test");
    if (!active_) {
      abort_on_programming_error(communicator_.native_handle(),
                                 "neighbor context test requires an active "
                                 "generation");
    }
    if (is_bounded()) {
      return test_bounded_generation();
    }
    auto complete = 0;
    check_or_abort(MPI_Test(&request_, &complete, MPI_STATUS_IGNORE),
                   communicator_.native_handle(),
                   "MPI_Test(neighborhood exchange)");
    if (complete != 0) {
      active_ = false;
      receive_ready_ = true;
    }
    return complete != 0;
  }

  void wait_generation() noexcept {
    require_active_runtime("neighborhood operation wait");
    if (!active_) {
      abort_on_programming_error(communicator_.native_handle(),
                                 "neighbor context wait requires an active "
                                 "generation");
    }
    if (is_bounded()) {
      wait_bounded_generation();
      return;
    }
    check_or_abort(MPI_Wait(&request_, MPI_STATUS_IGNORE),
                   communicator_.native_handle(),
                   "MPI_Wait(neighborhood exchange)");
    active_ = false;
    receive_ready_ = true;
  }

  void complete_active_for_destruction() noexcept {
    require_active_runtime("neighborhood operation destruction");
    if (active_) {
      if (is_bounded()) {
        wait_bounded_generation();
        return;
      }
      check_or_abort(MPI_Wait(&request_, MPI_STATUS_IGNORE),
                     communicator_.native_handle(),
                     "MPI_Wait(active neighborhood destruction)");
      active_ = false;
      receive_ready_ = true;
    }
  }

  void release_persistent_request() noexcept {
    if (!is_persistent(backend_)) {
      return;
    }
    if (active_) {
      abort_on_programming_error(communicator_.native_handle(),
                                 "persistent request free requires an "
                                 "inactive generation");
    }
    check_or_abort(MPI_Request_free(&request_), communicator_.native_handle(),
                   "MPI_Request_free(persistent neighborhood exchange)");
  }

  void require_active_runtime(std::string_view context) const noexcept {
    if (!runtime_is_active()) {
      abort_on_inactive_mpi_ownership(context);
    }
  }

  [[nodiscard]] auto send_segment(std::size_t index) noexcept -> std::span<T> {
    require_active_runtime("neighborhood send-buffer access");
    if (active_ || index >= sends_.segment_count()) {
      abort_on_programming_error(communicator_.native_handle(),
                                 active_ ? "neighbor context send mutation "
                                           "requires an inactive generation"
                                         : "neighbor context send segment "
                                           "index is out of range");
    }
    return sends_.segment(index);
  }

  [[nodiscard]] auto received_segment(std::size_t index) const noexcept
      -> std::span<T const> {
    require_active_runtime("neighborhood receive-buffer access");
    if (!receive_ready_ || active_ || index >= received_.segment_count()) {
      abort_on_programming_error(
          communicator_.native_handle(),
          !receive_ready_ || active_
              ? "neighbor context receive access requires a completed "
                "generation"
              : "neighbor context receive segment index is out of range");
    }
    return received_.segment(index);
  }

  communicator communicator_;
  datatype datatype_;
  segmented_buffer<T> sends_;
  segmented_buffer<T> received_;
  direct_neighbor_layout layout_;
  neighbor_direct_backend backend_;
  std::optional<mpi3_bounded_neighbor_plan> bounded_plan_;
  std::vector<int> bounded_send_counts_;
  std::vector<int> bounded_receive_counts_;
  std::vector<int> bounded_send_displacements_;
  std::vector<int> bounded_receive_displacements_;
  std::size_t bounded_phase_ = 0;
  std::size_t bounded_round_ = 0;
  MPI_Request request_ = MPI_REQUEST_NULL;
  bool active_ = false;
  bool receive_ready_ = false;
  int ignored_int_ = 0;
  MPI_Count ignored_count_ = 0;
  MPI_Aint ignored_aint_ = 0;
  std::byte ignored_send_byte_{};
  std::byte ignored_receive_byte_{};
};

[[nodiscard]] constexpr auto filter_local_backend_masks(
    neighbor_backend_mask available,
    persistence_policy persistence,
    bool policy_legacy_representable,
    bool physical_legacy_representable,
    bool large_count_representable) noexcept -> neighbor_backend_masks {
  constexpr auto legacy =
      backend_bit(neighbor_direct_backend::immediate_legacy) |
      backend_bit(neighbor_direct_backend::persistent_legacy);
  constexpr auto large_count =
      backend_bit(neighbor_direct_backend::immediate_large_count) |
      backend_bit(neighbor_direct_backend::persistent_large_count);
  auto allowed = available & policy_backend_mask(persistence);
  auto physical = available;
  if (!policy_legacy_representable) {
    allowed &= ~legacy;
  }
  if (!physical_legacy_representable) {
    physical &= ~legacy;
  }
  if (!large_count_representable) {
    physical &= ~large_count;
  }
  allowed &= physical;
  return neighbor_backend_masks{
      .allowed = allowed,
      .physical = physical,
  };
}

[[nodiscard]] constexpr auto make_local_backend_masks(
    persistence_policy persistence,
    bool force_mpi3,
    bool policy_legacy_representable,
    bool physical_legacy_representable,
    bool large_count_representable) noexcept -> neighbor_backend_masks {
  return filter_local_backend_masks(compiled_backend_mask(force_mpi3),
                                    persistence, policy_legacy_representable,
                                    physical_legacy_representable,
                                    large_count_representable);
}

template <typename T>
[[nodiscard]] auto prepare_direct_neighbor_storage(
    pending_neighbor_sends<T> pending_sends,
    distributed_graph const& graph,
    collective_options collective,
    persistence_policy persistence)
    -> std::unique_ptr<direct_neighbor_storage<T>> {
  auto semantic_failure = std::string_view{};
  auto result = std::unique_ptr<direct_neighbor_storage<T>>{};
  {
    auto operation_communicator = communicator{graph.view()};
    auto const collective_communicator = operation_communicator.view();
    try {
      auto const layout_is_valid = collective_predicate(
          pending_sends.locally_valid(graph.destinations().size()),
          collective_communicator);
      auto const mpi3_ceiling =
          validate_collective_options(collective, collective_communicator);
      auto const agreed_persistence =
          validate_persistence_policy(persistence, collective_communicator);
      if (!layout_is_valid) {
        semantic_failure = pending_sends.is_fixed()
                               ? "fixed neighborhood send layout validation "
                                 "failed"
                               : "direct neighborhood exchange input "
                                 "validation failed";
      } else if (!mpi3_ceiling.has_value() || !agreed_persistence.has_value()) {
        semantic_failure =
            "direct neighborhood exchange options must agree collectively";
      } else {
        auto const compiled_eligible =
            compiled_backend_mask(collective.force_mpi3) &
            policy_backend_mask(*agreed_persistence);
        if (compiled_eligible == 0) {
          semantic_failure =
              *agreed_persistence == persistence_policy::required
                  ? "persistent neighborhood exchange is unavailable"
                  : "direct neighborhood exchange is unavailable";
        }
      }

      if (semantic_failure.empty()) {
        pending_sends.prepare_fixed_layout();
        auto receive_count_exchange = exchange_neighbor_counts(
            pending_sends.counts(), graph.sources().size(),
            collective_communicator);
        auto receive_layout =
            canonical_neighbor_layout(receive_count_exchange.counts);
        auto local_capacity = combine_capacity_results(
            pending_sends.capacity,
            combine_capacity_results(receive_count_exchange.capacity,
                                     receive_layout.capacity));
        if (receive_layout.element_count >
            std::numeric_limits<std::size_t>::max() / sizeof(T)) {
          local_capacity = with_fatal_capacity_issue(
              local_capacity, capacity_issue::storage_byte_size_overflow);
        }

        auto const policy_legacy_representable =
            neighbor_mpi3_layout_is_representable_locally(
                pending_sends.counts(), pending_sends.offsets(),
                receive_count_exchange.counts, receive_layout.offsets,
                *mpi3_ceiling);
        auto const physical_legacy_representable =
            neighbor_mpi3_layout_is_representable_locally(
                pending_sends.counts(), pending_sends.offsets(),
                receive_count_exchange.counts, receive_layout.offsets,
                static_cast<std::size_t>(std::numeric_limits<int>::max()));
        auto const large_count_representable =
            neighbor_mpi4_layout_is_representable_locally(
                pending_sends.counts(), pending_sends.offsets(),
                receive_count_exchange.counts, receive_layout.offsets);
        auto const common_backends = agree_neighbor_backend_masks(
            make_local_backend_masks(*agreed_persistence, collective.force_mpi3,
                                     policy_legacy_representable,
                                     physical_legacy_representable,
                                     large_count_representable),
            collective_communicator);
        if (common_backends.allowed == 0) {
          local_capacity = with_bounded_capacity_issue(
              local_capacity, capacity_issue::direct_backend_not_representable);
        }

        auto const route = resolve_capacity_collectively(
            local_capacity, collective_communicator.native_handle(),
            collective_communicator.native_handle(),
            "direct neighborhood exchange");
        if (route == capacity_route::bounded) {
          if (*agreed_persistence == persistence_policy::required) {
            semantic_failure =
                "persistent neighborhood exchange requires a single "
                "representable payload";
          } else {
            auto bounded_plan = make_mpi3_bounded_neighbor_plan(
                pending_sends.counts(), pending_sends.offsets(),
                receive_count_exchange.counts, receive_layout.offsets,
                *mpi3_ceiling, graph, collective_communicator);
            auto sends = pending_sends.materialize();
            auto received = segmented_buffer<T>::uninitialized(
                receive_layout.element_count,
                std::move(receive_count_exchange.counts),
                std::move(receive_layout.offsets));
            auto operation_datatype =
                make_mpi_datatype<T>(collective_communicator.native_handle());
            result = std::make_unique<direct_neighbor_storage<T>>(
                std::move(operation_communicator),
                std::move(operation_datatype), std::move(sends),
                std::move(received), direct_neighbor_layout{std::monostate{}},
                neighbor_direct_backend::bounded_legacy,
                std::move(bounded_plan));
          }
        } else if (auto const backend =
                       choose_direct_backend(common_backends.allowed);
                   backend.has_value()) {
          auto mpi_layout = build_direct_neighbor_layout(
              *backend, pending_sends.counts(), pending_sends.offsets(),
              receive_count_exchange.counts, receive_layout.offsets);
          auto sends = pending_sends.materialize();
          auto received = segmented_buffer<T>::uninitialized(
              receive_layout.element_count,
              std::move(receive_count_exchange.counts),
              std::move(receive_layout.offsets));
          auto operation_datatype =
              make_mpi_datatype<T>(collective_communicator.native_handle());
          result = std::make_unique<direct_neighbor_storage<T>>(
              std::move(operation_communicator), std::move(operation_datatype),
              std::move(sends), std::move(received), std::move(mpi_layout),
              *backend);
        } else {
          abort_on_programming_error(
              collective_communicator.native_handle(),
              "direct neighborhood backend agreement selected no backend");
        }
      }
    } catch (...) {
      abort_on_exception(collective_communicator.native_handle(),
                         "direct neighborhood exchange local failure");
    }
  }
  // KAHIP_SEMANTIC_EXIT_BEGIN(async-direct)
  if (!semantic_failure.empty()) {
    throw_collectively_agreed_semantic_error(graph.native_handle(),
                                             semantic_failure);
  }
  // KAHIP_SEMANTIC_EXIT_END(async-direct)
  return result;
}
}  // namespace detail

template <mpi_datatype T>
class neighbor_exchange_request {
 public:
  neighbor_exchange_request(neighbor_exchange_request const&) = delete;
  auto operator=(neighbor_exchange_request const&)
      -> neighbor_exchange_request& = delete;
  neighbor_exchange_request(neighbor_exchange_request&&) noexcept = default;
  auto operator=(neighbor_exchange_request&&)
      -> neighbor_exchange_request& = delete;

  ~neighbor_exchange_request() noexcept {
    if (state_ != nullptr) {
      state_->complete_active_for_destruction();
    }
  }

  [[nodiscard]] auto test() noexcept -> bool {
    require_state("one-shot neighborhood test on moved-from request");
    state_->require_active_runtime("one-shot neighborhood test");
    if (!state_->active_) {
      return true;
    }
    return state_->test_generation();
  }

  [[nodiscard]] auto wait() && -> segmented_buffer<T> {
    require_state("one-shot neighborhood wait on moved-from request");
    state_->require_active_runtime("one-shot neighborhood wait");
    if (state_->active_) {
      state_->wait_generation();
    }
    auto result = std::move(state_->received_);
    state_.reset();
    return result;
  }

 private:
  explicit neighbor_exchange_request(
      std::unique_ptr<detail::direct_neighbor_storage<T>> state) noexcept
      : state_(std::move(state)) {}

  void require_state(std::string_view context) const noexcept {
    if (state_ == nullptr) {
      abort_on_programming_error(MPI_COMM_WORLD, context);
    }
  }

  std::unique_ptr<detail::direct_neighbor_storage<T>> state_;

  template <mpi_datatype U>
  friend auto start_neighbor_all_to_all_v(segmented_buffer<U>,
                                          distributed_graph const&,
                                          collective_options)
      -> neighbor_exchange_request<U>;
};

template <mpi_datatype T>
[[nodiscard]] auto start_neighbor_all_to_all_v(segmented_buffer<T> sends,
                                               distributed_graph const& graph,
                                               collective_options options = {})
    -> neighbor_exchange_request<T> {
  auto state = detail::prepare_direct_neighbor_storage(
      detail::pending_neighbor_sends<T>::one_shot(std::move(sends)), graph,
      options, persistence_policy::disabled);
  state->initiate_immediate();
  return neighbor_exchange_request<T>{std::move(state)};
}

template <mpi_datatype T>
class neighbor_all_to_all_v_context {
 public:
  neighbor_all_to_all_v_context(distributed_graph const& graph,
                                std::vector<std::size_t> send_counts,
                                context_options options = {})
      : state_(detail::prepare_direct_neighbor_storage(
            detail::pending_neighbor_sends<T>::fixed(std::move(send_counts)),
            graph,
            options.collective,
            options.persistence)) {
    if (detail::is_persistent(state_->backend_)) {
      state_->initialize_persistent();
    }
  }

  neighbor_all_to_all_v_context(neighbor_all_to_all_v_context const&) = delete;
  auto operator=(neighbor_all_to_all_v_context const&)
      -> neighbor_all_to_all_v_context& = delete;
  neighbor_all_to_all_v_context(neighbor_all_to_all_v_context&&) = delete;
  auto operator=(neighbor_all_to_all_v_context&&)
      -> neighbor_all_to_all_v_context& = delete;

  ~neighbor_all_to_all_v_context() noexcept {
    state_->complete_active_for_destruction();
    state_->release_persistent_request();
  }

  [[nodiscard]] auto send_segment(std::size_t index) noexcept -> std::span<T> {
    return state_->send_segment(index);
  }

  void start() noexcept { state_->start_generation(); }

  [[nodiscard]] auto test() noexcept -> bool {
    return state_->test_generation();
  }

  void wait() noexcept { state_->wait_generation(); }

  [[nodiscard]] auto received_segment(std::size_t index) const noexcept
      -> std::span<T const> {
    return state_->received_segment(index);
  }

 private:
  std::unique_ptr<detail::direct_neighbor_storage<T>> state_;
};
}  // namespace parhip::mpi
