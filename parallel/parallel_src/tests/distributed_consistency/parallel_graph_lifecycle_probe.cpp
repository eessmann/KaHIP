#include <mpi.h>

#include <cstdlib>
#include <memory>
#include <ranges>
#include <string_view>
#include <vector>

#include "data_structure/parallel_graph_access.h"

namespace {
void build_local_graph(parhip::parallel_graph_access& graph,
                       int rank,
                       int size) {
  graph.start_construction(1, 0, static_cast<parhip::NodeID>(size), 0, false);
  graph.set_range(static_cast<parhip::NodeID>(rank),
                  static_cast<parhip::NodeID>(rank));
  auto ranges = std::vector<parhip::NodeID>(static_cast<std::size_t>(size) + 1);
  std::ranges::iota(ranges, parhip::NodeID{0});
  graph.set_range_array(ranges);
  auto const node = graph.new_node();
  graph.setNodeWeight(node, 1);
  graph.setNodeLabel(node, static_cast<parhip::NodeID>(rank));
  graph.finish_construction();
}
}  // namespace

int main(int argc, char* argv[]) {
  if (argc != 2) {
    return 64;
  }
  auto const mode = std::string_view{argv[1]};
  auto const owns_plan = mode == "cached-plan";
  auto const active_destructor = mode == "active-destructor";
  auto const active_reset = mode == "active-reset";
  if (!owns_plan && !active_destructor && !active_reset && mode != "no-plan") {
    return 64;
  }

  if (MPI_Init(&argc, &argv) != MPI_SUCCESS) {
    return 70;
  }
  auto rank = 0;
  auto size = 0;
  if (MPI_Comm_rank(MPI_COMM_WORLD, &rank) != MPI_SUCCESS ||
      MPI_Comm_size(MPI_COMM_WORLD, &size) != MPI_SUCCESS) {
    return 70;
  }

  auto graph = std::make_unique<parhip::parallel_graph_access>(MPI_COMM_WORLD);
  build_local_graph(*graph, rank, size);
  if (active_destructor || active_reset) {
    graph->setNodeLabel(0, static_cast<parhip::NodeID>(rank + 1));
    graph->update_ghost_node_data(false);
    if (active_reset) {
      graph->reinit();
      return 2;
    }
    graph.reset();
    return 2;
  }
  if (owns_plan) {
    static_cast<void>(graph->ghost_plan());
  }

  if (MPI_Finalize() != MPI_SUCCESS) {
    return 70;
  }
  graph.reset();
  return owns_plan ? 2 : 0;
}
