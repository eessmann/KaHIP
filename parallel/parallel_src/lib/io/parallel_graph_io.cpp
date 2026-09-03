/******************************************************************************
 * parallel_graph_io.cpp
 * *
 * Source of KaHIP -- Karlsruhe High Quality Partitioning.
 * Christian Schulz <christian.schulz.phone@gmail.com>
 *****************************************************************************/

#define _FILE_OFFSET_BITS 64

#include <fcntl.h>
#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "communication/mpi_adapter.h"
#include "communication/mpi_failure.h"
#include "communication/mpi_fixed_broadcast.h"
#include "parallel_graph_io.h"
#include "tools/helpers.h"
namespace parhip {
const ULONG fileTypeVersionNumber = 3;
const ULONG header_count          = 3;

namespace {
using mpi::communicator_view;

class graph_file_descriptor final {
 public:
  graph_file_descriptor() noexcept = default;
  explicit graph_file_descriptor(int descriptor) noexcept
      : descriptor_(descriptor) {}
  ~graph_file_descriptor() noexcept {
    if (descriptor_ >= 0) {
      static_cast<void>(::close(descriptor_));
    }
  }

  graph_file_descriptor(graph_file_descriptor const&) = delete;
  auto operator=(graph_file_descriptor const&)
      -> graph_file_descriptor& = delete;
  graph_file_descriptor(graph_file_descriptor&& other) noexcept
      : descriptor_(std::exchange(other.descriptor_, -1)) {}
  auto operator=(graph_file_descriptor&& other) noexcept
      -> graph_file_descriptor& {
    if (this != &other) {
      if (descriptor_ >= 0) {
        static_cast<void>(::close(descriptor_));
      }
      descriptor_ = std::exchange(other.descriptor_, -1);
    }
    return *this;
  }

  [[nodiscard]] explicit operator bool() const noexcept {
    return descriptor_ >= 0;
  }
  [[nodiscard]] auto get() const noexcept -> int { return descriptor_; }
  [[nodiscard]] auto close() noexcept -> bool {
    if (descriptor_ < 0) {
      return false;
    }
    auto const descriptor = std::exchange(descriptor_, -1);
    return ::close(descriptor) == 0;
  }

 private:
  int descriptor_ = -1;
};

[[nodiscard]] auto read_graph_exact(int descriptor,
                                    std::span<std::byte> bytes,
                                    std::uint64_t offset) noexcept -> bool {
  constexpr auto maximum_offset =
      static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
  constexpr auto maximum_transfer =
      static_cast<std::size_t>(std::numeric_limits<ssize_t>::max());
  while (!bytes.empty()) {
    if (offset > maximum_offset) {
      return false;
    }
    auto const transfer = std::min(bytes.size(), maximum_transfer);
    auto const received =
        ::pread(descriptor, bytes.data(), transfer, static_cast<off_t>(offset));
    if (received < 0 && errno == EINTR) {
      continue;
    }
    if (received <= 0) {
      return false;
    }
    auto const count = static_cast<std::size_t>(received);
    bytes = bytes.subspan(count);
    offset += static_cast<std::uint64_t>(count);
  }
  return true;
}

template <typename T>
[[nodiscard]] auto read_graph_exact(int descriptor,
                                    std::span<T> values,
                                    std::uint64_t offset) noexcept -> bool {
  static_assert(std::is_trivially_copyable_v<T>);
  return read_graph_exact(descriptor, std::as_writable_bytes(values), offset);
}

[[nodiscard]] auto write_graph_exact(int descriptor,
                                     std::span<std::byte const> bytes,
                                     std::uint64_t offset) noexcept -> bool {
  constexpr auto maximum_offset =
      static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
  constexpr auto maximum_transfer =
      static_cast<std::size_t>(std::numeric_limits<ssize_t>::max());
  while (!bytes.empty()) {
    if (offset > maximum_offset) {
      return false;
    }
    auto const transfer = std::min(bytes.size(), maximum_transfer);
    auto const written = ::pwrite(descriptor, bytes.data(), transfer,
                                  static_cast<off_t>(offset));
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written <= 0) {
      return false;
    }
    auto const count = static_cast<std::size_t>(written);
    bytes = bytes.subspan(count);
    offset += static_cast<std::uint64_t>(count);
  }
  return true;
}

template <typename T>
[[nodiscard]] auto write_graph_exact(int descriptor,
                                     std::span<T const> values,
                                     std::uint64_t offset) noexcept -> bool {
  static_assert(std::is_trivially_copyable_v<T>);
  return write_graph_exact(descriptor, std::as_bytes(values), offset);
}

void require_graph_io_success(bool local_success,
                              communicator_view communicator,
                              std::string_view diagnostic,
                              std::string_view agreement_context) noexcept {
  auto const local = local_success ? 1 : 0;
  auto global = 0;
  mpi::check_or_abort(MPI_Allreduce(&local, &global, 1, MPI_INT, MPI_MIN,
                                    communicator.native_handle()),
                      communicator.native_handle(), agreement_context);
  if (global == 0) {
    mpi::abort_on_backend_failure(communicator.native_handle(), diagnostic);
  }
}

void require_graph_capacity(bool local_success,
                            communicator_view communicator,
                            std::string_view boundary,
                            std::string_view diagnostic) noexcept {
  auto const local = local_success ? 1 : 0;
  auto global = 0;
  mpi::check_or_abort(MPI_Allreduce(&local, &global, 1, MPI_INT, MPI_MIN,
                                    communicator.native_handle()),
                      communicator.native_handle(),
                      "MPI_Allreduce(graph I/O capacity status)");
  if (global == 0) {
    mpi::abort_on_capacity_failure(communicator.native_handle(), boundary,
                                   diagnostic);
  }
}

void validate_graph_call(parallel_graph_access& graph,
                         PEID supplied_rank,
                         PEID supplied_size,
                         communicator_view communicator) noexcept {
  int relation = MPI_UNEQUAL;
  mpi::check_or_abort(MPI_Comm_compare(graph.getCommunicator(),
                                       communicator.native_handle(), &relation),
                      communicator.native_handle(),
                      "MPI_Comm_compare(graph I/O communicator)");
  auto const local_valid = supplied_rank == communicator.rank() &&
                           supplied_size == communicator.size() &&
                           (relation == MPI_IDENT || relation == MPI_CONGRUENT);
  auto const local = local_valid ? 1 : 0;
  auto global = 0;
  mpi::check_or_abort(MPI_Allreduce(&local, &global, 1, MPI_INT, MPI_MIN,
                                    communicator.native_handle()),
                      communicator.native_handle(),
                      "MPI_Allreduce(graph I/O call validation)");
  if (global == 0) {
    mpi::abort_on_programming_error(
        communicator.native_handle(),
        "graph I/O rank, size, or communicator does not match the graph");
  }
}

void require_graph_filename(std::string_view filename,
                            communicator_view communicator,
                            std::string_view diagnostic) {
  auto size = std::uint64_t{0};
  if (communicator.rank() == ROOT) {
    size = filename.size();
  }
  mpi::broadcast_fixed(size, ROOT, communicator,
                       "MPI_Bcast(graph I/O filename size)");
  if (!std::in_range<std::size_t>(size)) {
    mpi::abort_on_capacity_failure(communicator.native_handle(),
                                   "graph I/O filename",
                                   "filename size is not representable");
  }
  auto canonical = std::string(static_cast<std::size_t>(size), '\0');
  if (communicator.rank() == ROOT) {
    std::ranges::copy(filename, canonical.begin());
  }
  mpi::broadcast_bounded(std::span<char>{canonical}, ROOT, communicator,
                         "MPI_Bcast(graph I/O filename)");
  require_graph_io_success(filename == canonical, communicator, diagnostic,
                           "MPI_Allreduce(graph I/O filename agreement)");
}

[[nodiscard]] auto graph_window(int configured,
                                communicator_view communicator) noexcept
    -> int {
  auto const local = std::max(1, std::min(configured, communicator.size()));
  auto minimum = 0;
  auto maximum = 0;
  mpi::check_or_abort(MPI_Allreduce(&local, &minimum, 1, MPI_INT, MPI_MIN,
                                    communicator.native_handle()),
                      communicator.native_handle(),
                      "MPI_Allreduce(graph I/O window minimum)");
  mpi::check_or_abort(MPI_Allreduce(&local, &maximum, 1, MPI_INT, MPI_MAX,
                                    communicator.native_handle()),
                      communicator.native_handle(),
                      "MPI_Allreduce(graph I/O window maximum)");
  if (minimum != maximum) {
    mpi::abort_on_programming_error(
        communicator.native_handle(),
        "graph I/O window differs across communicator");
  }
  return minimum;
}

[[nodiscard]] constexpr auto checked_add(ULONG left, ULONG right) noexcept
    -> std::optional<ULONG> {
  if (right > std::numeric_limits<ULONG>::max() - left) {
    return std::nullopt;
  }
  return left + right;
}

[[nodiscard]] constexpr auto checked_multiply(ULONG left, ULONG right) noexcept
    -> std::optional<ULONG> {
  if (left != 0 && right > std::numeric_limits<ULONG>::max() / left) {
    return std::nullopt;
  }
  return left * right;
}

[[nodiscard]] constexpr auto binary_adjacency_base(ULONG nodes) noexcept
    -> std::optional<ULONG> {
  auto const offset_count = checked_add(nodes, ULONG{1});
  if (!offset_count.has_value()) {
    return std::nullopt;
  }
  auto const word_count = checked_add(header_count, *offset_count);
  return word_count.has_value()
             ? checked_multiply(*word_count, ULONG{sizeof(ULONG)})
             : std::nullopt;
}

[[nodiscard]] constexpr auto distribution_boundary(NodeID nodes,
                                                   int boundary,
                                                   int size) noexcept
    -> NodeID {
  auto const divisor = static_cast<NodeID>(size);
  auto const chunk = nodes / divisor + (nodes % divisor != 0 ? 1 : 0);
  if (chunk == 0) {
    return 0;
  }
  auto const index = static_cast<NodeID>(boundary);
  if (index > nodes / chunk) {
    return nodes;
  }
  return std::min(nodes, index * chunk);
}

struct graph_interval final {
  NodeID first = 0;
  NodeID next = 0;

