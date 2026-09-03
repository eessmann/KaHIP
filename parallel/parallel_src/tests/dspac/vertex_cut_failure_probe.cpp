#include <mpi.h>
#include <unistd.h>

#include <cstdlib>
#include <limits>
#include <string_view>
#include <vector>

#include "communication/mpi_fixed_reduction.h"
#include "data_structure/parallel_graph_access.h"
#include "definitions.h"
#include "dspac/dspac.h"

// KAHIP_PMPI_CALLBACK_REGION_BEGIN
namespace vertex_cut_failure_probe {
enum class mode : unsigned char {
  backend,
  zero_k,
  undersized_partition,
  out_of_range_label,
  mismatched_k,
  intercommunicator,
};

inline bool active = false;
inline mode selected = mode::backend;
inline MPI_Comm expected_communicator = MPI_COMM_NULL;
inline int all_reductions = 0;
inline int minima = 0;
inline int maxima = 0;
inline int validations = 0;
inline int sums = 0;
inline int finalizations = 0;
inline bool callback_error = false;

void write_text(std::string_view text) noexcept {
  static_cast<void>(::write(STDERR_FILENO, text.data(), text.size()));
}

[[nodiscard]] auto valid_all_reduce(void const* send_buffer,
                                    void* receive_buffer,
                                    int count,
                                    MPI_Datatype datatype,
                                    MPI_Op operation,
                                    MPI_Comm communicator) noexcept -> bool {
  auto const supported_operation = operation == MPI_MIN ||
                                   operation == MPI_MAX ||
                                   operation == MPI_BOR || operation == MPI_SUM;
  return send_buffer != nullptr && receive_buffer != nullptr &&
         send_buffer != receive_buffer && count == 1 &&
         datatype == MPI_UNSIGNED_LONG_LONG && supported_operation &&
         communicator == expected_communicator;
}

[[nodiscard]] auto expected_abort_state() noexcept -> bool {
  if (callback_error || finalizations != 0) {
    return false;
  }
  switch (selected) {
    case mode::backend:
      return all_reductions == 4 && minima == 1 && maxima == 1 &&
             validations == 1 && sums == 1;
    case mode::zero_k:
    case mode::undersized_partition:
    case mode::out_of_range_label:
    case mode::mismatched_k:
      return all_reductions == 3 && minima == 1 && maxima == 1 &&
             validations == 1 && sums == 0;
    case mode::intercommunicator:
      return all_reductions == 0 && minima == 0 && maxima == 0 &&
             validations == 0 && sums == 0;
  }
  return false;
}

[[noreturn]] void observed_abort(MPI_Comm communicator,
                                 int error_code) noexcept {
  int relation = MPI_UNEQUAL;
  if (error_code != EXIT_FAILURE ||
      PMPI_Comm_compare(communicator, expected_communicator, &relation) !=
          MPI_SUCCESS ||
      relation != MPI_IDENT || !expected_abort_state()) {
    write_text("observed vertex-cut MPI_Abort with unexpected state\n");
    std::_Exit(91);
  }
  write_text("observed vertex-cut MPI_Abort on affected communicator\n");
  if (PMPI_Barrier(MPI_COMM_WORLD) != MPI_SUCCESS) {
    std::_Exit(90);
  }
  std::_Exit(86);
}
}  // namespace vertex_cut_failure_probe

static_assert(noexcept(vertex_cut_failure_probe::write_text({})));
static_assert(
    noexcept(vertex_cut_failure_probe::valid_all_reduce(nullptr,
                                                        nullptr,
                                                        0,
                                                        MPI_DATATYPE_NULL,
                                                        MPI_OP_NULL,
                                                        MPI_COMM_NULL)));
static_assert(noexcept(vertex_cut_failure_probe::expected_abort_state()));
static_assert(noexcept(vertex_cut_failure_probe::observed_abort(MPI_COMM_NULL,
                                                                0)));

extern "C" int MPI_Allreduce(void const* send_buffer,
                             void* receive_buffer,
                             int count,
                             MPI_Datatype datatype,
                             MPI_Op operation,
                             MPI_Comm communicator) {
  using namespace vertex_cut_failure_probe;
  if (!active) {
    return PMPI_Allreduce(send_buffer, receive_buffer, count, datatype,
                          operation, communicator);
  }

  ++all_reductions;
  if (!valid_all_reduce(send_buffer, receive_buffer, count, datatype, operation,
                        communicator)) {
    callback_error = true;
    return MPI_ERR_OTHER;
  }
  if (operation == MPI_MIN) {
    ++minima;
  } else if (operation == MPI_MAX) {
    ++maxima;
  } else if (operation == MPI_BOR) {
    ++validations;
  } else {
    ++sums;
    if (selected != mode::backend) {
      callback_error = true;
    }
    return MPI_ERR_OTHER;
  }
  return PMPI_Allreduce(send_buffer, receive_buffer, count, datatype, operation,
                        communicator);
}

extern "C" int MPI_Finalize() {
  if (vertex_cut_failure_probe::active) {
    ++vertex_cut_failure_probe::finalizations;
    return MPI_SUCCESS;
  }
  return PMPI_Finalize();
}

extern "C" int MPI_Abort(MPI_Comm communicator, int error_code) {
  vertex_cut_failure_probe::observed_abort(communicator, error_code);
}
// KAHIP_PMPI_CALLBACK_REGION_END

