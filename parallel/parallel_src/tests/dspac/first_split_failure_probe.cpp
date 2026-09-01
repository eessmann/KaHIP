#include <mpi.h>
#include <unistd.h>

#include <cstdlib>
#include <array>
#include <limits>
#include <string_view>
#include <vector>

#include "data_structure/parallel_graph_access.h"
#include "definitions.h"
#include "dspac/dspac.h"
#include "kahip_mpi_capabilities.h"

// KAHIP_PMPI_CALLBACK_REGION_BEGIN
namespace dspac_first_split_failure_probe {
enum class failure_mode { neighbor_payload, projection_permutation, projection_barrier };

inline bool active = false;
inline failure_mode selected = failure_mode::neighbor_payload;
inline MPI_Comm expected_communicator = MPI_COMM_NULL;
inline int topology_creations = 0;
inline int count_exchanges = 0;
inline int payload_calls = 0;
inline int barrier_calls = 0;
inline int point_to_point_calls = 0;
inline int finalizations = 0;
inline bool callback_error = false;

void write_text(std::string_view text) noexcept {
  static_cast<void>(::write(STDERR_FILENO, text.data(), text.size()));
}

[[nodiscard]] auto expected_abort_state() noexcept -> bool {
  if (callback_error || point_to_point_calls != 0 || finalizations != 0) {
    return false;
  }
  switch (selected) {
    case failure_mode::neighbor_payload:
      return topology_creations == 1 && count_exchanges == 1 &&
             payload_calls == 1 && barrier_calls == 0;
    case failure_mode::projection_permutation:
      return topology_creations == 0 && count_exchanges == 0 &&
             payload_calls == 0 && barrier_calls == 0;
    case failure_mode::projection_barrier:
      return topology_creations == 0 && count_exchanges == 0 &&
             payload_calls == 0 && barrier_calls == 1;
  }
  return false;
}

[[noreturn]] void observed_abort(MPI_Comm communicator,
                                 int error_code) noexcept {
  auto relation = int{MPI_UNEQUAL};
  if (error_code != EXIT_FAILURE ||
      PMPI_Comm_compare(communicator, expected_communicator, &relation) !=
          MPI_SUCCESS ||
      (relation != MPI_CONGRUENT && relation != MPI_IDENT) ||
      !expected_abort_state()) {
    write_text("observed DSPAC first-split MPI_Abort with unexpected state\n");
    std::_Exit(91);
  }
  write_text("observed DSPAC MPI_Abort on affected communicator\n");
  if (PMPI_Barrier(MPI_COMM_WORLD) != MPI_SUCCESS) {
    std::_Exit(90);
  }
  std::_Exit(86);
}
}  // namespace dspac_first_split_failure_probe

static_assert(
    noexcept(dspac_first_split_failure_probe::write_text(std::string_view{})));
static_assert(
    noexcept(dspac_first_split_failure_probe::expected_abort_state()));
static_assert(noexcept(dspac_first_split_failure_probe::observed_abort(
    MPI_COMM_NULL, 0)));

