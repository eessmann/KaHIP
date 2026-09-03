#include <mpi.h>

#include <catch2/catch_all.hpp>

#include <array>
#include <cstddef>
#include <functional>
#include <limits>
#include <ranges>
#include <string_view>
#include <utility>
#include <vector>

#include "data_structure/parallel_graph_access.h"
#include "distributed_partitioning/distributed_consistency.h"
#include "distributed_partitioning/distributed_partitioner.h"
#include "kahip_mpi_capabilities.h"

namespace consistency_probe {
enum class corruption {
  none,
  unknown_id,
  wrong_value,
  duplicate_replacing_missing,
  wrong_source,
};

inline bool active = false;
inline corruption payload_corruption = corruption::none;
inline bool corruption_fired = false;
inline int topology_create_calls = 0;
inline int count_exchange_calls = 0;
inline int payload_calls = 0;
inline int payload_c_calls = 0;
inline int point_to_point_calls = 0;
inline int immediate_neighbor_calls = 0;
inline int persistent_calls = 0;
inline int completion_calls = 0;
inline int barrier_calls = 0;

void reset(corruption mode = corruption::none) noexcept {
  payload_corruption = mode;
  corruption_fired = false;
  topology_create_calls = 0;
  count_exchange_calls = 0;
  payload_calls = 0;
  payload_c_calls = 0;
  point_to_point_calls = 0;
  immediate_neighbor_calls = 0;
  persistent_calls = 0;
  completion_calls = 0;
  barrier_calls = 0;
}

class activation final {
 public:
  explicit activation(corruption mode = corruption::none) noexcept {
    reset(mode);
    active = true;
  }
  ~activation() noexcept { active = false; }

  activation(activation const&) = delete;
  auto operator=(activation const&) -> activation& = delete;
};

template <typename Count, typename Displacement>
void corrupt_payload(void* receive_buffer,
                     Count const receive_counts[],
                     Displacement const receive_displacements[],
                     MPI_Comm communicator) noexcept {
  if (!active || payload_corruption == corruption::none ||
      receive_buffer == nullptr) {
    return;
  }
  auto rank = 0;
  auto indegree = 0;
  auto outdegree = 0;
  auto weighted = 0;
  if (PMPI_Comm_rank(communicator, &rank) != MPI_SUCCESS || rank != 1 ||
      PMPI_Dist_graph_neighbors_count(communicator, &indegree, &outdegree,
                                      &weighted) != MPI_SUCCESS ||
      indegree <= 0) {
    return;
  }

  auto first = -1;
  auto second = -1;
  for (auto index = 0; index < indegree; ++index) {
    if (receive_counts[index] > 0) {
      if (first < 0) {
        first = index;
      } else if (second < 0) {
        second = index;
      }
    }
  }
  if (first < 0) {
    return;
  }

  using record = parhip::distributed_consistency::node_value;
  auto* records = static_cast<record*>(receive_buffer);
  auto const first_offset =
      static_cast<std::size_t>(receive_displacements[first]);
  switch (payload_corruption) {
    case corruption::unknown_id:
      records[first_offset].global_id =
          std::numeric_limits<parhip::NodeID>::max();
      corruption_fired = true;
      break;
    case corruption::wrong_value:
      records[first_offset].value ^= parhip::NodeID{1};
      corruption_fired = true;
      break;
    case corruption::duplicate_replacing_missing:
      if (receive_counts[first] >= 2) {
        records[first_offset + 1] = records[first_offset];
        corruption_fired = true;
      }
      break;
    case corruption::wrong_source:
      if (second >= 0) {
        auto const second_offset =
            static_cast<std::size_t>(receive_displacements[second]);
        records[first_offset] = records[second_offset];
        corruption_fired = true;
      }
      break;
    case corruption::none:
      break;
  }
}
}  // namespace consistency_probe

