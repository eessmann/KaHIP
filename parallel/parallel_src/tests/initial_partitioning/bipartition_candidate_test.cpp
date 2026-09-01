#include <array>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string_view>

#include "partition/initial_partitioning/bipartition_candidate.h"

namespace {
namespace candidate = kahip::initial_partitioning;

void require(bool condition, std::string_view diagnostic) {
  if (!condition) {
    std::cerr << diagnostic << '\n';
    std::exit(EXIT_FAILURE);
  }
}
}  // namespace

int main() {
  auto const targets = candidate::bipartition_targets{100, 100};

  // This is the exact shape hidden by the historic
  // `lhs_overload + rhs_block_weight` typo: equal-cut challenger B has lower
  // total overload, but its raw RHS weight is necessarily larger than A's
  // overload and therefore could never replace A.
  auto const typo_incumbent = candidate::make_bipartition_candidate(
      7, 100, 110, targets, 3, 3, true, true, 0);
  auto const typo_challenger = candidate::make_bipartition_candidate(
      7, 100, 105, targets, 3, 3, true, true, 1);
  require(candidate::is_better_bipartition_candidate(typo_challenger,
                                                     typo_incumbent),
          "equal-cut candidates must compare total overload, not block weight");

  auto const lower_cut_infeasible = candidate::make_bipartition_candidate(
      1, 101, 100, targets, 3, 3, true, true, 0);
  auto const higher_cut_feasible = candidate::make_bipartition_candidate(
      2, 100, 100, targets, 3, 3, true, true, 1);
  require(candidate::is_better_bipartition_candidate(lower_cut_infeasible,
                                                     higher_cut_feasible),
          "growth targets must not replace KaHIP's cut-first objective");

  auto const lower_overload = candidate::make_bipartition_candidate(
      9, 103, 100, targets, 3, 3, true, true, 1);
  auto const lower_cut = candidate::make_bipartition_candidate(
      2, 104, 100, targets, 3, 3, true, true, 2);
  require(candidate::is_better_bipartition_candidate(lower_cut, lower_overload),
          "total overload must only break equal-cut candidates");

  auto const invalid_empty_block = candidate::make_bipartition_candidate(
      0, 100, 0, targets, 6, 0, true, true, 0);
  require(candidate::is_better_bipartition_candidate(higher_cut_feasible,
                                                     invalid_empty_block),
          "a mathematically invalid empty block must never beat a valid split");

  auto const tie_targets = candidate::bipartition_targets{110, 110};
  auto const balanced_tie = candidate::make_bipartition_candidate(
      2, 99, 101, tie_targets, 3, 3, true, true, 3);
  auto const imbalanced_tie = candidate::make_bipartition_candidate(
      2, 98, 102, tie_targets, 3, 3, true, true, 2);
  require(candidate::is_better_bipartition_candidate(imbalanced_tie,
                                                     balanced_tie),
          "equal cut and overload must retain deterministic trial order");

  auto const too_short = std::array<int, 1>{100};
  auto const negative = std::array<int, 2>{100, -1};
  auto const valid = std::array<int, 3>{100, 120, 999};
  require(!candidate::validated_bipartition_targets(too_short).has_value(),
          "one target weight must be rejected before indexing");
  require(!candidate::validated_bipartition_targets(negative).has_value(),
          "negative target weights must be rejected");
  require(candidate::validated_bipartition_targets(valid) ==
              candidate::bipartition_targets{100, 120},
          "the first two valid target weights must be preserved exactly");
}
