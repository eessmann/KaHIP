#include <mpi.h>

#include <catch2/catch_all.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <numeric>
#include <ranges>
#include <span>
#include <type_traits>
#include <vector>

#include "communication/ghost_exchange_plan.h"
#include "communication/mpi_adapter.h"
#include "data_structure/parallel_graph_access.h"
#include "distributed_partitioning/distributed_consistency.h"

namespace topology_probe {
inline bool active = false;
inline int create_calls = 0;

void reset() noexcept {
  create_calls = 0;
}

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
}  // namespace topology_probe

extern "C" int MPI_Dist_graph_create(MPI_Comm communicator,
                                     int source_count,
                                     int const sources[],
                                     int const degrees[],
                                     int const destinations[],
                                     int const weights[],
                                     MPI_Info info,
                                     int reorder,
                                     MPI_Comm* graph_communicator) {
  if (topology_probe::active) {
    ++topology_probe::create_calls;
  }
  return PMPI_Dist_graph_create(communicator, source_count, sources, degrees,
                                destinations, weights, info, reorder,
                                graph_communicator);
}

namespace {
using parhip::NodeID;
using parhip::parallel_graph_access;

[[nodiscard]] auto ranges_for(int size) -> std::vector<NodeID> {
  auto ranges = std::vector<NodeID>(static_cast<std::size_t>(size) + 1);
  std::ranges::iota(ranges, NodeID{0});
  return ranges;
}

void require_common(bool local_condition) {
  auto const local = local_condition ? 1 : 0;
  auto common = 0;
  REQUIRE(MPI_Allreduce(&local, &common, 1, MPI_INT, MPI_MIN, MPI_COMM_WORLD) ==
          MPI_SUCCESS);
  REQUIRE(common == 1);
}

void build_ring(parallel_graph_access& graph, int rank, int size) {
  auto targets = std::vector<int>{};
  if (size > 1) {
    targets = {(rank + size - 1) % size, (rank + 1) % size};
    std::ranges::sort(targets);
    auto const unique_end = std::ranges::unique(targets);
    targets.erase(unique_end.begin(), unique_end.end());
  }
  auto const remote_edges = targets.size() * 2;
  graph.start_construction(1, remote_edges, static_cast<NodeID>(size),
                           static_cast<NodeID>(size * remote_edges), false);
  graph.set_range(static_cast<NodeID>(rank), static_cast<NodeID>(rank));
  auto ranges = ranges_for(size);
  graph.set_range_array(ranges);

  auto const local_node = graph.new_node();
  graph.setNodeWeight(local_node, 1);
  graph.setNodeLabel(local_node, static_cast<NodeID>(100 + rank));
  graph.setSecondPartitionIndex(local_node, static_cast<NodeID>(200 + rank));

  for (auto const target : targets) {
    for (int duplicate = 0; duplicate < 2; ++duplicate) {
      auto const edge = graph.new_edge(local_node, static_cast<NodeID>(target));
      graph.setEdgeWeight(edge, 1);
    }
  }
  graph.finish_construction();
}

void build_root_only_complete_graph(parallel_graph_access& graph) {
  constexpr auto node_count = NodeID{2};
  graph.start_construction(node_count, 2, node_count, 2, false);
  graph.set_range(0, node_count - 1);
  auto ranges = std::vector<NodeID>{0, node_count};
  graph.set_range_array(ranges);
  for (NodeID global = 0; global < node_count; ++global) {
    auto const node = graph.new_node();
    graph.setNodeWeight(node, 1);
    graph.setNodeLabel(node, global);
    auto const edge = graph.new_edge(node, node_count - global - 1);
    graph.setEdgeWeight(edge, 1);
  }
  graph.finish_construction();
}
}  // namespace

TEST_CASE("graph and ghost communication ownership cannot be shallow-copied",
          "[unit][mpi][ghost-plan][ownership]") {
  STATIC_REQUIRE(!std::is_copy_constructible_v<parallel_graph_access>);
  STATIC_REQUIRE(!std::is_copy_assignable_v<parallel_graph_access>);
  STATIC_REQUIRE(!std::is_move_constructible_v<parallel_graph_access>);
  STATIC_REQUIRE(!std::is_move_assignable_v<parallel_graph_access>);
  STATIC_REQUIRE(
      !std::is_copy_constructible_v<parhip::ghost_node_communication>);
  STATIC_REQUIRE(!std::is_copy_assignable_v<parhip::ghost_node_communication>);
  STATIC_REQUIRE(
      !std::is_move_constructible_v<parhip::ghost_node_communication>);
  STATIC_REQUIRE(!std::is_move_assignable_v<parhip::ghost_node_communication>);
}

