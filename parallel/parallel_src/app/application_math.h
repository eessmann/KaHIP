#pragma once

#include "../../shared/random_state.h"

namespace parhip::application {
[[nodiscard]] constexpr auto rank_seed(int base_seed,
                                       int process_count,
                                       int rank) noexcept
    -> std::optional<int> {
  return kahip::random_compat::outer_rank_seed(base_seed, process_count, rank);
}

using kahip::random_compat::checked_add;
using kahip::random_compat::exact_partition_upper_bound;
}  // namespace parhip::application