  [[nodiscard]] constexpr auto size() const noexcept -> NodeID {
    return next - first;
  }
  [[nodiscard]] constexpr auto inclusive_last() const noexcept -> NodeID {
    return first == next ? first : next - 1;
  }
};

[[nodiscard]] constexpr auto local_graph_interval(NodeID nodes,
                                                  int rank,
                                                  int size) noexcept
    -> graph_interval {
  return graph_interval{distribution_boundary(nodes, rank, size),
                        distribution_boundary(nodes, rank + 1, size)};
}

[[nodiscard]] auto graph_distribution(NodeID nodes, int size)
    -> std::vector<NodeID> {
  auto result = std::vector<NodeID>(static_cast<std::size_t>(size) + 1);
  for (auto boundary = 0; boundary <= size; ++boundary) {
    result[static_cast<std::size_t>(boundary)] =
        distribution_boundary(nodes, boundary, size);
  }
  return result;
}

struct graph_output_layout final {
  NodeID global_nodes = 0;
  EdgeID global_edges = 0;
};

[[nodiscard]] auto validated_graph_output_layout(parallel_graph_access& graph,
                                                 communicator_view communicator)
    -> graph_output_layout {
  auto const local = std::array<std::uint64_t, 5>{
      graph.number_of_global_nodes(), graph.number_of_global_edges(),
      graph.get_from_range(), graph.number_of_local_nodes(),
      graph.number_of_local_edges()};
  auto gathered = std::vector<std::uint64_t>(
      static_cast<std::size_t>(communicator.size()) * local.size());
  mpi::check_or_abort(
      MPI_Allgather(local.data(), static_cast<int>(local.size()), MPI_UINT64_T,
                    gathered.data(), static_cast<int>(local.size()),
                    MPI_UINT64_T, communicator.native_handle()),
      communicator.native_handle(), "MPI_Allgather(graph output layout)");
  auto next_node = std::uint64_t{0};
  auto edge_sum = std::uint64_t{0};
  auto valid = true;
  for (auto rank = 0; rank < communicator.size(); ++rank) {
    auto const offset = static_cast<std::size_t>(rank) * local.size();
    auto const rank_nodes = gathered[offset];
    auto const rank_edges = gathered[offset + 1];
    auto const rank_from = gathered[offset + 2];
    auto const rank_node_count = gathered[offset + 3];
    auto const rank_edge_count = gathered[offset + 4];
    valid = valid && rank_nodes == local[0] && rank_edges == local[1] &&
            rank_from == next_node && next_node <= local[0] &&
            rank_node_count <= local[0] - next_node &&
            rank_edge_count <= local[1] - edge_sum;
    if (valid) {
      next_node += rank_node_count;
      edge_sum += rank_edge_count;
    }
  }
  valid = valid && next_node == local[0] && edge_sum == local[1] &&
          local[1] % 2 == 0;
  if (!valid) {
    mpi::abort_on_programming_error(
        communicator.native_handle(),
        "graph output ranges and edges do not form the global graph order");
  }
  return graph_output_layout{local[0], local[1]};
}

[[nodiscard]] auto parse_unsigned_token(std::string_view token,
                                        ULONG& value) noexcept -> bool {
  if (token.empty()) {
    return false;
  }
  auto const result =
      std::from_chars(token.data(), token.data() + token.size(), value);
  return result.ec == std::errc{} && result.ptr == token.data() + token.size();
}

[[nodiscard]] auto parse_metis_header(std::string const& line,
                                      NodeID& nodes,
                                      EdgeID& undirected_edges,
                                      int& format) -> bool {
  auto input = std::istringstream{line};
  auto nodes_token = std::string{};
  auto edges_token = std::string{};
  if (!(input >> nodes_token >> edges_token) ||
      !parse_unsigned_token(nodes_token, nodes) ||
      !parse_unsigned_token(edges_token, undirected_edges)) {
    return false;
  }
  auto format_token = std::string{};
  if (input >> format_token) {
    auto const parsed = std::from_chars(
        format_token.data(), format_token.data() + format_token.size(), format);
    if (parsed.ec != std::errc{} ||
        parsed.ptr != format_token.data() + format_token.size()) {
      return false;
    }
  } else {
    format = 0;
  }
  return format == 0 || format == 1 || format == 10 || format == 11;
}

struct metis_node final {
  NodeWeight weight = 1;
  std::vector<NodeID> targets;
  std::vector<EdgeWeight> edge_weights;
};

[[nodiscard]] auto parse_metis_node(std::string const& line,
                                    int format,
                                    NodeID global_nodes,
                                    metis_node& node) -> bool {
  auto const reads_edge_weights = format == 1 || format == 11;
  auto const reads_node_weight = format == 10 || format == 11;
  auto input = std::istringstream{line};
  auto token = std::string{};
  if (reads_node_weight) {
    if (!(input >> token) || !parse_unsigned_token(token, node.weight)) {
      return false;
    }
  }
  while (input >> token) {
    auto target = NodeID{0};
    if (!parse_unsigned_token(token, target) || target == 0 ||
        target > global_nodes) {
      return false;
    }
    auto edge_weight = EdgeWeight{1};
    if (reads_edge_weights) {
      if (!(input >> token) || !parse_unsigned_token(token, edge_weight)) {
        return false;
      }
    }
    node.targets.push_back(target - 1);
    node.edge_weights.push_back(edge_weight);
  }
  return input.eof();
}

[[nodiscard]] auto collectively_sum_edges(EdgeID local_edges,
                                          communicator_view communicator,
                                          std::string_view diagnostic)
    -> EdgeID {
  auto counts =
      std::vector<std::uint64_t>(static_cast<std::size_t>(communicator.size()));
  mpi::check_or_abort(
      MPI_Allgather(&local_edges, 1, MPI_UINT64_T, counts.data(), 1,
                    MPI_UINT64_T, communicator.native_handle()),
      communicator.native_handle(), "MPI_Allgather(graph I/O edge counts)");
  auto total = EdgeID{0};
  for (auto count : counts) {
    if (count > std::numeric_limits<EdgeID>::max() - total) {
      mpi::abort_on_capacity_failure(communicator.native_handle(), diagnostic,
                                     "global edge sum is not representable");
    }
    total += count;
  }
  return total;
}

void validate_binary_offset_ranges(std::span<NodeID const> local_offsets,
                                   ULONG adjacency_base,
                                   ULONG expected_end,
                                   communicator_view communicator) noexcept {
  auto const local =
      std::array<std::uint64_t, 2>{local_offsets.front(), local_offsets.back()};
  auto gathered = std::vector<std::uint64_t>(
      static_cast<std::size_t>(communicator.size()) * local.size());
  mpi::check_or_abort(
      MPI_Allgather(local.data(), static_cast<int>(local.size()), MPI_UINT64_T,
                    gathered.data(), static_cast<int>(local.size()),
                    MPI_UINT64_T, communicator.native_handle()),
      communicator.native_handle(),
      "MPI_Allgather(binary graph offset ranges)");

  auto valid = gathered.front() == adjacency_base;
  for (auto rank = 1; valid && rank < communicator.size(); ++rank) {
    auto const previous = static_cast<std::size_t>(rank - 1) * local.size();
    auto const current = static_cast<std::size_t>(rank) * local.size();
    valid = gathered[previous + 1] == gathered[current];
  }
  valid = valid && gathered.back() == expected_end;
  require_graph_io_success(
      valid, communicator, "binary graph payload I/O failed",
      "MPI_Allreduce(binary graph offset range validation)");
}

void construct_metis_graph(parallel_graph_access& graph,
                           NodeID global_nodes,
                           EdgeID global_edges,
                           graph_interval interval,
                           std::vector<metis_node> const& nodes,
                           std::vector<NodeID>& distribution) {
  auto local_edges = EdgeID{0};
  for (auto const& node : nodes) {
    local_edges += static_cast<EdgeID>(node.targets.size());
  }
  graph.start_construction(interval.size(), local_edges, global_nodes,
                           global_edges);
  graph.set_range(interval.first, interval.inclusive_last());
  graph.set_range_array(distribution);
  for (auto const& source_data : nodes) {
    auto const source = graph.new_node();
    graph.setNodeWeight(source, source_data.weight);
    graph.setNodeLabel(source, interval.first + source);
    graph.setSecondPartitionIndex(source, 0);
    for (std::size_t edge_index = 0; edge_index < source_data.targets.size();
         ++edge_index) {
      auto const edge = graph.new_edge(source, source_data.targets[edge_index]);
      graph.setEdgeWeight(edge, source_data.edge_weights[edge_index]);
    }
  }
  graph.finish_construction();
}

[[nodiscard]] auto read_metis_graph(parallel_graph_access& graph,
                                    std::string const& filename,
                                    PEID supplied_rank,
                                    PEID supplied_size,
                                    MPI_Comm native_communicator) -> int {
  auto const borrowed = communicator_view{native_communicator};
  mpi::require_live_intracommunicator(
      borrowed, "METIS graph input requires a live intracommunicator");
  validate_graph_call(graph, supplied_rank, supplied_size, borrowed);
  auto operation = mpi::communicator{borrowed};
  auto const communicator = operation.view();
  try {
    require_graph_filename(filename, communicator, "METIS graph I/O failed");
    auto input = std::ifstream{filename};
    require_graph_io_success(static_cast<bool>(input), communicator,
                             "METIS graph I/O failed",
                             "MPI_Allreduce(METIS graph open status)");

    auto header_line = std::string{};
    auto found_header = false;
    while (std::getline(input, header_line)) {
      auto content = std::string_view{header_line};
      while (!content.empty() &&
             (content.front() == ' ' || content.front() == '\t' ||
              content.front() == '\r')) {
        content.remove_prefix(1);
      }
      if (content.empty() || content.front() == '%') {
        continue;
      }
      found_header = true;
      break;
    }

    auto global_nodes = NodeID{0};
    auto undirected_edges = EdgeID{0};
    auto format = 0;
    auto header_success =
        found_header &&
        parse_metis_header(header_line, global_nodes, undirected_edges, format);
    require_graph_io_success(header_success, communicator,
                             "METIS graph I/O failed",
                             "MPI_Allreduce(METIS graph header status)");

    auto const local_header = std::array<std::uint64_t, 3>{
        global_nodes, undirected_edges, static_cast<std::uint64_t>(format)};
    auto minimum_header = std::array<std::uint64_t, 3>{};
    auto maximum_header = std::array<std::uint64_t, 3>{};
    mpi::check_or_abort(
        MPI_Allreduce(local_header.data(), minimum_header.data(),
                      static_cast<int>(local_header.size()), MPI_UINT64_T,
                      MPI_MIN, communicator.native_handle()),
        communicator.native_handle(),
        "MPI_Allreduce(METIS graph header minimum)");
    mpi::check_or_abort(
        MPI_Allreduce(local_header.data(), maximum_header.data(),
                      static_cast<int>(local_header.size()), MPI_UINT64_T,
                      MPI_MAX, communicator.native_handle()),
        communicator.native_handle(),
        "MPI_Allreduce(METIS graph header maximum)");
    if (minimum_header != maximum_header) {
      mpi::abort_on_backend_failure(communicator.native_handle(),
                                    "METIS graph I/O failed");
    }
    if (undirected_edges > std::numeric_limits<EdgeID>::max() / 2) {
      mpi::abort_on_capacity_failure(
          communicator.native_handle(), "METIS graph input",
          "directed edge count is not representable");
    }
    auto const global_edges = undirected_edges * 2;
    auto const interval = local_graph_interval(
        global_nodes, communicator.rank(), communicator.size());
    require_graph_capacity(std::in_range<std::size_t>(interval.size()),
                           communicator, "METIS graph input",
                           "local vertex count is not representable");
    auto local_nodes =
        std::vector<metis_node>(static_cast<std::size_t>(interval.size()));

    auto global_node = NodeID{0};
    auto payload_success = true;
    auto line = std::string{};
    while (std::getline(input, line)) {
      auto content = std::string_view{line};
      while (!content.empty() &&
             (content.front() == ' ' || content.front() == '\t' ||
              content.front() == '\r')) {
        content.remove_prefix(1);
      }
      if (!content.empty() && content.front() == '%') {
        continue;
      }
      if (global_node >= global_nodes) {
        payload_success = false;
        break;
      }
      if (global_node >= interval.first && global_node < interval.next) {
        auto& node =
            local_nodes[static_cast<std::size_t>(global_node - interval.first)];
        payload_success = parse_metis_node(line, format, global_nodes, node);
        if (!payload_success) {
          break;
        }
      }
      ++global_node;
    }
    payload_success =
        payload_success && !input.bad() && global_node == global_nodes;
    require_graph_io_success(payload_success, communicator,
                             "METIS graph I/O failed",
                             "MPI_Allreduce(METIS graph payload status)");

    auto local_edges = EdgeID{0};
    for (auto const& node : local_nodes) {
      auto const count = static_cast<EdgeID>(node.targets.size());
      if (count > std::numeric_limits<EdgeID>::max() - local_edges) {
        mpi::abort_on_capacity_failure(communicator.native_handle(),
                                       "METIS graph input",
                                       "local edge count is not representable");
      }
      local_edges += count;
    }
    auto const observed_edges =
        collectively_sum_edges(local_edges, communicator, "METIS graph input");
    if (observed_edges != global_edges) {
      mpi::abort_on_backend_failure(communicator.native_handle(),
                                    "METIS graph I/O failed");
    }
    auto distribution = graph_distribution(global_nodes, communicator.size());
    construct_metis_graph(graph, global_nodes, global_edges, interval,
                          local_nodes, distribution);
    return 0;
  } catch (...) {
    mpi::abort_on_exception(communicator.native_handle(),
                            "METIS graph input failed");
  }
}
}  // namespace

