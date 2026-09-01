#include <mpi.h>

#include <catch2/catch_all.hpp>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <numeric>
#include <ranges>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "communication/ghost_label_update.h"
#include "communication/mpi_error.h"
#include "communication/mpi_trace.h"
#include "data_structure/parallel_graph_access.h"
#include "kahip_mpi_capabilities.h"
#include "partition_config.h"

// KAHIP_PMPI_CALLBACK_REGION_BEGIN
namespace ghost_label_probe {
enum class corruption {
  none,
  unknown_id,
  wrong_source,
};

struct counters {
  int topology_creations = 0;
  int count_exchanges = 0;
  int blocking_payloads = 0;
  int immediate_payloads = 0;
  int completions = 0;
  int point_to_point_calls = 0;
  int barriers = 0;
  std::uint64_t immediate_records = 0;
  bool callback_error = false;

  auto operator==(counters const&) const -> bool = default;
};

using wire_record = parhip::ghost_label_update;

inline bool active = false;
inline corruption corruption_mode = corruption::none;
inline bool corruption_fired = false;
inline counters observed{};

void reset(corruption mode = corruption::none) noexcept {
  corruption_mode = mode;
  corruption_fired = false;
  observed = {};
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
void corrupt_neighbor_payload(void* receive_buffer,
                              Count const receive_counts[],
                              Displacement const receive_displacements[],
                              MPI_Comm communicator) noexcept {
  if (!active || corruption_mode == corruption::none || corruption_fired ||
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
      indegree == 0) {
    return;
  }

  auto first = -1;
  auto second = -1;
  for (auto index = 0; index < indegree; ++index) {
    if (receive_counts[index] <= 0) {
      continue;
    }
    if (first < 0) {
      first = index;
    } else if (second < 0) {
      second = index;
    }
  }
  if (first < 0) {
    return;
  }

  auto* records = static_cast<wire_record*>(receive_buffer);
  auto const first_offset =
      static_cast<std::size_t>(receive_displacements[first]);
  if (corruption_mode == corruption::unknown_id) {
    records[first_offset].global_id =
        std::numeric_limits<parhip::NodeID>::max();
    corruption_fired = true;
    return;
  }
  if (second >= 0) {
    auto const second_offset =
        static_cast<std::size_t>(receive_displacements[second]);
    records[first_offset].global_id = records[second_offset].global_id;
    corruption_fired = true;
  }
}

void corrupt_legacy_payload(void* receive_buffer,
                            int count,
                            int source,
                            MPI_Comm communicator) noexcept {
  if (!active || corruption_mode == corruption::none || corruption_fired ||
      receive_buffer == nullptr || count < 2) {
    return;
  }
  auto rank = 0;
  auto size = 0;
  if (PMPI_Comm_rank(communicator, &rank) != MPI_SUCCESS ||
      PMPI_Comm_size(communicator, &size) != MPI_SUCCESS || rank != 1) {
    return;
  }
  auto* words = static_cast<parhip::NodeID*>(receive_buffer);
  if (corruption_mode == corruption::unknown_id) {
    words[0] = std::numeric_limits<parhip::NodeID>::max();
    corruption_fired = true;
    return;
  }
  if (size >= 3 && (source == 0 || source == 2)) {
    words[0] = static_cast<parhip::NodeID>(source == 0 ? 2 : 0);
    corruption_fired = true;
  }
}

template <typename Count>
void observe_immediate_payload(Count const send_counts[],
                               MPI_Comm communicator) noexcept {
  if (!active) {
    return;
  }
  auto indegree = 0;
  auto outdegree = 0;
  auto weighted = 0;
  if (PMPI_Dist_graph_neighbors_count(communicator, &indegree, &outdegree,
                                      &weighted) != MPI_SUCCESS ||
      outdegree < 0 || (outdegree != 0 && send_counts == nullptr)) {
    observed.callback_error = true;
    return;
  }
  for (auto index = 0; index < outdegree; ++index) {
    if (send_counts[index] < 0 ||
        !std::in_range<std::uint64_t>(send_counts[index]) ||
        observed.immediate_records >
            std::numeric_limits<std::uint64_t>::max() -
                static_cast<std::uint64_t>(send_counts[index])) {
      observed.callback_error = true;
      return;
    }
    observed.immediate_records +=
        static_cast<std::uint64_t>(send_counts[index]);
  }
}
}  // namespace ghost_label_probe

static_assert(noexcept(
    ghost_label_probe::corrupt_neighbor_payload<int, int>(nullptr,
                                                          nullptr,
                                                          nullptr,
                                                          MPI_COMM_NULL)));
static_assert(
    noexcept(ghost_label_probe::corrupt_neighbor_payload<MPI_Count, MPI_Aint>(
        nullptr,
        nullptr,
        nullptr,
        MPI_COMM_NULL)));
static_assert(noexcept(
    ghost_label_probe::corrupt_legacy_payload(nullptr, 0, 0, MPI_COMM_NULL)));
static_assert(noexcept(
    ghost_label_probe::observe_immediate_payload<int>(nullptr, MPI_COMM_NULL)));
static_assert(noexcept(
    ghost_label_probe::observe_immediate_payload<MPI_Count>(nullptr,
                                                            MPI_COMM_NULL)));

extern "C" int MPI_Dist_graph_create(MPI_Comm communicator,
                                     int source_count,
                                     int const sources[],
                                     int const degrees[],
                                     int const destinations[],
                                     int const weights[],
                                     MPI_Info info,
                                     int reorder,
                                     MPI_Comm* graph_communicator) {
  if (ghost_label_probe::active) {
    ++ghost_label_probe::observed.topology_creations;
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
  if (ghost_label_probe::active) {
    ++ghost_label_probe::observed.count_exchanges;
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
  if (ghost_label_probe::active) {
    ++ghost_label_probe::observed.blocking_payloads;
  }
  auto const result = PMPI_Neighbor_alltoallv(
      send_buffer, send_counts, send_displacements, send_datatype,
      receive_buffer, receive_counts, receive_displacements, receive_datatype,
      communicator);
  if (result == MPI_SUCCESS) {
    ghost_label_probe::corrupt_neighbor_payload(
        receive_buffer, receive_counts, receive_displacements, communicator);
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
  if (ghost_label_probe::active) {
    ++ghost_label_probe::observed.blocking_payloads;
  }
  auto const result = PMPI_Neighbor_alltoallv_c(
      send_buffer, send_counts, send_displacements, send_datatype,
      receive_buffer, receive_counts, receive_displacements, receive_datatype,
      communicator);
  if (result == MPI_SUCCESS) {
    ghost_label_probe::corrupt_neighbor_payload(
        receive_buffer, receive_counts, receive_displacements, communicator);
  }
  return result;
}
#endif

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
  if (ghost_label_probe::active) {
    ++ghost_label_probe::observed.immediate_payloads;
    ghost_label_probe::observe_immediate_payload(send_counts, communicator);
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
  if (ghost_label_probe::active) {
    ++ghost_label_probe::observed.immediate_payloads;
    ghost_label_probe::observe_immediate_payload(send_counts, communicator);
  }
  return PMPI_Ineighbor_alltoallv_c(
      send_buffer, send_counts, send_displacements, send_datatype,
      receive_buffer, receive_counts, receive_displacements, receive_datatype,
      communicator, request);
}
#endif

extern "C" int MPI_Isend(void const* buffer,
                         int count,
                         MPI_Datatype datatype,
                         int destination,
                         int tag,
                         MPI_Comm communicator,
                         MPI_Request* request) {
  if (ghost_label_probe::active) {
    ++ghost_label_probe::observed.point_to_point_calls;
  }
  return PMPI_Isend(buffer, count, datatype, destination, tag, communicator,
                    request);
}

extern "C" int MPI_Probe(int source,
                         int tag,
                         MPI_Comm communicator,
                         MPI_Status* status) {
  if (ghost_label_probe::active) {
    ++ghost_label_probe::observed.point_to_point_calls;
  }
  return PMPI_Probe(source, tag, communicator, status);
}

extern "C" int MPI_Get_count(MPI_Status const* status,
                             MPI_Datatype datatype,
                             int* count) {
  if (ghost_label_probe::active) {
    ++ghost_label_probe::observed.point_to_point_calls;
  }
  return PMPI_Get_count(status, datatype, count);
}

extern "C" int MPI_Recv(void* buffer,
                        int count,
                        MPI_Datatype datatype,
                        int source,
                        int tag,
                        MPI_Comm communicator,
                        MPI_Status* status) {
  if (ghost_label_probe::active) {
    ++ghost_label_probe::observed.point_to_point_calls;
  }
  auto const result =
      PMPI_Recv(buffer, count, datatype, source, tag, communicator, status);
  if (result == MPI_SUCCESS) {
    ghost_label_probe::corrupt_legacy_payload(buffer, count, source,
                                              communicator);
  }
  return result;
}

extern "C" int MPI_Wait(MPI_Request* request, MPI_Status* status) {
  if (ghost_label_probe::active) {
    ++ghost_label_probe::observed.completions;
  }
  return PMPI_Wait(request, status);
}

extern "C" int MPI_Barrier(MPI_Comm communicator) {
  if (ghost_label_probe::active) {
    ++ghost_label_probe::observed.barriers;
  }
  return PMPI_Barrier(communicator);
}
// KAHIP_PMPI_CALLBACK_REGION_END

namespace {
using parhip::NodeID;
using parhip::parallel_graph_access;

struct graph_fixture {
  std::vector<NodeID> ranges;
  std::vector<std::vector<NodeID>> adjacency;
};

[[nodiscard]] auto ring_fixture(int size) -> graph_fixture {
  auto ranges = std::vector<NodeID>(static_cast<std::size_t>(size) + 1);
  std::ranges::iota(ranges, NodeID{0});
  auto adjacency =
      std::vector<std::vector<NodeID>>(static_cast<std::size_t>(size));
  if (size > 1) {
    for (auto rank = 0; rank < size; ++rank) {
      auto& neighbors = adjacency[static_cast<std::size_t>(rank)];
      neighbors = {static_cast<NodeID>((rank + size - 1) % size),
                   static_cast<NodeID>((rank + 1) % size)};
      std::ranges::sort(neighbors);
      auto const unique_end = std::ranges::unique(neighbors);
      neighbors.erase(unique_end.begin(), unique_end.end());
    }
  }
  return {std::move(ranges), std::move(adjacency)};
}

[[nodiscard]] auto zero_local_fixture(int size) -> graph_fixture {
  auto ranges = std::vector<NodeID>(static_cast<std::size_t>(size) + 1, 2);
  ranges[0] = 0;
  ranges[1] = 1;
  return {std::move(ranges), {{1}, {0}}};
}

[[nodiscard]] auto multi_node_ring_fixture(int size, NodeID nodes_per_rank)
    -> graph_fixture {
  auto ranges = std::vector<NodeID>(static_cast<std::size_t>(size) + 1);
  std::ranges::transform(
      std::views::iota(0, size + 1), ranges.begin(),
      [&](int rank) { return static_cast<NodeID>(rank) * nodes_per_rank; });
  auto adjacency = std::vector<std::vector<NodeID>>(
      static_cast<std::size_t>(size) * nodes_per_rank);
  if (size > 1) {
    for (auto rank = 0; rank < size; ++rank) {
      for (NodeID lane = 0; lane < nodes_per_rank; ++lane) {
        auto& neighbors = adjacency[static_cast<std::size_t>(
            static_cast<NodeID>(rank) * nodes_per_rank + lane)];
        neighbors = {
            static_cast<NodeID>((rank + size - 1) % size) * nodes_per_rank +
                lane,
            static_cast<NodeID>((rank + 1) % size) * nodes_per_rank + lane,
        };
        std::ranges::sort(neighbors);
        auto const unique_end = std::ranges::unique(neighbors);
        neighbors.erase(unique_end.begin(), unique_end.end());
      }
    }
  }
  return {std::move(ranges), std::move(adjacency)};
}

void build_graph(parallel_graph_access& graph,
                 graph_fixture const& fixture,
                 int rank,
                 NodeID label_base,
                 bool configure_incremental_rounds = false) {
  auto const first = fixture.ranges[static_cast<std::size_t>(rank)];
  auto const end = fixture.ranges[static_cast<std::size_t>(rank + 1)];
  auto local_edges = std::size_t{0};
  for (auto global = first; global < end; ++global) {
    local_edges += fixture.adjacency[static_cast<std::size_t>(global)].size();
  }
  auto const global_edges = std::ranges::fold_left(
      fixture.adjacency | std::views::transform(&std::vector<NodeID>::size),
      std::size_t{0}, std::plus<>{});
  graph.start_construction(
      end - first, static_cast<parhip::EdgeID>(local_edges),
      static_cast<NodeID>(fixture.adjacency.size()),
      static_cast<parhip::EdgeID>(global_edges), configure_incremental_rounds);
  graph.set_range(first, first == end ? first : end - 1);
  auto ranges = fixture.ranges;
  graph.set_range_array(ranges);

  for (auto global = first; global < end; ++global) {
    auto const local = graph.new_node();
    graph.setNodeWeight(local, 1);
    graph.setNodeLabel(local, label_base + global);
    graph.setSecondPartitionIndex(local, 0);
    for (auto const target :
         fixture.adjacency[static_cast<std::size_t>(global)]) {
      auto const edge = graph.new_edge(local, target);
      graph.setEdgeWeight(edge, 1);
    }
  }
  graph.finish_construction();
}

[[nodiscard]] auto graph_labels(parallel_graph_access& graph)
    -> std::vector<std::pair<NodeID, NodeID>> {
  auto result = std::vector<std::pair<NodeID, NodeID>>{};
  for (NodeID local = 0; local < graph.number_of_local_nodes(); ++local) {
    result.emplace_back(graph.getGlobalID(local), graph.getNodeLabel(local));
  }
  for (auto local = graph.number_of_local_nodes() + NodeID{1};
       local < graph.number_of_local_nodes() + NodeID{1} +
                   graph.number_of_ghost_nodes();
       ++local) {
    result.emplace_back(graph.getGlobalID(local), graph.getNodeLabel(local));
  }
  std::ranges::sort(result);
  return result;
}

[[nodiscard]] auto labels_are_exact(parallel_graph_access& graph,
                                    NodeID label_base) -> bool {
  return std::ranges::all_of(graph_labels(graph), [&](auto const& entry) {
    return entry.second == label_base + entry.first;
  });
}

[[nodiscard]] auto ghost_labels_are_exact(parallel_graph_access& graph,
                                          NodeID label_base) -> bool {
  for (auto local = graph.number_of_local_nodes() + NodeID{1};
       local < graph.number_of_local_nodes() + NodeID{1} +
                   graph.number_of_ghost_nodes();
       ++local) {
    if (graph.getNodeLabel(local) != label_base + graph.getGlobalID(local)) {
      return false;
    }
  }
  return true;
}

void require_common(bool local_condition) {
  auto const local = local_condition ? 1 : 0;
  auto common = 0;
  REQUIRE(PMPI_Allreduce(&local, &common, 1, MPI_INT, MPI_MIN,
                         MPI_COMM_WORLD) == MPI_SUCCESS);
  CHECK(common == 1);
}

[[nodiscard]] auto global_protocol_is_collective(
    ghost_label_probe::counters const& value) -> bool {
  return value.topology_creations == 1 && value.count_exchanges == 1 &&
         value.blocking_payloads == 1 && value.immediate_payloads == 0 &&
         value.completions == 0 && value.point_to_point_calls == 0 &&
         value.barriers == 0;
}

class trace_activation final {
 public:
  trace_activation() {
    parhip::mpi::trace::reset();
    parhip::mpi::trace::set_active(true);
    KAHIP_MPI_TRACE_SET_HIERARCHY(7, 3, parhip::mpi::trace::epoch::refinement);
    KAHIP_MPI_TRACE_SET_ITERATION(11);
  }
  ~trace_activation() { parhip::mpi::trace::set_active(false); }

  trace_activation(trace_activation const&) = delete;
  auto operator=(trace_activation const&) -> trace_activation& = delete;
};

[[nodiscard]] auto trace_preserves_repeated_updates(
    std::span<parhip::mpi::trace::record const> records,
    int rank,
    int size) -> bool {
  auto expected_sources = std::vector<int>{};
  if (size > 1) {
    expected_sources = {(rank + size - 1) % size, (rank + 1) % size};
    std::ranges::sort(expected_sources);
    auto const unique_end = std::ranges::unique(expected_sources);
    expected_sources.erase(unique_end.begin(), unique_end.end());
  }
  for (auto const source : expected_sources) {
    auto source_records = std::vector<parhip::mpi::trace::record>{};
    std::ranges::copy_if(
        records, std::back_inserter(source_records), [&](auto const& record) {
          return record.stage_id == parhip::mpi::trace::stage::ghost_update &&
                 record.global_id == static_cast<NodeID>(source);
        });
    if (source_records.size() != 3 ||
        source_records[0].payload != "label=" + std::to_string(100 + source) ||
        source_records[1].payload != "label=" + std::to_string(200 + source) ||
        source_records[2].payload != "label=" + std::to_string(300 + source) ||
        source_records[0].hierarchy.round != 2 ||
        source_records[1].hierarchy.round != 3 ||
        source_records[2].hierarchy.round != 3) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] auto trace_matches_public_scheduler(
    std::span<parhip::mpi::trace::record const> records,
    parallel_graph_access& graph,
    int rank) -> bool {
#if KAHIP_ENABLE_MPI_TRACE
  auto ghost_records = std::vector<parhip::mpi::trace::record>{};
  std::ranges::copy_if(
      records, std::back_inserter(ghost_records), [](auto const& record) {
        return record.stage_id == parhip::mpi::trace::stage::ghost_update;
      });
  if (ghost_records.size() != graph.number_of_ghost_nodes()) {
    return false;
  }
  std::ranges::sort(ghost_records, {}, &parhip::mpi::trace::record::global_id);
  auto expected_global_ids = std::vector<NodeID>{};
  for (auto local = graph.number_of_local_nodes() + NodeID{1};
       local < graph.number_of_local_nodes() + NodeID{1} +
                   graph.number_of_ghost_nodes();
       ++local) {
    expected_global_ids.push_back(graph.getGlobalID(local));
  }
  std::ranges::sort(expected_global_ids);
  for (std::size_t index = 0; index < ghost_records.size(); ++index) {
    auto const global_id = expected_global_ids[index];
    if (!std::in_range<int>(global_id / NodeID{4})) {
      return false;
    }
    auto const& record = ghost_records[index];
    auto const expected_round = global_id % NodeID{4} < NodeID{2} ? 2U : 3U;
    auto const expected_owner = static_cast<int>(global_id / NodeID{4});
    if (record.global_id != global_id || record.hierarchy.cycle != 7 ||
        record.hierarchy.level != 3 ||
        record.hierarchy.epoch_id != parhip::mpi::trace::epoch::refinement ||
        record.hierarchy.iteration != 11 ||
        record.hierarchy.round != expected_round ||
        record.actors.owner != expected_owner ||
        record.actors.requester != -1 || record.actors.receiver != rank ||
        record.semantic_key != "label" ||
        record.payload != "label=" + std::to_string(NodeID{100} + global_id)) {
      return false;
    }
  }
  return true;
#else
  static_cast<void>(graph);
  static_cast<void>(rank);
  return records.empty();
#endif
}

[[nodiscard]] auto ghost_balances_match_public_scheduler(
    parallel_graph_access& graph) -> bool {
  for (auto local = graph.number_of_local_nodes() + NodeID{1};
       local < graph.number_of_local_nodes() + NodeID{1} +
                   graph.number_of_ghost_nodes();
       ++local) {
    auto const global_id = graph.getGlobalID(local);
    if (graph.getBlockSize(global_id) != 0 ||
        graph.getBlockSize(NodeID{100} + global_id) != 1) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] auto trace_preserves_queue_across_global(
    std::span<parhip::mpi::trace::record const> records,
    int rank,
    int size) -> bool {
#if KAHIP_ENABLE_MPI_TRACE
  auto expected_sources = std::vector<int>{};
  if (size > 1) {
    expected_sources = {(rank + size - 1) % size, (rank + 1) % size};
    std::ranges::sort(expected_sources);
    auto const unique_end = std::ranges::unique(expected_sources);
    expected_sources.erase(unique_end.begin(), unique_end.end());
  }
  for (auto const source : expected_sources) {
    auto source_records = std::vector<parhip::mpi::trace::record>{};
    std::ranges::copy_if(
        records, std::back_inserter(source_records), [&](auto const& record) {
          return record.stage_id == parhip::mpi::trace::stage::ghost_update &&
                 record.global_id == static_cast<NodeID>(source);
        });
    if (source_records.size() != 3 ||
        source_records[0].payload != "label=" + std::to_string(100 + source) ||
        source_records[0].hierarchy.round != 0 ||
        source_records[1].payload != "label=" + std::to_string(100 + source) ||
        source_records[1].hierarchy.round != 2 ||
        source_records[2].payload != "label=" + std::to_string(200 + source) ||
        source_records[2].hierarchy.round != 2) {
      return false;
    }
  }
  return true;
#else
  static_cast<void>(rank);
  static_cast<void>(size);
  return records.empty();
#endif
}

void exercise_corruption(ghost_label_probe::corruption mode) {
  auto rank = 0;
  auto size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  if (size < 3) {
    return;
  }

  constexpr auto label_base = NodeID{100};
  parallel_graph_access graph{MPI_COMM_WORLD};
  build_graph(graph, ring_fixture(size), rank, label_base);
  auto config = parhip::PPartitionConfig{};
  config.k = 1024;
  config.total_num_labels = 1024;
  graph.init_balance_management(config);
  auto const before = graph_labels(graph);

  auto caught = 0;
  auto structured = 0;
  auto after_failure = std::vector<std::pair<NodeID, NodeID>>{};
  auto retry_is_exact = false;
  auto fired = false;
  {
    auto probe = ghost_label_probe::activation{mode};
    try {
      graph.update_ghost_node_data_global();
    } catch (parhip::mpi::mpi_error const&) {
      caught = 1;
      structured = 1;
    } catch (...) {
      caught = 1;
    }
    after_failure = graph_labels(graph);
    fired = ghost_label_probe::corruption_fired;
    ghost_label_probe::corruption_mode = ghost_label_probe::corruption::none;

    auto caught_minimum = 0;
    REQUIRE(PMPI_Allreduce(&caught, &caught_minimum, 1, MPI_INT, MPI_MIN,
                           MPI_COMM_WORLD) == MPI_SUCCESS);
    if (caught_minimum == 0) {
      for (NodeID local = 0; local < graph.number_of_local_nodes(); ++local) {
        graph.setNodeLabel(local, label_base + graph.getGlobalID(local));
      }
      graph.update_ghost_node_data_finish();
    }

    graph.update_ghost_node_data_global();
    retry_is_exact = labels_are_exact(graph, label_base);
  }

  auto caught_total = 0;
  auto structured_total = 0;
  auto fired_local = fired ? 1 : 0;
  auto fired_anywhere = 0;
  REQUIRE(PMPI_Allreduce(&caught, &caught_total, 1, MPI_INT, MPI_SUM,
                         MPI_COMM_WORLD) == MPI_SUCCESS);
  REQUIRE(PMPI_Allreduce(&structured, &structured_total, 1, MPI_INT, MPI_SUM,
                         MPI_COMM_WORLD) == MPI_SUCCESS);
  REQUIRE(PMPI_Allreduce(&fired_local, &fired_anywhere, 1, MPI_INT, MPI_MAX,
                         MPI_COMM_WORLD) == MPI_SUCCESS);
  CAPTURE(caught_total, structured_total, fired_anywhere,
          after_failure == before, retry_is_exact);
  require_common(fired_anywhere == 1 && caught_total == size &&
                 structured_total == size && after_failure == before &&
                 retry_is_exact);
}
}  // namespace

TEST_CASE("ghost label wire records have exact MPI extent",
          "[unit][mpi][ghost-label][datatype]") {
  STATIC_REQUIRE(std::is_standard_layout_v<parhip::ghost_label_update>);
  STATIC_REQUIRE(std::is_trivially_copyable_v<parhip::ghost_label_update>);
  auto datatype = parhip::mpi::make_mpi_datatype<parhip::ghost_label_update>(
      MPI_COMM_WORLD);
  auto lower_bound = MPI_Aint{-1};
  auto extent = MPI_Aint{-1};
  REQUIRE(PMPI_Type_get_extent(datatype.native_handle(), &lower_bound,
                               &extent) == MPI_SUCCESS);
  REQUIRE(lower_bound == 0);
  REQUIRE(extent == static_cast<MPI_Aint>(sizeof(parhip::ghost_label_update)));
}

TEST_CASE("global ghost labels use one blocking neighborhood exchange",
          "[unit][mpi][ghost-label][protocol]") {
  auto rank = 0;
  auto size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);

  constexpr auto label_base = NodeID{100};
  parallel_graph_access graph{MPI_COMM_WORLD};
  build_graph(graph, ring_fixture(size), rank, label_base);
  auto protocol = ghost_label_probe::counters{};
  auto exact = false;
  {
    auto probe = ghost_label_probe::activation{};
    graph.update_ghost_node_data_global();
    protocol = ghost_label_probe::observed;
    exact = labels_are_exact(graph, label_base);
  }
  CAPTURE(exact, protocol.topology_creations, protocol.count_exchanges,
          protocol.blocking_payloads, protocol.immediate_payloads,
          protocol.completions, protocol.point_to_point_calls,
          protocol.barriers);
  require_common(exact && global_protocol_is_collective(protocol));
}

TEST_CASE("global ghost labels include ranks with zero local work",
          "[unit][mpi][ghost-label][zero-local]") {
  auto rank = 0;
  auto size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  if (size < 3) {
    return;
  }

  constexpr auto label_base = NodeID{100};
  parallel_graph_access graph{MPI_COMM_WORLD};
  build_graph(graph, zero_local_fixture(size), rank, label_base);
  auto protocol = ghost_label_probe::counters{};
  auto exact = false;
  {
    auto probe = ghost_label_probe::activation{};
    graph.update_ghost_node_data_global();
    protocol = ghost_label_probe::observed;
    exact = labels_are_exact(graph, label_base);
  }
  CAPTURE(exact, protocol.topology_creations, protocol.count_exchanges,
          protocol.blocking_payloads, protocol.immediate_payloads,
          protocol.completions, protocol.point_to_point_calls,
          protocol.barriers);
  require_common(exact && global_protocol_is_collective(protocol));
}

TEST_CASE("incremental ghost labels preserve one-round lag and update order",
          "[unit][mpi][ghost-label][pipeline]") {
  auto rank = 0;
  auto size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  if (size == 1) {
    return;
  }

  parallel_graph_access::set_comm_rounds(8);
  parallel_graph_access graph{MPI_COMM_WORLD};
  build_graph(graph, ring_fixture(size), rank, NodeID{0}, true);
  auto config = parhip::PPartitionConfig{};
  config.k = 1024;
  config.total_num_labels = 1024;
  graph.init_balance_management(config);

  auto after_first = ghost_label_probe::counters{};
  auto after_second = ghost_label_probe::counters{};
  auto after_third = ghost_label_probe::counters{};
  auto first_post_did_not_apply = false;
  auto second_completed_first = false;
  auto third_preserved_order = false;
  auto trace_is_exact = KAHIP_ENABLE_MPI_TRACE == 0;
  {
    auto probe = ghost_label_probe::activation{};
    auto trace = trace_activation{};

    graph.setNodeLabel(0, static_cast<NodeID>(100 + rank));
    graph.update_ghost_node_data(false);
    after_first = ghost_label_probe::observed;
    first_post_did_not_apply = ghost_labels_are_exact(graph, NodeID{0});

    graph.setNodeLabel(0, static_cast<NodeID>(200 + rank));
    graph.setNodeLabel(0, static_cast<NodeID>(300 + rank));
    graph.update_ghost_node_data(false);
    after_second = ghost_label_probe::observed;
    second_completed_first = ghost_labels_are_exact(graph, NodeID{100});

    graph.update_ghost_node_data(false);
    after_third = ghost_label_probe::observed;
    third_preserved_order = ghost_labels_are_exact(graph, NodeID{300});
#if KAHIP_ENABLE_MPI_TRACE
    auto const records = parhip::mpi::trace::snapshot();
    trace_is_exact = trace_preserves_repeated_updates(records, rank, size);
#endif

    graph.update_ghost_node_data_finish();
  }

  auto const protocol_is_exact =
      after_first.topology_creations == 1 && after_first.count_exchanges == 1 &&
      after_first.immediate_payloads == 1 && after_first.completions == 0 &&
      after_first.point_to_point_calls == 0 &&
      after_second.count_exchanges == 2 &&
      after_second.immediate_payloads == 2 && after_second.completions == 1 &&
      after_second.point_to_point_calls == 0 &&
      after_third.count_exchanges == 3 && after_third.immediate_payloads == 3 &&
      after_third.completions == 2 && after_third.point_to_point_calls == 0;
  CAPTURE(first_post_did_not_apply, second_completed_first,
          third_preserved_order, trace_is_exact, protocol_is_exact,
          after_first.topology_creations, after_first.count_exchanges,
          after_first.immediate_payloads, after_first.completions,
          after_first.point_to_point_calls, after_second.count_exchanges,
          after_second.immediate_payloads, after_second.completions,
          after_second.point_to_point_calls, after_third.count_exchanges,
          after_third.immediate_payloads, after_third.completions,
          after_third.point_to_point_calls);
  require_common(first_post_did_not_apply && second_completed_first &&
                 third_preserved_order && trace_is_exact && protocol_is_exact);
}

TEST_CASE("public ghost scheduler completes multi-node rounds and finish",
          "[unit][mpi][ghost-label][scheduler][balance]") {
  auto rank = 0;
  auto size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  if (size == 1) {
    return;
  }

  constexpr auto nodes_per_rank = NodeID{4};
  parallel_graph_access::set_comm_rounds(8);
  parallel_graph_access graph{MPI_COMM_WORLD};
  build_graph(graph, multi_node_ring_fixture(size, nodes_per_rank), rank,
              NodeID{0}, true);
  auto config = parhip::PPartitionConfig{};
  config.k = 1;
  config.total_num_labels = 1024;
  graph.init_balance_management(config);

  auto protocol = ghost_label_probe::counters{};
  auto labels_are_final = false;
  auto balances_are_final = false;
  auto trace_is_exact = false;
  auto trace_text = std::string{};
  {
    auto probe = ghost_label_probe::activation{};
    auto trace = trace_activation{};
    for (NodeID local = 0; local < nodes_per_rank; ++local) {
      graph.setNodeLabel(local, NodeID{100} + graph.getGlobalID(local));
      graph.update_ghost_node_data();
    }
    graph.update_ghost_node_data_finish();
    protocol = ghost_label_probe::observed;
    labels_are_final = labels_are_exact(graph, NodeID{100});
    balances_are_final = ghost_balances_match_public_scheduler(graph);
    auto const records = parhip::mpi::trace::snapshot();
    trace_text = parhip::mpi::trace::canonical_text(records);
    trace_is_exact = trace_matches_public_scheduler(records, graph, rank);
  }

  auto const expected_records =
      static_cast<std::uint64_t>(nodes_per_rank) *
      static_cast<std::uint64_t>(graph.getNumberOfAdjacentPEs());
  auto const protocol_is_exact =
      protocol.topology_creations == 1 && protocol.count_exchanges == 8 &&
      protocol.blocking_payloads == 0 && protocol.immediate_payloads == 8 &&
      protocol.completions == 8 && protocol.point_to_point_calls == 0 &&
      protocol.barriers == 0 &&
      protocol.immediate_records == expected_records &&
      !protocol.callback_error;
  CAPTURE(labels_are_final, balances_are_final, trace_is_exact,
          protocol_is_exact, protocol.topology_creations,
          protocol.count_exchanges, protocol.blocking_payloads,
          protocol.immediate_payloads, protocol.completions,
          protocol.point_to_point_calls, protocol.barriers,
          protocol.immediate_records, expected_records, protocol.callback_error,
          trace_text);
  require_common(labels_are_final && balances_are_final && trace_is_exact &&
                 protocol_is_exact);
}

TEST_CASE("global ghost exchange preserves queued incremental updates",
          "[unit][mpi][ghost-label][pipeline][global]") {
  auto rank = 0;
  auto size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  if (size == 1) {
    return;
  }

  parallel_graph_access::set_comm_rounds(8);
  parallel_graph_access graph{MPI_COMM_WORLD};
  build_graph(graph, ring_fixture(size), rank, NodeID{0}, true);
  auto config = parhip::PPartitionConfig{};
  config.k = 1;
  config.total_num_labels = 1024;
  graph.init_balance_management(config);

  auto protocol = ghost_label_probe::counters{};
  auto labels_are_final = false;
  auto trace_is_exact = false;
  {
    auto probe = ghost_label_probe::activation{};
    auto trace = trace_activation{};
    graph.setNodeLabel(0, static_cast<NodeID>(100 + rank));
    graph.update_ghost_node_data_global();
    graph.setNodeLabel(0, static_cast<NodeID>(200 + rank));
    graph.update_ghost_node_data(false);
    graph.update_ghost_node_data(false);
    graph.update_ghost_node_data_finish();
    protocol = ghost_label_probe::observed;
    labels_are_final = labels_are_exact(graph, NodeID{200});
    trace_is_exact = trace_preserves_queue_across_global(
        parhip::mpi::trace::snapshot(), rank, size);
  }

  auto const expected_incremental_records =
      std::uint64_t{2} *
      static_cast<std::uint64_t>(graph.getNumberOfAdjacentPEs());
  auto const queue_was_preserved =
      !protocol.callback_error &&
      protocol.immediate_records == expected_incremental_records;
  CAPTURE(labels_are_final, trace_is_exact, queue_was_preserved,
          protocol.immediate_records, expected_incremental_records,
          protocol.callback_error);
  require_common(labels_are_final && trace_is_exact && queue_was_preserved);
}

TEST_CASE("unknown ghost IDs fail collectively without mutation and retry",
          "[unit][mpi][ghost-label][transaction]") {
  exercise_corruption(ghost_label_probe::corruption::unknown_id);
}

TEST_CASE("wrong-source ghost IDs fail collectively without mutation and retry",
          "[unit][mpi][ghost-label][transaction][source]") {
  exercise_corruption(ghost_label_probe::corruption::wrong_source);
}
