#ifndef KAHIP_SERIAL_KERNEL_PROFILE_H
#define KAHIP_SERIAL_KERNEL_PROFILE_H

#include <cstddef>
#include <cstdint>
#include <limits>

namespace kahip::serial_kernel {
enum class profile_reason : std::uint64_t {
  none,
  global_node_count_mismatch,
  global_directed_edge_count_mismatch,
  global_node_count_out_of_range,
  global_directed_edge_count_out_of_range,
  csr_offset_out_of_range,
  target_out_of_range,
  node_weight_out_of_range,
  edge_weight_out_of_range,
  total_node_weight_out_of_range,
  absolute_bound_out_of_range,
  total_directed_edge_weight_out_of_range,
  block_count_out_of_range,
  vcycle_labels_out_of_range,
  byte_count_overflow,
  vector_capacity_exceeded,
  collective_aggregate_overflow,
  collective_configuration_mismatch,
  stop_rule_domain_out_of_range,
  quotient_scheduler_domain_out_of_range,
};

struct profile_input final {
  std::uint64_t global_nodes{};
  std::uint64_t global_directed_edges{};
  std::uint64_t total_node_weight{};
  std::uint64_t maximum_node_weight{};
  std::uint64_t total_directed_edge_weight{};
  std::uint64_t maximum_directed_edge_weight{};
  std::uint64_t block_count{};
  std::uint64_t absolute_bound{};
  bool csr_offsets_are_valid = true;
  bool targets_are_valid = true;
  bool labels_are_valid = true;
  bool social_mode = false;
  std::uint64_t bank_factor_twice = 3;
};

struct profile_limits final {
  std::uint64_t modified_node_weight_max =
      std::numeric_limits<unsigned>::max();
  std::uint64_t int_max = std::numeric_limits<int>::max();
  std::uint64_t size_limit = std::numeric_limits<std::size_t>::max();
  std::uint64_t int_elements = std::numeric_limits<std::size_t>::max();
  std::uint64_t wire_node_elements = std::numeric_limits<std::size_t>::max();
  std::uint64_t wire_edge_elements = std::numeric_limits<std::size_t>::max();
  std::uint64_t complete_node_elements =
      std::numeric_limits<std::size_t>::max();
  std::uint64_t complete_node_data_elements =
      std::numeric_limits<std::size_t>::max();
  std::uint64_t complete_edge_elements =
      std::numeric_limits<std::size_t>::max();
  std::uint64_t wire_node_bytes = 32;
  std::uint64_t wire_edge_bytes = 16;
  std::uint64_t complete_node_bytes = 8;
  std::uint64_t complete_node_data_bytes = 24;
  std::uint64_t complete_edge_bytes = 16;

  [[nodiscard]] static constexpr auto native() noexcept -> profile_limits {
    return {};
  }
};

struct serial_kernel_profile final {
  std::uint64_t global_nodes{};
  std::uint64_t global_directed_edges{};
  std::uint64_t total_node_weight{};
  std::uint64_t maximum_node_weight{};
  std::uint64_t total_directed_edge_weight{};
  std::uint64_t maximum_directed_edge_weight{};
  std::uint64_t block_count{};
  std::uint64_t absolute_bound{};
  std::uint64_t wire_record_bytes{};
  std::uint64_t csr_bytes{};
  std::uint64_t partition_bytes{};
  std::uint64_t serial_input_bytes{};
  std::uint64_t complete_graph_bytes{};
  std::uint64_t base_memory_bytes{};
  profile_reason reason = profile_reason::none;

