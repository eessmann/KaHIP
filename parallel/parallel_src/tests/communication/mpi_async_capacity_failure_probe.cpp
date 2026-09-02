#include <mpi.h>
#include <limits.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <vector>

#include "communication/mpi_adapter.h"
#include "kahip_mpi_capabilities.h"

namespace async_capacity_probe {
struct alignas(64) wire_record final {
  std::uint64_t value;
};

struct byte_wire_record final {
  unsigned char value;
};
}  // namespace async_capacity_probe

template <>
struct parhip::mpi::wire_members<async_capacity_probe::wire_record> {
  inline static constexpr auto value =
      std::tuple{&async_capacity_probe::wire_record::value};
};

template <>
struct parhip::mpi::wire_members<async_capacity_probe::byte_wire_record> {
  inline static constexpr auto value =
      std::tuple{&async_capacity_probe::byte_wire_record::value};
};

static_assert(std::is_standard_layout_v<async_capacity_probe::wire_record>);
static_assert(std::is_trivially_copyable_v<async_capacity_probe::wire_record>);
static_assert(alignof(async_capacity_probe::wire_record) == 64);
static_assert(
    std::is_standard_layout_v<async_capacity_probe::byte_wire_record>);
static_assert(
    std::is_trivially_copyable_v<async_capacity_probe::byte_wire_record>);
static_assert(sizeof(async_capacity_probe::byte_wire_record) == 1);

namespace {
enum class failure_mode : std::uint8_t {
  one_shot_receive_offset,
  one_shot_receive_byte,
  fixed_send_offset,
  fixed_send_byte,
};

auto selected_mode = failure_mode::one_shot_receive_offset;
auto cached_rank = -1;
auto cached_size = -1;
auto track_next_duplicate = false;
auto operation_communicator = MPI_COMM_NULL;
auto graph_communicator = MPI_COMM_NULL;
auto injection_is_armed = false;
auto payload_allocation_watch = false;
auto error_string_attempts = 0;
auto cleanup_attempts = 0;
auto capacity_bor_attempts = 0;
auto backend_band_attempts = 0;
auto count_exchange_attempts = 0;
auto payload_allocation_attempts = 0;
auto datatype_attempts = 0;
auto immediate_init_attempts = 0;
auto persistent_init_attempts = 0;
auto payload_collective_attempts = 0;
auto operation_duplicate_attempts = 0;

[[nodiscard]] auto is_receive_mode() noexcept -> bool {
  return selected_mode == failure_mode::one_shot_receive_offset ||
         selected_mode == failure_mode::one_shot_receive_byte;
}

[[nodiscard]] auto is_offset_mode() noexcept -> bool {
  return selected_mode == failure_mode::one_shot_receive_offset ||
         selected_mode == failure_mode::fixed_send_offset;
}

[[nodiscard]] auto affected_name(MPI_Comm communicator) noexcept
    -> std::string_view {
  if (communicator == operation_communicator) {
    return "async-operation";
  }
  if (communicator == graph_communicator) {
    return "async-graph";
  }
  if (communicator == MPI_COMM_WORLD) {
    return "world";
  }
  return "unexpected";
}

[[noreturn]] void forbidden(char const* operation) noexcept {
  std::fprintf(stderr, "forbidden async capacity action: %s\n", operation);
  std::_Exit(90);
}

[[noreturn]] void returned_from_failure() noexcept {
  std::fputs("returned-from-async-capacity-failure\n", stderr);
  std::_Exit(2);
}

void write_abort_observation(std::string_view affected) noexcept {
  constexpr auto buffer_capacity = std::size_t{512};
  static_assert(buffer_capacity <= PIPE_BUF);
  auto buffer = std::array<char, buffer_capacity>{};
  auto const length = std::snprintf(
      buffer.data(), buffer.size(),
      "observed MPI_Abort rank=%d affected=%.*s error-string-attempts=%d "
      "cleanup-attempts=%d capacity-bor-attempts=%d "
      "backend-band-attempts=%d count-exchange-attempts=%d "
      "payload-allocation-attempts=%d datatype-attempts=%d "
      "immediate-init-attempts=%d persistent-init-attempts=%d "
      "payload-collective-attempts=%d operation-duplicate-attempts=%d\n",
      cached_rank, static_cast<int>(affected.size()), affected.data(),
      error_string_attempts, cleanup_attempts, capacity_bor_attempts,
      backend_band_attempts, count_exchange_attempts,
      payload_allocation_attempts, datatype_attempts, immediate_init_attempts,
      persistent_init_attempts, payload_collective_attempts,
      operation_duplicate_attempts);
  if (length < 0 || static_cast<std::size_t>(length) >= buffer.size() ||
      ::write(STDERR_FILENO, buffer.data(), static_cast<std::size_t>(length)) !=
          length) {
    std::_Exit(89);
  }
}

[[nodiscard]] auto allocate_aligned(std::size_t size, std::size_t alignment)
    -> void* {
  auto const allocation_size = std::max(size, std::size_t{1});
  if (alignment <= alignof(std::max_align_t)) {
    if (auto* allocation = std::malloc(allocation_size);
        allocation != nullptr) {
      return allocation;
    }
    throw std::bad_alloc{};
  }
  auto* allocation = static_cast<void*>(nullptr);
  if (posix_memalign(&allocation, alignment, allocation_size) != 0) {
    throw std::bad_alloc{};
  }
  return allocation;
}
}  // namespace

