/******************************************************************************
 * distributed_evolutionary_partitioning.cpp
 * *
 * Source of KaHIP -- Karlsruhe High Quality Partitioning.
 * Christian Schulz <christian.schulz.phone@gmail.com>
 *****************************************************************************/

#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

#include "communication/mpi_fixed_broadcast.h"
#include "communication/mpi_tools.h"
#include "distributed_evolutionary_partitioning.h"
#include "kaHIP_interface.h"
#include "kaHIP_evolutionary_interface_internal.h"
#include "parallel_contraction_projection/parallel_projection.h"
#include "io/parallel_graph_io.h"
#include "serial_kernel_bridge.h"
#include "tools/distributed_quality_metrics.h"
namespace parhip {
namespace {
struct checked_serial_input final {
  int node_count{};
  std::vector<int> xadj;
  std::vector<int> adjncy;
  std::vector<int> node_weights;
  std::vector<int> edge_weights;
  std::vector<int> partition;

  [[nodiscard]] static auto from_graph(complete_graph_access& graph,
                                       bool preserve_vcycle_labels)
      -> checked_serial_input {
    if (!std::in_range<int>(graph.number_of_local_nodes()) ||
        !std::in_range<int>(graph.number_of_local_edges())) {
      throw std::overflow_error{"serial graph counts exceed int"};
    }
    auto const nodes = static_cast<std::size_t>(graph.number_of_local_nodes());
    auto const edges = static_cast<std::size_t>(graph.number_of_local_edges());
    auto input = checked_serial_input{
        .node_count = static_cast<int>(nodes),
        .xadj = std::vector<int>(nodes + 1),
        .adjncy = std::vector<int>(edges),
        .node_weights = std::vector<int>(nodes),
        .edge_weights = std::vector<int>(edges),
        .partition = std::vector<int>(nodes),
    };
    for (std::size_t node = 0; node < nodes; ++node) {
      auto const node_id = static_cast<NodeID>(node);
      auto const first_edge = graph.get_first_edge(node_id);
      auto const weight = graph.getNodeWeight(node_id);
      if (!std::in_range<int>(first_edge) || !std::in_range<int>(weight) ||
          (preserve_vcycle_labels &&
           !std::in_range<int>(graph.getSecondPartitionIndex(node_id)))) {
        throw std::overflow_error{"serial graph node field exceeds int"};
      }
      input.xadj[node] = static_cast<int>(first_edge);
      input.node_weights[node] = static_cast<int>(weight);
      if (preserve_vcycle_labels) {
        input.partition[node] =
            static_cast<int>(graph.getSecondPartitionIndex(node_id));
      }
    }
    if (!std::in_range<int>(graph.get_first_edge(static_cast<NodeID>(nodes)))) {
      throw std::overflow_error{"serial CSR sentinel exceeds int"};
    }
    input.xadj[nodes] =
        static_cast<int>(graph.get_first_edge(static_cast<NodeID>(nodes)));
    for (std::size_t edge = 0; edge < edges; ++edge) {
      auto const edge_id = static_cast<EdgeID>(edge);
      auto const target = graph.getEdgeTarget(edge_id);
      auto const weight = graph.getEdgeWeight(edge_id);
      if (!std::in_range<int>(target) || !std::in_range<int>(weight)) {
        throw std::overflow_error{"serial graph edge field exceeds int"};
      }
      input.adjncy[edge] = static_cast<int>(target);
      input.edge_weights[edge] = static_cast<int>(weight);
    }
    return input;
  }
};
}  // namespace

distributed_evolutionary_partitioning::distributed_evolutionary_partitioning() {
                
}

distributed_evolutionary_partitioning::~distributed_evolutionary_partitioning() {
                
}

void distributed_evolutionary_partitioning::perform_partitioning( MPI_Comm communicator, PPartitionConfig & config, 
                                                                  parallel_graph_access & Q) {

  mpi_tools mpitools;
  parallel_graph_access Q_bar;
  distributed_quality_metrics dqm;
  static_cast<void>(mpitools.preflight_serial_kernel(communicator, config, Q));
  mpitools.collect_parallel_graph_to_checked_serial_graph(communicator, config,
                                                          Q, Q_bar);
  mpitools.distribute_local_graph( communicator, config, Q_bar);

  auto serial_input = std::optional<checked_serial_input>{};
  try {
    serial_input.emplace(checked_serial_input::from_graph(Q_bar,
                                                          config.vcycle));
  } catch (...) {
    mpi::abort_on_exception(communicator, "serial input construction failure");
  }
  int n       = serial_input->node_count;
  int nparts  = config.k;    // k-way partitioning.

  auto* xadj = serial_input->xadj.data();
  auto* adjncy = serial_input->adjncy.data();
  auto* vwgt = serial_input->node_weights.data();
  auto* adjwgt = serial_input->edge_weights.data();
  auto* partition_map = serial_input->partition.data();

  [[maybe_unused]] int trivial_edgecut = 0;
  [[maybe_unused]] double trivial_balance = 0.0;
  if (kahip::serial_kernel::solve_trivial_single_block(
          nparts, std::span<int>{partition_map, static_cast<std::size_t>(n)},
          trivial_edgecut, trivial_balance)) {
    forall_local_nodes(Q_bar, node) {
      Q_bar.setNodeLabel(node, 0);
    } endfor
    parallel_projection parallel_project_init;
    parallel_project_init.initial_assignment(Q, Q_bar);
    return;
  }

  auto const mpi_communicator = mpi::communicator_view{communicator};
  PEID const rank = mpi_communicator.rank();

  EdgeWeight prev_cut              = 0;
  NodeWeight prev_max_block_weight = 0;

  if( config.vcycle && rank == 0) {
    forall_local_nodes(Q_bar, node) {
      partition_map[node] = Q_bar.getSecondPartitionIndex(node);
    } endfor

    prev_cut = dqm.local_edge_cut(Q_bar, partition_map, communicator);
    prev_max_block_weight = dqm.local_max_block_weight(config, Q_bar, partition_map, communicator);
    //std::cout <<  "prev cut "  <<  prev_cut << std::endl;
    //std::cout <<  "prev max block "  <<  prev_max_block_weight << std::endl;
  }

  if( config.vcycle ) {
    mpi::broadcast_vcycle_state(
        std::span<int>{partition_map, static_cast<std::size_t>(n)}, prev_cut,
        prev_max_block_weight, ROOT, mpi_communicator);
  }

  int edgecut            = 0;
  double balance         = 0;
  bool graph_partitioned = config.vcycle;

  int mode = 0;

  switch( config.initial_partitioning_algorithm ) {
    case InitialPartitioningAlgorithm::KAFFPAESTRONG:
      mode = STRONG;
    break;
    case InitialPartitioningAlgorithm::KAFFPAEECO:
      mode = ECO;
    break;
    case InitialPartitioningAlgorithm::KAFFPAEFAST:
      mode = FAST;
    break;
    case InitialPartitioningAlgorithm::KAFFPAEULTRAFASTSNW:
      mode = ULTRAFASTSOCIAL;
    break;
    case InitialPartitioningAlgorithm::KAFFPAEFASTSNW:
      mode = FASTSOCIAL;
    break;
    case InitialPartitioningAlgorithm::KAFFPAEECOSNW:
      mode = ECOSOCIAL;
    break;
    case InitialPartitioningAlgorithm::KAFFPAESTRONGSNW:
      mode = STRONGSOCIAL;
    break;
    default:
      mode = FASTSOCIAL;
    break;
  }

  if(config.vcycle) {
    forall_local_nodes(Q_bar, node) {
      Q_bar.setNodeLabel(node, partition_map[node]);
    } endfor
}

  timer t;

#ifdef NOOUTPUT
  std::streambuf* backup = std::cout.rdbuf();
  std::ofstream ofs;
  ofs.open("/dev/null");
  std::cout.rdbuf(ofs.rdbuf());
#endif

#ifdef DETERMINISTIC_PARHIP
  auto const evolutionary_time_limit = 0;
#else
  auto const evolutionary_time_limit = config.evolutionary_time_limit;
#endif
  kahip::modified::kaffpaE_with_upper_bound(&n,
          vwgt,
          xadj,
          adjwgt,
          adjncy,
          &nparts,
          false,  // supress output
          graph_partitioned,
          evolutionary_time_limit,
          config.seed,
          mode,
          communicator,
          config.inbalance,
          config.upper_bound_partition,
          &edgecut,
          &balance,
          partition_map);



#ifdef NOOUTPUT
  ofs.close();
  std::cout.rdbuf(backup);
#endif

  if( rank == (int)ROOT) {
    PRINT(std::cout <<  "partitioner call took " <<  t.elapsed() << std::endl;);
  }

#ifndef NOOUTPUT
  if( rank == (int)ROOT) {
    std::cout <<  "log>cut computed by IP algorithm " <<  edgecut  << std::endl;
    std::cout <<  "log>balance computed by IP algorithm "<<  balance << std::endl;
  }
#endif

  if( !config.vcycle ) {
    forall_local_nodes(Q_bar, node) {
      Q_bar.setNodeLabel(node, partition_map[node]);
    } endfor
} else {
  NodeWeight cur_max_block_weight = dqm.local_max_block_weight(config, Q_bar, partition_map, communicator);

  //balance and cut improved
  bool accept = (cur_max_block_weight <= prev_max_block_weight || balance <= 1.03) && (EdgeWeight)edgecut <= prev_cut;
  // or we previously have not been feasible and now are feasible
  accept = accept || (prev_max_block_weight >= config.upper_bound_partition && cur_max_block_weight <= config.upper_bound_partition);

  if( accept ) {
    if( rank == (int)ROOT) {
      PRINT(std::cout <<  "log>update criterion reached, updating partition"  << std::endl;)

}
    forall_local_nodes(Q_bar, node) {
      Q_bar.setNodeLabel(node, partition_map[node]);
    } endfor
} else {
  if( rank == (int)ROOT) {
    PRINT(std::cout <<  "update criterion not reached, not updating partition"  << std::endl;)
}
}
}

  parallel_projection parallel_project_init;
  parallel_project_init.initial_assignment( Q, Q_bar );

#ifndef NOOUTPUT
  edgecut = dqm.edge_cut(Q, communicator);
  balance = dqm.balance(config, Q, communicator);
  if( rank == (int)ROOT) {
    std::cout <<  "log>cur edge cut " <<  edgecut  << std::endl;
    std::cout <<  "log>cur balance  " <<  balance << std::endl;
  }
#endif

}
}
