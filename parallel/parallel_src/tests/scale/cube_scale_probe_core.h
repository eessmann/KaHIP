#pragma once

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace parhip::scale_probe {
static_assert(std::numeric_limits<unsigned long long>::digits == 64);

struct cube_counts final {
  std::uint64_t vertices;
  std::uint64_t undirected_edges;
  std::uint64_t directed_edges;

  auto operator==(cube_counts const&) const -> bool = default;
};

namespace detail {
[[nodiscard]] constexpr auto checked_add(std::uint64_t left,
                                         std::uint64_t right) noexcept
    -> std::optional<std::uint64_t> {
  if (right > std::numeric_limits<std::uint64_t>::max() - left) {
    return std::nullopt;
  }
  return left + right;
}

[[nodiscard]] constexpr auto checked_multiply(std::uint64_t left,
                                              std::uint64_t right) noexcept
    -> std::optional<std::uint64_t> {
  if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left) {
    return std::nullopt;
  }
  return left * right;
}
}  // namespace detail

[[nodiscard]] constexpr auto counts_for_side(std::uint64_t side) noexcept
    -> std::optional<cube_counts> {
  if (side == 0) {
    return std::nullopt;
  }
  auto const square = detail::checked_multiply(side, side);
  if (!square.has_value()) {
    return std::nullopt;
  }
  auto const vertices = detail::checked_multiply(*square, side);
  auto const per_axis = detail::checked_multiply(*square, side - 1);
  if (!vertices.has_value() || !per_axis.has_value()) {
    return std::nullopt;
  }
  auto const undirected = detail::checked_multiply(*per_axis, 3);
  if (!undirected.has_value()) {
    return std::nullopt;
  }
  auto const directed = detail::checked_multiply(*undirected, 2);
  if (!directed.has_value()) {
    return std::nullopt;
  }
  return cube_counts{.vertices = *vertices,
                     .undirected_edges = *undirected,
                     .directed_edges = *directed};
}

[[nodiscard]] constexpr auto balanced_boundary(std::uint64_t total,
                                               std::uint32_t index,
                                               std::uint32_t parts) noexcept
    -> std::optional<std::uint64_t> {
  if (parts == 0 || index > parts) {
    return std::nullopt;
  }
  auto const quotient = total / parts;
  auto const remainder = total % parts;
  auto const whole = detail::checked_multiply(quotient, index);
  auto const fractional_numerator = detail::checked_multiply(remainder, index);
  if (!whole.has_value() || !fractional_numerator.has_value()) {
    return std::nullopt;
  }
  return detail::checked_add(*whole, *fractional_numerator / parts);
}

[[nodiscard]] inline auto write_balanced_boundaries(
    std::uint64_t total,
    std::span<std::uint64_t> boundaries) noexcept -> bool {
  if (boundaries.size() < 2 ||
      boundaries.size() - 1 >
          static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
    return false;
  }
  auto const parts = static_cast<std::uint32_t>(boundaries.size() - 1);
  for (auto index = std::uint32_t{0};; ++index) {
    auto const boundary = balanced_boundary(total, index, parts);
    if (!boundary.has_value()) {
      return false;
    }
    boundaries[index] = *boundary;
    if (index == parts) {
      break;
    }
  }
  return true;
}

[[nodiscard]] constexpr auto maximum_balanced_slice(
    std::uint64_t total,
    std::uint32_t parts) noexcept -> std::optional<std::uint64_t> {
  if (parts == 0) {
    return std::nullopt;
  }
  auto const quotient = total / parts;
  return quotient + (total % parts == 0 ? std::uint64_t{0} : std::uint64_t{1});
}

[[nodiscard]] constexpr auto exact_unit_weight_bound(
    std::uint64_t vertices,
    std::uint32_t blocks,
    unsigned imbalance_percent) noexcept -> std::optional<std::uint64_t> {
  if (blocks == 0) {
    return std::nullopt;
  }
  auto const quotient = vertices / blocks;
  auto const ceiling =
      quotient + (vertices % blocks == 0 ? std::uint64_t{0} : std::uint64_t{1});
  auto const factor =
      detail::checked_add(100, static_cast<std::uint64_t>(imbalance_percent));
  if (!factor.has_value()) {
    return std::nullopt;
  }

  // floor(ceiling * (100 + p) / 100), without forming the possibly
  // overflowing product. This implementation is deliberately independent of
  // the library helper whose public-bound behavior the probe is checking.
  auto const whole_hundreds = ceiling / 100;
  auto const remaining_hundredths = ceiling % 100;
  auto const whole = detail::checked_multiply(whole_hundreds, *factor);
  auto const fractional =
      detail::checked_multiply(remaining_hundredths, *factor);
  if (!whole.has_value() || !fractional.has_value()) {
    return std::nullopt;
  }
  return detail::checked_add(*whole, *fractional / 100);
}

struct cube_neighbor_list final {
  std::array<std::uint64_t, 6> values{};
  std::uint8_t count{};

