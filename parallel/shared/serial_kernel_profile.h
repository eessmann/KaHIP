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
  std::uint64_t xadj_elements = std::numeric_limits<std::size_t>::max();
  std::uint64_t adjncy_elements = std::numeric_limits<std::size_t>::max();
  std::uint64_t node_weight_elements =
      std::numeric_limits<std::size_t>::max();
  std::uint64_t edge_weight_elements =
      std::numeric_limits<std::size_t>::max();
  std::uint64_t partition_elements =
      std::numeric_limits<std::size_t>::max();
  std::uint64_t flat_payload_elements =
      std::numeric_limits<std::size_t>::max();
  std::uint64_t structural_validation_elements =
      std::numeric_limits<std::size_t>::max();
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
  std::uint64_t structural_validation_arc_bytes = 3 * sizeof(std::uint64_t);
  // These remain size_t limits in production. Separate fields make the pure
  // accounting stages independently testable without changing that policy.
  std::uint64_t node_wire_byte_limit = std::numeric_limits<std::size_t>::max();
  std::uint64_t edge_wire_byte_limit = std::numeric_limits<std::size_t>::max();
  std::uint64_t wire_record_byte_limit = std::numeric_limits<std::size_t>::max();
  std::uint64_t xadj_byte_limit = std::numeric_limits<std::size_t>::max();
  std::uint64_t adjacency_byte_limit = std::numeric_limits<std::size_t>::max();
  std::uint64_t node_weight_byte_limit = std::numeric_limits<std::size_t>::max();
  std::uint64_t edge_weight_byte_limit = std::numeric_limits<std::size_t>::max();
  std::uint64_t csr_byte_limit = std::numeric_limits<std::size_t>::max();
  std::uint64_t partition_byte_limit = std::numeric_limits<std::size_t>::max();
  std::uint64_t serial_input_byte_limit = std::numeric_limits<std::size_t>::max();
  std::uint64_t complete_node_byte_limit = std::numeric_limits<std::size_t>::max();
  std::uint64_t complete_node_data_byte_limit = std::numeric_limits<std::size_t>::max();
  std::uint64_t complete_edge_byte_limit = std::numeric_limits<std::size_t>::max();
  std::uint64_t complete_graph_byte_limit = std::numeric_limits<std::size_t>::max();
  std::uint64_t structural_validation_byte_limit =
      std::numeric_limits<std::size_t>::max();
  std::uint64_t base_memory_byte_limit = std::numeric_limits<std::size_t>::max();

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
  std::uint64_t structural_validation_bytes{};
  std::uint64_t base_memory_bytes{};
  std::uint64_t flat_payload_elements{};
  profile_reason reason = profile_reason::none;

  [[nodiscard]] constexpr auto safe() const noexcept -> bool {
    return reason == profile_reason::none;
  }
};

enum class byte_accounting_stage : std::uint64_t {
  none,
  nodes_plus_one,
  node_wire,
  edge_wire,
  wire_record,
  xadj,
  adjacency,
  node_weight,
  edge_weight,
  csr,
  partition,
  serial_input,
  complete_node,
  complete_node_data,
  complete_edge,
  complete_graph,
  structural_validation,
  base_memory,
};

struct profile_byte_accounting final {
  std::uint64_t nodes_plus_one{};
  std::uint64_t node_wire_bytes{};
  std::uint64_t edge_wire_bytes{};
  std::uint64_t wire_record_bytes{};
  std::uint64_t xadj_bytes{};
  std::uint64_t adjacency_bytes{};
  std::uint64_t node_weight_bytes{};
  std::uint64_t edge_weight_bytes{};
  std::uint64_t csr_bytes{};
  std::uint64_t partition_bytes{};
  std::uint64_t serial_input_bytes{};
  std::uint64_t complete_node_bytes{};
  std::uint64_t complete_node_data_bytes{};
  std::uint64_t complete_edge_bytes{};
  std::uint64_t complete_graph_bytes{};
  std::uint64_t structural_validation_bytes{};
  std::uint64_t base_memory_bytes{};
  byte_accounting_stage stage = byte_accounting_stage::none;

