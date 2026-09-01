#include <kaHIP_interface.h>

int kahip_c_constants_are_valid(void);

int main(void) {
  void (*edge_partitioning_function)(
      int*, int*, kahip_idx*, kahip_idx*, kahip_idx*, int*, double*, bool,
      int, int, int*, int*, kahip_idx) = edge_partitioning;

  return kahip_sizeof_idx() == (int)sizeof(kahip_idx) &&
                 kahip_c_constants_are_valid() &&
                 edge_partitioning_function != 0
             ? 0
             : 1;
}
