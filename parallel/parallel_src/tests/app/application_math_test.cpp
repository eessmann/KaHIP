#include <cstdlib>
#include <limits>

#include "application_math.h"

namespace {
constexpr auto maximum = std::numeric_limits<unsigned long long>::max();

static_assert(parhip::application::rank_seed(7, 4, 0) == 7);
static_assert(parhip::application::rank_seed(7, 4, 3) == 31);
static_assert(parhip::application::rank_seed(
                  std::numeric_limits<int>::max(), 2, 1) == -1);
static_assert(!parhip::application::rank_seed(1, 0, 0).has_value());
static_assert(!parhip::application::rank_seed(1, 2, 2).has_value());

static_assert(parhip::application::exact_partition_upper_bound(
                  10ULL, 3ULL, 3U) == 4ULL);
static_assert(parhip::application::exact_partition_upper_bound(
                  103ULL, 4ULL, 100U) == 52ULL);
static_assert(parhip::application::exact_partition_upper_bound(
                  maximum, 1ULL, 0U) == maximum);
static_assert(!parhip::application::exact_partition_upper_bound(
                   1ULL, 0ULL, 3U)
                   .has_value());
static_assert(!parhip::application::exact_partition_upper_bound(
                   maximum, 1ULL, 1U)
                   .has_value());

static_assert([] {
  auto value = maximum - 1;
  return parhip::application::checked_add(value, 1ULL) && value == maximum;
}());
static_assert([] {
  auto value = maximum;
  return !parhip::application::checked_add(value, 1ULL) && value == maximum;
}());
}  // namespace

int main() { return EXIT_SUCCESS; }
