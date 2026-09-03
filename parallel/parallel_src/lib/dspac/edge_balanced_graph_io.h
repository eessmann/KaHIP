/******************************************************************************
 * edge_balanced_graph_io.h
 * *
 * Source of KaHIP -- Karlsruhe High Quality Partitioning.
 * Christian Schulz <christian.schulz.phone@gmail.com>
 *****************************************************************************/

#ifndef KAHIP_EDGEBALANCED_GRAPH_IO_H
#define KAHIP_EDGEBALANCED_GRAPH_IO_H

#include <algorithm>
#include <cstddef>
#include <functional>
#include <limits>
#include <numeric>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "communication/mpi_handles.h"
#include "data_structure/parallel_graph_access.h"
#include "definitions.h"
#include "partition_config.h"

namespace parhip {
namespace edge_balanced_graph_io_detail {
inline constexpr ULONG file_type_version = 3;
inline constexpr ULONG header_words = 3;

static_assert(sizeof(ULONG) == 8,
              "KaHIP binary graph version 3 stores 64-bit words");

struct binary_layout final {
  ULONG adjacency_begin = 0;
  ULONG file_extent = 0;

  auto operator==(binary_layout const&) const -> bool = default;
};

[[nodiscard]] constexpr auto checked_add(ULONG left, ULONG right) noexcept
    -> std::optional<ULONG> {
  if (right > std::numeric_limits<ULONG>::max() - left) {
    return std::nullopt;
  }
  return left + right;
}

[[nodiscard]] constexpr auto checked_multiply(ULONG left, ULONG right) noexcept
    -> std::optional<ULONG> {
  if (left != 0 && right > std::numeric_limits<ULONG>::max() / left) {
    return std::nullopt;
  }
  return left * right;
}

[[nodiscard]] constexpr auto make_binary_layout(NodeID nodes,
                                                EdgeID edges) noexcept
    -> std::optional<binary_layout> {
  auto const offset_words = checked_add(nodes, ULONG{1});
  auto const words_before_adjacency =
      offset_words.has_value() ? checked_add(header_words, *offset_words)
                               : std::nullopt;
  auto const adjacency_begin =
      words_before_adjacency.has_value()
          ? checked_multiply(*words_before_adjacency, ULONG{sizeof(ULONG)})
          : std::nullopt;
  auto const adjacency_bytes = checked_multiply(edges, ULONG{sizeof(ULONG)});
  auto const file_extent =
      adjacency_begin.has_value() && adjacency_bytes.has_value()
          ? checked_add(*adjacency_begin, *adjacency_bytes)
          : std::nullopt;
  if (!adjacency_begin.has_value() || !file_extent.has_value()) {
    return std::nullopt;
  }
  return binary_layout{*adjacency_begin, *file_extent};
}

[[nodiscard]] constexpr auto file_extent_is_valid(ULONG observed_extent,
                                                  binary_layout layout) noexcept
    -> bool {
  return observed_extent == layout.file_extent;
}

[[nodiscard]] inline auto offsets_are_valid(std::span<ULONG const> offsets,
                                            binary_layout layout,
                                            bool require_adjacency_begin,
                                            bool require_file_extent) noexcept
    -> bool {
  if (offsets.empty() || !std::ranges::is_sorted(offsets)) {
    return false;
  }
  auto const in_layout = std::ranges::all_of(offsets, [&](auto offset) {
    return offset >= layout.adjacency_begin && offset <= layout.file_extent &&
           (offset - layout.adjacency_begin) % sizeof(ULONG) == 0;
  });
  return in_layout &&
         (!require_adjacency_begin ||
          offsets.front() == layout.adjacency_begin) &&
         (!require_file_extent || offsets.back() == layout.file_extent);
}

[[nodiscard]] inline auto targets_are_valid(std::span<EdgeID const> targets,
                                            NodeID nodes) noexcept -> bool {
  return std::ranges::all_of(targets,
                             [=](auto target) { return target < nodes; });
}

[[nodiscard]] constexpr auto validated_window(int configured,
                                              int communicator_size) noexcept
    -> std::optional<int> {
  if (configured <= 0 || communicator_size <= 0) {
    return std::nullopt;
  }
  return std::min(configured, communicator_size);
}

[[nodiscard]] constexpr auto balanced_vertex_boundary(NodeID nodes,
                                                      int boundary,
                                                      int size) noexcept
    -> NodeID {
  if (boundary <= 0) {
    return 0;
  }
  if (boundary >= size) {
    return nodes;
  }
  auto const divisor = static_cast<NodeID>(size);
  auto const quotient = nodes / divisor;
  auto const remainder = nodes % divisor;
  auto const index = static_cast<NodeID>(boundary);
  return quotient * index + std::min(index, remainder);
}

[[nodiscard]] constexpr auto balanced_edge_target(EdgeID edges,
                                                  int boundary,
                                                  int size) noexcept -> EdgeID {
  if (boundary <= 0) {
    return 0;
  }
  if (boundary >= size) {
    return edges;
  }
  auto const divisor = static_cast<EdgeID>(size);
  auto const quotient = edges / divisor;
  auto const remainder = edges % divisor;
  auto const index = static_cast<EdgeID>(boundary);
  return quotient * index + std::min(index, remainder);
}

[[nodiscard]] inline auto node_ranges_from_offsets(
    std::span<ULONG const> offsets,
    EdgeID edges,
    int size) -> std::vector<NodeID> {
  if (offsets.empty() || size <= 0) {
    return {};
  }
  auto const nodes = static_cast<NodeID>(offsets.size() - 1);
  auto ranges = std::vector<NodeID>(static_cast<std::size_t>(size) + 1);
  for (auto boundary = 0; boundary <= size; ++boundary) {
    if (edges == 0) {
      ranges[static_cast<std::size_t>(boundary)] =
          balanced_vertex_boundary(nodes, boundary, size);
      continue;
    }
    if (boundary == size) {
      ranges.back() = nodes;
      continue;
    }
    auto const target = balanced_edge_target(edges, boundary, size);
    auto const position = std::ranges::lower_bound(
        offsets, target, std::less<>{}, [&](auto offset) {
          return (offset - offsets.front()) / sizeof(ULONG);
        });
    ranges[static_cast<std::size_t>(boundary)] =
        static_cast<NodeID>(std::ranges::distance(offsets.begin(), position));
  }
  return ranges;
}

[[nodiscard]] inline auto canonicalize_adjacency(std::span<ULONG const> offsets,
                                                 std::span<EdgeID> adjacency,
                                                 std::span<EdgeID> permutation)
    -> bool {
  if (offsets.empty() || adjacency.size() != permutation.size() ||
      !std::in_range<ULONG>(adjacency.size()) ||
      !std::ranges::is_sorted(offsets)) {
    return false;
  }
  auto const adjacency_bytes = checked_multiply(
      static_cast<ULONG>(adjacency.size()), ULONG{sizeof(ULONG)});
  if (!adjacency_bytes.has_value() || offsets.back() < offsets.front() ||
      offsets.back() - offsets.front() != *adjacency_bytes ||
      !std::ranges::all_of(offsets, [&](auto offset) {
        return offset >= offsets.front() &&
               (offset - offsets.front()) % sizeof(ULONG) == 0;
      })) {
    return false;
  }

  std::iota(permutation.begin(), permutation.end(), EdgeID{0});
  auto position = std::size_t{0};
  for (auto local_node = std::size_t{0}; local_node + 1 < offsets.size();
       ++local_node) {
    auto const degree = static_cast<std::size_t>(
        (offsets[local_node + 1] - offsets[local_node]) / sizeof(ULONG));
    if (degree > adjacency.size() - position) {
      return false;
    }
    auto const first =
        permutation.begin() + static_cast<std::ptrdiff_t>(position);
    auto const next = first + static_cast<std::ptrdiff_t>(degree);
    std::sort(first, next, [&](EdgeID left, EdgeID right) {
      return adjacency[static_cast<std::size_t>(left)] <
             adjacency[static_cast<std::size_t>(right)];
    });
    std::ranges::sort(adjacency.subspan(position, degree));
    position += degree;
  }
  return position == adjacency.size();
}
}  // namespace edge_balanced_graph_io_detail

class edge_balanced_graph_io {
 public:
  static void read_binary_graph_edge_balanced(
      parallel_graph_access& graph,
      std::string const& filename,
      PPartitionConfig const& config,
      std::vector<EdgeID>& permutation,
      mpi::communicator_view communicator);

  // Compatibility shims retain existing callers while all rank and size
  // decisions inside the operation are derived from the graph communicator.
  static void read_binary_graph_edge_balanced(parallel_graph_access& graph,
                                              std::string const& filename,
                                              PPartitionConfig const& config,
                                              std::vector<EdgeID>& permutation,
                                              int supplied_rank,
                                              int supplied_size);

  static void read_binary_graph_edge_balanced(parallel_graph_access& graph,
                                              std::string const& filename,
                                              PPartitionConfig const& config,
                                              std::vector<EdgeID>& permutation);
};

}  // namespace parhip
#endif  // KAHIP_EDGEBALANCED_GRAPH_IO_H