extern "C" int MPI_Dist_graph_create(MPI_Comm communicator,
                                     int source_count,
                                     int const sources[],
                                     int const degrees[],
                                     int const destinations[],
                                     int const weights[],
                                     MPI_Info info,
                                     int reorder,
                                     MPI_Comm* graph_communicator) {
  if (consistency_probe::active) {
    ++consistency_probe::topology_create_calls;
  }
  return PMPI_Dist_graph_create(communicator, source_count, sources, degrees,
                                destinations, weights, info, reorder,
                                graph_communicator);
}

extern "C" int MPI_Neighbor_alltoall(void const* send_buffer,
                                     int send_count,
                                     MPI_Datatype send_datatype,
                                     void* receive_buffer,
                                     int receive_count,
                                     MPI_Datatype receive_datatype,
                                     MPI_Comm communicator) {
  if (consistency_probe::active) {
    ++consistency_probe::count_exchange_calls;
  }
  return PMPI_Neighbor_alltoall(send_buffer, send_count, send_datatype,
                                receive_buffer, receive_count, receive_datatype,
                                communicator);
}

extern "C" int MPI_Neighbor_alltoallv(void const* send_buffer,
                                      int const send_counts[],
                                      int const send_displacements[],
                                      MPI_Datatype send_datatype,
                                      void* receive_buffer,
                                      int const receive_counts[],
                                      int const receive_displacements[],
                                      MPI_Datatype receive_datatype,
                                      MPI_Comm communicator) {
  if (consistency_probe::active) {
    ++consistency_probe::payload_calls;
  }
  auto const result = PMPI_Neighbor_alltoallv(
      send_buffer, send_counts, send_displacements, send_datatype,
      receive_buffer, receive_counts, receive_displacements, receive_datatype,
      communicator);
  if (result == MPI_SUCCESS) {
    consistency_probe::corrupt_payload(receive_buffer, receive_counts,
                                       receive_displacements, communicator);
  }
  return result;
}

#if KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_C
extern "C" int MPI_Neighbor_alltoallv_c(void const* send_buffer,
                                        MPI_Count const send_counts[],
                                        MPI_Aint const send_displacements[],
                                        MPI_Datatype send_datatype,
                                        void* receive_buffer,
                                        MPI_Count const receive_counts[],
                                        MPI_Aint const receive_displacements[],
                                        MPI_Datatype receive_datatype,
                                        MPI_Comm communicator) {
  if (consistency_probe::active) {
    ++consistency_probe::payload_c_calls;
  }
  auto const result = PMPI_Neighbor_alltoallv_c(
      send_buffer, send_counts, send_displacements, send_datatype,
      receive_buffer, receive_counts, receive_displacements, receive_datatype,
      communicator);
  if (result == MPI_SUCCESS) {
    consistency_probe::corrupt_payload(receive_buffer, receive_counts,
                                       receive_displacements, communicator);
  }
  return result;
}
#endif

#define KAHIP_P2P_WRAPPER(name, signature, arguments) \
  extern "C" int name signature {                     \
    if (consistency_probe::active) {                  \
      ++consistency_probe::point_to_point_calls;      \
    }                                                 \
    return P##name arguments;                         \
  }

KAHIP_P2P_WRAPPER(MPI_Send,
                  (void const* buffer,
                   int count,
                   MPI_Datatype datatype,
                   int destination,
                   int tag,
                   MPI_Comm communicator),
                  (buffer, count, datatype, destination, tag, communicator))
KAHIP_P2P_WRAPPER(MPI_Ssend,
                  (void const* buffer,
                   int count,
                   MPI_Datatype datatype,
                   int destination,
                   int tag,
                   MPI_Comm communicator),
                  (buffer, count, datatype, destination, tag, communicator))
KAHIP_P2P_WRAPPER(MPI_Bsend,
                  (void const* buffer,
                   int count,
                   MPI_Datatype datatype,
                   int destination,
                   int tag,
                   MPI_Comm communicator),
                  (buffer, count, datatype, destination, tag, communicator))
KAHIP_P2P_WRAPPER(MPI_Rsend,
                  (void const* buffer,
                   int count,
                   MPI_Datatype datatype,
                   int destination,
                   int tag,
                   MPI_Comm communicator),
                  (buffer, count, datatype, destination, tag, communicator))
