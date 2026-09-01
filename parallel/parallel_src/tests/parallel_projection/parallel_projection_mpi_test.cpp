#include <mpi.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <functional>
#include <optional>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "communication/contiguous_owner_layout.h"
#include "communication/mpi_error.h"
#include "communication/mpi_trace.h"
#include "data_structure/parallel_graph_access.h"
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
inline int recv_calls = 0;
inline std::vector<MPI_Aint> payload_extents;
inline std::vector<int> isend_tags;
inline std::vector<int> probe_tags;
inline std::vector<int> recv_tags;

enum class receive_mutation {
  none,
  projection_request_wrong_owner,
  projection_reply_wrong_coarse_id,
  projection_reply_duplicate_request,
};

inline receive_mutation mutation = receive_mutation::none;
inline int mutation_payload_ordinal = 0;
inline int mutation_target_rank = 0;

void reset() {
  all_to_all_v_calls = 0;
  all_to_all_v_c_calls = 0;
  isend_calls = 0;
  probe_calls = 0;
  recv_calls = 0;
  payload_extents.clear();
  isend_tags.clear();
  probe_tags.clear();
  recv_tags.clear();
  mutation = receive_mutation::none;
  mutation_payload_ordinal = 0;
  mutation_target_rank = 0;
}

[[nodiscard]] auto dense_payload_collective_calls() -> int {
  return all_to_all_v_calls + all_to_all_v_c_calls;
}

void record_payload_extent(MPI_Datatype datatype) {
  MPI_Aint lower_bound = 0;
  MPI_Aint extent = 0;
  REQUIRE(PMPI_Type_get_extent(datatype, &lower_bound, &extent) == MPI_SUCCESS);
  REQUIRE(lower_bound == 0);
  payload_extents.push_back(extent);
}

class scoped_receive_mutation {
public:
  scoped_receive_mutation(receive_mutation selected,
                          int payload_ordinal,
                          int target_rank = 0) noexcept {
    mutation = selected;
    mutation_payload_ordinal = payload_ordinal;
    mutation_target_rank = target_rank;
  }

  ~scoped_receive_mutation() noexcept {
    mutation = receive_mutation::none;
    mutation_payload_ordinal = 0;
    mutation_target_rank = 0;
  }

  scoped_receive_mutation(scoped_receive_mutation const&) = delete;
  auto operator=(scoped_receive_mutation const&)
      -> scoped_receive_mutation& = delete;
};

template <typename Count, typename Displacement>
void mutate_received_payload(int payload_ordinal,
                             void* receive_buffer,
                             Count const receive_counts[],
                             Displacement const receive_displacements[],
                             MPI_Datatype receive_datatype,
                             MPI_Comm communicator) {
  if (mutation == receive_mutation::none ||
      payload_ordinal != mutation_payload_ordinal) {
    return;
  }

  int rank = 0;
  int size = 0;
  REQUIRE(PMPI_Comm_rank(communicator, &rank) == MPI_SUCCESS);
  REQUIRE(PMPI_Comm_size(communicator, &size) == MPI_SUCCESS);
  if (rank != mutation_target_rank) {
    return;
  }

  MPI_Aint lower_bound = 0;
  MPI_Aint extent = 0;
  REQUIRE(PMPI_Type_get_extent(
              receive_datatype, &lower_bound, &extent) == MPI_SUCCESS);
  REQUIRE(lower_bound == 0);
  for (int source = 0; source < size; ++source) {
    if (receive_counts[source] <= 0) {
      continue;
    }
    auto* first_record = static_cast<std::byte*>(receive_buffer) +
                         static_cast<MPI_Aint>(
                             receive_displacements[source]) *
                             extent;
    switch (mutation) {
      case receive_mutation::projection_request_wrong_owner:
        // The target rank owns coarse IDs 0 and 1 in this fixture. ID 2 is
        // in-domain but owned by the sender, so the grouped receiver
        // validation rejects the corrupted request.
        reinterpret_cast<parhip::projection::request*>(first_record)
            ->coarse_global_id = parhip::NodeID{2};
        return;
      case receive_mutation::projection_reply_wrong_coarse_id:
        // Rank 0 requested coarse ID 2 from source 1. ID 3 has the same
        // source owner but is not the coarse ID associated with the request.
        reinterpret_cast<parhip::projection::reply*>(first_record)
            ->coarse_global_id = parhip::NodeID{3};
        return;
      case receive_mutation::projection_reply_duplicate_request: {
        if (receive_counts[source] < 2) {
          continue;
        }
        auto* first = reinterpret_cast<parhip::projection::reply*>(
            first_record);
        auto* second = reinterpret_cast<parhip::projection::reply*>(
            first_record + extent);
        second->request_id = first->request_id;
        second->coarse_global_id = first->coarse_global_id;
        return;
      }
      case receive_mutation::none:
        return;
    }
  }
  FAIL("selected projection receive mutation found no record on its target rank");
}

