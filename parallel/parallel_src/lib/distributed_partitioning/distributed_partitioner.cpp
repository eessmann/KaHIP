/******************************************************************************
 * distributed_partitioner.cpp
 * *
 * Source of KaHIP -- Karlsruhe High Quality Partitioning.
 * Christian Schulz <christian.schulz.phone@gmail.com>
 *****************************************************************************/

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "communication/ghost_exchange_plan.h"
#include "communication/mpi_failure.h"
#include "communication/mpi_neighbors.h"
#include "communication/mpi_trace.h"
#include "communication/mpi_tools.h"
#include "distributed_partitioning/distributed_consistency.h"
#include "distributed_partitioner.h"
#include "initial_partitioning/initial_partitioning.h"
#include "io/parallel_graph_io.h"
#include "parallel_contraction_projection/parallel_contraction.h"
#include "parallel_contraction_projection/parallel_block_down_propagation.h"
#include "parallel_contraction_projection/parallel_projection.h"
#include "parallel_label_compress/parallel_label_compress.h"
#include "stop_rule.h"
#include "tools/distributed_quality_metrics.h"
#include "tools/random_functions.h"
#include "data_structure/linear_probing_hashmap.h"
namespace parhip {
namespace {
template <typename ProjectValue, typename ReadGhostValue>
void validate_distributed_node_values(
    MPI_Comm communicator,
    parallel_graph_access& graph,
    ProjectValue project_value,
    ReadGhostValue read_ghost_value,
    std::string_view failure_context) {
  auto const graph_communicator =
      mpi::communicator_view{graph.getCommunicator()};
  auto communicator_is_compatible = communicator != MPI_COMM_NULL;
  if (communicator_is_compatible) {
    auto comparison = int{MPI_UNEQUAL};
    mpi::check_or_abort(
        MPI_Comm_compare(communicator, graph.getCommunicator(), &comparison),
        graph.getCommunicator(),
        "MPI_Comm_compare(distributed consistency)");
    communicator_is_compatible =
        comparison == MPI_IDENT || comparison == MPI_CONGRUENT;
  }
  if (!mpi::detail::collective_predicate(communicator_is_compatible,
                                         graph_communicator)) {
    throw mpi::mpi_error{
        MPI_ERR_COMM,
        std::string{failure_context} + " communicator validation failed"};
  }

  auto const& plan = graph.ghost_plan();
  auto semantic_failure = false;
  try {
    auto outgoing =
        std::vector<std::vector<distributed_consistency::node_value>>(
            plan.topology().destinations().size());
    for (std::size_t destination_index = 0;
         destination_index < plan.topology().destinations().size();
         ++destination_index) {
      auto const local_nodes =
          plan.outgoing_local_nodes(destination_index);
      auto& records = outgoing[destination_index];
      records.reserve(local_nodes.size());
      std::ranges::transform(
          local_nodes,
          std::back_inserter(records),
          [&](NodeID const local_node) {
            return distributed_consistency::node_value{
                graph.getGlobalID(local_node),
                project_value(graph, local_node)};
          });
    }

    auto received = mpi::neighbor_all_to_all_v(
        mpi::segmented_buffer<distributed_consistency::node_value>::
            from_segments(outgoing),
        plan.topology());

    auto resolved_local_ids =
        std::vector<std::vector<NodeID>>(plan.topology().sources().size());
    auto local_structure_is_valid =
        received.segment_count() == plan.topology().sources().size();
    for (std::size_t source_index = 0;
         source_index < plan.topology().sources().size(); ++source_index) {
      auto const source = plan.topology().sources()[source_index];
      auto const records = received.segment(source_index);
      auto const expected = plan.expected_ghost_nodes(source_index);
      auto& local_ids = resolved_local_ids[source_index];
      local_ids.reserve(records.size());
      auto received_ids = std::vector<NodeID>{};
      received_ids.reserve(records.size());
      for (auto const& record : records) {
        received_ids.push_back(record.global_id);
        auto const local_id =
            graph.find_ghost_local_id(record.global_id, source);
        local_structure_is_valid =
            local_structure_is_valid && local_id.has_value();
        local_ids.push_back(local_id.value_or(NodeID{0}));
      }
      std::ranges::sort(received_ids);
      local_structure_is_valid =
          local_structure_is_valid && records.size() == expected.size() &&
          std::ranges::adjacent_find(received_ids) == received_ids.end() &&
          std::ranges::equal(received_ids, expected);
    }

    auto const structure_is_valid = mpi::detail::collective_predicate(
        local_structure_is_valid, plan.topology().view());
    if (!structure_is_valid) {
      semantic_failure = true;
    } else {
      auto local_values_are_valid = true;
      for (std::size_t source_index = 0;
           source_index < plan.topology().sources().size(); ++source_index) {
        auto const records = received.segment(source_index);
        auto const& local_ids = resolved_local_ids[source_index];
        for (std::size_t record_index = 0;
             record_index < records.size(); ++record_index) {
          local_values_are_valid =
              local_values_are_valid &&
              read_ghost_value(graph, local_ids[record_index]) ==
                  records[record_index].value;
        }
      }
      semantic_failure = !mpi::detail::collective_predicate(
          local_values_are_valid, plan.topology().view());
    }
  } catch (...) {
    mpi::abort_on_exception(plan.topology().native_handle(), failure_context);
  }

  if (semantic_failure) {
    throw mpi::mpi_error{MPI_ERR_ARG, std::string{failure_context}};
  }
}
}  // namespace

std::vector< NodeID > distributed_partitioner::m_cf = std::vector< NodeID >();
std::vector< NodeID > distributed_partitioner::m_sf = std::vector< NodeID >();
std::vector< NodeID > distributed_partitioner::m_lic = std::vector< NodeID >();

distributed_partitioner::distributed_partitioner() {
  m_total_graph_weight = std::numeric_limits< NodeWeight >::max();
  m_cur_rnd_choice = 0;
  m_level = -1;
  m_cycle = 0;
}

distributed_partitioner::~distributed_partitioner() {
}

void distributed_partitioner::generate_random_choices( PPartitionConfig & config ){
  for( int i = 0; i < config.num_tries; i++) {
    for( int j = 0; j < config.num_vcycles; j++) {
      m_cf.push_back(random_functions::nextDouble( 10, 25 ));
      m_sf.push_back(random_functions::nextInt( 20, 500 ));
      m_lic.push_back(random_functions::nextInt( 2, 15 ));
    }
  }
}

void distributed_partitioner::perform_recursive_partitioning( PPartitionConfig & partition_config, parallel_graph_access & G) {
  perform_partitioning( MPI_COMM_WORLD, partition_config, G);
}

void distributed_partitioner::perform_recursive_partitioning( MPI_Comm communicator, PPartitionConfig & partition_config, parallel_graph_access & G) {
}


void distributed_partitioner::perform_partitioning( PPartitionConfig & partition_config, parallel_graph_access & G) {
  perform_partitioning( MPI_COMM_WORLD, partition_config, G);
}

void distributed_partitioner::perform_partitioning( MPI_Comm communicator, PPartitionConfig & partition_config, parallel_graph_access & G) {
  timer t;
  double elapsed = 0;
  m_cur_rnd_choice = 0;
  PPartitionConfig config = partition_config;
  config.vcycle = false;

  PEID rank;
  MPI_Comm_rank( communicator, &rank);

  for( int cycle = 0; cycle < partition_config.num_vcycles; cycle++) {
    t.restart();
    m_cycle = cycle;

    if(cycle+1 == partition_config.num_vcycles && partition_config.no_refinement_in_last_iteration) {
      config.label_iterations_refinement = 0;
    }

    vcycle( communicator, config, G );

    if( rank == ROOT ) {
      PRINT(std::cout <<  "log>cycle: " << m_cycle << " uncoarsening took " << m_t.elapsed()  << std::endl;)
}
#ifndef NDEBUG
    check_labels(communicator, config, G);
#endif

    elapsed += t.elapsed();

#ifndef NOOUTPUT
    distributed_quality_metrics qm;
    EdgeWeight edge_cut = qm.edge_cut( G, communicator );
    double balance      = qm.balance( config, G, communicator );

    if( rank == ROOT ) {
      std::cout <<  "log>cycle: " << cycle << " k " <<  config.k << " cut " << edge_cut << " balance " << balance << " time " <<  elapsed  << std::endl;
    }
#endif
    t.restart();
    m_t.restart();
    if( cycle+1 < config.num_vcycles ) {
      forall_local_nodes(G, node) {
        G.setSecondPartitionIndex(node, G.getNodeLabel(node));
        G.setNodeLabel(node, G.getGlobalID(node));
      } endfor

      forall_ghost_nodes(G, node) {
        G.setSecondPartitionIndex(node, G.getNodeLabel(node));
        G.setNodeLabel(node, G.getGlobalID(node));
      } endfor
}

    config.vcycle = true;

    if( rank == ROOT && config.eco ) {
      config.cluster_coarsening_factor = m_cf[m_cur_rnd_choice++];
    }

    if(config.eco) {
      MPI_Bcast(&(config.cluster_coarsening_factor), 1, MPI_DOUBLE, ROOT, communicator);

      //std::cout << "cf " << config.cluster_coarsening_factor  << std::endl;
    }
    config.evolutionary_time_limit = 0;
    elapsed += t.elapsed();
    MPI_Barrier(communicator);

  }
}

void distributed_partitioner::vcycle( MPI_Comm communicator, PPartitionConfig & partition_config, parallel_graph_access & G) {
  PPartitionConfig config = partition_config;

  mpi_tools mpitools;
  timer t;

  if( m_total_graph_weight == std::numeric_limits< NodeWeight >::max() ) {
    m_total_graph_weight = G.number_of_global_nodes();
  }

  PEID rank;
  MPI_Comm_rank( communicator, &rank);

#ifndef NOOUTPUT
  if( rank == ROOT ) {
    std::cout << "log>" << "=====================================" << std::endl;
    std::cout << "log>" << "=============NEXT LEVEL==============" << std::endl;
    std::cout << "log>" << "=====================================" << std::endl;
  }
#endif
  t.restart();


  m_level++;
  KAHIP_MPI_TRACE_SET_HIERARCHY(
      m_cycle, m_level, mpi::trace::epoch::coarsening);
  config.label_iterations = config.label_iterations_coarsening;
  config.total_num_labels = G.number_of_global_nodes();
  //
  if( config.cluster_coarsening_factor > 100 ) {
    config.upper_bound_cluster = std::max(100, (int)(config.upper_bound_partition/(1.0*config.cluster_coarsening_factor)));
  } else {
    config.upper_bound_cluster = (int)(config.upper_bound_partition/(1.0*config.cluster_coarsening_factor));
  }
  G.init_balance_management( config );

  //parallel_label_compress< std::unordered_map< NodeID, NodeWeight> > plc;
  parallel_label_compress< linear_probing_hashmap  > plc;
  plc.perform_parallel_label_compression ( config, G, true);

#ifndef NOOUTPUT
  if( rank == ROOT ) {
    std::cout <<  "log>cycle: " << m_cycle << " level: " << m_level  << " parallel label compression took " <<  t.elapsed() << std::endl;
  }
#endif

  parallel_graph_access Q(communicator);
  t.restart();

  {
    KAHIP_MPI_TRACE_SET_HIERARCHY(
        m_cycle, m_level, mpi::trace::epoch::contraction);
    parallel_contraction parallel_contract;
    parallel_contract.contract_to_distributed_quotient( communicator, config, G, Q); // contains one Barrier

    parallel_block_down_propagation pbdp;
    if( config.vcycle ) {
      // in this case we have to propagate the partitionindex down
      pbdp.propagate_block_down( communicator, config, G, Q);
    }

    MPI_Barrier(communicator);
  }


#ifndef NOOUTPUT
  if( rank == ROOT ) {
    std::cout <<  "log>cycle: " << m_cycle << " level: " << m_level << " contraction took " <<  t.elapsed() << std::endl;
    std::cout <<  "log>cycle: " << m_cycle << " level: " << m_level << " coarse nodes n=" << Q.number_of_global_nodes() << ", coarse edges m=" << Q.number_of_global_edges() << std::endl;
  }
#endif

  if( !contraction_stop_decision.contraction_stop(config, G, Q)) {
    vcycle( communicator, config, Q);
  } else {
#ifndef NOOUTPUT
    if( rank == ROOT ) {
      std::cout << "log>" << "=====================================" << std::endl;
      std::cout << "log>" << "================ IP =================" << std::endl;
      std::cout << "log>" << "=====================================" << std::endl;
      std::cout <<  "log>cycle: " << m_cycle << " total number of levels " <<  (m_level+1) << std::endl;
      std::cout <<  "log>cycle: " << m_cycle << " number of coarsest nodes " <<  Q.number_of_global_nodes() << std::endl;
      std::cout <<  "log>cycle: " << m_cycle << " number of coarsest edges " <<  Q.number_of_global_edges() << std::endl;
      std::cout <<  "log>cycle: " << m_cycle << " coarsening took  " <<  m_t.elapsed()  << std::endl;
    }
#endif
    t.restart();

    KAHIP_MPI_TRACE_SET_HIERARCHY(
        m_cycle, m_level, mpi::trace::epoch::initial_partition);
    initial_partitioning_algorithm ip;
    ip.perform_partitioning( communicator, config, Q );

#ifndef NOOUTPUT
    if( rank == ROOT ) {
      std::cout <<  "log>cycle: " << m_cycle << " initial partitioning took " <<  t.elapsed() << std::endl;
    }
    m_t.restart();
#endif
  }

#ifndef NOOUTPUT
  if( rank == ROOT ) {
    std::cout << "log>" << "=====================================" << std::endl;
    std::cout << "log>" << "============PREV LEVEL ==============" << std::endl;
    std::cout << "log>" << "=====================================" << std::endl;
  }
#endif

  t.restart();
  KAHIP_MPI_TRACE_SET_HIERARCHY(
      m_cycle, m_level, mpi::trace::epoch::projection);
  parallel_projection parallel_project;
  parallel_project.parallel_project( communicator, G, Q ); // contains a Barrier

#ifndef NOOUTPUT
  if( rank == ROOT ) {
    std::cout <<  "log>cycle: " << m_cycle << " level: " << m_level << " projection took " <<  t.elapsed() << std::endl;
  }
#endif

  t.restart();
  config.label_iterations = config.label_iterations_refinement;

  if( config.label_iterations != 0 ) {
    KAHIP_MPI_TRACE_SET_HIERARCHY(
        m_cycle, m_level, mpi::trace::epoch::refinement);
    config.total_num_labels = config.k;
    config.upper_bound_cluster = config.upper_bound_partition;


    G.init_balance_management( config );
    PPartitionConfig working_config = config;
    working_config.vcycle = false; // assure that we actually can improve the cut

    parallel_label_compress< std::vector< NodeWeight> > plc_refinement;
    plc_refinement.perform_parallel_label_compression( working_config, G, false, false);
  }

#ifndef NOOUTPUT
  if( rank == ROOT ) {
    std::cout <<  "log>cycle: " << m_cycle <<" level: " << m_level << " label compression refinement took " <<  t.elapsed() << std::endl;
  }
#endif
  m_level--;
}

void distributed_partitioner::check_labels( MPI_Comm communicator, PPartitionConfig & config, parallel_graph_access & G) {
  static_cast<void>(config);
  validate_distributed_node_values(
      communicator,
      G,
      [](parallel_graph_access& graph, NodeID const local_node) {
        return graph.getNodeLabel(local_node);
      },
      [](parallel_graph_access& graph, NodeID const ghost_node) {
        return graph.getNodeLabel(ghost_node);
      },
      "label consistency validation failed");
}


void distributed_partitioner::check( MPI_Comm communicator, PPartitionConfig & config, parallel_graph_access & G) {
  static_cast<void>(config);
  validate_distributed_node_values(
      communicator,
      G,
      [](parallel_graph_access& graph, NodeID const local_node) {
        return graph.getSecondPartitionIndex(local_node);
      },
      [](parallel_graph_access& graph, NodeID const ghost_node) {
        return graph.getSecondPartitionIndex(ghost_node);
      },
      "second-partition consistency validation failed");
}
}
