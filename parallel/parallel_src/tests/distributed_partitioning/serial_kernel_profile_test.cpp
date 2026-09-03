#include <array>
#include <cstdint>
#include <limits>

#include <catch2/catch_test_macros.hpp>

#include "serial_kernel_profile.h"
#include "range_owner.h"

namespace {
using kahip::serial_kernel::profile_input;
using kahip::serial_kernel::profile_limits;
using kahip::serial_kernel::profile_reason;

TEST_CASE("serial-kernel profile accounts for a safe weighted quotient",
          "[serial-kernel][profile]") {
  auto const input = profile_input{
      .global_nodes = 3,
      .global_directed_edges = 4,
      .total_node_weight = 9,
      .maximum_node_weight = 5,
      .total_directed_edge_weight = 12,
      .maximum_directed_edge_weight = 6,
      .block_count = 2,
      .absolute_bound = 5,
      .labels_are_valid = true,
  };

  auto const profile = kahip::serial_kernel::make_profile(input);

  CHECK(profile.reason == profile_reason::none);
  CHECK(profile.wire_record_bytes == 160);
  CHECK(profile.csr_bytes == 60);
  CHECK(profile.partition_bytes == 12);
  CHECK(profile.serial_input_bytes == 72);
}

TEST_CASE("serial-kernel profile rejects each narrowed scalar one past its domain",
          "[serial-kernel][profile]") {
  auto input = profile_input{
      .global_nodes = 1,
      .global_directed_edges = 0,
      .total_node_weight = 1,
      .maximum_node_weight = 1,
      .total_directed_edge_weight = 0,
      .maximum_directed_edge_weight = 0,
      .block_count = 1,
      .absolute_bound = 1,
      .labels_are_valid = true,
  };

  input.global_nodes = static_cast<std::uint64_t>(
      std::numeric_limits<int>::max()) + 1;
  CHECK(kahip::serial_kernel::make_profile(input).reason ==
        profile_reason::global_node_count_out_of_range);

  input.global_nodes = 1;
  input.global_directed_edges = static_cast<std::uint64_t>(
      std::numeric_limits<int>::max()) + 1;
  CHECK(kahip::serial_kernel::make_profile(input).reason ==
        profile_reason::global_directed_edge_count_out_of_range);

  input.global_directed_edges = 0;
  input.maximum_node_weight = static_cast<std::uint64_t>(
      std::numeric_limits<int>::max()) + 1;
  CHECK(kahip::serial_kernel::make_profile(input).reason ==
        profile_reason::node_weight_out_of_range);

  input.maximum_node_weight = 1;
  input.maximum_directed_edge_weight = static_cast<std::uint64_t>(
      std::numeric_limits<int>::max()) + 1;
  CHECK(kahip::serial_kernel::make_profile(input).reason ==
        profile_reason::edge_weight_out_of_range);

  input.maximum_directed_edge_weight = 0;
  input.total_node_weight = static_cast<std::uint64_t>(
      std::numeric_limits<unsigned>::max()) + 1;
  CHECK(kahip::serial_kernel::make_profile(input).reason ==
        profile_reason::total_node_weight_out_of_range);

  input.total_node_weight = 1;
  input.absolute_bound = static_cast<std::uint64_t>(
      std::numeric_limits<unsigned>::max()) + 1;
  CHECK(kahip::serial_kernel::make_profile(input).reason ==
        profile_reason::absolute_bound_out_of_range);

  input.absolute_bound = 1;
  input.total_directed_edge_weight = static_cast<std::uint64_t>(
      std::numeric_limits<int>::max()) + 1;
  CHECK(kahip::serial_kernel::make_profile(input).reason ==
        profile_reason::total_directed_edge_weight_out_of_range);
}

TEST_CASE("serial-kernel profile honors modified-kernel aggregate domains",
          "[serial-kernel][profile]") {
  auto input = profile_input{
      .global_nodes = 2,
      .global_directed_edges = 0,
      .total_node_weight = 2,
      .maximum_node_weight = 1,
      .total_directed_edge_weight = 0,
      .maximum_directed_edge_weight = 0,
      .block_count = 2,
      .absolute_bound = 1,
      .labels_are_valid = true,
  };

  input.total_node_weight = static_cast<std::uint64_t>(
      std::numeric_limits<int>::max()) + 1;
  CHECK(kahip::serial_kernel::make_profile(input).reason ==
        profile_reason::total_node_weight_out_of_range);

  input.total_node_weight = 2;
  input.absolute_bound =
      static_cast<std::uint64_t>(std::numeric_limits<unsigned>::max() / 2) + 1;
  CHECK(kahip::serial_kernel::make_profile(input).reason ==
        profile_reason::absolute_bound_out_of_range);

  input.absolute_bound = 1;
  input.block_count = 0;
  CHECK(kahip::serial_kernel::make_profile(input).reason ==
        profile_reason::block_count_out_of_range);

  input.block_count = 3;
  CHECK(kahip::serial_kernel::make_profile(input).reason ==
        profile_reason::block_count_out_of_range);

  input.global_nodes = std::numeric_limits<int>::max();
  input.block_count = static_cast<std::uint64_t>(
      std::numeric_limits<unsigned>::max()) / 60 + 1;
  CHECK(kahip::serial_kernel::make_profile(input).reason ==
        profile_reason::stop_rule_domain_out_of_range);

  input.block_count = 2;
  input.global_directed_edges = std::numeric_limits<int>::max();
  input.bank_factor_twice = 3;
  CHECK(kahip::serial_kernel::make_profile(input).reason ==
        profile_reason::quotient_scheduler_domain_out_of_range);
}

TEST_CASE("serial-kernel profile rejects bad v-cycle labels",
          "[serial-kernel][profile]") {
  auto const input = profile_input{
      .global_nodes = 2,
      .global_directed_edges = 2,
      .total_node_weight = 2,
      .maximum_node_weight = 1,
      .total_directed_edge_weight = 2,
      .maximum_directed_edge_weight = 1,
      .block_count = 2,
      .absolute_bound = 1,
      .labels_are_valid = false,
  };
  CHECK(kahip::serial_kernel::make_profile(input).reason ==
        profile_reason::vcycle_labels_out_of_range);
}

TEST_CASE("serial-kernel profile rejects vectors beyond their capacity",
          "[serial-kernel][profile]") {
  auto const input = profile_input{
      .global_nodes = 2,
      .global_directed_edges = 2,
      .total_node_weight = 2,
      .maximum_node_weight = 1,
      .total_directed_edge_weight = 2,
      .maximum_directed_edge_weight = 1,
      .block_count = 2,
      .absolute_bound = 1,
      .labels_are_valid = true,
  };
  auto limits = profile_limits::native();
  limits.int_elements = 1;
  CHECK(kahip::serial_kernel::make_profile(input, limits).reason ==
        profile_reason::vector_capacity_exceeded);
}

TEST_CASE("serial-kernel profile detects checked byte arithmetic overflow",
          "[serial-kernel][profile]") {
  auto const input = profile_input{
      .global_nodes = 2,
      .global_directed_edges = 0,
      .total_node_weight = 0,
      .maximum_node_weight = 0,
      .total_directed_edge_weight = 0,
      .maximum_directed_edge_weight = 0,
      .block_count = 2,
      .absolute_bound = 0,
      .labels_are_valid = true,
  };
  auto limits = profile_limits::native();
  limits.size_limit = 7;
  CHECK(kahip::serial_kernel::make_profile(input, limits).reason ==
        profile_reason::byte_count_overflow);
}

TEST_CASE("range owner lookup preserves the legacy scan across empty ranks",
          "[serial-kernel][owner]") {
  auto const legacy_owner = [](std::array<std::uint64_t, 5> const& ranges,
                               std::uint64_t node) {
    for (int pe = 1; pe < static_cast<int>(ranges.size()); ++pe) {
      if (node < ranges[static_cast<std::size_t>(pe)]) {
        return pe - 1;
      }
    }
    return -1;
  };
  auto const ranges = std::array<std::uint64_t, 5>{0, 0, 2, 2, 5};

  for (auto const node : std::array<std::uint64_t, 8>{0, 1, 2, 3, 4, 5, 6,
                                                        99}) {
    CHECK(kahip::range_owner::from_boundaries(ranges, node) ==
          legacy_owner(ranges, node));
  }
}

TEST_CASE("range owner lookup retains first and last ordinary owners",
          "[serial-kernel][owner]") {
  auto const ranges = std::array<std::uint64_t, 4>{10, 13, 16, 19};

  CHECK(kahip::range_owner::from_boundaries(ranges, 9) == 0);
  CHECK(kahip::range_owner::from_boundaries(ranges, 10) == 0);
  CHECK(kahip::range_owner::from_boundaries(ranges, 12) == 0);
  CHECK(kahip::range_owner::from_boundaries(ranges, 13) == 1);
  CHECK(kahip::range_owner::from_boundaries(ranges, 18) == 2);
  CHECK(kahip::range_owner::from_boundaries(ranges, 19) == -1);
}
}  // namespace
