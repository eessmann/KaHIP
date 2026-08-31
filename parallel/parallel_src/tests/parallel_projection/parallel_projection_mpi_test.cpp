#include <mpi.h>

#include <array>
#include <optional>
#include <unordered_map>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "data_structure/parallel_graph_access.h"
#include "communication/mpi_trace.h"
#include "kahip_mpi_capabilities.h"
#include "parallel_label_compress/parallel_label_compress.h"
#include "parallel_contraction_projection/parallel_block_down_propagation.h"
#include "parallel_contraction_projection/parallel_projection.h"

namespace protocol_probe {
inline bool active = false;
inline int all_to_all_v_calls = 0;
inline int all_to_all_v_c_calls = 0;
inline int isend_calls = 0;
inline int probe_calls = 0;

[[nodiscard]] auto dense_payload_collective_calls() -> int {
  return all_to_all_v_calls + all_to_all_v_c_calls;
}
}  // namespace protocol_probe

extern "C" int MPI_Alltoallv(const void* send_buffer,
                             const int send_counts[],
                             const int send_displacements[],
                             MPI_Datatype send_datatype,
                             void* receive_buffer,
                             const int receive_counts[],
                             const int receive_displacements[],
                             MPI_Datatype receive_datatype,
                             MPI_Comm communicator) {
  if (protocol_probe::active) {
    ++protocol_probe::all_to_all_v_calls;
  }
  return PMPI_Alltoallv(send_buffer,
                        send_counts,
                        send_displacements,
                        send_datatype,
                        receive_buffer,
                        receive_counts,
                        receive_displacements,
                        receive_datatype,
                        communicator);
}

#if KAHIP_HAVE_MPI_ALLTOALLV_C
extern "C" int MPI_Alltoallv_c(const void* send_buffer,
                               const MPI_Count send_counts[],
                               const MPI_Aint send_displacements[],
                               MPI_Datatype send_datatype,
                               void* receive_buffer,
                               const MPI_Count receive_counts[],
                               const MPI_Aint receive_displacements[],
                               MPI_Datatype receive_datatype,
                               MPI_Comm communicator) {
  if (protocol_probe::active) {
    ++protocol_probe::all_to_all_v_c_calls;
  }
  return PMPI_Alltoallv_c(send_buffer,
                          send_counts,
                          send_displacements,
                          send_datatype,
                          receive_buffer,
                          receive_counts,
                          receive_displacements,
                          receive_datatype,
                          communicator);
}
#endif

extern "C" int MPI_Isend(const void* buffer,
                         int count,
                         MPI_Datatype datatype,
                         int destination,
                         int tag,
                         MPI_Comm communicator,
                         MPI_Request* request) {
  if (protocol_probe::active) {
    ++protocol_probe::isend_calls;
  }
  return PMPI_Isend(
      buffer, count, datatype, destination, tag, communicator, request);
}

extern "C" int MPI_Probe(int source,
                         int tag,
                         MPI_Comm communicator,
                         MPI_Status* status) {
  if (protocol_probe::active) {
    ++protocol_probe::probe_calls;
  }
  return PMPI_Probe(source, tag, communicator, status);
}

namespace {
void build_edgeless_graph(parhip::parallel_graph_access& graph,
                          int rank,
                          std::array<parhip::NodeID, 2> labels) {
  constexpr parhip::NodeID global_nodes = 4;
  auto const first = static_cast<parhip::NodeID>(rank) * 2;
  graph.start_construction(2, 0, global_nodes, 0, false);
  graph.set_range(first, first + 1);
  auto ranges = std::vector<parhip::NodeID>{0, 2, 4};
  graph.set_range_array(ranges);
  for (parhip::NodeID index = 0; index < 2; ++index) {
    auto const node = graph.new_node();
    graph.setNodeWeight(node, 1);
    graph.setNodeLabel(node, labels[static_cast<std::size_t>(index)]);
    graph.setSecondPartitionIndex(node, 0);
  }
  graph.finish_construction();
}

void build_two_rank_cross_edge(parhip::parallel_graph_access& graph,
                               int rank) {
  constexpr parhip::NodeID global_nodes = 2;
  constexpr parhip::EdgeID global_edges = 2;
  graph.start_construction(1, 1, global_nodes, global_edges);
  graph.set_range(static_cast<parhip::NodeID>(rank),
                  static_cast<parhip::NodeID>(rank));
  auto ranges = std::vector<parhip::NodeID>{0, 1, 2};
  graph.set_range_array(ranges);

  auto const node = graph.new_node();
  graph.setNodeWeight(node, 1);
  graph.setNodeLabel(node, static_cast<parhip::NodeID>(rank));
  graph.setSecondPartitionIndex(node, 0);
  auto const edge = graph.new_edge(
      node, static_cast<parhip::NodeID>(1 - rank));
  graph.setEdgeWeight(edge, 1);
  graph.finish_construction();
}
}  // namespace

