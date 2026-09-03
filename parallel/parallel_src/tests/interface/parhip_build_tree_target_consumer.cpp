#include <parhip_interface.h>

int main() {
  auto initialized = 0;
  return MPI_Initialized(&initialized) == MPI_SUCCESS && initialized == 0 ? 0
                                                                          : 1;
}
