#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <tuple>

namespace kahip::initial_partitioning {
struct bipartition_targets final {
  std::uint64_t lhs = 0;
  std::uint64_t rhs = 0;

  auto operator==(bipartition_targets const&) const -> bool = default;
};

[[nodiscard]] constexpr auto validated_bipartition_targets(
    std::span<int const> target_weights) noexcept
    -> std::optional<bipartition_targets> {
  if (target_weights.size() < 2 || target_weights[0] < 0 ||
      target_weights[1] < 0) {
    return std::nullopt;
  }
  return bipartition_targets{static_cast<std::uint64_t>(target_weights[0]),
                             static_cast<std::uint64_t>(target_weights[1])};
}

[[nodiscard]] constexpr auto overload(std::uint64_t weight,
                                      std::uint64_t target) noexcept
    -> std::uint64_t {
  return weight > target ? weight - target : 0;
}

[[nodiscard]] constexpr auto saturating_add(std::uint64_t lhs,
                                            std::uint64_t rhs) noexcept
    -> std::uint64_t {
  return rhs > std::numeric_limits<std::uint64_t>::max() - lhs
             ? std::numeric_limits<std::uint64_t>::max()
             : lhs + rhs;
}

struct bipartition_candidate final {
  std::uint64_t cut = 0;
  std::uint64_t lhs_weight = 0;
  std::uint64_t rhs_weight = 0;
  std::uint64_t lhs_overload = 0;
  std::uint64_t rhs_overload = 0;
  std::uint64_t lhs_vertices = 0;
  std::uint64_t rhs_vertices = 0;
  std::uint64_t semantic_order = 0;
  bool valid_blocks = false;

  [[nodiscard]] constexpr auto total_overload() const noexcept
      -> std::uint64_t {
    return saturating_add(lhs_overload, rhs_overload);
  }

};

[[nodiscard]] constexpr auto make_bipartition_candidate(
    std::uint64_t cut,
    std::uint64_t lhs_weight,
    std::uint64_t rhs_weight,
    bipartition_targets targets,
    std::uint64_t lhs_vertices,
    std::uint64_t rhs_vertices,
    bool partition_ids_are_valid,
    bool requires_two_nonempty_blocks,
    std::uint64_t semantic_order) noexcept -> bipartition_candidate {
  auto const valid_blocks =
      partition_ids_are_valid && (!requires_two_nonempty_blocks ||
                                  (lhs_vertices != 0 && rhs_vertices != 0));
  return bipartition_candidate{
      cut,
      lhs_weight,
      rhs_weight,
      overload(lhs_weight, targets.lhs),
      overload(rhs_weight, targets.rhs),
      lhs_vertices,
      rhs_vertices,
      semantic_order,
      valid_blocks,
  };
}

// A valid two-block partition always dominates a malformed candidate. The
// target weights guide region growth; they are not the final global balance
// constraint, which is verified after partitioning. Preserve KaHIP's cut-first
// candidate semantics, use total overload only to break equal-cut candidates,
// and retain the first trial when both quantities tie. This fixes the historic
// rhs-block-weight comparison without changing the established random stream.
[[nodiscard]] constexpr auto is_better_bipartition_candidate(
    bipartition_candidate const& challenger,
    bipartition_candidate const& incumbent) noexcept -> bool {
  if (challenger.valid_blocks != incumbent.valid_blocks) {
    return challenger.valid_blocks;
  }
  auto const selection_key = [](bipartition_candidate const& candidate) {
    return std::tuple{candidate.cut, candidate.total_overload(),
                      candidate.semantic_order};
  };
  return selection_key(challenger) < selection_key(incumbent);
}
}  // namespace kahip::initial_partitioning