TEST_CASE("outer label iterations distinguish ghost exchanges after reset",
          "[mpi][trace][ghost-update]") {
  int rank = 0;
  int size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  REQUIRE(size == 2);

  parhip::parallel_graph_access graph{MPI_COMM_WORLD};
  build_two_rank_cross_edge(graph, rank);
  parhip::PPartitionConfig config{};
  config.k = 1;
  config.total_num_labels = 2;
  config.label_iterations = 4;
  config.upper_bound_cluster = 2;
  config.node_ordering = parhip::NodeOrderingType::DEGREE_NODEORDERING;
  config.vcycle = false;
  graph.init_balance_management(config);

  parhip::mpi::trace::reset();
  parhip::mpi::trace::set_active(true);
  KAHIP_MPI_TRACE_SET_HIERARCHY(
      5, 2, parhip::mpi::trace::epoch::coarsening);
  parhip::parallel_label_compress<
      std::unordered_map<parhip::NodeID, parhip::NodeWeight>>{}
      .perform_parallel_label_compression(config, graph, false);

#if KAHIP_ENABLE_MPI_TRACE
  auto const owner = 1 - rank;
  auto const repeated_label = static_cast<parhip::NodeID>(owner);
  auto const common_prefix =
      std::string{"ghost-update cycle=5 level=2 epoch=coarsening iteration="};
  auto const common_suffix =
      " round=1 global=" + std::to_string(owner) +
      " owner=" + std::to_string(owner) + " requester=- receiver=" +
      std::to_string(rank) + " key=label label=" +
      std::to_string(repeated_label) + "\n";
  auto const trace = parhip::mpi::trace::canonical_text(
      parhip::mpi::trace::snapshot());
  INFO(trace);
  REQUIRE(trace.find(common_prefix + "1" + common_suffix) !=
          std::string::npos);
  REQUIRE(trace.find(common_prefix + "3" + common_suffix) !=
          std::string::npos);
#else
  REQUIRE(parhip::mpi::trace::snapshot().empty());
#endif
}

TEST_CASE("trace run ID mismatch fails collectively", "[mpi][trace][run-id]") {
  int rank = 0;
  int size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  REQUIRE(size == 2);

#if KAHIP_ENABLE_MPI_TRACE
  auto failed = false;
  try {
    static_cast<void>(parhip::mpi::trace::resolve_run_id_collectively(
        MPI_COMM_WORLD,
        std::optional<std::string>{rank == 0 ? "rank-zero" : "rank-one"}));
  } catch (std::runtime_error const&) {
    failed = true;
  }
  REQUIRE(failed);
#else
  SUCCEED("collective run-ID resolution is compiled out with tracing");
#endif
}