namespace {
[[nodiscard]] auto parse_mode(std::string_view value)
    -> vertex_cut_failure_probe::mode {
  using mode = vertex_cut_failure_probe::mode;
  if (value == "backend") {
    return mode::backend;
  }
  if (value == "zero-k") {
    return mode::zero_k;
  }
  if (value == "undersized-partition") {
    return mode::undersized_partition;
  }
  if (value == "out-of-range-label") {
    return mode::out_of_range_label;
  }
  if (value == "mismatched-k") {
    return mode::mismatched_k;
  }
  if (value == "intercommunicator") {
    return mode::intercommunicator;
  }
  vertex_cut_failure_probe::write_text("unknown vertex-cut probe mode\n");
  std::_Exit(2);
}

void build_vertex_cut_fixture(parhip::parallel_graph_access& graph,
                              int rank,
                              int size,
                              std::vector<parhip::PartitionID>& partition) {
  constexpr auto local_edges = parhip::EdgeID{3};
  auto const global_nodes = static_cast<parhip::NodeID>(size);
  auto const global_edges = static_cast<parhip::EdgeID>(size) * local_edges;
  graph.start_construction(1, local_edges, global_nodes, global_edges, false);

  auto ranges = std::vector<parhip::NodeID>(static_cast<std::size_t>(size) + 1);
  for (int index = 0; index <= size; ++index) {
    ranges[static_cast<std::size_t>(index)] =
        static_cast<parhip::NodeID>(index);
  }
  graph.set_range(static_cast<parhip::NodeID>(rank),
                  static_cast<parhip::NodeID>(rank));
  graph.set_range_array(ranges);

  auto const node = graph.new_node();
  graph.setNodeWeight(node, 1);
  partition = {0, 1, 2};
  for ([[maybe_unused]] auto const block : partition) {
    auto const edge = graph.new_edge(node, static_cast<parhip::NodeID>(rank));
    graph.setEdgeWeight(edge, 1);
  }
  graph.finish_construction();
}

[[nodiscard]] auto make_intercommunicator(int world_rank) -> MPI_Comm {
  auto local = MPI_COMM_NULL;
  if (PMPI_Comm_split(MPI_COMM_WORLD, world_rank, 0, &local) != MPI_SUCCESS ||
      local == MPI_COMM_NULL) {
    std::_Exit(7);
  }
  auto intercommunicator = MPI_COMM_NULL;
  if (PMPI_Intercomm_create(local, 0, MPI_COMM_WORLD, 1 - world_rank, 731,
                            &intercommunicator) != MPI_SUCCESS ||
      intercommunicator == MPI_COMM_NULL) {
    std::_Exit(8);
  }
  if (PMPI_Comm_free(&local) != MPI_SUCCESS) {
    std::_Exit(9);
  }
  return intercommunicator;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 2 || MPI_Init(&argc, &argv) != MPI_SUCCESS) {
    return 2;
  }

  auto world_rank = -1;
  auto world_size = 0;
  if (MPI_Comm_rank(MPI_COMM_WORLD, &world_rank) != MPI_SUCCESS ||
      MPI_Comm_size(MPI_COMM_WORLD, &world_size) != MPI_SUCCESS ||
      world_size != 2) {
    return 3;
  }

  using mode = vertex_cut_failure_probe::mode;
  auto const selected = parse_mode(argv[1]);
  auto communicator = MPI_COMM_NULL;
  if (selected == mode::intercommunicator) {
    communicator = make_intercommunicator(world_rank);
  } else if (MPI_Comm_split(MPI_COMM_WORLD, 0, world_size - world_rank,
                            &communicator) != MPI_SUCCESS ||
             communicator == MPI_COMM_NULL) {
    return 4;
  }
  if (MPI_Comm_set_errhandler(communicator, MPI_ERRORS_RETURN) != MPI_SUCCESS) {
    return 5;
  }

  auto rank = -1;
  auto size = 0;
  if (MPI_Comm_rank(communicator, &rank) != MPI_SUCCESS ||
      MPI_Comm_size(communicator, &size) != MPI_SUCCESS) {
    return 6;
  }

  vertex_cut_failure_probe::selected = selected;
  vertex_cut_failure_probe::expected_communicator = communicator;
  vertex_cut_failure_probe::active = true;

  if (selected == mode::intercommunicator) {
    static_cast<void>(parhip::mpi::all_reduce_sum(
        parhip::EdgeWeight{1}, parhip::mpi::communicator_view{communicator},
        "MPI_Allreduce(vertex cut intercommunicator probe)"));
  } else {
    auto graph = parhip::parallel_graph_access{communicator};
    auto partition = std::vector<parhip::PartitionID>{};
    build_vertex_cut_fixture(graph, rank, size, partition);
    if (selected == mode::undersized_partition) {
      partition.pop_back();
    } else if (selected == mode::out_of_range_label) {
      partition.back() = 3;
    }

    auto splitter = parhip::dspac{
        graph, communicator, std::numeric_limits<parhip::EdgeWeight>::max()};
    auto const k = selected == mode::zero_k ? parhip::PartitionID{0}
                   : selected == mode::mismatched_k
                       ? static_cast<parhip::PartitionID>(rank + 3)
                       : parhip::PartitionID{3};
    static_cast<void>(splitter.calculate_vertex_cut(k, partition));
  }

  vertex_cut_failure_probe::write_text(
      "vertex-cut operation returned without fail-fast\n");
  std::_Exit(92);
}