  [[nodiscard]] constexpr auto safe() const noexcept -> bool {
    return stage == byte_accounting_stage::none;
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

[[nodiscard]] constexpr auto account_profile_bytes(
    profile_input const& input,
    profile_limits const& limits = {}) noexcept -> profile_byte_accounting {
  auto accounting = profile_byte_accounting{};
  auto const stage_limit = [&limits](std::uint64_t limit) constexpr {
    return limit < limits.size_limit ? limit : limits.size_limit;
  };
  auto const multiply = [&accounting, &stage_limit](
                            byte_accounting_stage stage,
                            std::uint64_t left,
                            std::uint64_t right,
                            std::uint64_t limit,
                            std::uint64_t& result) constexpr {
    if (!checked_multiply(left, right, stage_limit(limit), result)) {
      accounting.stage = stage;
      return false;
    }
    return true;
  };
  auto const add = [&accounting, &stage_limit](byte_accounting_stage stage,
                                                std::uint64_t left,
                                                std::uint64_t right,
                                                std::uint64_t limit,
                                                std::uint64_t& result) constexpr {
    if (!checked_add(left, right, stage_limit(limit), result)) {
      accounting.stage = stage;
      return false;
    }
    return true;
  };

  if (!add(byte_accounting_stage::nodes_plus_one, input.global_nodes, 1,
           limits.size_limit, accounting.nodes_plus_one) ||
      !multiply(byte_accounting_stage::node_wire, input.global_nodes,
                limits.wire_node_bytes, limits.node_wire_byte_limit,
                accounting.node_wire_bytes) ||
      !multiply(byte_accounting_stage::edge_wire, input.global_directed_edges,
                limits.wire_edge_bytes, limits.edge_wire_byte_limit,
                accounting.edge_wire_bytes) ||
      !add(byte_accounting_stage::wire_record, accounting.node_wire_bytes,
           accounting.edge_wire_bytes, limits.wire_record_byte_limit,
           accounting.wire_record_bytes) ||
      !multiply(byte_accounting_stage::xadj, accounting.nodes_plus_one,
                sizeof(int), limits.xadj_byte_limit, accounting.xadj_bytes) ||
      !multiply(byte_accounting_stage::adjacency, input.global_directed_edges,
                sizeof(int), limits.adjacency_byte_limit,
                accounting.adjacency_bytes) ||
      !multiply(byte_accounting_stage::node_weight, input.global_nodes,
                sizeof(int), limits.node_weight_byte_limit,
                accounting.node_weight_bytes) ||
      !multiply(byte_accounting_stage::edge_weight,
                input.global_directed_edges, sizeof(int),
                limits.edge_weight_byte_limit, accounting.edge_weight_bytes) ||
      !add(byte_accounting_stage::csr, accounting.xadj_bytes,
           accounting.adjacency_bytes, limits.csr_byte_limit,
           accounting.csr_bytes) ||
      !add(byte_accounting_stage::csr, accounting.csr_bytes,
           accounting.node_weight_bytes, limits.csr_byte_limit,
           accounting.csr_bytes) ||
      !add(byte_accounting_stage::csr, accounting.csr_bytes,
           accounting.edge_weight_bytes, limits.csr_byte_limit,
           accounting.csr_bytes) ||
      !multiply(byte_accounting_stage::partition, input.global_nodes,
                sizeof(int), limits.partition_byte_limit,
                accounting.partition_bytes) ||
      !add(byte_accounting_stage::serial_input, accounting.csr_bytes,
           accounting.partition_bytes, limits.serial_input_byte_limit,
           accounting.serial_input_bytes) ||
      !multiply(byte_accounting_stage::complete_node,
                accounting.nodes_plus_one, limits.complete_node_bytes,
                limits.complete_node_byte_limit, accounting.complete_node_bytes) ||
      !multiply(byte_accounting_stage::complete_node_data,
                accounting.nodes_plus_one, limits.complete_node_data_bytes,
                limits.complete_node_data_byte_limit,
                accounting.complete_node_data_bytes) ||
      !multiply(byte_accounting_stage::complete_edge,
                input.global_directed_edges, limits.complete_edge_bytes,
                limits.complete_edge_byte_limit, accounting.complete_edge_bytes) ||
      !multiply(byte_accounting_stage::structural_validation,
                input.global_directed_edges,
                limits.structural_validation_arc_bytes,
                limits.structural_validation_byte_limit,
                accounting.structural_validation_bytes) ||
      !add(byte_accounting_stage::complete_graph,
           accounting.complete_node_bytes, accounting.complete_node_data_bytes,
           limits.complete_graph_byte_limit, accounting.complete_graph_bytes) ||
      !add(byte_accounting_stage::complete_graph, accounting.complete_graph_bytes,
           accounting.complete_edge_bytes, limits.complete_graph_byte_limit,
           accounting.complete_graph_bytes) ||
      !add(byte_accounting_stage::base_memory, accounting.wire_record_bytes,
           accounting.complete_graph_bytes, limits.base_memory_byte_limit,
           accounting.base_memory_bytes) ||
      !add(byte_accounting_stage::base_memory, accounting.base_memory_bytes,
           accounting.structural_validation_bytes, limits.base_memory_byte_limit,
           accounting.base_memory_bytes)) {
    return accounting;
  }
  return accounting;
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
  auto quotient_left = input.block_count;
  auto quotient_right = input.block_count == 0 ? 0 : input.block_count - 1;
  if (quotient_left % 2 == 0) {
    quotient_left /= 2;
  } else {
    quotient_right /= 2;
  }
  auto quotient_pair_count = std::uint64_t{};
  auto scheduler_twice = std::uint64_t{};
  auto scheduler_factor_ok =
      input.bank_factor_twice > 0 &&
      checked_multiply(quotient_left, quotient_right,
                       std::numeric_limits<std::uint64_t>::max(),
                       quotient_pair_count);
  auto const quotient_edge_max =
      input.global_directed_edges / 2 < quotient_pair_count
          ? input.global_directed_edges / 2
          : quotient_pair_count;
  scheduler_factor_ok =
      scheduler_factor_ok &&
      checked_multiply(input.bank_factor_twice, quotient_edge_max,
                       std::numeric_limits<std::uint64_t>::max(),
                       scheduler_twice) &&
      scheduler_twice / 2 + scheduler_twice % 2 <= limits.int_max;
  if (!scheduler_factor_ok) {
    fail(profile_reason::quotient_scheduler_domain_out_of_range);
  }
  if (!input.labels_are_valid) {
    fail(profile_reason::vcycle_labels_out_of_range);
  }

  auto const byte_accounting = account_profile_bytes(input, limits);
  profile.wire_record_bytes = byte_accounting.wire_record_bytes;
  profile.csr_bytes = byte_accounting.csr_bytes;
  profile.partition_bytes = byte_accounting.partition_bytes;
  profile.serial_input_bytes = byte_accounting.serial_input_bytes;
  profile.complete_graph_bytes = byte_accounting.complete_graph_bytes;
  profile.structural_validation_bytes =
      byte_accounting.structural_validation_bytes;
  profile.base_memory_bytes = byte_accounting.base_memory_bytes;
  auto doubled_nodes = std::uint64_t{};
  auto doubled_edges = std::uint64_t{};

  auto const arithmetic =
      byte_accounting.safe() &&
      checked_multiply(input.global_nodes, 2, limits.size_limit,
                       doubled_nodes) &&
      checked_multiply(input.global_directed_edges, 2, limits.size_limit,
                       doubled_edges) &&
      checked_add(doubled_nodes, doubled_edges, limits.size_limit,
                  profile.flat_payload_elements) &&
      checked_add(profile.flat_payload_elements, 1, limits.size_limit,
                  profile.flat_payload_elements);
  if (!arithmetic) {
    fail(profile_reason::byte_count_overflow);
  }

  if (byte_accounting.nodes_plus_one > limits.xadj_elements ||
      input.global_directed_edges > limits.adjncy_elements ||
      input.global_nodes > limits.node_weight_elements ||
      input.global_directed_edges > limits.edge_weight_elements ||
      input.global_nodes > limits.partition_elements ||
      profile.flat_payload_elements > limits.flat_payload_elements ||
      input.global_directed_edges > limits.structural_validation_elements ||
      input.global_nodes > limits.wire_node_elements ||
      input.global_directed_edges > limits.wire_edge_elements ||
      byte_accounting.nodes_plus_one > limits.complete_node_elements ||
      byte_accounting.nodes_plus_one > limits.complete_node_data_elements ||
      input.global_directed_edges > limits.complete_edge_elements) {
    fail(profile_reason::vector_capacity_exceeded);
  }
  return profile;
}
}  // namespace kahip::serial_kernel

#endif