[[nodiscard]] auto calls_in_tag_phase(std::vector<int> const& tags,
                                      int phase,
                                      int size) -> std::size_t {
  auto const first = phase * size;
  auto const last = (phase + 1) * size;
  return static_cast<std::size_t>(std::ranges::count_if(tags, [&](int tag) {
    return first <= tag && tag < last;
  }));
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
    protocol_probe::record_payload_extent(send_datatype);
  }
  auto const payload_ordinal = protocol_probe::dense_payload_collective_calls();
  auto const result = PMPI_Alltoallv(send_buffer,
                                     send_counts,
                                     send_displacements,
                                     send_datatype,
                                     receive_buffer,
                                     receive_counts,
                                     receive_displacements,
                                     receive_datatype,
                                     communicator);
  if (protocol_probe::active && result == MPI_SUCCESS) {
    protocol_probe::mutate_received_payload(payload_ordinal,
                                            receive_buffer,
                                            receive_counts,
                                            receive_displacements,
                                            receive_datatype,
                                            communicator);
  }
  return result;
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
    protocol_probe::record_payload_extent(send_datatype);
  }
  auto const payload_ordinal = protocol_probe::dense_payload_collective_calls();
  auto const result = PMPI_Alltoallv_c(send_buffer,
                                       send_counts,
                                       send_displacements,
                                       send_datatype,
                                       receive_buffer,
                                       receive_counts,
                                       receive_displacements,
                                       receive_datatype,
                                       communicator);
  if (protocol_probe::active && result == MPI_SUCCESS) {
    protocol_probe::mutate_received_payload(payload_ordinal,
                                            receive_buffer,
                                            receive_counts,
                                            receive_displacements,
                                            receive_datatype,
                                            communicator);
  }
  return result;
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
    protocol_probe::isend_tags.push_back(tag);
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
    protocol_probe::probe_tags.push_back(tag);
  }
  return PMPI_Probe(source, tag, communicator, status);
}