  [[nodiscard]] constexpr auto span() const noexcept
      -> std::span<std::uint64_t const> {
    return {values.data(), count};
  }
};

[[nodiscard]] constexpr auto neighbors_for_vertex(std::uint64_t side,
                                                  std::uint64_t vertex)
    -> std::optional<cube_neighbor_list> {
  auto const counts = counts_for_side(side);
  if (!counts.has_value() || vertex >= counts->vertices) {
    return std::nullopt;
  }
  auto const plane = side * side;
  auto const z = vertex / plane;
  auto const in_plane = vertex % plane;
  auto const y = in_plane / side;
  auto const x = in_plane % side;
  auto result = cube_neighbor_list{};
  auto const append = [&result](std::uint64_t target) constexpr {
    result.values[result.count++] = target;
  };
  if (x != 0) {
    append(vertex - 1);
  }
  if (x + 1 < side) {
    append(vertex + 1);
  }
  if (y != 0) {
    append(vertex - side);
  }
  if (y + 1 < side) {
    append(vertex + side);
  }
  if (z != 0) {
    append(vertex - plane);
  }
  if (z + 1 < side) {
    append(vertex + plane);
  }
  std::ranges::sort(
      std::span{result.values.data(), static_cast<std::size_t>(result.count)});
  return result;
}

struct local_cube_csr final {
  std::uint64_t first_vertex{};
  std::uint64_t vertex_end{};
  std::vector<unsigned long long> offsets;
  std::vector<unsigned long long> targets;
};

[[nodiscard]] inline auto build_local_cube_csr(std::uint64_t side,
                                               std::uint64_t first,
                                               std::uint64_t end)
    -> local_cube_csr {
  auto const counts = counts_for_side(side);
  if (!counts.has_value()) {
    throw std::overflow_error{"cube counts are outside the uint64 domain"};
  }
  if (first > end) {
    throw std::invalid_argument{"cube slice start exceeds its end"};
  }
  if (end > counts->vertices) {
    throw std::out_of_range{"cube slice exceeds the vertex domain"};
  }
  auto const local_vertices_u64 = end - first;
  if (!std::in_range<std::size_t>(local_vertices_u64)) {
    throw std::length_error{"cube slice exceeds the size_t domain"};
  }
  auto const local_vertices = static_cast<std::size_t>(local_vertices_u64);
  if (local_vertices == std::numeric_limits<std::size_t>::max() ||
      local_vertices + 1 > std::vector<unsigned long long>{}.max_size()) {
    throw std::length_error{"cube offsets exceed vector capacity"};
  }

  auto local_edges = std::uint64_t{0};
  for (auto vertex = first; vertex < end; ++vertex) {
    auto const adjacent = neighbors_for_vertex(side, vertex);
    if (!adjacent.has_value()) {
      throw std::logic_error{"valid cube slice produced an invalid vertex"};
    }
    auto const next = detail::checked_add(local_edges, adjacent->count);
    if (!next.has_value()) {
      throw std::overflow_error{"local cube edge count overflow"};
    }
    local_edges = *next;
  }
  if (!std::in_range<std::size_t>(local_edges) ||
      static_cast<std::size_t>(local_edges) >
          std::vector<unsigned long long>{}.max_size()) {
    throw std::length_error{"cube targets exceed vector capacity"};
  }

  auto result = local_cube_csr{
      .first_vertex = first,
      .vertex_end = end,
      .offsets = std::vector<unsigned long long>(local_vertices + 1),
      .targets = std::vector<unsigned long long>(
          static_cast<std::size_t>(local_edges)),
  };
  auto target_index = std::size_t{0};
  for (auto local = std::size_t{0}; local < local_vertices; ++local) {
    auto const adjacent = neighbors_for_vertex(side, first + local);
    if (!adjacent.has_value()) {
      throw std::logic_error{"valid cube slice produced an invalid vertex"};
    }
    result.offsets[local] = static_cast<unsigned long long>(target_index);
    for (auto target : adjacent->span()) {
      result.targets[target_index++] = static_cast<unsigned long long>(target);
    }
  }
  result.offsets.back() = static_cast<unsigned long long>(target_index);
  if (target_index != result.targets.size()) {
    throw std::logic_error{"cube CSR passes disagree"};
  }
  return result;
}

struct digest_lanes final {
  std::array<std::uint64_t, 4> values{};

  auto operator==(digest_lanes const&) const -> bool = default;

  constexpr auto operator^=(digest_lanes const& other) noexcept
      -> digest_lanes& {
    for (auto lane = std::size_t{0}; lane < values.size(); ++lane) {
      values[lane] ^= other.values[lane];
    }
    return *this;
  }
};

[[nodiscard]] constexpr auto operator^(digest_lanes left,
                                       digest_lanes const& right) noexcept
    -> digest_lanes {
  left ^= right;
  return left;
}

