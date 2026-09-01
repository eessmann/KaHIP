#include <kaHIP_interface.h>
#include <parhip_interface.h>

namespace {
using parhip_function = decltype(&ParHIPPartitionKWay);
parhip_function volatile parhip_partition = &ParHIPPartitionKWay;
}  // namespace

auto main() -> int {
  auto const serial_api_is_valid =
      kahip_sizeof_idx() == static_cast<int>(sizeof(kahip_idx));
  auto const social_modes_are_unambiguous =
      KAHIP_FASTSOCIAL == 3 && KAHIP_ECOSOCIAL == 4 &&
      PARHIP_FASTSOCIAL == 4 && PARHIP_ECOSOCIAL == 5;
  return serial_api_is_valid && social_modes_are_unambiguous &&
                 parhip_partition != nullptr
             ? 0
             : 1;
}
