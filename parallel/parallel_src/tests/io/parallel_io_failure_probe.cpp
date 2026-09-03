#include <mpi.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "data_structure/parallel_graph_access.h"
#include "io/parallel_graph_io.h"
#include "io/parallel_vector_io.h"
#include "partition_config.h"

// KAHIP_PMPI_CALLBACK_REGION_BEGIN
namespace parallel_io_failure_probe {
enum class mode : unsigned char {
  vector_missing,
  vector_truncated,
  graph_truncated,
  text_missing,
};

inline bool active = false;
inline mode selected = mode::vector_missing;
inline MPI_Comm expected_communicator = MPI_COMM_NULL;
inline int communicator_rank = -1;
inline int finalizations = 0;
inline std::string fixture;

void write_text(std::string_view text) noexcept {
  static_cast<void>(::write(STDERR_FILENO, text.data(), text.size()));
}

[[nodiscard]] auto mode_marker() noexcept -> std::string_view {
  switch (selected) {
    case mode::vector_missing:
      return "parallel-io-vector-missing";
    case mode::vector_truncated:
      return "parallel-io-vector-truncated";
    case mode::graph_truncated:
      return "parallel-io-graph-truncated";
    case mode::text_missing:
      return "parallel-io-text-missing";
  }
  return "parallel-io-unknown";
}

[[noreturn]] void observed_abort(MPI_Comm communicator,
                                 int error_code) noexcept {
  int relation = MPI_UNEQUAL;
  if (error_code != EXIT_FAILURE || finalizations != 0 ||
      communicator_rank < 0 || communicator_rank > 1 ||
      PMPI_Comm_compare(communicator, expected_communicator, &relation) !=
          MPI_SUCCESS ||
      (relation != MPI_IDENT && relation != MPI_CONGRUENT)) {
    write_text("observed parallel-io MPI_Abort with unexpected state\n");
    std::_Exit(91);
  }
  if (communicator_rank == 0) {
    switch (selected) {
      case mode::vector_missing:
        write_text(
            "observed MPI_Abort rank=0 parallel-io-vector-missing "
            "affected-communicator; internal MPI_Finalize counter is zero\n");
        break;
      case mode::vector_truncated:
        write_text(
            "observed MPI_Abort rank=0 parallel-io-vector-truncated "
            "affected-communicator; internal MPI_Finalize counter is zero\n");
        break;
      case mode::graph_truncated:
        write_text(
            "observed MPI_Abort rank=0 parallel-io-graph-truncated "
            "affected-communicator; internal MPI_Finalize counter is zero\n");
        break;
      case mode::text_missing:
        write_text(
            "observed MPI_Abort rank=0 parallel-io-text-missing "
            "affected-communicator; internal MPI_Finalize counter is zero\n");
        break;
    }
  } else {
    switch (selected) {
      case mode::vector_missing:
        write_text(
            "observed MPI_Abort rank=1 parallel-io-vector-missing "
            "affected-communicator; internal MPI_Finalize counter is zero\n");
        break;
      case mode::vector_truncated:
        write_text(
            "observed MPI_Abort rank=1 parallel-io-vector-truncated "
            "affected-communicator; internal MPI_Finalize counter is zero\n");
        break;
      case mode::graph_truncated:
        write_text(
            "observed MPI_Abort rank=1 parallel-io-graph-truncated "
            "affected-communicator; internal MPI_Finalize counter is zero\n");
        break;
      case mode::text_missing:
        write_text(
            "observed MPI_Abort rank=1 parallel-io-text-missing "
            "affected-communicator; internal MPI_Finalize counter is zero\n");
        break;
    }
  }
  if (PMPI_Barrier(MPI_COMM_WORLD) != MPI_SUCCESS) {
    std::_Exit(90);
  }
  if (communicator_rank == 0) {
    static_cast<void>(::unlink(fixture.c_str()));
  }
  std::_Exit(86);
}
}  // namespace parallel_io_failure_probe

static_assert(noexcept(parallel_io_failure_probe::write_text({})));
static_assert(noexcept(parallel_io_failure_probe::mode_marker()));
static_assert(noexcept(parallel_io_failure_probe::observed_abort(MPI_COMM_NULL,
                                                                 0)));

extern "C" int MPI_Finalize() {
  if (parallel_io_failure_probe::active) {
    ++parallel_io_failure_probe::finalizations;
    return MPI_SUCCESS;
  }
  return PMPI_Finalize();
}

extern "C" int MPI_Abort(MPI_Comm communicator, int error_code) {
  parallel_io_failure_probe::observed_abort(communicator, error_code);
}
// KAHIP_PMPI_CALLBACK_REGION_END

