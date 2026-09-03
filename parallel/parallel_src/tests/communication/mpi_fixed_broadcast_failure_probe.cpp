#include <mpi.h>
#include <unistd.h>

#include <array>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <span>
#include <string_view>

#include "communication/mpi_fixed_broadcast.h"
#include "definitions.h"
#include "io/parallel_graph_io.h"

// KAHIP_PMPI_CALLBACK_REGION_BEGIN
namespace fixed_broadcast_failure_probe {
enum class mode : unsigned char {
  status,
  header,
  partition_map,
  mismatched_map,
  previous_cut,
  previous_weight,
  missing_file,
  truncated_header,
  invalid_version,
  intercommunicator,
};

inline bool active = false;
inline mode selected = mode::status;
inline MPI_Comm expected_communicator = MPI_COMM_NULL;
inline int broadcasts = 0;
inline int finalizations = 0;
inline bool callback_error = false;

void write_text(std::string_view text) noexcept {
  static_cast<void>(::write(STDERR_FILENO, text.data(), text.size()));
}

[[nodiscard]] auto valid_broadcast(int index,
                                   MPI_Count count,
                                   MPI_Datatype datatype,
                                   int root,
                                   MPI_Comm communicator,
                                   void const* buffer) noexcept -> bool {
  if (communicator != expected_communicator || buffer == nullptr || root != 0) {
    return false;
  }
  switch (selected) {
    case mode::status:
      return index == 1 && count == 1 && datatype == MPI_INT;
    case mode::header:
      return index == 1 && count == 3 && datatype == MPI_UNSIGNED_LONG_LONG;
    case mode::partition_map:
      return index == 1 && count == 5 && datatype == MPI_INT;
    case mode::mismatched_map:
      return false;
    case mode::previous_cut:
      if (index == 1) {
        return count == 5 && datatype == MPI_INT;
      }
      return index == 2 && count == 1 && datatype == MPI_UNSIGNED_LONG_LONG;
    case mode::previous_weight:
      if (index == 1) {
        return count == 5 && datatype == MPI_INT;
      }
      return (index == 2 || index == 3) && count == 1 &&
             datatype == MPI_UNSIGNED_LONG_LONG;
    case mode::missing_file:
    case mode::truncated_header:
      return index == 1 && count == 1 && datatype == MPI_INT;
    case mode::invalid_version:
      if (index == 1) {
        return count == 1 && datatype == MPI_INT;
      }
      return index == 2 && count == 3 && datatype == MPI_UNSIGNED_LONG_LONG;
    case mode::intercommunicator:
      return false;
  }
  return false;
}

[[nodiscard]] auto should_fail_broadcast(int index) noexcept -> bool {
  switch (selected) {
    case mode::status:
    case mode::header:
    case mode::partition_map:
      return index == 1;
    case mode::mismatched_map:
      return false;
    case mode::previous_cut:
      return index == 2;
    case mode::previous_weight:
      return index == 3;
    case mode::missing_file:
    case mode::truncated_header:
    case mode::invalid_version:
      return false;
    case mode::intercommunicator:
      return false;
  }
  return true;
}

[[nodiscard]] auto expected_abort_state() noexcept -> bool {
  if (callback_error || finalizations != 0) {
    return false;
  }
  switch (selected) {
    case mode::status:
    case mode::header:
    case mode::partition_map:
    case mode::missing_file:
    case mode::truncated_header:
      return broadcasts == 1;
    case mode::mismatched_map:
      return broadcasts == 0;
    case mode::previous_cut:
    case mode::invalid_version:
      return broadcasts == 2;
    case mode::previous_weight:
      return broadcasts == 3;
    case mode::intercommunicator:
      return broadcasts == 0;
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
    write_text("observed fixed-broadcast MPI_Abort with unexpected state\n");
    std::_Exit(91);
  }
  write_text(
      "observed fixed-broadcast MPI_Abort on affected communicator; "
      "internal MPI_Finalize counter is zero\n");
  if (PMPI_Barrier(MPI_COMM_WORLD) != MPI_SUCCESS) {
    std::_Exit(90);
  }
  std::_Exit(86);
}
}  // namespace fixed_broadcast_failure_probe

