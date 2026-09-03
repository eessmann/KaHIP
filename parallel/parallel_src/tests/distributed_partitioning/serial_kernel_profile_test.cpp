#include <array>
#include <cstdint>
#include <limits>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "serial_kernel_profile.h"
#include "serial_kernel_bridge.h"
#include "serial_kernel_structure.h"
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
  CHECK(profile.complete_graph_bytes == 192);
  CHECK(profile.structural_validation_bytes == 96);
  CHECK(profile.base_memory_bytes == 448);
  CHECK(profile.flat_payload_elements == 15);
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

TEST_CASE("serial-kernel profile accepts exact scalar limits",
          "[serial-kernel][profile]") {
  auto node_limit = profile_input{
      .global_nodes = 1,
      .total_node_weight = std::numeric_limits<int>::max(),
      .maximum_node_weight = std::numeric_limits<int>::max(),
      .block_count = 1,
      .absolute_bound = std::numeric_limits<unsigned>::max() / 2,
  };
  CHECK(kahip::serial_kernel::make_profile(node_limit).safe());

  auto edge_limit = profile_input{
      .global_nodes = 1,
      .global_directed_edges = std::numeric_limits<int>::max(),
      .total_node_weight = 1,
      .maximum_node_weight = 1,
      .total_directed_edge_weight = std::numeric_limits<int>::max(),
      .maximum_directed_edge_weight = std::numeric_limits<int>::max(),
      .block_count = 1,
      .absolute_bound = 1,
      .bank_factor_twice = 1,
  };
  CHECK(kahip::serial_kernel::make_profile(edge_limit).safe());
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
  input.global_nodes = 2;
  input.global_directed_edges = std::numeric_limits<int>::max() - 1;
  input.bank_factor_twice = 6;
  CHECK(kahip::serial_kernel::make_profile(input).safe());
}