parallel_graph_io::parallel_graph_io() = default;

parallel_graph_io::~parallel_graph_io() = default;

int parallel_graph_io::readGraphWeighted(PPartitionConfig& config,
                                         parallel_graph_access& graph,
                                         std::string filename,
                                         PEID rank,
                                         PEID size,
                                         MPI_Comm communicator) {
  auto const view = communicator_view{communicator};
  mpi::require_live_intracommunicator(
      view, "graph input requires a live intracommunicator");
  validate_graph_call(graph, rank, size, view);
  if (hasEnding(filename, ".graph")) {
    auto const binary_filename = filename + ".bgf";
    auto const local_binary = file_exists(binary_filename) ? 1 : 0;
    auto minimum_binary = 0;
    auto maximum_binary = 0;
    mpi::check_or_abort(MPI_Allreduce(&local_binary, &minimum_binary, 1,
                                      MPI_INT, MPI_MIN, communicator),
                        communicator,
                        "MPI_Allreduce(binary graph availability minimum)");
    mpi::check_or_abort(MPI_Allreduce(&local_binary, &maximum_binary, 1,
                                      MPI_INT, MPI_MAX, communicator),
                        communicator,
                        "MPI_Allreduce(binary graph availability maximum)");
    if (minimum_binary != maximum_binary) {
      mpi::abort_on_backend_failure(
          communicator, "binary graph availability differs across ranks");
    }
    if (minimum_binary != 0) {
      return readGraphBinary(config, graph, binary_filename, rank, size,
                             communicator);
    }
  }
  if (hasEnding(filename, ".bgf")) {
    return readGraphBinary(config, graph, std::move(filename), rank, size,
                           communicator);
  }
  return readGraphWeightedFlexible(graph, std::move(filename), rank, size,
                                   communicator);
}

