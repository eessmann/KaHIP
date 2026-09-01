#pragma once

#include <cmath>
#include <cstdlib>

#include "../lib/data_structure/graph_access.h"
#include "../lib/partition/partition_config.h"
#include "../lib/tools/random_functions.h"

namespace kahip::modified {
inline void internal_build_graph(PartitionConfig& partition_config,
                                 int* n,
                                 int* vwgt,
                                 int* xadj,
                                 int* adjcwgt,
                                 int* adjncy,
                                 graph_access& graph) {
  graph.build_from_metis(*n, xadj, adjncy);
  graph.set_partition_count(partition_config.k);

  std::srand(partition_config.seed);
  random_functions::setSeed(partition_config.seed);

  if (vwgt != nullptr) {
    forall_nodes(graph, node) {
      graph.setNodeWeight(node, vwgt[node]);
    }
    endfor
  }

  if (adjcwgt != nullptr) {
    forall_edges(graph, edge) {
      graph.setEdgeWeight(edge, adjcwgt[edge]);
    }
    endfor
  }

  partition_config.largest_graph_weight = 0;
  forall_nodes(graph, node) {
    partition_config.largest_graph_weight += graph.getNodeWeight(node);
  }
  endfor

      auto const epsilon = partition_config.imbalance / 100;
  partition_config.upper_bound_partition =
      std::ceil((1 + epsilon) * partition_config.largest_graph_weight /
                static_cast<double>(partition_config.k));
  partition_config.graph_allready_partitioned = false;
}
}  // namespace kahip::modified
