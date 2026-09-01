#include <parhip_interface.h>

int parhip_c_constants_are_valid(void);

int main(void) {
  void (*partition_function)(idxtype*, idxtype*, idxtype*, idxtype*,
                             idxtype*, int*, double*, bool, int, int, int*,
                             idxtype*, MPI_Comm*) = ParHIPPartitionKWay;

  return parhip_c_constants_are_valid() && partition_function != 0 ? 0 : 1;
}
