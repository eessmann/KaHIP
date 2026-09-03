#include <mpi.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string_view>
#include <vector>

#include "data_structure/parallel_graph_access.h"
#include "distributed_partitioning/distributed_partitioner.h"
#include "distributed_partitioning/initial_partitioning/random_initial_partitioning.h"
#include "partition_config.h"

// KAHIP_PMPI_CALLBACK_REGION_BEGIN
namespace distributed_partitioner_failure_probe {
enum class mode : unsigned char {
  zero_k,
  mismatched_k,
  zero_cluster_factor,
  infinite_cluster_factor,
  negative_choice_count,
  choice_capacity,
  exhausted_choice_cursor,
  rank_backend,
  mismatched_communicator,
};

inline mode selected = mode::zero_k;
inline bool active = false;
inline MPI_Comm affected_communicator = MPI_COMM_NULL;
inline parhip::parallel_graph_access* graph = nullptr;
inline parhip::NodeID sentinel = 99;

void write_text(std::string_view text) noexcept {
  static_cast<void>(::write(STDERR_FILENO, text.data(), text.size()));
}

[[nodiscard]] auto labels_are_untouched() noexcept -> bool {
  if (graph == nullptr) {
    return true;
  }
  for (parhip::NodeID node = 0; node < graph->number_of_local_nodes(); ++node) {
    if (graph->getNodeLabel(node) != sentinel) {
      return false;
    }
  }
  return true;
}

[[noreturn]] void observed_abort(MPI_Comm communicator,
                                 int error_code) noexcept {
  auto relation = int{MPI_UNEQUAL};
  if (!active || error_code != EXIT_FAILURE ||
      PMPI_Comm_compare(communicator, affected_communicator, &relation) !=
          MPI_SUCCESS ||
      relation != MPI_IDENT || !labels_are_untouched()) {
    write_text("observed distributed-partitioner MPI_Abort with unexpected "
               "state\n");
    std::_Exit(91);
  }
  write_text(
      "observed distributed-partitioner MPI_Abort on affected communicator "
      "before graph mutation\n");
  if (PMPI_Barrier(MPI_COMM_WORLD) != MPI_SUCCESS) {
    std::_Exit(90);
  }
  std::_Exit(86);
}
}  // namespace distributed_partitioner_failure_probe

static_assert(
    noexcept(distributed_partitioner_failure_probe::write_text({})));
static_assert(
    noexcept(distributed_partitioner_failure_probe::labels_are_untouched()));
static_assert(noexcept(distributed_partitioner_failure_probe::observed_abort(
    MPI_COMM_NULL, 0)));

extern "C" int MPI_Comm_rank(MPI_Comm communicator, int* rank) {
  using namespace distributed_partitioner_failure_probe;
  if (active && selected == mode::rank_backend &&
      communicator == affected_communicator) {
    return MPI_ERR_OTHER;
  }
  return PMPI_Comm_rank(communicator, rank);
}

extern "C" int MPI_Abort(MPI_Comm communicator, int error_code) {
  distributed_partitioner_failure_probe::observed_abort(communicator,
                                                        error_code);
}
// KAHIP_PMPI_CALLBACK_REGION_END

namespace {
using distributed_partitioner_failure_probe::mode;

[[nodiscard]] auto parse_mode(std::string_view value) -> mode {
  if (value == "zero-k") {
    return mode::zero_k;
  }
  if (value == "mismatched-k") {
    return mode::mismatched_k;
  }
  if (value == "zero-cluster-factor") {
    return mode::zero_cluster_factor;
  }
  if (value == "infinite-cluster-factor") {
    return mode::infinite_cluster_factor;
  }
  if (value == "negative-choice-count") {
    return mode::negative_choice_count;
  }
  if (value == "choice-capacity") {
    return mode::choice_capacity;
  }
  if (value == "exhausted-choice-cursor") {
    return mode::exhausted_choice_cursor;
  }
  if (value == "rank-backend") {
    return mode::rank_backend;
  }
  if (value == "mismatched-communicator") {
    return mode::mismatched_communicator;
  }
  std::fprintf(stderr, "unknown failure mode: %.*s\n",
               static_cast<int>(value.size()), value.data());
  std::exit(64);
}

void build_single_isolate(parhip::parallel_graph_access& graph,
                          MPI_Comm communicator) {
  auto rank = 0;
  auto size = 0;
  if (PMPI_Comm_rank(communicator, &rank) != MPI_SUCCESS ||
      PMPI_Comm_size(communicator, &size) != MPI_SUCCESS) {
    std::exit(70);
  }
  auto ranges = std::vector<parhip::NodeID>(
      static_cast<std::size_t>(size) + 1, parhip::NodeID{1});
  ranges.front() = 0;
  auto const first = ranges[static_cast<std::size_t>(rank)];
  auto const end = ranges[static_cast<std::size_t>(rank + 1)];
  graph.start_construction(end - first, 0, 1, 0, false);
  graph.set_range(first, first == end ? first : end - 1);
  graph.set_range_array(ranges);
  if (first != end) {
    auto const local = graph.new_node();
    graph.setNodeWeight(local, 7);
    graph.setNodeLabel(local,
                       distributed_partitioner_failure_probe::sentinel);
    graph.setSecondPartitionIndex(local, 0);
  }
  graph.finish_construction();
}
}  // namespace

