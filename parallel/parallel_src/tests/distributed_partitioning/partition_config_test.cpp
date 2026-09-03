#include <catch2/catch_test_macros.hpp>

#include <tuple>
#include <utility>

#include "partition_config.h"

TEST_CASE("PPartitionConfig default state is deterministic and neutral") {
  auto const config = parhip::PPartitionConfig{};

  auto const neutral_scalars = std::tuple{
      config.log_num_verts,
      config.edge_factor,
      config.generate_rgg,
      config.generate_ba,
      config.comm_rounds,
      config.number_of_overall_nodes,
      std::to_underlying(config.permutation_quality),
      config.label_iterations,
      config.label_iterations_coarsening,
      config.label_iterations_refinement,
      config.cluster_coarsening_factor,
      config.time_limit,
      config.epsilon,
      config.inbalance,
      config.seed,
      config.k,
      config.evolutionary_time_limit,
      config.upper_bound_partition,
      config.upper_bound_cluster,
      config.total_num_labels,
      std::to_underlying(config.initial_partitioning_algorithm),
      config.stop_factor,
      config.vcycle,
      config.num_vcycles,
      config.num_tries,
      std::to_underlying(config.node_ordering),
      config.no_refinement_in_last_iteration,
      config.ht_fill_factor,
      config.eco,
      config.binary_io_window_size,
      config.barabasi_albert_mindegree,
      config.compute_degree_sequence_ba,
      config.compute_degree_sequence_k_first,
      config.kronecker_internal_only,
      config.k_deg,
      config.generate_ba_32bit,
      config.n,
      config.save_partition,
      config.save_partition_binary,
      config.vertex_degree_weights,
      config.converter_evaluate,
  };

  CHECK(std::apply(
      [](auto const... value) { return ((value == 0) && ...); },
      neutral_scalars));
  CHECK(config.input_partition.empty());
  CHECK(config.graph_filename.empty());
  CHECK(config.input_partition_filename.empty());
}
