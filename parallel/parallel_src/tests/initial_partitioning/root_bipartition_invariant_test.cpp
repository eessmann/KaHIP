#include <catch2/catch_test_macros.hpp>

#include "partition/initial_partitioning/bipartition.h"
#include "tools/random_functions.h"

#include "bipartition_invariant_cases.h"

// The internal friend keeps this test on the real growth routines so later FM
// refinement cannot hide a broken assignment counter.
struct bipartition_invariant_test_access final {
  using graph_type = graph_access;
  using config_type = PartitionConfig;
  using partitioner_type = bipartition;

  static void configure(config_type& config,
                        kahip::test::bipartition_growth algorithm,
                        unsigned grow_target) {
    config.buffoon = false;
    config.connected_blocks = false;
    config.grow_target = static_cast<int>(grow_target);
    config.bipartition_algorithm =
        algorithm == kahip::test::bipartition_growth::bfs ? BIPARTITION_BFS
                                                          : BIPARTITION_FM;
  }

  static void reset_seed(int seed) { random_functions::setSeed(seed); }

  static void grow(partitioner_type& partitioner,
                   kahip::test::bipartition_growth algorithm,
                   config_type const& config,
                   graph_type& graph) {
    if (algorithm == kahip::test::bipartition_growth::bfs) {
      partitioner.grow_regions_bfs(config, graph);
    } else {
      partitioner.grow_regions_fm(config, graph);
    }
  }
};

TEST_CASE("root bipartition leaves a singleton on the RHS") {
  SECTION("BFS") {
    kahip::test::require_singleton_rhs<bipartition_invariant_test_access>(
        kahip::test::bipartition_growth::bfs);
  }
  SECTION("FM") {
    kahip::test::require_singleton_rhs<bipartition_invariant_test_access>(
        kahip::test::bipartition_growth::fm);
  }
}

TEST_CASE("root bipartition assigns a weighted pair at the exact target") {
  SECTION("BFS") {
    kahip::test::require_weighted_pair_exact_target<
        bipartition_invariant_test_access>(
        kahip::test::bipartition_growth::bfs);
  }
  SECTION("FM") {
    kahip::test::require_weighted_pair_exact_target<
        bipartition_invariant_test_access>(kahip::test::bipartition_growth::fm);
  }
}

TEST_CASE("root bipartition reaches its target with two RHS vertices") {
  SECTION("BFS") {
    kahip::test::require_two_rhs_vertices_at_target<
        bipartition_invariant_test_access>(
        kahip::test::bipartition_growth::bfs);
  }
  SECTION("FM") {
    kahip::test::require_two_rhs_vertices_at_target<
        bipartition_invariant_test_access>(kahip::test::bipartition_growth::fm);
  }
}

TEST_CASE("root bipartition restarts across disconnected components") {
  SECTION("BFS") {
    kahip::test::require_disconnected_restart_reaches_target<
        bipartition_invariant_test_access>(
        kahip::test::bipartition_growth::bfs);
  }
  SECTION("FM") {
    kahip::test::require_disconnected_restart_reaches_target<
        bipartition_invariant_test_access>(kahip::test::bipartition_growth::fm);
  }
}
