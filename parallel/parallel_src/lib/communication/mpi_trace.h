#pragma once

#include <mpi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "communication/mpi_error.h"
#include "communication/mpi_handles.h"

#ifndef KAHIP_ENABLE_MPI_TRACE
#define KAHIP_ENABLE_MPI_TRACE 0
#endif

namespace parhip::mpi::trace {
inline constexpr std::string_view format_version = "kahip-mpi-trace-v3";
inline constexpr std::string_view upstream_revision =
    "5935f349f65f1788a9b68fcf6d853e698d86956d";

enum class stage : std::uint8_t {
  graph_distribution_node,
  graph_distribution_edge,
  contraction_label,
  quotient_node_weight,
  quotient_edge,
  projection_request,
  projection_reply,
  ghost_update,
  block_propagation,
  final_partition
};

enum class epoch : std::uint8_t {
  input,
  coarsening,
  contraction,
  initial_partition,
  projection,
  refinement,
  final_partition
};

struct hierarchy_position {
  std::uint32_t cycle;
  std::uint32_t level;
  epoch epoch_id;
  std::uint32_t iteration;
  std::uint32_t round;

  auto operator==(hierarchy_position const&) const -> bool = default;
};

struct semantic_actors {
  int owner = -1;
  int requester = -1;
  int receiver = -1;

  auto operator==(semantic_actors const&) const -> bool = default;
};

inline constexpr auto all_stages = std::array{
    stage::graph_distribution_node,
    stage::graph_distribution_edge,
    stage::contraction_label,
    stage::quotient_node_weight,
    stage::quotient_edge,
    stage::projection_request,
    stage::projection_reply,
    stage::ghost_update,
    stage::block_propagation,
    stage::final_partition};

struct record {
  stage stage_id;
  hierarchy_position hierarchy;
  std::uint64_t global_id;
  semantic_actors actors;
  std::string semantic_key;
  std::string payload;