int parallel_graph_io::readGraphWeightedFlexible(parallel_graph_access& graph,
                                                 std::string filename,
                                                 PEID rank,
                                                 PEID size,
                                                 MPI_Comm communicator) {
  return read_metis_graph(graph, filename, rank, size, communicator);
}

//int parallel_graph_io::readGraphWeightedMETISFast(parallel_graph_access & G,
//std::string filename,
//PEID peID, PEID comm_size, MPI_Comm communicator) {
//std::string line;

//// open file for reading
//std::ifstream in(filename.c_str());
//if (!in) {
//std::cerr << "Error opening " << filename << std::endl;
//return 1;
//}

//NodeID nmbNodes;
//EdgeID nmbEdges;

//std::getline(in,line);
////skip comments
//while( line[0] == '%' ) {
//std::getline(in, line);
//}

//int ew = 0;
//std::stringstream ss(line);
//ss >> nmbNodes;
//ss >> nmbEdges;
//ss >> ew;

//// pe p reads the lines p*ceil(n/size) to (p+1)floor(n/size) lines of that file
//ULONG from  = peID     * ceil(nmbNodes / (double)comm_size);
//ULONG to    = (peID+1) * ceil(nmbNodes / (double)comm_size) - 1;
//to = std::min(to, nmbNodes-1);

