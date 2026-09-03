#include <kaHIP_interface.h>

int main(void) {
  return kahip_sizeof_idx() == (int)sizeof(kahip_idx) ? 0 : 1;
}
