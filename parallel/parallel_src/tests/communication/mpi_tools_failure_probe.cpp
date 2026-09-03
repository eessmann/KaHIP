#include <mpi.h>
#include <unistd.h>

#include <cstdlib>
#include <limits>
#include <optional>
#include <string_view>
#include <vector>

#include "communication/mpi_tools.h"
#include "communication/serial_kernel_profile_observer.h"
#include "data_structure/parallel_graph_access.h"

// KAHIP_PMPI_CALLBACK_REGION_BEGIN
namespace mpi_tools_failure_probe {
inline bool active = false;
inline MPI_Comm expected_communicator = MPI_COMM_NULL;
inline int communicator_rank = -1;
inline int all_to_all_calls = 0;
inline int finalizations = 0;
inline bool callback_error = false;
inline bool unsafe_profile = false;
inline bool structural_self_loop = false;
inline bool vcycle_mismatch = false;
inline bool initial_algorithm_mismatch = false;
inline bool profile_aggregate_overflow = false;

void write_text(std::string_view text) noexcept {
  static_cast<void>(::write(STDERR_FILENO, text.data(), text.size()));
}

void observe_safe_serial_profile(
    void*, kahip::serial_kernel::serial_kernel_profile const&) noexcept {
  callback_error = true;
  write_text("misleading safe observer profile\n");
}

[[nodiscard]] auto valid_count_exchange(void const* send_buffer,
                                        int send_count,
                                        MPI_Datatype send_datatype,
                                        void* receive_buffer,
                                        int receive_count,
                                        MPI_Datatype receive_datatype,
                                        MPI_Comm communicator) noexcept
    -> bool {
  int relation = MPI_UNEQUAL;
  return send_buffer != nullptr && receive_buffer != nullptr &&
         send_count == 1 && receive_count == 1 &&
         send_datatype == MPI_UINT64_T && receive_datatype == MPI_UINT64_T &&
         PMPI_Comm_compare(communicator, expected_communicator, &relation) ==
             MPI_SUCCESS &&
         (relation == MPI_IDENT || relation == MPI_CONGRUENT);
}

[[nodiscard]] auto expected_abort_state() noexcept -> bool {
  return !callback_error &&
         all_to_all_calls ==
             ((unsafe_profile || structural_self_loop || vcycle_mismatch ||
               initial_algorithm_mismatch)
                  || profile_aggregate_overflow
                  ? 0
                  : 1) &&
         finalizations == 0 &&
         (communicator_rank == 0 || communicator_rank == 1);
}

[[noreturn]] void observed_abort(MPI_Comm communicator,
                                 int error_code) noexcept {
  int relation = MPI_UNEQUAL;
  if (error_code != EXIT_FAILURE ||
      PMPI_Comm_compare(communicator, expected_communicator, &relation) !=
          MPI_SUCCESS ||
      (relation != MPI_IDENT && relation != MPI_CONGRUENT) ||
      !expected_abort_state()) {
    write_text("observed mpi-tools MPI_Abort with unexpected state\n");
    std::_Exit(91);
  }
  if (profile_aggregate_overflow && communicator_rank == 0) {
    write_text("observed MPI_Abort rank=0 "
               "mpi-tools-profile-aggregate-overflow "
               "affected-communicator; payload all-to-all calls are zero\n");
  } else if (profile_aggregate_overflow) {
    write_text("observed MPI_Abort rank=1 "
               "mpi-tools-profile-aggregate-overflow "
               "affected-communicator; payload all-to-all calls are zero\n");
  } else if (vcycle_mismatch && communicator_rank == 0) {
    write_text("observed MPI_Abort rank=0 mpi-tools-config-vcycle-mismatch "
               "affected-communicator; payload all-to-all calls are zero\n");
  } else if (vcycle_mismatch) {
    write_text("observed MPI_Abort rank=1 mpi-tools-config-vcycle-mismatch "
               "affected-communicator; payload all-to-all calls are zero\n");
  } else if (initial_algorithm_mismatch && communicator_rank == 0) {
    write_text("observed MPI_Abort rank=0 "
               "mpi-tools-config-initial-algorithm-mismatch "
               "affected-communicator; payload all-to-all calls are zero\n");
  } else if (initial_algorithm_mismatch) {
    write_text("observed MPI_Abort rank=1 "
               "mpi-tools-config-initial-algorithm-mismatch "
               "affected-communicator; payload all-to-all calls are zero\n");
  } else if (structural_self_loop && communicator_rank == 0) {
    write_text("observed MPI_Abort rank=0 mpi-tools-structural-self-loop "
               "affected-communicator\n");
  } else if (structural_self_loop) {
    write_text("observed MPI_Abort rank=1 mpi-tools-structural-self-loop "
               "affected-communicator\n");
  } else if (unsafe_profile && communicator_rank == 0) {
    write_text("observed MPI_Abort rank=0 mpi-tools-unsafe-profile "
               "affected-communicator; payload all-to-all calls are zero\n");
  } else if (unsafe_profile) {
    write_text("observed MPI_Abort rank=1 mpi-tools-unsafe-profile "
               "affected-communicator; payload all-to-all calls are zero\n");
  } else if (communicator_rank == 0) {
    write_text(
        "observed MPI_Abort rank=0 mpi-tools-count-exchange "
        "affected-communicator; internal MPI_Finalize counter is zero\n");
  } else {
    write_text(
        "observed MPI_Abort rank=1 mpi-tools-count-exchange "
        "affected-communicator; internal MPI_Finalize counter is zero\n");
  }
  if (PMPI_Barrier(MPI_COMM_WORLD) != MPI_SUCCESS) {
    std::_Exit(90);
  }
  std::_Exit(86);
}
}  // namespace mpi_tools_failure_probe

