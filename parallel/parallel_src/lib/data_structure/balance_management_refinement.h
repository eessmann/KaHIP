/******************************************************************************
 * balance_management_refinement.h
 * *
 * Source of KaHIP -- Karlsruhe High Quality Partitioning.
 * Christian Schulz <christian.schulz.phone@gmail.com>
 *****************************************************************************/

#ifndef BALANCE_MANAGEMENT_REFINEMENT_ZHYKQBYB
#define BALANCE_MANAGEMENT_REFINEMENT_ZHYKQBYB

#include "balance_management.h"
#include "communication/mpi_handles.h"
namespace parhip {
class parallel_graph_access;

class balance_management_refinement : public balance_management {
 public:
  balance_management_refinement(parallel_graph_access* graph,
                                PartitionID total_num_labels);
  ~balance_management_refinement() override;

  [[nodiscard]] auto getBlockSize(PartitionID block) -> NodeWeight override;
  void setBlockSize(PartitionID block, NodeWeight block_size) override;
  void update_non_contained_block_balance(PartitionID,
                                          PartitionID,
                                          NodeWeight) override {}

  void init() override;
  void update() override;

 private:
  void init(mpi::communicator_view communicator);
  void update(mpi::communicator_view communicator);

  std::vector<NodeWeight> m_total_block_weights;
  std::vector<NodeWeight> m_local_block_weights;
};
}  // namespace parhip
#endif  // BALANCE_MANAGEMENT_REFINEMENT_ZHYKQBYB
