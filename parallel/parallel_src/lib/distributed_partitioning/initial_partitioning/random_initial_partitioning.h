/******************************************************************************
 * random_initial_partitioning.h
 * *
 * Source of KaHIP -- Karlsruhe High Quality Partitioning.
 * Christian Schulz <christian.schulz.phone@gmail.com>
 *****************************************************************************/

#ifndef RANDOM_INITIAL_PARTITIONING_FM8LJSI0
#define RANDOM_INITIAL_PARTITIONING_FM8LJSI0

#include "communication/mpi_handles.h"
#include "partition_config.h"
namespace parhip {
class parallel_graph_access;

class random_initial_partitioning {
public:
  random_initial_partitioning();
  virtual ~random_initial_partitioning();

  void perform_partitioning(
      mpi::communicator_view communicator,
      PPartitionConfig& config,
      parallel_graph_access& G);
};
}

#endif /* end of include guard: RANDOM_INITIAL_PARTITIONING_FM8LJSI0 */