static_assert(noexcept(mpi_tools_failure_probe::write_text({})));
static_assert(
    noexcept(mpi_tools_failure_probe::valid_count_exchange(nullptr,
                                                           0,
                                                           MPI_DATATYPE_NULL,
                                                           nullptr,
                                                           0,
                                                           MPI_DATATYPE_NULL,
                                                           MPI_COMM_NULL)));
static_assert(noexcept(mpi_tools_failure_probe::expected_abort_state()));
static_assert(noexcept(mpi_tools_failure_probe::observed_abort(MPI_COMM_NULL,
                                                               0)));

extern "C" int MPI_Alltoall(void const* send_buffer,
                            int send_count,
                            MPI_Datatype send_datatype,
                            void* receive_buffer,
                            int receive_count,
                            MPI_Datatype receive_datatype,
                            MPI_Comm communicator) {
  if (!mpi_tools_failure_probe::active) {
    return PMPI_Alltoall(send_buffer, send_count, send_datatype, receive_buffer,
                         receive_count, receive_datatype, communicator);
  }
  ++mpi_tools_failure_probe::all_to_all_calls;
  if (mpi_tools_failure_probe::unsafe_profile) {
    mpi_tools_failure_probe::write_text("forbidden payload MPI_Alltoall\n");
    mpi_tools_failure_probe::callback_error = true;
  }
  if (!mpi_tools_failure_probe::valid_count_exchange(
          send_buffer, send_count, send_datatype, receive_buffer, receive_count,
          receive_datatype, communicator)) {
    mpi_tools_failure_probe::callback_error = true;
  }
  return MPI_ERR_OTHER;
}

extern "C" int MPI_Finalize() {
  if (mpi_tools_failure_probe::active) {
    ++mpi_tools_failure_probe::finalizations;
    return MPI_SUCCESS;
  }
  return PMPI_Finalize();
}

extern "C" int MPI_Abort(MPI_Comm communicator, int error_code) {
  mpi_tools_failure_probe::observed_abort(communicator, error_code);
}
// KAHIP_PMPI_CALLBACK_REGION_END

