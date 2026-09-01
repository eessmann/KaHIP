#include <mpi.h>
#include <unistd.h>

#include <array>
#include <cstdlib>
#include <limits>
#include <string_view>
#include <vector>

#include "data_structure/balance_management_refinement.h"
#include "data_structure/parallel_graph_access.h"
#include "definitions.h"
#include "kahip_mpi_capabilities.h"

// KAHIP_PMPI_CALLBACK_REGION_BEGIN
namespace balance_refinement_failure_probe {
enum class mode : unsigned char {
  backend,
  zero_k,
  invalid_label,
  local_overflow,
  global_overflow,
};

inline bool active = false;
inline mode selected = mode::backend;
inline MPI_Comm expected_communicator = MPI_COMM_NULL;
inline int rank = -1;
inline int backend_injections = 0;
inline int finalizations = 0;
inline bool callback_error = false;

void write_text(std::string_view text) noexcept {
  static_cast<void>(::write(STDERR_FILENO, text.data(), text.size()));
}

[[nodiscard]] auto expected_abort_state() noexcept -> bool {
  auto const expected_injections = selected == mode::backend ? 1 : 0;
  return !callback_error && backend_injections == expected_injections &&
         finalizations == 0;
}

void write_abort_marker() noexcept {
  switch (rank) {
    case 0:
      write_text(
          "observed MPI_Abort rank=0 balance-refinement "
          "affected-communicator; internal MPI_Finalize counter is zero\n");
      return;
    case 1:
      write_text(
          "observed MPI_Abort rank=1 balance-refinement "
          "affected-communicator; internal MPI_Finalize counter is zero\n");
      return;
    case 2:
      write_text(
          "observed MPI_Abort rank=2 balance-refinement "
          "affected-communicator; internal MPI_Finalize counter is zero\n");
      return;
    default:
      write_text("observed balance-refinement abort with invalid rank\n");
      return;
  }
}

[[noreturn]] void observed_abort(MPI_Comm communicator,
                                 int error_code) noexcept {
  auto relation = int{MPI_UNEQUAL};
  if (error_code != EXIT_FAILURE ||
      PMPI_Comm_compare(communicator, expected_communicator, &relation) !=
          MPI_SUCCESS ||
      (relation != MPI_IDENT && relation != MPI_CONGRUENT) ||
      !expected_abort_state()) {
    write_text("observed balance-refinement MPI_Abort with unexpected state\n");
    std::_Exit(91);
  }
  write_abort_marker();
  if (PMPI_Barrier(MPI_COMM_WORLD) != MPI_SUCCESS) {
    std::_Exit(90);
  }
  std::_Exit(86);
}
}  // namespace balance_refinement_failure_probe

static_assert(
    noexcept(balance_refinement_failure_probe::write_text(std::string_view{})));
static_assert(
    noexcept(balance_refinement_failure_probe::expected_abort_state()));
static_assert(noexcept(balance_refinement_failure_probe::write_abort_marker()));
static_assert(noexcept(
    balance_refinement_failure_probe::observed_abort(MPI_COMM_NULL, 0)));

extern "C" int MPI_Allreduce(void const* send_buffer,
                             void* receive_buffer,
                             int count,
                             MPI_Datatype datatype,
                             MPI_Op reduction,
                             MPI_Comm communicator) {
  using namespace balance_refinement_failure_probe;
  auto const inject = active && selected == mode::backend && count == 2 &&
                      datatype == MPI_UNSIGNED_LONG_LONG &&
                      reduction == MPI_SUM;
  if (!inject) {
    return PMPI_Allreduce(send_buffer, receive_buffer, count, datatype,
                          reduction, communicator);
  }
  ++backend_injections;
  if (send_buffer == nullptr || receive_buffer == nullptr ||
      send_buffer == receive_buffer) {
    callback_error = true;
  }
  return MPI_ERR_OTHER;
}

#if KAHIP_HAVE_MPI_ALLREDUCE_C
extern "C" int MPI_Allreduce_c(void const* send_buffer,
                               void* receive_buffer,
                               MPI_Count count,
                               MPI_Datatype datatype,
                               MPI_Op reduction,
                               MPI_Comm communicator) {
  using namespace balance_refinement_failure_probe;
  auto const inject = active && selected == mode::backend && count == 2 &&
                      datatype == MPI_UNSIGNED_LONG_LONG &&
                      reduction == MPI_SUM;
  if (!inject) {
    return PMPI_Allreduce_c(send_buffer, receive_buffer, count, datatype,
                            reduction, communicator);
  }
  ++backend_injections;
  if (send_buffer == nullptr || receive_buffer == nullptr ||
      send_buffer == receive_buffer) {
    callback_error = true;
  }
  return MPI_ERR_OTHER;
}
#endif