KAHIP_P2P_WRAPPER(
    MPI_Isend,
    (void const* buffer,
     int count,
     MPI_Datatype datatype,
     int destination,
     int tag,
     MPI_Comm communicator,
     MPI_Request* request),
    (buffer, count, datatype, destination, tag, communicator, request))
KAHIP_P2P_WRAPPER(
    MPI_Issend,
    (void const* buffer,
     int count,
     MPI_Datatype datatype,
     int destination,
     int tag,
     MPI_Comm communicator,
     MPI_Request* request),
    (buffer, count, datatype, destination, tag, communicator, request))
KAHIP_P2P_WRAPPER(
    MPI_Ibsend,
    (void const* buffer,
     int count,
     MPI_Datatype datatype,
     int destination,
     int tag,
     MPI_Comm communicator,
     MPI_Request* request),
    (buffer, count, datatype, destination, tag, communicator, request))
KAHIP_P2P_WRAPPER(
    MPI_Irsend,
    (void const* buffer,
     int count,
     MPI_Datatype datatype,
     int destination,
     int tag,
     MPI_Comm communicator,
     MPI_Request* request),
    (buffer, count, datatype, destination, tag, communicator, request))
KAHIP_P2P_WRAPPER(MPI_Irecv,
                  (void* buffer,
                   int count,
                   MPI_Datatype datatype,
                   int source,
                   int tag,
                   MPI_Comm communicator,
                   MPI_Request* request),
                  (buffer, count, datatype, source, tag, communicator, request))
KAHIP_P2P_WRAPPER(MPI_Recv,
                  (void* buffer,
                   int count,
                   MPI_Datatype datatype,
                   int source,
                   int tag,
                   MPI_Comm communicator,
                   MPI_Status* status),
                  (buffer, count, datatype, source, tag, communicator, status))
KAHIP_P2P_WRAPPER(
    MPI_Probe,
    (int source, int tag, MPI_Comm communicator, MPI_Status* status),
    (source, tag, communicator, status))
KAHIP_P2P_WRAPPER(
    MPI_Iprobe,
    (int source, int tag, MPI_Comm communicator, int* flag, MPI_Status* status),
    (source, tag, communicator, flag, status))
KAHIP_P2P_WRAPPER(MPI_Sendrecv,
                  (void const* send_buffer,
                   int send_count,
                   MPI_Datatype send_datatype,
                   int destination,
                   int send_tag,
                   void* receive_buffer,
                   int receive_count,
                   MPI_Datatype receive_datatype,
                   int source,
                   int receive_tag,
                   MPI_Comm communicator,
                   MPI_Status* status),
                  (send_buffer,
                   send_count,
                   send_datatype,
                   destination,
                   send_tag,
                   receive_buffer,
                   receive_count,
                   receive_datatype,
                   source,
                   receive_tag,
                   communicator,
                   status))
KAHIP_P2P_WRAPPER(MPI_Sendrecv_replace,
                  (void* buffer,
                   int count,
                   MPI_Datatype datatype,
                   int destination,
                   int send_tag,
                   int source,
                   int receive_tag,
                   MPI_Comm communicator,
                   MPI_Status* status),
                  (buffer,
                   count,
                   datatype,
                   destination,
                   send_tag,
                   source,
                   receive_tag,
                   communicator,
                   status))
KAHIP_P2P_WRAPPER(MPI_Mprobe,
                  (int source,
                   int tag,
                   MPI_Comm communicator,
                   MPI_Message* message,
                   MPI_Status* status),
                  (source, tag, communicator, message, status))
KAHIP_P2P_WRAPPER(MPI_Improbe,
                  (int source,
                   int tag,
                   MPI_Comm communicator,
                   int* flag,
                   MPI_Message* message,
                   MPI_Status* status),
                  (source, tag, communicator, flag, message, status))
KAHIP_P2P_WRAPPER(MPI_Mrecv,
                  (void* buffer,
                   int count,
                   MPI_Datatype datatype,
                   MPI_Message* message,
                   MPI_Status* status),
                  (buffer, count, datatype, message, status))