TEST_CASE("serial-kernel profile bounds the quotient scheduler exactly",
          "[serial-kernel][profile]") {
  auto input = profile_input{
      .global_nodes = 4,
      .global_directed_edges = 12,
      .total_node_weight = 4,
      .maximum_node_weight = 1,
      .block_count = 4,
      .absolute_bound = 1,
      .bank_factor_twice = 6,
  };
  auto limits = profile_limits::native();
  limits.int_max = 18;
  CHECK(kahip::serial_kernel::make_profile(input, limits).safe());

  limits.int_max = 17;
  CHECK(kahip::serial_kernel::make_profile(input, limits).reason ==
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
  limits.xadj_elements = 2;
  CHECK(kahip::serial_kernel::make_profile(input, limits).reason ==
        profile_reason::vector_capacity_exceeded);
}

TEST_CASE("serial-kernel profile accounts for both complete-graph sentinels",
          "[serial-kernel][profile]") {
  auto const input = profile_input{
      .global_nodes = 2,
      .global_directed_edges = 0,
      .total_node_weight = 2,
      .maximum_node_weight = 1,
      .block_count = 1,
      .absolute_bound = 2,
  };
  auto limits = profile_limits::native();
  limits.complete_node_elements = 3;
  limits.complete_node_data_elements = 3;
  CHECK(kahip::serial_kernel::make_profile(input, limits).safe());

  limits.complete_node_data_elements = 2;
  CHECK(kahip::serial_kernel::make_profile(input, limits).reason ==
        profile_reason::vector_capacity_exceeded);
}

TEST_CASE("serial-kernel profile checks the combined distribution payload",
          "[serial-kernel][profile]") {
  auto const input = profile_input{
      .global_nodes = 2,
      .global_directed_edges = 3,
      .total_node_weight = 2,
      .maximum_node_weight = 1,
      .total_directed_edge_weight = 3,
      .maximum_directed_edge_weight = 1,
      .block_count = 1,
      .absolute_bound = 2,
  };
  auto limits = profile_limits::native();
  // Each array fits independently: xadj=3, adjncy=3, node=2, edge=3.
  limits.xadj_elements = 3;
  limits.adjncy_elements = 3;
  limits.node_weight_elements = 2;
  limits.edge_weight_elements = 3;
  limits.partition_elements = 2;
  limits.flat_payload_elements = 11;
  auto const profile = kahip::serial_kernel::make_profile(input, limits);
  CHECK(profile.flat_payload_elements == 11);
  CHECK(profile.safe());

  limits.flat_payload_elements = 10;
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

TEST_CASE("serial-kernel profile accounts every byte stage at its boundary",
          "[serial-kernel][profile]") {
  auto const input = profile_input{
      .global_nodes = 3,
      .global_directed_edges = 4,
      .total_node_weight = 3,
      .maximum_node_weight = 1,
      .total_directed_edge_weight = 4,
      .maximum_directed_edge_weight = 1,
      .block_count = 2,
      .absolute_bound = 2,
  };
  using kahip::serial_kernel::byte_accounting_stage;
  auto const accounting = kahip::serial_kernel::account_profile_bytes(input);
  REQUIRE(accounting.safe());
  struct byte_total_case final {
    std::uint64_t kahip::serial_kernel::profile_byte_accounting::*member;
    std::uint64_t profile_limits::*limit;
    byte_accounting_stage rejection_stage;
    std::uint64_t exact;
  };
  auto const totals = std::array<byte_total_case, 15>{
      byte_total_case{&kahip::serial_kernel::profile_byte_accounting::node_wire_bytes,
                      &profile_limits::node_wire_byte_limit,
                      byte_accounting_stage::node_wire, 96},
      byte_total_case{&kahip::serial_kernel::profile_byte_accounting::edge_wire_bytes,
                      &profile_limits::edge_wire_byte_limit,
                      byte_accounting_stage::edge_wire, 64},
      byte_total_case{&kahip::serial_kernel::profile_byte_accounting::wire_record_bytes,
                      &profile_limits::wire_record_byte_limit,
                      byte_accounting_stage::wire_record, 160},
      byte_total_case{&kahip::serial_kernel::profile_byte_accounting::xadj_bytes,
                      &profile_limits::xadj_byte_limit,
                      byte_accounting_stage::xadj, 16},
      byte_total_case{&kahip::serial_kernel::profile_byte_accounting::adjacency_bytes,
                      &profile_limits::adjacency_byte_limit,
                      byte_accounting_stage::adjacency, 16},
      byte_total_case{&kahip::serial_kernel::profile_byte_accounting::node_weight_bytes,
                      &profile_limits::node_weight_byte_limit,
                      byte_accounting_stage::node_weight, 12},
      byte_total_case{&kahip::serial_kernel::profile_byte_accounting::edge_weight_bytes,
                      &profile_limits::edge_weight_byte_limit,
                      byte_accounting_stage::edge_weight, 16},
      byte_total_case{&kahip::serial_kernel::profile_byte_accounting::csr_bytes,
                      &profile_limits::csr_byte_limit,
                      byte_accounting_stage::csr, 60},
      byte_total_case{&kahip::serial_kernel::profile_byte_accounting::partition_bytes,
                      &profile_limits::partition_byte_limit,
                      byte_accounting_stage::partition, 12},
      byte_total_case{&kahip::serial_kernel::profile_byte_accounting::serial_input_bytes,
                      &profile_limits::serial_input_byte_limit,
                      byte_accounting_stage::serial_input, 72},
      byte_total_case{&kahip::serial_kernel::profile_byte_accounting::complete_node_bytes,
                      &profile_limits::complete_node_byte_limit,
                      byte_accounting_stage::complete_node, 32},
      byte_total_case{&kahip::serial_kernel::profile_byte_accounting::complete_node_data_bytes,
                      &profile_limits::complete_node_data_byte_limit,
                      byte_accounting_stage::complete_node_data, 96},
      byte_total_case{&kahip::serial_kernel::profile_byte_accounting::complete_edge_bytes,
                      &profile_limits::complete_edge_byte_limit,
                      byte_accounting_stage::complete_edge, 64},
      byte_total_case{&kahip::serial_kernel::profile_byte_accounting::complete_graph_bytes,
                      &profile_limits::complete_graph_byte_limit,
                      byte_accounting_stage::complete_graph, 192},
      byte_total_case{&kahip::serial_kernel::profile_byte_accounting::structural_validation_bytes,
                      &profile_limits::structural_validation_byte_limit,
                      byte_accounting_stage::structural_validation, 96},
  };
  for (auto const& test : totals) {
    CHECK(accounting.*(test.member) == test.exact);
    auto at_limit = profile_limits::native();
    at_limit.*(test.limit) = test.exact;
    CHECK(kahip::serial_kernel::account_profile_bytes(input, at_limit).safe());
    at_limit.*(test.limit) = test.exact - 1;
    CHECK(kahip::serial_kernel::account_profile_bytes(input, at_limit).stage ==
          test.rejection_stage);
  }
  auto base_limit = profile_limits::native();
  base_limit.base_memory_byte_limit = 448;
  CHECK(kahip::serial_kernel::account_profile_bytes(input, base_limit).safe());
  base_limit.base_memory_byte_limit = 447;
  CHECK(kahip::serial_kernel::account_profile_bytes(input, base_limit).stage ==
        byte_accounting_stage::base_memory);
}

TEST_CASE("serial-kernel profile has independent exact vector boundaries",
          "[serial-kernel][profile]") {
  auto const input = profile_input{
      .global_nodes = 2,
      .global_directed_edges = 2,
      .total_node_weight = 2,
      .maximum_node_weight = 1,
      .total_directed_edge_weight = 2,
      .maximum_directed_edge_weight = 1,
      .block_count = 1,
      .absolute_bound = 2,
  };
  auto check_boundary = [&](auto member, std::uint64_t required) {
    auto limits = profile_limits::native();
    limits.*member = required;
    CHECK(kahip::serial_kernel::make_profile(input, limits).safe());
    limits.*member = required - 1;
    CHECK(kahip::serial_kernel::make_profile(input, limits).reason ==
          profile_reason::vector_capacity_exceeded);
  };
  check_boundary(&profile_limits::xadj_elements, 3);
  check_boundary(&profile_limits::adjncy_elements, 2);
  check_boundary(&profile_limits::node_weight_elements, 2);
  check_boundary(&profile_limits::edge_weight_elements, 2);
  check_boundary(&profile_limits::partition_elements, 2);
  check_boundary(&profile_limits::wire_node_elements, 2);
  check_boundary(&profile_limits::wire_edge_elements, 2);
  check_boundary(&profile_limits::complete_node_elements, 3);
  check_boundary(&profile_limits::complete_node_data_elements, 3);
  check_boundary(&profile_limits::complete_edge_elements, 2);
  check_boundary(&profile_limits::structural_validation_elements, 2);
  check_boundary(&profile_limits::flat_payload_elements, 9);
}

TEST_CASE("serial-kernel profile exposes flag and mode-product boundaries",
          "[serial-kernel][profile]") {
  auto input = profile_input{
      .global_nodes = 2,
      .global_directed_edges = 0,
      .total_node_weight = 2,
      .maximum_node_weight = 1,
      .block_count = 2,
      .absolute_bound = 1,
  };
  input.csr_offsets_are_valid = false;
  CHECK(kahip::serial_kernel::make_profile(input).reason ==
        profile_reason::csr_offset_out_of_range);
  input.csr_offsets_are_valid = true;
  input.targets_are_valid = false;
  CHECK(kahip::serial_kernel::make_profile(input).reason ==
        profile_reason::target_out_of_range);

  input.targets_are_valid = true;
  input.social_mode = true;
  auto limits = profile_limits::native();
  limits.modified_node_weight_max = 10'000;
  CHECK(kahip::serial_kernel::make_profile(input, limits).safe());
  input.global_nodes = 3;
  input.total_node_weight = 3;
  input.block_count = 3;
  CHECK(kahip::serial_kernel::make_profile(input, limits).reason ==
        profile_reason::stop_rule_domain_out_of_range);

  input.block_count = 2;
  input.social_mode = false;
  limits = profile_limits::native();
  limits.int_max = 8;
  CHECK(kahip::serial_kernel::make_profile(input, limits).safe());
  input.block_count = 3;
  CHECK(kahip::serial_kernel::make_profile(input, limits).reason ==
        profile_reason::stop_rule_domain_out_of_range);

  input.global_nodes = 2;
  input.total_node_weight = 2;
  input.block_count = 2;
  limits = profile_limits::native();
  limits.modified_node_weight_max = 120;
  CHECK(kahip::serial_kernel::make_profile(input, limits).safe());
  limits.modified_node_weight_max = 119;
  CHECK(kahip::serial_kernel::make_profile(input, limits).reason ==
        profile_reason::stop_rule_domain_out_of_range);
}

TEST_CASE("range owner lookup matches the legacy oracle at degenerate boundaries",
          "[serial-kernel][owner]") {
  auto const legacy_owner = [](auto const& ranges, std::uint64_t node) {
    for (int pe = 1; pe < static_cast<int>(ranges.size()); ++pe) {
      if (node < ranges[static_cast<std::size_t>(pe)]) {
        return pe - 1;
      }
    }
    return -1;
  };
  auto check = [&](auto const& ranges, auto const& nodes) {
    for (auto node : nodes) {
      CHECK(kahip::range_owner::from_boundaries(ranges, node) ==
            legacy_owner(ranges, node));
    }
  };
  check(std::array<std::uint64_t, 1>{0},
        std::array<std::uint64_t, 3>{0, 1, 99});
  check(std::array<std::uint64_t, 2>{7, 8},
        std::array<std::uint64_t, 4>{6, 7, 8, 99});
  check(std::array<std::uint64_t, 4>{0, 0, 0, 0},
        std::array<std::uint64_t, 3>{0, 1, 99});
  check(std::array<std::uint64_t, 5>{0, 2, 2, 2, 2},
        std::array<std::uint64_t, 5>{0, 1, 2, 3, 99});
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

TEST_CASE("serial kernel accepts only reciprocal loop-free weighted arcs",
          "[serial-kernel][structure]") {
  using kahip::serial_kernel::directed_arc;
  auto const valid_unsorted = std::vector<directed_arc>{
      {1, 0, 7}, {0, 2, 3}, {2, 0, 3}, {0, 1, 7},
      {1, 0, 7}, {0, 1, 7}};
  CHECK(kahip::serial_kernel::is_loop_free_reciprocal_undirected(
      valid_unsorted));
  CHECK_FALSE(kahip::serial_kernel::is_loop_free_reciprocal_undirected(
      std::vector<directed_arc>{{0, 0, 1}}));
  CHECK_FALSE(kahip::serial_kernel::is_loop_free_reciprocal_undirected(
      std::vector<directed_arc>{{0, 1, 1}}));
  CHECK_FALSE(kahip::serial_kernel::is_loop_free_reciprocal_undirected(
      std::vector<directed_arc>{{0, 1, 1}, {1, 0, 2}}));
  CHECK_FALSE(kahip::serial_kernel::is_loop_free_reciprocal_undirected(
      std::vector<directed_arc>{{0, 1, 1}, {1, 0, 1}, {0, 1, 1}}));
}

TEST_CASE("single-block bridge avoids the undefined generic kernel domain",
          "[serial-kernel][bridge]") {
  auto partition = std::array<int, 3>{9, 8, 7};
  auto edgecut = -1;
  auto balance = -1.0;
  CHECK(kahip::serial_kernel::solve_trivial_single_block(
      1, partition, edgecut, balance));
  CHECK(partition == std::array<int, 3>{0, 0, 0});
  CHECK(edgecut == 0);
  CHECK(balance == 1.0);
  CHECK_FALSE(kahip::serial_kernel::solve_trivial_single_block(
      2, partition, edgecut, balance));
}
}  // namespace
