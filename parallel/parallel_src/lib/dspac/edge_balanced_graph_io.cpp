#define _FILE_OFFSET_BITS 64

/******************************************************************************
 * edge_balanced_graph_io.cpp
 * *
 * Source of KaHIP -- Karlsruhe High Quality Partitioning.
 * Christian Schulz <christian.schulz.phone@gmail.com>
 *****************************************************************************/

#include "edge_balanced_graph_io.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "communication/mpi_failure.h"
#include "communication/mpi_fixed_broadcast.h"

namespace parhip {
namespace {
namespace detail = edge_balanced_graph_io_detail;
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
                                    ULONG offset) noexcept -> bool {
  constexpr auto maximum_offset =
      static_cast<ULONG>(std::numeric_limits<off_t>::max());
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
    offset += static_cast<ULONG>(count);
  }
  return true;
}

template <typename T>
[[nodiscard]] auto read_graph_exact(int descriptor,
                                    std::span<T> values,
                                    ULONG offset) noexcept -> bool {
  static_assert(std::is_trivially_copyable_v<T>);
  return read_graph_exact(descriptor, std::as_writable_bytes(values), offset);
}

[[nodiscard]] auto observed_file_extent(int descriptor, ULONG& extent) noexcept
    -> bool {
  struct stat status{};
  if (::fstat(descriptor, &status) != 0 || status.st_size < 0) {
    return false;
  }
  auto const observed = static_cast<std::uintmax_t>(status.st_size);
  if (observed >
      static_cast<std::uintmax_t>(std::numeric_limits<ULONG>::max())) {
    return false;
  }
  extent = static_cast<ULONG>(observed);
  return true;
}

[[nodiscard]] auto descriptor_has_extent(int descriptor,
                                         ULONG expected) noexcept -> bool {
  auto observed = ULONG{0};
  return observed_file_extent(descriptor, observed) && observed == expected;
}