int main(int argc, char** argv) {
  using namespace distributed_partitioner_failure_probe;
  if (argc != 2) {
    std::fputs("usage: distributed_partitioner_failure_probe MODE\n", stderr);
    return 64;
  }
  selected = parse_mode(argv[1]);
  if (MPI_Init(&argc, &argv) != MPI_SUCCESS) {
    return 70;
  }
  MPI_Comm_set_errhandler(MPI_COMM_WORLD, MPI_ERRORS_RETURN);

  auto world_rank = 0;
  auto world_size = 0;
  PMPI_Comm_rank(MPI_COMM_WORLD, &world_rank);
  PMPI_Comm_size(MPI_COMM_WORLD, &world_size);
  if (PMPI_Comm_split(MPI_COMM_WORLD, 0, world_size - world_rank,
                      &affected_communicator) != MPI_SUCCESS) {
    return 70;
  }
  MPI_Comm_set_errhandler(affected_communicator, MPI_ERRORS_RETURN);

  auto const graph_communicator =
      selected == mode::mismatched_communicator ? MPI_COMM_WORLD
                                                : affected_communicator;
  {
    auto owned_graph = parhip::parallel_graph_access{graph_communicator};
    build_single_isolate(owned_graph, graph_communicator);
    graph = &owned_graph;

    auto config = parhip::PPartitionConfig{};
    config.k = 2;
    config.num_tries = 1;
    config.num_vcycles = 0;
    config.cluster_coarsening_factor = 14;
    config.upper_bound_partition = 7;
    if (selected == mode::zero_k) {
      config.k = 0;
    } else if (selected == mode::mismatched_k) {
      config.k = world_rank == 0 ? 0 : 2;
    } else if (selected == mode::zero_cluster_factor) {
      config.cluster_coarsening_factor = 0;
    } else if (selected == mode::infinite_cluster_factor) {
      config.cluster_coarsening_factor =
          std::numeric_limits<double>::infinity();
    } else if (selected == mode::negative_choice_count) {
      config.num_tries = -1;
      config.num_vcycles = 2;
    } else if (selected == mode::choice_capacity) {
      config.num_tries = std::numeric_limits<int>::max();
      config.num_vcycles = std::numeric_limits<int>::max();
    } else if (selected == mode::exhausted_choice_cursor) {
      config.num_tries = 0;
      config.num_vcycles = 1;
    }

    active = true;
    if (selected == mode::zero_k || selected == mode::mismatched_k ||
        selected == mode::mismatched_communicator) {
      auto random = parhip::random_initial_partitioning{};
      random.perform_partitioning(
          parhip::mpi::communicator_view{affected_communicator}, config,
          owned_graph);
    } else if (selected == mode::negative_choice_count ||
               selected == mode::choice_capacity) {
      parhip::distributed_partitioner::generate_random_choices(
          config, parhip::mpi::communicator_view{affected_communicator});
    } else if (selected == mode::exhausted_choice_cursor) {
      parhip::distributed_partitioner::generate_random_choices(
          config, parhip::mpi::communicator_view{affected_communicator});
      config.eco = true;
      auto partitioner = parhip::distributed_partitioner{};
      partitioner.perform_partitioning(affected_communicator, config,
                                       owned_graph);
    } else {
      auto partitioner = parhip::distributed_partitioner{};
      partitioner.perform_partitioning(affected_communicator, config,
                                       owned_graph);
    }
    active = false;
    graph = nullptr;
  }

  PMPI_Comm_free(&affected_communicator);
  MPI_Finalize();
  std::fputs("failure mode returned without fail-fast termination\n", stderr);
  return 72;
}
