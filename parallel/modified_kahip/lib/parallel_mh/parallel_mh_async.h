/******************************************************************************
 * parallel_mh_async.h
 * *
 * Source of KaHIP -- Karlsruhe High Quality Partitioning.
 * Christian Schulz <christian.schulz.phone@gmail.com>
 *****************************************************************************/

#ifndef PARALLEL_MH_ASYNC_HF106Y0G
#define PARALLEL_MH_ASYNC_HF106Y0G

#include <mpi.h>

#include <memory>

#include "data_structure/graph_access.h"
#include "partition_config.h"
#include "population.h"
#include "timer.h"
namespace kahip::parallel_mh {
class owned_evolutionary_communicator;
}
namespace kahip::modified {
class parallel_mh_async final {
 public:
  parallel_mh_async();
  explicit parallel_mh_async(MPI_Comm communicator);
  ~parallel_mh_async();

  parallel_mh_async(parallel_mh_async const&) = delete;
  auto operator=(parallel_mh_async const&) -> parallel_mh_async& = delete;
  parallel_mh_async(parallel_mh_async&&) = delete;
  auto operator=(parallel_mh_async&&) -> parallel_mh_async& = delete;

  void perform_partitioning(PartitionConfig const& graph_partitioner_config,
                            graph_access& G);
  void initialize(PartitionConfig& graph_partitioner_config, graph_access& G);
  EdgeWeight perform_local_partitioning(
      PartitionConfig& graph_partitioner_config,
      graph_access& G);
  EdgeWeight collect_best_partitioning(graph_access& G,
                                       PartitionConfig const& config);
  void perform_cycle_partitioning(PartitionConfig& graph_partitioner_config,
                                  graph_access& G);

 private:
  std::unique_ptr<::kahip::parallel_mh::owned_evolutionary_communicator>
      m_communicator;
  timer m_t;
  int m_rank;
  int m_size;
  double m_time_limit = 0.0;
  unsigned m_rounds = 0;
  std::unique_ptr<population> m_island;
};
}  // namespace kahip::modified

#endif /* end of include guard: PARALLEL_MH_ASYNC_HF106Y0G */
