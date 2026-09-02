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
  auto const binary32_conversion_tolerance =
      next_percentage * static_cast<double>(std::numeric_limits<float>::epsilon());
  if (percentage >= next_percentage - binary32_conversion_tolerance) {
    return normalized_imbalance{.effective_percent = next_percent,
                                .was_normalized = true};
  }
  return normalized_imbalance{.effective_percent = effective_percent,
                              .was_normalized = false};
}
}  // namespace kahip::balance
