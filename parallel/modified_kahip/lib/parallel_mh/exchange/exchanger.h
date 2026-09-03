/******************************************************************************
 * exchanger.h
 * *
 * Source of KaHIP -- Karlsruhe High Quality Partitioning.
 * Christian Schulz <christian.schulz.phone@gmail.com>
 *****************************************************************************/

#ifndef EXCHANGER_YPB6QKNL
#define EXCHANGER_YPB6QKNL

#include <mpi.h>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

#include "data_structure/graph_access.h"
#include "parallel_mh/population.h"
#include "partition_config.h"
#include "tools/quality_metrics.h"
namespace kahip::modified {
class exchanger final {
 public:
  explicit exchanger(MPI_Comm communicator);
  ~exchanger() noexcept;

  exchanger(exchanger const&) = delete;
  auto operator=(exchanger const&) -> exchanger& = delete;
  exchanger(exchanger&&) = delete;
  auto operator=(exchanger&&) -> exchanger& = delete;

  void diversify_population(PartitionConfig& config,
                            graph_access& graph,
                            population& island,
                            bool replace);
  void quick_start(PartitionConfig& config,
                   graph_access& graph,
                   population& island);
  void push_best(PartitionConfig& config,
                 graph_access& graph,
                 population& island);
  void recv_incoming(PartitionConfig& config,
                     graph_access& graph,
                     population& island);
  void finish(std::size_t graph_order);

 private:
  struct pending_send final {
    std::vector<int> payload;
    MPI_Request request = MPI_REQUEST_NULL;
    MPI_Comm communicator = MPI_COMM_NULL;

    pending_send(std::vector<int> values,
                 MPI_Request handle,
                 MPI_Comm failure_communicator) noexcept
        : payload(std::move(values)),
          request(handle),
          communicator(failure_communicator) {}
    pending_send(pending_send const&) = delete;
    auto operator=(pending_send const&) -> pending_send& = delete;
    pending_send(pending_send&& other) noexcept
        : payload(std::move(other.payload)),
          request(std::exchange(other.request, MPI_REQUEST_NULL)),
          communicator(std::exchange(other.communicator, MPI_COMM_NULL)) {}
    auto operator=(pending_send&& other) noexcept -> pending_send&;
  };

  void exchange_individum(PartitionConfig const& config,
                          graph_access& graph,
                          int source,
                          int destination,
                          Individuum& input,
                          Individuum& output);
  [[nodiscard]] auto observe_graph_order(std::size_t graph_order,
                                         std::string_view operation) -> int;
  void validate_partition_status(MPI_Status const& status,
                                 int expected_source,
                                 int expected_tag,
                                 int expected_count,
                                 std::string_view operation) const;
  void receive_available(PartitionConfig& config,
                         graph_access& graph,
                         population& island,
                         MPI_Status const& probe_status,
                         int graph_count);
  void retire_completed_sends();

  std::vector<pending_send> m_pending_sends;
  std::vector<bool> m_already_sent_to;
  std::vector<std::uint64_t> m_issued_sends;
  std::vector<std::uint64_t> m_consumed_receives;

  EdgeWeight m_prev_best_objective;
  int m_max_num_pushes;
  int m_cur_num_pushes = 0;
  int m_rank;
  int m_size;
  std::size_t m_graph_order = 0;
  bool m_graph_order_observed = false;
  bool m_finished = false;

  MPI_Comm m_communicator;

  quality_metrics m_qm;
};
}  // namespace kahip::modified

#endif /* end of include guard: EXCHANGER_YPB6QKNL */