//ULONG local_no_nodes = to - from + 1;
//std::cout <<  "peID " <<  peID <<  " from " <<  from <<  " to " <<  to  <<  " amount " <<  local_no_nodes << std::endl;

//std::vector< std::vector< NodeID > > local_edge_lists;
//local_edge_lists.resize(local_no_nodes);

//ULONG counter  = 0;
//NodeID node_counter = 0;
//EdgeID edge_counter = 0;

//char *oldstr, *newstr;
//while( std::getline(in, line) ) {
//if( counter > to ) {
//break;
//}
//if (line[0] == '%') { // a comment in the file
//continue;
//}

//if( counter >= from ) {
//oldstr = &line[0];
//newstr = 0;

//for (;;) {
//NodeID target;
//target = (NodeID) strtol(oldstr, &newstr, 10);

//if (target == 0) {
//break;
//}

//oldstr = newstr;

//local_edge_lists[node_counter].push_back(target);
//edge_counter++;

//}

//node_counter++;
//}

//counter++;

//if( in.eof() ) {
//break;
//}
//}

//MPI_Barrier(communicator);

//G.start_construction(local_no_nodes, 2*edge_counter, nmbNodes, 2*nmbEdges);
//G.set_range(from, to);

//for (NodeID i = 0; i < local_no_nodes; ++i) {
//NodeID node = G.new_node();
//G.setNodeWeight(node, 1);
//G.setNodeLabel(node, from+node);
//G.setSecondPartitionIndex(node, 0);

//for( ULONG j = 0; j < local_edge_lists[i].size(); j++) {
//NodeID target = local_edge_lists[i][j]-1; // -1 since there are no nodes with id 0 in the file
//EdgeID e = G.new_edge(node, target);
//G.setEdgeWeight(e, 1);
//}
//}

//G.finish_construction();
//MPI_Barrier(communicator);
//return 0;
//}
// we start with the simplest version of IO
// where each process reads the graph sequentially
//
//int parallel_graph_io::readGraphWeightedMETIS(parallel_graph_access & G,
//std::string filename,
//PEID peID, PEID comm_size, MPI_Comm communicator) {
//std::string line;

//// open file for reading
//std::ifstream in(filename.c_str());
//if (!in) {
//std::cerr << "Error opening " << filename << std::endl;
//return 1;
//}

//NodeID nmbNodes;
//EdgeID nmbEdges;

//std::getline(in,line);
////skip comments
//while( line[0] == '%' ) {
//std::getline(in, line);
//}

//int ew = 0;
//std::stringstream ss(line);
//ss >> nmbNodes;
//ss >> nmbEdges;
//ss >> ew;

//if(ew == 1) {
//std::cout <<  "io of weighted graphs not supported yet"  << std::endl;
//exit(0);
//} else if (ew == 11) {
//std::cout <<  "io of weighted graphs not supported yet"  << std::endl;
//exit(0);
//} else if (ew == 10) {
//std::cout <<  "io of weighted graphs not supported yet"  << std::endl;
//exit(0);
//}

//// pe p reads the lines p*ceil(n/size) to (p+1)floor(n/size) lines of that file
//ULONG from           = peID     * ceil(nmbNodes / (double)comm_size);
//ULONG to             = (peID+1) * ceil(nmbNodes / (double)comm_size) - 1;
//to = std::min(to, nmbNodes-1);

//ULONG local_no_nodes = to - from + 1;
//std::cout <<  "peID " <<  peID <<  " from " <<  from <<  " to " <<  to  <<  " amount " <<  local_no_nodes << std::endl;

//std::vector< std::vector< NodeID > > local_edge_lists;
//local_edge_lists.resize(local_no_nodes);


////std::getline(in, line);
//ULONG counter      = 0;
//NodeID node_counter = 0;
//EdgeID edge_counter = 0;

//while( std::getline(in, line) ) {
//if( counter > to ) {
//break;
//}
//if (line[0] == '%') { // a comment in the file
//continue;
//}

//if( counter >= from ) {
//std::stringstream ss(line);

//NodeID target;
//while( ss >> target ) {
//local_edge_lists[node_counter].push_back(target);
//edge_counter++;
//}
//node_counter++;
//}

//counter++;

//if( in.eof() ) {
//break;
//}
//}

//MPI_Barrier(communicator);

//G.start_construction(local_no_nodes, 2*edge_counter, nmbNodes, 2*nmbEdges);
//G.set_range(from, to);

//for (NodeID i = 0; i < local_no_nodes; ++i) {
//NodeID node = G.new_node();
//G.setNodeWeight(node, 1);
//G.setNodeLabel(node, from+node);
//G.setSecondPartitionIndex(node, 0);