namespace {
using parhip::NodeID;
using parhip::ULONG;

[[nodiscard]] auto parse_mode(std::string_view value)
    -> parallel_io_failure_probe::mode {
  using mode = parallel_io_failure_probe::mode;
  if (value == "vector-missing") {
    return mode::vector_missing;
  }
  if (value == "vector-truncated") {
    return mode::vector_truncated;
  }
  if (value == "graph-truncated") {
    return mode::graph_truncated;
  }
  if (value == "text-missing") {
    return mode::text_missing;
  }
  std::_Exit(2);
}

void write_fixture(parallel_io_failure_probe::mode selected,
                   std::string const& filename) {
  auto output = std::ofstream{filename, std::ios::binary | std::ios::trunc};
  switch (selected) {
    case parallel_io_failure_probe::mode::vector_missing: {
      auto const values = std::array<ULONG, 4>{1, 2, 17, 19};
      output.write(reinterpret_cast<char const*>(values.data()),
                   static_cast<std::streamsize>(sizeof(values)));
      break;
    }
    case parallel_io_failure_probe::mode::vector_truncated: {
      auto const values = std::array<ULONG, 3>{1, 2, 17};
      output.write(reinterpret_cast<char const*>(values.data()),
                   static_cast<std::streamsize>(sizeof(values)));
      break;
    }
    case parallel_io_failure_probe::mode::graph_truncated: {
      auto const header = std::array<ULONG, 3>{3, 2, 2};
      auto const offsets = std::array<ULONG, 3>{48, 56, 64};
      auto const adjacency = std::array<ULONG, 1>{1};
      output.write(reinterpret_cast<char const*>(header.data()),
                   static_cast<std::streamsize>(sizeof(header)));
      output.write(reinterpret_cast<char const*>(offsets.data()),
                   static_cast<std::streamsize>(sizeof(offsets)));
      output.write(reinterpret_cast<char const*>(adjacency.data()),
                   static_cast<std::streamsize>(sizeof(adjacency)));
      break;
    }
    case parallel_io_failure_probe::mode::text_missing:
      output << "2 1\n2\n1\n";
      break;
  }
  if (!output) {
    std::_Exit(3);
  }
}

void build_partition_graph(parhip::parallel_graph_access& graph, int rank) {
  graph.start_construction(1, 0, 2, 0, false);
  graph.set_range(static_cast<NodeID>(rank), static_cast<NodeID>(rank));
  auto ranges = std::vector<NodeID>{0, 1, 2};
  graph.set_range_array(ranges);
  auto const node = graph.new_node();
  graph.setNodeWeight(node, 1);
  graph.setNodeLabel(node, 99);
  graph.setSecondPartitionIndex(node, 0);
  graph.finish_construction();
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
    filename = "/tmp/kahip-parallel-io-failure-" + std::to_string(::getpid()) +
               ".fixture";
    write_fixture(selected, filename);
  }
  std::uint64_t filename_size = filename.size();
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

  parallel_io_failure_probe::selected = selected;
  parallel_io_failure_probe::fixture = filename;
  parallel_io_failure_probe::expected_communicator = communicator;
  parallel_io_failure_probe::communicator_rank = rank;

  auto graph = parhip::parallel_graph_access{communicator};
  auto config = parhip::PPartitionConfig{};
  config.binary_io_window_size = 2;
  parallel_io_failure_probe::active = true;
  switch (selected) {
    case parallel_io_failure_probe::mode::vector_missing:
      build_partition_graph(graph, rank);
      if (rank == 1) {
        filename += ".missing";
      }
      parhip::parallel_vector_io{}.readPartitionBinaryParallel(config, graph,
                                                               filename);
      break;
    case parallel_io_failure_probe::mode::vector_truncated:
      build_partition_graph(graph, rank);
      parhip::parallel_vector_io{}.readPartitionBinaryParallel(config, graph,
                                                               filename);
      break;
    case parallel_io_failure_probe::mode::graph_truncated:
      static_cast<void>(parhip::parallel_graph_io::readGraphBinary(
          config, graph, filename, rank, 2, communicator));
      break;
    case parallel_io_failure_probe::mode::text_missing:
      if (rank == 1) {
        filename += ".missing";
      }
      static_cast<void>(parhip::parallel_graph_io::readGraphWeightedFlexible(
          graph, filename, rank, 2, communicator));
      break;
  }
  parallel_io_failure_probe::write_text("returned-from-failure\n");
  std::_Exit(92);
}