int main(int argc, char** argv) {
  if (argc != 2 ||
      (std::string_view{argv[1]} != "backend" &&
       std::string_view{argv[1]} != "unsafe-profile" &&
       std::string_view{argv[1]} != "structural-self-loop" &&
       std::string_view{argv[1]} != "config-vcycle-mismatch" &&
       std::string_view{argv[1]} != "config-initial-algorithm-mismatch" &&
       std::string_view{argv[1]} != "profile-aggregate-overflow") ||
      MPI_Init(&argc, &argv) != MPI_SUCCESS) {
    return 2;
  }

  auto world_rank = 0;
  auto world_size = 0;
  if (MPI_Comm_rank(MPI_COMM_WORLD, &world_rank) != MPI_SUCCESS ||
      MPI_Comm_size(MPI_COMM_WORLD, &world_size) != MPI_SUCCESS ||
      world_size != 2) {
    return 3;
  }

  auto communicator = MPI_COMM_NULL;
  if (MPI_Comm_split(MPI_COMM_WORLD, 0, world_size - world_rank,
                     &communicator) != MPI_SUCCESS ||
      communicator == MPI_COMM_NULL) {
    return 4;
  }
  auto rank = 0;
  if (MPI_Comm_rank(communicator, &rank) != MPI_SUCCESS) {
    return 5;
  }

  mpi_tools_failure_probe::structural_self_loop =
      std::string_view{argv[1]} == "structural-self-loop";
  mpi_tools_failure_probe::vcycle_mismatch =
      std::string_view{argv[1]} == "config-vcycle-mismatch";
  mpi_tools_failure_probe::initial_algorithm_mismatch =
      std::string_view{argv[1]} == "config-initial-algorithm-mismatch";
  mpi_tools_failure_probe::profile_aggregate_overflow =
      std::string_view{argv[1]} == "profile-aggregate-overflow";
  parhip::parallel_graph_access distributed{communicator};
  auto const local_nodes = mpi_tools_failure_probe::structural_self_loop
                               ? (rank == 0 ? 1 : 0)
                               : 1;
  auto const local_edges =
      mpi_tools_failure_probe::structural_self_loop ? (rank == 0 ? 1 : 0) : 0;
  auto const global_nodes =
      mpi_tools_failure_probe::structural_self_loop ? 1 : 2;
  auto const global_edges =
      mpi_tools_failure_probe::structural_self_loop ? 1 : 0;
  distributed.start_construction(local_nodes, local_edges, global_nodes,
                                 global_edges, false);
  auto ranges = mpi_tools_failure_probe::structural_self_loop
                    ? std::vector<parhip::NodeID>{0, 1, 1}
                    : std::vector<parhip::NodeID>{0, 1, 2};
  auto const first = ranges[static_cast<std::size_t>(rank)];
  auto const end = ranges[static_cast<std::size_t>(rank + 1)];
  distributed.set_range(first, first == end ? first : end - 1);
  distributed.set_range_array(ranges);
  if (local_nodes != 0) {
    auto const node = distributed.new_node();
    distributed.setNodeWeight(
        node, mpi_tools_failure_probe::profile_aggregate_overflow
                  ? std::numeric_limits<parhip::NodeWeight>::max()
                  : 1);
    distributed.setSecondPartitionIndex(node, 0);
    if (local_edges != 0) {
      auto const edge = distributed.new_edge(node, node);
      distributed.setEdgeWeight(edge, 1);
    }
  }
  distributed.finish_construction();

  parhip::complete_graph_access complete{communicator};
  parhip::PPartitionConfig config{};
  mpi_tools_failure_probe::unsafe_profile =
      std::string_view{argv[1]} == "unsafe-profile";
  if (mpi_tools_failure_probe::unsafe_profile) {
    config.k = 0;
  }
  if (mpi_tools_failure_probe::structural_self_loop) {
    config.k = 1;
    config.upper_bound_partition = 1;
  }
  if (mpi_tools_failure_probe::vcycle_mismatch ||
      mpi_tools_failure_probe::initial_algorithm_mismatch ||
      mpi_tools_failure_probe::profile_aggregate_overflow) {
    config.k = 1;
    config.upper_bound_partition = 1;
  }
  if (mpi_tools_failure_probe::vcycle_mismatch) {
    config.vcycle = rank == 0;
  }
  if (mpi_tools_failure_probe::initial_algorithm_mismatch && rank == 1) {
    config.initial_partitioning_algorithm =
        parhip::InitialPartitioningAlgorithm::KAFFPAEFAST;
  }
  mpi_tools_failure_probe::expected_communicator = communicator;
  mpi_tools_failure_probe::communicator_rank = rank;
  mpi_tools_failure_probe::active = !mpi_tools_failure_probe::structural_self_loop;
  auto observer =
      std::optional<parhip::mpi_tools_detail::scoped_serial_kernel_profile_observer>{};
  if (mpi_tools_failure_probe::unsafe_profile) {
    observer.emplace(mpi_tools_failure_probe::observe_safe_serial_profile,
                     nullptr);
  }
  if (mpi_tools_failure_probe::unsafe_profile) {
    parhip::mpi_tools{}.collect_parallel_graph_to_checked_serial_graph(
        communicator, config, distributed, complete);
  } else if (mpi_tools_failure_probe::structural_self_loop) {
    parhip::mpi_tools{}.collect_parallel_graph_to_checked_serial_graph(
        communicator, config, distributed, complete);
  } else if (mpi_tools_failure_probe::vcycle_mismatch ||
             mpi_tools_failure_probe::initial_algorithm_mismatch ||
             mpi_tools_failure_probe::profile_aggregate_overflow) {
    parhip::mpi_tools{}.collect_parallel_graph_to_checked_serial_graph(
        communicator, config, distributed, complete);
  } else {
    parhip::mpi_tools{}.collect_parallel_graph_to_local_graph(
        communicator, config, distributed, complete);
  }
  mpi_tools_failure_probe::write_text("returned-from-failure\n");
  std::_Exit(92);
}
