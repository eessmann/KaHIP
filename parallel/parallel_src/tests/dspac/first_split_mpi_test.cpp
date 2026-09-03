#include <mpi.h>

#include <catch2/catch_all.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <functional>
#include <limits>
#include <numeric>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

#include "communication/mpi_collectives.h"
#include "data_structure/parallel_graph_access.h"
#include "definitions.h"
#include "dspac/dspac.h"
#include "kahip_mpi_capabilities.h"

// KAHIP_PMPI_CALLBACK_REGION_BEGIN
namespace dspac_first_split_probe {
inline bool active = false;
inline int topology_creations = 0;
inline int count_exchanges = 0;
inline int legacy_payloads = 0;
inline int large_count_payloads = 0;
inline int point_to_point_calls = 0;
inline int maximum_payload_count = 0;
inline bool payload_signature_is_valid = true;

void reset() noexcept {
  topology_creations = 0;
  count_exchanges = 0;
  legacy_payloads = 0;
  large_count_payloads = 0;
  point_to_point_calls = 0;
  maximum_payload_count = 0;
  payload_signature_is_valid = true;
}

void record_legacy_payload(int const send_counts[],
                           int const receive_counts[],
                           MPI_Datatype send_datatype,
                           MPI_Datatype receive_datatype,
                           MPI_Comm communicator) noexcept {
  ++legacy_payloads;
  auto indegree = 0;
  auto outdegree = 0;
  auto weighted = 0;
  if (PMPI_Dist_graph_neighbors_count(communicator, &indegree, &outdegree,
                                      &weighted) != MPI_SUCCESS) {
    payload_signature_is_valid = false;
    return;
  }
  payload_signature_is_valid =
      payload_signature_is_valid &&
      (outdegree == 0 || send_counts != nullptr) &&
      (indegree == 0 || receive_counts != nullptr) &&
      send_datatype == MPI_UNSIGNED_LONG_LONG &&
      receive_datatype == MPI_UNSIGNED_LONG_LONG;
  for (int index = 0; index < outdegree; ++index) {
    maximum_payload_count =
        std::max(maximum_payload_count, send_counts[index]);
  }
  for (int index = 0; index < indegree; ++index) {
    maximum_payload_count =
        std::max(maximum_payload_count, receive_counts[index]);
  }
}

#if defined(KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_C) && \
    KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_C
void record_large_count_payload(MPI_Count const send_counts[],
                                MPI_Count const receive_counts[],
                                MPI_Datatype send_datatype,
                                MPI_Datatype receive_datatype,
                                MPI_Comm communicator) noexcept {
  ++large_count_payloads;
  auto indegree = 0;
  auto outdegree = 0;
  auto weighted = 0;
  if (PMPI_Dist_graph_neighbors_count(communicator, &indegree, &outdegree,
                                      &weighted) != MPI_SUCCESS) {
    payload_signature_is_valid = false;
    return;
  }
  payload_signature_is_valid =
      payload_signature_is_valid &&
      (outdegree == 0 || send_counts != nullptr) &&
      (indegree == 0 || receive_counts != nullptr) &&
      send_datatype == MPI_UNSIGNED_LONG_LONG &&
      receive_datatype == MPI_UNSIGNED_LONG_LONG;
  for (int index = 0; index < outdegree; ++index) {
    if (send_counts[index] > std::numeric_limits<int>::max()) {
      maximum_payload_count = std::numeric_limits<int>::max();
    } else if (send_counts[index] > maximum_payload_count) {
      maximum_payload_count = static_cast<int>(send_counts[index]);
    }
  }
  for (int index = 0; index < indegree; ++index) {
    if (receive_counts[index] > std::numeric_limits<int>::max()) {
      maximum_payload_count = std::numeric_limits<int>::max();
    } else if (receive_counts[index] > maximum_payload_count) {
      maximum_payload_count = static_cast<int>(receive_counts[index]);
    }
  }
}
#endif

class activation final {
 public:
  activation() noexcept {
    reset();
    active = true;
  }
  ~activation() noexcept { active = false; }

