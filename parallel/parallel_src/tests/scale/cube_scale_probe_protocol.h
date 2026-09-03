#pragma once

#include <mpi.h>

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ranges>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "communication/mpi_adapter.h"
#include "communication/mpi_fixed_reduction.h"
#include "range_owner.h"
#include "scale/cube_scale_probe_core.h"

namespace parhip::scale_probe {
struct remote_label_request final {
  std::uint64_t source;
  std::uint64_t target;

  auto operator==(remote_label_request const&) const -> bool = default;
};

struct remote_label_reply final {
  std::uint64_t source;
  std::uint64_t target;
  std::uint64_t label;

  auto operator==(remote_label_reply const&) const -> bool = default;
};

static_assert(std::is_standard_layout_v<remote_label_request>);
static_assert(std::is_trivially_copyable_v<remote_label_request>);
static_assert(sizeof(remote_label_request) == 2 * sizeof(std::uint64_t));
static_assert(std::is_standard_layout_v<remote_label_reply>);
static_assert(std::is_trivially_copyable_v<remote_label_reply>);
static_assert(sizeof(remote_label_reply) == 3 * sizeof(std::uint64_t));

struct request_validation_context final {
  std::uint64_t side;
  std::span<std::uint64_t const> boundaries;
  int sender;
  int receiver;
  std::uint64_t window_first;
  std::uint64_t window_end;
};

namespace detail {
[[nodiscard]] inline auto boundaries_are_canonical(
    std::span<std::uint64_t const> boundaries,
    std::uint64_t vertices) noexcept -> bool {
  if (boundaries.size() < 2 || boundaries.front() != 0 ||
      boundaries.back() != vertices ||
      boundaries.size() - 1 >
          static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
    return false;
  }
  auto const parts = static_cast<std::uint32_t>(boundaries.size() - 1);
  for (auto index = std::uint32_t{0};; ++index) {
    auto const expected = balanced_boundary(vertices, index, parts);
    if (!expected.has_value() || boundaries[index] != *expected) {
      return false;
    }
    if (index == parts) {
      break;
    }
  }
  return true;
}

[[nodiscard]] constexpr auto key_is_before(remote_label_request left,
                                           remote_label_request right) noexcept
    -> bool {
  return left.source < right.source ||
         (left.source == right.source && left.target < right.target);
}

[[nodiscard]] inline auto is_cube_neighbor(std::uint64_t side,
                                           std::uint64_t source,
                                           std::uint64_t target) noexcept
    -> bool {
  auto const adjacent = neighbors_for_vertex(side, source);
  return adjacent.has_value() &&
         std::ranges::binary_search(adjacent->span(), target);
}
}  // namespace detail

template <std::ranges::forward_range Requests>
  requires std::same_as<std::remove_cv_t<std::ranges::range_value_t<Requests>>,
                        remote_label_request>
[[nodiscard]] auto request_segment_is_valid(
    Requests const& requests,
    request_validation_context const& context) noexcept -> bool {
  auto const counts = counts_for_side(context.side);
  if (!counts.has_value() ||
      !detail::boundaries_are_canonical(context.boundaries, counts->vertices)) {
    return false;
  }
  auto const process_count = context.boundaries.size() - 1;
  if (context.sender < 0 || context.receiver < 0 ||
      static_cast<std::size_t>(context.sender) >= process_count ||
      static_cast<std::size_t>(context.receiver) >= process_count ||
      context.sender == context.receiver) {
    return false;
  }
  auto const sender = static_cast<std::size_t>(context.sender);
  if (context.window_first < context.boundaries[sender] ||
      context.window_first > context.window_end ||
      context.window_end > context.boundaries[sender + 1]) {
    return false;
  }

  auto previous = remote_label_request{};
  auto has_previous = false;
  for (auto const& request : requests) {
    if (request.source < context.window_first ||
        request.source >= context.window_end ||
        request.source >= request.target ||
        kahip::range_owner::from_boundaries(context.boundaries,
                                            request.source) != context.sender ||
        kahip::range_owner::from_boundaries(
            context.boundaries, request.target) != context.receiver ||
        !detail::is_cube_neighbor(context.side, request.source,
                                  request.target) ||
        (has_previous && !detail::key_is_before(previous, request))) {
      return false;
    }
    previous = request;
    has_previous = true;
  }
  return true;
}

template <std::ranges::forward_range Requests,
          std::ranges::forward_range Replies>
  requires std::same_as<std::remove_cv_t<std::ranges::range_value_t<Requests>>,
                        remote_label_request> &&
           std::same_as<std::remove_cv_t<std::ranges::range_value_t<Replies>>,
                        remote_label_reply>
[[nodiscard]] auto reply_segment_is_valid(Requests const& requests,
                                          Replies const& replies,
                                          std::uint64_t blocks) noexcept
    -> bool {
  if (blocks == 0 ||
      std::ranges::distance(requests) != std::ranges::distance(replies)) {
    return false;
  }
  auto request = std::ranges::begin(requests);
  auto reply = std::ranges::begin(replies);
  for (; request != std::ranges::end(requests); ++request, ++reply) {
    if (request->source != reply->source || request->target != reply->target ||
        reply->label >= blocks) {
      return false;
    }
  }
  return true;
}

enum class capacity_reason : std::uint8_t {
  none,
  zero_window,
  count_overflow,
  int_count,
  size_count,
  mpi_aint_offset,
  request_vector,
  reply_vector,
  metadata_vector,
  boundary_vector,
  request_bytes,
  reply_bytes,
  metadata_bytes,
};

struct protocol_capacity_limits final {
  std::uint64_t int_max = std::numeric_limits<int>::max();
  std::uint64_t size_max = std::numeric_limits<std::size_t>::max();
  std::uint64_t mpi_aint_max =
      static_cast<std::uint64_t>(std::numeric_limits<MPI_Aint>::max());
  std::uint64_t request_elements =
      std::vector<remote_label_request>{}.max_size();
  std::uint64_t reply_elements = std::vector<remote_label_reply>{}.max_size();
  std::uint64_t metadata_elements = std::min(
      {std::vector<std::size_t>{}.max_size(),
       std::vector<std::uint64_t>{}.max_size(), std::vector<int>{}.max_size(),
       std::vector<MPI_Count>{}.max_size(), std::vector<MPI_Aint>{}.max_size(),
       std::vector<std::vector<remote_label_request>>{}.max_size(),
       std::vector<std::vector<remote_label_reply>>{}.max_size()});
  std::uint64_t boundary_elements = std::vector<std::uint64_t>{}.max_size();
  std::uint64_t request_bytes = std::numeric_limits<std::size_t>::max();
  std::uint64_t reply_bytes = std::numeric_limits<std::size_t>::max();
  std::uint64_t metadata_bytes = std::numeric_limits<std::size_t>::max();
};

struct protocol_capacity_result final {
  std::uint64_t maximum_send{};
  std::uint64_t maximum_receive{};
  capacity_reason reason = capacity_reason::none;

