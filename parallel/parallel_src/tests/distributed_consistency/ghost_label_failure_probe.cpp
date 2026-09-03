#include <mpi.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "communication/ghost_label_update.h"
#include "communication/mpi_error.h"
#include "data_structure/parallel_graph_access.h"
#include "kahip_mpi_capabilities.h"
#include "partition_config.h"

// KAHIP_PMPI_CALLBACK_REGION_BEGIN
namespace ghost_failure_probe {
enum class mode : std::uint8_t {
  active_incremental_then_global,
  skewed_incremental_protocol,
  corrupted_incremental_completion,
};

inline mode selected = mode::active_incremental_then_global;
inline int world_rank = -1;
inline int count_exchanges = 0;
inline int blocking_payloads = 0;
inline int immediate_payloads = 0;
inline int completions = 0;
inline bool callback_error = false;
inline bool corruption_fired = false;
inline MPI_Request tracked_request = MPI_REQUEST_NULL;
inline parhip::ghost_label_update* tracked_record = nullptr;

template <typename Count, typename Displacement>
void track_incremental_payload(void* receive_buffer,
                               Count const receive_counts[],
                               Displacement const receive_displacements[],
                               MPI_Datatype receive_datatype,
                               MPI_Request const* request) noexcept {
  if (selected != mode::corrupted_incremental_completion || world_rank != 0) {
    return;
  }
  MPI_Aint lower_bound = -1;
  MPI_Aint extent = -1;
  if (receive_buffer == nullptr || receive_counts == nullptr ||
      receive_displacements == nullptr || request == nullptr ||
      *request == MPI_REQUEST_NULL || receive_counts[0] <= 0 ||
      receive_displacements[0] < 0 ||
      PMPI_Type_get_extent(receive_datatype, &lower_bound, &extent) !=
          MPI_SUCCESS ||
      lower_bound != 0 ||
      extent != static_cast<MPI_Aint>(sizeof(parhip::ghost_label_update)) ||
      !std::in_range<std::ptrdiff_t>(receive_displacements[0])) {
    callback_error = true;
    return;
  }
  tracked_request = *request;
  tracked_record = static_cast<parhip::ghost_label_update*>(receive_buffer) +
                   static_cast<std::ptrdiff_t>(receive_displacements[0]);
}

void corrupt_completed_incremental_payload() noexcept {
  if (selected != mode::corrupted_incremental_completion || world_rank != 0) {
    return;
  }
  if (tracked_record == nullptr) {
    callback_error = true;
    return;
  }
  tracked_record->global_id = std::numeric_limits<parhip::NodeID>::max();
  corruption_fired = true;
}

[[nodiscard]] auto expected_abort_state() noexcept -> bool {
  if (callback_error) {
    return false;
  }
  switch (selected) {
    case mode::active_incremental_then_global:
      return count_exchanges == 1 && blocking_payloads == 0 &&
             immediate_payloads == 1 && completions == 0;
    case mode::skewed_incremental_protocol:
      return count_exchanges == 0 && blocking_payloads == 0 &&
             immediate_payloads == 0 && completions == 0;
    case mode::corrupted_incremental_completion:
      return count_exchanges == 1 && blocking_payloads == 0 &&
             immediate_payloads == 1 && completions == 1 &&
             (world_rank != 0 || corruption_fired);
  }
  return false;
}

void write_text(std::string_view text) noexcept {
  static_cast<void>(::write(STDERR_FILENO, text.data(), text.size()));
}

[[noreturn]] void observed_abort() noexcept {
  if (!expected_abort_state()) {
    write_text(
        "observed ghost-label MPI_Abort with unexpected callback state\n");
    std::_Exit(91);
  }
  switch (selected) {
    case mode::active_incremental_then_global:
      write_text(
          "observed active-incremental/global MPI_Abort before blocking "
          "payload\n");
      break;
    case mode::skewed_incremental_protocol:
      write_text(
          "observed skewed incremental-protocol MPI_Abort before first "
          "payload\n");
      break;
    case mode::corrupted_incremental_completion:
      write_text(
          "observed corrupted incremental-completion terminal MPI_Abort\n");
      break;
  }
  std::_Exit(86);
}
}  // namespace ghost_failure_probe

static_assert(noexcept(
    ghost_failure_probe::track_incremental_payload<int, int>(nullptr,
                                                             nullptr,
                                                             nullptr,
                                                             MPI_DATATYPE_NULL,
                                                             nullptr)));
