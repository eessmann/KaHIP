/******************************************************************************
 * parallel_block_down_propagation.h
 * *
 * Source of KaHIP -- Karlsruhe High Quality Partitioning.
 * Christian Schulz <christian.schulz.phone@gmail.com>
 *****************************************************************************/

#ifndef PARALLEL_BLOCK_DOWN_PROPAGATION_SRTCMH8F
#define PARALLEL_BLOCK_DOWN_PROPAGATION_SRTCMH8F

#include <tuple>
#include <type_traits>

#include "communication/mpi_types.h"
#include "data_structure/parallel_graph_access.h"
#include "partition_config.h"
namespace parhip {
namespace block_down {
struct block_update {
  NodeID coarse_global_id;
  PartitionID block;
};
}  // namespace block_down

static_assert(std::is_standard_layout_v<block_down::block_update>);
static_assert(std::is_trivially_copyable_v<block_down::block_update>);

class parallel_block_down_propagation {
 public:
  void propagate_block_down(MPI_Comm communicator,
                            PPartitionConfig& config,
                            parallel_graph_access& G,
                            parallel_graph_access& Q);
};
}  // namespace parhip

template <>
struct parhip::mpi::wire_members<parhip::block_down::block_update> {
  inline static constexpr auto value = std::tuple{
      &parhip::block_down::block_update::coarse_global_id,
      &parhip::block_down::block_update::block};
};

#endif /* end of include guard: PARALLEL_BLOCK_DOWN_PROPAGATION_SRTCMH8F */