  [[nodiscard]] constexpr auto safe() const noexcept -> bool {
    return reason == capacity_reason::none;
  }
};

[[nodiscard]] inline auto protocol_capacity(
    std::uint64_t source_window,
    std::uint64_t local_nodes,
    std::uint64_t processes,
    protocol_capacity_limits const& limits = {}) noexcept
    -> protocol_capacity_result {
  auto result = protocol_capacity_result{};
  if (source_window == 0 || processes == 0) {
    result.reason = capacity_reason::zero_window;
    return result;
  }
  auto const maximum_send = detail::checked_multiply(source_window, 3);
  auto const maximum_receive = detail::checked_multiply(local_nodes, 3);
  if (!maximum_send.has_value() || !maximum_receive.has_value()) {
    result.reason = capacity_reason::count_overflow;
    return result;
  }
  result.maximum_send = *maximum_send;
  result.maximum_receive = *maximum_receive;
  auto const maximum_count = std::max(*maximum_send, *maximum_receive);
  auto const boundary_count = detail::checked_add(processes, 1);
  if (!boundary_count.has_value()) {
    result.reason = capacity_reason::count_overflow;
    return result;
  }
  if (maximum_count > limits.int_max || processes > limits.int_max) {
    result.reason = capacity_reason::int_count;
    return result;
  }
  if (maximum_count > limits.size_max || *boundary_count > limits.size_max) {
    result.reason = capacity_reason::size_count;
    return result;
  }
  if (maximum_count > limits.mpi_aint_max) {
    result.reason = capacity_reason::mpi_aint_offset;
    return result;
  }
  if (maximum_count > limits.request_elements) {
    result.reason = capacity_reason::request_vector;
    return result;
  }
  if (maximum_count > limits.reply_elements) {
    result.reason = capacity_reason::reply_vector;
    return result;
  }
  if (processes > limits.metadata_elements) {
    result.reason = capacity_reason::metadata_vector;
    return result;
  }
  if (*boundary_count > limits.boundary_elements) {
    result.reason = capacity_reason::boundary_vector;
    return result;
  }
  auto const request_byte_count =
      detail::checked_multiply(maximum_count, sizeof(remote_label_request));
  if (!request_byte_count.has_value() ||
      *request_byte_count > limits.request_bytes) {
    result.reason = capacity_reason::request_bytes;
    return result;
  }
  auto const reply_byte_count =
      detail::checked_multiply(maximum_count, sizeof(remote_label_reply));
  if (!reply_byte_count.has_value() || *reply_byte_count > limits.reply_bytes) {
    result.reason = capacity_reason::reply_bytes;
    return result;
  }
  auto const boundary_bytes =
      detail::checked_multiply(*boundary_count, sizeof(std::uint64_t));
  auto metadata_byte_count =
      boundary_bytes.value_or(std::numeric_limits<std::uint64_t>::max());
  auto metadata_bytes_are_safe = boundary_bytes.has_value();
  for (auto element_bytes : std::array<std::uint64_t, 7>{
           sizeof(std::size_t), sizeof(std::uint64_t), sizeof(int),
           sizeof(MPI_Count), sizeof(MPI_Aint),
           sizeof(std::vector<remote_label_request>),
           sizeof(std::vector<remote_label_reply>)}) {
    auto const byte_count = detail::checked_multiply(processes, element_bytes);
    metadata_bytes_are_safe = metadata_bytes_are_safe && byte_count.has_value();
    if (byte_count.has_value()) {
      metadata_byte_count = std::max(metadata_byte_count, *byte_count);
    }
  }
  if (!metadata_bytes_are_safe || metadata_byte_count > limits.metadata_bytes) {
    result.reason = capacity_reason::metadata_bytes;
    return result;
  }
  return result;
}

struct cut_exchange_options final {
  std::uint64_t source_window = 65'536;
  std::size_t mpi3_round_ceiling =
      static_cast<std::size_t>(std::numeric_limits<int>::max());
  bool force_mpi3 = false;
};

struct cut_exchange_result final {
  std::uint64_t cut{};
  std::uint64_t rounds{};
  std::uint64_t maximum_send{};
  std::uint64_t maximum_receive{};
};

namespace detail {
[[nodiscard]] inline auto local_graph_is_exact(
    local_cube_csr const& graph,
    std::uint64_t side,
    std::uint64_t expected_first,
    std::uint64_t expected_end) noexcept -> bool {
  auto const local_nodes = expected_end - expected_first;
  if (graph.first_vertex != expected_first ||
      graph.vertex_end != expected_end ||
      !std::in_range<std::size_t>(local_nodes) || graph.offsets.empty() ||
      graph.offsets.size() != static_cast<std::size_t>(local_nodes) + 1 ||
      graph.offsets.front() != 0 ||
      graph.offsets.back() != graph.targets.size()) {
    return false;
  }
  for (auto local = std::size_t{0}; local < local_nodes; ++local) {
    auto const first = graph.offsets[local];
    auto const end = graph.offsets[local + 1];
    if (first > end || end > graph.targets.size()) {
      return false;
    }
    auto const expected = neighbors_for_vertex(side, expected_first + local);
    if (!expected.has_value() || end - first != expected->count) {
      return false;
    }
    for (auto ordinal = std::size_t{0}; ordinal < expected->count; ++ordinal) {
      if (graph.targets[static_cast<std::size_t>(first) + ordinal] !=
          expected->values[ordinal]) {
        return false;
      }
    }
  }
  return true;
}

template <std::integral Label>
[[nodiscard]] inline auto partition_is_valid(std::span<Label const> labels,
                                             std::uint64_t expected_size,
                                             std::uint64_t blocks) noexcept
    -> bool {
  if (!std::in_range<std::size_t>(expected_size) ||
      labels.size() != static_cast<std::size_t>(expected_size) || blocks == 0) {
    return false;
  }
  return std::ranges::all_of(labels, [blocks](Label label) {
    if constexpr (std::signed_integral<Label>) {
      if (label < 0) {
        return false;
      }
    }
    return static_cast<std::uint64_t>(label) < blocks;
  });
}

[[nodiscard]] constexpr auto window_for_round(std::uint64_t first,
                                              std::uint64_t end,
                                              std::uint64_t window,
                                              std::uint64_t round) noexcept
    -> std::pair<std::uint64_t, std::uint64_t> {
  auto const local_nodes = end - first;
  if (round > local_nodes / window) {
    return {end, end};
  }
  auto const offset = round * window;
  if (offset >= local_nodes) {
    return {end, end};
  }
  auto const window_first = first + offset;
  auto const remaining = end - window_first;
  return {window_first, window_first + std::min(window, remaining)};
}

[[nodiscard]] inline auto expected_receive_counts(
    std::uint64_t side,
    std::span<std::uint64_t const> boundaries,
    int rank,
    std::uint64_t round,
    std::uint64_t window) -> std::vector<std::size_t> {
  auto result = std::vector<std::size_t>(boundaries.size() - 1, 0);
  auto const local_first = boundaries[static_cast<std::size_t>(rank)];
  auto const local_end = boundaries[static_cast<std::size_t>(rank) + 1];
  for (auto target = local_first; target < local_end; ++target) {
    auto const adjacent = *neighbors_for_vertex(side, target);
    for (auto source : adjacent.span()) {
      if (source >= target) {
        continue;
      }
      auto const sender =
          kahip::range_owner::from_boundaries(boundaries, source);
      if (sender < 0 || sender == rank) {
        continue;
      }
      auto const sender_index = static_cast<std::size_t>(sender);
      auto const [window_first, window_end] =
          window_for_round(boundaries[sender_index],
                           boundaries[sender_index + 1], window, round);
      if (source >= window_first && source < window_end) {
        ++result[sender_index];
      }
    }
  }
  return result;
}

[[nodiscard]] inline auto sum_counts(
    std::span<std::size_t const> counts) noexcept
    -> std::optional<std::uint64_t> {
  auto total = std::uint64_t{0};
  for (auto count : counts) {
    if (!std::in_range<std::uint64_t>(count)) {
      return std::nullopt;
    }
    auto const next = checked_add(total, static_cast<std::uint64_t>(count));
    if (!next.has_value()) {
      return std::nullopt;
    }
    total = *next;
  }
  return total;
}
}  // namespace detail
}  // namespace parhip::scale_probe

