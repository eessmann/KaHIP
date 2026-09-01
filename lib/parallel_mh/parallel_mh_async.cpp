/******************************************************************************
 * parallel_mh_async.cpp
 * *
 * Source of KaHIP -- Karlsruhe High Quality Partitioning.
 * Christian Schulz <christian.schulz.phone@gmail.com>
 *****************************************************************************/

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <mpi.h>
#include <sstream>
#include <utility>

#include "diversifyer.h"
#include "exchange/exchanger.h"
#include "galinier_combine/construct_partition.h"
#include "graph_io.h"
#include "graph_partitioner.h"
#include "parallel_mh_async.h"
#include "parallel_mh/evolutionary_collectives.h"
#include "parallel_mh/population_size_broadcast.h"
#include "quality_metrics.h"
#include "random_functions.h"
parallel_mh_async::parallel_mh_async()
    : parallel_mh_async(MPI_COMM_WORLD) {}

parallel_mh_async::parallel_mh_async(MPI_Comm communicator)
    : m_communicator(
          std::make_unique<
              ::kahip::parallel_mh::owned_evolutionary_communicator>(
              communicator)),
      m_rank(m_communicator->rank()),
      m_size(m_communicator->size()) {}

parallel_mh_async::~parallel_mh_async() = default;

void parallel_mh_async::perform_partitioning(const PartitionConfig & partition_config, graph_access & G) {
  m_time_limit = partition_config.time_limit;
  m_rounds = 0;
  m_island = std::make_unique<population>(m_communicator->native_handle(),
                                          partition_config);

  auto const derived_seed =
      static_cast<std::int64_t>(partition_config.seed) * m_size + m_rank;
  if (!std::in_range<int>(derived_seed)) {
    ::kahip::parallel_mh::detail::abort_evolutionary_collective(
        m_communicator->native_handle(), "evolutionary random seed derivation",
        "rank-derived random seed exceeds the int domain");
  }
  auto const local_seed = static_cast<int>(derived_seed);
  std::srand(local_seed);
  random_functions::setSeed(local_seed);

  PartitionConfig ini_working_config  = partition_config;
  initialize( ini_working_config, G);

  m_t.restart();
  {
    // Isend/Iprobe intentionally implement the paper's asynchronous
    // evolutionary rumor spreading, not a collective-shaped redistribution.
    // exchanger::finish still gives every request and payload an exact,
    // collective teardown lifetime before this scope ends.
    exchanger ex(m_communicator->native_handle());
    do {
      PartitionConfig working_config  = partition_config;

      working_config.graph_allready_partitioned  = false;
      if(!partition_config.strong)
        working_config.no_new_initial_partitioning = false;

      working_config.mh_pool_size = ini_working_config.mh_pool_size;
      if(m_rounds == 0 && working_config.mh_enable_quickstart) {
        ex.quick_start( working_config, G, *m_island );
      }

      perform_local_partitioning( working_config, G );
      if(m_rank == ROOT) {
        std::cout <<  "t left " <<  (m_time_limit - m_t.elapsed()) << std::endl;
      }

      //push and recv
      if( m_t.elapsed() <= m_time_limit && m_size > 1) {
        auto const messages =
            static_cast<unsigned>(std::ceil(std::log(m_size)));
        for( unsigned i = 0; i < messages; i++) {
          ex.push_best( working_config, G, *m_island );
          ex.recv_incoming( working_config, G, *m_island );
        }
      }

      m_rounds++;
    } while( m_t.elapsed() <= m_time_limit );
    ex.finish(static_cast<std::size_t>(G.number_of_nodes()));
  }

  collect_best_partitioning(G, partition_config);
  m_island->print();

  //print logfile (for convergence plots)
  if( partition_config.mh_print_log ) {
    std::stringstream filename_stream;
    filename_stream << "log_"<<  partition_config.graph_filename <<
                       "_m_rank_" <<  m_rank <<
                       "_file_" <<
                       "_seed_" <<  partition_config.seed <<
                       "_k_" <<  partition_config.k;

    std::string filename(filename_stream.str());
    m_island->write_log(filename);
  }

  m_island.reset();
}

void parallel_mh_async::initialize(PartitionConfig & working_config, graph_access & G) {
  // each PE performs a partitioning
  // estimate the runtime of a partitioner call
  // calculate the poolsize and broadcast it to the communicator.
  Individuum first_one;
  m_t.restart();
  if( !working_config.mh_easy_construction) {
    m_island->createIndividuum( working_config, G, first_one, true);
  } else {
    construct_partition cp;
    cp.createIndividuum( working_config, G, first_one, true);
    std::cout <<  "created with objective " <<  first_one.objective << std::endl;
  }

  double time_spend = m_t.elapsed();
  m_island->insert(G, first_one);

  //compute S and Bcast
  int population_size = 1;
  double fraction     = working_config.mh_initial_population_fraction;

  if( m_rank == ROOT ) {
    auto const estimate = ::kahip::parallel_mh::estimate_population_size(
        m_time_limit, fraction, time_spend,
        working_config.mh_easy_construction);
    if (!estimate.has_value()) {
      ::kahip::parallel_mh::detail::abort_evolutionary_collective(
          m_communicator->native_handle(),
          "evolutionary population-size estimation",
          "time limit, initial fraction, and elapsed time must be finite and "
          "within their valid domains");
    }
    population_size = *estimate;
  }

  population_size = ::kahip::parallel_mh::broadcast_population_size(
      m_communicator->native_handle(), population_size,
      working_config.mh_easy_construction);
  std::cout <<  "poolsize = " <<  population_size  << std::endl;

  //set S
  m_island->set_pool_size(population_size);
  working_config.mh_pool_size = population_size;

}