KAHIP_P2P_WRAPPER(MPI_Imrecv,
                  (void* buffer,
                   int count,
                   MPI_Datatype datatype,
                   MPI_Message* message,
                   MPI_Request* request),
                  (buffer, count, datatype, message, request))

#undef KAHIP_P2P_WRAPPER

extern "C" int MPI_Ineighbor_alltoallv(void const* send_buffer,
                                       int const send_counts[],
                                       int const send_displacements[],
                                       MPI_Datatype send_datatype,
                                       void* receive_buffer,
                                       int const receive_counts[],
                                       int const receive_displacements[],
                                       MPI_Datatype receive_datatype,
                                       MPI_Comm communicator,
                                       MPI_Request* request) {
  if (consistency_probe::active) {
    ++consistency_probe::immediate_neighbor_calls;
  }
  return PMPI_Ineighbor_alltoallv(send_buffer, send_counts, send_displacements,
                                  send_datatype, receive_buffer, receive_counts,
                                  receive_displacements, receive_datatype,
                                  communicator, request);
}

#if KAHIP_HAVE_MPI_INEIGHBOR_ALLTOALLV_C
extern "C" int MPI_Ineighbor_alltoallv_c(void const* send_buffer,
                                         MPI_Count const send_counts[],
                                         MPI_Aint const send_displacements[],
                                         MPI_Datatype send_datatype,
                                         void* receive_buffer,
                                         MPI_Count const receive_counts[],
                                         MPI_Aint const receive_displacements[],
                                         MPI_Datatype receive_datatype,
                                         MPI_Comm communicator,
                                         MPI_Request* request) {
  if (consistency_probe::active) {
    ++consistency_probe::immediate_neighbor_calls;
  }
  return PMPI_Ineighbor_alltoallv_c(
      send_buffer, send_counts, send_displacements, send_datatype,
      receive_buffer, receive_counts, receive_displacements, receive_datatype,
      communicator, request);
}
#endif

#if KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_INIT
extern "C" int MPI_Neighbor_alltoallv_init(void const* send_buffer,
                                           int const send_counts[],
                                           int const send_displacements[],
                                           MPI_Datatype send_datatype,
                                           void* receive_buffer,
                                           int const receive_counts[],
                                           int const receive_displacements[],
                                           MPI_Datatype receive_datatype,
                                           MPI_Comm communicator,
                                           MPI_Info info,
                                           MPI_Request* request) {
  if (consistency_probe::active) {
    ++consistency_probe::persistent_calls;
  }
  return PMPI_Neighbor_alltoallv_init(
      send_buffer, send_counts, send_displacements, send_datatype,
      receive_buffer, receive_counts, receive_displacements, receive_datatype,
      communicator, info, request);
}
#endif

#if KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_INIT_C
extern "C" int MPI_Neighbor_alltoallv_init_c(
    void const* send_buffer,
    MPI_Count const send_counts[],
    MPI_Aint const send_displacements[],
    MPI_Datatype send_datatype,
    void* receive_buffer,
    MPI_Count const receive_counts[],
    MPI_Aint const receive_displacements[],
    MPI_Datatype receive_datatype,
    MPI_Comm communicator,
    MPI_Info info,
    MPI_Request* request) {
  if (consistency_probe::active) {
    ++consistency_probe::persistent_calls;
  }
  return PMPI_Neighbor_alltoallv_init_c(
      send_buffer, send_counts, send_displacements, send_datatype,
      receive_buffer, receive_counts, receive_displacements, receive_datatype,
      communicator, info, request);
}
#endif

extern "C" int MPI_Start(MPI_Request* request) {
  if (consistency_probe::active) {
    ++consistency_probe::persistent_calls;
  }
  return PMPI_Start(request);
}

extern "C" int MPI_Startall(int count, MPI_Request requests[]) {
  if (consistency_probe::active) {
    ++consistency_probe::persistent_calls;
  }
  return PMPI_Startall(count, requests);
}

