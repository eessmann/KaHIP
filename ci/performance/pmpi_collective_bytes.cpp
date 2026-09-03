// Standalone PMPI interposer for logical collective payload accounting and
// distributed-graph topology-construction timing.
//
// Build independently with an MPI compiler wrapper and inject with LD_PRELOAD.
// This file intentionally has no KaHIP or CMake dependency.

#include <mpi.h>

#include <dlfcn.h>
#include <fcntl.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <time.h>
#include <unistd.h>

namespace {

using wide_count = unsigned __int128;

enum class operation : std::size_t {
  alltoall,
  alltoallv,
  ialltoallv,
  neighbor_alltoall,
  neighbor_alltoallv,
  ineighbor_alltoallv,
  alltoallv_c,
  ialltoallv_c,
  neighbor_alltoallv_c,
  ineighbor_alltoallv_c,
  neighbor_alltoallv_persistent,
  neighbor_alltoallv_persistent_c,
  count,
};

constexpr auto operation_count = static_cast<std::size_t>(operation::count);

constexpr auto operation_names = std::array{
    "MPI_Alltoall",
    "MPI_Alltoallv",
    "MPI_Ialltoallv",
    "MPI_Neighbor_alltoall",
    "MPI_Neighbor_alltoallv",
    "MPI_Ineighbor_alltoallv",
    "MPI_Alltoallv_c",
    "MPI_Ialltoallv_c",
    "MPI_Neighbor_alltoallv_c",
    "MPI_Ineighbor_alltoallv_c",
    "MPI_Neighbor_alltoallv_init",
    "MPI_Neighbor_alltoallv_init_c",
};
static_assert(operation_names.size() == operation_count);

enum class topology_operation : std::size_t {
  distributed_graph,
  distributed_graph_adjacent,
  count,
};

constexpr auto topology_operation_count =
    static_cast<std::size_t>(topology_operation::count);

constexpr auto topology_operation_names = std::array{
    "MPI_Dist_graph_create",
    "MPI_Dist_graph_create_adjacent",
};
static_assert(topology_operation_names.size() == topology_operation_count);

struct traffic {
  wide_count sent{};
  wide_count received{};
  wide_count self_sent{};
  wide_count self_received{};
};

struct counter {
  wide_count calls{};
  traffic bytes{};
};

struct duration_counter {
  wide_count calls{};
  wide_count elapsed_nanoseconds{};
};

struct persistent_record {
  operation kind{};
  traffic bytes{};
  bool valid{};
};

struct accounting_state {
  std::mutex mutex;
  std::array<counter, operation_count> counters{};
  std::array<duration_counter, topology_operation_count> topology_counters{};
  std::unordered_map<MPI_Fint, persistent_record> persistent;
  bool complete{true};
  std::string first_error;
};

auto state() -> accounting_state& {
  static accounting_state instance;
  return instance;
}

constexpr auto wide_max = ~wide_count{};

auto checked_add(wide_count left, wide_count right, wide_count& result) noexcept
    -> bool {
  if (right > wide_max - left) {
    return false;
  }
  result = left + right;
  return true;
}

auto checked_multiply(wide_count left,
                      wide_count right,
                      wide_count& result) noexcept -> bool {
  if (left != 0 && right > wide_max / left) {
    return false;
  }
  result = left * right;
  return true;
}

void mark_error(std::string_view message) noexcept {
  try {
    auto& shared = state();
    auto lock = std::scoped_lock{shared.mutex};
    shared.complete = false;
    if (shared.first_error.empty()) {
      shared.first_error.assign(message);
    }
  } catch (...) {
    // Instrumentation must never alter the MPI call's behavior.
  }
}

auto add_traffic(traffic& target, traffic const& value) noexcept -> bool {
  return checked_add(target.sent, value.sent, target.sent) &&
         checked_add(target.received, value.received, target.received) &&
         checked_add(target.self_sent, value.self_sent, target.self_sent) &&
         checked_add(target.self_received, value.self_received,
                     target.self_received);
}

void charge(operation kind, traffic const& bytes) noexcept {
  try {
    auto& shared = state();
    auto lock = std::scoped_lock{shared.mutex};
    auto& destination = shared.counters[static_cast<std::size_t>(kind)];
    if (!checked_add(destination.calls, wide_count{1}, destination.calls) ||
        !add_traffic(destination.bytes, bytes)) {
      shared.complete = false;
      if (shared.first_error.empty()) {
        shared.first_error = "collective byte counter overflow";
      }
    }
  } catch (...) {
    mark_error("cannot update collective byte counters");
  }
}

auto monotonic_nanoseconds() noexcept -> std::optional<wide_count> {
  auto timestamp = timespec{};
  if (::clock_gettime(CLOCK_MONOTONIC, &timestamp) != 0 ||
      timestamp.tv_sec < 0 || timestamp.tv_nsec < 0 ||
      timestamp.tv_nsec >= 1'000'000'000L) {
    mark_error("cannot read monotonic topology timer");
    return std::nullopt;
  }
  auto seconds = wide_count{};
  auto result = wide_count{};
  if (!checked_multiply(static_cast<wide_count>(timestamp.tv_sec),
                        wide_count{1'000'000'000}, seconds) ||
      !checked_add(seconds, static_cast<wide_count>(timestamp.tv_nsec),
                   result)) {
    mark_error("topology timer representation overflow");
    return std::nullopt;
  }
  return result;
}

void charge_topology(topology_operation kind,
                     std::optional<wide_count> started,
                     std::optional<wide_count> finished) noexcept {
  if (!started || !finished) {
    return;
  }
  if (*finished < *started) {
    mark_error("monotonic topology timer moved backwards");
    return;
  }
  try {
    auto& shared = state();
    auto lock = std::scoped_lock{shared.mutex};
    auto& destination =
        shared.topology_counters[static_cast<std::size_t>(kind)];
    auto const elapsed = *finished - *started;
    if (!checked_add(destination.calls, wide_count{1}, destination.calls) ||
        !checked_add(destination.elapsed_nanoseconds, elapsed,
                     destination.elapsed_nanoseconds)) {
      shared.complete = false;
      if (shared.first_error.empty()) {
        shared.first_error = "topology timing counter overflow";
      }
    }
  } catch (...) {
    mark_error("cannot update topology timing counters");
  }
}

auto datatype_size(MPI_Datatype datatype) noexcept
    -> std::optional<wide_count> {
  MPI_Count size{};
  if (PMPI_Type_size_x(datatype, &size) != MPI_SUCCESS || size < 0) {
    mark_error("PMPI_Type_size_x failed");
    return std::nullopt;
  }
  return static_cast<wide_count>(size);
}

template <typename Count>
auto nonnegative_count(Count value) noexcept -> std::optional<wide_count> {
  if (value < 0) {
    mark_error("negative collective count");
    return std::nullopt;
  }
  return static_cast<wide_count>(value);
}

template <typename Count>
auto add_count_bytes(traffic& bytes,
                     wide_count traffic::* field,
                     Count count,
                     wide_count type_size) noexcept -> bool {
  auto const normalized = nonnegative_count(count);
  if (!normalized) {
    return false;
  }
  wide_count product{};
  if (!checked_multiply(*normalized, type_size, product) ||
      !checked_add(bytes.*field, product, bytes.*field)) {
    mark_error("collective payload byte count overflow");
    return false;
  }
  return true;
}

template <typename Count>
auto dense_v_traffic(void const* send_buffer,
                     Count const* send_counts,
                     MPI_Datatype send_type,
                     Count const* receive_counts,
                     MPI_Datatype receive_type,
                     MPI_Comm communicator) noexcept -> std::optional<traffic> {
  int size{};
  int rank{};
  if (PMPI_Comm_size(communicator, &size) != MPI_SUCCESS || size < 0 ||
      PMPI_Comm_rank(communicator, &rank) != MPI_SUCCESS || rank < 0 ||
      rank >= size) {
    mark_error("cannot query dense communicator shape");
    return std::nullopt;
  }
  if (size != 0 && receive_counts == nullptr) {
    mark_error("null dense receive-count array");
    return std::nullopt;
  }
  auto const receive_size = datatype_size(receive_type);
  if (!receive_size) {
    return std::nullopt;
  }
  traffic bytes;
  for (int peer = 0; peer < size; ++peer) {
    if (!add_count_bytes(bytes, &traffic::received, receive_counts[peer],
                         *receive_size)) {
      return std::nullopt;
    }
    if (peer == rank && !add_count_bytes(bytes, &traffic::self_received,
                                         receive_counts[peer], *receive_size)) {
      return std::nullopt;
    }
  }
  if (send_buffer == MPI_IN_PLACE) {
    bytes.sent = bytes.received;
    bytes.self_sent = bytes.self_received;
    return bytes;
  }
  if (size != 0 && send_counts == nullptr) {
    mark_error("null dense send-count array");
    return std::nullopt;
  }
  auto const send_size = datatype_size(send_type);
  if (!send_size) {
    return std::nullopt;
  }
  for (int peer = 0; peer < size; ++peer) {
    if (!add_count_bytes(bytes, &traffic::sent, send_counts[peer],
                         *send_size)) {
      return std::nullopt;
    }
    if (peer == rank && !add_count_bytes(bytes, &traffic::self_sent,
                                         send_counts[peer], *send_size)) {
      return std::nullopt;
    }
  }
  return bytes;
}

auto dense_fixed_traffic(void const* send_buffer,
                         int send_count,
                         MPI_Datatype send_type,
                         int receive_count,
                         MPI_Datatype receive_type,
                         MPI_Comm communicator) noexcept
    -> std::optional<traffic> {
  int size{};
  int rank{};
  if (PMPI_Comm_size(communicator, &size) != MPI_SUCCESS || size < 0 ||
      PMPI_Comm_rank(communicator, &rank) != MPI_SUCCESS || rank < 0 ||
      rank >= size) {
    mark_error("cannot query dense communicator shape");
    return std::nullopt;
  }
  auto const receive_size = datatype_size(receive_type);
  auto const normalized_receive = nonnegative_count(receive_count);
  if (!receive_size || !normalized_receive) {
    return std::nullopt;
  }
  wide_count one_receive{};
  wide_count all_receives{};
  if (!checked_multiply(*normalized_receive, *receive_size, one_receive) ||
      !checked_multiply(one_receive, static_cast<wide_count>(size),
                        all_receives)) {
    mark_error("dense fixed-count payload overflow");
    return std::nullopt;
  }
  traffic bytes{.received = all_receives, .self_received = one_receive};
  if (send_buffer == MPI_IN_PLACE) {
    bytes.sent = bytes.received;
    bytes.self_sent = bytes.self_received;
    return bytes;
  }
  auto const send_size = datatype_size(send_type);
  auto const normalized_send = nonnegative_count(send_count);
  if (!send_size || !normalized_send) {
    return std::nullopt;
  }
  wide_count one_send{};
  if (!checked_multiply(*normalized_send, *send_size, one_send) ||
      !checked_multiply(one_send, static_cast<wide_count>(size), bytes.sent)) {
    mark_error("dense fixed-count payload overflow");
    return std::nullopt;
  }
  bytes.self_sent = one_send;
  return bytes;
}

struct neighborhood {
  int rank{};
  std::vector<int> sources;
  std::vector<int> destinations;
};

auto communicator_neighborhood(MPI_Comm communicator) noexcept
    -> std::optional<neighborhood> {
  neighborhood result;
  if (PMPI_Comm_rank(communicator, &result.rank) != MPI_SUCCESS) {
    mark_error("cannot query neighborhood rank");
    return std::nullopt;
  }
  int topology{};
  if (PMPI_Topo_test(communicator, &topology) != MPI_SUCCESS) {
    mark_error("PMPI_Topo_test failed");
    return std::nullopt;
  }
  if (topology == MPI_DIST_GRAPH) {
    int indegree{};
    int outdegree{};
    int weighted{};
    if (PMPI_Dist_graph_neighbors_count(communicator, &indegree, &outdegree,
                                        &weighted) != MPI_SUCCESS ||
        indegree < 0 || outdegree < 0) {
      mark_error("cannot query distributed-graph neighborhood size");
      return std::nullopt;
    }
    result.sources.resize(static_cast<std::size_t>(indegree));
    result.destinations.resize(static_cast<std::size_t>(outdegree));
    if (PMPI_Dist_graph_neighbors(communicator, indegree, result.sources.data(),
                                  MPI_UNWEIGHTED, outdegree,
                                  result.destinations.data(),
                                  MPI_UNWEIGHTED) != MPI_SUCCESS) {
      mark_error("cannot query distributed-graph neighbors");
      return std::nullopt;
    }
    return result;
  }
  if (topology == MPI_GRAPH) {
    int degree{};
    if (PMPI_Graph_neighbors_count(communicator, result.rank, &degree) !=
            MPI_SUCCESS ||
        degree < 0) {
      mark_error("cannot query graph neighborhood size");
      return std::nullopt;
    }
    result.sources.resize(static_cast<std::size_t>(degree));
    if (PMPI_Graph_neighbors(communicator, result.rank, degree,
                             result.sources.data()) != MPI_SUCCESS) {
      mark_error("cannot query graph neighbors");
      return std::nullopt;
    }
    result.destinations = result.sources;
    return result;
  }
  if (topology == MPI_CART) {
    int dimensions{};
    if (PMPI_Cartdim_get(communicator, &dimensions) != MPI_SUCCESS ||
        dimensions < 0) {
      mark_error("cannot query Cartesian neighborhood size");
      return std::nullopt;
    }
    result.sources.reserve(static_cast<std::size_t>(2 * dimensions));
    result.destinations.reserve(static_cast<std::size_t>(2 * dimensions));
    for (int dimension = 0; dimension < dimensions; ++dimension) {
      int negative{};
      int positive{};
      if (PMPI_Cart_shift(communicator, dimension, 1, &negative, &positive) !=
          MPI_SUCCESS) {
        mark_error("cannot query Cartesian neighbors");
        return std::nullopt;
      }
      result.sources.push_back(negative);
      result.sources.push_back(positive);
      result.destinations.push_back(negative);
      result.destinations.push_back(positive);
    }
    return result;
  }
  mark_error("neighborhood collective used on communicator without topology");
  return std::nullopt;
}

template <typename Count>
auto neighbor_v_traffic(void const* send_buffer,
                        Count const* send_counts,
                        MPI_Datatype send_type,
                        Count const* receive_counts,
                        MPI_Datatype receive_type,
                        MPI_Comm communicator) noexcept
    -> std::optional<traffic> {
  if (send_buffer == MPI_IN_PLACE) {
    mark_error("MPI_IN_PLACE is invalid for neighborhood collectives");
    return std::nullopt;
  }
  auto const neighbors = communicator_neighborhood(communicator);
  auto const send_size = datatype_size(send_type);
  auto const receive_size = datatype_size(receive_type);
  if (!neighbors || !send_size || !receive_size) {
    return std::nullopt;
  }
  if ((!neighbors->destinations.empty() && send_counts == nullptr) ||
      (!neighbors->sources.empty() && receive_counts == nullptr)) {
    mark_error("null neighborhood count array");
    return std::nullopt;
  }
  traffic bytes;
  for (std::size_t index = 0; index < neighbors->destinations.size(); ++index) {
    auto const peer = neighbors->destinations[index];
    if (peer == MPI_PROC_NULL) {
      continue;
    }
    if (!add_count_bytes(bytes, &traffic::sent, send_counts[index],
                         *send_size)) {
      return std::nullopt;
    }
    if (peer == neighbors->rank &&
        !add_count_bytes(bytes, &traffic::self_sent, send_counts[index],
                         *send_size)) {
      return std::nullopt;
    }
  }
  for (std::size_t index = 0; index < neighbors->sources.size(); ++index) {
    auto const peer = neighbors->sources[index];
    if (peer == MPI_PROC_NULL) {
      continue;
    }
    if (!add_count_bytes(bytes, &traffic::received, receive_counts[index],
                         *receive_size)) {
      return std::nullopt;
    }
    if (peer == neighbors->rank &&
        !add_count_bytes(bytes, &traffic::self_received, receive_counts[index],
                         *receive_size)) {
      return std::nullopt;
    }
  }
  return bytes;
}

auto neighbor_fixed_traffic(void const* send_buffer,
                            int send_count,
                            MPI_Datatype send_type,
                            int receive_count,
                            MPI_Datatype receive_type,
                            MPI_Comm communicator) noexcept
    -> std::optional<traffic> {
  if (send_buffer == MPI_IN_PLACE) {
    mark_error("MPI_IN_PLACE is invalid for neighborhood collectives");
    return std::nullopt;
  }
  auto const neighbors = communicator_neighborhood(communicator);
  auto const send_size = datatype_size(send_type);
  auto const receive_size = datatype_size(receive_type);
  auto const normalized_send = nonnegative_count(send_count);
  auto const normalized_receive = nonnegative_count(receive_count);
  if (!neighbors || !send_size || !receive_size || !normalized_send ||
      !normalized_receive) {
    return std::nullopt;
  }
  wide_count send_bytes{};
  wide_count receive_bytes{};
  if (!checked_multiply(*normalized_send, *send_size, send_bytes) ||
      !checked_multiply(*normalized_receive, *receive_size, receive_bytes)) {
    mark_error("neighborhood fixed-count payload overflow");
    return std::nullopt;
  }
  traffic bytes;
  for (auto const peer : neighbors->destinations) {
    if (peer == MPI_PROC_NULL) {
      continue;
    }
    if (!checked_add(bytes.sent, send_bytes, bytes.sent)) {
      mark_error("neighborhood fixed-count payload overflow");
      return std::nullopt;
    }
    if (peer == neighbors->rank &&
        !checked_add(bytes.self_sent, send_bytes, bytes.self_sent)) {
      mark_error("neighborhood fixed-count payload overflow");
      return std::nullopt;
    }
  }
  for (auto const peer : neighbors->sources) {
    if (peer == MPI_PROC_NULL) {
      continue;
    }
    if (!checked_add(bytes.received, receive_bytes, bytes.received)) {
      mark_error("neighborhood fixed-count payload overflow");
      return std::nullopt;
    }
    if (peer == neighbors->rank &&
        !checked_add(bytes.self_received, receive_bytes, bytes.self_received)) {
      mark_error("neighborhood fixed-count payload overflow");
      return std::nullopt;
    }
  }
  return bytes;
}

auto request_key(MPI_Request request) noexcept -> MPI_Fint {
  return PMPI_Request_c2f(request);
}

void remember_persistent(MPI_Request request,
                         operation kind,
                         std::optional<traffic> const& bytes) noexcept {
  if (request == MPI_REQUEST_NULL) {
    mark_error("persistent collective init returned MPI_REQUEST_NULL");
    return;
  }
  try {
    auto& shared = state();
    auto lock = std::scoped_lock{shared.mutex};
    auto const key = request_key(request);
    auto const [_, inserted] = shared.persistent.emplace(
        key, persistent_record{.kind = kind,
                               .bytes = bytes.value_or(traffic{}),
                               .valid = bytes.has_value()});
    if (!inserted) {
      shared.complete = false;
      if (shared.first_error.empty()) {
        shared.first_error = "duplicate persistent MPI request identity";
      }
    }
  } catch (...) {
    mark_error("cannot retain persistent collective metadata");
  }
}

template <typename Function>
auto resolve_optional_pmpi(char const* name) noexcept -> Function {
  auto* symbol = ::dlsym(RTLD_NEXT, name);
  Function function{};
  static_assert(sizeof(function) == sizeof(symbol));
  std::memcpy(&function, &symbol, sizeof(function));
  if (function == nullptr) {
    mark_error(std::string{"cannot resolve "} + name);
  }
  return function;
}

void charge_persistent(MPI_Request request) noexcept {
  try {
    auto& shared = state();
    auto lock = std::scoped_lock{shared.mutex};
    auto const found = shared.persistent.find(request_key(request));
    if (found == shared.persistent.end()) {
      return;  // May be a persistent point-to-point request.
    }
    if (!found->second.valid) {
      shared.complete = false;
      return;
    }
    auto& destination =
        shared.counters[static_cast<std::size_t>(found->second.kind)];
    if (!checked_add(destination.calls, wide_count{1}, destination.calls) ||
        !add_traffic(destination.bytes, found->second.bytes)) {
      shared.complete = false;
      if (shared.first_error.empty()) {
        shared.first_error = "persistent collective byte counter overflow";
      }
    }
  } catch (...) {
    mark_error("cannot charge persistent collective metadata");
  }
}

void forget_request(MPI_Request request) noexcept {
  try {
    auto& shared = state();
    auto lock = std::scoped_lock{shared.mutex};
    shared.persistent.erase(request_key(request));
  } catch (...) {
    mark_error("cannot erase persistent collective metadata");
  }
}

auto wide_string(wide_count value) -> std::string {
  if (value == 0) {
    return "0";
  }
  std::string result;
  while (value != 0) {
    result.push_back(static_cast<char>('0' + value % 10));
    value /= 10;
  }
  std::reverse(result.begin(), result.end());
  return result;
}

auto json_escape(std::string_view value) -> std::string {
  std::ostringstream output;
  for (auto const character : value) {
    switch (character) {
      case '\\':
        output << "\\\\";
        break;
      case '"':
        output << "\\\"";
        break;
      case '\n':
        output << "\\n";
        break;
      case '\r':
        output << "\\r";
        break;
      case '\t':
        output << "\\t";
        break;
      default:
        if (static_cast<unsigned char>(character) < 0x20) {
          output << "?";
        } else {
          output << character;
        }
    }
  }
  return output.str();
}

struct state_snapshot {
  std::array<counter, operation_count> counters{};
  std::array<duration_counter, topology_operation_count> topology_counters{};
  bool complete{};
  std::string first_error;
  std::size_t live_persistent_requests{};
};

auto snapshot() -> state_snapshot {
  auto& shared = state();
  auto lock = std::scoped_lock{shared.mutex};
  return {.counters = shared.counters,
          .topology_counters = shared.topology_counters,
          .complete = shared.complete,
          .first_error = shared.first_error,
          .live_persistent_requests = shared.persistent.size()};
}

auto render_json(int rank,
                 std::string_view hostname,
                 state_snapshot const& data) -> std::string {
  traffic totals;
  wide_count calls{};
  for (auto const& value : data.counters) {
    checked_add(calls, value.calls, calls);
    add_traffic(totals, value.bytes);
  }
  auto topology_totals = duration_counter{};
  for (auto const& value : data.topology_counters) {
    checked_add(topology_totals.calls, value.calls, topology_totals.calls);
    checked_add(topology_totals.elapsed_nanoseconds,
                value.elapsed_nanoseconds,
                topology_totals.elapsed_nanoseconds);
  }
  std::ostringstream output;
  output << "{\"schema_version\":1,\"rank\":" << rank
         << ",\"pid\":" << static_cast<long long>(::getpid())
         << ",\"hostname\":\"" << json_escape(hostname) << "\""
         << ",\"complete\":" << (data.complete ? "true" : "false")
         << ",\"error\":";
  if (data.first_error.empty()) {
    output << "null";
  } else {
    output << "\"" << json_escape(data.first_error) << "\"";
  }
  output << ",\"live_persistent_requests\":" << data.live_persistent_requests
         << ",\"operations\": [";
  for (std::size_t index = 0; index < data.counters.size(); ++index) {
    if (index != 0) {
      output << ',';
    }
    auto const& value = data.counters[index];
    output << "{\"name\":\"" << operation_names[index] << "\""
           << ",\"calls\":" << wide_string(value.calls)
           << ",\"sent_bytes\":" << wide_string(value.bytes.sent)
           << ",\"received_bytes\":" << wide_string(value.bytes.received)
           << ",\"self_sent_bytes\":" << wide_string(value.bytes.self_sent)
           << ",\"self_received_bytes\":"
           << wide_string(value.bytes.self_received) << '}';
  }
  output << "],\"topology_setup\":{\"operations\":[";
  for (std::size_t index = 0; index < data.topology_counters.size(); ++index) {
    if (index != 0) {
      output << ',';
    }
    auto const& value = data.topology_counters[index];
    output << "{\"name\":\"" << topology_operation_names[index] << "\""
           << ",\"calls\":" << wide_string(value.calls)
           << ",\"elapsed_nanoseconds\":"
           << wide_string(value.elapsed_nanoseconds) << '}';
  }
  output << "],\"totals\":{\"calls\":"
         << wide_string(topology_totals.calls)
         << ",\"elapsed_nanoseconds\":"
         << wide_string(topology_totals.elapsed_nanoseconds)
         << "}},\"totals\":{\"calls\":" << wide_string(calls)
         << ",\"sent_bytes\":" << wide_string(totals.sent)
         << ",\"received_bytes\":" << wide_string(totals.received)
         << ",\"self_sent_bytes\":" << wide_string(totals.self_sent)
         << ",\"self_received_bytes\":" << wide_string(totals.self_received)
         << "}}\n";
  return output.str();
}

auto write_all(int descriptor, std::string_view value) noexcept -> bool {
  std::size_t offset{};
  while (offset < value.size()) {
    auto const written =
        ::write(descriptor, value.data() + offset, value.size() - offset);
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written <= 0) {
      return false;
    }
    offset += static_cast<std::size_t>(written);
  }
  return true;
}

void error_marker(int rank, std::string_view message) noexcept {
  auto const record = "KAHIP_PMPI_BYTES_ERROR rank=" + std::to_string(rank) +
                      " message=" + std::string(message) + "\n";
  write_all(STDERR_FILENO, record);
}

auto write_rank_record(int rank, std::string const& record) noexcept -> bool {
  auto const* raw_directory = std::getenv("KAHIP_PMPI_BYTES_DIRECTORY");
  if (raw_directory == nullptr || *raw_directory == '\0') {
    error_marker(rank, "KAHIP_PMPI_BYTES_DIRECTORY is unset");
    return false;
  }
  auto const directory = std::string{raw_directory};
  auto const final_path = directory + "/rank-" + std::to_string(rank) + ".json";
  auto const temporary_path = directory + "/.rank-" + std::to_string(rank) +
                              "." + std::to_string(::getpid()) + ".tmp";
  auto const descriptor =
      ::open(temporary_path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (descriptor < 0) {
    error_marker(rank, std::strerror(errno));
    return false;
  }
  auto success = write_all(descriptor, record);
  if (success && ::fsync(descriptor) != 0) {
    success = false;
  }
  if (::close(descriptor) != 0) {
    success = false;
  }
  if (success && ::link(temporary_path.c_str(), final_path.c_str()) != 0) {
    success = false;
  }
  auto const saved_error = errno;
  ::unlink(temporary_path.c_str());
  if (!success) {
    error_marker(rank, std::strerror(saved_error));
  }
  return success;
}

void emit_rank_record() noexcept {
  int rank{-1};
  if (PMPI_Comm_rank(MPI_COMM_WORLD, &rank) != MPI_SUCCESS) {
    error_marker(rank, "PMPI_Comm_rank failed during accounting output");
    return;
  }
  std::array<char, 256> hostname{};
  if (::gethostname(hostname.data(), hostname.size()) != 0) {
    hostname.front() = '?';
    hostname[1] = '\0';
  } else {
    hostname.back() = '\0';
  }
  try {
    auto data = snapshot();
    if (data.live_persistent_requests != 0) {
      data.complete = false;
      if (data.first_error.empty()) {
        data.first_error =
            "persistent collective requests still live at finalize";
      }
    }
    write_rank_record(rank, render_json(rank, hostname.data(), data));
  } catch (...) {
    error_marker(rank, "cannot render collective accounting record");
  }
}

}  // namespace

extern "C" int MPI_Dist_graph_create(MPI_Comm old_communicator,
                                      int source_count,
                                      int const sources[],
                                      int const degrees[],
                                      int const destinations[],
                                      int const weights[],
                                      MPI_Info info,
                                      int reorder,
                                      MPI_Comm* graph_communicator) {
  auto const started = monotonic_nanoseconds();
  auto const result = PMPI_Dist_graph_create(
      old_communicator, source_count, sources, degrees, destinations, weights,
      info, reorder, graph_communicator);
  auto const finished = monotonic_nanoseconds();
  if (result == MPI_SUCCESS) {
    charge_topology(topology_operation::distributed_graph, started, finished);
  }
  return result;
}

extern "C" int MPI_Dist_graph_create_adjacent(
    MPI_Comm old_communicator,
    int indegree,
    int const sources[],
    int const source_weights[],
    int outdegree,
    int const destinations[],
    int const destination_weights[],
    MPI_Info info,
    int reorder,
    MPI_Comm* graph_communicator) {
  auto const started = monotonic_nanoseconds();
  auto const result = PMPI_Dist_graph_create_adjacent(
      old_communicator, indegree, sources, source_weights, outdegree,
      destinations, destination_weights, info, reorder, graph_communicator);
  auto const finished = monotonic_nanoseconds();
  if (result == MPI_SUCCESS) {
    charge_topology(topology_operation::distributed_graph_adjacent, started,
                    finished);
  }
  return result;
}

extern "C" int MPI_Alltoall(void const* send_buffer,
                            int send_count,
                            MPI_Datatype send_type,
                            void* receive_buffer,
                            int receive_count,
                            MPI_Datatype receive_type,
                            MPI_Comm communicator) {
  auto const bytes =
      dense_fixed_traffic(send_buffer, send_count, send_type, receive_count,
                          receive_type, communicator);
  auto const result =
      PMPI_Alltoall(send_buffer, send_count, send_type, receive_buffer,
                    receive_count, receive_type, communicator);
  if (result == MPI_SUCCESS && bytes) {
    charge(operation::alltoall, *bytes);
  }
  return result;
}

extern "C" int MPI_Alltoallv(void const* send_buffer,
                             int const send_counts[],
                             int const send_displacements[],
                             MPI_Datatype send_type,
                             void* receive_buffer,
                             int const receive_counts[],
                             int const receive_displacements[],
                             MPI_Datatype receive_type,
                             MPI_Comm communicator) {
  auto const bytes =
      dense_v_traffic(send_buffer, send_counts, send_type, receive_counts,
                      receive_type, communicator);
  auto const result = PMPI_Alltoallv(
      send_buffer, send_counts, send_displacements, send_type, receive_buffer,
      receive_counts, receive_displacements, receive_type, communicator);
  if (result == MPI_SUCCESS && bytes) {
    charge(operation::alltoallv, *bytes);
  }
  return result;
}

extern "C" int MPI_Ialltoallv(void const* send_buffer,
                              int const send_counts[],
                              int const send_displacements[],
                              MPI_Datatype send_type,
                              void* receive_buffer,
                              int const receive_counts[],
                              int const receive_displacements[],
                              MPI_Datatype receive_type,
                              MPI_Comm communicator,
                              MPI_Request* request) {
  auto const bytes =
      dense_v_traffic(send_buffer, send_counts, send_type, receive_counts,
                      receive_type, communicator);
  auto const result =
      PMPI_Ialltoallv(send_buffer, send_counts, send_displacements, send_type,
                      receive_buffer, receive_counts, receive_displacements,
                      receive_type, communicator, request);
  if (result == MPI_SUCCESS && bytes) {
    charge(operation::ialltoallv, *bytes);
  }
  return result;
}

extern "C" int MPI_Neighbor_alltoall(void const* send_buffer,
                                     int send_count,
                                     MPI_Datatype send_type,
                                     void* receive_buffer,
                                     int receive_count,
                                     MPI_Datatype receive_type,
                                     MPI_Comm communicator) {
  auto const bytes =
      neighbor_fixed_traffic(send_buffer, send_count, send_type, receive_count,
                             receive_type, communicator);
  auto const result =
      PMPI_Neighbor_alltoall(send_buffer, send_count, send_type, receive_buffer,
                             receive_count, receive_type, communicator);
  if (result == MPI_SUCCESS && bytes) {
    charge(operation::neighbor_alltoall, *bytes);
  }
  return result;
}

extern "C" int MPI_Neighbor_alltoallv(void const* send_buffer,
                                      int const send_counts[],
                                      int const send_displacements[],
                                      MPI_Datatype send_type,
                                      void* receive_buffer,
                                      int const receive_counts[],
                                      int const receive_displacements[],
                                      MPI_Datatype receive_type,
                                      MPI_Comm communicator) {
  auto const bytes =
      neighbor_v_traffic(send_buffer, send_counts, send_type, receive_counts,
                         receive_type, communicator);
  auto const result = PMPI_Neighbor_alltoallv(
      send_buffer, send_counts, send_displacements, send_type, receive_buffer,
      receive_counts, receive_displacements, receive_type, communicator);
  if (result == MPI_SUCCESS && bytes) {
    charge(operation::neighbor_alltoallv, *bytes);
  }
  return result;
}

extern "C" int MPI_Ineighbor_alltoallv(void const* send_buffer,
                                       int const send_counts[],
                                       int const send_displacements[],
                                       MPI_Datatype send_type,
                                       void* receive_buffer,
                                       int const receive_counts[],
                                       int const receive_displacements[],
                                       MPI_Datatype receive_type,
                                       MPI_Comm communicator,
                                       MPI_Request* request) {
  auto const bytes =
      neighbor_v_traffic(send_buffer, send_counts, send_type, receive_counts,
                         receive_type, communicator);
  auto const result = PMPI_Ineighbor_alltoallv(
      send_buffer, send_counts, send_displacements, send_type, receive_buffer,
      receive_counts, receive_displacements, receive_type, communicator,
      request);
  if (result == MPI_SUCCESS && bytes) {
    charge(operation::ineighbor_alltoallv, *bytes);
  }
  return result;
}

extern "C" int MPI_Alltoallv_c(void const* send_buffer,
                               MPI_Count const send_counts[],
                               MPI_Aint const send_displacements[],
                               MPI_Datatype send_type,
                               void* receive_buffer,
                               MPI_Count const receive_counts[],
                               MPI_Aint const receive_displacements[],
                               MPI_Datatype receive_type,
                               MPI_Comm communicator) {
  using pmpi_function = int (*)(
      void const*, MPI_Count const[], MPI_Aint const[], MPI_Datatype, void*,
      MPI_Count const[], MPI_Aint const[], MPI_Datatype, MPI_Comm);
  static auto const pmpi =
      resolve_optional_pmpi<pmpi_function>("PMPI_Alltoallv_c");
  if (pmpi == nullptr) {
    return MPI_ERR_OTHER;
  }
  auto const bytes =
      dense_v_traffic(send_buffer, send_counts, send_type, receive_counts,
                      receive_type, communicator);
  auto const result = pmpi(send_buffer, send_counts, send_displacements,
                           send_type, receive_buffer, receive_counts,
                           receive_displacements, receive_type, communicator);
  if (result == MPI_SUCCESS && bytes) {
    charge(operation::alltoallv_c, *bytes);
  }
  return result;
}

extern "C" int MPI_Ialltoallv_c(void const* send_buffer,
                                MPI_Count const send_counts[],
                                MPI_Aint const send_displacements[],
                                MPI_Datatype send_type,
                                void* receive_buffer,
                                MPI_Count const receive_counts[],
                                MPI_Aint const receive_displacements[],
                                MPI_Datatype receive_type,
                                MPI_Comm communicator,
                                MPI_Request* request) {
  using pmpi_function =
      int (*)(void const*, MPI_Count const[], MPI_Aint const[], MPI_Datatype,
              void*, MPI_Count const[], MPI_Aint const[], MPI_Datatype,
              MPI_Comm, MPI_Request*);
  static auto const pmpi =
      resolve_optional_pmpi<pmpi_function>("PMPI_Ialltoallv_c");
  if (pmpi == nullptr) {
    return MPI_ERR_OTHER;
  }
  auto const bytes =
      dense_v_traffic(send_buffer, send_counts, send_type, receive_counts,
                      receive_type, communicator);
  auto const result =
      pmpi(send_buffer, send_counts, send_displacements, send_type,
           receive_buffer, receive_counts, receive_displacements, receive_type,
           communicator, request);
  if (result == MPI_SUCCESS && bytes) {
    charge(operation::ialltoallv_c, *bytes);
  }
  return result;
}

extern "C" int MPI_Neighbor_alltoallv_c(void const* send_buffer,
                                        MPI_Count const send_counts[],
                                        MPI_Aint const send_displacements[],
                                        MPI_Datatype send_type,
                                        void* receive_buffer,
                                        MPI_Count const receive_counts[],
                                        MPI_Aint const receive_displacements[],
                                        MPI_Datatype receive_type,
                                        MPI_Comm communicator) {
  using pmpi_function = int (*)(
      void const*, MPI_Count const[], MPI_Aint const[], MPI_Datatype, void*,
      MPI_Count const[], MPI_Aint const[], MPI_Datatype, MPI_Comm);
  static auto const pmpi =
      resolve_optional_pmpi<pmpi_function>("PMPI_Neighbor_alltoallv_c");
  if (pmpi == nullptr) {
    return MPI_ERR_OTHER;
  }
  auto const bytes =
      neighbor_v_traffic(send_buffer, send_counts, send_type, receive_counts,
                         receive_type, communicator);
  auto const result = pmpi(send_buffer, send_counts, send_displacements,
                           send_type, receive_buffer, receive_counts,
                           receive_displacements, receive_type, communicator);
  if (result == MPI_SUCCESS && bytes) {
    charge(operation::neighbor_alltoallv_c, *bytes);
  }
  return result;
}

extern "C" int MPI_Ineighbor_alltoallv_c(void const* send_buffer,
                                         MPI_Count const send_counts[],
                                         MPI_Aint const send_displacements[],
                                         MPI_Datatype send_type,
                                         void* receive_buffer,
                                         MPI_Count const receive_counts[],
                                         MPI_Aint const receive_displacements[],
                                         MPI_Datatype receive_type,
                                         MPI_Comm communicator,
                                         MPI_Request* request) {
  using pmpi_function =
      int (*)(void const*, MPI_Count const[], MPI_Aint const[], MPI_Datatype,
              void*, MPI_Count const[], MPI_Aint const[], MPI_Datatype,
              MPI_Comm, MPI_Request*);
  static auto const pmpi =
      resolve_optional_pmpi<pmpi_function>("PMPI_Ineighbor_alltoallv_c");
  if (pmpi == nullptr) {
    return MPI_ERR_OTHER;
  }
  auto const bytes =
      neighbor_v_traffic(send_buffer, send_counts, send_type, receive_counts,
                         receive_type, communicator);
  auto const result =
      pmpi(send_buffer, send_counts, send_displacements, send_type,
           receive_buffer, receive_counts, receive_displacements, receive_type,
           communicator, request);
  if (result == MPI_SUCCESS && bytes) {
    charge(operation::ineighbor_alltoallv_c, *bytes);
  }
  return result;
}

extern "C" int MPI_Neighbor_alltoallv_init(void const* send_buffer,
                                           int const send_counts[],
                                           int const send_displacements[],
                                           MPI_Datatype send_type,
                                           void* receive_buffer,
                                           int const receive_counts[],
                                           int const receive_displacements[],
                                           MPI_Datatype receive_type,
                                           MPI_Comm communicator,
                                           MPI_Info info,
                                           MPI_Request* request) {
  using pmpi_function = int (*)(void const*, int const[], int const[],
                                MPI_Datatype, void*, int const[], int const[],
                                MPI_Datatype, MPI_Comm, MPI_Info, MPI_Request*);
  static auto const pmpi =
      resolve_optional_pmpi<pmpi_function>("PMPI_Neighbor_alltoallv_init");
  if (pmpi == nullptr) {
    return MPI_ERR_OTHER;
  }
  auto const bytes =
      neighbor_v_traffic(send_buffer, send_counts, send_type, receive_counts,
                         receive_type, communicator);
  auto const result =
      pmpi(send_buffer, send_counts, send_displacements, send_type,
           receive_buffer, receive_counts, receive_displacements, receive_type,
           communicator, info, request);
  if (result == MPI_SUCCESS && request != nullptr) {
    remember_persistent(*request, operation::neighbor_alltoallv_persistent,
                        bytes);
  }
  return result;
}

extern "C" int MPI_Neighbor_alltoallv_init_c(
    void const* send_buffer,
    MPI_Count const send_counts[],
    MPI_Aint const send_displacements[],
    MPI_Datatype send_type,
    void* receive_buffer,
    MPI_Count const receive_counts[],
    MPI_Aint const receive_displacements[],
    MPI_Datatype receive_type,
    MPI_Comm communicator,
    MPI_Info info,
    MPI_Request* request) {
  using pmpi_function =
      int (*)(void const*, MPI_Count const[], MPI_Aint const[], MPI_Datatype,
              void*, MPI_Count const[], MPI_Aint const[], MPI_Datatype,
              MPI_Comm, MPI_Info, MPI_Request*);
  static auto const pmpi =
      resolve_optional_pmpi<pmpi_function>("PMPI_Neighbor_alltoallv_init_c");
  if (pmpi == nullptr) {
    return MPI_ERR_OTHER;
  }
  auto const bytes =
      neighbor_v_traffic(send_buffer, send_counts, send_type, receive_counts,
                         receive_type, communicator);
  auto const result =
      pmpi(send_buffer, send_counts, send_displacements, send_type,
           receive_buffer, receive_counts, receive_displacements, receive_type,
           communicator, info, request);
  if (result == MPI_SUCCESS && request != nullptr) {
    remember_persistent(*request, operation::neighbor_alltoallv_persistent_c,
                        bytes);
  }
  return result;
}

extern "C" int MPI_Start(MPI_Request* request) {
  auto const before = request == nullptr ? MPI_REQUEST_NULL : *request;
  auto const result = PMPI_Start(request);
  if (result == MPI_SUCCESS && before != MPI_REQUEST_NULL) {
    charge_persistent(before);
  }
  return result;
}

extern "C" int MPI_Startall(int count, MPI_Request requests[]) {
  std::vector<MPI_Request> before;
  try {
    if (count > 0 && requests != nullptr) {
      before.assign(requests, requests + count);
    }
  } catch (...) {
    mark_error("cannot snapshot MPI_Startall requests");
  }
  auto const result = PMPI_Startall(count, requests);
  if (result == MPI_SUCCESS) {
    for (auto const request : before) {
      if (request != MPI_REQUEST_NULL) {
        charge_persistent(request);
      }
    }
  }
  return result;
}

extern "C" int MPI_Request_free(MPI_Request* request) {
  auto const before = request == nullptr ? MPI_REQUEST_NULL : *request;
  auto const result = PMPI_Request_free(request);
  if (result == MPI_SUCCESS && before != MPI_REQUEST_NULL) {
    forget_request(before);
  }
  return result;
}

extern "C" int MPI_Finalize() {
  emit_rank_record();
  return PMPI_Finalize();
}
