#ifndef KAHIP_SERIAL_KERNEL_BRIDGE_H
#define KAHIP_SERIAL_KERNEL_BRIDGE_H

#include <span>

namespace kahip::serial_kernel {
[[nodiscard]] inline auto solve_trivial_single_block(int block_count,
                                                      std::span<int> partition,
                                                      int& edgecut,
                                                      double& balance) noexcept
    -> bool {
  if (block_count != 1) {
    return false;
  }
  for (auto& label : partition) {
    label = 0;
  }
  edgecut = 0;
  balance = 1.0;
  return true;
}
}  // namespace kahip::serial_kernel

#endif