extern "C" int MPI_Test(MPI_Request* request,
                        int* complete,
                        MPI_Status* status) {
  if (consistency_probe::active) {
    ++consistency_probe::completion_calls;
  }
  return PMPI_Test(request, complete, status);
}

extern "C" int MPI_Wait(MPI_Request* request, MPI_Status* status) {
  if (consistency_probe::active) {
    ++consistency_probe::completion_calls;
  }
  return PMPI_Wait(request, status);
}

extern "C" int MPI_Waitall(int count,
                           MPI_Request requests[],
                           MPI_Status statuses[]) {
  if (consistency_probe::active) {
    ++consistency_probe::completion_calls;
  }
  return PMPI_Waitall(count, requests, statuses);
}

extern "C" int MPI_Testall(int count,
                           MPI_Request requests[],
                           int* complete,
                           MPI_Status statuses[]) {
  if (consistency_probe::active) {
    ++consistency_probe::completion_calls;
  }
  return PMPI_Testall(count, requests, complete, statuses);
}

extern "C" int MPI_Testany(int count,
                           MPI_Request requests[],
                           int* index,
                           int* complete,
                           MPI_Status* status) {
  if (consistency_probe::active) {
    ++consistency_probe::completion_calls;
  }
  return PMPI_Testany(count, requests, index, complete, status);
}

extern "C" int MPI_Testsome(int count,
                            MPI_Request requests[],
                            int* completed,
                            int indices[],
                            MPI_Status statuses[]) {
  if (consistency_probe::active) {
    ++consistency_probe::completion_calls;
  }
  return PMPI_Testsome(count, requests, completed, indices, statuses);
}

extern "C" int MPI_Waitany(int count,
                           MPI_Request requests[],
                           int* index,
                           MPI_Status* status) {
  if (consistency_probe::active) {
    ++consistency_probe::completion_calls;
  }
  return PMPI_Waitany(count, requests, index, status);
}

extern "C" int MPI_Waitsome(int count,
                            MPI_Request requests[],
                            int* completed,
                            int indices[],
                            MPI_Status statuses[]) {
  if (consistency_probe::active) {
    ++consistency_probe::completion_calls;
  }
  return PMPI_Waitsome(count, requests, completed, indices, statuses);
}

extern "C" int MPI_Request_free(MPI_Request* request) {
  if (consistency_probe::active) {
    ++consistency_probe::completion_calls;
  }
  return PMPI_Request_free(request);
}

extern "C" int MPI_Cancel(MPI_Request* request) {
  if (consistency_probe::active) {
    ++consistency_probe::completion_calls;
  }
  return PMPI_Cancel(request);
}

extern "C" int MPI_Barrier(MPI_Comm communicator) {
  if (consistency_probe::active) {
    ++consistency_probe::barrier_calls;
  }
  return PMPI_Barrier(communicator);
}

