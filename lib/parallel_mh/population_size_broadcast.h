#pragma once

#include <mpi.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

#include "parallel_mh/evolutionary_collectives.h"

namespace kahip::parallel_mh {
struct quick_start_plan final {
  unsigned local_creations = 0;
  unsigned diversifications = 0;

  auto operator==(quick_start_plan const&) const -> bool = default;
};

[[nodiscard]] constexpr auto clamp_population_size(
    int estimate,
    bool easy_construction) noexcept -> int {
  return std::clamp(estimate, 3, easy_construction ? 50 : 100);
}

[[nodiscard]] constexpr auto quick_start_population_plan(
    unsigned population_size,
    int communicator_size) noexcept -> std::optional<quick_start_plan> {
  if (communicator_size <= 0) {
    return std::nullopt;
  }

  auto const pool = static_cast<std::int64_t>(population_size);
  auto const peers = static_cast<std::int64_t>(communicator_size);
  auto const local_share = pool / peers + (pool % peers != 0 ? 1 : 0);
  auto const local_creations = std::max<std::int64_t>(local_share - 1, 0);
  auto const diversifications =
      std::max(pool - local_creations, std::int64_t{0});
  return quick_start_plan{
      .local_creations = static_cast<unsigned>(local_creations),
      .diversifications = static_cast<unsigned>(diversifications),
  };
}

[[nodiscard]] inline auto estimate_population_size(
    double time_limit,
    double initial_population_fraction,
    double elapsed_partition_time,
    bool easy_construction) noexcept -> std::optional<int> {
  if (!std::isfinite(time_limit) || time_limit < 0.0 ||
      !std::isfinite(initial_population_fraction) ||
      initial_population_fraction <= 0.0 ||
      !std::isfinite(elapsed_partition_time) || elapsed_partition_time < 0.0) {
    return std::nullopt;
  }

  auto const maximum = easy_construction ? 50 : 100;
  if (elapsed_partition_time == 0.0) {
    return maximum;
  }

  // Preserve the upstream formula and its ceiling, but compare against the
  // bounded population domain before converting a potentially huge value to
  // int. Long double also avoids avoidable overflow in the intermediate
  // divisions on implementations where it has a wider exponent range.
  auto const estimate =
      (static_cast<long double>(time_limit) /
       static_cast<long double>(initial_population_fraction)) /
      static_cast<long double>(elapsed_partition_time);
  if (!std::isfinite(estimate) || estimate >= maximum) {
    return maximum;
  }
  return clamp_population_size(static_cast<int>(std::ceil(estimate)),
                               easy_construction);
}

[[nodiscard]] inline auto broadcast_population_size(
    MPI_Comm communicator,
    int root_estimate,
    bool easy_construction) noexcept -> int {
  auto population_size = root_estimate;
  detail::check_mpi(MPI_Bcast(&population_size, 1, MPI_INT, 0, communicator),
                    communicator, "MPI_Bcast(population size)");
  return clamp_population_size(population_size, easy_construction);
}
}  // namespace kahip::parallel_mh