extern "C" int MPI_Recv(void* buffer,
                        int count,
                        MPI_Datatype datatype,
                        int source,
                        int tag,
                        MPI_Comm communicator,
                        MPI_Status* status) {
  if (protocol_probe::active) {
    ++protocol_probe::recv_calls;
    protocol_probe::recv_tags.push_back(tag);
  }
  return PMPI_Recv(
      buffer, count, datatype, source, tag, communicator, status);
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

void build_block_finer(parhip::parallel_graph_access& graph,
                       int rank,
                       int size) {
  auto const global_nodes = static_cast<parhip::NodeID>(4 * size);
  auto const first = static_cast<parhip::NodeID>(4 * rank);
  graph.start_construction(4, 0, global_nodes, 0, false);
  graph.set_range(first, first + 3);
  auto ranges = std::vector<parhip::NodeID>(
      static_cast<std::size_t>(size) + 1);
  for (int pe = 0; pe <= size; ++pe) {
    ranges[static_cast<std::size_t>(pe)] =
        static_cast<parhip::NodeID>(4 * pe);
  }
  graph.set_range_array(ranges);
  for (parhip::NodeID index = 0; index < 4; ++index) {
    auto const node = graph.new_node();
    graph.setNodeWeight(node, 1);
    graph.setNodeLabel(node, 0);
    graph.setSecondPartitionIndex(node, 0);
  }
  graph.finish_construction();
}

void build_block_coarser(parhip::parallel_graph_access& graph,
                         int rank,
                         int size) {
  constexpr auto global_nodes = parhip::NodeID{4};
  auto const ownership = parhip::mpi::contiguous_owner_layout<parhip::NodeID>{
      global_nodes, static_cast<std::size_t>(size)};
  auto ranges = std::vector<parhip::NodeID>(
      static_cast<std::size_t>(size) + 1);
  for (int pe = 0; pe <= size; ++pe) {
    ranges[static_cast<std::size_t>(pe)] =
        ownership.boundary(static_cast<std::size_t>(pe));
  }
  auto const first = ranges[static_cast<std::size_t>(rank)];
  auto const end = ranges[static_cast<std::size_t>(rank + 1)];
  auto const local_nodes = end - first;
  graph.start_construction(local_nodes, 0, global_nodes, 0, false);
  graph.set_range(first, local_nodes == 0 ? first : end - 1);
  graph.set_range_array(ranges);
  for (parhip::NodeID index = 0; index < local_nodes; ++index) {
    auto const node = graph.new_node();
    graph.setNodeWeight(node, 1);
    graph.setNodeLabel(node, 0);
    graph.setSecondPartitionIndex(node, 0);
  }
  graph.finish_construction();
}

void build_empty_graph_with_global_count(
    parhip::parallel_graph_access& graph,
    parhip::NodeID global_nodes,
    int size) {
  graph.start_construction(0, 0, global_nodes, 0, false);
  graph.set_range(0, 0);
  auto ranges = std::vector<parhip::NodeID>(
      static_cast<std::size_t>(size) + 1, parhip::NodeID{0});
  graph.set_range_array(ranges);
  graph.finish_construction();
}

void build_projection_coarser(parhip::parallel_graph_access& graph,
                              int rank,
                              int size,
                              parhip::NodeID global_nodes) {
  auto const ownership = parhip::mpi::contiguous_owner_layout<parhip::NodeID>{
      global_nodes, static_cast<std::size_t>(size)};
  auto ranges = std::vector<parhip::NodeID>(
      static_cast<std::size_t>(size) + 1);
  for (int pe = 0; pe <= size; ++pe) {
    ranges[static_cast<std::size_t>(pe)] =
        ownership.boundary(static_cast<std::size_t>(pe));
  }
  auto const first = ranges[static_cast<std::size_t>(rank)];
  auto const end = ranges[static_cast<std::size_t>(rank + 1)];
  auto const local_nodes = end - first;
  graph.start_construction(local_nodes, 0, global_nodes, 0, false);
  graph.set_range(first, local_nodes == 0 ? first : end - 1);
  graph.set_range_array(ranges);
  for (auto global = first; global < end; ++global) {
    auto const node = graph.new_node();
    graph.setNodeWeight(node, 1);
    graph.setNodeLabel(node, parhip::NodeID{100} + global);
    graph.setSecondPartitionIndex(node, 0);
  }
  graph.finish_construction();
}

template <std::size_t Count>
void build_projection_finer(
    parhip::parallel_graph_access& graph,
    int rank,
    int size,
    std::array<parhip::NodeID, Count> const& coarse_nodes) {
  auto const local_nodes = static_cast<parhip::NodeID>(Count);
  auto const global_nodes =
      local_nodes * static_cast<parhip::NodeID>(size);
  auto const first = local_nodes * static_cast<parhip::NodeID>(rank);
  graph.start_construction(local_nodes, 0, global_nodes, 0, false);
  graph.set_range(first, local_nodes == 0 ? first : first + local_nodes - 1);
  auto ranges = std::vector<parhip::NodeID>(
      static_cast<std::size_t>(size) + 1);
  for (int pe = 0; pe <= size; ++pe) {
    ranges[static_cast<std::size_t>(pe)] =
        local_nodes * static_cast<parhip::NodeID>(pe);
  }
  graph.set_range_array(ranges);
  for (std::size_t index = 0; index < Count; ++index) {
    auto const node = graph.new_node();
    graph.setNodeWeight(node, 1);
    graph.setNodeLabel(
        node, parhip::NodeID{900} + first + static_cast<parhip::NodeID>(index));
    graph.setSecondPartitionIndex(node, 0);
  }
  graph.finish_construction();
  graph.allocate_node_to_cnode();
  for (std::size_t index = 0; index < Count; ++index) {
    graph.setCNode(static_cast<parhip::NodeID>(index), coarse_nodes[index]);
  }
}

template <std::size_t Count>
void require_projection_labels_unchanged(
    parhip::parallel_graph_access& finer,
    int rank) {
  auto const first = static_cast<parhip::NodeID>(Count) *
                     static_cast<parhip::NodeID>(rank);
  for (std::size_t index = 0; index < Count; ++index) {
    REQUIRE(finer.getNodeLabel(static_cast<parhip::NodeID>(index)) ==
            parhip::NodeID{900} + first +
                static_cast<parhip::NodeID>(index));
  }
}

template <typename Operation>
void require_collective_validation_failure(Operation&& operation,
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
    context_matches = error.context().find(expected_context) !=
                              std::string_view::npos
                          ? 1
                          : 0;
  } catch (std::exception const&) {
    caught = 1;
  }

  auto caught_by_all = 0;
  auto structured_by_all = 0;
  auto context_matches_all = 0;
  REQUIRE(MPI_Allreduce(
              &caught, &caught_by_all, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD) ==
          MPI_SUCCESS);
  REQUIRE(MPI_Allreduce(&structured,
                        &structured_by_all,
                        1,
                        MPI_INT,
                        MPI_SUM,
                        MPI_COMM_WORLD) == MPI_SUCCESS);
  REQUIRE(MPI_Allreduce(&context_matches,
                        &context_matches_all,
                        1,
                        MPI_INT,
                        MPI_SUM,
                        MPI_COMM_WORLD) == MPI_SUCCESS);
  REQUIRE(caught_by_all == size);
  REQUIRE(structured_by_all == size);
  REQUIRE(context_matches_all == size);
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

  protocol_probe::reset();
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

TEST_CASE("projection rejects an empty-payload coarse-count mismatch collectively",
          "[mpi][projection][failure][domain]") {
  int rank = 0;
  int size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  if (size != 3) {
    return;
  }

  parhip::parallel_graph_access finer{MPI_COMM_WORLD};
  parhip::parallel_graph_access coarser{MPI_COMM_WORLD};
  build_projection_finer(finer, rank, size,
                         std::array<parhip::NodeID, 0>{});
  build_empty_graph_with_global_count(
      coarser, rank == 0 ? parhip::NodeID{2} : parhip::NodeID{3}, size);

  protocol_probe::reset();
  protocol_probe::active = true;
  require_collective_validation_failure(
      [&] {
        parhip::parallel_projection{}.parallel_project(
            MPI_COMM_WORLD, finer, coarser);
      },
      "projection coarse node count agreement failed",
      size);
  protocol_probe::active = false;
  REQUIRE(protocol_probe::dense_payload_collective_calls() == 0);
}

TEST_CASE("zero-node projection performs two empty dense exchanges",
          "[mpi][projection][zero]") {
  int rank = 0;
  int size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  if (size != 3) {
    return;
  }

  parhip::parallel_graph_access finer{MPI_COMM_WORLD};
  parhip::parallel_graph_access coarser{MPI_COMM_WORLD};
  build_projection_finer(finer, rank, size,
                         std::array<parhip::NodeID, 0>{});
  build_projection_coarser(coarser, rank, size, 0);

  protocol_probe::reset();
  protocol_probe::active = true;
  parhip::parallel_projection{}.parallel_project(
      MPI_COMM_WORLD, finer, coarser);
  protocol_probe::active = false;

  REQUIRE(finer.number_of_local_nodes() == 0);
  REQUIRE(protocol_probe::dense_payload_collective_calls() == 2);
  REQUIRE(protocol_probe::isend_calls == 0);
  REQUIRE(protocol_probe::probe_calls == 0);
  REQUIRE(protocol_probe::recv_calls == 0);
}

TEST_CASE("projection rejects a tail coarse node before exchanging or mutating",
          "[mpi][projection][failure][domain]") {
  int rank = 0;
  int size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  if (size != 3) {
    return;
  }

  auto const coarse_nodes = rank == 0
      ? std::array<parhip::NodeID, 2>{5, 0}
      : rank == 1 ? std::array<parhip::NodeID, 2>{2, 3}
                  : std::array<parhip::NodeID, 2>{4, 4};
  parhip::parallel_graph_access finer{MPI_COMM_WORLD};
  parhip::parallel_graph_access coarser{MPI_COMM_WORLD};
  build_projection_finer(finer, rank, size, coarse_nodes);
  build_projection_coarser(coarser, rank, size, 5);

  protocol_probe::reset();
  protocol_probe::active = true;
  require_collective_validation_failure(
      [&] {
        parhip::parallel_projection{}.parallel_project(
            MPI_COMM_WORLD, finer, coarser);
      },
      "projection local coarse-node validation failed",
      size);
  protocol_probe::active = false;

  require_projection_labels_unchanged<2>(finer, rank);
  REQUIRE(protocol_probe::dense_payload_collective_calls() == 0);
}

TEST_CASE("projection routes an uneven coarse domain by exact ownership",
          "[mpi][projection][ownership]") {
  int rank = 0;
  int size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  if (size != 3) {
    return;
  }

  auto const coarse_nodes = rank == 0
      ? std::array<parhip::NodeID, 2>{0, 4}
      : rank == 1 ? std::array<parhip::NodeID, 2>{2, 1}
                  : std::array<parhip::NodeID, 2>{4, 3};
  parhip::parallel_graph_access finer{MPI_COMM_WORLD};
  parhip::parallel_graph_access coarser{MPI_COMM_WORLD};
  build_projection_finer(finer, rank, size, coarse_nodes);
  build_projection_coarser(coarser, rank, size, 5);

  protocol_probe::reset();
  protocol_probe::active = true;
  parhip::parallel_projection{}.parallel_project(
      MPI_COMM_WORLD, finer, coarser);
  protocol_probe::active = false;

  for (std::size_t index = 0; index < coarse_nodes.size(); ++index) {
    REQUIRE(finer.getNodeLabel(static_cast<parhip::NodeID>(index)) ==
            parhip::NodeID{100} + coarse_nodes[index]);
  }
  REQUIRE(protocol_probe::dense_payload_collective_calls() == 2);
}

TEST_CASE("projection request corruption fails before replies and preserves labels",
          "[mpi][projection][failure][receive]") {
  int rank = 0;
  int size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  if (size != 2) {
    return;
  }

  auto const coarse_nodes = rank == 0
      ? std::array<parhip::NodeID, 2>{0, 2}
      : std::array<parhip::NodeID, 2>{2, 0};
  parhip::parallel_graph_access finer{MPI_COMM_WORLD};
  parhip::parallel_graph_access coarser{MPI_COMM_WORLD};
  build_projection_finer(finer, rank, size, coarse_nodes);
  build_projection_coarser(coarser, rank, size, 4);

  protocol_probe::reset();
  parhip::mpi::trace::reset();
  parhip::mpi::trace::set_active(true);
  protocol_probe::scoped_receive_mutation mutation{
      protocol_probe::receive_mutation::projection_request_wrong_owner, 1};
  protocol_probe::active = true;
  require_collective_validation_failure(
      [&] {
        parhip::parallel_projection{}.parallel_project(
            MPI_COMM_WORLD, finer, coarser);
      },
      "projection request received validation failed",
      size);
  protocol_probe::active = false;

  require_projection_labels_unchanged<2>(finer, rank);
  REQUIRE(protocol_probe::dense_payload_collective_calls() == 1);
  REQUIRE(parhip::mpi::trace::snapshot().empty());
  parhip::mpi::trace::set_active(false);
}

TEST_CASE("projection reply corruption fails transactionally",
          "[mpi][projection][failure][receive]") {
  int rank = 0;
  int size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  if (size != 2) {
    return;
  }

  auto const coarse_nodes = rank == 0
      ? std::array<parhip::NodeID, 2>{0, 2}
      : std::array<parhip::NodeID, 2>{2, 0};
  parhip::parallel_graph_access finer{MPI_COMM_WORLD};
  parhip::parallel_graph_access coarser{MPI_COMM_WORLD};
  build_projection_finer(finer, rank, size, coarse_nodes);
  build_projection_coarser(coarser, rank, size, 4);

  protocol_probe::reset();
  parhip::mpi::trace::reset();
  parhip::mpi::trace::set_active(true);
  protocol_probe::scoped_receive_mutation mutation{
      protocol_probe::receive_mutation::projection_reply_wrong_coarse_id, 2};
  protocol_probe::active = true;
  require_collective_validation_failure(
      [&] {
        parhip::parallel_projection{}.parallel_project(
            MPI_COMM_WORLD, finer, coarser);
      },
      "projection reply received validation failed",
      size);
  protocol_probe::active = false;

  require_projection_labels_unchanged<2>(finer, rank);
  REQUIRE(protocol_probe::dense_payload_collective_calls() == 2);
  REQUIRE(parhip::mpi::trace::snapshot().empty());
  parhip::mpi::trace::set_active(false);
}

TEST_CASE("projection rejects duplicate replies without partial label writes",
          "[mpi][projection][failure][receive][duplicate]") {
  int rank = 0;
  int size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  if (size != 2) {
    return;
  }

  auto const coarse_nodes = rank == 0
      ? std::array<parhip::NodeID, 2>{2, 3}
      : std::array<parhip::NodeID, 2>{0, 1};
  parhip::parallel_graph_access finer{MPI_COMM_WORLD};
  parhip::parallel_graph_access coarser{MPI_COMM_WORLD};
  build_projection_finer(finer, rank, size, coarse_nodes);
  build_projection_coarser(coarser, rank, size, 4);

  protocol_probe::reset();
  parhip::mpi::trace::reset();
  parhip::mpi::trace::set_active(true);
  protocol_probe::scoped_receive_mutation mutation{
      protocol_probe::receive_mutation::projection_reply_duplicate_request,
      2};
  protocol_probe::active = true;
  require_collective_validation_failure(
      [&] {
        parhip::parallel_projection{}.parallel_project(
            MPI_COMM_WORLD, finer, coarser);
      },
      "projection reply received validation failed",
      size);
  protocol_probe::active = false;

  require_projection_labels_unchanged<2>(finer, rank);
  REQUIRE(protocol_probe::dense_payload_collective_calls() == 2);
  REQUIRE(parhip::mpi::trace::snapshot().empty());
  parhip::mpi::trace::set_active(false);
}

TEST_CASE("block-down dense owner phase uses keyed collective and exact updates",
          "[mpi][trace][block-propagation][owner]") {
  int rank = 0;
  int size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  REQUIRE(size >= 1);
  REQUIRE(size <= 5);

  parhip::parallel_graph_access finer{MPI_COMM_WORLD};
  parhip::parallel_graph_access coarser{MPI_COMM_WORLD};
  build_block_finer(finer, rank, size);
  build_block_coarser(coarser, rank, size);
  finer.allocate_node_to_cnode();
  for (parhip::NodeID node = 0; node < 4; ++node) {
    finer.setCNode(node, node);
    finer.setSecondPartitionIndex(node, 10 + node);
  }

  parhip::PPartitionConfig config{};
  parhip::mpi::trace::reset();
  parhip::mpi::trace::set_active(true);
  KAHIP_MPI_TRACE_SET_HIERARCHY(
      2, 4, parhip::mpi::trace::epoch::contraction);
  protocol_probe::reset();
  protocol_probe::active = true;
  parhip::parallel_block_down_propagation{}.propagate_block_down(
      MPI_COMM_WORLD, config, finer, coarser);
  protocol_probe::active = false;

  auto expected_block = [](parhip::NodeID global) {
    return parhip::NodeID{10} + global;
  };
  for (parhip::NodeID node = 0; node < coarser.number_of_local_nodes();
       ++node) {
    auto const global = coarser.getGlobalID(node);
    REQUIRE(coarser.getSecondPartitionIndex(node) == expected_block(global));
  }

  CAPTURE(protocol_probe::all_to_all_v_calls,
          protocol_probe::all_to_all_v_c_calls,
          protocol_probe::isend_tags,
          protocol_probe::probe_tags,
          protocol_probe::recv_tags,
          protocol_probe::payload_extents);
  REQUIRE(protocol_probe::dense_payload_collective_calls() == 1);
  REQUIRE(protocol_probe::payload_extents ==
          std::vector<MPI_Aint>{
              static_cast<MPI_Aint>(2 * sizeof(parhip::NodeID))});
  REQUIRE(protocol_probe::calls_in_tag_phase(
              protocol_probe::isend_tags, 10, size) == 0);
  REQUIRE(protocol_probe::calls_in_tag_phase(
              protocol_probe::probe_tags, 10, size) == 0);
  REQUIRE(protocol_probe::calls_in_tag_phase(
              protocol_probe::recv_tags, 10, size) == 0);
#if KAHIP_ENABLE_MPI_TRACE
  auto expected_trace =
      std::string{"kahip-mpi-trace-v3 upstream="
                  "5935f349f65f1788a9b68fcf6d853e698d86956d\n"};
  for (parhip::NodeID node = 0; node < coarser.number_of_local_nodes();
       ++node) {
    auto const global = coarser.getGlobalID(node);
    expected_trace +=
        "block-propagation cycle=2 level=4 epoch=contraction iteration=0 round=0 global=" +
        std::to_string(global) + " owner=" + std::to_string(rank) +
        " requester=- receiver=" + std::to_string(rank) +
        " key=block block=" + std::to_string(expected_block(global)) + "\n";
  }
  REQUIRE(parhip::mpi::trace::canonical_text(
              parhip::mpi::trace::snapshot()) == expected_trace);
#else
  REQUIRE(parhip::mpi::trace::snapshot().empty());
#endif
}

TEST_CASE("block-down accepts an identical same-sender duplicate",
          "[mpi][block-propagation][owner][duplicate]") {
  int rank = 0;
  int size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  if (size != 3) {
    return;
  }

  parhip::parallel_graph_access finer{MPI_COMM_WORLD};
  parhip::parallel_graph_access coarser{MPI_COMM_WORLD};
  build_block_finer(finer, rank, size);
  build_block_coarser(coarser, rank, size);
  finer.allocate_node_to_cnode();
  for (parhip::NodeID node = 0; node < 4; ++node) {
    finer.setCNode(node, rank == 0 && node == 1 ? 0 : node);
    finer.setSecondPartitionIndex(
        node, rank == 0 && node == 1 ? 10 : 10 + node);
  }

  parhip::PPartitionConfig config{};
  parhip::parallel_block_down_propagation{}.propagate_block_down(
      MPI_COMM_WORLD, config, finer, coarser);

  for (parhip::NodeID node = 0; node < coarser.number_of_local_nodes();
       ++node) {
    auto const global = coarser.getGlobalID(node);
    REQUIRE(coarser.getSecondPartitionIndex(node) == 10 + global);
  }
}

TEST_CASE("block-down rejects an empty-payload coarse-count mismatch collectively",
          "[mpi][block-propagation][owner][failure][domain]") {
  int rank = 0;
  int size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  if (size != 3) {
    return;
  }

  parhip::parallel_graph_access finer{MPI_COMM_WORLD};
  parhip::parallel_graph_access coarser{MPI_COMM_WORLD};
  build_empty_graph_with_global_count(finer, 0, size);
  build_empty_graph_with_global_count(
      coarser, rank == 0 ? parhip::NodeID{2} : parhip::NodeID{3}, size);
  finer.allocate_node_to_cnode();

  parhip::PPartitionConfig config{};
  require_collective_validation_failure(
      [&] {
        parhip::parallel_block_down_propagation{}.propagate_block_down(
            MPI_COMM_WORLD, config, finer, coarser);
      },
      "block coarse node count agreement failed",
      size);
  REQUIRE(coarser.number_of_local_nodes() == 0);
}

TEST_CASE("block-down rejects a rank-local conflicting coarse block collectively",
          "[mpi][block-propagation][owner][failure]") {
  int rank = 0;
  int size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  if (size != 3) {
    return;
  }

  parhip::parallel_graph_access finer{MPI_COMM_WORLD};
  parhip::parallel_graph_access coarser{MPI_COMM_WORLD};
  build_block_finer(finer, rank, size);
  build_block_coarser(coarser, rank, size);
  finer.allocate_node_to_cnode();
  finer.setCNode(0, 0);
  finer.setCNode(1, 0);
  finer.setCNode(2, 2);
  finer.setCNode(3, 3);
  finer.setSecondPartitionIndex(0, 10);
  finer.setSecondPartitionIndex(1, rank == 0 ? 11 : 10);
  finer.setSecondPartitionIndex(2, 12);
  finer.setSecondPartitionIndex(3, 13);

  parhip::PPartitionConfig config{};
  require_collective_validation_failure(
      [&] {
        parhip::parallel_block_down_propagation{}.propagate_block_down(
            MPI_COMM_WORLD, config, finer, coarser);
      },
      "block update local validation",
      size);
}

TEST_CASE("block-down rejects a tail-padding coarse ID collectively",
          "[mpi][block-propagation][owner][failure]") {
  int rank = 0;
  int size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  if (size != 3) {
    return;
  }

  parhip::parallel_graph_access finer{MPI_COMM_WORLD};
  parhip::parallel_graph_access coarser{MPI_COMM_WORLD};
  build_block_finer(finer, rank, size);
  build_block_coarser(coarser, rank, size);
  finer.allocate_node_to_cnode();
  finer.setCNode(0, 0);
  finer.setCNode(1, rank == 0 ? 4 : 1);
  finer.setCNode(2, 2);
  finer.setCNode(3, 3);
  finer.setSecondPartitionIndex(0, 10);
  finer.setSecondPartitionIndex(1, 11);
  finer.setSecondPartitionIndex(2, 12);
  finer.setSecondPartitionIndex(3, 13);

  parhip::PPartitionConfig config{};
  require_collective_validation_failure(
      [&] {
        parhip::parallel_block_down_propagation{}.propagate_block_down(
            MPI_COMM_WORLD, config, finer, coarser);
      },
      "block update local validation",
      size);
  REQUIRE(coarser.number_of_local_edges() == 0);
}

TEST_CASE("block-down rejects a cross-rank conflicting coarse block collectively",
          "[mpi][block-propagation][owner][failure]") {
  int rank = 0;
  int size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  if (size != 3) {
    return;
  }

  parhip::parallel_graph_access finer{MPI_COMM_WORLD};
  parhip::parallel_graph_access coarser{MPI_COMM_WORLD};
  build_block_finer(finer, rank, size);
  build_block_coarser(coarser, rank, size);
  finer.allocate_node_to_cnode();
  finer.setCNode(0, 0);
  finer.setCNode(1, 1);
  finer.setCNode(2, 2);
  finer.setCNode(3, 3);
  finer.setSecondPartitionIndex(0, rank == 0 ? 11 : 10);
  finer.setSecondPartitionIndex(1, 11);
  finer.setSecondPartitionIndex(2, 12);
  finer.setSecondPartitionIndex(3, 13);

  parhip::PPartitionConfig config{};
  require_collective_validation_failure(
      [&] {
        parhip::parallel_block_down_propagation{}.propagate_block_down(
            MPI_COMM_WORLD, config, finer, coarser);
      },
      "block update received validation",
      size);
}

TEST_CASE("block-down rejects a missing coarse block collectively",
          "[mpi][block-propagation][owner][failure]") {
  int rank = 0;
  int size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  if (size != 3) {
    return;
  }

  parhip::parallel_graph_access finer{MPI_COMM_WORLD};
  parhip::parallel_graph_access coarser{MPI_COMM_WORLD};
  build_block_finer(finer, rank, size);
  build_block_coarser(coarser, rank, size);
  finer.allocate_node_to_cnode();
  for (parhip::NodeID node = 0; node < 4; ++node) {
    finer.setCNode(node, 0);
    finer.setSecondPartitionIndex(node, 10);
  }

  parhip::PPartitionConfig config{};
  require_collective_validation_failure(
      [&] {
        parhip::parallel_block_down_propagation{}.propagate_block_down(
            MPI_COMM_WORLD, config, finer, coarser);
      },
      "block update received validation",
      size);
}
