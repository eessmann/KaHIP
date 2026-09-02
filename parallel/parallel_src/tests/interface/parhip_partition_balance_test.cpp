#include <array>
#include <span>

#include <catch2/catch_test_macros.hpp>

#include "interface/parhip_partition_balance.h"

TEST_CASE("lowest-ID heaviest block wins ties", "[parhip][partition][balance]") {
  auto const weights = std::array{10, 10, 3, 3};

  auto const [block, weight] =
      parhip::detail::lowest_id_heaviest_block(std::span{weights});

  CHECK(block == 0);
  CHECK(weight == 10);
}

TEST_CASE("lowest-ID heaviest block keeps the first nonzero tie",
          "[parhip][partition][balance]") {
  auto const weights = std::array{3, 10, 10};

  auto const [block, weight] =
      parhip::detail::lowest_id_heaviest_block(std::span{weights});

  CHECK(block == 1);
  CHECK(weight == 10);
}

TEST_CASE("lowest-ID heaviest block returns a unique maximum",
          "[parhip][partition][balance]") {
  auto const weights = std::array{3, 7, 11, 2};

  auto const [block, weight] =
      parhip::detail::lowest_id_heaviest_block(std::span{weights});

  CHECK(block == 2);
  CHECK(weight == 11);
}