static_assert(noexcept(fixed_broadcast_failure_probe::write_text({})));
static_assert(
    noexcept(fixed_broadcast_failure_probe::valid_broadcast(0,
                                                            0,
                                                            MPI_DATATYPE_NULL,
                                                            0,
                                                            MPI_COMM_NULL,
                                                            nullptr)));
static_assert(
    noexcept(fixed_broadcast_failure_probe::should_fail_broadcast(0)));
static_assert(noexcept(fixed_broadcast_failure_probe::expected_abort_state()));
static_assert(
    noexcept(fixed_broadcast_failure_probe::observed_abort(MPI_COMM_NULL, 0)));

extern "C" int MPI_Bcast(void* buffer,
                         int count,
                         MPI_Datatype datatype,
                         int root,
                         MPI_Comm communicator) {
  if (!fixed_broadcast_failure_probe::active) {
    return PMPI_Bcast(buffer, count, datatype, root, communicator);
  }
  auto const index = ++fixed_broadcast_failure_probe::broadcasts;
  if (!fixed_broadcast_failure_probe::valid_broadcast(
          index, count, datatype, root, communicator, buffer)) {
    fixed_broadcast_failure_probe::callback_error = true;
    return MPI_ERR_OTHER;
  }
  if (fixed_broadcast_failure_probe::should_fail_broadcast(index)) {
    return MPI_ERR_OTHER;
  }
  return PMPI_Bcast(buffer, count, datatype, root, communicator);
}

#if KAHIP_HAVE_MPI_BCAST_C
extern "C" int MPI_Bcast_c(void* buffer,
                           MPI_Count count,
                           MPI_Datatype datatype,
                           int root,
                           MPI_Comm communicator) {
  if (!fixed_broadcast_failure_probe::active) {
    return PMPI_Bcast_c(buffer, count, datatype, root, communicator);
  }
  auto const index = ++fixed_broadcast_failure_probe::broadcasts;
  if (!fixed_broadcast_failure_probe::valid_broadcast(
          index, count, datatype, root, communicator, buffer)) {
    fixed_broadcast_failure_probe::callback_error = true;
    return MPI_ERR_OTHER;
  }
  if (fixed_broadcast_failure_probe::should_fail_broadcast(index)) {
    return MPI_ERR_OTHER;
  }
  return PMPI_Bcast_c(buffer, count, datatype, root, communicator);
}
#endif

extern "C" int MPI_Finalize() {
  if (fixed_broadcast_failure_probe::active) {
    ++fixed_broadcast_failure_probe::finalizations;
    return MPI_SUCCESS;
  }
  return PMPI_Finalize();
}

extern "C" int MPI_Abort(MPI_Comm communicator, int error_code) {
  fixed_broadcast_failure_probe::observed_abort(communicator, error_code);
}
// KAHIP_PMPI_CALLBACK_REGION_END

