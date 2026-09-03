/******************************************************************************
 * dspac.h
 * *
 * Source of KaHIP -- Karlsruhe High Quality Partitioning.
 * Author: Daniel Seemaier <daniel.seemaier@student.kit.edu>
 * Christian Schulz <christian.schulz.phone@gmail.com>
 *****************************************************************************/

#ifndef KAHIP_DSPAC_H
#define KAHIP_DSPAC_H

#include <vector>

#include "communication/mpi_collectives.h"
#include "data_structure/parallel_graph_access.h"
#include "definitions.h"

namespace parhip {
class dspac {
 public:
  dspac(parallel_graph_access& graph,
        MPI_Comm comm,
        EdgeWeight infinity,
        mpi::collective_options collective_options = {});
  void construct(parallel_graph_access& split_graph);
  std::vector<PartitionID> project_partition(
      parallel_graph_access& split_graph,
      std::vector<EdgeID> const& permutation);
  EdgeWeight calculate_vertex_cut(
      PartitionID k,
      std::vector<PartitionID> const& edge_partition);
  void fix_cut_dominant_edges(parallel_graph_access& split_graph);

 private:
  bool assert_sanity_checks(parallel_graph_access& split_graph);

  void internal_construct(parallel_graph_access& split_graph,
                          mpi::communicator_view communicator);

  MPI_Comm m_comm;
  EdgeWeight m_infinity;
  parallel_graph_access& m_input_graph;
  mpi::collective_options m_collective_options;
};
}  // namespace parhip
#endif  // KAHIP_DSPAC_H