static_assert(noexcept(write_abort_observation({})));

void* operator new(std::size_t size, std::align_val_t alignment) {
  if (payload_allocation_watch) {
    ++payload_allocation_attempts;
    forbidden("aligned payload allocation");
  }
  return allocate_aligned(size, static_cast<std::size_t>(alignment));
}

void operator delete(void* allocation, std::align_val_t) noexcept {
  std::free(allocation);
}

void operator delete(void* allocation, std::size_t, std::align_val_t) noexcept {
  std::free(allocation);
}

extern "C" int MPI_Comm_dup(MPI_Comm communicator,
                            MPI_Comm* duplicate_communicator) {
  auto const result = PMPI_Comm_dup(communicator, duplicate_communicator);
  if (payload_allocation_watch) {
    ++operation_duplicate_attempts;
    if (operation_duplicate_attempts > 1) {
      forbidden("extra async operation duplicate");
    }
  }
  if (result == MPI_SUCCESS && track_next_duplicate &&
      duplicate_communicator != nullptr) {
    operation_communicator = *duplicate_communicator;
    track_next_duplicate = false;
    std::fputs("captured async operation duplicate\n", stderr);
    if (!is_receive_mode()) {
      injection_is_armed = true;
    }
  }
  return result;
}

extern "C" int MPI_Neighbor_alltoall(void const* send_buffer,
                                     int send_count,
                                     MPI_Datatype send_datatype,
                                     void* receive_buffer,
                                     int receive_count,
                                     MPI_Datatype receive_datatype,
                                     MPI_Comm communicator) {
  auto const result = PMPI_Neighbor_alltoall(
      send_buffer, send_count, send_datatype, receive_buffer, receive_count,
      receive_datatype, communicator);
  if (communicator != operation_communicator) {
    return result;
  }
  ++count_exchange_attempts;
  if (result != MPI_SUCCESS || count_exchange_attempts != 1 ||
      cached_size != 2 || send_count != 1 || receive_count != 1 ||
      send_datatype != MPI_UINT64_T || receive_datatype != MPI_UINT64_T ||
      receive_buffer == nullptr) {
    forbidden("neighbor count exchange shape");
  }
  if (is_receive_mode() && cached_rank == 0) {
    auto* counts = static_cast<std::uint64_t*>(receive_buffer);
    if (is_offset_mode()) {
      counts[0] = std::numeric_limits<std::size_t>::max();
      counts[1] = std::uint64_t{1};
      std::fputs("injected rank-zero async receive offset capacity\n", stderr);
    } else {
      counts[0] = std::numeric_limits<std::size_t>::max() /
                      sizeof(async_capacity_probe::wire_record) +
                  std::uint64_t{1};
      counts[1] = std::uint64_t{0};
      std::fputs("injected rank-zero async receive byte capacity\n", stderr);
    }
  }
  if (is_receive_mode()) {
    injection_is_armed = true;
  }
  return result;
}

extern "C" int MPI_Allreduce(void const* send_buffer,
                             void* receive_buffer,
                             int count,
                             MPI_Datatype datatype,
                             MPI_Op operation,
                             MPI_Comm communicator) {
  if (communicator == operation_communicator && count == 2 &&
      datatype == MPI_UINT64_T && operation == MPI_BOR) {
    ++capacity_bor_attempts;
  }
  if (communicator == operation_communicator && count == 2 &&
      datatype == MPI_UINT64_T && operation == MPI_BAND) {
    ++backend_band_attempts;
  }
  if (injection_is_armed && communicator == operation_communicator &&
      ((operation == MPI_BOR && capacity_bor_attempts > 1) ||
       (operation == MPI_BAND && backend_band_attempts > 1))) {
    forbidden("duplicate async capacity/backend reduction");
  }
  return PMPI_Allreduce(send_buffer, receive_buffer, count, datatype, operation,
                        communicator);
}