TEST_CASE("trace run ID fallback is common and explicit IDs stay deterministic",
          "[mpi][trace][run-id]") {
  int rank = 0;
  int size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  REQUIRE(size == 2);

#if KAHIP_ENABLE_MPI_TRACE
  auto const generated = parhip::mpi::trace::resolve_run_id_collectively(
      MPI_COMM_WORLD, std::nullopt);
  REQUIRE(!generated.empty());

  auto root_length = rank == 0 ? static_cast<int>(generated.size()) : 0;
  REQUIRE(MPI_Bcast(&root_length, 1, MPI_INT, 0, MPI_COMM_WORLD) ==
          MPI_SUCCESS);
  auto root_value = std::string(static_cast<std::size_t>(root_length), '\0');
  if (rank == 0) {
    root_value = generated;
  }
  REQUIRE(MPI_Bcast(root_value.data(), root_length, MPI_CHAR, 0,
                    MPI_COMM_WORLD) == MPI_SUCCESS);
  REQUIRE(generated == root_value);

  auto const explicit_id = parhip::mpi::trace::resolve_run_id_collectively(
      MPI_COMM_WORLD, std::optional<std::string>{"oracle-fixture"});
  REQUIRE(explicit_id == "oracle-fixture");
#else
  SUCCEED("collective run-ID resolution is compiled out with tracing");
#endif
}

TEST_CASE("projection uses two dense exchanges and correlates stable request IDs",
          "[mpi][projection]") {
  int rank = 0;
  int size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  REQUIRE(size == 2);

  parhip::parallel_graph_access finer{MPI_COMM_WORLD};
  parhip::parallel_graph_access coarser{MPI_COMM_WORLD};
  auto const coarse_labels = rank == 0
                                 ? std::array<parhip::NodeID, 2>{100, 101}
                                 : std::array<parhip::NodeID, 2>{202, 203};
  build_edgeless_graph(finer, rank, {0, 0});
  build_edgeless_graph(coarser, rank, coarse_labels);

  finer.allocate_node_to_cnode();
  if (rank == 0) {
    // Fine-node order and coarse-node order disagree.  A reply sorted by its
    // stable request ID therefore cannot be applied by arrival position.
    finer.setCNode(0, 3);
    finer.setCNode(1, 2);
  } else {
    finer.setCNode(0, 1);
    finer.setCNode(1, 0);
  }

  protocol_probe::all_to_all_v_calls = 0;
  protocol_probe::all_to_all_v_c_calls = 0;
  protocol_probe::isend_calls = 0;
  protocol_probe::probe_calls = 0;
  parhip::mpi::trace::reset();
  parhip::mpi::trace::set_active(true);
  KAHIP_MPI_TRACE_SET_HIERARCHY(
      7, 3, parhip::mpi::trace::epoch::projection);
  protocol_probe::active = true;
  parhip::parallel_projection{}.parallel_project(
      MPI_COMM_WORLD, finer, coarser);
  protocol_probe::active = false;

  auto const expected = rank == 0
                            ? std::array<parhip::NodeID, 2>{203, 202}
                            : std::array<parhip::NodeID, 2>{101, 100};
  REQUIRE(finer.getNodeLabel(0) == expected[0]);
  REQUIRE(finer.getNodeLabel(1) == expected[1]);
  CAPTURE(protocol_probe::all_to_all_v_calls,
          protocol_probe::all_to_all_v_c_calls,
          protocol_probe::isend_calls,
          protocol_probe::probe_calls);
  REQUIRE(protocol_probe::dense_payload_collective_calls() == 2);
  REQUIRE(protocol_probe::isend_calls == 0);
  REQUIRE(protocol_probe::probe_calls == 0);

#if KAHIP_ENABLE_MPI_TRACE
  auto const expected_trace = rank == 0
      ? std::string{
            "kahip-mpi-trace-v3 upstream="
            "5935f349f65f1788a9b68fcf6d853e698d86956d\n"
            "projection-request cycle=7 level=3 epoch=projection iteration=0 round=0 "
            "global=2 owner=1 requester=0 receiver=1 key=request:1 "
            "requester=0 owner=1\n"
            "projection-request cycle=7 level=3 epoch=projection iteration=0 round=0 "
            "global=3 owner=1 requester=0 receiver=1 key=request:0 "
            "requester=0 owner=1\n"
            "projection-reply cycle=7 level=3 epoch=projection iteration=0 round=0 "
            "global=0 owner=0 requester=1 receiver=1 key=request:3 "
            "requester=1 owner=0 label=100\n"
            "projection-reply cycle=7 level=3 epoch=projection iteration=0 round=0 "
            "global=1 owner=0 requester=1 receiver=1 key=request:2 "
            "requester=1 owner=0 label=101\n"}
      : std::string{
            "kahip-mpi-trace-v3 upstream="
            "5935f349f65f1788a9b68fcf6d853e698d86956d\n"
            "projection-request cycle=7 level=3 epoch=projection iteration=0 round=0 "
            "global=0 owner=0 requester=1 receiver=0 key=request:3 "
            "requester=1 owner=0\n"
            "projection-request cycle=7 level=3 epoch=projection iteration=0 round=0 "
            "global=1 owner=0 requester=1 receiver=0 key=request:2 "
            "requester=1 owner=0\n"
            "projection-reply cycle=7 level=3 epoch=projection iteration=0 round=0 "
            "global=2 owner=1 requester=0 receiver=0 key=request:1 "
            "requester=0 owner=1 label=202\n"
            "projection-reply cycle=7 level=3 epoch=projection iteration=0 round=0 "
            "global=3 owner=1 requester=0 receiver=0 key=request:0 "
            "requester=0 owner=1 label=203\n"};
  REQUIRE(parhip::mpi::trace::canonical_text(
              parhip::mpi::trace::snapshot()) == expected_trace);
#else
  REQUIRE(parhip::mpi::trace::snapshot().empty());
#endif
}