namespace parhip::mpi {
template <>
struct wire_members<scale_probe::remote_label_request> {
  inline static constexpr auto value =
      std::tuple{&scale_probe::remote_label_request::source,
                 &scale_probe::remote_label_request::target};
};

template <>
struct wire_members<scale_probe::remote_label_reply> {
  inline static constexpr auto value =
      std::tuple{&scale_probe::remote_label_reply::source,
                 &scale_probe::remote_label_reply::target,
                 &scale_probe::remote_label_reply::label};
};
}  // namespace parhip::mpi

namespace parhip::scale_probe {
template <std::integral Label>
[[nodiscard]] auto independent_cut(std::uint64_t side,
                                   local_cube_csr const& graph,
                                   std::span<Label const> partition,
                                   std::span<std::uint64_t const> boundaries,
                                   std::uint64_t blocks,
                                   MPI_Comm communicator,
                                   cut_exchange_options options = {})
    -> cut_exchange_result {
  auto const collective = mpi::communicator_view{communicator};
  mpi::require_live_intracommunicator(
      collective, "cube cut exchange requires a live intracommunicator");
  auto const rank = collective.rank();
  auto const size = collective.size();
  auto const boundary_shape_is_valid =
      size > 0 &&
      boundaries.size() == static_cast<std::size_t>(size) + std::size_t{1};
  mpi::validate_collectively(
      boundary_shape_is_valid, collective,
      "cube cut exchange boundary shape differs from communicator");

  static_assert(sizeof(std::size_t) <= sizeof(std::uint64_t));
  auto const signature = std::array<std::uint64_t, 5>{
      side,
      blocks,
      options.source_window,
      static_cast<std::uint64_t>(options.mpi3_round_ceiling),
      options.force_mpi3 ? std::uint64_t{1} : std::uint64_t{0},
  };
  auto minimum_signature = std::array<std::uint64_t, signature.size()>{};
  auto maximum_signature = std::array<std::uint64_t, signature.size()>{};
  mpi::check_or_abort(MPI_Allreduce(signature.data(), minimum_signature.data(),
                                    static_cast<int>(signature.size()),
                                    MPI_UINT64_T, MPI_MIN, communicator),
                      communicator,
                      "MPI_Allreduce(cube cut signature minimum)");
  mpi::check_or_abort(MPI_Allreduce(signature.data(), maximum_signature.data(),
                                    static_cast<int>(signature.size()),
                                    MPI_UINT64_T, MPI_MAX, communicator),
                      communicator,
                      "MPI_Allreduce(cube cut signature maximum)");

  constexpr auto agreement_chunk = std::size_t{256};
  auto root_boundaries = std::array<std::uint64_t, agreement_chunk>{};
  auto boundaries_agree = true;
  for (auto offset = std::size_t{0}; offset < boundaries.size();
       offset += agreement_chunk) {
    auto const count = std::min(agreement_chunk, boundaries.size() - offset);
    if (rank == 0) {
      std::ranges::copy(boundaries.subspan(offset, count),
                        root_boundaries.begin());
    }
    mpi::check_or_abort(
        MPI_Bcast(root_boundaries.data(), static_cast<int>(count), MPI_UINT64_T,
                  0, communicator),
        communicator, "MPI_Bcast(cube cut canonical boundaries)");
    boundaries_agree =
        boundaries_agree &&
        std::ranges::equal(boundaries.subspan(offset, count),
                           std::span{root_boundaries}.first(count));
  }
  mpi::validate_collectively(
      minimum_signature == maximum_signature && boundaries_agree, collective,
      "cube cut exchange semantic inputs differ across communicator");

  auto const recovered_counts = counts_for_side(side);

  auto const local_first =
      rank >= 0 && rank < size &&
              boundaries.size() == static_cast<std::size_t>(size) + 1
          ? boundaries[static_cast<std::size_t>(rank)]
          : std::uint64_t{0};
  auto const local_end =
      rank >= 0 && rank < size &&
              boundaries.size() == static_cast<std::size_t>(size) + 1
          ? boundaries[static_cast<std::size_t>(rank) + 1]
          : std::uint64_t{0};
  auto const local_nodes = local_end - local_first;
  auto const capacity = protocol_capacity(
      options.source_window, local_nodes,
      size > 0 ? static_cast<std::uint64_t>(size) : std::uint64_t{0});
  auto const local_input_is_valid =
      recovered_counts.has_value() && rank >= 0 && rank < size &&
      boundaries.size() == static_cast<std::size_t>(size) + 1 &&
      detail::boundaries_are_canonical(boundaries,
                                       recovered_counts->vertices) &&
      detail::local_graph_is_exact(graph, side, local_first, local_end) &&
      detail::partition_is_valid(partition, local_nodes, blocks) &&
      capacity.safe() && options.mpi3_round_ceiling != 0;
  mpi::validate_collectively(local_input_is_valid, collective,
                             "cube cut exchange input validation failed");

  auto const maximum_local_nodes = *maximum_balanced_slice(
      recovered_counts->vertices, static_cast<std::uint32_t>(size));
  auto const rounds =
      maximum_local_nodes == 0
          ? std::uint64_t{0}
          : (maximum_local_nodes - 1) / options.source_window + 1;
  auto local_cut = std::uint64_t{0};
  auto local_maximum_send = std::uint64_t{0};
  auto local_maximum_receive = std::uint64_t{0};
  auto const mpi_options = mpi::collective_options{
      .mpi3_round_ceiling = options.mpi3_round_ceiling,
      .force_mpi3 = options.force_mpi3,
  };

  for (auto round = std::uint64_t{0}; round < rounds; ++round) {
    auto const [window_first, window_end] = detail::window_for_round(
        local_first, local_end, options.source_window, round);
    auto request_segments = std::vector<std::vector<remote_label_request>>(
        static_cast<std::size_t>(size));
    auto local_round_is_valid = true;
    for (auto source = window_first; source < window_end; ++source) {
      auto const local_source = static_cast<std::size_t>(source - local_first);
      auto const edge_first =
          static_cast<std::size_t>(graph.offsets[local_source]);
      auto const edge_end =
          static_cast<std::size_t>(graph.offsets[local_source + 1]);
      for (auto edge = edge_first; edge < edge_end; ++edge) {
        auto const target = static_cast<std::uint64_t>(graph.targets[edge]);
        if (target <= source) {
          continue;
        }
        auto const owner =
            kahip::range_owner::from_boundaries(boundaries, target);
        if (owner < 0 || owner >= size) {
          local_round_is_valid = false;
          continue;
        }
        if (owner == rank) {
          auto const target_label = static_cast<std::uint64_t>(
              partition[static_cast<std::size_t>(target - local_first)]);
          auto const source_label =
              static_cast<std::uint64_t>(partition[local_source]);
          if (source_label != target_label) {
            auto const next = detail::checked_add(local_cut, 1);
            local_round_is_valid = local_round_is_valid && next.has_value();
            if (next.has_value()) {
              local_cut = *next;
            }
          }
        } else {
          request_segments[static_cast<std::size_t>(owner)].push_back(
              remote_label_request{.source = source, .target = target});
        }
      }
    }

    auto send_counts = std::vector<std::size_t>(request_segments.size());
    for (auto receiver = std::size_t{0}; receiver < request_segments.size();
         ++receiver) {
      send_counts[receiver] = request_segments[receiver].size();
      if (receiver == static_cast<std::size_t>(rank)) {
        local_round_is_valid =
            local_round_is_valid && request_segments[receiver].empty();
      } else {
        local_round_is_valid =
            local_round_is_valid &&
            request_segment_is_valid(request_segments[receiver],
                                     request_validation_context{
                                         .side = side,
                                         .boundaries = boundaries,
                                         .sender = rank,
                                         .receiver = static_cast<int>(receiver),
                                         .window_first = window_first,
                                         .window_end = window_end,
                                     });
      }
    }
    auto const send_total = detail::sum_counts(send_counts);
    auto const expected_receive = detail::expected_receive_counts(
        side, boundaries, rank, round, options.source_window);
    auto const receive_total = detail::sum_counts(expected_receive);
    local_round_is_valid =
        local_round_is_valid && send_total.has_value() &&
        receive_total.has_value() && *send_total <= capacity.maximum_send &&
        *receive_total <= capacity.maximum_receive &&
        expected_receive[static_cast<std::size_t>(rank)] == 0;
    mpi::validate_collectively(
        local_round_is_valid, collective,
        "cube cut request pre-payload validation failed");

    auto received_requests = mpi::all_to_all_v(
        mpi::segmented_buffer<remote_label_request>::from_segments(
            request_segments),
        collective, mpi_options);
    local_round_is_valid =
        received_requests.counts() == expected_receive &&
        received_requests.segment(static_cast<std::size_t>(rank)).empty();
    for (auto sender = std::size_t{0}; sender < expected_receive.size();
         ++sender) {
      if (sender == static_cast<std::size_t>(rank)) {
        continue;
      }
      auto const [source_first, source_end] =
          detail::window_for_round(boundaries[sender], boundaries[sender + 1],
                                   options.source_window, round);
      local_round_is_valid =
          local_round_is_valid &&
          request_segment_is_valid(received_requests.segment(sender),
                                   request_validation_context{
                                       .side = side,
                                       .boundaries = boundaries,
                                       .sender = static_cast<int>(sender),
                                       .receiver = rank,
                                       .window_first = source_first,
                                       .window_end = source_end,
                                   });
    }
    mpi::validate_collectively(local_round_is_valid, collective,
                               "cube cut request payload validation failed");

    auto reply_segments = std::vector<std::vector<remote_label_reply>>(
        static_cast<std::size_t>(size));
    for (auto sender = std::size_t{0}; sender < expected_receive.size();
         ++sender) {
      auto const requests = received_requests.segment(sender);
      auto& replies = reply_segments[sender];
      replies.reserve(requests.size());
      for (auto const& request : requests) {
        replies.push_back(remote_label_reply{
            .source = request.source,
            .target = request.target,
            .label =
                static_cast<std::uint64_t>(partition[static_cast<std::size_t>(
                    request.target - local_first)]),
        });
      }
    }
    auto const reply_send_total =
        detail::sum_counts(std::span<std::size_t const>{expected_receive});
    local_round_is_valid =
        reply_send_total.has_value() &&
        *reply_send_total <= capacity.maximum_receive &&
        reply_segments[static_cast<std::size_t>(rank)].empty();
    mpi::validate_collectively(local_round_is_valid, collective,
                               "cube cut reply pre-payload validation failed");

    auto received_replies = mpi::all_to_all_v(
        mpi::segmented_buffer<remote_label_reply>::from_segments(
            reply_segments),
        collective, mpi_options);
    local_round_is_valid =
        received_replies.counts() == send_counts &&
        received_replies.segment(static_cast<std::size_t>(rank)).empty();
    for (auto receiver = std::size_t{0}; receiver < request_segments.size();
         ++receiver) {
      if (receiver == static_cast<std::size_t>(rank)) {
        continue;
      }
      local_round_is_valid =
          local_round_is_valid &&
          reply_segment_is_valid(request_segments[receiver],
                                 received_replies.segment(receiver), blocks);
    }
    mpi::validate_collectively(local_round_is_valid, collective,
                               "cube cut reply payload validation failed");

    auto remote_cut_is_exact = true;
    for (auto receiver = std::size_t{0}; receiver < request_segments.size();
         ++receiver) {
      for (auto const& reply : received_replies.segment(receiver)) {
        auto const source_label = static_cast<std::uint64_t>(
            partition[static_cast<std::size_t>(reply.source - local_first)]);
        if (source_label != reply.label) {
          auto const next = detail::checked_add(local_cut, 1);
          remote_cut_is_exact = remote_cut_is_exact && next.has_value();
          if (next.has_value()) {
            local_cut = *next;
          }
        }
      }
    }
    mpi::validate_collectively(remote_cut_is_exact, collective,
                               "cube cut accumulation overflowed uint64");
    local_maximum_send = std::max(local_maximum_send, *send_total);
    local_maximum_receive = std::max(local_maximum_receive, *receive_total);
  }

  auto local_values = std::array<std::uint64_t, 1>{local_cut};
  auto global_values = std::array<std::uint64_t, 1>{};
  mpi::all_reduce_checked_sum(std::span<std::uint64_t const>{local_values},
                              std::span<std::uint64_t>{global_values},
                              collective, "MPI_Allreduce(cube independent cut)",
                              "cube independent cut",
                              "cube independent cut exceeds uint64");

  auto local_maxima =
      std::array<std::uint64_t, 2>{local_maximum_send, local_maximum_receive};
  auto global_maxima = std::array<std::uint64_t, 2>{};
  mpi::all_reduce_bounded(std::span<std::uint64_t const>{local_maxima},
                          std::span<std::uint64_t>{global_maxima},
                          mpi::reduction_kind::maximum, collective,
                          "MPI_Allreduce(cube cut protocol maxima)");
  return cut_exchange_result{
      .cut = global_values[0],
      .rounds = rounds,
      .maximum_send = global_maxima[0],
      .maximum_receive = global_maxima[1],
  };
}

template <std::integral Label>
[[nodiscard]] auto independent_cut(std::uint64_t side,
                                   local_cube_csr const& graph,
                                   std::vector<Label> const& partition,
                                   std::vector<std::uint64_t> const& boundaries,
                                   std::uint64_t blocks,
                                   MPI_Comm communicator,
                                   cut_exchange_options options = {})
    -> cut_exchange_result {
  return independent_cut(side, graph, std::span<Label const>{partition},
                         std::span<std::uint64_t const>{boundaries}, blocks,
                         communicator, options);
}
}  // namespace parhip::scale_probe
