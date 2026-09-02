#include <array>
#include <cmath>
#include <limits>
#include <span>

#include <catch2/catch_test_macros.hpp>

#include "imbalance.h"
#include "interface/parhip_partition_balance.h"
#include "random_state.h"

TEST_CASE("native three-percent imbalance keeps its whole percentage",
          "[parhip][partition][balance]") {
  auto const normalized = kahip::balance::normalize_fractional_imbalance(0.03);

  REQUIRE(normalized.has_value());
  CHECK(normalized->effective_percent == 3);
  CHECK_FALSE(normalized->was_normalized);
}

TEST_CASE("only the exact widened binary32 origin normalizes upward",
          "[parhip][partition][balance]") {
  auto const widened =
      kahip::balance::normalize_fractional_imbalance(
          static_cast<double>(float{0.03F}));
  auto const preceding_binary32 =
      kahip::balance::normalize_fractional_imbalance(static_cast<double>(
          std::nextafter(float{0.03F}, 0.0F)));
  auto const genuine_near_integer =
      kahip::balance::normalize_fractional_imbalance(0.029999998);
  auto const large_percentage_ambiguity =
      kahip::balance::normalize_fractional_imbalance(83886.075);

  REQUIRE(widened.has_value());
  CHECK(widened->effective_percent == 3);
  CHECK(widened->was_normalized);
  REQUIRE(preceding_binary32.has_value());
  CHECK(preceding_binary32->effective_percent == 2);
  CHECK_FALSE(preceding_binary32->was_normalized);
  REQUIRE(genuine_near_integer.has_value());
  CHECK(genuine_near_integer->effective_percent == 2);
  CHECK_FALSE(genuine_near_integer->was_normalized);
  REQUIRE(large_percentage_ambiguity.has_value());
  CHECK(large_percentage_ambiguity->effective_percent == 8388607);
  CHECK_FALSE(large_percentage_ambiguity->was_normalized);
}

TEST_CASE("genuine fractional percentages retain floor semantics",
          "[parhip][partition][balance]") {
  auto const normalized =
      kahip::balance::normalize_fractional_imbalance(0.025);

  REQUIRE(normalized.has_value());
  CHECK(normalized->effective_percent == 2);
  CHECK_FALSE(normalized->was_normalized);
}

TEST_CASE("imbalance normalization rejects invalid and out-of-range inputs",
          "[parhip][partition][balance]") {
  auto const largest_fraction =
      static_cast<double>(std::numeric_limits<unsigned>::max()) / 100.0;
  auto const largest =
      kahip::balance::normalize_fractional_imbalance(largest_fraction);

  REQUIRE(largest.has_value());
  CHECK(largest->effective_percent == std::numeric_limits<unsigned>::max());
  CHECK_FALSE(largest->was_normalized);
  CHECK_FALSE(kahip::balance::normalize_fractional_imbalance(-0.01));
  CHECK_FALSE(kahip::balance::normalize_fractional_imbalance(
      std::numeric_limits<double>::infinity()));
  CHECK_FALSE(kahip::balance::normalize_fractional_imbalance(
      std::numeric_limits<double>::quiet_NaN()));
  CHECK_FALSE(kahip::balance::normalize_fractional_imbalance(
      std::nextafter(largest_fraction, std::numeric_limits<double>::infinity())));
}

TEST_CASE("normalized three percent keeps the checked 600-cubed bound",
          "[parhip][partition][balance]") {
  auto const normalized = kahip::balance::normalize_fractional_imbalance(0.03);

  REQUIRE(normalized.has_value());
  CHECK(kahip::random_compat::exact_partition_upper_bound(
            600ULL * 600ULL * 600ULL, 2304ULL,
            normalized->effective_percent) == 96562ULL);
  CHECK_FALSE(kahip::random_compat::exact_partition_upper_bound(
      std::numeric_limits<unsigned long long>::max(), 1ULL, 1U));
}

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
