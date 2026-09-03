#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string_view>
#include <vector>

#include "dspac/edge_balanced_graph_io.h"

namespace {
using parhip::EdgeID;
using parhip::NodeID;
using parhip::ULONG;
namespace detail = parhip::edge_balanced_graph_io_detail;

void require(bool condition, std::string_view diagnostic) {
  if (!condition) {
    std::cerr << diagnostic << '\n';
    std::exit(EXIT_FAILURE);
  }
}

[[nodiscard]] auto checksum(std::span<EdgeID const> values) noexcept
    -> std::uint64_t {
  auto result = std::uint64_t{14695981039346656037ULL};
  for (auto value : values) {
    result ^= static_cast<std::uint64_t>(value);
    result *= std::uint64_t{1099511628211ULL};
  }
  return result;
}
}  // namespace

int main() {
  auto const empty_layout = detail::make_binary_layout(0, 0);
  require(empty_layout.has_value(),
          "empty binary layout must be representable");
  require(empty_layout->adjacency_begin == ULONG{32} &&
              empty_layout->file_extent == ULONG{32},
          "empty binary layout must contain one terminal offset");
  auto const empty_offsets = std::array<ULONG, 1>{32};
  require(detail::offsets_are_valid(empty_offsets, *empty_layout, true, true),
          "empty graph terminal offset must be valid");

  auto const edgeless_layout = detail::make_binary_layout(3, 0);
  require(edgeless_layout.has_value(),
          "edgeless binary layout must be representable");
  auto const edgeless_offsets = std::array<ULONG, 4>{56, 56, 56, 56};
  require(
      detail::offsets_are_valid(edgeless_offsets, *edgeless_layout, true, true),
      "repeated offsets must be valid for isolated vertices");
  auto const edgeless_ranges =
      detail::node_ranges_from_offsets(edgeless_offsets, 0, 5);
  require(
      edgeless_ranges == std::vector<NodeID>{NodeID{0}, NodeID{1}, NodeID{2},
                                             NodeID{3}, NodeID{3}, NodeID{3}},
      "edgeless graphs must use deterministic vertex-balanced ranges");

  auto const small_layout = detail::make_binary_layout(3, 4);
  require(small_layout.has_value(), "3-vertex layout must be representable");
  auto const small_offsets = std::array<ULONG, 4>{56, 72, 80, 88};
  require(detail::offsets_are_valid(small_offsets, *small_layout, true, true),
          "valid uneven offsets must be accepted");
  auto const small_ranges =
      detail::node_ranges_from_offsets(small_offsets, 4, 5);
  require(small_ranges == std::vector<NodeID>{NodeID{0}, NodeID{1}, NodeID{1},
                                              NodeID{2}, NodeID{3}, NodeID{3}},
          "edge-balanced lower-bound ranges must allow zero-work ranks");

  require(!detail::validated_window(0, 5).has_value(),
          "a zero I/O window must be rejected");
  require(!detail::validated_window(-1, 5).has_value(),
          "a negative I/O window must be rejected");
  require(detail::validated_window(8, 5) == 5,
          "the I/O window must not exceed communicator size");

  require(!detail::file_extent_is_valid(87, *small_layout),
          "a truncated payload must be rejected");
  require(!detail::file_extent_is_valid(96, *small_layout),
          "trailing payload bytes must be rejected");
  auto const nonmonotone = std::array<ULONG, 4>{56, 80, 72, 88};
  require(!detail::offsets_are_valid(nonmonotone, *small_layout, true, true),
          "nonmonotone offsets must be rejected");
  auto const unaligned = std::array<ULONG, 4>{56, 73, 80, 88};
  require(!detail::offsets_are_valid(unaligned, *small_layout, true, true),
          "unaligned offsets must be rejected");
  auto const wrong_terminal = std::array<ULONG, 4>{56, 72, 80, 80};
  require(!detail::offsets_are_valid(wrong_terminal, *small_layout, true, true),
          "an incorrect terminal offset must be rejected");
  auto const invalid_target = std::array<EdgeID, 1>{3};
  require(!detail::targets_are_valid(invalid_target, 3),
          "an out-of-domain target must be rejected");
  require(detail::targets_are_valid(std::span<EdgeID const>{}, 0),
          "an empty graph must have a valid empty target range");

  auto adjacency = std::vector<EdgeID>{2, 0, 1, 2, 0};
  auto permutation = std::vector<EdgeID>(adjacency.size());
  auto const local_offsets = std::array<ULONG, 3>{64, 88, 104};
  require(detail::canonicalize_adjacency(local_offsets, adjacency, permutation),
          "valid local adjacency must canonicalize");
  require(adjacency == std::vector<EdgeID>{0, 1, 2, 0, 2},
          "adjacency targets must be sorted within each vertex");
  require(permutation == std::vector<EdgeID>{1, 2, 0, 4, 3},
          "permutation must retain original local edge positions");
  require(checksum(adjacency) == std::uint64_t{3706092854568170190ULL} &&
              checksum(permutation) == std::uint64_t{1726334132918446519ULL},
          "valid fixture checksums must remain exact");
}
