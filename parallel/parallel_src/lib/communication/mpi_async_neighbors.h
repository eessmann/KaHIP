#pragma once

#include <mpi.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "communication/mpi_neighbors.h"

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

template <typename T>
[[nodiscard]] auto make_fixed_neighbor_sends(distributed_graph const& graph,
                                             std::vector<std::size_t> counts)
    -> segmented_buffer<T> {
  auto semantic_failure = std::string_view{};
  auto deferred_capacity_failure = std::string_view{};
  auto result = std::optional<segmented_buffer<T>>{};
  {
    auto owned_communicator = communicator{graph.view()};
    auto const collective_communicator = owned_communicator.view();
    try {
      auto const cardinality_is_valid =
          collective_predicate(counts.size() == graph.destinations().size(),
                               collective_communicator);
      if (!cardinality_is_valid) {
        semantic_failure = "fixed neighborhood send layout validation failed";
      } else {
        auto offsets = neighbor_offsets(counts);
        auto storage_size = std::optional<std::size_t>{};
        if (offsets.has_value()) {
          auto const element_count = neighbor_storage_size(counts, *offsets);
          if (element_count <=
              std::numeric_limits<std::size_t>::max() / sizeof(T)) {
            storage_size = element_count;
          }
        }
        auto const capacity_is_valid = collective_predicate(
            offsets.has_value() && storage_size.has_value(),
            collective_communicator);
        if (!capacity_is_valid) {
          deferred_capacity_failure =
              "fixed neighborhood send layout exceeds local capacity";
        } else {
          result.emplace(segmented_buffer<T>::uninitialized(
              *storage_size, std::move(counts), std::move(*offsets)));
        }
      }
    } catch (...) {
      abort_on_exception(collective_communicator.native_handle(),
                         "fixed neighborhood send allocation failure");
    }
  }
  // KAHIP_SEMANTIC_EXIT_BEGIN(fixed-neighbor-cardinality)
  if (!semantic_failure.empty()) {
    throw_collectively_agreed_semantic_error(graph.native_handle(),
                                             semantic_failure);
  }
  // KAHIP_SEMANTIC_EXIT_END(fixed-neighbor-cardinality)
  if (!deferred_capacity_failure.empty()) {
    throw mpi_error{MPI_ERR_ARG, std::string{deferred_capacity_failure}};
  }
  return std::move(*result);
}

template <typename T>
struct direct_neighbor_storage {
  direct_neighbor_storage(communicator operation_communicator,
                          datatype operation_datatype,
                          segmented_buffer<T> send_buffer,
                          segmented_buffer<T> receive_buffer,
                          std::vector<int> source_ranks,
                          std::vector<int> destination_ranks,
                          neighbor_direct_backend selected_backend)
      : communicator_(std::move(operation_communicator)),
        datatype_(std::move(operation_datatype)),
        sends_(std::move(send_buffer)),
        received_(std::move(receive_buffer)),
        sources_(std::move(source_ranks)),
        destinations_(std::move(destination_ranks)),
        backend_(selected_backend) {
    build_mpi_layout();
  }

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