namespace {
using parhip::NodeID;
using parhip::parallel_graph_access;

struct graph_fixture {
  std::vector<NodeID> ranges;
  std::vector<std::vector<NodeID>> adjacency;
};

[[nodiscard]] auto normal_fixture(int size) -> graph_fixture {
  if (size == 1) {
    return {{0, 2}, std::vector<std::vector<NodeID>>(2)};
  }
  if (size == 2) {
    return {{0, 1, 2}, {{1, 1}, {0, 0}}};
  }
  if (size == 3) {
    return {{0, 1, 2, 2}, {{1, 1}, {0, 0}}};
  }
  if (size == 4) {
    return {{0, 1, 2, 3, 4}, {{3, 1}, {0, 2}, {1, 3}, {2, 0}}};
  }
  return {{0, 2, 3, 5, 6, 7}, {{1}, {0, 2}, {1, 3}, {2, 4}, {3, 5}, {4}, {}}};
}

[[nodiscard]] auto validation_fixture(int size) -> graph_fixture {
  if (size == 2) {
    return {{0, 2, 4}, {{2}, {3}, {0}, {1}}};
  }
  auto ranges = std::vector<NodeID>(static_cast<std::size_t>(size) + 1, 5);
  ranges[0] = 0;
  ranges[1] = 2;
  ranges[2] = 3;
  ranges[3] = 5;
  return {std::move(ranges), {{2}, {2}, {0, 1, 3, 4}, {2}, {2}}};
}

[[nodiscard]] auto one_way_fixture(int size) -> graph_fixture {
  auto ranges = std::vector<NodeID>(static_cast<std::size_t>(size) + 1, 2);
  ranges[0] = 0;
  ranges[1] = 1;
  ranges[2] = 2;
  return {std::move(ranges), {{1}, {}}};
}

void build_graph(parallel_graph_access& graph,
                 graph_fixture const& fixture,
                 int rank) {
  auto const first = fixture.ranges[static_cast<std::size_t>(rank)];
  auto const end = fixture.ranges[static_cast<std::size_t>(rank + 1)];
  auto local_edges = std::size_t{0};
  for (auto global = first; global < end; ++global) {
    local_edges += fixture.adjacency[static_cast<std::size_t>(global)].size();
  }
  auto const global_edges = std::ranges::fold_left(
      fixture.adjacency | std::views::transform(&std::vector<NodeID>::size),
      std::size_t{0}, std::plus<>{});
  graph.start_construction(end - first,
                           static_cast<parhip::EdgeID>(local_edges),
                           static_cast<NodeID>(fixture.adjacency.size()),
                           static_cast<parhip::EdgeID>(global_edges), false);
  graph.set_range(first, first == end ? first : end - 1);
  auto ranges = fixture.ranges;
  graph.set_range_array(ranges);

  for (auto global = first; global < end; ++global) {
    auto const local = graph.new_node();
    graph.setNodeWeight(local, 1);
    graph.setNodeLabel(local, NodeID{1000} + global);
    graph.setSecondPartitionIndex(local, NodeID{2000} + global);
    for (auto const target :
         fixture.adjacency[static_cast<std::size_t>(global)]) {
      auto const edge = graph.new_edge(local, target);
      graph.setEdgeWeight(edge, 1);
    }
  }
  graph.finish_construction();
  for (auto local = graph.number_of_local_nodes() + NodeID{1};
       local < graph.number_of_local_nodes() + NodeID{1} +
                   graph.number_of_ghost_nodes();
       ++local) {
    auto const global = graph.getGlobalID(local);
    graph.setNodeLabel(local, NodeID{1000} + global);
    graph.setSecondPartitionIndex(local, NodeID{2000} + global);
  }
}

void require_common(bool local_condition) {
  auto const local = local_condition ? 1 : 0;
  auto common = 0;
  REQUIRE(PMPI_Allreduce(&local, &common, 1, MPI_INT, MPI_MIN,
                         MPI_COMM_WORLD) == MPI_SUCCESS);
  REQUIRE(common == 1);
}

template <typename Operation>
void require_common_error(Operation&& operation,
                          std::string_view expected_context,
                          int size) {
  auto caught = 0;
  auto structured = 0;
  auto context_matches = 0;
  try {
    std::invoke(std::forward<Operation>(operation));
  } catch (parhip::mpi::mpi_error const& error) {
    caught = 1;
    structured = 1;
    context_matches =
        error.context().find(expected_context) != std::string_view::npos ? 1
                                                                         : 0;
  } catch (...) {
    caught = 1;
  }
  auto caught_total = 0;
  auto structured_total = 0;
  auto context_total = 0;
  REQUIRE(PMPI_Allreduce(&caught, &caught_total, 1, MPI_INT, MPI_SUM,
                         MPI_COMM_WORLD) == MPI_SUCCESS);
  REQUIRE(PMPI_Allreduce(&structured, &structured_total, 1, MPI_INT, MPI_SUM,
                         MPI_COMM_WORLD) == MPI_SUCCESS);
  REQUIRE(PMPI_Allreduce(&context_matches, &context_total, 1, MPI_INT, MPI_SUM,
                         MPI_COMM_WORLD) == MPI_SUCCESS);
  REQUIRE(caught_total == size);
  REQUIRE(structured_total == size);
  REQUIRE(context_total == size);
}

struct snapshot_entry {
  NodeID global_id;
  NodeID label;
  NodeID second;

