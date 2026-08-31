#pragma once

#include <mpi.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#ifndef KAHIP_ENABLE_MPI_TRACE
#define KAHIP_ENABLE_MPI_TRACE 0
#endif

namespace parhip::mpi::trace {
inline constexpr std::string_view format_version = "kahip-mpi-trace-v1";
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
  std::uint64_t global_id;
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

[[nodiscard]] inline auto graph_distribution_node(std::uint64_t global_id,
                                                  int owner,
                                                  std::uint64_t weight)
    -> record {
  return {stage::graph_distribution_node,
          global_id,
          "owner:" + std::to_string(owner),
          "weight=" + std::to_string(weight)};
}

[[nodiscard]] inline auto graph_distribution_edge(std::uint64_t source,
                                                  std::uint64_t target,
                                                  std::uint64_t weight)
    -> record {
  return {stage::graph_distribution_edge,
          source,
          "target:" + std::to_string(target),
          "weight=" + std::to_string(weight)};
}

[[nodiscard]] inline auto contraction_label(std::uint64_t global_id,
                                            std::uint64_t label,
                                            std::uint64_t coarse_id) -> record {
  return {stage::contraction_label,
          global_id,
          "label:" + std::to_string(label),
          "coarse=" + std::to_string(coarse_id)};
}

[[nodiscard]] inline auto quotient_node_weight(std::uint64_t global_id,
                                               std::uint64_t weight) -> record {
  return {stage::quotient_node_weight,
          global_id,
          "node",
          "weight=" + std::to_string(weight)};
}

[[nodiscard]] inline auto quotient_edge(std::uint64_t source,
                                        std::uint64_t target,
                                        std::uint64_t weight) -> record {
  return {stage::quotient_edge,
          source,
          "target:" + std::to_string(target),
          "weight=" + std::to_string(weight)};
}

[[nodiscard]] inline auto projection_request(std::uint64_t request_id,
                                             int source,
                                             int destination,
                                             std::uint64_t coarse_id) -> record {
  return {stage::projection_request,
          coarse_id,
          "request:" + std::to_string(request_id),
          "source=" + std::to_string(source) +
              " destination=" + std::to_string(destination)};
}

[[nodiscard]] inline auto projection_reply(std::uint64_t request_id,
                                           int source,
                                           int destination,
                                           std::uint64_t coarse_id,
                                           std::uint64_t label) -> record {
  return {stage::projection_reply,
          coarse_id,
          "request:" + std::to_string(request_id),
          "source=" + std::to_string(source) +
              " destination=" + std::to_string(destination) +
              " label=" + std::to_string(label)};
}

[[nodiscard]] inline auto ghost_update(std::uint64_t global_id,
                                       int owner,
                                       std::uint64_t label) -> record {
  return {stage::ghost_update,
          global_id,
          "owner:" + std::to_string(owner),
          "label=" + std::to_string(label)};
}

[[nodiscard]] inline auto block_propagation(std::uint64_t global_id,
                                            std::uint64_t block) -> record {
  return {stage::block_propagation,
          global_id,
          "block",
          "block=" + std::to_string(block)};
}

[[nodiscard]] inline auto final_partition(std::uint64_t global_id,
                                          std::uint64_t block) -> record {
  return {stage::final_partition,
          global_id,
          "partition",
          "block=" + std::to_string(block)};
}

[[nodiscard]] inline auto canonical_text(std::span<record const> input)
    -> std::string {
  auto records = std::vector<record>{input.begin(), input.end()};
  std::ranges::sort(records, {}, [](record const& value) {
    return std::tie(value.stage_id,
                    value.global_id,
                    value.semantic_key,
                    value.payload);
  });

  auto output = std::string{format_version} + " upstream=" +
                std::string{upstream_revision} + "\n";
  for (auto const& value : records) {
    output += std::string{stage_name(value.stage_id)} +
              " global=" + std::to_string(value.global_id) +
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

#if KAHIP_ENABLE_MPI_TRACE
namespace detail {
inline std::vector<record> records;
inline bool active = std::getenv("KAHIP_MPI_TRACE_PATH") != nullptr;
}  // namespace detail

inline void set_active(bool active) { detail::active = active; }
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
  auto const* base_path = std::getenv("KAHIP_MPI_TRACE_PATH");
  if (base_path == nullptr) {
    return;
  }
  int rank = 0;
  if (MPI_Comm_rank(communicator, &rank) != MPI_SUCCESS) {
    throw std::runtime_error{"MPI trace could not query rank"};
  }
  auto const path =
      std::string{base_path} + ".rank" + std::to_string(rank) + ".trace";
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
#else
#define KAHIP_MPI_TRACE(record_expression)                                    \
  do {                                                                        \
  } while (false)
#endif
