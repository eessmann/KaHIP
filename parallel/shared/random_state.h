#ifndef KAHIP_PARALLEL_SHARED_RANDOM_STATE_H
#define KAHIP_PARALLEL_SHARED_RANDOM_STATE_H

#include <bit>
#include <concepts>
#include <limits>
#include <optional>
#include <random>
#include <type_traits>
#include <utility>

namespace kahip::random_compat {
using engine_type = std::mt19937;

// The pinned upstream ParHIP and modified KaHIP objects resolve to one global
// random_functions state.  Keep that behavioral contract while retaining the
// branch's component namespaces.
inline int seed = 0;
inline engine_type engine;

[[nodiscard]] constexpr auto mixed_rank_seed(int base_seed,
                                             int process_count,
                                             int rank) noexcept
    -> std::optional<int> {
  if (process_count <= 0 || rank < 0 || rank >= process_count) {
    return std::nullopt;
  }

  // Reproduce the historical base * size + rank stream modulo int width
  // without evaluating an overflowing signed multiplication.
  using unsigned_int = std::make_unsigned_t<int>;
  auto const base_bits = std::bit_cast<unsigned_int>(base_seed);
  auto const mixed_bits =
      base_bits * static_cast<unsigned_int>(process_count) +
      static_cast<unsigned_int>(rank);
  return std::bit_cast<int>(mixed_bits);
}

[[nodiscard]] constexpr auto outer_rank_seed(int base_seed,
                                             int process_count,
                                             int rank) noexcept
    -> std::optional<int> {
  if (process_count <= 0 || rank < 0 || rank >= process_count) {
    return std::nullopt;
  }
  return rank == 0 ? std::optional<int>{base_seed}
                   : mixed_rank_seed(base_seed, process_count, rank);
}

template <std::unsigned_integral Value>
[[nodiscard]] constexpr auto checked_add(Value& accumulator,
                                         Value increment) noexcept -> bool {
  if (increment > std::numeric_limits<Value>::max() - accumulator) {
    return false;
  }
  accumulator += increment;
  return true;
}

template <std::integral Destination, std::integral Source>
[[nodiscard]] constexpr auto checked_narrow(Source value) noexcept
    -> std::optional<Destination> {
  if (!std::in_range<Destination>(value)) {
    return std::nullopt;
  }
  return static_cast<Destination>(value);
}

template <std::unsigned_integral Value>
[[nodiscard]] constexpr auto exact_partition_upper_bound(
    Value total_weight,
    Value block_count,
    unsigned imbalance_percent) noexcept -> std::optional<Value> {
  constexpr auto maximum = std::numeric_limits<Value>::max();
  if (block_count == 0 || !std::in_range<Value>(imbalance_percent)) {
    return std::nullopt;
  }

  auto const quotient = total_weight / block_count;
  auto const remainder = total_weight % block_count;
  if (remainder != 0 && quotient == maximum) {
    return std::nullopt;
  }
  auto const ceiling = quotient + (remainder == 0 ? Value{0} : Value{1});
  auto const imbalance = static_cast<Value>(imbalance_percent);

  auto const whole_hundreds = ceiling / Value{100};
  if (imbalance != 0 && whole_hundreds > maximum / imbalance) {
    return std::nullopt;
  }
  auto const whole_extra = whole_hundreds * imbalance;

  auto const remaining_hundredths = ceiling % Value{100};
  if (imbalance != 0 && remaining_hundredths > maximum / imbalance) {
    return std::nullopt;
  }
  auto const fractional_extra =
      remaining_hundredths * imbalance / Value{100};
  if (fractional_extra > maximum - whole_extra) {
    return std::nullopt;
  }
  auto const total_extra = whole_extra + fractional_extra;
  if (total_extra > maximum - ceiling) {
    return std::nullopt;
  }
  return ceiling + total_extra;
}
}  // namespace kahip::random_compat

#endif