  auto operator==(snapshot_entry const&) const -> bool = default;
};

[[nodiscard]] auto snapshot(parallel_graph_access& graph)
    -> std::vector<snapshot_entry> {
  auto result = std::vector<snapshot_entry>{};
  result.reserve(static_cast<std::size_t>(graph.number_of_local_nodes() +
                                          graph.number_of_ghost_nodes()));
  for (NodeID local = 0; local < graph.number_of_local_nodes(); ++local) {
    result.push_back({graph.getGlobalID(local), graph.getNodeLabel(local),
                      graph.getSecondPartitionIndex(local)});
  }
  for (auto local = graph.number_of_local_nodes() + NodeID{1};
       local < graph.number_of_local_nodes() + NodeID{1} +
                   graph.number_of_ghost_nodes();
       ++local) {
    result.push_back({graph.getGlobalID(local), graph.getNodeLabel(local),
                      graph.getSecondPartitionIndex(local)});
  }
  return result;
}

[[nodiscard]] auto protocol_is_blocking_collective(int expected_operations,
                                                   int expected_topologies)
    -> bool {
#if KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_C
  auto const payload_path_is_valid =
      consistency_probe::payload_calls == 0 &&
      consistency_probe::payload_c_calls == expected_operations;
#else
  auto const payload_path_is_valid =
      consistency_probe::payload_calls == expected_operations &&
      consistency_probe::payload_c_calls == 0;
#endif
  return consistency_probe::topology_create_calls == expected_topologies &&
         consistency_probe::count_exchange_calls == expected_operations &&
         payload_path_is_valid &&
         consistency_probe::point_to_point_calls == 0 &&
         consistency_probe::immediate_neighbor_calls == 0 &&
         consistency_probe::persistent_calls == 0 &&
         consistency_probe::completion_calls == 0 &&
         consistency_probe::barrier_calls == 0;
}
}  // namespace

TEST_CASE("consistency checks use one cached blocking neighborhood plan",
          "[unit][mpi][distributed-consistency][protocol]") {
  auto rank = 0;
  auto size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  parallel_graph_access graph{MPI_COMM_WORLD};
  build_graph(graph, normal_fixture(size), rank);
  parhip::distributed_partitioner partitioner;
  parhip::PPartitionConfig config;

  {
    auto probe = consistency_probe::activation{};
    partitioner.check_labels(MPI_COMM_WORLD, config, graph);
    partitioner.check(MPI_COMM_WORLD, config, graph);
    partitioner.check_labels(MPI_COMM_WORLD, config, graph);
    require_common(protocol_is_blocking_collective(3, 1));
  }
}

TEST_CASE("reconstruction invalidates exactly one cached topology",
          "[unit][mpi][distributed-consistency][rebuild]") {
  auto rank = 0;
  auto size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  parallel_graph_access graph{MPI_COMM_WORLD};
  build_graph(graph, normal_fixture(size), rank);
  parhip::distributed_partitioner partitioner;
  parhip::PPartitionConfig config;

  {
    auto probe = consistency_probe::activation{};
    partitioner.check_labels(MPI_COMM_WORLD, config, graph);
    build_graph(graph, normal_fixture(size), rank);
    partitioner.check(MPI_COMM_WORLD, config, graph);
    require_common(protocol_is_blocking_collective(2, 2));
  }
}

TEST_CASE("asymmetric graph plans fail before count or payload traffic",
          "[unit][mpi][distributed-consistency][asymmetric]") {
  auto rank = 0;
  auto size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  if (size == 1) {
    return;
  }
  parallel_graph_access graph{MPI_COMM_WORLD};
  build_graph(graph, one_way_fixture(size), rank);
  parhip::distributed_partitioner partitioner;
  parhip::PPartitionConfig config;

  {
    auto probe = consistency_probe::activation{};
    require_common_error(
        [&] { partitioner.check_labels(MPI_COMM_WORLD, config, graph); },
        "ghost exchange plan semantic validation", size);
    require_common(consistency_probe::topology_create_calls == 1 &&
                   consistency_probe::count_exchange_calls == 0 &&
                   consistency_probe::payload_calls == 0 &&
                   consistency_probe::payload_c_calls == 0 &&
                   consistency_probe::point_to_point_calls == 0);
  }
}