// Digest schema v1 hashes semantic integers only. Each record starts from one
// of four published lane seeds XORed with its domain and the versioned golden
// ratio tag. Ordered fields are tagged by their one-based ordinal, finalized
// with SplitMix64, and folded into the lane state in order. Complete record
// digests are combined with XOR, making the aggregate independent of the MPI
// decomposition while retaining field order inside each semantic record. No
// object representation or std::hash participates.
enum class digest_domain : std::uint64_t {
  cube_vertex = 0x637562655f767478ULL,
  cube_arc = 0x637562655f617263ULL,
  partition_map = 0x706172745f6d6170ULL,
  profile_sequence = 0x70726f66696c655fULL,
};

namespace detail {
inline constexpr auto digest_version = std::uint64_t{1};
inline constexpr auto golden = std::uint64_t{0x9e3779b97f4a7c15ULL};
inline constexpr auto lane_seeds = std::array{
    std::uint64_t{0x243f6a8885a308d3ULL},
    std::uint64_t{0x13198a2e03707344ULL},
    std::uint64_t{0xa4093822299f31d0ULL},
    std::uint64_t{0x082efa98ec4e6c89ULL},
};

[[nodiscard]] constexpr auto splitmix64(std::uint64_t value) noexcept
    -> std::uint64_t {
  value += golden;
  value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
  value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
  return value ^ (value >> 31);
}
}  // namespace detail

template <std::size_t FieldCount>
[[nodiscard]] constexpr auto digest_record(
    digest_domain domain,
    std::array<std::uint64_t, FieldCount> const& fields) noexcept
    -> digest_lanes {
  auto result = digest_lanes{};
  for (auto lane = std::size_t{0}; lane < result.values.size(); ++lane) {
    auto hash = detail::splitmix64(detail::lane_seeds[lane] ^
                                   static_cast<std::uint64_t>(domain) ^
                                   (detail::golden * detail::digest_version));
    for (auto field = std::size_t{0}; field < fields.size(); ++field) {
      auto const tagged =
          fields[field] +
          detail::golden * (static_cast<std::uint64_t>(field) + 1);
      hash = detail::splitmix64(hash ^ detail::splitmix64(tagged));
    }
    result.values[lane] = hash;
  }
  return result;
}

[[nodiscard]] inline auto graph_digest(std::uint64_t side,
                                       std::uint64_t first,
                                       std::uint64_t end) -> digest_lanes {
  auto const counts = counts_for_side(side);
  if (!counts.has_value() || first > end || end > counts->vertices) {
    throw std::out_of_range{"graph digest slice is outside the cube"};
  }
  auto result = digest_lanes{};
  for (auto vertex = first; vertex < end; ++vertex) {
    auto const adjacent = *neighbors_for_vertex(side, vertex);
    // Vertex tuple: side, global ID, directed degree, unit node weight.
    result ^= digest_record(
        digest_domain::cube_vertex,
        std::array<std::uint64_t, 4>{side, vertex, adjacent.count, 1});
    for (auto ordinal = std::size_t{0}; ordinal < adjacent.count; ++ordinal) {
      // Arc tuple: side, source, sorted-adjacency ordinal, target, unit edge
      // weight. Both directions are present because this hashes the CSR.
      result ^=
          digest_record(digest_domain::cube_arc,
                        std::array<std::uint64_t, 5>{
                            side, vertex, static_cast<std::uint64_t>(ordinal),
                            adjacent.values[ordinal], 1});
    }
  }
  return result;
}

template <std::integral Label, std::size_t Extent>
[[nodiscard]] auto partition_digest(std::uint64_t side,
                                    std::uint64_t first,
                                    std::span<Label const, Extent> labels,
                                    std::uint64_t blocks) -> digest_lanes {
  auto const counts = counts_for_side(side);
  if (!counts.has_value() || blocks == 0 || labels.size() > counts->vertices ||
      first > counts->vertices - labels.size()) {
    throw std::out_of_range{"partition digest slice is outside the cube"};
  }
  auto result = digest_lanes{};
  for (auto local = std::size_t{0}; local < labels.size(); ++local) {
    using label_type = std::remove_cv_t<Label>;
    if constexpr (std::signed_integral<label_type>) {
      if (labels[local] < 0) {
        throw std::out_of_range{"partition digest label is negative"};
      }
    }
    auto const label = static_cast<std::uint64_t>(labels[local]);
    if (label >= blocks) {
      throw std::out_of_range{
          "partition digest label is outside the block domain"};
    }
    result ^= digest_record(
        digest_domain::partition_map,
        std::array<std::uint64_t, 4>{side, first + local, label, blocks});
  }
  return result;
}

template <std::integral Label, std::size_t Size>
[[nodiscard]] auto partition_digest(std::uint64_t side,
                                    std::uint64_t first,
                                    std::array<Label, Size> const& labels,
                                    std::uint64_t blocks) -> digest_lanes {
  return partition_digest(side, first, std::span<Label const>{labels}, blocks);
}
}  // namespace parhip::scale_probe