//for( ULONG j = 0; j < local_edge_lists[i].size(); j++) {
//NodeID target = local_edge_lists[i][j]-1; // -1 since there are no nodes with id 0 in the file
//EdgeID e = G.new_edge(node, target);
//G.setEdgeWeight(e, 1);
//}
//}

//G.finish_construction();
//return 0;
//}

int parallel_graph_io::writeGraphExternallyBinary(std::string input_filename, std::string output_filename) {

  std::string line;

  // open file for reading
  std::ifstream in(input_filename.c_str());
  if (!in) {
    std::cerr << "Error opening " << input_filename << std::endl;
    return 1;
  }

  NodeID n;
  EdgeID m;

  std::getline(in,line);
  //skip comments
  while( line[0] == '%' ) {
    std::getline(in, line);
  }

  int ew = 0;
  std::stringstream ss(line);
  ss >> n;
  ss >> m;
  ss >> ew;

  m *= 2;

  std::ofstream outfile;
  outfile.open(output_filename.c_str(), std::ios::binary | std::ios::out);
  outfile.write((char*)(&fileTypeVersionNumber), sizeof( ULONG ));
  outfile.write((char*)(&n), sizeof( ULONG ));
  outfile.write((char*)(&m), sizeof( ULONG ));

  NodeID offset = (header_count + n + 1) * (sizeof(ULONG));

  while( std::getline(in, line) ) {
    if (line[0] == '%') { // a comment in the file
      continue;
    }

    std::stringstream ss(line);

    EdgeID edge_counter = 0;
    NodeID target;
    while( ss >> target ) {
      edge_counter++;
    }
    outfile.write((char*)(&offset), sizeof( ULONG ));
    offset += edge_counter*sizeof( ULONG );

    if( in.eof() ) {
      break;
    }
  }
  outfile.write((char*)(&offset), sizeof( ULONG ));
  in.close();

  // second stream to actually write the edges
  std::ifstream second_in(input_filename.c_str());
  std::getline(second_in,line);
  //skip comments
  while( line[0] == '%' ) {
    std::getline(second_in, line);
  }

  while( std::getline(second_in, line) ) {
    if (line[0] == '%') { // a comment in the file
      continue;
    }

    std::stringstream ss(line);

    NodeID target;
    while( ss >> target ) {
      target -= 1;
      outfile.write((char*)(&target), sizeof( ULONG ));
    }

    if( second_in.eof() ) {
      break;
    }
  }
  second_in.close();

  return 0;

}

int parallel_graph_io::writeGraphSequentiallyBinary(
    complete_graph_access& graph,
    std::string filename) {
  auto operation =
      mpi::communicator{communicator_view{graph.getCommunicator()}};
  auto const communicator = operation.view();
  if (communicator.size() != 1) {
    mpi::abort_on_programming_error(
        communicator.native_handle(),
        "sequential binary graph output requires exactly one rank");
  }
  try {
    auto const global_nodes = graph.number_of_global_nodes();
    auto const global_edges = graph.number_of_global_edges();
    auto const node_storage_valid =
        std::in_range<std::size_t>(global_nodes) &&
        global_nodes < std::numeric_limits<std::size_t>::max();
    auto const edge_storage_valid = std::in_range<std::size_t>(global_edges);
    if (!node_storage_valid || !edge_storage_valid) {
      mpi::abort_on_capacity_failure(communicator.native_handle(),
                                     "sequential binary graph output",
                                     "graph storage size is not representable");
    }
    auto const adjacency_base = binary_adjacency_base(global_nodes);
    if (!adjacency_base.has_value()) {
      mpi::abort_on_capacity_failure(
          communicator.native_handle(), "sequential binary graph output",
          "binary graph byte layout is not representable");
    }

    auto offsets =
        std::vector<NodeID>(static_cast<std::size_t>(global_nodes) + 1);
    auto edges = std::vector<EdgeID>(static_cast<std::size_t>(global_edges));
    auto offset = *adjacency_base;
    auto edge_position = std::size_t{0};
    for (NodeID node = 0; node < graph.number_of_local_nodes(); ++node) {
      offsets[static_cast<std::size_t>(node)] = offset;
      auto const degree_bytes =
          checked_multiply(graph.getNodeDegree(node), ULONG{sizeof(ULONG)});
      if (!degree_bytes.has_value() ||
          !checked_add(offset, *degree_bytes).has_value()) {
        mpi::abort_on_capacity_failure(
            communicator.native_handle(), "sequential binary graph output",
            "binary graph adjacency offset is not representable");
      }
      offset += *degree_bytes;
      for (EdgeID edge = graph.get_first_edge(node);
           edge < graph.get_first_invalid_edge(node); ++edge) {
        if (edge_position >= edges.size()) {
          mpi::abort_on_programming_error(
              communicator.native_handle(),
              "sequential binary graph edge count is inconsistent");
        }
        edges[edge_position++] = graph.getEdgeTarget(edge);
      }
    }
    offsets.back() = offset;
    if (graph.number_of_local_nodes() != global_nodes ||
        edge_position != edges.size()) {
      mpi::abort_on_programming_error(
          communicator.native_handle(),
          "sequential binary graph counts are inconsistent");
    }

    std::cout << "Writing graph " << filename << std::endl;
    auto const header =
        std::array<ULONG, 3>{fileTypeVersionNumber, global_nodes, global_edges};
    auto descriptor = graph_file_descriptor{
        ::open(filename.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644)};
    auto write_success =
        descriptor &&
        write_graph_exact(descriptor.get(), std::span<ULONG const>{header}, 0);
    auto const offsets_position = ULONG{sizeof(header)};
    write_success =
        write_success &&
        write_graph_exact(descriptor.get(), std::span<NodeID const>{offsets},
                          offsets_position) &&
        write_graph_exact(descriptor.get(), std::span<EdgeID const>{edges},
                          *adjacency_base);
    auto const close_success = descriptor && descriptor.close();
    if (!write_success || !close_success) {
      mpi::abort_on_backend_failure(communicator.native_handle(),
                                    "sequential binary graph I/O failed");
    }
    return 0;
  } catch (...) {
    mpi::abort_on_exception(communicator.native_handle(),
                            "sequential binary graph output failed");
  }
}