  auto operator==(record const&) const -> bool = default;
};

[[nodiscard]] inline auto stage_name(stage value) -> std::string_view {
  switch (value) {
    case stage::graph_distribution_node:
      return "graph-distribution-node";
    case stage::graph_distribution_edge:
      return "graph-distribution-edge";
    case stage::contraction_label:
      return "contraction-label";
    case stage::quotient_node_weight:
      return "quotient-node-weight";
    case stage::quotient_edge:
      return "quotient-edge";
    case stage::projection_request:
      return "projection-request";
    case stage::projection_reply:
      return "projection-reply";
    case stage::ghost_update:
      return "ghost-update";
    case stage::block_propagation:
      return "block-propagation";
    case stage::final_partition:
      return "final-partition";
  }
  throw std::logic_error{"unknown MPI trace stage"};
}

[[nodiscard]] inline auto epoch_name(epoch value) -> std::string_view {
  switch (value) {
    case epoch::input:
      return "input";
    case epoch::coarsening:
      return "coarsening";
    case epoch::contraction:
      return "contraction";
    case epoch::initial_partition:
      return "initial-partition";
    case epoch::projection:
      return "projection";
    case epoch::refinement:
      return "refinement";
    case epoch::final_partition:
      return "final-partition";
  }
  throw std::logic_error{"unknown MPI trace epoch"};
}

[[nodiscard]] inline auto rank_name(int rank) -> std::string {
  return rank < 0 ? "-" : std::to_string(rank);
}

[[nodiscard]] inline auto graph_distribution_node(hierarchy_position hierarchy,
                                                  std::uint64_t global_id,
                                                  int owner,
                                                  std::uint64_t weight)
    -> record {
  return {stage::graph_distribution_node,
          hierarchy,
          global_id,
          {.owner = owner, .receiver = owner},
          "owner:" + std::to_string(owner),
          "weight=" + std::to_string(weight)};
}

[[nodiscard]] inline auto graph_distribution_edge(hierarchy_position hierarchy,
                                                  std::uint64_t source,
                                                  int owner,
                                                  std::uint64_t target,
                                                  std::uint64_t weight)
    -> record {
  return {stage::graph_distribution_edge,
          hierarchy,
          source,
          {.owner = owner, .receiver = owner},
          "target:" + std::to_string(target),
          "weight=" + std::to_string(weight)};
}

[[nodiscard]] inline auto contraction_label(hierarchy_position hierarchy,
                                            std::uint64_t global_id,
                                            int owner,
                                            std::uint64_t label,
                                            std::uint64_t coarse_id) -> record {
  return {stage::contraction_label,
          hierarchy,
          global_id,
          {.owner = owner, .receiver = owner},
          "label:" + std::to_string(label),
          "coarse=" + std::to_string(coarse_id)};
}

[[nodiscard]] inline auto quotient_node_weight(hierarchy_position hierarchy,
                                               std::uint64_t global_id,
                                               int owner,
                                               std::uint64_t weight) -> record {
  return {stage::quotient_node_weight,
          hierarchy,
          global_id,
          {.owner = owner, .receiver = owner},
          "node",
          "weight=" + std::to_string(weight)};
}

[[nodiscard]] inline auto quotient_edge(hierarchy_position hierarchy,
                                        std::uint64_t source,
                                        int owner,
                                        std::uint64_t target,
                                        std::uint64_t weight) -> record {
  return {stage::quotient_edge,
          hierarchy,
          source,
          {.owner = owner, .receiver = owner},
          "target:" + std::to_string(target),
          "weight=" + std::to_string(weight)};
}

[[nodiscard]] inline auto projection_request(hierarchy_position hierarchy,
                                             std::uint64_t request_id,
                                             int requester,
                                             int owner,
                                             std::uint64_t coarse_id) -> record {
  return {stage::projection_request,
          hierarchy,
          coarse_id,
          {.owner = owner, .requester = requester, .receiver = owner},
          "request:" + std::to_string(request_id),
          "requester=" + std::to_string(requester) +
              " owner=" + std::to_string(owner)};
}

[[nodiscard]] inline auto projection_reply(hierarchy_position hierarchy,
                                           std::uint64_t request_id,
                                           int requester,
                                           int owner,
                                           std::uint64_t coarse_id,
                                           std::uint64_t label) -> record {
  return {stage::projection_reply,
          hierarchy,
          coarse_id,
          {.owner = owner, .requester = requester, .receiver = requester},
          "request:" + std::to_string(request_id),
          "requester=" + std::to_string(requester) +
              " owner=" + std::to_string(owner) +
              " label=" + std::to_string(label)};
}

[[nodiscard]] inline auto ghost_update(hierarchy_position hierarchy,
                                       std::uint64_t global_id,
                                       int owner,
                                       int receiver,
                                       std::uint64_t label) -> record {
  return {stage::ghost_update,
          hierarchy,
          global_id,
          {.owner = owner, .receiver = receiver},
          "label",
          "label=" + std::to_string(label)};
}

[[nodiscard]] inline auto block_propagation(hierarchy_position hierarchy,
                                            std::uint64_t global_id,
                                            int owner,
                                            int receiver,
                                            std::uint64_t block) -> record {
  return {stage::block_propagation,
          hierarchy,
          global_id,
          {.owner = owner, .receiver = receiver},
          "block",
          "block=" + std::to_string(block)};
}

[[nodiscard]] inline auto final_partition(hierarchy_position hierarchy,
                                          std::uint64_t global_id,
                                          int owner,
                                          std::uint64_t block) -> record {
  return {stage::final_partition,
          hierarchy,
          global_id,
          {.owner = owner, .receiver = owner},
          "partition",
          "block=" + std::to_string(block)};
}

[[nodiscard]] inline auto canonical_text(std::span<record const> input)
    -> std::string {
  auto records = std::vector<record>{input.begin(), input.end()};
  std::ranges::sort(records, {}, [](record const& value) {
    return std::tie(value.stage_id,
                    value.hierarchy.cycle,
                    value.hierarchy.level,
                    value.hierarchy.epoch_id,
                    value.hierarchy.iteration,
                    value.hierarchy.round,
                    value.global_id,
                    value.actors.owner,
                    value.actors.requester,
                    value.actors.receiver,
                    value.semantic_key,
                    value.payload);
  });

  auto output = std::string{format_version} + " upstream=" +
                std::string{upstream_revision} + "\n";
  for (auto const& value : records) {
    output += std::string{stage_name(value.stage_id)} +
              " cycle=" + std::to_string(value.hierarchy.cycle) +
              " level=" + std::to_string(value.hierarchy.level) +
              " epoch=" + std::string{epoch_name(value.hierarchy.epoch_id)} +
              " iteration=" + std::to_string(value.hierarchy.iteration) +
              " round=" + std::to_string(value.hierarchy.round) +
              " global=" + std::to_string(value.global_id) +
              " owner=" + rank_name(value.actors.owner) +
              " requester=" + rank_name(value.actors.requester) +
              " receiver=" + rank_name(value.actors.receiver) +
              " key=" + value.semantic_key;
    if (!value.payload.empty()) {
      output += " " + value.payload;
    }
    output += '\n';
  }
  return output;
}

[[nodiscard]] inline auto canonical_text(std::vector<record> const& input)
    -> std::string {
  return canonical_text(std::span<record const>{input});
}

[[nodiscard]] inline auto sanitize_run_id(std::string_view run_id)
    -> std::string {
  auto result = std::string{run_id};
  for (auto& character : result) {
    auto const ascii_alphanumeric =
        (character >= 'a' && character <= 'z') ||
        (character >= 'A' && character <= 'Z') ||
        (character >= '0' && character <= '9');
    if (!ascii_alphanumeric && character != '-' && character != '_') {
      character = '_';
    }
  }
  return result;
}

[[nodiscard]] inline auto stable_run_id_hash(std::string_view run_id)
    -> std::uint64_t {
  constexpr std::uint64_t offset_basis = 14695981039346656037ULL;
  constexpr std::uint64_t prime = 1099511628211ULL;
  auto hash = offset_basis;
  for (auto const character : run_id) {
    hash ^= static_cast<unsigned char>(character);
    hash *= prime;
  }
  return hash;
}

[[nodiscard]] inline auto hexadecimal(std::uint64_t value) -> std::string {
  constexpr auto digits = std::string_view{"0123456789abcdef"};
  auto result = std::string(16, '0');
  for (auto index = result.size(); index > 0; --index) {
    auto const digit = static_cast<unsigned>(value & 0xfU);
    result[index - 1] = digits[digit];
    value >>= 4U;
  }
  return result;
}

[[nodiscard]] inline auto run_id_filename_component(std::string_view run_id)
    -> std::string {
  return sanitize_run_id(run_id) + "-" +
         hexadecimal(stable_run_id_hash(run_id));
}

[[nodiscard]] inline auto rank_file_path(std::string_view base_path,
                                         std::string_view run_id,
                                         int rank) -> std::string {
  if (run_id.empty()) {
    throw std::invalid_argument{
        "MPI trace filename requires a collectively resolved run ID"};
  }
  return std::string{base_path} + ".run-" +
         run_id_filename_component(run_id) + ".rank" +
         std::to_string(rank) + ".trace";
}

#if KAHIP_ENABLE_MPI_TRACE
namespace detail {
inline std::vector<record> records;
inline bool active = std::getenv("KAHIP_MPI_TRACE_PATH") != nullptr;
inline hierarchy_position hierarchy{
    .cycle = 0,
    .level = 0,
    .epoch_id = epoch::input,
    .iteration = 0,
    .round = 0};

[[nodiscard]] inline auto automatic_run_id() -> std::string {
  auto entropy = std::random_device{};
  auto first = (static_cast<std::uint64_t>(entropy()) << 32U) |
               static_cast<std::uint64_t>(entropy());
  auto second = (static_cast<std::uint64_t>(entropy()) << 32U) |
                static_cast<std::uint64_t>(entropy());
  first ^= static_cast<std::uint64_t>(
      std::chrono::system_clock::now().time_since_epoch().count());
  second ^= static_cast<std::uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  return "auto-" + hexadecimal(first) + hexadecimal(second);
}

[[nodiscard]] inline auto broadcast_string(MPI_Comm communicator,
                                           int rank,
                                           std::string root_value)
    -> std::string {
  auto length = rank == 0
                    ? static_cast<unsigned long long>(root_value.size())
                    : 0ULL;
  check(MPI_Bcast(&length, 1, MPI_UNSIGNED_LONG_LONG, 0, communicator),
        "MPI_Bcast(trace string length)");
  if (length >
      static_cast<unsigned long long>(std::numeric_limits<int>::max())) {
    throw std::runtime_error{"MPI trace run ID is too long"};
  }

  auto bytes = std::vector<char>(static_cast<std::size_t>(length));
  if (rank == 0) {
    std::ranges::copy(root_value, bytes.begin());
  }
  check(MPI_Bcast(bytes.data(), static_cast<int>(length), MPI_CHAR, 0,
                  communicator),
        "MPI_Bcast(trace string payload)");
  return {bytes.begin(), bytes.end()};
}

[[nodiscard]] inline auto resolve_base_path_collectively(
    MPI_Comm communicator,
    char const* local_base_path) -> std::optional<std::string> {
  int rank = 0;
  check(MPI_Comm_rank(communicator, &rank), "MPI_Comm_rank(trace path)");

  auto const local_present = local_base_path == nullptr ? 0 : 1;
  auto root_present = rank == 0 ? local_present : 0;
  check(MPI_Bcast(&root_present, 1, MPI_INT, 0, communicator),
        "MPI_Bcast(trace path presence)");
  auto const root_path = broadcast_string(
      communicator, rank,
      rank == 0 && local_base_path != nullptr ? local_base_path : "");
  auto const local_mismatch =
      local_present != root_present ||
      (local_present != 0 && std::string_view{local_base_path} != root_path);
  auto mismatch = local_mismatch ? 1 : 0;
  auto any_mismatch = 0;
  check(MPI_Allreduce(&mismatch, &any_mismatch, 1, MPI_INT, MPI_MAX,
                      communicator),
        "MPI_Allreduce(trace path agreement)");
  if (any_mismatch != 0) {
    throw std::runtime_error{
        "MPI trace path differs across communicator ranks"};
  }
  if (root_present == 0) {
    return std::nullopt;
  }
  return root_path;
}
}  // namespace detail

[[nodiscard]] inline auto requested_run_id() -> std::optional<std::string> {
  constexpr auto variables = std::array{
      "KAHIP_MPI_TRACE_RUN_ID",
      "SLURM_JOB_ID",
      "PBS_JOBID",
      "LSB_JOBID",
      "PMI_JOBID",
      "OMPI_MCA_orte_ess_jobid"};
  for (auto const* variable : variables) {
    auto const* value = std::getenv(variable);
    if (value != nullptr && *value != '\0') {
      return std::string{value};
    }
  }
  return std::nullopt;
}

[[nodiscard]] inline auto resolve_run_id_collectively(
    MPI_Comm communicator,
    std::optional<std::string> local_run_id) -> std::string {
  if (local_run_id && local_run_id->empty()) {
    local_run_id.reset();
  }

  int rank = 0;
  if (MPI_Comm_rank(communicator, &rank) != MPI_SUCCESS) {
    throw std::runtime_error{"MPI trace could not query rank"};
  }
  auto root_run_id = detail::broadcast_string(
      communicator, rank,
      rank == 0 ? local_run_id.value_or(std::string{}) : std::string{});
  auto const local_mismatch =
      local_run_id ? *local_run_id != root_run_id : !root_run_id.empty();
  auto mismatch = local_mismatch ? 1 : 0;
  auto any_mismatch = 0;
  if (MPI_Allreduce(&mismatch, &any_mismatch, 1, MPI_INT, MPI_MAX,
                    communicator) != MPI_SUCCESS) {
    throw std::runtime_error{"MPI trace run-ID validation failed"};
  }
  if (any_mismatch != 0) {
    throw std::runtime_error{
        "MPI trace run ID differs across communicator ranks"};
  }
  if (!root_run_id.empty()) {
    return root_run_id;
  }

  return detail::broadcast_string(
      communicator, rank, rank == 0 ? detail::automatic_run_id() : "");
}

inline void set_active(bool active) { detail::active = active; }
inline void set_hierarchy(hierarchy_position hierarchy) {
  detail::hierarchy = hierarchy;
}
inline void set_iteration(std::uint32_t iteration) {
  detail::hierarchy.iteration = iteration;
}
[[nodiscard]] inline auto current_hierarchy() -> hierarchy_position {
  return detail::hierarchy;
}
[[nodiscard]] inline auto current_hierarchy_with_round(std::uint32_t round)
    -> hierarchy_position {
  auto hierarchy = current_hierarchy();
  hierarchy.round = round;
  return hierarchy;
}
inline void reset() { detail::records.clear(); }
[[nodiscard]] inline auto snapshot() -> std::vector<record> {
  return detail::records;
}
inline void append(record value) {
  if (detail::active) {
    detail::records.push_back(std::move(value));
  }
}
inline void write_rank_file_if_requested(MPI_Comm communicator) {
  auto owned_communicator = ::parhip::mpi::communicator{
      ::parhip::mpi::communicator_view{communicator}};
  auto const collective_communicator = owned_communicator.view();
  auto const base_path = detail::resolve_base_path_collectively(
      collective_communicator.native_handle(),
      std::getenv("KAHIP_MPI_TRACE_PATH"));
  if (!base_path) {
    return;
  }
  auto const rank = collective_communicator.rank();
  auto const run_id = resolve_run_id_collectively(
      collective_communicator.native_handle(), requested_run_id());
  auto const path = rank_file_path(*base_path, run_id, rank);
  auto output = std::ofstream{path, std::ios::binary | std::ios::trunc};
  if (!output) {
    throw std::runtime_error{"MPI trace could not open " + path};
  }
  output << canonical_text(detail::records);
  if (!output) {
    throw std::runtime_error{"MPI trace could not write " + path};
  }
}
#else
inline void set_active(bool) noexcept {}
inline void reset() noexcept {}
[[nodiscard]] inline auto snapshot() -> std::vector<record> { return {}; }
inline void append(record) noexcept {}
inline void write_rank_file_if_requested(MPI_Comm) noexcept {}
#endif
}  // namespace parhip::mpi::trace

