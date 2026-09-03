#ifndef KAHIP_RANGE_OWNER_H
#define KAHIP_RANGE_OWNER_H

#include <algorithm>
#include <cstddef>
#include <ranges>

namespace kahip::range_owner {
template <std::ranges::random_access_range Boundaries, typename Node>
[[nodiscard]] constexpr auto from_boundaries(Boundaries const& boundaries,
                                             Node node) noexcept -> int {
  auto const begin = std::ranges::begin(boundaries);
  auto const end = std::ranges::end(boundaries);
  if (begin == end) {
    return -1;
  }
  auto const first_boundary = begin + 1;
  auto const owner_end = std::ranges::upper_bound(first_boundary, end, node);
  if (owner_end == end) {
    return -1;
  }
  return static_cast<int>(owner_end - begin - 1);
}
}  // namespace kahip::range_owner

#endif
