#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>

#include "parallel_mh/evolutionary_collectives.h"
#include "parallel_mh/population_size_broadcast.h"

TEST_CASE(
    "evolutionary population timing estimate is bounded and deterministic") {
  using kahip::parallel_mh::estimate_population_size;

  CHECK(estimate_population_size(100.0, 10.0, 2.0, false) == 5);
  CHECK(estimate_population_size(100.0, 10.0, 3.0, false) == 4);
  CHECK(estimate_population_size(0.0, 10.0, 2.0, false) == 3);
  CHECK(estimate_population_size(100.0, 10.0, 0.0, false) == 100);
  CHECK(estimate_population_size(100.0, 10.0, 0.0, true) == 50);
  CHECK(estimate_population_size(1.0e300, 1.0e-300, 1.0e-300, false) == 100);
}

TEST_CASE("evolutionary population timing estimate rejects invalid domains") {
  using kahip::parallel_mh::estimate_population_size;
  auto const nan = std::numeric_limits<double>::quiet_NaN();
  auto const infinity = std::numeric_limits<double>::infinity();

  CHECK_FALSE(estimate_population_size(-1.0, 10.0, 2.0, false));
  CHECK_FALSE(estimate_population_size(100.0, 0.0, 2.0, false));
  CHECK_FALSE(estimate_population_size(100.0, -1.0, 2.0, false));
  CHECK_FALSE(estimate_population_size(100.0, 10.0, -1.0, false));
  CHECK_FALSE(estimate_population_size(nan, 10.0, 2.0, false));
  CHECK_FALSE(estimate_population_size(100.0, nan, 2.0, false));
  CHECK_FALSE(estimate_population_size(100.0, 10.0, nan, false));
  CHECK_FALSE(estimate_population_size(infinity, 10.0, 2.0, false));
}

TEST_CASE("evolutionary quick start handles empty and undersubscribed pools") {
  using kahip::parallel_mh::quick_start_plan;
  using kahip::parallel_mh::quick_start_population_plan;

  CHECK((quick_start_population_plan(0U, 5) == quick_start_plan{0U, 0U}));
  CHECK((quick_start_population_plan(1U, 5) == quick_start_plan{0U, 1U}));
  CHECK((quick_start_population_plan(3U, 5) == quick_start_plan{0U, 3U}));
  CHECK((quick_start_population_plan(64U, 5) == quick_start_plan{12U, 52U}));
  CHECK_FALSE(quick_start_population_plan(64U, 0));
}

TEST_CASE("evolutionary objective ordering preserves the weight domain") {
  using kahip::parallel_mh::objective_improved;
  auto const above_int =
      static_cast<std::int64_t>(std::numeric_limits<int>::max()) + 1;

  CHECK(objective_improved(above_int, above_int + 1));
  CHECK_FALSE(objective_improved(above_int + 1, above_int));
  CHECK(objective_improved(std::int64_t{-1}, std::int64_t{0}));
  CHECK_FALSE(objective_improved(std::int64_t{0}, std::int64_t{-1}));
}