extern "C" int MPI_Finalize() {
  if (balance_refinement_failure_probe::active) {
    ++balance_refinement_failure_probe::finalizations;
    return MPI_SUCCESS;
  }
  return PMPI_Finalize();
}

extern "C" int MPI_Abort(MPI_Comm communicator, int error_code) {
  balance_refinement_failure_probe::observed_abort(communicator, error_code);
}
// KAHIP_PMPI_CALLBACK_REGION_END

namespace {
[[nodiscard]] auto parse_mode(std::string_view value)
    -> balance_refinement_failure_probe::mode {
  using mode = balance_refinement_failure_probe::mode;
  if (value == "backend") {
    return mode::backend;
  }
  if (value == "zero-k") {
    return mode::zero_k;
  }
  if (value == "invalid-label") {
    return mode::invalid_label;
  }
  if (value == "local-overflow") {
    return mode::local_overflow;
  }
  if (value == "global-overflow") {
    return mode::global_overflow;
  }
  std::_Exit(2);
}

void build_fixture(parhip::parallel_graph_access& graph,
                   balance_refinement_failure_probe::mode selected,
                   int rank) {
  using mode = balance_refinement_failure_probe::mode;
  auto ranges = std::array<parhip::NodeID, 4>{};
  switch (selected) {
    case mode::zero_k:
      ranges = {0, 0, 0, 0};
      break;
    case mode::invalid_label:
      ranges = {0, 0, 1, 1};
      break;
    case mode::local_overflow:
      ranges = {0, 0, 2, 2};
      break;
    case mode::backend:
    case mode::global_overflow:
      ranges = {0, 0, 1, 2};
      break;
  }

  auto const local_nodes = ranges[static_cast<std::size_t>(rank) + 1] -
                           ranges[static_cast<std::size_t>(rank)];
  graph.start_construction(local_nodes, 0, ranges.back(), 0, false);
  auto mutable_ranges =
      std::vector<parhip::NodeID>(ranges.begin(), ranges.end());
  auto const from = ranges[static_cast<std::size_t>(rank)];
  graph.set_range(from, local_nodes == 0 ? from : from + local_nodes - 1);
  graph.set_range_array(mutable_ranges);

  for (parhip::NodeID local = 0; local < local_nodes; ++local) {
    auto const node = graph.new_node();
    auto weight = parhip::NodeWeight{1};
    auto label = parhip::PartitionID{0};
    if (selected == mode::invalid_label) {
      label = 2;
    } else if (selected == mode::local_overflow) {
      weight = local == 0 ? std::numeric_limits<parhip::NodeWeight>::max() : 1;
    } else if (selected == mode::global_overflow) {
      weight = std::numeric_limits<parhip::NodeWeight>::max() / 2 + 1;
    }
    graph.setNodeWeight(node, weight);
    graph.setNodeLabel(node, label);
  }
  graph.finish_construction();
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 2 || MPI_Init(&argc, &argv) != MPI_SUCCESS) {
    return 1;
  }
  auto world_rank = -1;
  auto world_size = 0;
  if (MPI_Comm_rank(MPI_COMM_WORLD, &world_rank) != MPI_SUCCESS ||
      MPI_Comm_size(MPI_COMM_WORLD, &world_size) != MPI_SUCCESS ||
      world_size != 3) {
    return 2;
  }

  auto communicator = MPI_COMM_NULL;
  if (MPI_Comm_split(MPI_COMM_WORLD, 0, world_size - world_rank,
                     &communicator) != MPI_SUCCESS ||
      communicator == MPI_COMM_NULL ||
      MPI_Comm_set_errhandler(communicator, MPI_ERRORS_RETURN) != MPI_SUCCESS) {
    return 3;
  }
  auto rank = -1;
  if (MPI_Comm_rank(communicator, &rank) != MPI_SUCCESS) {
    return 4;
  }

  auto const selected = parse_mode(argv[1]);
  auto graph = parhip::parallel_graph_access{communicator};
  build_fixture(graph, selected, rank);
  auto const block_count =
      selected == balance_refinement_failure_probe::mode::zero_k
          ? parhip::PartitionID{0}
          : parhip::PartitionID{2};

  balance_refinement_failure_probe::selected = selected;
  balance_refinement_failure_probe::expected_communicator = communicator;
  balance_refinement_failure_probe::rank = rank;
  balance_refinement_failure_probe::active = true;
  auto manager = parhip::balance_management_refinement{&graph, block_count};
  static_cast<void>(manager);

  balance_refinement_failure_probe::write_text("returned-from-failure\n");
  std::_Exit(92);
}