EdgeWeight parallel_mh_async::collect_best_partitioning(graph_access & G, const PartitionConfig & config) {
  //perform partitioning locally
  EdgeWeight min_objective = 0;
  m_island->apply_fittest(G, min_objective);

  std::vector<PartitionID> best_local_map(G.number_of_nodes());
  std::vector< NodeWeight > block_sizes(G.get_partition_count(),0);

  forall_nodes(G, node) {
    best_local_map[node] = G.getPartitionIndex(node);
    block_sizes[G.getPartitionIndex(node)]++;
  } endfor

NodeWeight max_domain_weight = 0;
  for( unsigned i = 0; i < G.get_partition_count(); i++) {
    if( block_sizes[i] > max_domain_weight ) {
      max_domain_weight = block_sizes[i];
    }
  }

  auto const best_global_objective =
      ::kahip::parallel_mh::select_and_broadcast_best_partition(
          m_communicator->native_handle(), min_objective, max_domain_weight,
          config.upper_bound_partition, best_local_map.data(),
          best_local_map.size());

  forall_nodes(G, node) {
    G.setPartitionIndex(node, best_local_map[node]);
  } endfor

  return best_global_objective;
}

EdgeWeight parallel_mh_async::perform_local_partitioning(PartitionConfig & working_config, graph_access & G) {

  quality_metrics qm;
  unsigned local_repetitions = working_config.local_partitioning_repetitions;

  if( working_config.mh_diversify ) {
    diversifyer div;
    div.diversify(working_config);
  }

  //start a new round
  for( unsigned i = 0; i < local_repetitions; i++) {
    if( working_config.mh_no_mh ) {
      Individuum first_ind;

      if( !working_config.mh_easy_construction) {
        m_island->createIndividuum(working_config, G, first_ind, true);
        m_island->insert(G, first_ind);
      } else {
        construct_partition cp;
        cp.createIndividuum( working_config, G, first_ind, true);

        m_island->insert(G, first_ind);
        std::cout <<  "created with objective " <<  first_ind.objective << std::endl;
      }
    } else {
      if( m_island->is_full() && !working_config.mh_disable_combine) {

        int decision = random_functions::nextInt(0,9);
        Individuum output;

        if(decision < working_config.mh_flip_coin) {
          m_island->mutate_random(working_config, G, output);
          m_island->insert(G, output);
        } else {

          int combine_decision = random_functions::nextInt(0,5);
          if(combine_decision <= 4) {
            Individuum first_rnd;
            Individuum second_rnd;
            if(working_config.mh_enable_tournament_selection) {
              m_island->get_two_individuals_tournament(first_rnd, second_rnd);
            } else {
              m_island->get_two_random_individuals(first_rnd, second_rnd);
            }

            m_island->combine(working_config, G, first_rnd, second_rnd, output);

            int coin = 0;

            if( working_config.mh_enable_gal_combine ) {
              coin = random_functions::nextInt(0,100);
            }
            if( coin == 23 ) {
              if( first_rnd.objective > second_rnd.objective) {
                m_island->replace(first_rnd, output);
              } else {
                m_island->replace(second_rnd, output);
              }
            } else {
              m_island->insert(G, output);
            }
          } else if( combine_decision == 5 ) {
            if(!working_config.mh_disable_cross_combine) {
              Individuum selected;
              m_island->get_one_individual_tournament(selected);
              m_island->combine_cross(working_config, G, selected, output);
              m_island->insert(G, output);
            }
          }
        }

      } else {
        Individuum first_ind;
        if(m_island->is_full()) {
          m_island->mutate_random(working_config, G, first_ind);
        } else {
          if( !working_config.mh_easy_construction) {
            m_island->createIndividuum(working_config, G, first_ind, true);
          } else {
            construct_partition cp;
            cp.createIndividuum( working_config, G, first_ind, true);
            std::cout <<  "created with objective " <<  first_ind.objective << std::endl;
          }
        }
        m_island->insert(G, first_ind);
      }
    }

    //try to combine to random inidividuals from pool
    if( m_t.elapsed() > m_time_limit ) {
      break;
    }

  }

  EdgeWeight min_objective = 0;
  m_island->apply_fittest(G, min_objective);

  return min_objective;
}
