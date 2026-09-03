#include <parhip_interface.h>
#include <kaHIP_interface.h>

int combined_reverse_c_constants_are_valid(void) {
  return KAHIP_FASTSOCIAL == 3 && KAHIP_ECOSOCIAL == 4 &&
         PARHIP_FASTSOCIAL == 4 && PARHIP_ECOSOCIAL == 5;
}