extern "C" int MPI_Error_string(int error_code,
                                char* error_text,
                                int* error_text_length) {
  if (injection_is_armed) {
    ++error_string_attempts;
    forbidden("MPI_Error_string");
  }
  return PMPI_Error_string(error_code, error_text, error_text_length);
}

extern "C" int MPI_Comm_free(MPI_Comm* communicator) {
  if (injection_is_armed && communicator != nullptr &&
      *communicator == operation_communicator) {
    ++cleanup_attempts;
    forbidden("MPI_Comm_free(async operation duplicate)");
  }
  return PMPI_Comm_free(communicator);
}

extern "C" int MPI_Abort(MPI_Comm communicator, int) {
  if (!injection_is_armed) {
    forbidden("MPI_Abort before async capacity injection");
  }
  auto const affected = affected_name(communicator);
  if (std::fflush(stderr) != 0) {
    std::_Exit(89);
  }
  write_abort_observation(affected);
  std::_Exit(86);
}

void record_datatype_attempt(char const* operation) {
  if (injection_is_armed) {
    ++datatype_attempts;
    forbidden(operation);
  }
}

extern "C" int MPI_Get_address(void const* location, MPI_Aint* address) {
  record_datatype_attempt("MPI_Get_address(async datatype)");
  return PMPI_Get_address(location, address);
}

extern "C" int MPI_Type_create_struct(int count,
                                      int const block_lengths[],
                                      MPI_Aint const displacements[],
                                      MPI_Datatype const datatypes[],
                                      MPI_Datatype* new_datatype) {
  record_datatype_attempt("MPI_Type_create_struct(async datatype)");
  return PMPI_Type_create_struct(count, block_lengths, displacements, datatypes,
                                 new_datatype);
}

extern "C" int MPI_Type_create_resized(MPI_Datatype old_datatype,
                                       MPI_Aint lower_bound,
                                       MPI_Aint extent,
                                       MPI_Datatype* new_datatype) {
  record_datatype_attempt("MPI_Type_create_resized(async datatype)");
  return PMPI_Type_create_resized(old_datatype, lower_bound, extent,
                                  new_datatype);
}

extern "C" int MPI_Type_commit(MPI_Datatype* datatype) {
  record_datatype_attempt("MPI_Type_commit(async datatype)");
  return PMPI_Type_commit(datatype);
}

extern "C" int MPI_Type_free(MPI_Datatype* datatype) {
  record_datatype_attempt("MPI_Type_free(async datatype)");
  return PMPI_Type_free(datatype);
}

void record_immediate_attempt(char const* operation) {
  if (injection_is_armed) {
    ++immediate_init_attempts;
    ++payload_collective_attempts;
    forbidden(operation);
  }
}

void record_persistent_attempt(char const* operation) {
  if (injection_is_armed) {
    ++persistent_init_attempts;
    ++payload_collective_attempts;
    forbidden(operation);
  }
}

extern "C" int MPI_Ineighbor_alltoallv(void const* send_buffer,
                                       int const send_counts[],
                                       int const send_displacements[],
                                       MPI_Datatype send_datatype,
                                       void* receive_buffer,
                                       int const receive_counts[],
                                       int const receive_displacements[],
                                       MPI_Datatype receive_datatype,
                                       MPI_Comm communicator,
                                       MPI_Request* request) {
  record_immediate_attempt("MPI_Ineighbor_alltoallv(async payload)");
  return PMPI_Ineighbor_alltoallv(send_buffer, send_counts, send_displacements,
                                  send_datatype, receive_buffer, receive_counts,
                                  receive_displacements, receive_datatype,
                                  communicator, request);
}

#if KAHIP_HAVE_MPI_INEIGHBOR_ALLTOALLV_C
extern "C" int MPI_Ineighbor_alltoallv_c(void const* send_buffer,
                                         MPI_Count const send_counts[],
                                         MPI_Aint const send_displacements[],
                                         MPI_Datatype send_datatype,
                                         void* receive_buffer,
                                         MPI_Count const receive_counts[],
                                         MPI_Aint const receive_displacements[],
                                         MPI_Datatype receive_datatype,
                                         MPI_Comm communicator,
                                         MPI_Request* request) {
  record_immediate_attempt("MPI_Ineighbor_alltoallv_c(async payload)");
  return PMPI_Ineighbor_alltoallv_c(
      send_buffer, send_counts, send_displacements, send_datatype,
      receive_buffer, receive_counts, receive_displacements, receive_datatype,
      communicator, request);
}
#endif

