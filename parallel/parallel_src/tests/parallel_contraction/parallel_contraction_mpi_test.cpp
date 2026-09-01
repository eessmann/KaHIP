//
// Created by Erich Essmann on 16/08/2024.
//
#include <mpi.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <functional>
#include <numeric>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include <catch2/catch_all.hpp>
#include <fmt/format.h>
#include <fmt/ostream.h>
#include <fmt/ranges.h>

#include "kahip_mpi_capabilities.h"
#include "communication/mpi_error.h"
#include "communication/mpi_tools.h"
#include "parallel_contraction_projection/parallel_contraction.h"

using namespace parhip;

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

[[nodiscard]] auto calls_in_tag_phase(std::vector<int> const& tags,
                                      int phase,
                                      int size) -> std::size_t {
  auto const first = phase * size;
  auto const last = (phase + 1) * size;
  return static_cast<std::size_t>(std::ranges::count_if(tags, [&](int tag) {
    return first <= tag && tag < last;
  }));
}

[[nodiscard]] auto payload_calls_with_extent(MPI_Aint extent) -> std::size_t {
  return static_cast<std::size_t>(
      std::ranges::count(payload_extents, extent));
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
    protocol_probe::record_payload_extent(send_datatype);
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

namespace parhip {
struct parallel_contraction_test_access {
  [[nodiscard]] static auto compute_label_mapping(
      MPI_Comm communicator,
      parallel_graph_access& graph)
      -> std::pair<NodeID, std::unordered_map<NodeID, NodeID>> {
    NodeID global_num_distinct_ids = 0;
    std::unordered_map<NodeID, NodeID> label_mapping;
    parallel_contraction contraction;
    contraction.compute_label_mapping(
        communicator, graph, global_num_distinct_ids, label_mapping);
    return {global_num_distinct_ids, std::move(label_mapping)};
  }

  static void redistribute_quotient(
      MPI_Comm communicator,
      hashed_graph& graph,
      std::unordered_map<NodeID, NodeWeight>& node_weights,
      NodeID number_of_cnodes,
      parallel_graph_access& quotient) {
    parallel_contraction contraction;
    contraction.redistribute_hased_graph_and_build_graph_locally(
        communicator, graph, node_weights, number_of_cnodes, quotient);
  }
};
}  // namespace parhip

namespace {
void build_label_fixture(parallel_graph_access& graph, int rank, int size) {
  constexpr auto global_nodes = NodeID{4};
  constexpr auto labels = std::array<NodeID, 4>{3, 1, 3, 0};
  auto ranges = std::vector<NodeID>(static_cast<std::size_t>(size) + 1);
  for (int pe = 0; pe <= size; ++pe) {
    ranges[static_cast<std::size_t>(pe)] =
        (static_cast<NodeID>(pe) * global_nodes) /
        static_cast<NodeID>(size);
  }
  auto const first = ranges[static_cast<std::size_t>(rank)];
  auto const end = ranges[static_cast<std::size_t>(rank + 1)];
  auto const local_nodes = end - first;
  graph.start_construction(local_nodes, 0, global_nodes, 0, false);
  graph.set_range(first, local_nodes == 0 ? first : end - 1);
  graph.set_range_array(ranges);
  for (NodeID global = first; global < end; ++global) {
    auto const node = graph.new_node();
    graph.setNodeWeight(node, 1);
    graph.setNodeLabel(node, labels[static_cast<std::size_t>(global)]);
  }
  graph.finish_construction();
}

void build_empty_label_fixture(parallel_graph_access& graph, int size) {
  graph.start_construction(0, 0, 0, 0, false);
  graph.set_range(0, 0);
  auto ranges = std::vector<NodeID>(static_cast<std::size_t>(size) + 1, 0);
  graph.set_range_array(ranges);
  graph.finish_construction();
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
  } catch (mpi::mpi_error const& error) {
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

TEST_CASE("label mapping uses explicit semantic replies and preserves contiguous IDs",
          "[unit][mpi][contraction][label-mapping]") {
  int rank = 0;
  int size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  REQUIRE(size >= 1);
  REQUIRE(size <= 5);

  parallel_graph_access graph{MPI_COMM_WORLD};
  build_label_fixture(graph, rank, size);

  protocol_probe::reset();
  protocol_probe::active = true;
  auto [global_num_distinct_ids, mapping] =
      parallel_contraction_test_access::compute_label_mapping(
          MPI_COMM_WORLD, graph);
  protocol_probe::active = false;

  REQUIRE(global_num_distinct_ids == 3);
  forall_local_nodes(graph, node) {
    auto const old_label = graph.getNodeLabel(node);
    REQUIRE(mapping.contains(old_label));
    auto const expected = old_label == 0 ? NodeID{0}
                         : old_label == 1 ? NodeID{1}
                                          : NodeID{2};
    REQUIRE(mapping.at(old_label) == expected);
  } endfor

  CAPTURE(protocol_probe::all_to_all_v_calls,
          protocol_probe::all_to_all_v_c_calls,
          protocol_probe::isend_calls,
          protocol_probe::probe_calls,
          protocol_probe::payload_extents);
  REQUIRE(protocol_probe::dense_payload_collective_calls() == 2);
  REQUIRE(protocol_probe::payload_extents ==
          std::vector<MPI_Aint>{static_cast<MPI_Aint>(sizeof(NodeID)),
                                static_cast<MPI_Aint>(2 * sizeof(NodeID))});
  REQUIRE(protocol_probe::isend_calls == 0);
  REQUIRE(protocol_probe::probe_calls == 0);
  REQUIRE(protocol_probe::recv_calls == 0);
}

TEST_CASE("global-zero label mapping keeps empty keyed exchanges exact",
          "[unit][mpi][contraction][label-mapping][zero]") {
  int size = 0;
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  REQUIRE(size >= 1);
  REQUIRE(size <= 5);

  parallel_graph_access graph{MPI_COMM_WORLD};
  build_empty_label_fixture(graph, size);

  protocol_probe::reset();
  protocol_probe::active = true;
  auto [global_num_distinct_ids, mapping] =
      parallel_contraction_test_access::compute_label_mapping(
          MPI_COMM_WORLD, graph);
  protocol_probe::active = false;

  REQUIRE(global_num_distinct_ids == 0);
  REQUIRE(mapping.empty());
  REQUIRE(protocol_probe::dense_payload_collective_calls() == 2);
  REQUIRE(protocol_probe::payload_extents ==
          std::vector<MPI_Aint>{static_cast<MPI_Aint>(sizeof(NodeID)),
                                static_cast<MPI_Aint>(2 * sizeof(NodeID))});
  REQUIRE(protocol_probe::isend_calls == 0);
  REQUIRE(protocol_probe::probe_calls == 0);
  REQUIRE(protocol_probe::recv_calls == 0);
}

TEST_CASE("label mapping rejects an out-of-domain local label collectively",
          "[unit][mpi][contraction][label-mapping][failure]") {
  int rank = 0;
  int size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  if (size != 3) {
    return;
  }

  parallel_graph_access graph{MPI_COMM_WORLD};
  build_label_fixture(graph, rank, size);
  if (rank == 0) {
    graph.setNodeLabel(0, graph.number_of_global_nodes());
  }

  require_collective_validation_failure(
      [&] {
        static_cast<void>(
            parallel_contraction_test_access::compute_label_mapping(
                MPI_COMM_WORLD, graph));
      },
      "label request local validation",
      size);
}

TEST_CASE("quotient edges use one dense keyed exchange and aggregate exactly",
          "[unit][mpi][contraction][quotient-edges]") {
  int rank = 0;
  int size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  REQUIRE(size >= 1);
  REQUIRE(size <= 5);

  constexpr auto coarse_nodes = NodeID{4};
  hashed_graph local_edges;
  local_edges[hashed_edge{coarse_nodes, 0, 2}].weight +=
      4 * static_cast<NodeWeight>(rank + 1);
  if (rank % 2 == 0) {
    local_edges[hashed_edge{coarse_nodes, 1, 2}].weight += 8;
  }
  std::unordered_map<NodeID, NodeWeight> no_node_weights;
  parallel_graph_access quotient{MPI_COMM_WORLD};

  protocol_probe::reset();
  protocol_probe::active = true;
  parallel_contraction_test_access::redistribute_quotient(
      MPI_COMM_WORLD,
      local_edges,
      no_node_weights,
      coarse_nodes,
      quotient);
  protocol_probe::active = false;

  auto actual = std::vector<std::tuple<NodeID, NodeID, EdgeWeight>>{};
  forall_local_nodes(quotient, node) {
    auto const source = quotient.getGlobalID(node);
    forall_out_edges(quotient, edge, node) {
      actual.emplace_back(source,
                          quotient.getGlobalID(quotient.getEdgeTarget(edge)),
                          quotient.getEdgeWeight(edge));
    } endfor
  } endfor
  auto const expected_legacy_order = rank == 0
      ? std::vector<std::tuple<NodeID, NodeID, EdgeWeight>>{
            {0, 2, static_cast<EdgeWeight>(size * (size + 1))},
            {1, 2, static_cast<EdgeWeight>(4 * ((size + 1) / 2))}}
      : rank == 1
            ? std::vector<std::tuple<NodeID, NodeID, EdgeWeight>>{
                  {2, 0, static_cast<EdgeWeight>(size * (size + 1))},
                  {2, 1, static_cast<EdgeWeight>(4 * ((size + 1) / 2))}}
            : std::vector<std::tuple<NodeID, NodeID, EdgeWeight>>{};
  if (size == 3) {
    REQUIRE(actual == expected_legacy_order);
  }
  std::ranges::sort(actual);

  auto const owner_divisor = static_cast<NodeID>(
      std::ceil(coarse_nodes / static_cast<double>(size)));
  auto const first = std::min(
      coarse_nodes, static_cast<NodeID>(rank) * owner_divisor);
  auto const end = std::min(
      coarse_nodes, static_cast<NodeID>(rank + 1) * owner_divisor);
  auto expected = std::vector<std::tuple<NodeID, NodeID, EdgeWeight>>{};
  auto const edge_0_2_weight = static_cast<EdgeWeight>(
      size * (size + 1));
  auto const even_contributors = static_cast<EdgeWeight>((size + 1) / 2);
  auto const edge_1_2_weight = EdgeWeight{4} * even_contributors;
  for (auto source = first; source < end; ++source) {
    if (source == 0) {
      expected.emplace_back(0, 2, edge_0_2_weight);
    } else if (source == 1) {
      expected.emplace_back(1, 2, edge_1_2_weight);
    } else if (source == 2) {
      expected.emplace_back(2, 0, edge_0_2_weight);
      expected.emplace_back(2, 1, edge_1_2_weight);
    }
  }
  std::ranges::sort(expected);
  REQUIRE(actual == expected);

  CAPTURE(protocol_probe::all_to_all_v_calls,
          protocol_probe::all_to_all_v_c_calls,
          protocol_probe::isend_tags,
          protocol_probe::probe_tags,
          protocol_probe::recv_tags,
          protocol_probe::payload_extents);
  REQUIRE(protocol_probe::payload_calls_with_extent(
              static_cast<MPI_Aint>(4 * sizeof(NodeID))) == 1);
  REQUIRE(protocol_probe::calls_in_tag_phase(
              protocol_probe::isend_tags, 7, size) == 0);
  REQUIRE(protocol_probe::calls_in_tag_phase(
              protocol_probe::probe_tags, 7, size) == 0);
  REQUIRE(protocol_probe::calls_in_tag_phase(
              protocol_probe::recv_tags, 7, size) == 0);
}

TEST_CASE("quotient node weights use one dense keyed exchange and sum exactly",
          "[unit][mpi][contraction][quotient-node-weights]") {
  int rank = 0;
  int size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  REQUIRE(size >= 1);
  REQUIRE(size <= 5);

  constexpr auto coarse_nodes = NodeID{4};
  hashed_graph no_edges;
  std::unordered_map<NodeID, NodeWeight> local_weights;
  local_weights[0] = static_cast<NodeWeight>(rank + 1);
  local_weights[2] = 2 * static_cast<NodeWeight>(rank + 1);
  if (rank % 2 == 0) {
    local_weights[1] = 5;
  }
  parallel_graph_access quotient{MPI_COMM_WORLD};

  protocol_probe::reset();
  protocol_probe::active = true;
  parallel_contraction_test_access::redistribute_quotient(
      MPI_COMM_WORLD,
      no_edges,
      local_weights,
      coarse_nodes,
      quotient);
  protocol_probe::active = false;

  auto const triangular = static_cast<NodeWeight>(size * (size + 1) / 2);
  auto const expected_weights = std::array<NodeWeight, 4>{
      triangular,
      static_cast<NodeWeight>(5 * ((size + 1) / 2)),
      2 * triangular,
      0};
  forall_local_nodes(quotient, node) {
    auto const global = quotient.getGlobalID(node);
    REQUIRE(quotient.getNodeWeight(node) ==
            expected_weights.at(static_cast<std::size_t>(global)));
  } endfor

  CAPTURE(protocol_probe::all_to_all_v_calls,
          protocol_probe::all_to_all_v_c_calls,
          protocol_probe::isend_tags,
          protocol_probe::probe_tags,
          protocol_probe::recv_tags,
          protocol_probe::payload_extents);
  REQUIRE(protocol_probe::payload_calls_with_extent(
              static_cast<MPI_Aint>(2 * sizeof(NodeID))) == 1);
  REQUIRE(protocol_probe::calls_in_tag_phase(
              protocol_probe::isend_tags, 8, size) == 0);
  REQUIRE(protocol_probe::calls_in_tag_phase(
              protocol_probe::probe_tags, 8, size) == 0);
  REQUIRE(protocol_probe::calls_in_tag_phase(
              protocol_probe::recv_tags, 8, size) == 0);
}

TEST_CASE("zero coarse-node redistribution remains an empty dense exchange",
          "[unit][mpi][contraction][quotient][zero]") {
  int size = 0;
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  REQUIRE(size >= 1);
  REQUIRE(size <= 5);

  hashed_graph no_edges;
  std::unordered_map<NodeID, NodeWeight> no_node_weights;
  parallel_graph_access quotient{MPI_COMM_WORLD};

  protocol_probe::reset();
  protocol_probe::active = true;
  parallel_contraction_test_access::redistribute_quotient(
      MPI_COMM_WORLD, no_edges, no_node_weights, 0, quotient);
  protocol_probe::active = false;

  REQUIRE(quotient.number_of_local_nodes() == 0);
  REQUIRE(quotient.number_of_local_edges() == 0);
  REQUIRE(quotient.get_from_range() == 0);
  REQUIRE(quotient.get_to_range() == 0);
  REQUIRE(protocol_probe::dense_payload_collective_calls() == 2);
}

TEST_CASE("quotient redistribution rejects a tail-padding edge source collectively",
          "[unit][mpi][contraction][quotient-edges][failure]") {
  int rank = 0;
  int size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  if (size != 3) {
    return;
  }

  constexpr auto coarse_nodes = NodeID{4};
  hashed_graph local_edges;
  if (rank == 0) {
    local_edges[hashed_edge{coarse_nodes, coarse_nodes, 0}].weight = 4;
  }
  std::unordered_map<NodeID, NodeWeight> no_node_weights;
  parallel_graph_access quotient{MPI_COMM_WORLD};

  require_collective_validation_failure(
      [&] {
        parallel_contraction_test_access::redistribute_quotient(
            MPI_COMM_WORLD,
            local_edges,
            no_node_weights,
            coarse_nodes,
            quotient);
      },
      "quotient edge local validation",
      size);
  REQUIRE(quotient.number_of_local_nodes() == 0);
  REQUIRE(quotient.number_of_local_edges() == 0);
}

TEST_CASE("quotient redistribution rejects a tail-padding edge target collectively",
          "[unit][mpi][contraction][quotient-edges][failure]") {
  int rank = 0;
  int size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  if (size != 3) {
    return;
  }

  constexpr auto coarse_nodes = NodeID{4};
  hashed_graph local_edges;
  if (rank == 0) {
    local_edges[hashed_edge{coarse_nodes, 0, coarse_nodes}].weight = 4;
  }
  std::unordered_map<NodeID, NodeWeight> no_node_weights;
  parallel_graph_access quotient{MPI_COMM_WORLD};

  require_collective_validation_failure(
      [&] {
        parallel_contraction_test_access::redistribute_quotient(
            MPI_COMM_WORLD,
            local_edges,
            no_node_weights,
            coarse_nodes,
            quotient);
      },
      "quotient edge local validation",
      size);
  REQUIRE(quotient.number_of_local_nodes() == 0);
  REQUIRE(quotient.number_of_local_edges() == 0);
}

TEST_CASE("quotient redistribution rejects a tail-padding node weight collectively",
          "[unit][mpi][contraction][quotient-node-weights][failure]") {
  int rank = 0;
  int size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  if (size != 3) {
    return;
  }

  constexpr auto coarse_nodes = NodeID{4};
  hashed_graph no_edges;
  std::unordered_map<NodeID, NodeWeight> local_weights;
  if (rank == 0) {
    local_weights[coarse_nodes] = 7;
  }
  parallel_graph_access quotient{MPI_COMM_WORLD};

  require_collective_validation_failure(
      [&] {
        parallel_contraction_test_access::redistribute_quotient(
            MPI_COMM_WORLD,
            no_edges,
            local_weights,
            coarse_nodes,
            quotient);
      },
      "quotient node-weight local validation",
      size);
  REQUIRE(quotient.number_of_local_nodes() == 0);
  REQUIRE(quotient.number_of_local_edges() == 0);
}

TEST_CASE("all to all vector of vectors", "[unit][mpi]") {
	SECTION("empty cases") {
		PEID rank, size;
		MPI_Comm_rank(MPI_COMM_WORLD, &rank);
		MPI_Comm_size(MPI_COMM_WORLD, &size);

		const std::vector<std::vector<NodeID>> v_empty(
				static_cast<std::size_t>(size), std::vector<NodeID>{1, 2, 3});
		auto vec = mpi::all_to_all(v_empty, MPI_COMM_WORLD);
		MPI_Barrier(MPI_COMM_WORLD);
		REQUIRE(v_empty == vec);
	}
	SECTION("complex case") {
		PEID rank, size;
		MPI_Comm_rank(MPI_COMM_WORLD, &rank);
		MPI_Comm_size(MPI_COMM_WORLD, &size);

		auto v_empty = std::vector<std::vector<unsigned short>>(
				static_cast<std::size_t>(size));
		for (int destination = 0; destination < size; ++destination) {
			v_empty[static_cast<std::size_t>(destination)].assign(
					static_cast<std::size_t>(destination),
					static_cast<unsigned short>(destination));
		}
		auto vec = mpi::all_to_all(v_empty, MPI_COMM_WORLD);
		MPI_Barrier(MPI_COMM_WORLD);
		fmt::print("rank: {} -> {}\n", rank, vec);
		REQUIRE(v_empty.size() == vec.size());
	}

	SECTION("custom types") {
		PEID rank, size;
		MPI_Comm_rank(MPI_COMM_WORLD, &rank);
		MPI_Comm_size(MPI_COMM_WORLD, &size);

		const std::vector<std::vector<contraction::bundled_edge>> empty_edges(
				static_cast<std::size_t>(size),
				std::vector<contraction::bundled_edge>{{0, 0, 0, 0}});
		const std::vector<std::vector<contraction::node_weight_contribution>> empty_weights(
				static_cast<std::size_t>(size),
				std::vector<contraction::node_weight_contribution>{{}});
		const auto empty_meta = empty_weights;
		auto vec_1 = mpi::all_to_all(empty_edges, MPI_COMM_WORLD);
		auto vec_2 = mpi::all_to_all(empty_weights, MPI_COMM_WORLD);
		auto vec_3 = mpi::all_to_all(empty_meta, MPI_COMM_WORLD);
		MPI_Barrier(MPI_COMM_WORLD);
		REQUIRE(empty_edges.size() == vec_1.size());
		REQUIRE(empty_weights.size() == vec_2.size());
		REQUIRE(empty_meta.size() == vec_3.size());
	}
}