  [[nodiscard]] constexpr auto safe() const noexcept -> bool {
    return reason == profile_reason::none;
  }
};

[[nodiscard]] constexpr auto reason_name(profile_reason reason) noexcept
    -> char const* {
  switch (reason) {
    case profile_reason::none: return "none";
    case profile_reason::global_node_count_mismatch: return "global-node-count-mismatch";
    case profile_reason::global_directed_edge_count_mismatch: return "global-directed-edge-count-mismatch";
    case profile_reason::global_node_count_out_of_range: return "global-node-count-out-of-range";
    case profile_reason::global_directed_edge_count_out_of_range: return "global-directed-edge-count-out-of-range";
    case profile_reason::csr_offset_out_of_range: return "csr-offset-out-of-range";
    case profile_reason::target_out_of_range: return "target-out-of-range";
    case profile_reason::node_weight_out_of_range: return "node-weight-out-of-range";
    case profile_reason::edge_weight_out_of_range: return "edge-weight-out-of-range";
    case profile_reason::total_node_weight_out_of_range: return "total-node-weight-out-of-range";
    case profile_reason::absolute_bound_out_of_range: return "absolute-bound-out-of-range";
    case profile_reason::total_directed_edge_weight_out_of_range: return "total-directed-edge-weight-out-of-range";
    case profile_reason::block_count_out_of_range: return "block-count-out-of-range";
    case profile_reason::vcycle_labels_out_of_range: return "vcycle-labels-out-of-range";
    case profile_reason::byte_count_overflow: return "byte-count-overflow";
    case profile_reason::vector_capacity_exceeded: return "vector-capacity-exceeded";
    case profile_reason::collective_aggregate_overflow: return "collective-aggregate-overflow";
    case profile_reason::collective_configuration_mismatch: return "collective-configuration-mismatch";
    case profile_reason::stop_rule_domain_out_of_range: return "stop-rule-domain-out-of-range";
    case profile_reason::quotient_scheduler_domain_out_of_range: return "quotient-scheduler-domain-out-of-range";
  }
  return "unknown";
}

[[nodiscard]] constexpr auto checked_add(std::uint64_t left,
                                         std::uint64_t right,
                                         std::uint64_t limit,
                                         std::uint64_t& result) noexcept
    -> bool {
  if (left > limit || right > limit - left) {
    return false;
  }
  result = left + right;
  return true;
}

[[nodiscard]] constexpr auto checked_multiply(std::uint64_t left,
                                              std::uint64_t right,
                                              std::uint64_t limit,
                                              std::uint64_t& result) noexcept
    -> bool {
  if (left > limit || (right != 0 && left > limit / right)) {
    return false;
  }
  result = left * right;
  return true;
}

[[nodiscard]] constexpr auto make_profile(profile_input const& input,
                                           profile_limits const& limits = {})
    noexcept -> serial_kernel_profile {
  auto profile = serial_kernel_profile{
      .global_nodes = input.global_nodes,
      .global_directed_edges = input.global_directed_edges,
      .total_node_weight = input.total_node_weight,
      .maximum_node_weight = input.maximum_node_weight,
      .total_directed_edge_weight = input.total_directed_edge_weight,
      .maximum_directed_edge_weight = input.maximum_directed_edge_weight,
      .block_count = input.block_count,
      .absolute_bound = input.absolute_bound,
  };
  auto fail = [&profile](profile_reason reason) constexpr {
    if (profile.reason == profile_reason::none) {
      profile.reason = reason;
    }
  };

  if (input.global_nodes > limits.int_max) {
    fail(profile_reason::global_node_count_out_of_range);
  }
  if (input.global_directed_edges > limits.int_max) {
    fail(profile_reason::global_directed_edge_count_out_of_range);
  }
  if (!input.csr_offsets_are_valid) {
    fail(profile_reason::csr_offset_out_of_range);
  }
  if (!input.targets_are_valid) {
    fail(profile_reason::target_out_of_range);
  }
  if (input.maximum_node_weight > limits.int_max) {
    fail(profile_reason::node_weight_out_of_range);
  }
  if (input.maximum_directed_edge_weight > limits.int_max) {
    fail(profile_reason::edge_weight_out_of_range);
  }
  if (input.total_node_weight > limits.int_max ||
      input.total_node_weight > limits.modified_node_weight_max) {
    fail(profile_reason::total_node_weight_out_of_range);
  }
  if (input.absolute_bound > limits.modified_node_weight_max ||
      input.absolute_bound > limits.modified_node_weight_max / 2) {
    fail(profile_reason::absolute_bound_out_of_range);
  }
  if (input.total_directed_edge_weight > limits.int_max) {
    fail(profile_reason::total_directed_edge_weight_out_of_range);
  }
  if (input.block_count == 0 || input.block_count > limits.int_max ||
      input.block_count > input.global_nodes) {
    fail(profile_reason::block_count_out_of_range);
  }
  auto stop_product = std::uint64_t{};
  auto population_product = std::uint64_t{};
  if (!checked_multiply(input.block_count, input.social_mode ? 5000 : 60,
                        limits.modified_node_weight_max, stop_product) ||
      !checked_multiply(input.block_count, 4, limits.int_max,
                        population_product)) {
    fail(profile_reason::stop_rule_domain_out_of_range);
  }
  auto const half_edges = input.global_directed_edges / 2;
  auto const odd_edge = input.global_directed_edges % 2;
  auto scheduler_whole = std::uint64_t{};
  auto scheduler_tail = std::uint64_t{};
  auto scheduler_factor_ok = input.bank_factor_twice > 0 &&
      checked_multiply(half_edges, input.bank_factor_twice, limits.int_max,
                       scheduler_whole) &&
      checked_add(input.bank_factor_twice, 1, limits.int_max,
                  scheduler_tail) &&
      (odd_edge == 0 ||
       checked_add(scheduler_whole, scheduler_tail / 2, limits.int_max,
                   scheduler_whole));
  if (!scheduler_factor_ok) {
    fail(profile_reason::quotient_scheduler_domain_out_of_range);
  }
  if (!input.labels_are_valid) {
    fail(profile_reason::vcycle_labels_out_of_range);
  }

  auto nodes_plus_one = std::uint64_t{};
  auto node_wire_bytes = std::uint64_t{};
  auto edge_wire_bytes = std::uint64_t{};
  auto xadj_bytes = std::uint64_t{};
  auto adjacency_bytes = std::uint64_t{};
  auto node_weight_bytes = std::uint64_t{};
  auto edge_weight_bytes = std::uint64_t{};
  auto complete_nodes_bytes = std::uint64_t{};
  auto complete_node_data_bytes = std::uint64_t{};
  auto complete_edges_bytes = std::uint64_t{};
  auto serial_arrays_bytes = std::uint64_t{};

  auto const arithmetic =
      checked_add(input.global_nodes, 1, limits.size_limit, nodes_plus_one) &&
      checked_multiply(input.global_nodes, limits.wire_node_bytes,
                       limits.size_limit, node_wire_bytes) &&
      checked_multiply(input.global_directed_edges, limits.wire_edge_bytes,
                       limits.size_limit, edge_wire_bytes) &&
      checked_add(node_wire_bytes, edge_wire_bytes, limits.size_limit,
                  profile.wire_record_bytes) &&
      checked_multiply(nodes_plus_one, sizeof(int), limits.size_limit,
                       xadj_bytes) &&
      checked_multiply(input.global_directed_edges, sizeof(int),
                       limits.size_limit, adjacency_bytes) &&
      checked_multiply(input.global_nodes, sizeof(int), limits.size_limit,
                       node_weight_bytes) &&
      checked_multiply(input.global_directed_edges, sizeof(int),
                       limits.size_limit, edge_weight_bytes) &&
      checked_add(xadj_bytes, adjacency_bytes, limits.size_limit,
                  serial_arrays_bytes) &&
      checked_add(serial_arrays_bytes, node_weight_bytes, limits.size_limit,
                  serial_arrays_bytes) &&
      checked_add(serial_arrays_bytes, edge_weight_bytes, limits.size_limit,
                  profile.csr_bytes) &&
      checked_multiply(input.global_nodes, sizeof(int), limits.size_limit,
                       profile.partition_bytes) &&
      checked_add(profile.csr_bytes, profile.partition_bytes,
                  limits.size_limit, profile.serial_input_bytes) &&
      checked_multiply(nodes_plus_one, limits.complete_node_bytes,
                       limits.size_limit, complete_nodes_bytes) &&
      checked_multiply(input.global_nodes, limits.complete_node_data_bytes,
                       limits.size_limit, complete_node_data_bytes) &&
      checked_multiply(input.global_directed_edges, limits.complete_edge_bytes,
                       limits.size_limit, complete_edges_bytes) &&
      checked_add(complete_nodes_bytes, complete_node_data_bytes,
                  limits.size_limit, profile.complete_graph_bytes) &&
      checked_add(profile.complete_graph_bytes, complete_edges_bytes,
                  limits.size_limit, profile.complete_graph_bytes) &&
      checked_add(profile.wire_record_bytes, profile.complete_graph_bytes,
                  limits.size_limit, profile.base_memory_bytes);
  if (!arithmetic) {
    fail(profile_reason::byte_count_overflow);
  }

  if (nodes_plus_one > limits.int_elements ||
      input.global_nodes > limits.int_elements ||
      input.global_directed_edges > limits.int_elements ||
      input.global_nodes > limits.wire_node_elements ||
      input.global_directed_edges > limits.wire_edge_elements ||
      nodes_plus_one > limits.complete_node_elements ||
      input.global_nodes > limits.complete_node_data_elements ||
      input.global_directed_edges > limits.complete_edge_elements) {
    fail(profile_reason::vector_capacity_exceeded);
  }
  return profile;
}
}  // namespace kahip::serial_kernel

#endif
