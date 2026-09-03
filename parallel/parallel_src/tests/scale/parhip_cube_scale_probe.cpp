#include <mpi.h>

#if defined(__linux__) || defined(__APPLE__)
#include <sys/resource.h>
#else
#error "parhip_cube_scale_probe supports getrusage peak RSS on Linux and macOS"
#endif

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <optional>
#include <ranges>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "communication/mpi_failure.h"
#include "communication/mpi_fixed_reduction.h"
#include "communication/mpi_handles.h"
#include "communication/mpi_tools.h"
#include "communication/serial_kernel_profile_observer.h"
#include "imbalance.h"
#include "parhip_interface.h"
#include "scale/cube_scale_probe_core.h"
#include "scale/cube_scale_probe_protocol.h"
#include "serial_kernel_profile.h"
#include "serial_kernel_structure.h"
#include "version.h"

namespace {
using clock_type = std::chrono::steady_clock;
using profile_type = kahip::serial_kernel::serial_kernel_profile;
using parhip::scale_probe::digest_lanes;

constexpr auto probe_version = std::uint64_t{1};
constexpr auto seed = 2022;
constexpr auto source_window = std::uint64_t{65'536};
constexpr auto profile_field_count = std::size_t{17};
constexpr auto expected_profile_count = std::size_t{2};

#if defined(__linux__)
constexpr auto platform_name = std::string_view{"Linux"};
constexpr auto peak_rss_native_unit = std::string_view{"KiB"};
constexpr auto peak_rss_byte_scale = std::uint64_t{1024};
#elif defined(__APPLE__)
constexpr auto platform_name = std::string_view{"macOS"};
constexpr auto peak_rss_native_unit = std::string_view{"bytes"};
constexpr auto peak_rss_byte_scale = std::uint64_t{1};
#endif

struct cli_options final {
  std::uint64_t side{};
  std::optional<int> expected_ranks;
};

[[nodiscard]] auto parse_positive_u64(std::string_view text)
    -> std::optional<std::uint64_t> {
  auto value = std::uint64_t{};
  auto const* first = text.data();
  auto const* end = first + text.size();
  auto const result = std::from_chars(first, end, value);
  if (text.empty() || result.ec != std::errc{} || result.ptr != end ||
      value == 0) {
    return std::nullopt;
  }
  return value;
}

[[nodiscard]] auto parse_cli(int argc, char const* const* argv)
    -> std::optional<cli_options> {
  auto result = cli_options{};
  auto has_side = false;
  auto has_expected_ranks = false;
  for (auto index = 1; index < argc;) {
    auto const option = std::string_view{argv[index++]};
    if (index == argc) {
      return std::nullopt;
    }
    auto const value = parse_positive_u64(argv[index++]);
    if (!value.has_value()) {
      return std::nullopt;
    }
    if (option == "--side" && !has_side) {
      result.side = *value;
      has_side = true;
    } else if (option == "--expected-ranks" && !has_expected_ranks &&
               *value <= static_cast<std::uint64_t>(
                             std::numeric_limits<int>::max())) {
      result.expected_ranks = static_cast<int>(*value);
      has_expected_ranks = true;
    } else {
      return std::nullopt;
    }
  }
  return has_side ? std::optional<cli_options>{result} : std::nullopt;
}

[[nodiscard]] auto agree_cli(std::optional<cli_options> const& local,
                             parhip::mpi::communicator_view communicator)
    -> std::optional<cli_options> {
  auto const signature = std::array<std::uint64_t, 4>{
      local.has_value() ? 1U : 0U,
      local.has_value() ? local->side : 0U,
      local.has_value() && local->expected_ranks.has_value() ? 1U : 0U,
      local.has_value() && local->expected_ranks.has_value()
          ? static_cast<std::uint64_t>(*local->expected_ranks)
          : 0U,
  };
  auto minimum = std::array<std::uint64_t, signature.size()>{};
  auto maximum = std::array<std::uint64_t, signature.size()>{};
  parhip::mpi::all_reduce_bounded(
      std::span<std::uint64_t const>{signature},
      std::span<std::uint64_t>{minimum}, parhip::mpi::reduction_kind::minimum,
      communicator, "MPI_Allreduce(cube CLI signature minimum)");
  parhip::mpi::all_reduce_bounded(
      std::span<std::uint64_t const>{signature},
      std::span<std::uint64_t>{maximum}, parhip::mpi::reduction_kind::maximum,
      communicator, "MPI_Allreduce(cube CLI signature maximum)");
  if (minimum != maximum) {
    parhip::mpi::abort_on_programming_error(
        communicator.native_handle(),
        "cube scale probe CLI arguments differ across communicator");
  }
  return local;
}

void print_usage(char const* executable) {
  std::cerr << "usage: " << executable
            << " --side <positive-u64> [--expected-ranks <positive-int>]\n";
}

struct profile_capture final {
  std::array<profile_type, expected_profile_count> profiles{};
  std::size_t count{};
  bool overflow{};
};

void capture_profile(void* context, profile_type const& profile) noexcept {
  auto& capture = *static_cast<profile_capture*>(context);
  auto const index = capture.count++;
  if (index < capture.profiles.size()) {
    capture.profiles[index] = profile;
  } else {
    capture.overflow = true;
  }
}

[[nodiscard]] constexpr auto profile_fields(
    profile_type const& profile) noexcept
    -> std::array<std::uint64_t, profile_field_count> {
  return {profile.global_nodes,
          profile.global_directed_edges,
          profile.total_node_weight,
          profile.maximum_node_weight,
          profile.total_directed_edge_weight,
          profile.maximum_directed_edge_weight,
          profile.block_count,
          profile.absolute_bound,
          profile.wire_record_bytes,
          profile.csr_bytes,
          profile.partition_bytes,
          profile.serial_input_bytes,
          profile.complete_graph_bytes,
          profile.structural_validation_bytes,
          profile.base_memory_bytes,
          profile.flat_payload_elements,
          static_cast<std::uint64_t>(profile.reason)};
}

[[nodiscard]] auto checked_profile_layout(profile_type const& profile) noexcept
    -> bool {
  using parhip::scale_probe::detail::checked_add;
  using parhip::scale_probe::detail::checked_multiply;
  auto const nodes_plus_one = checked_add(profile.global_nodes, 1);
  auto const node_wire = checked_multiply(
      profile.global_nodes,
      sizeof(parhip::mpi_tools_detail::complete_graph_node_record));
  auto const edge_wire = checked_multiply(
      profile.global_directed_edges,
      sizeof(parhip::mpi_tools_detail::complete_graph_edge_record));
  auto const xadj = nodes_plus_one.has_value()
                        ? checked_multiply(*nodes_plus_one, sizeof(int))
                        : std::optional<std::uint64_t>{};
  auto const adjacency =
      checked_multiply(profile.global_directed_edges, sizeof(int));
  auto const node_weights = checked_multiply(profile.global_nodes, sizeof(int));
  auto const edge_weights =
      checked_multiply(profile.global_directed_edges, sizeof(int));
  auto const partition = checked_multiply(profile.global_nodes, sizeof(int));
  auto const complete_nodes =
      nodes_plus_one.has_value()
          ? checked_multiply(*nodes_plus_one, sizeof(parhip::Node))
          : std::optional<std::uint64_t>{};
  auto const complete_node_data =
      nodes_plus_one.has_value()
          ? checked_multiply(*nodes_plus_one, sizeof(parhip::NodeData))
          : std::optional<std::uint64_t>{};
  auto const complete_edges =
      checked_multiply(profile.global_directed_edges, sizeof(parhip::Edge));
  auto const structural =
      checked_multiply(profile.global_directed_edges,
                       sizeof(kahip::serial_kernel::directed_arc));
  auto const doubled_nodes = checked_multiply(profile.global_nodes, 2);
  auto const doubled_edges = checked_multiply(profile.global_directed_edges, 2);
  if (!nodes_plus_one.has_value() || !node_wire.has_value() ||
      !edge_wire.has_value() || !xadj.has_value() || !adjacency.has_value() ||
      !node_weights.has_value() || !edge_weights.has_value() ||
      !partition.has_value() || !complete_nodes.has_value() ||
      !complete_node_data.has_value() || !complete_edges.has_value() ||
      !structural.has_value() || !doubled_nodes.has_value() ||
      !doubled_edges.has_value()) {
    return false;
  }
  auto const wire = checked_add(*node_wire, *edge_wire);
  auto csr = checked_add(*xadj, *adjacency);
  csr = csr.has_value() ? checked_add(*csr, *node_weights)
                        : std::optional<std::uint64_t>{};
  csr = csr.has_value() ? checked_add(*csr, *edge_weights)
                        : std::optional<std::uint64_t>{};
  auto const serial_input = csr.has_value() ? checked_add(*csr, *partition)
                                            : std::optional<std::uint64_t>{};
  auto complete = checked_add(*complete_nodes, *complete_node_data);
  complete = complete.has_value() ? checked_add(*complete, *complete_edges)
                                  : std::optional<std::uint64_t>{};
  auto base = wire.has_value() && complete.has_value()
                  ? checked_add(*wire, *complete)
                  : std::optional<std::uint64_t>{};
  base = base.has_value() ? checked_add(*base, *structural)
                          : std::optional<std::uint64_t>{};
  auto flat = checked_add(*doubled_nodes, *doubled_edges);
  flat =
      flat.has_value() ? checked_add(*flat, 1) : std::optional<std::uint64_t>{};
  return wire.has_value() && csr.has_value() && serial_input.has_value() &&
         complete.has_value() && base.has_value() && flat.has_value() &&
         profile.wire_record_bytes == *wire && profile.csr_bytes == *csr &&
         profile.partition_bytes == *partition &&
         profile.serial_input_bytes == *serial_input &&
         profile.complete_graph_bytes == *complete &&
         profile.structural_validation_bytes == *structural &&
         profile.base_memory_bytes == *base &&
         profile.flat_payload_elements == *flat;
}

void require_collectively(bool local_condition,
                          parhip::mpi::communicator_view communicator,
                          std::string_view context) {
  parhip::mpi::validate_collectively(local_condition, communicator, context);
}

template <std::size_t Size>
[[nodiscard]] auto checked_sum(std::array<std::uint64_t, Size> const& local,
                               parhip::mpi::communicator_view communicator,
                               std::string_view context)
    -> std::array<std::uint64_t, Size> {
  auto global = std::array<std::uint64_t, Size>{};
  parhip::mpi::all_reduce_checked_sum(
      std::span<std::uint64_t const>{local}, std::span<std::uint64_t>{global},
      communicator, context, "parhip_cube_scale_probe",
      "collective uint64 sum overflow");
  return global;
}

template <std::size_t Size>
[[nodiscard]] auto maximum(std::array<double, Size> const& local,
                           parhip::mpi::communicator_view communicator,
                           std::string_view context)
    -> std::array<double, Size> {
  auto global = std::array<double, Size>{};
  parhip::mpi::check_or_abort(
      MPI_Allreduce(local.data(), global.data(), static_cast<int>(Size),
                    MPI_DOUBLE, MPI_MAX, communicator.native_handle()),
      communicator.native_handle(), context);
  return global;
}

[[nodiscard]] auto xor_digest(digest_lanes local,
                              parhip::mpi::communicator_view communicator,
                              std::string_view context) -> digest_lanes {
  auto global = digest_lanes{};
  parhip::mpi::check_or_abort(
      MPI_Allreduce(local.values.data(), global.values.data(),
                    static_cast<int>(local.values.size()), MPI_UINT64_T,
                    MPI_BXOR, communicator.native_handle()),
      communicator.native_handle(), context);
  return global;
}

[[nodiscard]] auto digest_profiles(profile_capture const& capture)
    -> digest_lanes {
  auto result = digest_lanes{};
  for (auto index = std::size_t{0}; index < capture.profiles.size(); ++index) {
    auto fields = std::array<std::uint64_t, profile_field_count + 1>{};
    fields[0] = index;
    auto const profile = profile_fields(capture.profiles[index]);
    std::ranges::copy(profile, fields.begin() + 1);
    result ^= parhip::scale_probe::digest_record(
        parhip::scale_probe::digest_domain::profile_sequence, fields);
  }
  return result;
}

[[nodiscard]] auto json_escape(std::string_view input) -> std::string {
  auto output = std::string{};
  output.reserve(input.size());
  constexpr auto hex = std::string_view{"0123456789abcdef"};
  for (unsigned char character : input) {
    switch (character) {
      case '"':
        output += "\\\"";
        break;
      case '\\':
        output += "\\\\";
        break;
      case '\b':
        output += "\\b";
        break;
      case '\f':
        output += "\\f";
        break;
      case '\n':
        output += "\\n";
        break;
      case '\r':
        output += "\\r";
        break;
      case '\t':
        output += "\\t";
        break;
      default:
        if (character < 0x20) {
          output += "\\u00";
          output += hex[(character >> 4) & 0xf];
          output += hex[character & 0xf];
        } else {
          output.push_back(static_cast<char>(character));
        }
    }
  }
  return output;
}

[[nodiscard]] auto mpi_library_version(MPI_Comm communicator) -> std::string {
  auto storage = std::array<char, MPI_MAX_LIBRARY_VERSION_STRING>{};
  auto length = 0;
  parhip::mpi::check_or_abort(MPI_Get_library_version(storage.data(), &length),
                              communicator,
                              "MPI_Get_library_version(cube scale probe)");
  if (length < 0 || static_cast<std::size_t>(length) > storage.size()) {
    parhip::mpi::abort_on_programming_error(
        communicator, "MPI library version length is invalid");
  }
  return {storage.data(), static_cast<std::size_t>(length)};
}

[[nodiscard]] auto peak_rss_bytes(MPI_Comm communicator) -> std::uint64_t {
  auto usage = rusage{};
  if (getrusage(RUSAGE_SELF, &usage) != 0 || usage.ru_maxrss < 0) {
    parhip::mpi::abort_on_backend_failure(
        communicator, "getrusage(RUSAGE_SELF) failed for cube scale probe");
  }
  auto const kib = static_cast<std::uint64_t>(usage.ru_maxrss);
  auto const bytes =
      parhip::scale_probe::detail::checked_multiply(kib, peak_rss_byte_scale);
  if (!bytes.has_value()) {
    parhip::mpi::abort_on_capacity_failure(
        communicator, "parhip_cube_scale_probe",
        "peak RSS native-unit-to-byte conversion overflow");
  }
  return *bytes;
}

void append_profile(std::ostream& output, profile_type const& profile) {
  output << "{\"global_nodes\":" << profile.global_nodes
         << ",\"global_directed_edges\":" << profile.global_directed_edges
         << ",\"total_node_weight\":" << profile.total_node_weight
         << ",\"maximum_node_weight\":" << profile.maximum_node_weight
         << ",\"total_directed_edge_weight\":"
         << profile.total_directed_edge_weight
         << ",\"maximum_directed_edge_weight\":"
         << profile.maximum_directed_edge_weight
         << ",\"block_count\":" << profile.block_count
         << ",\"absolute_bound\":" << profile.absolute_bound
         << ",\"wire_record_bytes\":" << profile.wire_record_bytes
         << ",\"csr_bytes\":" << profile.csr_bytes
         << ",\"partition_bytes\":" << profile.partition_bytes
         << ",\"serial_input_bytes\":" << profile.serial_input_bytes
         << ",\"complete_graph_bytes\":" << profile.complete_graph_bytes
         << ",\"structural_validation_bytes\":"
         << profile.structural_validation_bytes
         << ",\"base_memory_bytes\":" << profile.base_memory_bytes
         << ",\"flat_payload_elements\":" << profile.flat_payload_elements
         << ",\"reason\":\""
         << kahip::serial_kernel::reason_name(profile.reason) << "\"}";
}

void append_digest(std::ostream& output, digest_lanes const& digest) {
  output << '[';
  auto separator = std::string_view{};
  for (auto lane : digest.values) {
    output << separator << '"' << std::hex << std::nouppercase
           << std::setfill('0') << std::setw(16) << lane << std::dec
           << std::setfill(' ') << '"';
    separator = ",";
  }
  output << ']';
}

[[nodiscard]] auto duration_seconds(clock_type::time_point begin,
                                    clock_type::time_point end) noexcept
    -> double {
  return std::chrono::duration<double>(end - begin).count();
}

struct probe_evidence final {
  cli_options options;
  int world_size;
  parhip::scale_probe::cube_counts counts;
  std::uint64_t maximum_local_nodes;
  double raw_imbalance;
  std::uint64_t raw_imbalance_bits;
  kahip::balance::normalized_imbalance normalized;
  std::uint64_t expected_bound;
  std::uint64_t total_weight;
  std::uint64_t block_weight_sum;
  std::uint64_t heaviest_block;
  std::uint64_t heaviest_weight;
  std::uint64_t sentinel_remaining;
  std::uint64_t invalid_labels;
  profile_capture profiles;
  digest_lanes profile_digest;
  std::size_t selected_profile_index;
  std::uint64_t independent_cut;
  int c_edge_cut;
  digest_lanes graph_digest;
  digest_lanes partition_digest;
  parhip::scale_probe::cut_exchange_result cut_protocol;
  std::array<double, 4> timings;
  std::uint64_t maximum_rss_bytes;
  std::string mpi_library;
};

void print_json(probe_evidence const& evidence) {
  auto output = std::ostringstream{};
  output.imbue(std::locale::classic());
  output << std::setprecision(std::numeric_limits<double>::max_digits10)
         << std::boolalpha;
#ifdef DETERMINISTIC_PARHIP
  constexpr auto deterministic = true;
#else
  constexpr auto deterministic = false;
#endif
  auto const& selected =
      evidence.profiles.profiles[evidence.selected_profile_index];
  output << "{\"schema\":\"parhip_cube_scale_probe.v1\""
         << ",\"status\":\"pass\""
         << ",\"probe_version\":" << probe_version << ",\"project_version\":\""
         << json_escape(KAHIP_SCALE_PROBE_PROJECT_VERSION) << '"'
         << ",\"source_revision\":\""
         << json_escape(KAHIP_SCALE_PROBE_SOURCE_REVISION) << '"'
         << ",\"kahip_version\":\"" << json_escape(KAHIPVERSION) << '"'
         << ",\"compiler_id\":\"" << json_escape(KAHIP_SCALE_PROBE_COMPILER_ID)
         << '"' << ",\"compiler_version\":\""
         << json_escape(KAHIP_SCALE_PROBE_COMPILER_VERSION) << '"'
         << ",\"build_type\":\"" << json_escape(KAHIP_SCALE_PROBE_BUILD_TYPE)
         << '"' << ",\"cxx_standard\":" << KAHIP_SCALE_PROBE_CXX_STANDARD
         << ",\"mpi_standard_major\":" << MPI_VERSION
         << ",\"mpi_standard_minor\":" << MPI_SUBVERSION
         << ",\"deterministic_parhip\":" << deterministic
         << ",\"mpi_library\":\"" << json_escape(evidence.mpi_library) << '"'
         << ",\"platform\":\"" << platform_name << '"'
         << ",\"peak_rss_source\":\"getrusage(RUSAGE_SELF)\""
         << ",\"peak_rss_native_unit\":\"" << peak_rss_native_unit << '"'
         << ",\"peak_rss_output_unit\":\"bytes\""
         << ",\"distribution_recipe\":\"floor-iN-over-P\""
         << ",\"cell_id_recipe\":\"x+side*(y+side*z)\""
         << ",\"neighbor_recipe\":\"sorted-six-face-global-ids\""
         << ",\"digest_algorithm\":\"semantic-splitmix64-xor-v1\""
         << ",\"digest_collective\":\"MPI_BXOR\""
         << ",\"side\":" << evidence.options.side
         << ",\"world_ranks\":" << evidence.world_size << ",\"expected_ranks\":"
         << evidence.options.expected_ranks.value_or(evidence.world_size)
         << ",\"expected_ranks_provided\":"
         << evidence.options.expected_ranks.has_value()
         << ",\"blocks\":" << evidence.world_size << ",\"seed\":" << seed
         << ",\"mode\":\"PARHIP_FASTSOCIAL\""
         << ",\"suppress_output\":true"
         << ",\"unit_node_weights\":true"
         << ",\"unit_edge_weights\":true"
         << ",\"global_nodes\":" << evidence.counts.vertices
         << ",\"global_undirected_edges\":" << evidence.counts.undirected_edges
         << ",\"global_directed_edges\":" << evidence.counts.directed_edges
         << ",\"maximum_local_nodes\":" << evidence.maximum_local_nodes
         << ",\"local_source_window\":" << source_window
         << ",\"cut_exchange_rounds\":" << evidence.cut_protocol.rounds
         << ",\"cut_protocol_max_send\":" << evidence.cut_protocol.maximum_send
         << ",\"cut_protocol_max_receive\":"
         << evidence.cut_protocol.maximum_receive
         << ",\"raw_imbalance\":" << evidence.raw_imbalance
         << ",\"raw_imbalance_bits\":\"" << std::hex << std::nouppercase
         << std::setfill('0') << std::setw(16) << evidence.raw_imbalance_bits
         << std::dec << std::setfill(' ') << '"'
         << ",\"effective_imbalance_percent\":"
         << evidence.normalized.effective_percent
         << ",\"imbalance_was_normalized\":"
         << evidence.normalized.was_normalized
         << ",\"expected_absolute_bound\":" << evidence.expected_bound
         << ",\"total_weight\":" << evidence.total_weight
         << ",\"block_weight_sum\":" << evidence.block_weight_sum
         << ",\"heaviest_block\":" << evidence.heaviest_block
         << ",\"heaviest_weight\":" << evidence.heaviest_weight
         << ",\"sentinel_remaining\":" << evidence.sentinel_remaining
         << ",\"invalid_labels\":" << evidence.invalid_labels
         << ",\"observer_profile_count\":" << evidence.profiles.count
         << ",\"serial_kernel_profiles\":[";
  auto profile_separator = std::string_view{};
  for (auto const& profile : evidence.profiles.profiles) {
    output << profile_separator;
    append_profile(output, profile);
    profile_separator = ",";
  }
  output << "]"
         << ",\"profile_sequence_digest\":";
  append_digest(output, evidence.profile_digest);
  output << ",\"selected_profile_index\":" << evidence.selected_profile_index
         << ",\"selected_profile_global_nodes\":" << selected.global_nodes
         << ",\"selected_profile_global_directed_edges\":"
         << selected.global_directed_edges
         << ",\"selected_profile_total_node_weight\":"
         << selected.total_node_weight
         << ",\"selected_profile_maximum_node_weight\":"
         << selected.maximum_node_weight
         << ",\"selected_profile_total_directed_edge_weight\":"
         << selected.total_directed_edge_weight
         << ",\"selected_profile_maximum_directed_edge_weight\":"
         << selected.maximum_directed_edge_weight
         << ",\"selected_profile_block_count\":" << selected.block_count
         << ",\"selected_profile_absolute_bound\":" << selected.absolute_bound
         << ",\"selected_profile_wire_record_bytes\":"
         << selected.wire_record_bytes
         << ",\"selected_profile_csr_bytes\":" << selected.csr_bytes
         << ",\"selected_profile_partition_bytes\":" << selected.partition_bytes
         << ",\"selected_profile_serial_input_bytes\":"
         << selected.serial_input_bytes
         << ",\"selected_profile_complete_graph_bytes\":"
         << selected.complete_graph_bytes
         << ",\"selected_profile_structural_validation_bytes\":"
         << selected.structural_validation_bytes
         << ",\"selected_profile_base_memory_bytes\":"
         << selected.base_memory_bytes
         << ",\"selected_profile_flat_payload_elements\":"
         << selected.flat_payload_elements << ",\"selected_profile_reason\":\""
         << kahip::serial_kernel::reason_name(selected.reason) << '"'
         << ",\"independent_cut\":" << evidence.independent_cut
         << ",\"c_edge_cut\":" << evidence.c_edge_cut << ",\"graph_digest\":";
  append_digest(output, evidence.graph_digest);
  output << ",\"partition_digest\":";
  append_digest(output, evidence.partition_digest);
  output << ",\"generation_seconds\":" << evidence.timings[0]
         << ",\"partition_seconds\":" << evidence.timings[1]
         << ",\"validation_seconds\":" << evidence.timings[2]
         << ",\"elapsed_seconds\":" << evidence.timings[3]
         << ",\"max_rank_rss_bytes\":" << evidence.maximum_rss_bytes << '}';
  if (!output.good()) {
    throw std::runtime_error{"cube scale probe JSON construction failed"};
  }
  std::cout << output.str() << '\n';
  std::cout.flush();
  if (!std::cout.good()) {
    throw std::runtime_error{"cube scale probe JSON output failed"};
  }
}

[[nodiscard]] auto run_probe(cli_options const& options,
                             parhip::mpi::communicator_view communicator)
    -> probe_evidence {
  auto const rank = communicator.rank();
  auto const size = communicator.size();
  if (options.expected_ranks.has_value() && *options.expected_ranks != size) {
    parhip::mpi::abort_on_programming_error(
        communicator.native_handle(),
        "cube scale probe --expected-ranks differs from MPI world size");
  }
  if (size <= 0 || static_cast<std::uint64_t>(size) >
                       std::numeric_limits<std::uint32_t>::max()) {
    parhip::mpi::abort_on_capacity_failure(
        communicator.native_handle(), "parhip_cube_scale_probe",
        "MPI world size exceeds the balanced-boundary domain");
  }
  auto const counts = parhip::scale_probe::counts_for_side(options.side);
  if (!counts.has_value()) {
    parhip::mpi::abort_on_capacity_failure(
        communicator.native_handle(), "parhip_cube_scale_probe",
        "cube counts exceed the uint64 domain");
  }
  auto const parts = static_cast<std::uint32_t>(size);
  auto const maximum_local_nodes =
      parhip::scale_probe::maximum_balanced_slice(counts->vertices, parts);
  auto const expected_bound =
      parhip::scale_probe::exact_unit_weight_bound(counts->vertices, parts, 3);
  if (!maximum_local_nodes.has_value() || !expected_bound.has_value()) {
    parhip::mpi::abort_on_capacity_failure(
        communicator.native_handle(), "parhip_cube_scale_probe",
        "cube balance arithmetic exceeds the uint64 domain");
  }
  auto const raw_imbalance = static_cast<double>(float{0.03F});
  auto const normalized =
      kahip::balance::normalize_fractional_imbalance(raw_imbalance);
  require_collectively(normalized.has_value() &&
                           normalized->effective_percent == 3 &&
                           normalized->was_normalized,
                       communicator,
                       "cube scale probe widened-float imbalance did not "
                       "normalize to 3 percent");

  parhip::mpi::check_or_abort(MPI_Barrier(communicator.native_handle()),
                              communicator.native_handle(),
                              "MPI_Barrier(cube scale probe start)");
  auto const total_start = clock_type::now();
  auto const generation_start = total_start;
  auto boundaries =
      std::vector<std::uint64_t>(static_cast<std::size_t>(size) + 1);
  if (!parhip::scale_probe::write_balanced_boundaries(counts->vertices,
                                                      boundaries)) {
    parhip::mpi::abort_on_capacity_failure(
        communicator.native_handle(), "parhip_cube_scale_probe",
        "balanced cube boundaries are not representable");
  }
  auto distribution = std::vector<idxtype>(boundaries.size());
  std::ranges::transform(boundaries, distribution.begin(), [](std::uint64_t x) {
    return static_cast<idxtype>(x);
  });
  auto const first = boundaries[static_cast<std::size_t>(rank)];
  auto const end = boundaries[static_cast<std::size_t>(rank) + 1];
  auto graph =
      parhip::scale_probe::build_local_cube_csr(options.side, first, end);
  auto partition = std::vector<idxtype>(static_cast<std::size_t>(end - first),
                                        std::numeric_limits<idxtype>::max());
  auto const local_graph_digest =
      parhip::scale_probe::graph_digest(options.side, first, end);
  auto const generation_end = clock_type::now();

  auto blocks = size;
  auto mutable_imbalance = raw_imbalance;
  auto c_edge_cut = -1;
  auto mutable_communicator = communicator.native_handle();
  auto capture = profile_capture{};
  auto const partition_start = clock_type::now();
  {
    auto observer =
        parhip::mpi_tools_detail::scoped_serial_kernel_profile_observer{
            capture_profile, &capture};
    ParHIPPartitionKWay(distribution.data(), graph.offsets.data(),
                        graph.targets.empty() ? nullptr : graph.targets.data(),
                        nullptr, nullptr, &blocks, &mutable_imbalance, true,
                        seed, PARHIP_FASTSOCIAL, &c_edge_cut,
                        partition.empty() ? nullptr : partition.data(),
                        &mutable_communicator);
  }
  auto const partition_end = clock_type::now();
  auto const validation_start = partition_end;

  auto const local_counts = std::array<std::uint64_t, 2>{
      end - first, static_cast<std::uint64_t>(graph.targets.size())};
  auto const global_counts = checked_sum(
      local_counts, communicator, "MPI_Allreduce(cube generated counts)");
  require_collectively(global_counts[0] == counts->vertices &&
                           global_counts[1] == counts->directed_edges,
                       communicator,
                       "generated cube totals differ from exact counts");

  auto local_sentinel = std::uint64_t{0};
  auto local_invalid = std::uint64_t{0};
  for (auto label : partition) {
    local_sentinel += label == std::numeric_limits<idxtype>::max() ? 1 : 0;
    local_invalid += label >= static_cast<idxtype>(size) ? 1 : 0;
  }
  auto const label_failures = checked_sum(
      std::array<std::uint64_t, 2>{local_sentinel, local_invalid}, communicator,
      "MPI_Allreduce(cube partition label validation)");
  require_collectively(label_failures[0] == 0 && label_failures[1] == 0,
                       communicator,
                       "cube partition has sentinel or out-of-domain labels");

  auto local_block_weights =
      std::vector<std::uint64_t>(static_cast<std::size_t>(size), 0);
  for (auto label : partition) {
    auto& weight = local_block_weights[static_cast<std::size_t>(label)];
    if (weight == std::numeric_limits<std::uint64_t>::max()) {
      parhip::mpi::abort_on_capacity_failure(
          communicator.native_handle(), "parhip_cube_scale_probe",
          "local block weight exceeds uint64");
    }
    ++weight;
  }
  auto global_block_weights =
      std::vector<std::uint64_t>(local_block_weights.size());
  parhip::mpi::all_reduce_checked_sum(
      std::span<std::uint64_t const>{local_block_weights},
      std::span<std::uint64_t>{global_block_weights}, communicator,
      "MPI_Allreduce(cube block weights)", "parhip_cube_scale_probe",
      "global block weight exceeds uint64");
  auto block_weight_sum = std::uint64_t{0};
  auto heaviest_block = std::size_t{0};
  auto heaviest_weight = global_block_weights.front();
  auto block_weights_are_valid = true;
  for (auto block = std::size_t{0}; block < global_block_weights.size();
       ++block) {
    auto const next = parhip::scale_probe::detail::checked_add(
        block_weight_sum, global_block_weights[block]);
    block_weights_are_valid = block_weights_are_valid && next.has_value() &&
                              global_block_weights[block] <= *expected_bound;
    if (next.has_value()) {
      block_weight_sum = *next;
    }
    if (global_block_weights[block] > heaviest_weight) {
      heaviest_block = block;
      heaviest_weight = global_block_weights[block];
    }
  }
  require_collectively(
      block_weights_are_valid && block_weight_sum == counts->vertices,
      communicator, "cube partition weight or exact 3-percent bound failed");

  auto packed_profiles =
      std::array<std::uint64_t, expected_profile_count * profile_field_count>{};
  auto profiles_are_valid =
      capture.count == expected_profile_count && !capture.overflow;
  for (auto index = std::size_t{0}; index < capture.profiles.size(); ++index) {
    auto const fields = profile_fields(capture.profiles[index]);
    std::ranges::copy(fields,
                      packed_profiles.begin() +
                          static_cast<std::ptrdiff_t>(index * fields.size()));
    auto const& profile = capture.profiles[index];
    profiles_are_valid =
        profiles_are_valid && profile.safe() &&
        profile.total_node_weight == counts->vertices &&
        profile.block_count == static_cast<std::uint64_t>(size) &&
        profile.absolute_bound == *expected_bound && profile.global_nodes > 0 &&
        profile.global_nodes <= counts->vertices &&
        profile.global_directed_edges <= counts->directed_edges &&
        profile.maximum_node_weight <= *expected_bound &&
        profile.total_directed_edge_weight <= counts->directed_edges &&
        checked_profile_layout(profile);
  }
  auto minimum_profiles = decltype(packed_profiles){};
  auto maximum_profiles = decltype(packed_profiles){};
  parhip::mpi::all_reduce_bounded(
      std::span<std::uint64_t const>{packed_profiles},
      std::span<std::uint64_t>{minimum_profiles},
      parhip::mpi::reduction_kind::minimum, communicator,
      "MPI_Allreduce(cube profile field minimum)");
  parhip::mpi::all_reduce_bounded(
      std::span<std::uint64_t const>{packed_profiles},
      std::span<std::uint64_t>{maximum_profiles},
      parhip::mpi::reduction_kind::maximum, communicator,
      "MPI_Allreduce(cube profile field maximum)");
  require_collectively(
      profiles_are_valid && minimum_profiles == maximum_profiles &&
          minimum_profiles == packed_profiles,
      communicator, "cube observer profiles are invalid or disagree");
  auto selected_profile_index = std::size_t{0};
  for (auto index = std::size_t{1}; index < capture.profiles.size(); ++index) {
    if (capture.profiles[index].base_memory_bytes >
        capture.profiles[selected_profile_index].base_memory_bytes) {
      selected_profile_index = index;
    }
  }
  auto const profile_digest = digest_profiles(capture);

  auto const cut_protocol = parhip::scale_probe::independent_cut(
      options.side, graph, partition, boundaries,
      static_cast<std::uint64_t>(size), communicator.native_handle());
  require_collectively(
      cut_protocol.cut <=
              static_cast<std::uint64_t>(std::numeric_limits<int>::max()) &&
          c_edge_cut >= 0 &&
          cut_protocol.cut == static_cast<std::uint64_t>(c_edge_cut),
      communicator, "independent cube cut differs from the public C cut");

  auto const graph_digest = xor_digest(local_graph_digest, communicator,
                                       "MPI_Allreduce(cube graph digest BXOR)");
  auto const local_partition_digest = parhip::scale_probe::partition_digest(
      options.side, first, std::span<idxtype const>{partition},
      static_cast<std::uint64_t>(size));
  auto const partition_digest =
      xor_digest(local_partition_digest, communicator,
                 "MPI_Allreduce(cube partition digest BXOR)");
  auto const validation_end = clock_type::now();
  auto mpi_library = mpi_library_version(communicator.native_handle());
  auto const local_rss = std::array<std::uint64_t, 1>{
      peak_rss_bytes(communicator.native_handle())};
  parhip::mpi::check_or_abort(MPI_Barrier(communicator.native_handle()),
                              communicator.native_handle(),
                              "MPI_Barrier(cube probe logical completion)");
  auto const total_end = clock_type::now();
  auto const timing_maxima = maximum(
      std::array<double, 4>{
          duration_seconds(generation_start, generation_end),
          duration_seconds(partition_start, partition_end),
          duration_seconds(validation_start, validation_end),
          duration_seconds(total_start, total_end),
      },
      communicator, "MPI_Allreduce(cube probe timing maxima)");
  auto maximum_rss = std::array<std::uint64_t, 1>{};
  parhip::mpi::all_reduce_bounded(std::span<std::uint64_t const>{local_rss},
                                  std::span<std::uint64_t>{maximum_rss},
                                  parhip::mpi::reduction_kind::maximum,
                                  communicator,
                                  "MPI_Allreduce(cube probe maximum RSS)");

  return probe_evidence{
      .options = options,
      .world_size = size,
      .counts = *counts,
      .maximum_local_nodes = *maximum_local_nodes,
      .raw_imbalance = raw_imbalance,
      .raw_imbalance_bits = std::bit_cast<std::uint64_t>(raw_imbalance),
      .normalized = *normalized,
      .expected_bound = *expected_bound,
      .total_weight = global_counts[0],
      .block_weight_sum = block_weight_sum,
      .heaviest_block = heaviest_block,
      .heaviest_weight = heaviest_weight,
      .sentinel_remaining = label_failures[0],
      .invalid_labels = label_failures[1],
      .profiles = capture,
      .profile_digest = profile_digest,
      .selected_profile_index = selected_profile_index,
      .independent_cut = cut_protocol.cut,
      .c_edge_cut = c_edge_cut,
      .graph_digest = graph_digest,
      .partition_digest = partition_digest,
      .cut_protocol = cut_protocol,
      .timings = timing_maxima,
      .maximum_rss_bytes = maximum_rss[0],
      .mpi_library = std::move(mpi_library),
  };
}
}  // namespace

int main(int argc, char** argv) {
  if (MPI_Init(&argc, &argv) != MPI_SUCCESS) {
    return EXIT_FAILURE;
  }
  parhip::mpi::check_or_abort(
      MPI_Comm_set_errhandler(MPI_COMM_WORLD, MPI_ERRORS_RETURN),
      MPI_COMM_WORLD, "MPI_Comm_set_errhandler(cube scale probe)");
  auto const communicator = parhip::mpi::communicator_view{MPI_COMM_WORLD};
  auto const rank = communicator.rank();
  auto const options = agree_cli(parse_cli(argc, argv), communicator);
  if (!options.has_value()) {
    if (rank == 0) {
      print_usage(argc > 0 ? argv[0] : "parhip_cube_scale_probe");
    }
    return MPI_Finalize() == MPI_SUCCESS ? 2 : EXIT_FAILURE;
  }

  auto evidence = std::optional<probe_evidence>{};
  parhip::mpi::run_with_exception_barrier(
      [&] { evidence.emplace(run_probe(*options, communicator)); },
      [](std::exception_ptr failure) noexcept {
        parhip::mpi::abort_on_exception(MPI_COMM_WORLD,
                                        "parhip_cube_scale_probe", failure);
      });
  parhip::mpi::run_with_exception_barrier(
      [&] {
        if (rank == 0) {
          print_json(*evidence);
        }
      },
      [](std::exception_ptr failure) noexcept {
        parhip::mpi::abort_on_exception(
            MPI_COMM_WORLD, "parhip_cube_scale_probe result record", failure);
      });
  parhip::mpi::check_or_abort(
      MPI_Barrier(MPI_COMM_WORLD), MPI_COMM_WORLD,
      "MPI_Barrier(cube scale probe after result record)");
  return MPI_Finalize() == MPI_SUCCESS ? EXIT_SUCCESS : EXIT_FAILURE;
}