#if KAHIP_ENABLE_MPI_TRACE
#define KAHIP_MPI_TRACE(record_expression)                                    \
  do {                                                                        \
    ::parhip::mpi::trace::append((record_expression));                        \
  } while (false)
#define KAHIP_MPI_TRACE_SET_HIERARCHY(cycle_value, level_value, epoch_value)   \
  do {                                                                        \
    ::parhip::mpi::trace::set_hierarchy(                                      \
        {.cycle = static_cast<std::uint32_t>(cycle_value),                    \
         .level = static_cast<std::uint32_t>(level_value),                    \
         .epoch_id = (epoch_value),                                           \
         .iteration = 0,                                                      \
         .round = 0});                                                        \
  } while (false)
#define KAHIP_MPI_TRACE_SET_ITERATION(iteration_value)                        \
  do {                                                                        \
    ::parhip::mpi::trace::set_iteration(                                      \
        static_cast<std::uint32_t>(iteration_value));                         \
  } while (false)
#else
#define KAHIP_MPI_TRACE(record_expression)                                    \
  do {                                                                        \
  } while (false)
#define KAHIP_MPI_TRACE_SET_HIERARCHY(cycle_value, level_value, epoch_value)   \
  do {                                                                        \
  } while (false)
#define KAHIP_MPI_TRACE_SET_ITERATION(iteration_value)                        \
  do {                                                                        \
  } while (false)
#endif
