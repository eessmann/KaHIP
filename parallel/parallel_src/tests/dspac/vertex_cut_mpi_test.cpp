#include <mpi.h>

#include <catch2/catch_all.hpp>

#include <array>
#include <cstddef>
#include <limits>
#include <vector>

#include "communication/mpi_fixed_reduction.h"
#include "data_structure/parallel_graph_access.h"
#include "definitions.h"
#include "dspac/dspac.h"

// KAHIP_PMPI_CALLBACK_REGION_BEGIN
namespace vertex_cut_probe {
struct observation final {
  int count = 0;
  MPI_Datatype datatype = MPI_DATATYPE_NULL;
  MPI_Op operation = MPI_OP_NULL;
  MPI_Comm communicator = MPI_COMM_NULL;
  parhip::EdgeWeight local_value = 0;
  bool buffers_are_distinct = false;
};

inline bool active = false;
inline int call_count = 0;
inline int minimum_count = 0;
inline int maximum_count = 0;
inline int validation_count = 0;
inline bool unexpected_operation = false;
inline bool all_calls_well_formed = true;
inline MPI_Comm expected_communicator = MPI_COMM_NULL;
inline observation observed{};

void reset(MPI_Comm communicator) noexcept {
  call_count = 0;
  minimum_count = 0;
  maximum_count = 0;
  validation_count = 0;
  unexpected_operation = false;
  all_calls_well_formed = true;
  expected_communicator = communicator;
  observed = {};
}

void record(void const* send_buffer,
            void* receive_buffer,
            int count,
            MPI_Datatype datatype,
            MPI_Op operation,
            MPI_Comm communicator) noexcept {
  if (!active) {
    return;
  }
  ++call_count;
  all_calls_well_formed = all_calls_well_formed && send_buffer != nullptr &&
                          receive_buffer != nullptr &&
                          send_buffer != receive_buffer && count == 1 &&
                          datatype == MPI_UNSIGNED_LONG_LONG &&
                          communicator == expected_communicator;
  if (operation == MPI_MIN) {
    ++minimum_count;
    return;
  }
  if (operation == MPI_MAX) {
    ++maximum_count;
    return;
  }
  if (operation == MPI_BOR) {
    ++validation_count;
    return;
  }
  if (operation != MPI_SUM) {
    unexpected_operation = true;
    return;
  }
  observed = {
      .count = count,
      .datatype = datatype,
      .operation = operation,
      .communicator = communicator,
      .local_value = send_buffer == nullptr
                         ? parhip::EdgeWeight{}
                         : *static_cast<parhip::EdgeWeight const*>(send_buffer),
      .buffers_are_distinct = send_buffer != nullptr &&
                              receive_buffer != nullptr &&
                              send_buffer != receive_buffer,
  };
}

class activation final {
 public:
  explicit activation(MPI_Comm communicator) noexcept {
    reset(communicator);
    active = true;
  }

  ~activation() noexcept { active = false; }

  activation(activation const&) = delete;
  auto operator=(activation const&) -> activation& = delete;
};
}  // namespace vertex_cut_probe

static_assert(noexcept(vertex_cut_probe::reset(MPI_COMM_NULL)));
static_assert(noexcept(vertex_cut_probe::record(nullptr,
                                                nullptr,
                                                0,
                                                MPI_DATATYPE_NULL,
                                                MPI_OP_NULL,
                                                MPI_COMM_NULL)));

extern "C" int MPI_Allreduce(void const* send_buffer,
                             void* receive_buffer,
                             int count,
                             MPI_Datatype datatype,
                             MPI_Op operation,
                             MPI_Comm communicator) {
  vertex_cut_probe::record(send_buffer, receive_buffer, count, datatype,
                           operation, communicator);
  return PMPI_Allreduce(send_buffer, receive_buffer, count, datatype, operation,
                        communicator);
}
// KAHIP_PMPI_CALLBACK_REGION_END