  activation(activation const&) = delete;
  auto operator=(activation const&) -> activation& = delete;
};
}  // namespace dspac_first_split_probe

static_assert(noexcept(dspac_first_split_probe::reset()));
static_assert(noexcept(dspac_first_split_probe::record_legacy_payload(
    nullptr, nullptr, MPI_DATATYPE_NULL, MPI_DATATYPE_NULL, MPI_COMM_NULL)));
#if defined(KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_C) && \
    KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_C
static_assert(noexcept(dspac_first_split_probe::record_large_count_payload(
    nullptr, nullptr, MPI_DATATYPE_NULL, MPI_DATATYPE_NULL, MPI_COMM_NULL)));
#endif

extern "C" int MPI_Dist_graph_create(MPI_Comm communicator,
                                     int source_count,
                                     int const sources[],
                                     int const degrees[],
                                     int const destinations[],
                                     int const weights[],
                                     MPI_Info info,
                                     int reorder,
                                     MPI_Comm* graph_communicator) {
  if (dspac_first_split_probe::active) {
    ++dspac_first_split_probe::topology_creations;
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
  if (dspac_first_split_probe::active) {
    ++dspac_first_split_probe::count_exchanges;
  }
  return PMPI_Neighbor_alltoall(send_buffer, send_count, send_datatype,
                                receive_buffer, receive_count,
                                receive_datatype, communicator);
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
  if (dspac_first_split_probe::active) {
    dspac_first_split_probe::record_legacy_payload(
        send_counts, receive_counts, send_datatype, receive_datatype,
        communicator);
  }
  return PMPI_Neighbor_alltoallv(send_buffer, send_counts, send_displacements,
                                 send_datatype, receive_buffer, receive_counts,
                                 receive_displacements, receive_datatype,
                                 communicator);
}

#if defined(KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_C) && \
    KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_C
extern "C" int MPI_Neighbor_alltoallv_c(
    void const* send_buffer,
    MPI_Count const send_counts[],
    MPI_Aint const send_displacements[],
    MPI_Datatype send_datatype,
    void* receive_buffer,
    MPI_Count const receive_counts[],
    MPI_Aint const receive_displacements[],
    MPI_Datatype receive_datatype,
    MPI_Comm communicator) {
  if (dspac_first_split_probe::active) {
    dspac_first_split_probe::record_large_count_payload(
        send_counts, receive_counts, send_datatype, receive_datatype,
        communicator);
  }
  return PMPI_Neighbor_alltoallv_c(
      send_buffer, send_counts, send_displacements, send_datatype,
      receive_buffer, receive_counts, receive_displacements, receive_datatype,
      communicator);
}
#endif

extern "C" int MPI_Isend(void const* buffer,
                         int count,
                         MPI_Datatype datatype,
                         int destination,
                         int tag,
                         MPI_Comm communicator,
                         MPI_Request* request) {
  if (dspac_first_split_probe::active) {
    ++dspac_first_split_probe::point_to_point_calls;
  }
  return PMPI_Isend(buffer, count, datatype, destination, tag, communicator,
                    request);
}

extern "C" int MPI_Irecv(void* buffer,
                         int count,
                         MPI_Datatype datatype,
                         int source,
                         int tag,
                         MPI_Comm communicator,
                         MPI_Request* request) {
  if (dspac_first_split_probe::active) {
    ++dspac_first_split_probe::point_to_point_calls;
  }
  return PMPI_Irecv(buffer, count, datatype, source, tag, communicator,
                    request);
}

extern "C" int MPI_Recv(void* buffer,
                        int count,
                        MPI_Datatype datatype,
                        int source,
                        int tag,
                        MPI_Comm communicator,
                        MPI_Status* status) {
  if (dspac_first_split_probe::active) {
    ++dspac_first_split_probe::point_to_point_calls;
  }
  return PMPI_Recv(buffer, count, datatype, source, tag, communicator, status);
}

extern "C" int MPI_Wait(MPI_Request* request, MPI_Status* status) {
  if (dspac_first_split_probe::active) {
    ++dspac_first_split_probe::point_to_point_calls;
  }
  return PMPI_Wait(request, status);
}
// KAHIP_PMPI_CALLBACK_REGION_END

namespace {
using adjacency_list = std::vector<std::vector<parhip::NodeID>>;

struct graph_fixture final {
  std::vector<parhip::NodeID> node_ranges;
  std::vector<parhip::EdgeID> edge_ranges;
  adjacency_list adjacency;
};

struct expected_edge final {
  parhip::NodeID target;
  parhip::EdgeWeight weight;

  auto operator==(expected_edge const&) const -> bool = default;
};

[[nodiscard]] auto make_fixture(adjacency_list adjacency,
                                int size) -> graph_fixture {
  auto const global_nodes = static_cast<parhip::NodeID>(adjacency.size());
  auto node_ranges =
      std::vector<parhip::NodeID>(static_cast<std::size_t>(size) + 1);
  for (int pe = 0; pe <= size; ++pe) {
    node_ranges[static_cast<std::size_t>(pe)] =
        global_nodes * static_cast<parhip::NodeID>(pe) /
        static_cast<parhip::NodeID>(size);
  }

  auto edge_ranges =
      std::vector<parhip::EdgeID>(static_cast<std::size_t>(size) + 1);
  for (int pe = 0; pe < size; ++pe) {
    auto extent = parhip::EdgeID{};
    for (auto node = node_ranges[static_cast<std::size_t>(pe)];
         node < node_ranges[static_cast<std::size_t>(pe) + 1]; ++node) {
      extent += static_cast<parhip::EdgeID>(
          adjacency[static_cast<std::size_t>(node)].size());
    }
    edge_ranges[static_cast<std::size_t>(pe) + 1] =
        edge_ranges[static_cast<std::size_t>(pe)] + extent;
  }
  return {std::move(node_ranges), std::move(edge_ranges),
          std::move(adjacency)};
}

void build_graph(parhip::parallel_graph_access& graph,
                 graph_fixture const& fixture,
                 int rank) {
  auto const first = fixture.node_ranges[static_cast<std::size_t>(rank)];
  auto const end = fixture.node_ranges[static_cast<std::size_t>(rank) + 1];
  auto const local_nodes = end - first;
  auto const local_edges =
      fixture.edge_ranges[static_cast<std::size_t>(rank) + 1] -
      fixture.edge_ranges[static_cast<std::size_t>(rank)];
  auto const global_nodes =
      static_cast<parhip::NodeID>(fixture.adjacency.size());
  auto const global_edges = fixture.edge_ranges.back();

  graph.start_construction(local_nodes, local_edges, global_nodes,
                           global_edges, false);
  graph.set_range(first, first == end ? first : end - parhip::NodeID{1});
  auto node_ranges = fixture.node_ranges;
  graph.set_range_array(node_ranges);
  graph.set_edge_range_array(fixture.edge_ranges);

  for (auto global = first; global < end; ++global) {
    auto const local = graph.new_node();
    graph.setNodeWeight(local, 1);
    graph.setNodeLabel(local, global);
    graph.setSecondPartitionIndex(local, 0);
    for (auto const target :
         fixture.adjacency[static_cast<std::size_t>(global)]) {
      auto const edge = graph.new_edge(local, target);
      graph.setEdgeWeight(edge, 1);
    }
  }
  graph.finish_construction();
}

void require_exact_split(
    parhip::parallel_graph_access& split,
    graph_fixture const& fixture,
    int rank,
    std::span<std::vector<expected_edge> const> expected) {
  auto const first = fixture.edge_ranges[static_cast<std::size_t>(rank)];
  auto const end = fixture.edge_ranges[static_cast<std::size_t>(rank) + 1];
  auto const local_nodes = end - first;
  auto expected_local_edges = parhip::EdgeID{};
  for (auto global = first; global < end; ++global) {
    expected_local_edges += static_cast<parhip::EdgeID>(
        expected[static_cast<std::size_t>(global)].size());
  }
  auto const expected_global_edges = std::ranges::fold_left(
      expected | std::views::transform(&std::vector<expected_edge>::size),
      parhip::EdgeID{}, std::plus<>{});

  REQUIRE(split.number_of_local_nodes() == local_nodes);
  REQUIRE(split.number_of_local_edges() == expected_local_edges);
  REQUIRE(split.number_of_global_nodes() == expected.size());
  REQUIRE(split.number_of_global_edges() == expected_global_edges);
  REQUIRE(split.get_from_range() == first);
  REQUIRE(split.get_to_range() == (first == end ? first : end - 1));
  REQUIRE(split.get_range_array() == fixture.edge_ranges);

  for (auto local = parhip::NodeID{}; local < local_nodes; ++local) {
    auto const global = first + local;
    REQUIRE(split.getNodeWeight(local) == 1);
    REQUIRE(split.getNodeLabel(local) == global);
    REQUIRE(split.getSecondPartitionIndex(local) == 0);

    auto actual = std::vector<expected_edge>{};
    for (auto edge = split.get_first_edge(local);
         edge < split.get_first_invalid_edge(local); ++edge) {
      auto const target = split.getEdgeTarget(edge);
      actual.push_back(
          {split.getGlobalID(target), split.getEdgeWeight(edge)});
    }
    REQUIRE(actual == expected[static_cast<std::size_t>(global)]);
  }
}

void require_exact_reverse_projection(parhip::dspac& splitter,
                                      parhip::parallel_graph_access& split,
                                      graph_fixture const& fixture,
                                      int rank) {
  auto const first = fixture.edge_ranges[static_cast<std::size_t>(rank)];
  auto const end = fixture.edge_ranges[static_cast<std::size_t>(rank) + 1];
  auto const local_edge_count = static_cast<std::size_t>(end - first);
  auto permutation = std::vector<parhip::EdgeID>(local_edge_count);
  std::ranges::iota(permutation, parhip::EdgeID{});
  std::ranges::reverse(permutation);

  auto expected = std::vector<parhip::PartitionID>(local_edge_count);
  for (std::size_t edge = 0; edge < local_edge_count; ++edge) {
    expected[static_cast<std::size_t>(permutation[edge])] =
        static_cast<parhip::PartitionID>(first + edge);
  }

  REQUIRE(splitter.project_partition(split, permutation) == expected);
}

[[nodiscard]] auto path_with_isolate_fixture(int size) -> graph_fixture {
  return make_fixture({{1, 2}, {0}, {0, 3}, {2}, {}}, size);
}

[[nodiscard]] auto path_with_isolate_expected(parhip::EdgeWeight infinity)
    -> std::array<std::vector<expected_edge>, 6> {
  return {{
      {{2, infinity}, {1, 1}},
      {{3, infinity}, {0, 1}},
      {{0, infinity}},
      {{1, infinity}, {4, 1}},
      {{5, infinity}, {3, 1}},
      {{4, infinity}},
  }};
}

[[nodiscard]] auto two_node_fixture(int size) -> graph_fixture {
  return make_fixture({{1}, {0}}, size);
}

[[nodiscard]] auto two_node_expected(parhip::EdgeWeight infinity)
    -> std::array<std::vector<expected_edge>, 2> {
  return {{{{1, infinity}}, {{0, infinity}}}};
}

void require_collective_protocol(bool single_payload = true) {
  REQUIRE(dspac_first_split_probe::topology_creations == 1);
  REQUIRE(dspac_first_split_probe::count_exchanges == 1);
  auto const payload_calls = dspac_first_split_probe::legacy_payloads +
                             dspac_first_split_probe::large_count_payloads;
  REQUIRE(payload_calls >= 1);
  if (single_payload) {
    REQUIRE(payload_calls == 1);
  }
  REQUIRE(dspac_first_split_probe::point_to_point_calls == 0);
  REQUIRE(dspac_first_split_probe::payload_signature_is_valid);
}
}  // namespace

TEST_CASE("DSPAC first split preserves the exact split graph with neighborhood collectives") {
  auto rank = -1;
  auto size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  REQUIRE(size >= 1);
  REQUIRE(size <= 5);

  constexpr auto infinity = std::numeric_limits<parhip::EdgeWeight>::max();
  auto const fixture = path_with_isolate_fixture(size);
  auto const expected = path_with_isolate_expected(infinity);
  auto input = parhip::parallel_graph_access{MPI_COMM_WORLD};
  auto split = parhip::parallel_graph_access{MPI_COMM_WORLD};
  build_graph(input, fixture, rank);
  auto splitter = parhip::dspac{input, MPI_COMM_WORLD, infinity};

  {
    dspac_first_split_probe::activation const probe;
    splitter.construct(split);
  }

  require_exact_split(split, fixture, rank, expected);
  require_exact_reverse_projection(splitter, split, fixture, rank);
  require_collective_protocol();
}

TEST_CASE("DSPAC first split supports zero-local-work ranks") {
  auto rank = -1;
  auto size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);

  constexpr auto infinity = parhip::EdgeWeight{37};
  auto const fixture = two_node_fixture(size);
  auto const expected = two_node_expected(infinity);
  auto input = parhip::parallel_graph_access{MPI_COMM_WORLD};
  auto split = parhip::parallel_graph_access{MPI_COMM_WORLD};
  build_graph(input, fixture, rank);
  auto splitter = parhip::dspac{input, MPI_COMM_WORLD, infinity};

  {
    dspac_first_split_probe::activation const probe;
    splitter.construct(split);
  }

  require_exact_split(split, fixture, rank, expected);
  require_exact_reverse_projection(splitter, split, fixture, rank);
  require_collective_protocol();
  if (size > 2 && fixture.node_ranges[static_cast<std::size_t>(rank)] ==
                      fixture.node_ranges[static_cast<std::size_t>(rank) + 1]) {
    REQUIRE(input.number_of_local_nodes() == 0);
  }
}