extern "C" int MPI_Dist_graph_create(MPI_Comm communicator,
                                     int source_count,
                                     int const sources[],
                                     int const degrees[],
                                     int const destinations[],
                                     int const weights[],
                                     MPI_Info info,
                                     int reorder,
                                     MPI_Comm* graph_communicator) {
  if (dspac_first_split_failure_probe::active) {
    ++dspac_first_split_failure_probe::topology_creations;
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
  if (dspac_first_split_failure_probe::active) {
    ++dspac_first_split_failure_probe::count_exchanges;
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
  if (!dspac_first_split_failure_probe::active ||
      dspac_first_split_failure_probe::selected !=
          dspac_first_split_failure_probe::failure_mode::neighbor_payload) {
    return PMPI_Neighbor_alltoallv(
        send_buffer, send_counts, send_displacements, send_datatype,
        receive_buffer, receive_counts, receive_displacements,
        receive_datatype, communicator);
  }
  ++dspac_first_split_failure_probe::payload_calls;
  if (send_counts == nullptr || receive_counts == nullptr ||
      send_datatype != MPI_UNSIGNED_LONG_LONG ||
      receive_datatype != MPI_UNSIGNED_LONG_LONG) {
    dspac_first_split_failure_probe::callback_error = true;
  }
  return MPI_ERR_OTHER;
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
  if (!dspac_first_split_failure_probe::active ||
      dspac_first_split_failure_probe::selected !=
          dspac_first_split_failure_probe::failure_mode::neighbor_payload) {
    return PMPI_Neighbor_alltoallv_c(
        send_buffer, send_counts, send_displacements, send_datatype,
        receive_buffer, receive_counts, receive_displacements,
        receive_datatype, communicator);
  }
  ++dspac_first_split_failure_probe::payload_calls;
  if (send_counts == nullptr || receive_counts == nullptr ||
      send_datatype != MPI_UNSIGNED_LONG_LONG ||
      receive_datatype != MPI_UNSIGNED_LONG_LONG) {
    dspac_first_split_failure_probe::callback_error = true;
  }
  return MPI_ERR_OTHER;
}
#endif

extern "C" int MPI_Isend(void const* buffer,
                         int count,
                         MPI_Datatype datatype,
                         int destination,
                         int tag,
                         MPI_Comm communicator,
                         MPI_Request* request) {
  if (dspac_first_split_failure_probe::active) {
    ++dspac_first_split_failure_probe::point_to_point_calls;
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
  if (dspac_first_split_failure_probe::active) {
    ++dspac_first_split_failure_probe::point_to_point_calls;
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
  if (dspac_first_split_failure_probe::active) {
    ++dspac_first_split_failure_probe::point_to_point_calls;
  }
  return PMPI_Recv(buffer, count, datatype, source, tag, communicator, status);
}

extern "C" int MPI_Wait(MPI_Request* request, MPI_Status* status) {
  if (dspac_first_split_failure_probe::active) {
    ++dspac_first_split_failure_probe::point_to_point_calls;
  }
  return PMPI_Wait(request, status);
}

extern "C" int MPI_Barrier(MPI_Comm communicator) {
  if (dspac_first_split_failure_probe::active &&
      dspac_first_split_failure_probe::selected ==
          dspac_first_split_failure_probe::failure_mode::projection_barrier) {
    ++dspac_first_split_failure_probe::barrier_calls;
    return MPI_ERR_OTHER;
  }
  return PMPI_Barrier(communicator);
}

extern "C" int MPI_Finalize() {
  if (dspac_first_split_failure_probe::active) {
    ++dspac_first_split_failure_probe::finalizations;
    return MPI_SUCCESS;
  }
  return PMPI_Finalize();
}

extern "C" int MPI_Abort(MPI_Comm communicator, int error_code) {
  dspac_first_split_failure_probe::observed_abort(communicator, error_code);
}
// KAHIP_PMPI_CALLBACK_REGION_END

namespace {
[[nodiscard]] auto parse_mode(char const* value)
    -> dspac_first_split_failure_probe::failure_mode {
  auto const name = std::string_view{value};
  if (name == "neighbor-payload") {
    return dspac_first_split_failure_probe::failure_mode::neighbor_payload;
  }
  if (name == "projection-permutation") {
    return dspac_first_split_failure_probe::failure_mode::projection_permutation;
  }
  if (name == "projection-barrier") {
    return dspac_first_split_failure_probe::failure_mode::projection_barrier;
  }
  std::exit(6);
}

void build_fixture(parhip::parallel_graph_access& graph, int rank) {
  constexpr auto node_ranges = std::array<parhip::NodeID, 3>{0, 1, 2};
  constexpr auto edge_ranges = std::array<parhip::EdgeID, 3>{0, 1, 2};
  graph.start_construction(1, 1, 2, 2, false);
  graph.set_range(node_ranges[static_cast<std::size_t>(rank)],
                  node_ranges[static_cast<std::size_t>(rank)]);
  auto mutable_node_ranges =
      std::vector<parhip::NodeID>(node_ranges.begin(), node_ranges.end());
  graph.set_range_array(mutable_node_ranges);
  graph.set_edge_range_array(
      std::vector<parhip::EdgeID>(edge_ranges.begin(), edge_ranges.end()));

  auto const local = graph.new_node();
  graph.setNodeWeight(local, 1);
  graph.setNodeLabel(local, static_cast<parhip::NodeID>(rank));
  auto const edge = graph.new_edge(local, static_cast<parhip::NodeID>(1 - rank));
  graph.setEdgeWeight(edge, 1);
  graph.finish_construction();
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    return 1;
  }
  dspac_first_split_failure_probe::selected = parse_mode(argv[1]);
  if (MPI_Init(&argc, &argv) != MPI_SUCCESS) {
    return 2;
  }
  auto rank = -1;
  auto size = 0;
  if (MPI_Comm_rank(MPI_COMM_WORLD, &rank) != MPI_SUCCESS ||
      MPI_Comm_size(MPI_COMM_WORLD, &size) != MPI_SUCCESS || size != 2) {
    return 3;
  }
  if (MPI_Comm_set_errhandler(MPI_COMM_WORLD, MPI_ERRORS_RETURN) !=
      MPI_SUCCESS) {
    return 4;
  }

  auto input = parhip::parallel_graph_access{MPI_COMM_WORLD};
  auto split = parhip::parallel_graph_access{MPI_COMM_WORLD};
  build_fixture(input, rank);
  auto splitter = parhip::dspac{
      input, MPI_COMM_WORLD,
      std::numeric_limits<parhip::EdgeWeight>::max()};

  auto const neighbor_payload_failure =
      dspac_first_split_failure_probe::selected ==
      dspac_first_split_failure_probe::failure_mode::neighbor_payload;
  dspac_first_split_failure_probe::expected_communicator = MPI_COMM_WORLD;
  dspac_first_split_failure_probe::active = neighbor_payload_failure;
  splitter.construct(split);

  dspac_first_split_failure_probe::active = true;
  if (dspac_first_split_failure_probe::selected ==
      dspac_first_split_failure_probe::failure_mode::projection_permutation) {
    static_cast<void>(splitter.project_partition(split, {1}));
  } else if (dspac_first_split_failure_probe::selected ==
             dspac_first_split_failure_probe::failure_mode::projection_barrier) {
    static_cast<void>(splitter.project_partition(split, {0}));
  }

  dspac_first_split_failure_probe::write_text(
      "DSPAC failure returned without fail-fast\n");
  static_cast<void>(MPI_Finalize());
  return 5;
}