namespace {
template <typename T>
concept fixed_sum_reducible =
    requires(T value, parhip::mpi::communicator_view communicator) {
      parhip::mpi::all_reduce_sum(value, communicator, "test reduction");
    };

static_assert(fixed_sum_reducible<parhip::EdgeWeight>);
static_assert(!fixed_sum_reducible<bool>);

void build_vertex_cut_fixture(parhip::parallel_graph_access& graph,
                              int rank,
                              int size,
                              std::vector<parhip::PartitionID>& partition) {
  constexpr auto labels_by_node = std::array{
      std::array<parhip::PartitionID, 3>{0, 0, 1},
      std::array<parhip::PartitionID, 3>{0, 1, 1},
      std::array<parhip::PartitionID, 3>{2, 2, 2},
  };
  constexpr auto nodes_per_active_rank = parhip::NodeID{3};
  constexpr auto edges_per_active_rank = parhip::EdgeID{9};
  auto const local_nodes = rank == 0 ? parhip::NodeID{} : nodes_per_active_rank;
  auto const local_edges = rank == 0 ? parhip::EdgeID{} : edges_per_active_rank;
  auto const global_nodes =
      static_cast<parhip::NodeID>(size - 1) * nodes_per_active_rank;
  auto const global_edges =
      static_cast<parhip::EdgeID>(size - 1) * edges_per_active_rank;

  graph.start_construction(local_nodes, local_edges, global_nodes, global_edges,
                           false);
  auto ranges = std::vector<parhip::NodeID>(static_cast<std::size_t>(size) + 1);
  for (int index = 0; index <= size; ++index) {
    ranges[static_cast<std::size_t>(index)] =
        index == 0
            ? parhip::NodeID{}
            : static_cast<parhip::NodeID>(index - 1) * nodes_per_active_rank;
  }
  auto const from = ranges[static_cast<std::size_t>(rank)];
  auto const to =
      local_nodes == 0 ? from : ranges[static_cast<std::size_t>(rank) + 1] - 1;
  graph.set_range(from, to);
  graph.set_range_array(ranges);

  if (rank != 0) {
    for (auto const& labels : labels_by_node) {
      auto const node = graph.new_node();
      graph.setNodeWeight(node, 1);
      for (auto const block : labels) {
        partition.push_back(block);
        auto const edge = graph.new_edge(node, from + node);
        graph.setEdgeWeight(edge, 1);
      }
    }
  }
  graph.finish_construction();
}
}  // namespace

TEST_CASE("vertex cut is summed exactly onto every rank") {
  auto world_rank = -1;
  auto world_size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &world_rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &world_size) == MPI_SUCCESS);
  REQUIRE(world_size >= 1);
  REQUIRE(world_size <= 5);

  auto communicator = MPI_COMM_NULL;
  REQUIRE(MPI_Comm_split(MPI_COMM_WORLD, 0, world_size - world_rank,
                         &communicator) == MPI_SUCCESS);
  REQUIRE(communicator != MPI_COMM_NULL);

  auto rank = -1;
  REQUIRE(MPI_Comm_rank(communicator, &rank) == MPI_SUCCESS);

  constexpr auto expected_by_size =
      std::array<parhip::EdgeWeight, 5>{0, 2, 4, 6, 8};
  auto result = parhip::EdgeWeight{};
  auto observed = vertex_cut_probe::observation{};
  auto operation_count = 0;
  auto minimum_count = 0;
  auto maximum_count = 0;
  auto validation_count = 0;
  auto unexpected_operation = false;
  auto all_calls_well_formed = false;
  {
    auto graph = parhip::parallel_graph_access{communicator};
    auto partition = std::vector<parhip::PartitionID>{};
    build_vertex_cut_fixture(graph, rank, world_size, partition);
    auto splitter = parhip::dspac{
        graph, communicator, std::numeric_limits<parhip::EdgeWeight>::max()};

    {
      vertex_cut_probe::activation const probe{communicator};
      result = splitter.calculate_vertex_cut(3, partition);
      observed = vertex_cut_probe::observed;
      operation_count = vertex_cut_probe::call_count;
      minimum_count = vertex_cut_probe::minimum_count;
      maximum_count = vertex_cut_probe::maximum_count;
      validation_count = vertex_cut_probe::validation_count;
      unexpected_operation = vertex_cut_probe::unexpected_operation;
      all_calls_well_formed = vertex_cut_probe::all_calls_well_formed;
    }
  }

  REQUIRE(result == expected_by_size[static_cast<std::size_t>(world_size - 1)]);
  REQUIRE(operation_count == 4);
  REQUIRE(minimum_count == 1);
  REQUIRE(maximum_count == 1);
  REQUIRE(validation_count == 1);
  REQUIRE_FALSE(unexpected_operation);
  REQUIRE(all_calls_well_formed);
  REQUIRE(observed.count == 1);
  REQUIRE(observed.datatype == MPI_UNSIGNED_LONG_LONG);
  REQUIRE(observed.operation == MPI_SUM);
  REQUIRE(observed.communicator == communicator);
  REQUIRE(observed.local_value == (rank == 0 ? 0 : 2));
  REQUIRE(observed.buffers_are_distinct);

  REQUIRE(MPI_Comm_free(&communicator) == MPI_SUCCESS);
}