int parallel_graph_io::readGraphBinary(PPartitionConfig& config,
                                       parallel_graph_access& graph,
                                       std::string filename,
                                       PEID supplied_rank,
                                       PEID supplied_size,
                                       MPI_Comm native_communicator) {
  auto const borrowed = communicator_view{native_communicator};
  mpi::require_live_intracommunicator(
      borrowed, "binary graph input requires a live intracommunicator");
  validate_graph_call(graph, supplied_rank, supplied_size, borrowed);

  auto buffer = std::array<ULONG, 3>{};
  int success = 0;
  if (borrowed.rank() == ROOT) {
    std::cout << "Reading binary graph ..." << std::endl;
    auto file = std::ifstream{filename, std::ios::binary | std::ios::in};
    if (file) {
      file.read(reinterpret_cast<char*>(buffer.data()),
                static_cast<std::streamsize>(sizeof(buffer)));
      success = file ? 1 : -1;
    }
  }

  mpi::broadcast_fixed(success, ROOT, borrowed,
                       "MPI_Bcast(binary graph read status)");

  if (success == 0) {
    mpi::abort_on_backend_failure(native_communicator,
                                  "unable to open binary graph file");
  }
  if (success < 0) {
    mpi::abort_on_backend_failure(native_communicator,
                                  "unable to read binary graph header");
  }

  mpi::broadcast_fixed(std::span{buffer}, ROOT, borrowed,
                       "MPI_Bcast(binary graph header)");
  auto const version = buffer[0];
  auto const global_nodes = NodeID{buffer[1]};
  auto const global_edges = EdgeID{buffer[2]};

  if (borrowed.rank() == ROOT) {
    std::cout << "version: " << version << " n: " << global_nodes
              << " m: " << global_edges << std::endl;
  }
  if (version != fileTypeVersionNumber) {
    mpi::abort_on_backend_failure(native_communicator,
                                  "unsupported binary graph version");
  }

  auto operation = mpi::communicator{borrowed};
  auto const communicator = operation.view();
  try {
    require_graph_filename(filename, communicator,
                           "binary graph payload I/O failed");
    auto const window =
        graph_window(config.binary_io_window_size, communicator);
    auto const interval = local_graph_interval(
        global_nodes, communicator.rank(), communicator.size());
    auto const local_size_is_representable =
        std::in_range<std::size_t>(interval.size()) &&
        interval.size() < std::numeric_limits<std::size_t>::max();
    require_graph_capacity(local_size_is_representable, communicator,
                           "binary graph input",
                           "local vertex count is not representable");

    auto const adjacency_base = binary_adjacency_base(global_nodes);
    auto const adjacency_bytes =
        checked_multiply(global_edges, ULONG{sizeof(ULONG)});
    auto const expected_end =
        adjacency_base.has_value() && adjacency_bytes.has_value()
            ? checked_add(*adjacency_base, *adjacency_bytes)
            : std::nullopt;
    auto const local_offset_word = checked_add(header_count, interval.first);
    auto const local_offset_byte =
        local_offset_word.has_value()
            ? checked_multiply(*local_offset_word, ULONG{sizeof(ULONG)})
            : std::nullopt;
    require_graph_capacity(
        adjacency_base.has_value() && expected_end.has_value() &&
            local_offset_byte.has_value() &&
            *expected_end <=
                static_cast<ULONG>(std::numeric_limits<off_t>::max()),
        communicator, "binary graph input",
        "binary graph byte layout is not representable");

    auto vertex_offsets =
        std::vector<NodeID>(static_cast<std::size_t>(interval.size()) + 1);
    auto edges = std::vector<EdgeID>{};
    auto local_edges = EdgeID{0};
    auto const rank = communicator.rank();
    auto const size = communicator.size();
    for (auto low = 0; low < size; low += window) {
      auto const high = std::min(size, low + window);
      auto const active = rank >= low && rank < high;
      auto descriptor = graph_file_descriptor{
          active ? ::open(filename.c_str(), O_RDONLY) : -1};
      require_graph_io_success(
          !active || descriptor, communicator,
          "binary graph payload I/O failed",
          "MPI_Allreduce(binary graph payload open status)");

      auto payload_success = true;
      if (active) {
        payload_success = read_graph_exact(descriptor.get(),
                                           std::span<NodeID>{vertex_offsets},
                                           *local_offset_byte);
        if (payload_success) {
          payload_success =
              std::ranges::is_sorted(vertex_offsets) &&
              std::ranges::all_of(vertex_offsets, [&](auto offset) {
                return offset >= *adjacency_base && offset <= *expected_end &&
                       (offset - *adjacency_base) % sizeof(ULONG) == 0;
              });
        }
        if (payload_success) {
          auto const bytes = vertex_offsets.back() - vertex_offsets.front();
          local_edges = bytes / sizeof(ULONG);
          payload_success = std::in_range<std::size_t>(local_edges);
        }
        if (payload_success) {
          edges.resize(static_cast<std::size_t>(local_edges));
          payload_success =
              read_graph_exact(descriptor.get(), std::span<EdgeID>{edges},
                               vertex_offsets.front());
        }
        if (payload_success) {
          payload_success = std::ranges::all_of(
              edges, [&](auto target) { return target < global_nodes; });
        }
        auto const close_success = descriptor.close();
        payload_success = payload_success && close_success;
      }
      require_graph_io_success(
          payload_success, communicator, "binary graph payload I/O failed",
          "MPI_Allreduce(binary graph payload read status)");
    }

    validate_binary_offset_ranges(vertex_offsets, *adjacency_base,
                                  *expected_end, communicator);

    auto const observed_edges =
        collectively_sum_edges(local_edges, communicator, "binary graph input");
    if (observed_edges != global_edges) {
      mpi::abort_on_backend_failure(communicator.native_handle(),
                                    "binary graph payload I/O failed");
    }

    graph.start_construction(interval.size(), local_edges, global_nodes,
                             global_edges);
    graph.set_range(interval.first, interval.inclusive_last());
    auto distribution = graph_distribution(global_nodes, communicator.size());
    graph.set_range_array(distribution);
    auto edge_position = std::size_t{0};
    for (NodeID local = 0; local < interval.size(); ++local) {
      auto const source = graph.new_node();
      graph.setNodeWeight(source, 1);
      graph.setNodeLabel(source, interval.first + source);
      graph.setSecondPartitionIndex(source, 0);
      auto const degree = static_cast<std::size_t>(
          (vertex_offsets[static_cast<std::size_t>(local) + 1] -
           vertex_offsets[static_cast<std::size_t>(local)]) /
          sizeof(ULONG));
      for (auto local_edge = std::size_t{0}; local_edge < degree;
           ++local_edge, ++edge_position) {
        auto const edge = graph.new_edge(source, edges[edge_position]);
        graph.setEdgeWeight(edge, 1);
      }
    }
    graph.finish_construction();
    return 0;
  } catch (...) {
    mpi::abort_on_exception(communicator.native_handle(),
                            "binary graph input failed");
  }
}

