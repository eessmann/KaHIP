#include <mpi.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "communication/mpi_handles.h"
#include "data_structure/parallel_graph_access.h"
#include "dspac/edge_balanced_graph_io.h"
#include "partition_config.h"

// KAHIP_PMPI_CALLBACK_REGION_BEGIN
namespace edge_balanced_failure_probe {
inline auto active = false;
inline auto expected_communicator = MPI_COMM_NULL;
inline auto communicator_rank = -1;
inline auto finalizations = 0;
inline auto fixture = std::array<char, 1024>{};
inline auto marker = std::array<char, 256>{};
inline auto marker_size = std::size_t{0};

void write_text(std::string_view text) noexcept {
  static_cast<void>(::write(STDERR_FILENO, text.data(), text.size()));
}

[[noreturn]] void observed_abort(MPI_Comm communicator,
                                 int error_code) noexcept {
  auto relation = int{MPI_UNEQUAL};
  if (error_code != EXIT_FAILURE || finalizations != 0 ||
      communicator_rank < 0 || communicator_rank > 1 ||
      PMPI_Comm_compare(communicator, expected_communicator, &relation) !=
          MPI_SUCCESS ||
      (relation != MPI_IDENT && relation != MPI_CONGRUENT)) {
    write_text("observed edge-balanced MPI_Abort with unexpected state\n");
    std::_Exit(91);
  }
  write_text(std::string_view{marker.data(), marker_size});
  if (PMPI_Barrier(MPI_COMM_WORLD) != MPI_SUCCESS) {
    std::_Exit(90);
  }
  if (communicator_rank == 0) {
    static_cast<void>(::unlink(fixture.data()));
  }
  std::_Exit(86);
}
}  // namespace edge_balanced_failure_probe

static_assert(noexcept(edge_balanced_failure_probe::write_text({})));
static_assert(
    noexcept(edge_balanced_failure_probe::observed_abort(MPI_COMM_NULL, 0)));

extern "C" int MPI_Finalize() {
  if (edge_balanced_failure_probe::active) {
    ++edge_balanced_failure_probe::finalizations;
    return MPI_SUCCESS;
  }
  return PMPI_Finalize();
}

extern "C" int MPI_Abort(MPI_Comm communicator, int error_code) {
  edge_balanced_failure_probe::observed_abort(communicator, error_code);
}
// KAHIP_PMPI_CALLBACK_REGION_END

namespace {
using parhip::ULONG;

enum class failure_mode : unsigned char {
  truncated,
  nonmonotone,
  unaligned,
  wrong_terminal,
  invalid_target,
  zero_window,
};

[[nodiscard]] auto parse_mode(std::string_view value) -> failure_mode {
  if (value == "truncated") {
    return failure_mode::truncated;
  }
  if (value == "nonmonotone") {
    return failure_mode::nonmonotone;
  }
  if (value == "unaligned") {
    return failure_mode::unaligned;
  }
  if (value == "wrong-terminal") {
    return failure_mode::wrong_terminal;
  }
  if (value == "invalid-target") {
    return failure_mode::invalid_target;
  }
  if (value == "window-zero") {
    return failure_mode::zero_window;
  }
  std::_Exit(2);
}

void write_fixture(failure_mode mode, std::string const& filename) {
  auto output = std::ofstream{filename, std::ios::binary | std::ios::trunc};
  auto const header = std::array<ULONG, 3>{3, 3, 4};
  auto offsets = std::array<ULONG, 4>{56, 72, 80, 88};
  auto adjacency = std::array<ULONG, 4>{2, 1, 0, 1};
  switch (mode) {
    case failure_mode::nonmonotone:
      offsets = {56, 80, 72, 88};
      break;
    case failure_mode::unaligned:
      offsets = {56, 73, 80, 88};
      break;
    case failure_mode::wrong_terminal:
      offsets = {56, 72, 80, 80};
      break;
    case failure_mode::invalid_target:
      adjacency = {2, 1, 0, 3};
      break;
    case failure_mode::truncated:
    case failure_mode::zero_window:
      break;
  }

  output.write(reinterpret_cast<char const*>(header.data()),
               static_cast<std::streamsize>(sizeof(header)));
  output.write(reinterpret_cast<char const*>(offsets.data()),
               static_cast<std::streamsize>(sizeof(offsets)));
  auto const adjacency_words =
      mode == failure_mode::truncated ? std::size_t{3} : adjacency.size();
  output.write(reinterpret_cast<char const*>(adjacency.data()),
               static_cast<std::streamsize>(adjacency_words * sizeof(ULONG)));
  if (!output) {
    std::_Exit(3);
  }
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 2 || MPI_Init(&argc, &argv) != MPI_SUCCESS) {
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
      communicator == MPI_COMM_NULL ||
      MPI_Comm_set_errhandler(communicator, MPI_ERRORS_RETURN) != MPI_SUCCESS) {
    return 4;
  }
  auto rank = -1;
  if (MPI_Comm_rank(communicator, &rank) != MPI_SUCCESS) {
    return 5;
  }

  auto const selected = parse_mode(argv[1]);
  auto filename = std::string{};
  if (rank == 0) {
    filename = "/tmp/kahip-edge-balanced-failure-" +
               std::to_string(::getpid()) + ".bgf";
    write_fixture(selected, filename);
  }
  auto filename_size = std::uint64_t{filename.size()};
  if (MPI_Bcast(&filename_size, 1, MPI_UINT64_T, 0, communicator) !=
          MPI_SUCCESS ||
      filename_size >
          static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
    return 6;
  }
  filename.resize(static_cast<std::size_t>(filename_size));
  if (MPI_Bcast(filename.data(), static_cast<int>(filename_size), MPI_CHAR, 0,
                communicator) != MPI_SUCCESS ||
      MPI_Barrier(communicator) != MPI_SUCCESS) {
    return 7;
  }

  edge_balanced_failure_probe::expected_communicator = communicator;
  edge_balanced_failure_probe::communicator_rank = rank;
  if (filename.size() + 1 > edge_balanced_failure_probe::fixture.size()) {
    return 8;
  }
  std::ranges::copy(filename, edge_balanced_failure_probe::fixture.begin());
  edge_balanced_failure_probe::fixture[filename.size()] = '\0';
  auto const marker =
      "observed MPI_Abort rank=" + std::to_string(rank) + " edge-balanced-" +
      std::string{argv[1]} +
      " affected-communicator; internal MPI_Finalize counter is zero\n";
  if (marker.size() > edge_balanced_failure_probe::marker.size()) {
    return 8;
  }
  std::ranges::copy(marker, edge_balanced_failure_probe::marker.begin());
  edge_balanced_failure_probe::marker_size = marker.size();

  auto graph = parhip::parallel_graph_access{communicator};
  auto config = parhip::PPartitionConfig{};
  config.binary_io_window_size = selected == failure_mode::zero_window ? 0 : 1;
  auto permutation = std::vector<parhip::EdgeID>{};
  edge_balanced_failure_probe::active = true;
  parhip::edge_balanced_graph_io::read_binary_graph_edge_balanced(
      graph, filename, config, permutation,
      parhip::mpi::communicator_view{communicator});
  edge_balanced_failure_probe::write_text("returned-from-failure\n");
  std::_Exit(92);
}