TEST_CASE("distributed consistency wire records have exact MPI extent",
          "[unit][mpi][ghost-plan][datatype]") {
  using record = parhip::distributed_consistency::node_value;
  STATIC_REQUIRE(std::is_standard_layout_v<record>);
  STATIC_REQUIRE(std::is_trivially_copyable_v<record>);

  auto datatype = parhip::mpi::make_mpi_datatype<record>(MPI_COMM_WORLD);
  MPI_Aint lower_bound = -1;
  MPI_Aint extent = -1;
  REQUIRE(MPI_Type_get_extent(datatype.native_handle(), &lower_bound,
                              &extent) == MPI_SUCCESS);
  REQUIRE(lower_bound == 0);
  REQUIRE(extent == static_cast<MPI_Aint>(sizeof(record)));
}

TEST_CASE("finish construction stays local including root-only graphs",
          "[unit][mpi][ghost-plan][root-only]") {
  int rank = 0;
  int size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);

  auto probe = topology_probe::activation{};
  auto local_finish_is_local = true;
  if (rank == 0) {
    parallel_graph_access root_graph{MPI_COMM_WORLD};
    build_root_only_complete_graph(root_graph);
    local_finish_is_local = topology_probe::create_calls == 0;
  }
  require_common(local_finish_is_local);
}

TEST_CASE("lazy ghost plans preserve MPI order and rebuild transactionally",
          "[unit][mpi][ghost-plan][cache]") {
  int rank = 0;
  int size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  REQUIRE(size >= 1);
  REQUIRE(size <= 5);

  parallel_graph_access graph{MPI_COMM_WORLD};
  auto probe = topology_probe::activation{};
  build_ring(graph, rank, size);
  auto local_plan_is_valid = topology_probe::create_calls == 0;

  auto const& first_plan = graph.ghost_plan();
  local_plan_is_valid =
      local_plan_is_valid && topology_probe::create_calls == 1;
  local_plan_is_valid =
      local_plan_is_valid &&
      std::addressof(graph.ghost_plan()) == std::addressof(first_plan) &&
      topology_probe::create_calls == 1;

  auto const& topology = first_plan.topology();
  local_plan_is_valid =
      local_plan_is_valid &&
      topology.sources().size() == topology.destinations().size() &&
      std::ranges::is_permutation(topology.sources(), topology.destinations());
  for (std::size_t index = 0; index < topology.destinations().size(); ++index) {
    auto const locals = first_plan.outgoing_local_nodes(index);
    local_plan_is_valid = local_plan_is_valid && locals.size() == 1;
    if (locals.size() == 1) {
      local_plan_is_valid = local_plan_is_valid && locals.front() == 0;
    }
  }
  for (std::size_t index = 0; index < topology.sources().size(); ++index) {
    auto const source = topology.sources()[index];
    auto const ghosts = first_plan.expected_ghost_nodes(index);
    local_plan_is_valid = local_plan_is_valid && ghosts.size() == 1;
    if (ghosts.size() == 1) {
      local_plan_is_valid =
          local_plan_is_valid &&
          ghosts.front() == static_cast<NodeID>(source) &&
          graph.find_ghost_local_id(ghosts.front(), source).has_value();
      if (size > 1) {
        local_plan_is_valid =
            local_plan_is_valid &&
            !graph.find_ghost_local_id(ghosts.front(), (source + 1) % size)
                 .has_value();
      }
    }
  }

  local_plan_is_valid =
      local_plan_is_valid &&
      graph.find_local_id(static_cast<NodeID>(rank)) == NodeID{0} &&
      !graph.find_local_id(static_cast<NodeID>(size + 10)).has_value();
  auto const ghost_count = graph.number_of_ghost_nodes();
  local_plan_is_valid =
      local_plan_is_valid &&
      !graph.find_ghost_local_id(static_cast<NodeID>(size + 10), rank)
           .has_value() &&
      graph.number_of_ghost_nodes() == ghost_count;
  require_common(local_plan_is_valid);

  build_ring(graph, rank, size);
  auto local_rebuild_is_valid = topology_probe::create_calls == 1;
  static_cast<void>(graph.ghost_plan());
  local_rebuild_is_valid =
      local_rebuild_is_valid && topology_probe::create_calls == 2;

  graph.setNodeLabel(0, static_cast<NodeID>(300 + rank));
  graph.reinit();
  local_rebuild_is_valid =
      local_rebuild_is_valid && graph.number_of_local_nodes() == 0 &&
      graph.number_of_ghost_nodes() == 0 &&
      graph.number_of_local_edges() == 0 &&
      !graph.find_local_id(static_cast<NodeID>(rank)).has_value() &&
      !graph.find_ghost_local_id(static_cast<NodeID>(rank), rank).has_value();
  require_common(local_rebuild_is_valid);
}
