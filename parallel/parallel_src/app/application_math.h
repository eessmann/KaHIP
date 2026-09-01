#pragma once

#include <bit>
#include <concepts>
#include <limits>
#include <optional>
#include <type_traits>
#include <utility>

namespace parhip::application {
[[nodiscard]] constexpr auto rank_seed(int base_seed,
                                       int process_count,
                                       int rank) noexcept
    -> std::optional<int> {
  if (process_count <= 0 || rank < 0 || rank >= process_count) {
    return std::nullopt;
  }
  if (rank == 0) {
    return base_seed;
  }

  // Preserve KaHIP's historical rank-specific stream modulo the width of
  // int, but perform the arithmetic in the corresponding unsigned type so
  // large seeds have deterministic C++23 semantics instead of signed UB.
  using unsigned_int = std::make_unsigned_t<int>;
  auto const base_bits = std::bit_cast<unsigned_int>(base_seed);
  auto const mixed_bits =
      base_bits * static_cast<unsigned_int>(process_count) +
      static_cast<unsigned_int>(rank);
  return std::bit_cast<int>(mixed_bits);
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
}  // namespace parhip::application
