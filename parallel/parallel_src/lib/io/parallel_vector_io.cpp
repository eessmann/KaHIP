/******************************************************************************
 * parallel_vector_io.cpp
 * *
 * Source of KaHIP -- Karlsruhe High Quality Partitioning.
 * Christian Schulz <christian.schulz.phone@gmail.com>
 *****************************************************************************/

#include <errno.h>
#include <fcntl.h>
#include <mpi.h>
#include <sys/types.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

#include "communication/mpi_failure.h"
#include "communication/mpi_fixed_broadcast.h"
#include "communication/mpi_handles.h"
#include "parallel_vector_io.h"
#include "tools/helpers.h"
namespace parhip {
namespace {
using mpi::communicator_view;

class file_descriptor final {
 public:
  file_descriptor() noexcept = default;
  explicit file_descriptor(int descriptor) noexcept : descriptor_(descriptor) {}
  ~file_descriptor() noexcept {
    if (descriptor_ >= 0) {
      static_cast<void>(::close(descriptor_));
    }
  }

  file_descriptor(file_descriptor const&) = delete;
  auto operator=(file_descriptor const&) -> file_descriptor& = delete;
  file_descriptor(file_descriptor&& other) noexcept
      : descriptor_(std::exchange(other.descriptor_, -1)) {}
  auto operator=(file_descriptor&& other) noexcept -> file_descriptor& {
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

[[nodiscard]] auto byte_offset(ULONG word) noexcept
    -> std::optional<std::uint64_t> {
  constexpr auto width = std::uint64_t{sizeof(ULONG)};
  if (word > std::numeric_limits<std::uint64_t>::max() / width) {
    return std::nullopt;
  }
  return static_cast<std::uint64_t>(word) * width;
}

[[nodiscard]] auto byte_offset(ULONG prefix, ULONG index) noexcept
    -> std::optional<std::uint64_t> {
  if (index > std::numeric_limits<ULONG>::max() - prefix) {
    return std::nullopt;
  }
  return byte_offset(prefix + index);
}

[[nodiscard]] auto read_exact(int descriptor,
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

[[nodiscard]] auto write_exact(int descriptor,
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
[[nodiscard]] auto read_exact(int descriptor,
                              std::span<T> values,
                              std::uint64_t offset) noexcept -> bool {
  static_assert(std::is_trivially_copyable_v<T>);
  return read_exact(descriptor, std::as_writable_bytes(values), offset);
}

template <typename T>
[[nodiscard]] auto write_exact(int descriptor,
                               std::span<T const> values,
                               std::uint64_t offset) noexcept -> bool {
  static_assert(std::is_trivially_copyable_v<T>);
  return write_exact(descriptor, std::as_bytes(values), offset);
}

void require_collective_io_success(
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

void require_common_filename(std::string_view filename,
                             communicator_view communicator,
                             std::string_view diagnostic) {
  auto size = std::uint64_t{0};
  if (communicator.rank() == ROOT) {
    size = filename.size();
  }
  mpi::broadcast_fixed(size, ROOT, communicator,
                       "MPI_Bcast(partition I/O filename size)");
  if (!std::in_range<std::size_t>(size)) {
    mpi::abort_on_capacity_failure(communicator.native_handle(),
                                   "partition I/O filename",
                                   "filename size is not representable");
  }
  auto canonical = std::string(static_cast<std::size_t>(size), '\0');
  if (communicator.rank() == ROOT) {
    std::ranges::copy(filename, canonical.begin());
  }
  mpi::broadcast_bounded(std::span<char>{canonical}, ROOT, communicator,
                         "MPI_Bcast(partition I/O filename)");
  require_collective_io_success(
      filename == canonical, communicator, diagnostic,
      "MPI_Allreduce(partition I/O filename agreement)");
}

[[nodiscard]] auto effective_window(int configured,
                                    communicator_view communicator) noexcept
    -> int {
  auto const local = std::max(1, std::min(configured, communicator.size()));
  auto minimum = 0;
  auto maximum = 0;
  mpi::check_or_abort(MPI_Allreduce(&local, &minimum, 1, MPI_INT, MPI_MIN,
                                    communicator.native_handle()),
                      communicator.native_handle(),
                      "MPI_Allreduce(partition I/O window minimum)");
  mpi::check_or_abort(MPI_Allreduce(&local, &maximum, 1, MPI_INT, MPI_MAX,
                                    communicator.native_handle()),
                      communicator.native_handle(),
                      "MPI_Allreduce(partition I/O window maximum)");
  if (minimum != maximum) {
    mpi::abort_on_programming_error(
        communicator.native_handle(),
        "partition I/O window differs across communicator");
  }
  return minimum;
}

struct partition_layout final {
  ULONG global_nodes = 0;
  ULONG from = 0;
  ULONG local_nodes = 0;
};

[[nodiscard]] auto validated_layout(parallel_graph_access& graph,
                                    communicator_view communicator)
    -> partition_layout {
  auto const local = std::array<std::uint64_t, 3>{
      graph.number_of_global_nodes(), graph.get_from_range(),
      graph.number_of_local_nodes()};
  auto gathered = std::vector<std::uint64_t>(
      static_cast<std::size_t>(communicator.size()) * local.size());
  mpi::check_or_abort(
      MPI_Allgather(local.data(), static_cast<int>(local.size()), MPI_UINT64_T,
                    gathered.data(), static_cast<int>(local.size()),
                    MPI_UINT64_T, communicator.native_handle()),
      communicator.native_handle(),
      "MPI_Allgather(partition I/O graph layout)");

  auto const global_nodes = local[0];
  auto next = std::uint64_t{0};
  auto valid = true;
  for (auto rank = 0; rank < communicator.size(); ++rank) {
    auto const offset = static_cast<std::size_t>(rank) * local.size();
    auto const rank_global = gathered[offset];
    auto const rank_from = gathered[offset + 1];
    auto const rank_count = gathered[offset + 2];
    valid = valid && rank_global == global_nodes && rank_from == next &&
            next <= global_nodes && rank_count <= global_nodes - next;
    if (valid) {
      next += rank_count;
    }
  }
  valid = valid && next == global_nodes;
  if (!valid) {
    mpi::abort_on_programming_error(
        communicator.native_handle(),
        "partition I/O graph ranges do not form the global vertex order");
  }
  return partition_layout{local[0], local[1], local[2]};
}

[[nodiscard]] auto parse_partition_id(std::string_view line,
                                      ULONG& value) noexcept -> bool {
  auto const whitespace = [](char character) {
    return character == ' ' || character == '\t' || character == '\r';
  };
  while (!line.empty() && whitespace(line.front())) {
    line.remove_prefix(1);
  }
  while (!line.empty() && whitespace(line.back())) {
    line.remove_suffix(1);
  }
  if (line.empty()) {
    return false;
  }
  auto const result =
      std::from_chars(line.data(), line.data() + line.size(), value);
  return result.ec == std::errc{} && result.ptr == line.data() + line.size();
}
}  // namespace

parallel_vector_io::parallel_vector_io() = default;

parallel_vector_io::~parallel_vector_io() = default;

void parallel_vector_io::writePartitionBinaryParallelPosix(
    PPartitionConfig& config,
    parallel_graph_access& graph,
    std::string filename) {
  auto operation =
      mpi::communicator{communicator_view{graph.getCommunicator()}};
  auto const communicator = operation.view();
  try {
    auto const rank = communicator.rank();
    auto const size = communicator.size();
    require_common_filename(filename, communicator,
                            "partition binary payload I/O failed");
    auto const layout = validated_layout(graph, communicator);
    auto const window =
        effective_window(config.binary_io_window_size, communicator);

    auto header_success = true;
    if (rank == ROOT) {
      auto const header = std::array<ULONG, header_count_partition>{
          fileTypeVersionNumberPartition, layout.global_nodes};
      auto descriptor = file_descriptor{
          ::open(filename.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644)};
      auto const write_success =
          descriptor &&
          write_exact(descriptor.get(), std::span<ULONG const>{header}, 0);
      auto const close_success = descriptor && descriptor.close();
      header_success = write_success && close_success;
    }
    require_collective_io_success(
        header_success, communicator, "partition binary header I/O failed",
        "MPI_Allreduce(partition binary header I/O status)");

    auto labels =
        std::vector<ULONG>(static_cast<std::size_t>(layout.local_nodes));
    for (NodeID node = 0; node < graph.number_of_local_nodes(); ++node) {
      labels[static_cast<std::size_t>(node)] = graph.getNodeLabel(node);
    }
    auto const start = byte_offset(header_count_partition, layout.from);
    if (!start.has_value()) {
      mpi::abort_on_capacity_failure(
          communicator.native_handle(), "partition binary output",
          "partition byte offset is not representable");
    }

    for (auto low = 0; low < size; low += window) {
      auto const high = std::min(size, low + window);
      auto const active = rank >= low && rank < high;
      auto descriptor =
          file_descriptor{active ? ::open(filename.c_str(), O_WRONLY) : -1};
      require_collective_io_success(
          !active || descriptor, communicator,
          "partition binary payload I/O failed",
          "MPI_Allreduce(partition binary payload open status)");

      auto payload_success = true;
      if (active) {
        auto const write_success = write_exact(
            descriptor.get(), std::span<ULONG const>{labels}, *start);
        auto const close_success = descriptor.close();
        payload_success = write_success && close_success;
      }
      require_collective_io_success(
          payload_success, communicator, "partition binary payload I/O failed",
          "MPI_Allreduce(partition binary payload write status)");
    }
  } catch (...) {
    mpi::abort_on_exception(communicator.native_handle(),
                            "partition binary output failed");
  }
}

void parallel_vector_io::writePartitionBinaryParallel(
    PPartitionConfig& config,
    parallel_graph_access& graph,
    std::string filename) {
  writePartitionBinaryParallelPosix(config, graph, std::move(filename));
}

void parallel_vector_io::readPartitionBinaryParallel(
    PPartitionConfig& config,
    parallel_graph_access& graph,
    std::string filename) {
  auto operation =
      mpi::communicator{communicator_view{graph.getCommunicator()}};
  auto const communicator = operation.view();
  try {
    auto const rank = communicator.rank();
    auto const size = communicator.size();
    require_common_filename(filename, communicator,
                            "partition binary payload I/O failed");
    auto const layout = validated_layout(graph, communicator);
    auto const window =
        effective_window(config.binary_io_window_size, communicator);
    if (rank == ROOT) {
      std::cout << "reading binary partition" << std::endl;
    }

    auto header = std::array<ULONG, header_count_partition>{};
    auto header_success = true;
    if (rank == ROOT) {
      auto descriptor = file_descriptor{::open(filename.c_str(), O_RDONLY)};
      auto const read_success =
          descriptor &&
          read_exact(descriptor.get(), std::span<ULONG>{header}, 0);
      auto const close_success = descriptor && descriptor.close();
      header_success = read_success && close_success;
    }
    require_collective_io_success(
        header_success, communicator, "partition binary header I/O failed",
        "MPI_Allreduce(partition binary header I/O status)");
    mpi::broadcast_fixed(std::span{header}, ROOT, communicator,
                         "MPI_Bcast(partition binary header)");
    if (header[0] != fileTypeVersionNumberPartition ||
        header[1] != layout.global_nodes) {
      mpi::abort_on_backend_failure(communicator.native_handle(),
                                    "partition binary header is incompatible");
    }

    auto labels =
        std::vector<ULONG>(static_cast<std::size_t>(layout.local_nodes));
    auto const start = byte_offset(header_count_partition, layout.from);
    if (!start.has_value()) {
      mpi::abort_on_capacity_failure(
          communicator.native_handle(), "partition binary input",
          "partition byte offset is not representable");
    }

    for (auto low = 0; low < size; low += window) {
      auto const high = std::min(size, low + window);
      auto const active = rank >= low && rank < high;
      auto descriptor =
          file_descriptor{active ? ::open(filename.c_str(), O_RDONLY) : -1};
      require_collective_io_success(
          !active || descriptor, communicator,
          "partition binary payload I/O failed",
          "MPI_Allreduce(partition binary payload open status)");

      auto payload_success = true;
      if (active) {
        auto const read_success =
            read_exact(descriptor.get(), std::span<ULONG>{labels}, *start);
        auto const close_success = descriptor.close();
        payload_success = read_success && close_success;
      }
      require_collective_io_success(
          payload_success, communicator, "partition binary payload I/O failed",
          "MPI_Allreduce(partition binary payload read status)");
    }

    for (NodeID node = 0; node < graph.number_of_local_nodes(); ++node) {
      graph.setNodeLabel(node, labels[static_cast<std::size_t>(node)]);
    }
    graph.update_ghost_node_data_global();
  } catch (...) {
    mpi::abort_on_exception(communicator.native_handle(),
                            "partition binary input failed");
  }
}

void parallel_vector_io::writePartitionSimpleParallel(
    parallel_graph_access& graph,
    std::string filename) {
  auto operation =
      mpi::communicator{communicator_view{graph.getCommunicator()}};
  auto const communicator = operation.view();
  try {
    auto const rank = communicator.rank();
    auto const size = communicator.size();
    require_common_filename(filename, communicator,
                            "partition text output I/O failed");
    static_cast<void>(validated_layout(graph, communicator));

    for (auto writer = 0; writer < size; ++writer) {
      auto output = std::ofstream{};
      auto open_success = true;
      if (rank == writer) {
        auto const mode = writer == ROOT ? std::ios::out | std::ios::trunc
                                         : std::ios::out | std::ios::app;
        output.open(filename, mode);
        open_success = static_cast<bool>(output);
      }
      require_collective_io_success(
          open_success, communicator, "partition text output I/O failed",
          "MPI_Allreduce(partition text output open status)");

      auto write_success = true;
      if (rank == writer) {
        for (NodeID node = 0; node < graph.number_of_local_nodes(); ++node) {
          output << graph.getNodeLabel(node) << '\n';
        }
        output.close();
        write_success = static_cast<bool>(output);
      }
      require_collective_io_success(
          write_success, communicator, "partition text output I/O failed",
          "MPI_Allreduce(partition text output write status)");
    }
  } catch (...) {
    mpi::abort_on_exception(communicator.native_handle(),
                            "partition text output failed");
  }
}
void parallel_vector_io::readPartition(PPartitionConfig& config,
                                       parallel_graph_access& graph,
                                       std::string filename) {
  if (hasEnding(filename, ".txtp")) {
    return readPartitionSimpleParallel(graph, std::move(filename));
  }
  if (hasEnding(filename, ".binp")) {
    return readPartitionBinaryParallel(config, graph, std::move(filename));
  }
}

void parallel_vector_io::readPartitionSimpleParallel(
    parallel_graph_access& graph,
    std::string filename) {
  auto operation =
      mpi::communicator{communicator_view{graph.getCommunicator()}};
  auto const communicator = operation.view();
  try {
    auto const rank = communicator.rank();
    require_common_filename(filename, communicator,
                            "partition text input I/O failed");
    auto const layout = validated_layout(graph, communicator);
    if (rank == ROOT) {
      std::cout << "reading text partition" << std::endl;
    }

    auto input = std::ifstream{filename};
    require_collective_io_success(
        static_cast<bool>(input), communicator,
        "partition text input I/O failed",
        "MPI_Allreduce(partition text input open status)");

    auto labels =
        std::vector<ULONG>(static_cast<std::size_t>(layout.local_nodes));
    auto line = std::string{};
    auto global = ULONG{0};
    auto parse_success = true;
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
      auto value = ULONG{0};
      if (global >= layout.global_nodes ||
          !parse_partition_id(content, value)) {
        parse_success = false;
        break;
      }
      if (global >= layout.from && global - layout.from < layout.local_nodes) {
        labels[static_cast<std::size_t>(global - layout.from)] = value;
      }
      ++global;
    }
    parse_success =
        parse_success && !input.bad() && global == layout.global_nodes;
    require_collective_io_success(
        parse_success, communicator, "partition text input I/O failed",
        "MPI_Allreduce(partition text input parse status)");

    for (NodeID node = 0; node < graph.number_of_local_nodes(); ++node) {
      graph.setNodeLabel(node, labels[static_cast<std::size_t>(node)]);
    }
    graph.update_ghost_node_data_global();
  } catch (...) {
    mpi::abort_on_exception(communicator.native_handle(),
                            "partition text input failed");
  }
}
}
