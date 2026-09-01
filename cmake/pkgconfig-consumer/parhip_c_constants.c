#include <parhip_interface.h>

int parhip_c_constants_are_valid(void) {
  return ULTRAFASTMESH == 0 && FASTMESH == 1 && ECOMESH == 2 &&
         ULTRAFASTSOCIAL == 3 && PARHIP_FASTSOCIAL == 4 &&
         PARHIP_ECOSOCIAL == 5;
}
