#include <array>
#include <cstddef>

#include <catch2/catch_test_macros.hpp>

#include "tools/random_functions.h"
#include "../../../modified_kahip/lib/definitions.h"
#include "../../../modified_kahip/lib/partition/partition_config.h"
#undef RANDOM_FUNCTIONS_RMEPKWYT
#include "../../../modified_kahip/lib/tools/random_functions.h"

namespace {
// Literal golden values produced by the unmodified implementation at
// 5935f349f65f1788a9b68fcf6d853e698d86956d.  They deliberately do not use
// another random engine or distribution to compute the expected result.
constexpr auto upstream_seed_zero_integers =
    std::array<unsigned, 8>{549, 593, 715, 845, 603, 858, 545, 848};
constexpr auto upstream_seed_zero_doubles = std::array{
    2.2009385857735477,
    -0.028085365904534809,
    1.9154961187930293,
    1.9922001673803664,
    2.5582367896839218,
    -1.0122431535330803,
    -0.3238862214255549,
    1.84114797405952};
constexpr auto upstream_seed_zero_booleans = std::array{
    true, true, true, true, true, true, true, true,
    false, true, true, false, false, false, true, false};
}  // namespace

TEST_CASE("ParHIP integer draws match pinned upstream", "[rng][oracle]") {
  parhip::random_functions::setSeed(0);

  for (auto const expected : upstream_seed_zero_integers) {
    REQUIRE(parhip::random_functions::nextInt<unsigned>(0, 1000) == expected);
  }
}

TEST_CASE("ParHIP double draws match pinned upstream", "[rng][oracle]") {
  parhip::random_functions::setSeed(0);

  for (auto const expected : upstream_seed_zero_doubles) {
    REQUIRE(parhip::random_functions::nextDouble(-2.0, 3.0) == expected);
  }
}

TEST_CASE("ParHIP boolean draws match pinned upstream", "[rng][oracle]") {
  parhip::random_functions::setSeed(0);

  for (auto const expected : upstream_seed_zero_booleans) {
    REQUIRE(parhip::random_functions::nextBool() == expected);
  }
}

TEST_CASE("ParHIP seed reset restores both upstream streams", "[rng][oracle]") {
  parhip::random_functions::setSeed(17);
  auto const integer =
      parhip::random_functions::nextInt<unsigned long long>(0, 1'000'000);
  auto const real = parhip::random_functions::nextDouble(10.0, 25.0);
  auto const boolean = parhip::random_functions::nextBool();

  REQUIRE(integer == 294665);
  REQUIRE(real == 18.576910003822722);
  REQUIRE_FALSE(boolean);

  parhip::random_functions::setSeed(17);
  REQUIRE(parhip::random_functions::nextInt<unsigned long long>(0, 1'000'000) ==
          integer);
  REQUIRE(parhip::random_functions::nextDouble(10.0, 25.0) == real);
  REQUIRE(parhip::random_functions::nextBool() == boolean);
}

TEST_CASE("ParHIP and modified KaHIP retain the pinned shared RNG stream",
          "[rng][oracle][integration]") {
  // At upstream 5935f349 the two components resolve to one global
  // random_functions state.  These literals were produced by alternating the
  // same calls in that pristine implementation, preserving C rand and MT
  // reset timing.
  parhip::random_functions::setSeed(17);
  REQUIRE(parhip::random_functions::nextInt<unsigned>(0, 1000) == 294);
  REQUIRE_FALSE(kahip::modified::random_functions::nextBool());
  REQUIRE(parhip::random_functions::nextInt<unsigned>(0, 1000) == 531);
  REQUIRE(kahip::modified::random_functions::nextDouble(-4.0, 7.0) ==
          2.2897340028033284);
  REQUIRE(kahip::modified::random_functions::nextBool());

  kahip::modified::random_functions::setSeed(9);
  REQUIRE(parhip::random_functions::nextInt<unsigned>(0, 1000) == 10);
  REQUIRE(parhip::random_functions::nextDouble(-4.0, 7.0) ==
          -1.7233800723791961);
}