void require_collective_backend_success(
    bool local_success,
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

void require_collective_capacity(bool local_success,
                                 communicator_view communicator,
                                 std::string_view boundary,
                                 std::string_view diagnostic) noexcept {
  auto const local = local_success ? 1 : 0;
  auto global = 0;
  mpi::check_or_abort(MPI_Allreduce(&local, &global, 1, MPI_INT, MPI_MIN,
                                    communicator.native_handle()),
                      communicator.native_handle(),
                      "MPI_Allreduce(edge-balanced graph I/O capacity status)");
  if (global == 0) {
    mpi::abort_on_capacity_failure(communicator.native_handle(), boundary,
                                   diagnostic);
  }
}

void require_collective_programming_condition(
    bool local_success,
    communicator_view communicator,
    std::string_view diagnostic,
    std::string_view agreement_context) noexcept {
  auto const local = local_success ? 1 : 0;
  auto global = 0;
  mpi::check_or_abort(MPI_Allreduce(&local, &global, 1, MPI_INT, MPI_MIN,
                                    communicator.native_handle()),
                      communicator.native_handle(), agreement_context);
  if (global == 0) {
    mpi::abort_on_programming_error(communicator.native_handle(), diagnostic);
  }
}

void validate_graph_communicator(parallel_graph_access& graph,
                                 communicator_view communicator) noexcept {
  auto relation = int{MPI_UNEQUAL};
  mpi::check_or_abort(MPI_Comm_compare(graph.getCommunicator(),
                                       communicator.native_handle(), &relation),
                      communicator.native_handle(),
                      "MPI_Comm_compare(edge-balanced graph I/O communicator)");
  require_collective_programming_condition(
      relation == MPI_IDENT || relation == MPI_CONGRUENT, communicator,
      "edge-balanced graph I/O communicator does not match the graph",
      "MPI_Allreduce(edge-balanced graph communicator agreement)");
}

void validate_legacy_rank_and_size(int supplied_rank,
                                   int supplied_size,
                                   communicator_view communicator) noexcept {
  require_collective_programming_condition(
      supplied_rank == communicator.rank() &&
          supplied_size == communicator.size(),
      communicator,
      "edge-balanced graph I/O supplied rank or size does not match the "
      "graph communicator",
      "MPI_Allreduce(edge-balanced legacy rank and size validation)");
}

void require_common_filename(std::string_view filename,
                             communicator_view communicator) {
  auto canonical_size = std::uint64_t{0};
  if (communicator.rank() == ROOT) {
    canonical_size = filename.size();
  }
  mpi::broadcast_fixed(canonical_size, ROOT, communicator,
                       "MPI_Bcast(edge-balanced filename size)");
  if (!std::in_range<std::size_t>(canonical_size)) {
    mpi::abort_on_capacity_failure(communicator.native_handle(),
                                   "edge-balanced graph filename",
                                   "filename size is not representable");
  }
  auto canonical = std::string(static_cast<std::size_t>(canonical_size), '\0');
  if (communicator.rank() == ROOT) {
    std::ranges::copy(filename, canonical.begin());
  }
  mpi::broadcast_bounded(std::span<char>{canonical}, ROOT, communicator,
                         "MPI_Bcast(edge-balanced filename)");
  require_collective_backend_success(
      filename == canonical, communicator,
      "edge-balanced graph filename differs across communicator",
      "MPI_Allreduce(edge-balanced filename agreement)");
}

[[nodiscard]] auto common_io_window(int configured,
                                    communicator_view communicator) noexcept
    -> int {
  auto minimum = 0;
  auto maximum = 0;
  mpi::check_or_abort(MPI_Allreduce(&configured, &minimum, 1, MPI_INT, MPI_MIN,
                                    communicator.native_handle()),
                      communicator.native_handle(),
                      "MPI_Allreduce(edge-balanced I/O window minimum)");
  mpi::check_or_abort(MPI_Allreduce(&configured, &maximum, 1, MPI_INT, MPI_MAX,
                                    communicator.native_handle()),
                      communicator.native_handle(),
                      "MPI_Allreduce(edge-balanced I/O window maximum)");
  if (minimum != maximum) {
    mpi::abort_on_programming_error(
        communicator.native_handle(),
        "edge-balanced graph I/O window differs across communicator");
  }
  auto const window = detail::validated_window(minimum, communicator.size());
  if (!window.has_value()) {
    mpi::abort_on_programming_error(
        communicator.native_handle(),
        "edge-balanced graph I/O window must be positive");
  }
  return *window;
}

struct node_interval final {
  NodeID first = 0;
  NodeID next = 0;

  [[nodiscard]] constexpr auto size() const noexcept -> NodeID {
    return next - first;
  }
  [[nodiscard]] constexpr auto inclusive_last() const noexcept -> NodeID {
    return first == next ? first : next - 1;
  }
};

[[nodiscard]] constexpr auto validation_interval(NodeID nodes,
                                                 int rank,
                                                 int size) noexcept
    -> node_interval {
  return node_interval{detail::balanced_vertex_boundary(nodes, rank, size),
                       detail::balanced_vertex_boundary(nodes, rank + 1, size)};
}

[[nodiscard]] constexpr auto offset_table_position(NodeID node) noexcept
    -> std::optional<ULONG> {
  auto const word = detail::checked_add(detail::header_words, node);
  return word.has_value()
             ? detail::checked_multiply(*word, ULONG{sizeof(ULONG)})
             : std::nullopt;
}

[[nodiscard]] auto read_header(std::string const& filename,
                               communicator_view communicator,
                               std::array<ULONG, 3>& header,
                               ULONG& file_extent) noexcept -> bool {
  if (communicator.rank() != ROOT) {
    return true;
  }
  auto descriptor = graph_file_descriptor{::open(filename.c_str(), O_RDONLY)};
  if (!descriptor) {
    return false;
  }
  auto success =
      observed_file_extent(descriptor.get(), file_extent) &&
      read_graph_exact(descriptor.get(), std::span<ULONG>{header}, ULONG{0});
  auto const close_success = descriptor.close();
  return success && close_success;
}

[[nodiscard]] auto read_node_boundary(int descriptor,
                                      NodeID nodes,
                                      EdgeID edges,
                                      int boundary,
                                      int size,
                                      detail::binary_layout layout,
                                      NodeID& result) noexcept -> bool {
  if (edges == 0) {
    result = detail::balanced_vertex_boundary(nodes, boundary, size);
    return true;
  }
  if (boundary <= 0) {
    result = 0;
    return true;
  }
  if (boundary >= size) {
    result = nodes;
    return true;
  }

  auto const target = detail::balanced_edge_target(edges, boundary, size);
  auto low = NodeID{0};
  auto high = nodes;
  while (low < high) {
    auto const middle = low + (high - low) / 2;
    auto const position = offset_table_position(middle);
    auto offset = ULONG{0};
    if (!position.has_value() ||
        !read_graph_exact(descriptor, std::span<ULONG>{&offset, 1},
                          *position) ||
        !detail::offsets_are_valid(std::span<ULONG const>{&offset, 1}, layout,
                                   false, false)) {
      return false;
    }
    auto const prefix = (offset - layout.adjacency_begin) / sizeof(ULONG);
    if (prefix < target) {
      low = middle + 1;
    } else {
      high = middle;
    }
  }
  result = low;
  return true;
}

void validate_offset_slices(std::span<ULONG const> local_offsets,
                            detail::binary_layout layout,
                            communicator_view communicator,
                            std::string_view context) {
  auto const local =
      std::array<std::uint64_t, 2>{local_offsets.front(), local_offsets.back()};
  auto gathered = std::vector<std::uint64_t>(
      static_cast<std::size_t>(communicator.size()) * local.size());
  mpi::check_or_abort(
      MPI_Allgather(local.data(), static_cast<int>(local.size()), MPI_UINT64_T,
                    gathered.data(), static_cast<int>(local.size()),
                    MPI_UINT64_T, communicator.native_handle()),
      communicator.native_handle(), context);

  auto valid = gathered.front() == layout.adjacency_begin;
  for (auto rank = 1; valid && rank < communicator.size(); ++rank) {
    auto const previous = static_cast<std::size_t>(rank - 1) * local.size();
    auto const current = static_cast<std::size_t>(rank) * local.size();
    valid = gathered[previous + 1] == gathered[current];
  }
  valid = valid && gathered.back() == layout.file_extent;
  require_collective_backend_success(
      valid, communicator, "edge-balanced binary offset table is invalid",
      "MPI_Allreduce(edge-balanced offset slice validation)");
}

[[nodiscard]] auto gather_node_ranges(NodeID first,
                                      NodeID nodes,
                                      communicator_view communicator)
    -> std::vector<NodeID> {
  auto ranges =
      std::vector<NodeID>(static_cast<std::size_t>(communicator.size()) + 1);
  mpi::check_or_abort(
      MPI_Allgather(&first, 1, MPI_UNSIGNED_LONG_LONG, ranges.data(), 1,
                    MPI_UNSIGNED_LONG_LONG, communicator.native_handle()),
      communicator.native_handle(), "MPI_Allgather(edge-balanced node ranges)");
  ranges.back() = nodes;
  auto const valid = ranges.front() == 0 && ranges.back() == nodes &&
                     std::ranges::is_sorted(ranges) &&
                     std::ranges::all_of(ranges, [=](auto boundary) {
                       return boundary <= nodes;
                     });
  require_collective_backend_success(
      valid, communicator, "edge-balanced node ranges are invalid",
      "MPI_Allreduce(edge-balanced node range validation)");
  return ranges;
}

[[nodiscard]] auto gather_edge_ranges(EdgeID local_edges,
                                      EdgeID global_edges,
                                      communicator_view communicator)
    -> std::vector<EdgeID> {
  auto counts =
      std::vector<EdgeID>(static_cast<std::size_t>(communicator.size()));
  mpi::check_or_abort(
      MPI_Allgather(&local_edges, 1, MPI_UNSIGNED_LONG_LONG, counts.data(), 1,
                    MPI_UNSIGNED_LONG_LONG, communicator.native_handle()),
      communicator.native_handle(), "MPI_Allgather(edge-balanced edge counts)");

  auto ranges = std::vector<EdgeID>(counts.size() + 1, EdgeID{0});
  auto representable = true;
  for (auto rank = std::size_t{0}; rank < counts.size(); ++rank) {
    representable =
        representable &&
        counts[rank] <= std::numeric_limits<EdgeID>::max() - ranges[rank];
    if (representable) {
      ranges[rank + 1] = ranges[rank] + counts[rank];
    }
  }
  require_collective_capacity(representable, communicator,
                              "edge-balanced graph input",
                              "global edge prefix sum is not representable");
  require_collective_backend_success(
      ranges.back() == global_edges, communicator,
      "edge-balanced binary graph edge count does not match its header",
      "MPI_Allreduce(edge-balanced global edge validation)");
  return ranges;
}

void construct_graph(parallel_graph_access& graph,
                     NodeID global_nodes,
                     EdgeID global_edges,
                     node_interval interval,
                     std::span<ULONG const> offsets,
                     std::span<EdgeID const> adjacency,
                     std::vector<NodeID>& node_ranges,
                     std::vector<EdgeID>& edge_ranges) {
  graph.start_construction(interval.size(),
                           static_cast<EdgeID>(adjacency.size()), global_nodes,
                           global_edges);
  graph.set_range(interval.first, interval.inclusive_last());
  graph.set_range_array(node_ranges);
  graph.set_edge_range_array(edge_ranges);

  auto edge_position = std::size_t{0};
  for (auto local = NodeID{0}; local < interval.size(); ++local) {
    auto const source = graph.new_node();
    graph.setNodeWeight(source, 1);
    graph.setNodeLabel(source, interval.first + source);
    graph.setSecondPartitionIndex(source, 0);
    auto const index = static_cast<std::size_t>(local);
    auto const degree = static_cast<std::size_t>(
        (offsets[index + 1] - offsets[index]) / sizeof(ULONG));
    for (auto local_edge = std::size_t{0}; local_edge < degree;
         ++local_edge, ++edge_position) {
      auto const edge = graph.new_edge(source, adjacency[edge_position]);
      graph.setEdgeWeight(edge, 1);
    }
  }
  graph.finish_construction();
}
}  // namespace