TEST_CASE("DSPAC first split preserves a globally empty graph") {
  auto rank = -1;
  auto size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);

  constexpr auto infinity = parhip::EdgeWeight{91};
  auto const fixture = make_fixture({}, size);
  auto const expected = std::array<std::vector<expected_edge>, 0>{};
  auto input = parhip::parallel_graph_access{MPI_COMM_WORLD};
  auto split = parhip::parallel_graph_access{MPI_COMM_WORLD};
  build_graph(input, fixture, rank);
  auto splitter = parhip::dspac{input, MPI_COMM_WORLD, infinity};

  {
    dspac_first_split_probe::activation const probe;
    splitter.construct(split);
  }

  require_exact_split(split, fixture, rank, expected);
  require_exact_reverse_projection(splitter, split, fixture, rank);
  require_collective_protocol();
}

TEST_CASE("DSPAC first split uses bounded MPI-3 neighborhood rounds") {
  auto rank = -1;
  auto size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);

  constexpr auto infinity = parhip::EdgeWeight{53};
  auto const fixture = path_with_isolate_fixture(size);
  auto const expected = path_with_isolate_expected(infinity);
  auto input = parhip::parallel_graph_access{MPI_COMM_WORLD};
  auto split = parhip::parallel_graph_access{MPI_COMM_WORLD};
  build_graph(input, fixture, rank);
  auto splitter = parhip::dspac{
      input, MPI_COMM_WORLD, infinity,
      parhip::mpi::collective_options{.mpi3_round_ceiling = 1,
                                      .force_mpi3 = true}};

  {
    dspac_first_split_probe::activation const probe;
    splitter.construct(split);
  }

  require_exact_split(split, fixture, rank, expected);
  require_collective_protocol(false);
  REQUIRE(dspac_first_split_probe::large_count_payloads == 0);
  REQUIRE(dspac_first_split_probe::maximum_payload_count <= 1);
  if (size >= 2 && size <= 4) {
    REQUIRE(dspac_first_split_probe::legacy_payloads > 1);
  }
}