int parallel_graph_io::writeGraphParallelSimple(parallel_graph_access& graph,
                                                std::string filename,
                                                MPI_Comm native_communicator) {
  auto const borrowed = communicator_view{native_communicator};
  mpi::require_live_intracommunicator(
      borrowed, "graph text output requires a live intracommunicator");
  validate_graph_call(graph, borrowed.rank(), borrowed.size(), borrowed);
  auto operation = mpi::communicator{borrowed};
  auto const communicator = operation.view();
  try {
    require_graph_filename(filename, communicator,
                           "graph text output I/O failed");
    auto const layout = validated_graph_output_layout(graph, communicator);
    auto const rank = communicator.rank();
    for (auto writer = 0; writer < communicator.size(); ++writer) {
      auto output = std::ofstream{};
      auto open_success = true;
      if (rank == writer) {
        auto const mode = writer == ROOT ? std::ios::out | std::ios::trunc
                                         : std::ios::out | std::ios::app;
        output.open(filename, mode);
        open_success = static_cast<bool>(output);
      }
      require_graph_io_success(open_success, communicator,
                               "graph text output I/O failed",
                               "MPI_Allreduce(graph text output open status)");

      auto write_success = true;
      if (rank == writer) {
        if (writer == ROOT) {
          output << layout.global_nodes << ' ' << layout.global_edges / 2
                 << '\n';
        }
        for (NodeID node = 0; node < graph.number_of_local_nodes(); ++node) {
          for (EdgeID edge = graph.get_first_edge(node);
               edge < graph.get_first_invalid_edge(node); ++edge) {
            output << graph.getGlobalID(graph.getEdgeTarget(edge)) + 1 << ' ';
          }
          output << '\n';
        }
        output.close();
        write_success = static_cast<bool>(output);
      }
      require_graph_io_success(write_success, communicator,
                               "graph text output I/O failed",
                               "MPI_Allreduce(graph text output write status)");
    }
    return 0;
  } catch (...) {
    mpi::abort_on_exception(communicator.native_handle(),
                            "graph text output failed");
  }
}

int parallel_graph_io::writeGraphWeightedParallelSimple(
    parallel_graph_access& graph,
    std::string filename,
    MPI_Comm native_communicator) {
  auto const borrowed = communicator_view{native_communicator};
  mpi::require_live_intracommunicator(
      borrowed, "weighted graph output requires a live intracommunicator");
  validate_graph_call(graph, borrowed.rank(), borrowed.size(), borrowed);
  auto operation = mpi::communicator{borrowed};
  auto const communicator = operation.view();
  try {
    require_graph_filename(filename, communicator,
                           "weighted graph output I/O failed");
    auto const layout = validated_graph_output_layout(graph, communicator);
    auto const rank = communicator.rank();
    for (auto writer = 0; writer < communicator.size(); ++writer) {
      auto output = std::ofstream{};
      auto open_success = true;
      if (rank == writer) {
        auto const mode = writer == ROOT ? std::ios::out | std::ios::trunc
                                         : std::ios::out | std::ios::app;
        output.open(filename, mode);
        open_success = static_cast<bool>(output);
      }
      require_graph_io_success(
          open_success, communicator, "weighted graph output I/O failed",
          "MPI_Allreduce(weighted graph output open status)");

      auto write_success = true;
      if (rank == writer) {
        if (writer == ROOT) {
          output << layout.global_nodes << ' ' << layout.global_edges / 2
                 << " 11\n";
        }
        for (NodeID node = 0; node < graph.number_of_local_nodes(); ++node) {
          output << graph.getNodeWeight(node);
          for (EdgeID edge = graph.get_first_edge(node);
               edge < graph.get_first_invalid_edge(node); ++edge) {
            output << ' ' << graph.getGlobalID(graph.getEdgeTarget(edge)) + 1
                   << ' ' << graph.getEdgeWeight(edge);
          }
          output << '\n';
        }
        output.close();
        write_success = static_cast<bool>(output);
      }
      require_graph_io_success(
          write_success, communicator, "weighted graph output I/O failed",
          "MPI_Allreduce(weighted graph output write status)");
    }
    return 0;
  } catch (...) {
    mpi::abort_on_exception(communicator.native_handle(),
                            "weighted graph output failed");
  }
}

int parallel_graph_io::writeGraphWeightedSequentially(complete_graph_access & G, std::string filename) {
  std::ofstream f(filename.c_str());
  f << G.number_of_global_nodes() <<  " " <<  G.number_of_global_edges()/2 <<  " 11" <<  std::endl;

  forall_local_nodes(G, node) {
    f <<  G.getNodeWeight(node) ;
    forall_out_edges(G, e, node) {
      f << " " <<   (G.getEdgeTarget(e)+1) <<  " " <<  G.getEdgeWeight(e) ;
    } endfor
    f <<  "\n";
  } endfor

  f.close();
  return 0;
}

int parallel_graph_io::writeGraphSequentially(complete_graph_access & G, std::ofstream & f) {
  f << G.number_of_global_nodes() <<  " " <<  G.number_of_global_edges()/2 <<   std::endl;

  forall_local_nodes(G, node) {
    forall_out_edges(G, e, node) {
      f << " " << (G.getEdgeTarget(e)+1)  ;
    } endfor
    f <<  "\n";
  } endfor
  return 0;
}

int parallel_graph_io::writeGraphSequentially(complete_graph_access & G, std::string filename) {
  std::ofstream f(filename);
  writeGraphSequentially(G, f);
  f.close();
  return 0;

}

// we start with the simplest version of IO
// where each process reads the graph sequentially
// TODO write weighted code and fully parallel io code
int parallel_graph_io::readGraphWeightedMETIS_fixed(
    parallel_graph_access& graph,
    std::string filename,
    PEID rank,
    PEID size,
    MPI_Comm communicator) {
  return read_metis_graph(graph, filename, rank, size, communicator);
}
}
