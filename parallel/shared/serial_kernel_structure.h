#ifndef KAHIP_SERIAL_KERNEL_STRUCTURE_H
#define KAHIP_SERIAL_KERNEL_STRUCTURE_H

#include <algorithm>
#include <compare>
#include <cstdint>
#include <vector>

namespace kahip::serial_kernel {
struct directed_arc final {
  std::uint64_t source{};
  std::uint64_t target{};
  std::uint64_t weight{};

  [[nodiscard]] constexpr auto operator<=>(directed_arc const&) const = default;
};

[[nodiscard]] inline auto is_loop_free_reciprocal_undirected(
    std::vector<directed_arc> arcs) -> bool {
  for (auto const& arc : arcs) {
    if (arc.source == arc.target) {
      return false;
    }
  }
  std::ranges::sort(arcs);
  for (auto first = std::size_t{0}; first < arcs.size();) {
    auto last = first + 1;
    while (last < arcs.size() && arcs[last] == arcs[first]) {
      ++last;
    }
    auto const reverse = directed_arc{.source = arcs[first].target,
                                      .target = arcs[first].source,
                                      .weight = arcs[first].weight};
    auto const reverse_first = std::ranges::lower_bound(arcs, reverse);
    if (reverse_first == arcs.end() || *reverse_first != reverse) {
      return false;
    }
    auto reverse_last = reverse_first;
    while (reverse_last != arcs.end() && *reverse_last == reverse) {
      ++reverse_last;
    }
    if (static_cast<std::size_t>(reverse_last - reverse_first) != last - first) {
      return false;
    }
    first = last;
  }
  return true;
}
}  // namespace kahip::serial_kernel

#endif
