#pragma once

#include <cmath>
#include <limits>
#include <optional>

namespace kahip::balance {
struct normalized_imbalance final {
  unsigned effective_percent;
  bool was_normalized;
};

[[nodiscard]] inline auto normalize_fractional_imbalance(
    double fractional_imbalance) noexcept
    -> std::optional<normalized_imbalance> {
  if (!std::isfinite(fractional_imbalance) || fractional_imbalance < 0.0) {
    return std::nullopt;
  }

  auto const maximum = static_cast<double>(std::numeric_limits<unsigned>::max());
  auto const maximum_fraction = maximum / 100.0;
  if (fractional_imbalance > maximum_fraction) {
    return std::nullopt;
  }

  auto const percentage = 100.0 * fractional_imbalance;
  if (!std::isfinite(percentage)) {
    return std::nullopt;
  }
  if (percentage >= maximum) {
    return normalized_imbalance{
        .effective_percent = std::numeric_limits<unsigned>::max(),
        .was_normalized = false};
  }

  auto const floored_percentage = std::floor(percentage);
  auto const effective_percent = static_cast<unsigned>(floored_percentage);
  if (percentage == floored_percentage ||
      effective_percent == std::numeric_limits<unsigned>::max()) {
    return normalized_imbalance{.effective_percent = effective_percent,
                                .was_normalized = false};
  }

  auto const next_percent = effective_percent + 1;
  auto const next_percentage = static_cast<double>(next_percent);
  auto const widened_binary32_origin = static_cast<double>(
      static_cast<float>(next_percentage / 100.0));
  auto const binary32_origin_for = [](unsigned percent) noexcept {
    return static_cast<double>(
        static_cast<float>(static_cast<double>(percent) / 100.0));
  };
  auto const lower_target_collides =
      next_percent != 0 &&
      binary32_origin_for(next_percent - 1) == widened_binary32_origin;
  auto const upper_target_collides =
      next_percent != std::numeric_limits<unsigned>::max() &&
      binary32_origin_for(next_percent + 1) == widened_binary32_origin;
  if (fractional_imbalance == widened_binary32_origin &&
      !lower_target_collides && !upper_target_collides) {
    return normalized_imbalance{.effective_percent = next_percent,
                                .was_normalized = true};
  }
  return normalized_imbalance{.effective_percent = effective_percent,
                              .was_normalized = false};
}
}  // namespace kahip::balance
