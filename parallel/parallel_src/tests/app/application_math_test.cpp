#include <cstdlib>
#include <cstdint>
#include <limits>

#include "random_state.h"

namespace {
constexpr auto maximum = std::numeric_limits<unsigned long long>::max();

static_assert(kahip::random_compat::outer_rank_seed(7, 4, 0) == 7);
static_assert(kahip::random_compat::outer_rank_seed(7, 4, 3) == 31);
static_assert(kahip::random_compat::outer_rank_seed(536870912, 2, 0) ==
              536870912);
static_assert(kahip::random_compat::outer_rank_seed(536870912, 2, 1) ==
              1073741825);

static_assert(kahip::random_compat::mixed_rank_seed(536870912, 2, 0) ==
              1073741824);
static_assert(kahip::random_compat::mixed_rank_seed(1073741825, 2, 1) ==
              -2147483645);
static_assert(kahip::random_compat::mixed_rank_seed(
                  std::numeric_limits<int>::max(), 2, 0) == -2);
static_assert(kahip::random_compat::mixed_rank_seed(
                  std::numeric_limits<int>::max(), 2, 1) == -1);
static_assert(kahip::random_compat::mixed_rank_seed(
                  std::numeric_limits<int>::min(), 2, 0) == 0);
static_assert(kahip::random_compat::mixed_rank_seed(
                  std::numeric_limits<int>::min(), 2, 1) == 1);

static_assert(!kahip::random_compat::mixed_rank_seed(1, 0, 0).has_value());
static_assert(!kahip::random_compat::mixed_rank_seed(1, -1, 0).has_value());
static_assert(!kahip::random_compat::mixed_rank_seed(1, 2, -1).has_value());
static_assert(!kahip::random_compat::mixed_rank_seed(1, 2, 2).has_value());
static_assert(!kahip::random_compat::outer_rank_seed(1, 0, 0).has_value());
static_assert(!kahip::random_compat::outer_rank_seed(1, 2, 2).has_value());

static_assert(kahip::random_compat::exact_partition_upper_bound(
                  10ULL, 3ULL, 3U) == 4ULL);
static_assert(kahip::random_compat::exact_partition_upper_bound(
                  103ULL, 4ULL, 100U) == 52ULL);
static_assert(kahip::random_compat::exact_partition_upper_bound(
                  200ULL * 200ULL * 200ULL, 256ULL, 3U) == 32187ULL);
static_assert(kahip::random_compat::exact_partition_upper_bound(
                  400ULL * 400ULL * 400ULL, 1564ULL, 3U) == 42148ULL);
static_assert(kahip::random_compat::exact_partition_upper_bound(
                  maximum, 1ULL, 0U) == maximum);
static_assert(!kahip::random_compat::exact_partition_upper_bound(
                   1ULL, 0ULL, 3U)
                   .has_value());
static_assert(!kahip::random_compat::exact_partition_upper_bound(
                   maximum, 1ULL, 1U)
                   .has_value());

static_assert([] {
  auto value = maximum - 1;
  return kahip::random_compat::checked_add(value, 1ULL) && value == maximum;
}());
static_assert([] {
  auto value = maximum;
  return !kahip::random_compat::checked_add(value, 1ULL) && value == maximum;
}());

static_assert(kahip::random_compat::checked_narrow<unsigned>(42148ULL) ==
              42148U);
static_assert(!kahip::random_compat::checked_narrow<std::uint32_t>(maximum)
                   .has_value());
}  // namespace

int main() { return EXIT_SUCCESS; }