void edge_balanced_graph_io::read_binary_graph_edge_balanced(
    parallel_graph_access& graph,
    std::string const& filename,
    PPartitionConfig const& config,
    std::vector<EdgeID>& permutation,
    mpi::communicator_view borrowed) {
  mpi::require_live_intracommunicator(
      borrowed, "edge-balanced graph input requires a live intracommunicator");
  auto operation = mpi::communicator{borrowed};
  auto const communicator = operation.view();
  validate_graph_communicator(graph, communicator);

  try {
    require_common_filename(filename, communicator);
    auto const window =
        common_io_window(config.binary_io_window_size, communicator);

    auto header = std::array<ULONG, 3>{};
    auto observed_extent = ULONG{0};
    auto const local_header_success =
        read_header(filename, communicator, header, observed_extent);
    require_collective_backend_success(
        local_header_success, communicator,
        "unable to open or read edge-balanced binary graph header",
        "MPI_Allreduce(edge-balanced header read status)");
    mpi::broadcast_fixed(std::span{header}, ROOT, communicator,
                         "MPI_Bcast(edge-balanced graph header)");
    mpi::broadcast_fixed(observed_extent, ROOT, communicator,
                         "MPI_Bcast(edge-balanced graph file extent)");

    auto const version = header[0];
    auto const global_nodes = NodeID{header[1]};
    auto const global_edges = EdgeID{header[2]};
    require_collective_backend_success(
        version == detail::file_type_version, communicator,
        "unsupported edge-balanced binary graph version",
        "MPI_Allreduce(edge-balanced header version validation)");

    auto const layout = detail::make_binary_layout(global_nodes, global_edges);
    require_collective_capacity(
        layout.has_value() &&
            layout->file_extent <=
                static_cast<ULONG>(std::numeric_limits<off_t>::max()),
        communicator, "edge-balanced graph input",
        "binary graph byte layout is not representable");
    require_collective_backend_success(
        detail::file_extent_is_valid(observed_extent, *layout), communicator,
        "edge-balanced binary graph has an invalid file extent",
        "MPI_Allreduce(edge-balanced file extent validation)");

    auto const rank = communicator.rank();
    auto const size = communicator.size();
    auto const validation = validation_interval(global_nodes, rank, size);
    auto const validation_position = offset_table_position(validation.first);
    auto const validation_size_is_representable =
        std::in_range<std::size_t>(validation.size()) &&
        validation.size() < std::numeric_limits<std::size_t>::max();
    require_collective_capacity(
        validation_position.has_value() && validation_size_is_representable,
        communicator, "edge-balanced graph input",
        "offset validation range is not representable");

    auto validation_offsets =
        std::vector<ULONG>(static_cast<std::size_t>(validation.size()) + 1);
    auto first_node = NodeID{0};
    for (auto low = 0; low < size; low += window) {
      auto const high = std::min(size, low + window);
      auto const active = rank >= low && rank < high;
      auto descriptor = graph_file_descriptor{
          active ? ::open(filename.c_str(), O_RDONLY) : -1};
      require_collective_backend_success(
          !active || descriptor, communicator,
          "unable to open edge-balanced binary graph payload",
          "MPI_Allreduce(edge-balanced validation open status)");

      auto local_success = true;
      if (active) {
        local_success =
            descriptor_has_extent(descriptor.get(), layout->file_extent) &&
            read_graph_exact(descriptor.get(),
                             std::span<ULONG>{validation_offsets},
                             *validation_position) &&
            detail::offsets_are_valid(validation_offsets, *layout, false,
                                      false) &&
            read_node_boundary(descriptor.get(), global_nodes, global_edges,
                               rank, size, *layout, first_node);
        auto const close_success = descriptor.close();
        local_success = local_success && close_success;
      }
      require_collective_backend_success(
          local_success, communicator,
          "edge-balanced binary graph offset validation failed",
          "MPI_Allreduce(edge-balanced offset validation read status)");
    }
    validate_offset_slices(
        validation_offsets, *layout, communicator,
        "MPI_Allgather(edge-balanced validation offset slices)");

    auto node_ranges =
        gather_node_ranges(first_node, global_nodes, communicator);
    auto const interval =
        node_interval{node_ranges[static_cast<std::size_t>(rank)],
                      node_ranges[static_cast<std::size_t>(rank) + 1]};
    auto const local_position = offset_table_position(interval.first);
    auto const local_size_is_representable =
        std::in_range<std::size_t>(interval.size()) &&
        interval.size() < std::numeric_limits<std::size_t>::max();
    require_collective_capacity(
        local_position.has_value() && local_size_is_representable, communicator,
        "edge-balanced graph input", "local graph range is not representable");

    auto local_offsets =
        std::vector<ULONG>(static_cast<std::size_t>(interval.size()) + 1);
    auto adjacency = std::vector<EdgeID>{};
    auto local_edges = EdgeID{0};
    for (auto low = 0; low < size; low += window) {
      auto const high = std::min(size, low + window);
      auto const active = rank >= low && rank < high;
      auto descriptor = graph_file_descriptor{
          active ? ::open(filename.c_str(), O_RDONLY) : -1};
      require_collective_backend_success(
          !active || descriptor, communicator,
          "unable to open edge-balanced binary graph payload",
          "MPI_Allreduce(edge-balanced payload open status)");

      auto local_success = true;
      if (active) {
        local_success =
            descriptor_has_extent(descriptor.get(), layout->file_extent) &&
            read_graph_exact(descriptor.get(), std::span<ULONG>{local_offsets},
                             *local_position) &&
            detail::offsets_are_valid(local_offsets, *layout, false, false);
        if (local_success) {
          local_edges =
              (local_offsets.back() - local_offsets.front()) / sizeof(ULONG);
          local_success = std::in_range<std::size_t>(local_edges);
        }
        if (local_success) {
          adjacency.resize(static_cast<std::size_t>(local_edges));
          local_success =
              read_graph_exact(descriptor.get(), std::span<EdgeID>{adjacency},
                               local_offsets.front()) &&
              detail::targets_are_valid(adjacency, global_nodes);
        }
        auto const close_success = descriptor.close();
        local_success = local_success && close_success;
      }
      require_collective_backend_success(
          local_success, communicator,
          "edge-balanced binary graph payload validation failed",
          "MPI_Allreduce(edge-balanced payload read status)");
    }
    validate_offset_slices(
        local_offsets, *layout, communicator,
        "MPI_Allgather(edge-balanced payload offset slices)");
    auto edge_ranges =
        gather_edge_ranges(local_edges, global_edges, communicator);

    auto local_permutation = std::vector<EdgeID>(adjacency.size());
    require_collective_backend_success(
        detail::canonicalize_adjacency(local_offsets, adjacency,
                                       local_permutation),
        communicator,
        "edge-balanced binary graph adjacency canonicalization failed",
        "MPI_Allreduce(edge-balanced canonicalization status)");

    permutation = std::move(local_permutation);
    construct_graph(graph, global_nodes, global_edges, interval, local_offsets,
                    adjacency, node_ranges, edge_ranges);
  } catch (...) {
    mpi::abort_on_exception(communicator.native_handle(),
                            "edge-balanced binary graph input failed");
  }
}

void edge_balanced_graph_io::read_binary_graph_edge_balanced(
    parallel_graph_access& graph,
    std::string const& filename,
    PPartitionConfig const& config,
    std::vector<EdgeID>& permutation,
    int supplied_rank,
    int supplied_size) {
  auto const communicator = mpi::communicator_view{graph.getCommunicator()};
  mpi::require_live_intracommunicator(
      communicator,
      "edge-balanced graph input requires a live graph communicator");
  validate_legacy_rank_and_size(supplied_rank, supplied_size, communicator);
  read_binary_graph_edge_balanced(graph, filename, config, permutation,
                                  communicator);
}

void edge_balanced_graph_io::read_binary_graph_edge_balanced(
    parallel_graph_access& graph,
    std::string const& filename,
    PPartitionConfig const& config,
    std::vector<EdgeID>& permutation) {
  read_binary_graph_edge_balanced(
      graph, filename, config, permutation,
      mpi::communicator_view{graph.getCommunicator()});
}
}  // namespace parhip
