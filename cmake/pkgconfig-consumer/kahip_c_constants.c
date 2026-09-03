#include <kaHIP_interface.h>

int kahip_c_constants_are_valid(void) {
  return FAST == 0 && ECO == 1 && STRONG == 2 &&
         KAHIP_FASTSOCIAL == 3 && KAHIP_ECOSOCIAL == 4 &&
         STRONGSOCIAL == 5 && MAPMODE_MULTISECTION == 0 &&
         MAPMODE_BISECTION == 1;
}