static_assert(noexcept(
    ghost_failure_probe::track_incremental_payload<MPI_Count, MPI_Aint>(
        nullptr,
        nullptr,
        nullptr,
        MPI_DATATYPE_NULL,
        nullptr)));
static_assert(
    noexcept(ghost_failure_probe::corrupt_completed_incremental_payload()));
static_assert(noexcept(ghost_failure_probe::expected_abort_state()));

extern "C" int MPI_Neighbor_alltoall(void const* send_buffer,
                                     int send_count,
                                     MPI_Datatype send_datatype,
                                     void* receive_buffer,
                                     int receive_count,
                                     MPI_Datatype receive_datatype,
                                     MPI_Comm communicator) {
  ++ghost_failure_probe::count_exchanges;
  return PMPI_Neighbor_alltoall(send_buffer, send_count, send_datatype,
                                receive_buffer, receive_count, receive_datatype,
                                communicator);
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
  ++ghost_failure_probe::blocking_payloads;
  return PMPI_Neighbor_alltoallv(send_buffer, send_counts, send_displacements,
                                 send_datatype, receive_buffer, receive_counts,
                                 receive_displacements, receive_datatype,
                                 communicator);
}

#if KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_C
extern "C" int MPI_Neighbor_alltoallv_c(void const* send_buffer,
                                        MPI_Count const send_counts[],
                                        MPI_Aint const send_displacements[],
                                        MPI_Datatype send_datatype,
                                        void* receive_buffer,
                                        MPI_Count const receive_counts[],
                                        MPI_Aint const receive_displacements[],
                                        MPI_Datatype receive_datatype,
                                        MPI_Comm communicator) {
  ++ghost_failure_probe::blocking_payloads;
  return PMPI_Neighbor_alltoallv_c(send_buffer, send_counts, send_displacements,
                                   send_datatype, receive_buffer,
                                   receive_counts, receive_displacements,
                                   receive_datatype, communicator);
}
#endif

extern "C" int MPI_Ineighbor_alltoallv(void const* send_buffer,
                                       int const send_counts[],
                                       int const send_displacements[],
                                       MPI_Datatype send_datatype,
                                       void* receive_buffer,
                                       int const receive_counts[],
                                       int const receive_displacements[],
                                       MPI_Datatype receive_datatype,
                                       MPI_Comm communicator,
                                       MPI_Request* request) {
  ++ghost_failure_probe::immediate_payloads;
  auto const result = PMPI_Ineighbor_alltoallv(
      send_buffer, send_counts, send_displacements, send_datatype,
      receive_buffer, receive_counts, receive_displacements, receive_datatype,
      communicator, request);
  if (result == MPI_SUCCESS) {
    ghost_failure_probe::track_incremental_payload(
        receive_buffer, receive_counts, receive_displacements, receive_datatype,
        request);
  }
  return result;
}

#if KAHIP_HAVE_MPI_INEIGHBOR_ALLTOALLV_C
extern "C" int MPI_Ineighbor_alltoallv_c(void const* send_buffer,
                                         MPI_Count const send_counts[],
                                         MPI_Aint const send_displacements[],
                                         MPI_Datatype send_datatype,
                                         void* receive_buffer,
                                         MPI_Count const receive_counts[],
                                         MPI_Aint const receive_displacements[],
                                         MPI_Datatype receive_datatype,
                                         MPI_Comm communicator,
                                         MPI_Request* request) {
  ++ghost_failure_probe::immediate_payloads;
  auto const result = PMPI_Ineighbor_alltoallv_c(
      send_buffer, send_counts, send_displacements, send_datatype,
      receive_buffer, receive_counts, receive_displacements, receive_datatype,
      communicator, request);
  if (result == MPI_SUCCESS) {
    ghost_failure_probe::track_incremental_payload(
        receive_buffer, receive_counts, receive_displacements, receive_datatype,
        request);
  }
  return result;
}
#endif

extern "C" int MPI_Wait(MPI_Request* request, MPI_Status* status) {
  auto const tracked =
      request != nullptr &&
      ghost_failure_probe::tracked_request != MPI_REQUEST_NULL &&
      *request == ghost_failure_probe::tracked_request;
  ++ghost_failure_probe::completions;
  auto const result = PMPI_Wait(request, status);
  if (tracked && result == MPI_SUCCESS) {
    ghost_failure_probe::corrupt_completed_incremental_payload();
  }
  return result;
}