TEST_CASE("congruent communicators are accepted and similar ones rejected",
          "[unit][mpi][distributed-consistency][communicator]") {
  auto rank = 0;
  auto size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  parallel_graph_access graph{MPI_COMM_WORLD};
  build_graph(graph, normal_fixture(size), rank);
  parhip::distributed_partitioner partitioner;
  parhip::PPartitionConfig config;
  MPI_Comm duplicate = MPI_COMM_NULL;
  REQUIRE(MPI_Comm_dup(MPI_COMM_WORLD, &duplicate) == MPI_SUCCESS);
  partitioner.check_labels(duplicate, config, graph);
  REQUIRE(MPI_Comm_free(&duplicate) == MPI_SUCCESS);

  if (size > 1) {
    MPI_Comm similar = MPI_COMM_NULL;
    REQUIRE(MPI_Comm_split(MPI_COMM_WORLD, 0, size - rank, &similar) ==
            MPI_SUCCESS);
    {
      auto probe = consistency_probe::activation{};
      require_common_error(
          [&] { partitioner.check_labels(similar, config, graph); },
          "communicator", size);
      require_common(consistency_probe::count_exchange_calls == 0 &&
                     consistency_probe::payload_calls == 0 &&
                     consistency_probe::payload_c_calls == 0);
    }
    REQUIRE(MPI_Comm_free(&similar) == MPI_SUCCESS);
  }
}

TEST_CASE("received corruption is common mutation-free and recoverable",
          "[unit][mpi][distributed-consistency][validation]") {
  auto rank = 0;
  auto size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  if (size == 1) {
    return;
  }

  auto const modes =
      std::array{consistency_probe::corruption::unknown_id,
                 consistency_probe::corruption::wrong_value,
                 consistency_probe::corruption::duplicate_replacing_missing,
                 consistency_probe::corruption::wrong_source};
  for (auto const mode : modes) {
    if (mode == consistency_probe::corruption::wrong_source && size < 3) {
      continue;
    }
    parallel_graph_access graph{MPI_COMM_WORLD};
    build_graph(graph, validation_fixture(size), rank);
    parhip::distributed_partitioner partitioner;
    parhip::PPartitionConfig config;
    auto const before = snapshot(graph);
    auto const unknown = std::numeric_limits<NodeID>::max();
    require_common(!graph.find_ghost_local_id(unknown, 0).has_value());

    {
      auto probe = consistency_probe::activation{mode};
      auto const second_partition =
          mode == consistency_probe::corruption::wrong_value;
      require_common_error(
          [&] {
            if (second_partition) {
              partitioner.check(MPI_COMM_WORLD, config, graph);
            } else {
              partitioner.check_labels(MPI_COMM_WORLD, config, graph);
            }
          },
          second_partition ? "second-partition consistency"
                           : "label consistency",
          size);
      auto local_fired = consistency_probe::corruption_fired ? 1 : 0;
      auto fired = 0;
      REQUIRE(PMPI_Allreduce(&local_fired, &fired, 1, MPI_INT, MPI_MAX,
                             MPI_COMM_WORLD) == MPI_SUCCESS);
      require_common(fired == 1 && snapshot(graph) == before &&
                     !graph.find_ghost_local_id(unknown, 0).has_value());

      consistency_probe::payload_corruption =
          consistency_probe::corruption::none;
      if (second_partition) {
        partitioner.check(MPI_COMM_WORLD, config, graph);
      } else {
        partitioner.check_labels(MPI_COMM_WORLD, config, graph);
      }
      require_common(protocol_is_blocking_collective(2, 1));
    }
  }
}