namespace {
[[nodiscard]] auto parse_mode(std::string_view value)
    -> fixed_broadcast_failure_probe::mode {
  using mode = fixed_broadcast_failure_probe::mode;
  if (value == "status") {
    return mode::status;
  }
  if (value == "header") {
    return mode::header;
  }
  if (value == "partition-map") {
    return mode::partition_map;
  }
  if (value == "mismatched-map") {
    return mode::mismatched_map;
  }
  if (value == "previous-cut") {
    return mode::previous_cut;
  }
  if (value == "previous-weight") {
    return mode::previous_weight;
  }
  if (value == "missing-file") {
    return mode::missing_file;
  }
  if (value == "truncated-header") {
    return mode::truncated_header;
  }
  if (value == "invalid-version") {
    return mode::invalid_version;
  }
  if (value == "intercommunicator") {
    return mode::intercommunicator;
  }
  fixed_broadcast_failure_probe::write_text("unknown failure-probe mode\n");
  std::_Exit(2);
}

void write_graph_failure_file(std::string_view path,
                              int communicator_rank,
                              MPI_Comm communicator,
                              fixed_broadcast_failure_probe::mode selected) {
  if (communicator_rank == 0) {
    auto output =
        std::ofstream{path.data(), std::ios::binary | std::ios::trunc};
    if (selected == fixed_broadcast_failure_probe::mode::invalid_version) {
      auto const header = std::array<parhip::ULONG, 3>{4, 0, 0};
      output.write(reinterpret_cast<char const*>(header.data()),
                   static_cast<std::streamsize>(sizeof(header)));
    } else {
      constexpr auto truncated_header = std::byte{0x04};
      output.write(reinterpret_cast<char const*>(&truncated_header), 1);
    }
  }
  if (PMPI_Barrier(communicator) != MPI_SUCCESS) {
    std::_Exit(3);
  }
}

[[nodiscard]] auto make_intercommunicator(int world_rank) -> MPI_Comm {
  auto local = MPI_COMM_NULL;
  if (PMPI_Comm_split(MPI_COMM_WORLD, world_rank, 0, &local) != MPI_SUCCESS ||
      local == MPI_COMM_NULL) {
    std::_Exit(7);
  }
  auto intercommunicator = MPI_COMM_NULL;
  if (PMPI_Intercomm_create(local, 0, MPI_COMM_WORLD, 1 - world_rank, 719,
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
  if (argc != 3 || MPI_Init(&argc, &argv) != MPI_SUCCESS) {
    return 2;
  }

  auto world_rank = 0;
  auto world_size = 0;
  if (MPI_Comm_rank(MPI_COMM_WORLD, &world_rank) != MPI_SUCCESS ||
      MPI_Comm_size(MPI_COMM_WORLD, &world_size) != MPI_SUCCESS) {
    return 3;
  }

  using mode = fixed_broadcast_failure_probe::mode;
  auto const selected = parse_mode(argv[1]);
  auto communicator = MPI_COMM_NULL;
  if (selected == mode::intercommunicator) {
    if (world_size != 2) {
      return 4;
    }
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

  if (selected == mode::truncated_header || selected == mode::invalid_version) {
    write_graph_failure_file(argv[2], rank, communicator, selected);
  }

  fixed_broadcast_failure_probe::selected = selected;
  fixed_broadcast_failure_probe::expected_communicator = communicator;
  fixed_broadcast_failure_probe::active = true;
  auto const view = parhip::mpi::communicator_view{communicator};

  switch (selected) {
    case mode::status: {
      auto value = 1;
      parhip::mpi::broadcast_fixed(value, 0, view,
                                   "MPI_Bcast(binary graph read status)");
      break;
    }
    case mode::header: {
      auto value = std::array<parhip::ULONG, 3>{3, 11, 17};
      parhip::mpi::broadcast_fixed(std::span{value}, 0, view,
                                   "MPI_Bcast(binary graph header)");
      break;
    }
    case mode::partition_map:
    case mode::previous_cut:
    case mode::previous_weight: {
      auto partition_map = std::array{2, 3, 5, 7, 11};
      auto previous_cut = parhip::EdgeWeight{23};
      auto previous_weight = parhip::NodeWeight{29};
      parhip::mpi::broadcast_vcycle_state(
          std::span{partition_map}, previous_cut, previous_weight, 0, view);
      break;
    }
    case mode::mismatched_map: {
      auto partition_map = std::array{2, 3, 5, 7, 11};
      auto previous_cut = parhip::EdgeWeight{23};
      auto previous_weight = parhip::NodeWeight{29};
      auto const map_size = rank == 0 ? std::size_t{5} : std::size_t{4};
      parhip::mpi::broadcast_vcycle_state(
          std::span<int>{partition_map.data(), map_size}, previous_cut,
          previous_weight, 0, view);
      break;
    }
    case mode::missing_file:
    case mode::truncated_header:
    case mode::invalid_version: {
      auto config = parhip::PPartitionConfig{};
      auto graph = parhip::parallel_graph_access{communicator};
      static_cast<void>(parhip::parallel_graph_io::readGraphBinary(
          config, graph, argv[2], rank, size, communicator));
      break;
    }
    case mode::intercommunicator: {
      auto values = std::array{2, 3, 5};
      parhip::mpi::broadcast_bounded(std::span<int>{values}, 0, view,
                                     "MPI_Bcast(intercommunicator probe)");
      break;
    }
  }

  fixed_broadcast_failure_probe::write_text(
      "fixed-broadcast operation returned without fail-fast\n");
  std::_Exit(92);
}