  void initiate_immediate() noexcept {
    active_ = true;
    receive_ready_ = false;
    if (backend_ == neighbor_direct_backend::immediate_legacy) {
      check_or_abort(
          MPI_Ineighbor_alltoallv(
              send_buffer(), data_or_ignored(send_counts_i_, ignored_int_),
              data_or_ignored(send_offsets_i_, ignored_int_),
              datatype_.native_handle(), receive_buffer(),
              data_or_ignored(receive_counts_i_, ignored_int_),
              data_or_ignored(receive_offsets_i_, ignored_int_),
              datatype_.native_handle(), communicator_.native_handle(),
              &request_),
          communicator_.native_handle(),
          "MPI_Ineighbor_alltoallv(immediate neighborhood exchange)");
      return;
    }
#if KAHIP_HAVE_MPI_INEIGHBOR_ALLTOALLV_C
    if (backend_ == neighbor_direct_backend::immediate_large_count) {
      check_or_abort(
          MPI_Ineighbor_alltoallv_c(
              send_buffer(), data_or_ignored(send_counts_c_, ignored_count_),
              data_or_ignored(send_offsets_c_, ignored_aint_),
              datatype_.native_handle(), receive_buffer(),
              data_or_ignored(receive_counts_c_, ignored_count_),
              data_or_ignored(receive_offsets_c_, ignored_aint_),
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
      check_or_abort(
          MPI_Neighbor_alltoallv_init(
              send_buffer(), data_or_ignored(send_counts_i_, ignored_int_),
              data_or_ignored(send_offsets_i_, ignored_int_),
              datatype_.native_handle(), receive_buffer(),
              data_or_ignored(receive_counts_i_, ignored_int_),
              data_or_ignored(receive_offsets_i_, ignored_int_),
              datatype_.native_handle(), communicator_.native_handle(),
              MPI_INFO_NULL, &request_),
          communicator_.native_handle(),
          "MPI_Neighbor_alltoallv_init(persistent neighborhood exchange)");
      return;
#endif
    }
#if KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_INIT_C
    if (backend_ == neighbor_direct_backend::persistent_large_count) {
      check_or_abort(
          MPI_Neighbor_alltoallv_init_c(
              send_buffer(), data_or_ignored(send_counts_c_, ignored_count_),
              data_or_ignored(send_offsets_c_, ignored_aint_),
              datatype_.native_handle(), receive_buffer(),
              data_or_ignored(receive_counts_c_, ignored_count_),
              data_or_ignored(receive_offsets_c_, ignored_aint_),
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
    check_or_abort(MPI_Wait(&request_, MPI_STATUS_IGNORE),
                   communicator_.native_handle(),
                   "MPI_Wait(neighborhood exchange)");
    active_ = false;
    receive_ready_ = true;
  }

  void complete_active_for_destruction() noexcept {
    require_active_runtime("neighborhood operation destruction");
    if (active_) {
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

  void build_mpi_layout() {
    if (uses_large_count(backend_)) {
#if KAHIP_HAVE_MPI_INEIGHBOR_ALLTOALLV_C || \
    KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_INIT_C
      send_counts_c_.reserve(sends_.segment_count());
      send_offsets_c_.reserve(sends_.segment_count());
      receive_counts_c_.reserve(received_.segment_count());
      receive_offsets_c_.reserve(received_.segment_count());
      for (std::size_t index = 0; index < sends_.segment_count(); ++index) {
        send_counts_c_.push_back(
            checked_mpi_count(sends_.counts()[index], "neighbor send count"));
        send_offsets_c_.push_back(
            checked_mpi_aint(sends_.offsets()[index], "neighbor send offset"));
      }
      for (std::size_t index = 0; index < received_.segment_count(); ++index) {
        receive_counts_c_.push_back(checked_mpi_count(
            received_.counts()[index], "neighbor receive count"));
        receive_offsets_c_.push_back(checked_mpi_aint(
            received_.offsets()[index], "neighbor receive offset"));
      }
      return;
#endif
    }

    send_counts_i_.reserve(sends_.segment_count());
    send_offsets_i_.reserve(sends_.segment_count());
    receive_counts_i_.reserve(received_.segment_count());
    receive_offsets_i_.reserve(received_.segment_count());
    for (std::size_t index = 0; index < sends_.segment_count(); ++index) {
      send_counts_i_.push_back(
          checked_int(sends_.counts()[index], "neighbor send count"));
      send_offsets_i_.push_back(
          checked_int(sends_.offsets()[index], "neighbor send offset"));
    }
    for (std::size_t index = 0; index < received_.segment_count(); ++index) {
      receive_counts_i_.push_back(
          checked_int(received_.counts()[index], "neighbor receive count"));
      receive_offsets_i_.push_back(
          checked_int(received_.offsets()[index], "neighbor receive offset"));
    }
  }

  communicator communicator_;
  datatype datatype_;
  segmented_buffer<T> sends_;
  segmented_buffer<T> received_;
  std::vector<int> sources_;
  std::vector<int> destinations_;
  neighbor_direct_backend backend_;
  std::vector<int> send_counts_i_;
  std::vector<int> send_offsets_i_;
  std::vector<int> receive_counts_i_;
  std::vector<int> receive_offsets_i_;
  std::vector<MPI_Count> send_counts_c_;
  std::vector<MPI_Aint> send_offsets_c_;
  std::vector<MPI_Count> receive_counts_c_;
  std::vector<MPI_Aint> receive_offsets_c_;
  MPI_Request request_ = MPI_REQUEST_NULL;
  bool active_ = false;
  bool receive_ready_ = false;
  int ignored_int_ = 0;
  MPI_Count ignored_count_ = 0;
  MPI_Aint ignored_aint_ = 0;
  std::byte ignored_send_byte_{};
  std::byte ignored_receive_byte_{};
};

[[nodiscard]] inline auto choose_direct_backend(persistence_policy policy,
                                                bool legacy_representable,
                                                bool large_count_representable,
                                                bool force_mpi3)
    -> std::optional<neighbor_direct_backend> {
  auto const can_immediate_large = !force_mpi3 &&
                                   KAHIP_HAVE_MPI_INEIGHBOR_ALLTOALLV_C &&
                                   large_count_representable;
  auto const can_immediate_legacy =
      KAHIP_HAVE_MPI_INEIGHBOR_ALLTOALLV && legacy_representable;
  auto const can_persistent_large = !force_mpi3 &&
                                    KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_INIT_C &&
                                    large_count_representable;
  auto const can_persistent_legacy = !force_mpi3 &&
                                     KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_INIT &&
                                     legacy_representable;

  if (policy != persistence_policy::disabled) {
    if (can_persistent_large) {
      return neighbor_direct_backend::persistent_large_count;
    }
    if (can_persistent_legacy) {
      return neighbor_direct_backend::persistent_legacy;
    }
    if (policy == persistence_policy::required) {
      return std::nullopt;
    }
  }
  if (can_immediate_large) {
    return neighbor_direct_backend::immediate_large_count;
  }
  if (can_immediate_legacy) {
    return neighbor_direct_backend::immediate_legacy;
  }
  return std::nullopt;
}

template <typename T>
[[nodiscard]] auto prepare_direct_neighbor_storage(
    segmented_buffer<T> sends,
    distributed_graph const& graph,
    collective_options collective,
    persistence_policy persistence)
    -> std::unique_ptr<direct_neighbor_storage<T>> {
  auto semantic_failure = std::string_view{};
  auto deferred_capacity_failure = std::string_view{};
  auto result = std::unique_ptr<direct_neighbor_storage<T>>{};
  {
    auto operation_communicator = communicator{graph.view()};
    auto const collective_communicator = operation_communicator.view();
    try {
      auto const layout_is_valid = collective_predicate(
          sends.has_canonical_layout(graph.destinations().size()),
          collective_communicator);
      auto const mpi3_ceiling =
          validate_collective_options(collective, collective_communicator);
      auto const agreed_persistence =
          validate_persistence_policy(persistence, collective_communicator);
      if (!layout_is_valid) {
        semantic_failure =
            "direct neighborhood exchange input validation failed";
      } else if (!mpi3_ceiling.has_value() || !agreed_persistence.has_value()) {
        semantic_failure =
            "direct neighborhood exchange options must agree collectively";
      } else {
        auto receive_count_exchange = exchange_neighbor_counts(
            sends.counts(), graph.sources().size(), collective_communicator);
        auto receive_offsets =
            receive_count_exchange.representable
                ? neighbor_offsets(receive_count_exchange.counts)
                : std::nullopt;
        auto receive_storage_size = std::optional<std::size_t>{};
        if (receive_offsets.has_value()) {
          auto const element_count = neighbor_storage_size(
              receive_count_exchange.counts, *receive_offsets);
          if (element_count <=
              std::numeric_limits<std::size_t>::max() / sizeof(T)) {
            receive_storage_size = element_count;
          }
        }
        auto const receive_layout_is_valid = collective_predicate(
            receive_count_exchange.representable &&
                receive_offsets.has_value() && receive_storage_size.has_value(),
            collective_communicator);
        if (!receive_layout_is_valid) {
          deferred_capacity_failure =
              "direct neighborhood exchange receive layout validation failed";
        } else {
          auto received = segmented_buffer<T>::uninitialized(
              *receive_storage_size, std::move(receive_count_exchange.counts),
              std::move(*receive_offsets));
          auto const legacy_representable = !neighbor_needs_bounded_rounds(
              sends, received.counts(), received.offsets(), *mpi3_ceiling,
              collective_communicator);
          auto large_count_representable = false;
#if KAHIP_HAVE_MPI_INEIGHBOR_ALLTOALLV_C || \
    KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_INIT_C
          large_count_representable = neighbor_mpi4_layout_is_representable(
              sends, received, collective_communicator);
#endif
          auto const backend = choose_direct_backend(
              *agreed_persistence, legacy_representable,
              large_count_representable, collective.force_mpi3);
          if (!backend.has_value()) {
            constexpr auto persistent_backend_is_available =
                KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_INIT != 0 ||
                KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_INIT_C != 0;
            constexpr auto immediate_backend_is_available =
                KAHIP_HAVE_MPI_INEIGHBOR_ALLTOALLV != 0 ||
                KAHIP_HAVE_MPI_INEIGHBOR_ALLTOALLV_C != 0;
            auto const required_persistence_is_unavailable =
                *agreed_persistence == persistence_policy::required &&
                (collective.force_mpi3 || !persistent_backend_is_available);
            if (required_persistence_is_unavailable) {
              semantic_failure =
                  "persistent neighborhood exchange is unavailable";
            } else if (!immediate_backend_is_available &&
                       (*agreed_persistence == persistence_policy::disabled ||
                        !persistent_backend_is_available)) {
              semantic_failure = "direct neighborhood exchange is unavailable";
            } else {
              auto const physical_legacy_layout_is_representable =
                  !neighbor_needs_bounded_rounds(
                      sends, received.counts(), received.offsets(),
                      static_cast<std::size_t>(std::numeric_limits<int>::max()),
                      collective_communicator);
              auto const synthetic_ceiling_rejected_layout =
                  !legacy_representable &&
                  physical_legacy_layout_is_representable;
              if (synthetic_ceiling_rejected_layout) {
                semantic_failure =
                    *agreed_persistence == persistence_policy::required
                        ? "persistent neighborhood exchange requires a "
                          "single representable payload"
                        : "direct neighborhood exchange requires a single "
                          "representable payload";
              } else {
                deferred_capacity_failure =
                    *agreed_persistence == persistence_policy::required
                        ? "persistent neighborhood exchange is unavailable or "
                          "unrepresentable"
                        : "direct neighborhood exchange requires a single "
                          "representable payload";
              }
            }
          } else {
            auto operation_datatype =
                make_mpi_datatype<T>(collective_communicator.native_handle());
            auto sources = std::vector<int>(graph.sources().begin(),
                                            graph.sources().end());
            auto destinations = std::vector<int>(graph.destinations().begin(),
                                                 graph.destinations().end());
            result = std::make_unique<direct_neighbor_storage<T>>(
                std::move(operation_communicator),
                std::move(operation_datatype), std::move(sends),
                std::move(received), std::move(sources),
                std::move(destinations), *backend);
          }
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
  if (!deferred_capacity_failure.empty()) {
    throw mpi_error{MPI_ERR_ARG, std::string{deferred_capacity_failure}};
  }
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
      std::move(sends), graph, options, persistence_policy::disabled);
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
            detail::make_fixed_neighbor_sends<T>(graph, std::move(send_counts)),
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