TEST_CASE("vcycle block-down hook emits canonical records",
          "[mpi][trace][block-propagation]") {
  int rank = 0;
  int size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  REQUIRE(size == 2);

  parhip::parallel_graph_access finer{MPI_COMM_WORLD};
  parhip::parallel_graph_access coarser{MPI_COMM_WORLD};
  build_edgeless_graph(finer, rank, {0, 0});
  build_edgeless_graph(coarser, rank, {0, 0});
  finer.allocate_node_to_cnode();
  if (rank == 0) {
    finer.setCNode(0, 0);
    finer.setSecondPartitionIndex(0, 10);
    finer.setCNode(1, 2);
    finer.setSecondPartitionIndex(1, 12);
  } else {
    finer.setCNode(0, 1);
    finer.setSecondPartitionIndex(0, 11);
    finer.setCNode(1, 3);
    finer.setSecondPartitionIndex(1, 13);
  }

  parhip::PPartitionConfig config{};
  parhip::mpi::trace::reset();
  parhip::mpi::trace::set_active(true);
  KAHIP_MPI_TRACE_SET_HIERARCHY(
      2, 4, parhip::mpi::trace::epoch::contraction);
  parhip::parallel_block_down_propagation{}.propagate_block_down(
      MPI_COMM_WORLD, config, finer, coarser);

  auto const expected_blocks = rank == 0
                                   ? std::array<parhip::NodeID, 2>{10, 11}
                                   : std::array<parhip::NodeID, 2>{12, 13};
  REQUIRE(coarser.getSecondPartitionIndex(0) == expected_blocks[0]);
  REQUIRE(coarser.getSecondPartitionIndex(1) == expected_blocks[1]);
#if KAHIP_ENABLE_MPI_TRACE
  auto const first_global = static_cast<parhip::NodeID>(rank) * 2;
  auto const expected_trace =
      std::string{"kahip-mpi-trace-v3 upstream="
                  "5935f349f65f1788a9b68fcf6d853e698d86956d\n"} +
      "block-propagation cycle=2 level=4 epoch=contraction iteration=0 round=0 global=" +
      std::to_string(first_global) + " owner=" + std::to_string(rank) +
      " requester=- receiver=" + std::to_string(rank) +
      " key=block block=" + std::to_string(expected_blocks[0]) + "\n" +
      "block-propagation cycle=2 level=4 epoch=contraction iteration=0 round=0 global=" +
      std::to_string(first_global + 1) + " owner=" + std::to_string(rank) +
      " requester=- receiver=" + std::to_string(rank) +
      " key=block block=" + std::to_string(expected_blocks[1]) + "\n";
  REQUIRE(parhip::mpi::trace::canonical_text(
              parhip::mpi::trace::snapshot()) == expected_trace);
#else
  REQUIRE(parhip::mpi::trace::snapshot().empty());
#endif
}