#if KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_INIT
extern "C" int MPI_Neighbor_alltoallv_init(void const* send_buffer,
                                           int const send_counts[],
                                           int const send_displacements[],
                                           MPI_Datatype send_datatype,
                                           void* receive_buffer,
                                           int const receive_counts[],
                                           int const receive_displacements[],
                                           MPI_Datatype receive_datatype,
                                           MPI_Comm communicator,
                                           MPI_Info info,
                                           MPI_Request* request) {
  record_persistent_attempt("MPI_Neighbor_alltoallv_init(async payload)");
  return PMPI_Neighbor_alltoallv_init(
      send_buffer, send_counts, send_displacements, send_datatype,
      receive_buffer, receive_counts, receive_displacements, receive_datatype,
      communicator, info, request);
}
#endif

#if KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_INIT_C
extern "C" int MPI_Neighbor_alltoallv_init_c(
    void const* send_buffer,
    MPI_Count const send_counts[],
    MPI_Aint const send_displacements[],
    MPI_Datatype send_datatype,
    void* receive_buffer,
    MPI_Count const receive_counts[],
    MPI_Aint const receive_displacements[],
    MPI_Datatype receive_datatype,
    MPI_Comm communicator,
    MPI_Info info,
    MPI_Request* request) {
  record_persistent_attempt("MPI_Neighbor_alltoallv_init_c(async payload)");
  return PMPI_Neighbor_alltoallv_init_c(
      send_buffer, send_counts, send_displacements, send_datatype,
      receive_buffer, receive_counts, receive_displacements, receive_datatype,
      communicator, info, request);
}
#endif

namespace {
[[nodiscard]] auto parse_mode(std::string_view mode) -> bool {
  if (mode == "one-shot-receive-offset") {
    selected_mode = failure_mode::one_shot_receive_offset;
  } else if (mode == "one-shot-receive-byte") {
    selected_mode = failure_mode::one_shot_receive_byte;
  } else if (mode == "fixed-send-offset") {
    selected_mode = failure_mode::fixed_send_offset;
  } else if (mode == "fixed-send-byte") {
    selected_mode = failure_mode::fixed_send_byte;
  } else {
    return false;
  }
  return true;
}

[[noreturn]] void run_receive_failure(
    parhip::mpi::distributed_graph const& graph) {
  auto segments = std::vector<std::vector<async_capacity_probe::wire_record>>(
      graph.destinations().size(),
      std::vector{async_capacity_probe::wire_record{
          .value = static_cast<std::uint64_t>(cached_rank + 1)}});
  auto sends = parhip::mpi::segmented_buffer<
      async_capacity_probe::wire_record>::from_segments(segments);
  track_next_duplicate = true;
  payload_allocation_watch = true;
  static_cast<void>(
      parhip::mpi::start_neighbor_all_to_all_v(std::move(sends), graph));
  returned_from_failure();
}

[[noreturn]] void run_fixed_failure(
    parhip::mpi::distributed_graph const& graph) {
  auto counts = std::vector<std::size_t>(graph.destinations().size(), 1);
  if (cached_rank == 0) {
    if (selected_mode == failure_mode::fixed_send_offset) {
      counts[0] = std::numeric_limits<std::size_t>::max();
      counts[1] = std::size_t{1};
      std::fputs("armed rank-zero async fixed-send offset capacity\n", stderr);
    } else {
      counts[0] = std::numeric_limits<std::size_t>::max() /
                      sizeof(async_capacity_probe::wire_record) +
                  std::size_t{1};
      counts[1] = std::size_t{0};
      std::fputs("armed rank-zero async fixed-send byte capacity\n", stderr);
    }
  }
  track_next_duplicate = true;
  payload_allocation_watch = true;
  parhip::mpi::neighbor_all_to_all_v_context<async_capacity_probe::wire_record>
      context{graph, std::move(counts)};
  static_cast<void>(context);
  returned_from_failure();
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc != 2 || !parse_mode(argv[1])) {
    std::fputs("usage: mpi_async_capacity_failure_probe MODE\n", stderr);
    return 64;
  }
  if (MPI_Init(&argc, &argv) != MPI_SUCCESS ||
      PMPI_Comm_rank(MPI_COMM_WORLD, &cached_rank) != MPI_SUCCESS ||
      PMPI_Comm_size(MPI_COMM_WORLD, &cached_size) != MPI_SUCCESS) {
    std::fputs("MPI setup failed before async capacity probe\n", stderr);
    return 70;
  }
  if (cached_size != 2) {
    std::fputs("async capacity probe requires exactly two ranks\n", stderr);
    return 64;
  }
  auto graph = parhip::mpi::distributed_graph{
      parhip::mpi::communicator_view{MPI_COMM_WORLD}, {0, 1}};
  graph_communicator = graph.native_handle();
  if (is_receive_mode()) {
    run_receive_failure(graph);
  }
  run_fixed_failure(graph);
}
