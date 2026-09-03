#include <kaHIP_interface.h>
#include <parhip_interface.h>

int combined_reverse_c_constants_are_valid(void);

int main(void) {
  int (*serial_size_function)(void) = kahip_sizeof_idx;
  void (*parallel_partition_function)(
      idxtype*, idxtype*, idxtype*, idxtype*, idxtype*, int*, double*, bool,
      int, int, int*, idxtype*, MPI_Comm*) = ParHIPPartitionKWay;

  return KAHIP_FASTSOCIAL == 3 && KAHIP_ECOSOCIAL == 4 &&
                 PARHIP_FASTSOCIAL == 4 && PARHIP_ECOSOCIAL == 5 &&
                 combined_reverse_c_constants_are_valid() &&
                 serial_size_function != 0 &&
                 parallel_partition_function != 0
             ? 0
             : 1;
}