extern "C" int MPI_Abort(MPI_Comm, int) {
  ghost_failure_probe::observed_abort();
}
// KAHIP_PMPI_CALLBACK_REGION_END

namespace {
[[nodiscard]] auto parse_mode(std::string_view value)
    -> std::optional<ghost_failure_probe::mode> {
  if (value == "active-incremental-then-global") {
    return ghost_failure_probe::mode::active_incremental_then_global;
  }
  if (value == "skewed-incremental-protocol") {
    return ghost_failure_probe::mode::skewed_incremental_protocol;
  }
  if (value == "corrupted-incremental-completion") {
    return ghost_failure_probe::mode::corrupted_incremental_completion;
  }
  return std::nullopt;
}

void build_payload_graph(parhip::parallel_graph_access& graph,
                         int rank,
                         bool skew_protocol) {
  parhip::parallel_graph_access::set_comm_rounds(
      static_cast<parhip::ULONG>(8 + (skew_protocol ? rank : 0)));
  graph.start_construction(1, 1, 2, 2, true);
  graph.set_range(static_cast<parhip::NodeID>(rank),
                  static_cast<parhip::NodeID>(rank));
  auto ranges = std::vector<parhip::NodeID>{0, 1, 2};
  graph.set_range_array(ranges);
  auto const local = graph.new_node();
  graph.setNodeWeight(local, 1);
  graph.setNodeLabel(local, static_cast<parhip::NodeID>(rank));
  graph.setSecondPartitionIndex(local, 0);
  auto const edge =
      graph.new_edge(local, static_cast<parhip::NodeID>(rank == 0 ? 1 : 0));
  graph.setEdgeWeight(edge, 1);
  graph.finish_construction();

  auto config = parhip::PPartitionConfig{};
  config.k = 1;
  config.total_num_labels = 1024;
  graph.init_balance_management(config);
  graph.setNodeLabel(local, static_cast<parhip::NodeID>(100 + rank));
}

[[noreturn]] void unexpected_success(std::string_view detail) {
  std::fwrite(detail.data(), sizeof(char), detail.size(), stderr);
  std::fputc('\n', stderr);
  std::_Exit(0);
}
}  // namespace

int main(int argc, char* argv[]) {
  if (argc != 2) {
    std::fputs("usage: ghost_label_failure_probe MODE\n", stderr);
    return 64;
  }
  auto const selected = parse_mode(argv[1]);
  if (!selected.has_value()) {
    std::fputs("unknown ghost-label failure-probe mode\n", stderr);
    return 64;
  }
  ghost_failure_probe::selected = *selected;

  if (MPI_Init(&argc, &argv) != MPI_SUCCESS) {
    std::fputs("MPI_Init failed\n", stderr);
    return 70;
  }
  auto size = 0;
  if (PMPI_Comm_rank(MPI_COMM_WORLD, &ghost_failure_probe::world_rank) !=
          MPI_SUCCESS ||
      PMPI_Comm_size(MPI_COMM_WORLD, &size) != MPI_SUCCESS || size != 2) {
    std::fputs("ghost-label failure probe requires exactly two ranks\n",
               stderr);
    std::_Exit(70);
  }

  try {
    auto* graph = new parhip::parallel_graph_access{MPI_COMM_WORLD};
    build_payload_graph(
        *graph, ghost_failure_probe::world_rank,
        *selected == ghost_failure_probe::mode::skewed_incremental_protocol);
    switch (*selected) {
      case ghost_failure_probe::mode::active_incremental_then_global:
        graph->update_ghost_node_data(false);
        graph->update_ghost_node_data_global();
        unexpected_success(
            "active incremental then global returned without fail-fast");
      case ghost_failure_probe::mode::skewed_incremental_protocol:
        graph->update_ghost_node_data(false);
        unexpected_success(
            "skewed incremental protocol posted its first payload");
      case ghost_failure_probe::mode::corrupted_incremental_completion:
        graph->update_ghost_node_data(false);
        graph->update_ghost_node_data(false);
        unexpected_success(
            "corrupted completed incremental payload returned normally");
    }
  } catch (parhip::mpi::mpi_error const&) {
    unexpected_success(
        "ghost-label failure escaped as a recoverable mpi_error");
  } catch (...) {
    unexpected_success("ghost-label failure escaped as a C++ exception");
  }
}
