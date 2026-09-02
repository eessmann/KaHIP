#include <mpi.h>

#include <catch2/catch_all.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <numeric>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <vector>

#include "communication/mpi_adapter.h"
#include "kahip_mpi_capabilities.h"

namespace async_test_support {
struct wire_entry {
  std::uint64_t generation;
  int source;
  int destination;

  auto operator==(wire_entry const&) const -> bool = default;
};
}  // namespace async_test_support

template <>
struct parhip::mpi::wire_members<async_test_support::wire_entry> {
  inline static constexpr auto value = std::tuple{
      &async_test_support::wire_entry::generation,
      &async_test_support::wire_entry::source,
      &async_test_support::wire_entry::destination};
};

namespace async_protocol_probe {
inline bool active = false;
inline bool force_first_test_incomplete = false;
inline bool first_test_was_forced = false;
inline int count_exchange_calls = 0;
inline int blocking_payload_calls = 0;
inline int blocking_payload_c_calls = 0;
inline int immediate_payload_calls = 0;
inline int immediate_payload_c_calls = 0;
inline int persistent_init_calls = 0;
inline int persistent_init_c_calls = 0;
inline int start_calls = 0;
inline int test_calls = 0;
inline int wait_calls = 0;
inline int waitall_calls = 0;
inline int hidden_completion_calls = 0;
inline int cancel_calls = 0;
inline int request_free_calls = 0;
inline int point_to_point_calls = 0;
inline int tracked_type_free_calls = 0;
inline int tracked_communicator_free_calls = 0;
inline bool tracked_request_active = false;
inline bool request_free_was_inactive = false;
inline MPI_Request tracked_request = MPI_REQUEST_NULL;
inline MPI_Datatype tracked_datatype = MPI_DATATYPE_NULL;
inline MPI_Comm tracked_communicator = MPI_COMM_NULL;
inline std::array<std::string_view, 64> lifecycle{};
inline std::size_t lifecycle_size = 0;
inline bool lifecycle_overflow = false;

void reset() {
  force_first_test_incomplete = false;
  first_test_was_forced = false;
  count_exchange_calls = 0;
  blocking_payload_calls = 0;
  blocking_payload_c_calls = 0;
  immediate_payload_calls = 0;
  immediate_payload_c_calls = 0;
  persistent_init_calls = 0;
  persistent_init_c_calls = 0;
  start_calls = 0;
  test_calls = 0;
  wait_calls = 0;
  waitall_calls = 0;
  hidden_completion_calls = 0;
  cancel_calls = 0;
  request_free_calls = 0;
  point_to_point_calls = 0;
  tracked_type_free_calls = 0;
  tracked_communicator_free_calls = 0;
  tracked_request_active = false;
  request_free_was_inactive = false;
  tracked_request = MPI_REQUEST_NULL;
  tracked_datatype = MPI_DATATYPE_NULL;
  tracked_communicator = MPI_COMM_NULL;
  lifecycle_size = 0;
  lifecycle_overflow = false;
}

void record_event(std::string_view event) noexcept {
  if (lifecycle_size == lifecycle.size()) {
    lifecycle_overflow = true;
    return;
  }
  lifecycle[lifecycle_size++] = event;
}

[[nodiscard]] auto events() noexcept -> std::span<std::string_view const> {
  return {lifecycle.data(), lifecycle_size};
}

void track_operation(MPI_Comm communicator,
                     MPI_Datatype datatype,
                     MPI_Request request,
                     bool request_is_active) {
  tracked_communicator = communicator;
  tracked_datatype = datatype;
  tracked_request = request;
  tracked_request_active = request_is_active;
}
}  // namespace async_protocol_probe

namespace semantic_error_protocol_probe {
inline bool active = false;
inline int error_string_calls = 0;

class activation final {
 public:
  activation() noexcept {
    error_string_calls = 0;
    active = true;
  }
  ~activation() noexcept { active = false; }

  activation(activation const&) = delete;
  auto operator=(activation const&) -> activation& = delete;
};
}  // namespace semantic_error_protocol_probe

namespace backend_agreement_probe {
inline bool active = false;
inline int band_calls = 0;

class activation final {
 public:
  activation() noexcept {
    band_calls = 0;
    active = true;
  }
  ~activation() noexcept { active = false; }

  activation(activation const&) = delete;
  auto operator=(activation const&) -> activation& = delete;
};
}  // namespace backend_agreement_probe

extern "C" int MPI_Allreduce(void const* send_buffer,
                             void* receive_buffer,
                             int count,
                             MPI_Datatype datatype,
                             MPI_Op operation,
                             MPI_Comm communicator) {
  if (backend_agreement_probe::active && count == 2 &&
      datatype == MPI_UINT64_T && operation == MPI_BAND) {
    ++backend_agreement_probe::band_calls;
  }
  return PMPI_Allreduce(send_buffer, receive_buffer, count, datatype, operation,
                        communicator);
}

extern "C" int MPI_Error_string(int error_code,
                                char* error_text,
                                int* error_text_length) {
  if (semantic_error_protocol_probe::active) {
    ++semantic_error_protocol_probe::error_string_calls;
  }
  return PMPI_Error_string(error_code, error_text, error_text_length);
}

extern "C" int MPI_Neighbor_alltoall(void const* send_buffer,
                                     int send_count,
                                     MPI_Datatype send_datatype,
                                     void* receive_buffer,
                                     int receive_count,
                                     MPI_Datatype receive_datatype,
                                     MPI_Comm communicator) {
  if (async_protocol_probe::active) {
    ++async_protocol_probe::count_exchange_calls;
  }
  return PMPI_Neighbor_alltoall(send_buffer, send_count, send_datatype,
                                receive_buffer, receive_count, receive_datatype,
                                communicator);
}

extern "C" int MPI_Neighbor_alltoallv(void const* send_buffer,
                                      int const send_counts[],
                                      int const send_displacements[],
                                      MPI_Datatype send_datatype,
                                      void* receive_buffer,
                                      int const receive_counts[],
                                      int const receive_displacements[],
                                      MPI_Datatype receive_datatype,
                                      MPI_Comm communicator) {
  if (async_protocol_probe::active) {
    ++async_protocol_probe::blocking_payload_calls;
  }
  return PMPI_Neighbor_alltoallv(send_buffer, send_counts, send_displacements,
                                 send_datatype, receive_buffer, receive_counts,
                                 receive_displacements, receive_datatype,
                                 communicator);
}

#if defined(KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_C) && \
    KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_C
extern "C" int MPI_Neighbor_alltoallv_c(void const* send_buffer,
                                        MPI_Count const send_counts[],
                                        MPI_Aint const send_displacements[],
                                        MPI_Datatype send_datatype,
                                        void* receive_buffer,
                                        MPI_Count const receive_counts[],
                                        MPI_Aint const receive_displacements[],
                                        MPI_Datatype receive_datatype,
                                        MPI_Comm communicator) {
  if (async_protocol_probe::active) {
    ++async_protocol_probe::blocking_payload_c_calls;
  }
  return PMPI_Neighbor_alltoallv_c(send_buffer, send_counts, send_displacements,
                                   send_datatype, receive_buffer,
                                   receive_counts, receive_displacements,
                                   receive_datatype, communicator);
}
#endif

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
  if (async_protocol_probe::active) {
    ++async_protocol_probe::immediate_payload_calls;
  }
  auto const result = PMPI_Ineighbor_alltoallv(
      send_buffer, send_counts, send_displacements, send_datatype,
      receive_buffer, receive_counts, receive_displacements, receive_datatype,
      communicator, request);
  if (async_protocol_probe::active && result == MPI_SUCCESS) {
    async_protocol_probe::track_operation(communicator, send_datatype, *request,
                                          true);
    async_protocol_probe::record_event("initiate");
  }
  return result;
}

#if defined(KAHIP_HAVE_MPI_INEIGHBOR_ALLTOALLV_C) && \
    KAHIP_HAVE_MPI_INEIGHBOR_ALLTOALLV_C
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
  if (async_protocol_probe::active) {
    ++async_protocol_probe::immediate_payload_c_calls;
  }
  auto const result = PMPI_Ineighbor_alltoallv_c(
      send_buffer, send_counts, send_displacements, send_datatype,
      receive_buffer, receive_counts, receive_displacements, receive_datatype,
      communicator, request);
  if (async_protocol_probe::active && result == MPI_SUCCESS) {
    async_protocol_probe::track_operation(communicator, send_datatype, *request,
                                          true);
    async_protocol_probe::record_event("initiate-c");
  }
  return result;
}
#endif

#if defined(KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_INIT) && \
    KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_INIT
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
  if (async_protocol_probe::active) {
    ++async_protocol_probe::persistent_init_calls;
  }
  auto const result = PMPI_Neighbor_alltoallv_init(
      send_buffer, send_counts, send_displacements, send_datatype,
      receive_buffer, receive_counts, receive_displacements, receive_datatype,
      communicator, info, request);
  if (async_protocol_probe::active && result == MPI_SUCCESS) {
    async_protocol_probe::track_operation(communicator, send_datatype, *request,
                                          false);
    async_protocol_probe::record_event("persistent-init");
  }
  return result;
}
#endif

#if defined(KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_INIT_C) && \
    KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_INIT_C
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
  if (async_protocol_probe::active) {
    ++async_protocol_probe::persistent_init_c_calls;
  }
  auto const result = PMPI_Neighbor_alltoallv_init_c(
      send_buffer, send_counts, send_displacements, send_datatype,
      receive_buffer, receive_counts, receive_displacements, receive_datatype,
      communicator, info, request);
  if (async_protocol_probe::active && result == MPI_SUCCESS) {
    async_protocol_probe::track_operation(communicator, send_datatype, *request,
                                          false);
    async_protocol_probe::record_event("persistent-init-c");
  }
  return result;
}
#endif

extern "C" int MPI_Start(MPI_Request* request) {
  auto const tracked = async_protocol_probe::active &&
                       *request == async_protocol_probe::tracked_request;
  if (tracked) {
    ++async_protocol_probe::start_calls;
    async_protocol_probe::record_event("start");
  }
  auto const result = PMPI_Start(request);
  if (tracked && result == MPI_SUCCESS) {
    async_protocol_probe::tracked_request_active = true;
  }
  return result;
}

extern "C" int MPI_Test(MPI_Request* request,
                        int* complete,
                        MPI_Status* status) {
  auto const tracked = async_protocol_probe::active &&
                       *request == async_protocol_probe::tracked_request;
  if (tracked) {
    ++async_protocol_probe::test_calls;
    if (async_protocol_probe::force_first_test_incomplete &&
        !async_protocol_probe::first_test_was_forced) {
      async_protocol_probe::first_test_was_forced = true;
      *complete = 0;
      return MPI_SUCCESS;
    }
  }
  auto const result = PMPI_Test(request, complete, status);
  if (tracked && result == MPI_SUCCESS && *complete != 0) {
    async_protocol_probe::tracked_request_active = false;
    async_protocol_probe::record_event("test-complete");
  }
  return result;
}

extern "C" int MPI_Wait(MPI_Request* request, MPI_Status* status) {
  auto const tracked = async_protocol_probe::active &&
                       *request == async_protocol_probe::tracked_request;
  if (tracked) {
    ++async_protocol_probe::wait_calls;
    async_protocol_probe::record_event("wait");
  }
  auto const result = PMPI_Wait(request, status);
  if (tracked && result == MPI_SUCCESS) {
    async_protocol_probe::tracked_request_active = false;
  }
  return result;
}

extern "C" int MPI_Waitall(int count,
                           MPI_Request requests[],
                           MPI_Status statuses[]) {
  if (async_protocol_probe::active) {
    ++async_protocol_probe::waitall_calls;
  }
  return PMPI_Waitall(count, requests, statuses);
}

extern "C" int MPI_Waitany(int count,
                           MPI_Request requests[],
                           int* index,
                           MPI_Status* status) {
  if (async_protocol_probe::active) {
    ++async_protocol_probe::hidden_completion_calls;
  }
  return PMPI_Waitany(count, requests, index, status);
}

extern "C" int MPI_Waitsome(int count,
                            MPI_Request requests[],
                            int* completed_count,
                            int indices[],
                            MPI_Status statuses[]) {
  if (async_protocol_probe::active) {
    ++async_protocol_probe::hidden_completion_calls;
  }
  return PMPI_Waitsome(count, requests, completed_count, indices, statuses);
}

extern "C" int MPI_Testall(int count,
                           MPI_Request requests[],
                           int* complete,
                           MPI_Status statuses[]) {
  if (async_protocol_probe::active) {
    ++async_protocol_probe::hidden_completion_calls;
  }
  return PMPI_Testall(count, requests, complete, statuses);
}

extern "C" int MPI_Testany(int count,
                           MPI_Request requests[],
                           int* index,
                           int* complete,
                           MPI_Status* status) {
  if (async_protocol_probe::active) {
    ++async_protocol_probe::hidden_completion_calls;
  }
  return PMPI_Testany(count, requests, index, complete, status);
}

extern "C" int MPI_Testsome(int count,
                            MPI_Request requests[],
                            int* completed_count,
                            int indices[],
                            MPI_Status statuses[]) {
  if (async_protocol_probe::active) {
    ++async_protocol_probe::hidden_completion_calls;
  }
  return PMPI_Testsome(count, requests, completed_count, indices, statuses);
}

extern "C" int MPI_Cancel(MPI_Request* request) {
  if (async_protocol_probe::active) {
    ++async_protocol_probe::cancel_calls;
  }
  return PMPI_Cancel(request);
}

extern "C" int MPI_Request_free(MPI_Request* request) {
  auto const tracked = async_protocol_probe::active &&
                       *request == async_protocol_probe::tracked_request;
  if (tracked) {
    ++async_protocol_probe::request_free_calls;
    async_protocol_probe::request_free_was_inactive =
        !async_protocol_probe::tracked_request_active;
    async_protocol_probe::record_event("request-free");
  }
  return PMPI_Request_free(request);
}

extern "C" int MPI_Type_free(MPI_Datatype* datatype) {
  auto const tracked = async_protocol_probe::active &&
                       *datatype == async_protocol_probe::tracked_datatype;
  if (tracked) {
    ++async_protocol_probe::tracked_type_free_calls;
    async_protocol_probe::record_event("type-free");
  }
  return PMPI_Type_free(datatype);
}

extern "C" int MPI_Comm_free(MPI_Comm* communicator) {
  auto const tracked =
      async_protocol_probe::active &&
      *communicator == async_protocol_probe::tracked_communicator;
  if (tracked) {
    ++async_protocol_probe::tracked_communicator_free_calls;
    async_protocol_probe::record_event("comm-free");
  }
  return PMPI_Comm_free(communicator);
}

#define KAHIP_COUNT_P2P_WRAPPER(name, signature, call) \
  extern "C" int name signature {                      \
    if (async_protocol_probe::active) {                \
      ++async_protocol_probe::point_to_point_calls;    \
    }                                                  \
    return call;                                       \
  }

KAHIP_COUNT_P2P_WRAPPER(MPI_Isend,
                        (void const* buffer,
                         int count,
                         MPI_Datatype datatype,
                         int destination,
                         int tag,
                         MPI_Comm communicator,
                         MPI_Request* request),
                        PMPI_Isend(buffer,
                                   count,
                                   datatype,
                                   destination,
                                   tag,
                                   communicator,
                                   request))
KAHIP_COUNT_P2P_WRAPPER(
    MPI_Irecv,
    (void* buffer,
     int count,
     MPI_Datatype datatype,
     int source,
     int tag,
     MPI_Comm communicator,
     MPI_Request* request),
    PMPI_Irecv(buffer, count, datatype, source, tag, communicator, request))
KAHIP_COUNT_P2P_WRAPPER(
    MPI_Send,
    (void const* buffer,
     int count,
     MPI_Datatype datatype,
     int destination,
     int tag,
     MPI_Comm communicator),
    PMPI_Send(buffer, count, datatype, destination, tag, communicator))
KAHIP_COUNT_P2P_WRAPPER(
    MPI_Recv,
    (void* buffer,
     int count,
     MPI_Datatype datatype,
     int source,
     int tag,
     MPI_Comm communicator,
     MPI_Status* status),
    PMPI_Recv(buffer, count, datatype, source, tag, communicator, status))
KAHIP_COUNT_P2P_WRAPPER(
    MPI_Probe,
    (int source, int tag, MPI_Comm communicator, MPI_Status* status),
    PMPI_Probe(source, tag, communicator, status))
KAHIP_COUNT_P2P_WRAPPER(
    MPI_Iprobe,
    (int source, int tag, MPI_Comm communicator, int* flag, MPI_Status* status),
    PMPI_Iprobe(source, tag, communicator, flag, status))
KAHIP_COUNT_P2P_WRAPPER(MPI_Mprobe,
                        (int source,
                         int tag,
                         MPI_Comm communicator,
                         MPI_Message* message,
                         MPI_Status* status),
                        PMPI_Mprobe(source, tag, communicator, message, status))
KAHIP_COUNT_P2P_WRAPPER(
    MPI_Improbe,
    (int source,
     int tag,
     MPI_Comm communicator,
     int* flag,
     MPI_Message* message,
     MPI_Status* status),
    PMPI_Improbe(source, tag, communicator, flag, message, status))
KAHIP_COUNT_P2P_WRAPPER(MPI_Sendrecv,
                        (void const* send_buffer,
                         int send_count,
                         MPI_Datatype send_datatype,
                         int destination,
                         int send_tag,
                         void* receive_buffer,
                         int receive_count,
                         MPI_Datatype receive_datatype,
                         int source,
                         int receive_tag,
                         MPI_Comm communicator,
                         MPI_Status* status),
                        PMPI_Sendrecv(send_buffer,
                                      send_count,
                                      send_datatype,
                                      destination,
                                      send_tag,
                                      receive_buffer,
                                      receive_count,
                                      receive_datatype,
                                      source,
                                      receive_tag,
                                      communicator,
                                      status))
KAHIP_COUNT_P2P_WRAPPER(MPI_Sendrecv_replace,
                        (void* buffer,
                         int count,
                         MPI_Datatype datatype,
                         int destination,
                         int send_tag,
                         int source,
                         int receive_tag,
                         MPI_Comm communicator,
                         MPI_Status* status),
                        PMPI_Sendrecv_replace(buffer,
                                              count,
                                              datatype,
                                              destination,
                                              send_tag,
                                              source,
                                              receive_tag,
                                              communicator,
                                              status))

#undef KAHIP_COUNT_P2P_WRAPPER

namespace {
using async_test_support::wire_entry;
using parhip::mpi::collective_options;
using parhip::mpi::communicator_view;
using parhip::mpi::context_options;
using parhip::mpi::distributed_graph;
using parhip::mpi::neighbor_all_to_all_v;
using parhip::mpi::neighbor_all_to_all_v_context;
using parhip::mpi::neighbor_exchange_request;
using parhip::mpi::persistence_policy;
using parhip::mpi::segmented_buffer;
using parhip::mpi::start_neighbor_all_to_all_v;

static_assert(!std::is_nothrow_constructible_v<
              parhip::mpi::detail::direct_neighbor_storage<int>,
              parhip::mpi::communicator, parhip::mpi::datatype,
              parhip::mpi::segmented_buffer<int>,
              parhip::mpi::segmented_buffer<int>,
              parhip::mpi::detail::direct_neighbor_layout,
              parhip::mpi::detail::neighbor_direct_backend,
              std::optional<parhip::mpi::detail::mpi3_bounded_neighbor_plan>>,
              "allocating direct-neighbor storage must propagate allocation "
              "failures to the communicator-scoped fail-fast boundary");

template <typename Operation>
void require_exact_common_mpi_error(Operation&& operation,
                                    std::string_view expected_context,
                                    communicator_view communicator) {
  auto caught = 0;
  auto exact_dynamic_type = 0;
  auto raw_code_matches = 0;
  auto context_matches = 0;
  auto error_string_calls = 0;
  {
    semantic_error_protocol_probe::activation observation{};
    try {
      std::invoke(std::forward<Operation>(operation));
    } catch (parhip::mpi::mpi_error const& error) {
      caught = 1;
      exact_dynamic_type =
          typeid(error) == typeid(parhip::mpi::mpi_error) ? 1 : 0;
      raw_code_matches = error.error_code() == MPI_ERR_ARG ? 1 : 0;
      context_matches = error.context() == expected_context ? 1 : 0;
    } catch (...) {
      caught = 1;
    }
    error_string_calls = semantic_error_protocol_probe::error_string_calls;
  }
  auto local = std::array{caught, exact_dynamic_type, raw_code_matches,
                          context_matches, error_string_calls};
  auto global = std::array{0, 0, 0, 0, 0};
  REQUIRE(PMPI_Allreduce(local.data(), global.data(),
                         static_cast<int>(local.size()), MPI_INT, MPI_SUM,
                         communicator.native_handle()) == MPI_SUCCESS);
  REQUIRE(global == std::array{communicator.size(), communicator.size(),
                               communicator.size(), communicator.size(), 0});
}

template <typename Operation>
void require_collective_semantic_error(Operation&& operation,
                                       std::string_view expected_context,
                                       communicator_view communicator) {
  require_exact_common_mpi_error(std::forward<Operation>(operation),
                                 expected_context, communicator);
}

auto ring_segments(distributed_graph const& graph,
                   int rank,
                   std::uint64_t generation)
    -> std::vector<std::vector<wire_entry>> {
  auto segments = std::vector<std::vector<wire_entry>>{};
  segments.reserve(graph.destinations().size());
  for (auto const destination : graph.destinations()) {
    auto const count = static_cast<std::size_t>((rank + destination) % 3 + 1);
    segments.emplace_back(count, wire_entry{generation, rank, destination});
  }
  return segments;
}

void require_default_immediate_backend(int expected_calls) {
#if KAHIP_HAVE_MPI_INEIGHBOR_ALLTOALLV_C
  REQUIRE(async_protocol_probe::immediate_payload_calls == 0);
  REQUIRE(async_protocol_probe::immediate_payload_c_calls == expected_calls);
#else
  REQUIRE(async_protocol_probe::immediate_payload_calls == expected_calls);
  REQUIRE(async_protocol_probe::immediate_payload_c_calls == 0);
#endif
}

void require_default_persistent_backend(int expected_calls) {
#if KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_INIT_C
  REQUIRE(async_protocol_probe::persistent_init_calls == 0);
  REQUIRE(async_protocol_probe::persistent_init_c_calls == expected_calls);
#else
  REQUIRE(async_protocol_probe::persistent_init_calls == expected_calls);
  REQUIRE(async_protocol_probe::persistent_init_c_calls == 0);
#endif
}

TEST_CASE("Task 7B MPI capabilities match independent generated probes",
          "[unit][mpi][neighbor][async][capability]") {
#if !defined(KAHIP_HAVE_MPI_INEIGHBOR_ALLTOALLV) ||     \
    !defined(KAHIP_HAVE_MPI_INEIGHBOR_ALLTOALLV_C) ||   \
    !defined(KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_INIT) || \
    !defined(KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_INIT_C)
  FAIL("generated Task 7B MPI capability macro is missing");
#else
  STATIC_REQUIRE(parhip::mpi::capabilities::has_ineighbor_alltoallv ==
                 (KAHIP_HAVE_MPI_INEIGHBOR_ALLTOALLV != 0));
  STATIC_REQUIRE(parhip::mpi::capabilities::has_ineighbor_alltoallv_c ==
                 (KAHIP_HAVE_MPI_INEIGHBOR_ALLTOALLV_C != 0));
  STATIC_REQUIRE(parhip::mpi::capabilities::has_neighbor_alltoallv_init ==
                 (KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_INIT != 0));
  STATIC_REQUIRE(parhip::mpi::capabilities::has_neighbor_alltoallv_init_c ==
                 (KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_INIT_C != 0));
  STATIC_REQUIRE(parhip::mpi::capabilities::has_ineighbor_alltoallv);
#endif
}

TEST_CASE("one-shot neighborhood request is move-only and context immovable",
          "[unit][mpi][neighbor][async][ownership]") {
  STATIC_REQUIRE_FALSE(
      std::is_copy_constructible_v<neighbor_exchange_request<int>>);
  STATIC_REQUIRE(std::is_move_constructible_v<neighbor_exchange_request<int>>);
  STATIC_REQUIRE_FALSE(
      std::is_move_assignable_v<neighbor_exchange_request<int>>);
  STATIC_REQUIRE_FALSE(
      std::is_copy_constructible_v<neighbor_all_to_all_v_context<int>>);
  STATIC_REQUIRE_FALSE(
      std::is_move_constructible_v<neighbor_all_to_all_v_context<int>>);
}

TEST_CASE("one-shot zero-degree initiation is immediate and owns completion",
          "[unit][mpi][neighbor][async][zero]") {
  communicator_view const world{MPI_COMM_WORLD};
  distributed_graph graph{world, {}};
  auto sends =
      segmented_buffer<int>::from_segments(std::vector<std::vector<int>>{});

  async_protocol_probe::reset();
  async_protocol_probe::active = true;
  auto request = start_neighbor_all_to_all_v(std::move(sends), graph);
  REQUIRE(async_protocol_probe::count_exchange_calls == 1);
  require_default_immediate_backend(1);
  REQUIRE(async_protocol_probe::blocking_payload_calls == 0);
  REQUIRE(async_protocol_probe::blocking_payload_c_calls == 0);
  REQUIRE(async_protocol_probe::test_calls == 0);
  REQUIRE(async_protocol_probe::wait_calls == 0);
  REQUIRE(async_protocol_probe::waitall_calls == 0);
  REQUIRE(async_protocol_probe::hidden_completion_calls == 0);
  REQUIRE(async_protocol_probe::point_to_point_calls == 0);
  auto received = std::move(request).wait();
  async_protocol_probe::active = false;

  REQUIRE(received.storage().empty());
  REQUIRE(received.segment_count() == 0);
  REQUIRE(async_protocol_probe::wait_calls == 1);
  REQUIRE(async_protocol_probe::point_to_point_calls == 0);
  REQUIRE(async_protocol_probe::waitall_calls == 0);
  REQUIRE(async_protocol_probe::hidden_completion_calls == 0);
  REQUIRE(async_protocol_probe::cancel_calls == 0);
}

TEST_CASE("one-shot preserves an explicit empty destination segment",
          "[unit][mpi][neighbor][async][empty]") {
  communicator_view const world{MPI_COMM_WORLD};
  distributed_graph graph{world, {world.rank()}};
  auto sends =
      segmented_buffer<int>::from_segments(std::vector<std::vector<int>>{{}});

  async_protocol_probe::reset();
  async_protocol_probe::active = true;
  auto request = start_neighbor_all_to_all_v(std::move(sends), graph);
  auto received = std::move(request).wait();
  async_protocol_probe::active = false;

  REQUIRE(received.segment_count() == 1);
  REQUIRE(received.segment(0).empty());
  REQUIRE(async_protocol_probe::count_exchange_calls == 1);
  require_default_immediate_backend(1);
  REQUIRE(async_protocol_probe::point_to_point_calls == 0);
  REQUIRE(async_protocol_probe::hidden_completion_calls == 0);
  REQUIRE(async_protocol_probe::cancel_calls == 0);
}

TEST_CASE("active one-shot request survives moves and source graph destruction",
          "[unit][mpi][neighbor][async][move][order][wire]") {
  communicator_view const world{MPI_COMM_WORLD};
  auto const rank = world.rank();
  auto const next = (rank + 1) % world.size();
  auto sources = std::vector<int>{};
  auto received = std::optional<segmented_buffer<wire_entry>>{};
  async_protocol_probe::reset();
  async_protocol_probe::force_first_test_incomplete = true;
  async_protocol_probe::active = true;
  {
    auto request = [&] {
      distributed_graph graph{world, {next}};
      sources.assign(graph.sources().begin(), graph.sources().end());
      auto started = start_neighbor_all_to_all_v(
          segmented_buffer<wire_entry>::from_segments(
              ring_segments(graph, rank, 17)),
          graph);
      REQUIRE(MPI_Barrier(world.native_handle()) == MPI_SUCCESS);
      return neighbor_exchange_request<wire_entry>{std::move(started)};
    }();

    neighbor_exchange_request<wire_entry> moved{std::move(request)};
    REQUIRE_FALSE(moved.test());
    auto completion_observed = false;
    for (int attempt = 0; attempt < 10'000 && !completion_observed; ++attempt) {
      completion_observed = moved.test();
    }
    REQUIRE(completion_observed);
    received.emplace(std::move(moved).wait());
  }
  async_protocol_probe::active = false;

  REQUIRE(async_protocol_probe::first_test_was_forced);
  REQUIRE(async_protocol_probe::count_exchange_calls == 1);
  require_default_immediate_backend(1);
  REQUIRE(async_protocol_probe::wait_calls == 0);
  REQUIRE(async_protocol_probe::request_free_calls == 0);
  REQUIRE(async_protocol_probe::tracked_type_free_calls == 1);
  REQUIRE(async_protocol_probe::tracked_communicator_free_calls == 1);
  REQUIRE(async_protocol_probe::point_to_point_calls == 0);
  REQUIRE(received->segment_count() == sources.size());
  for (std::size_t index = 0; index < sources.size(); ++index) {
    REQUIRE(std::ranges::equal(
        received->segment(index),
        std::vector<wire_entry>(
            static_cast<std::size_t>((sources[index] + rank) % 3 + 1),
            wire_entry{17, sources[index], rank})));
  }
}

TEST_CASE("one-shot MPI-3 fallback advances deterministic bounded rounds",
          "[unit][mpi][neighbor][async][bounded]") {
  communicator_view const world{MPI_COMM_WORLD};
  distributed_graph graph{world, {world.rank()}};
  auto const element_count =
      world.rank() == 0 ? std::size_t{5} : std::size_t{1};
  auto const segments = std::vector<std::vector<int>>{
      std::vector<int>(element_count, world.rank())};
  auto const options =
      collective_options{.mpi3_round_ceiling = 2, .force_mpi3 = true};

  async_protocol_probe::reset();
  async_protocol_probe::force_first_test_incomplete = true;
  async_protocol_probe::active = true;
  auto request = start_neighbor_all_to_all_v(
      segmented_buffer<int>::from_segments(segments), graph, options);
  REQUIRE(async_protocol_probe::count_exchange_calls == 1);
  REQUIRE(async_protocol_probe::immediate_payload_calls == 1);
  REQUIRE(async_protocol_probe::immediate_payload_c_calls == 0);
  REQUIRE(async_protocol_probe::blocking_payload_calls == 0);
  REQUIRE_FALSE(request.test());
  auto complete = false;
  for (int attempt = 0; attempt < 10'000 && !complete; ++attempt) {
    complete = request.test();
  }
  REQUIRE(complete);
  auto received = std::move(request).wait();
  async_protocol_probe::active = false;
  REQUIRE(std::ranges::equal(received.segment(0), segments[0]));
  REQUIRE(async_protocol_probe::immediate_payload_calls == 3);
  REQUIRE(async_protocol_probe::first_test_was_forced);
  REQUIRE(async_protocol_probe::test_calls >= 4);
  REQUIRE(async_protocol_probe::wait_calls == 0);
  REQUIRE(async_protocol_probe::point_to_point_calls == 0);
}

TEST_CASE("bounded MPI-3 star phases retain zero-count participants",
          "[unit][mpi][neighbor][async][bounded][asymmetric][sparse]") {
  communicator_view const world{MPI_COMM_WORLD};
  if (world.size() < 2) {
    return;
  }

  auto outgoing = std::vector<int>{};
  if (world.rank() == 0) {
    outgoing.resize(static_cast<std::size_t>(world.size() - 1));
    std::ranges::iota(outgoing, 1);
  }
  distributed_graph graph{world, std::move(outgoing)};
  auto const counts = std::vector<std::size_t>(graph.destinations().size(), 5);

  async_protocol_probe::reset();
  async_protocol_probe::active = true;
  {
    neighbor_all_to_all_v_context<int> context{
        graph,
        counts,
        context_options{
            .collective = collective_options{.mpi3_round_ceiling = 2,
                                              .force_mpi3 = true}}};
    for (std::size_t index = 0; index < graph.destinations().size(); ++index) {
      std::ranges::fill(context.send_segment(index),
                        graph.destinations()[index]);
    }
    context.start();
    context.wait();
    if (world.rank() == 0) {
      REQUIRE(graph.sources().empty());
    } else {
      REQUIRE(std::ranges::equal(graph.sources(), std::array{0}));
      REQUIRE(std::ranges::equal(
          context.received_segment(0),
          std::vector<int>(5, world.rank())));
    }
  }
  async_protocol_probe::active = false;

  REQUIRE(async_protocol_probe::immediate_payload_calls ==
          3 * (world.size() - 1));
  REQUIRE(async_protocol_probe::immediate_payload_c_calls == 0);
  REQUIRE(async_protocol_probe::point_to_point_calls == 0);
}

TEST_CASE("one-shot options are validated collectively before count exchange",
          "[unit][mpi][neighbor][async][options][failure]") {
  communicator_view const world{MPI_COMM_WORLD};
  distributed_graph graph{world, {world.rank()}};

  for (auto const options : std::array{
           collective_options{.mpi3_round_ceiling = 0, .force_mpi3 = true},
           collective_options{.mpi3_round_ceiling = 2,
                              .force_mpi3 = world.rank() == 0}}) {
    if (world.size() == 1 && options.mpi3_round_ceiling != 0) {
      continue;
    }
    async_protocol_probe::reset();
    async_protocol_probe::active = true;
    require_collective_semantic_error(
        [&] {
          static_cast<void>(start_neighbor_all_to_all_v(
              segmented_buffer<int>::from_segments(
                  std::vector<std::vector<int>>{{world.rank()}}),
              graph, options));
        },
        "direct neighborhood exchange options must agree collectively", world);
    REQUIRE(async_protocol_probe::count_exchange_calls == 0);
    REQUIRE(async_protocol_probe::immediate_payload_calls == 0);
    REQUIRE(async_protocol_probe::immediate_payload_c_calls == 0);
    async_protocol_probe::active = false;
  }

  if (world.size() == 1) {
    async_protocol_probe::reset();
    async_protocol_probe::active = true;
    semantic_error_protocol_probe::activation observation{};
    auto request = start_neighbor_all_to_all_v(
        segmented_buffer<int>::from_segments(
            std::vector<std::vector<int>>{{world.rank()}}),
        graph, collective_options{.mpi3_round_ceiling = 2, .force_mpi3 = true});
    auto received = std::move(request).wait();
    async_protocol_probe::active = false;
    REQUIRE(std::ranges::equal(received.segment(0), std::array{world.rank()}));
    REQUIRE(semantic_error_protocol_probe::error_string_calls == 0);
  }
}

TEST_CASE("one-shot rejects a rank-local malformed segment layout commonly",
          "[unit][mpi][neighbor][async][layout][failure]") {
  communicator_view const world{MPI_COMM_WORLD};
  distributed_graph graph{world, {world.rank()}};
  auto sends = world.rank() == 0
                   ? segmented_buffer<int>::uninitialized(0, {}, {})
                   : segmented_buffer<int>::from_segments(
                         std::vector<std::vector<int>>{{world.rank()}});

  async_protocol_probe::reset();
  async_protocol_probe::active = true;
  require_collective_semantic_error(
      [&] {
        static_cast<void>(start_neighbor_all_to_all_v(std::move(sends), graph));
      },
      "direct neighborhood exchange input validation failed", world);
  REQUIRE(async_protocol_probe::count_exchange_calls == 0);
  REQUIRE(async_protocol_probe::immediate_payload_calls == 0);
  REQUIRE(async_protocol_probe::immediate_payload_c_calls == 0);
  async_protocol_probe::active = false;
}

TEST_CASE("reusable persistence policy is collectively identical",
          "[unit][mpi][neighbor][async][context][options][failure]") {
  communicator_view const world{MPI_COMM_WORLD};
  if (world.size() == 1) {
    distributed_graph graph{world, {world.rank()}};
    async_protocol_probe::reset();
    async_protocol_probe::active = true;
    semantic_error_protocol_probe::activation observation{};
    neighbor_all_to_all_v_context<int> context{
        graph,
        {1},
        context_options{.persistence = persistence_policy::disabled}};
    async_protocol_probe::active = false;
    REQUIRE(context.send_segment(0).size() == 1);
    REQUIRE(semantic_error_protocol_probe::error_string_calls == 0);
    return;
  }
  distributed_graph graph{world, {world.rank()}};
  auto const policy = world.rank() == 0 ? persistence_policy::disabled
                                        : persistence_policy::prefer;

  async_protocol_probe::reset();
  async_protocol_probe::active = true;
  require_collective_semantic_error(
      [&] {
        neighbor_all_to_all_v_context<int> context{
            graph, {1}, context_options{.persistence = policy}};
        static_cast<void>(context);
      },
      "direct neighborhood exchange options must agree collectively", world);
  REQUIRE(async_protocol_probe::count_exchange_calls == 0);
  REQUIRE(async_protocol_probe::immediate_payload_calls == 0);
  REQUIRE(async_protocol_probe::persistent_init_calls == 0);
  async_protocol_probe::active = false;
}

TEST_CASE("reusable context rejects fixed send-count cardinality collectively",
          "[unit][mpi][neighbor][async][context][layout][failure]") {
  communicator_view const world{MPI_COMM_WORLD};
  distributed_graph graph{world, {world.rank()}};
  auto counts = world.rank() == 0 ? std::vector<std::size_t>{}
                                  : std::vector<std::size_t>{1};

  async_protocol_probe::reset();
  async_protocol_probe::active = true;
  require_collective_semantic_error(
      [&] {
        neighbor_all_to_all_v_context<int> context{graph, std::move(counts)};
        static_cast<void>(context);
      },
      "fixed neighborhood send layout validation failed", world);
  async_protocol_probe::active = false;
  REQUIRE(async_protocol_probe::count_exchange_calls == 0);
  REQUIRE(async_protocol_probe::immediate_payload_calls == 0);
  REQUIRE(async_protocol_probe::persistent_init_calls == 0);
}

TEST_CASE("reusable persistence policy rejects an invalid value collectively",
          "[unit][mpi][neighbor][async][context][options][failure]") {
  communicator_view const world{MPI_COMM_WORLD};
  distributed_graph graph{world, {world.rank()}};

  async_protocol_probe::reset();
  async_protocol_probe::active = true;
  require_collective_semantic_error(
      [&] {
        neighbor_all_to_all_v_context<int> context{
            graph,
            {1},
            context_options{.persistence =
                                static_cast<persistence_policy>(0xff)}};
        static_cast<void>(context);
      },
      "direct neighborhood exchange options must agree collectively", world);
  async_protocol_probe::active = false;
  REQUIRE(async_protocol_probe::count_exchange_calls == 0);
  REQUIRE(async_protocol_probe::immediate_payload_calls == 0);
  REQUIRE(async_protocol_probe::persistent_init_calls == 0);
}

TEST_CASE("default reusable context exchanges three fresh generations",
          "[unit][mpi][neighbor][async][context][generation]") {
  communicator_view const world{MPI_COMM_WORLD};
  auto const rank = world.rank();
  auto const next = (rank + 1) % world.size();
  distributed_graph graph{world, {next}};
  auto const send_counts = std::vector<std::size_t>{2};

  async_protocol_probe::reset();
  async_protocol_probe::active = true;
  {
    neighbor_all_to_all_v_context<wire_entry> context{graph, send_counts};
    REQUIRE(async_protocol_probe::count_exchange_calls == 1);
    REQUIRE(async_protocol_probe::persistent_init_calls == 0);
    REQUIRE(async_protocol_probe::persistent_init_c_calls == 0);
    auto const send_address = context.send_segment(0).data();
    auto receive_address = static_cast<wire_entry const*>(nullptr);

    for (std::uint64_t generation = 1; generation <= 3; ++generation) {
      auto send = context.send_segment(0);
      REQUIRE(send.data() == send_address);
      std::ranges::fill(send, wire_entry{generation, rank, next});
      context.start();
      if (generation == 1) {
        async_protocol_probe::force_first_test_incomplete = true;
        REQUIRE_FALSE(context.test());
        context.wait();
      } else if (generation == 2) {
        auto complete = false;
        for (int attempt = 0; attempt < 10'000 && !complete; ++attempt) {
          complete = context.test();
        }
        REQUIRE(complete);
      } else {
        context.wait();
      }
      REQUIRE(context.send_segment(0).data() == send_address);
      REQUIRE(context.received_segment(0).size() == 2);
      if (receive_address == nullptr) {
        receive_address = context.received_segment(0).data();
      }
      REQUIRE(context.received_segment(0).data() == receive_address);
      REQUIRE(std::ranges::all_of(
          context.received_segment(0), [&](wire_entry const& entry) {
            return entry == wire_entry{generation, graph.sources()[0], rank};
          }));
    }
  }
  async_protocol_probe::active = false;

  REQUIRE(async_protocol_probe::count_exchange_calls == 1);
  require_default_immediate_backend(3);
  REQUIRE(async_protocol_probe::blocking_payload_calls == 0);
  REQUIRE(async_protocol_probe::blocking_payload_c_calls == 0);
  REQUIRE(async_protocol_probe::request_free_calls == 0);
  REQUIRE(async_protocol_probe::tracked_type_free_calls == 1);
  REQUIRE(async_protocol_probe::tracked_communicator_free_calls == 1);
  REQUIRE(async_protocol_probe::point_to_point_calls == 0);
  REQUIRE(async_protocol_probe::waitall_calls == 0);
  REQUIRE(async_protocol_probe::hidden_completion_calls == 0);
  REQUIRE(async_protocol_probe::cancel_calls == 0);
}

TEST_CASE("active one-shot destructor waits before owned MPI cleanup",
          "[unit][mpi][neighbor][async][destructor][order]") {
  communicator_view const world{MPI_COMM_WORLD};

  async_protocol_probe::reset();
  async_protocol_probe::active = true;
  {
    distributed_graph graph{world, {world.rank()}};
    {
      auto request = start_neighbor_all_to_all_v(
          segmented_buffer<wire_entry>::from_segments(
              std::vector<std::vector<wire_entry>>{
                  {{1, world.rank(), world.rank()}}}),
          graph);
      static_cast<void>(request);
      REQUIRE(async_protocol_probe::wait_calls == 0);
    }
    REQUIRE(async_protocol_probe::wait_calls == 1);
    REQUIRE(async_protocol_probe::tracked_type_free_calls == 1);
    REQUIRE(async_protocol_probe::tracked_communicator_free_calls == 1);
  }
  async_protocol_probe::active = false;

  REQUIRE_FALSE(async_protocol_probe::lifecycle_overflow);
  auto const lifecycle = async_protocol_probe::events();
  auto const wait = std::ranges::find(lifecycle, "wait");
  auto const type_free = std::ranges::find(lifecycle, "type-free");
  auto const comm_free = std::ranges::find(lifecycle, "comm-free");
  REQUIRE(wait < type_free);
  REQUIRE(type_free < comm_free);
  REQUIRE(async_protocol_probe::request_free_calls == 0);
}

TEST_CASE("active reusable destructor completes before releasing resources",
          "[unit][mpi][neighbor][async][context][destructor][order]") {
  communicator_view const world{MPI_COMM_WORLD};
  distributed_graph graph{world, {world.rank()}};
  auto policies = std::vector{persistence_policy::disabled};
#if (defined(KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_INIT) &&   \
     KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_INIT) ||           \
    (defined(KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_INIT_C) && \
     KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_INIT_C)
  policies.push_back(persistence_policy::required);
#endif

  for (auto const policy : policies) {
    async_protocol_probe::reset();
    async_protocol_probe::active = true;
    {
      neighbor_all_to_all_v_context<wire_entry> context{
          graph, {1}, context_options{.persistence = policy}};
      context.send_segment(0)[0] = wire_entry{1, world.rank(), world.rank()};
      context.start();
      REQUIRE(async_protocol_probe::wait_calls == 0);
    }
    async_protocol_probe::active = false;

    REQUIRE_FALSE(async_protocol_probe::lifecycle_overflow);
    auto const lifecycle = async_protocol_probe::events();
    auto const wait = std::ranges::find(lifecycle, "wait");
    auto const type_free = std::ranges::find(lifecycle, "type-free");
    auto const comm_free = std::ranges::find(lifecycle, "comm-free");
    REQUIRE(wait < type_free);
    REQUIRE(type_free < comm_free);
    if (policy == persistence_policy::required) {
      auto const request_free = std::ranges::find(lifecycle, "request-free");
      REQUIRE(wait < request_free);
      REQUIRE(request_free < type_free);
      REQUIRE(async_protocol_probe::request_free_calls == 1);
      REQUIRE(async_protocol_probe::request_free_was_inactive);
    } else {
      REQUIRE(async_protocol_probe::request_free_calls == 0);
    }
  }
}

TEST_CASE("reusable context supports both asymmetric star orientations",
          "[unit][mpi][neighbor][async][context][asymmetric]") {
  communicator_view const world{MPI_COMM_WORLD};
  if (world.size() < 2) {
    return;
  }

  for (auto const reversed : std::array{false, true}) {
    auto outgoing = std::vector<int>{};
    if ((!reversed && world.rank() == 0) || (reversed && world.rank() != 0)) {
      for (int destination = 1; !reversed && destination < world.size();
           ++destination) {
        outgoing.push_back(destination);
      }
      if (reversed) {
        outgoing.push_back(0);
      }
    }
    distributed_graph graph{world, std::move(outgoing)};
    auto counts = std::vector<std::size_t>(graph.destinations().size(), 1);
    neighbor_all_to_all_v_context<int> context{graph, std::move(counts)};
    for (std::size_t index = 0; index < graph.destinations().size(); ++index) {
      context.send_segment(index)[0] =
          world.rank() * 100 + graph.destinations()[index];
    }
    context.start();
    context.wait();
    for (std::size_t index = 0; index < graph.sources().size(); ++index) {
      REQUIRE(std::ranges::equal(
          context.received_segment(index),
          std::array{graph.sources()[index] * 100 + world.rank()}));
    }
  }
}

TEST_CASE("persistent context uses guarded init start wait and inactive free",
          "[unit][mpi][neighbor][async][context][persistent]") {
  communicator_view const world{MPI_COMM_WORLD};
  distributed_graph graph{world, {world.rank()}};

#if (defined(KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_INIT) &&   \
     KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_INIT) ||           \
    (defined(KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_INIT_C) && \
     KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_INIT_C)
  async_protocol_probe::reset();
  async_protocol_probe::active = true;
  {
    neighbor_all_to_all_v_context<wire_entry> context{
        graph,
        {1},
        context_options{.persistence = persistence_policy::required}};
    require_default_persistent_backend(1);
    for (std::uint64_t generation = 1; generation <= 3; ++generation) {
      context.send_segment(0)[0] =
          wire_entry{generation, world.rank(), world.rank()};
      context.start();
      context.wait();
      REQUIRE(context.received_segment(0)[0].generation == generation);
    }
    REQUIRE(async_protocol_probe::request_free_calls == 0);
  }
  async_protocol_probe::active = false;
  REQUIRE(async_protocol_probe::start_calls == 3);
  REQUIRE(async_protocol_probe::wait_calls == 3);
  REQUIRE(async_protocol_probe::request_free_calls == 1);
  REQUIRE(async_protocol_probe::request_free_was_inactive);
  REQUIRE(async_protocol_probe::tracked_type_free_calls == 1);
  REQUIRE(async_protocol_probe::tracked_communicator_free_calls == 1);
  REQUIRE_FALSE(async_protocol_probe::lifecycle_overflow);
  auto const lifecycle = async_protocol_probe::events();
  auto const request_free = std::ranges::find(lifecycle, "request-free");
  auto const type_free = std::ranges::find(lifecycle, "type-free");
  auto const comm_free = std::ranges::find(lifecycle, "comm-free");
  REQUIRE(request_free < type_free);
  REQUIRE(type_free < comm_free);
#else
  require_collective_semantic_error(
      [&] {
        neighbor_all_to_all_v_context<int> context{
            graph,
            {1},
            context_options{.persistence = persistence_policy::required}};
        static_cast<void>(context);
      },
      "persistent neighborhood exchange is unavailable", world);
#endif
}

TEST_CASE("forced MPI-3 disables every MPI-4 persistent backend",
          "[unit][mpi][neighbor][async][context][persistent][mpi3]") {
  communicator_view const world{MPI_COMM_WORLD};
  distributed_graph graph{world, {world.rank()}};
  auto const mpi3_options = collective_options{.force_mpi3 = true};

  async_protocol_probe::reset();
  async_protocol_probe::active = true;
  {
    neighbor_all_to_all_v_context<int> context{
        graph,
        {1},
        context_options{.collective = mpi3_options,
                        .persistence = persistence_policy::prefer}};
    context.send_segment(0)[0] = world.rank();
    context.start();
    context.wait();
  }
  REQUIRE(async_protocol_probe::persistent_init_calls == 0);
  REQUIRE(async_protocol_probe::persistent_init_c_calls == 0);
  REQUIRE(async_protocol_probe::immediate_payload_calls == 1);
  REQUIRE(async_protocol_probe::immediate_payload_c_calls == 0);

  require_collective_semantic_error(
      [&] {
        neighbor_all_to_all_v_context<int> context{
            graph,
            {1},
            context_options{.collective = mpi3_options,
                            .persistence = persistence_policy::required}};
        static_cast<void>(context);
      },
      "persistent neighborhood exchange is unavailable", world);
  async_protocol_probe::active = false;
}

TEST_CASE("reusable context restarts the bounded MPI-3 state machine",
          "[unit][mpi][neighbor][async][context][bounded]") {
  communicator_view const world{MPI_COMM_WORLD};
  distributed_graph graph{world, {world.rank()}};
  async_protocol_probe::reset();
  async_protocol_probe::active = true;
  {
    neighbor_all_to_all_v_context<int> context{
        graph,
        {5},
        context_options{
            .collective = collective_options{.mpi3_round_ceiling = 2,
                                              .force_mpi3 = true},
            .persistence = persistence_policy::prefer}};
    for (auto generation : std::array{7, 11}) {
      std::ranges::fill(context.send_segment(0), world.rank() + generation);
      context.start();
      context.wait();
      REQUIRE(std::ranges::equal(
          context.received_segment(0),
          std::vector<int>(5, world.rank() + generation)));
    }
  }
  async_protocol_probe::active = false;
  REQUIRE(async_protocol_probe::count_exchange_calls == 1);
  REQUIRE(async_protocol_probe::immediate_payload_calls == 6);
  REQUIRE(async_protocol_probe::immediate_payload_c_calls == 0);
  REQUIRE(async_protocol_probe::persistent_init_calls == 0);
  REQUIRE(async_protocol_probe::persistent_init_c_calls == 0);
  REQUIRE(async_protocol_probe::wait_calls == 6);
  REQUIRE(async_protocol_probe::point_to_point_calls == 0);
}

TEST_CASE("bounded context destruction drains every active round",
          "[unit][mpi][neighbor][async][context][bounded][destructor]") {
  communicator_view const world{MPI_COMM_WORLD};
  distributed_graph graph{world, {world.rank()}};
  async_protocol_probe::reset();
  async_protocol_probe::active = true;
  {
    neighbor_all_to_all_v_context<wire_entry> context{
        graph,
        {5},
        context_options{
            .collective = collective_options{.mpi3_round_ceiling = 2,
                                              .force_mpi3 = true}}};
    std::ranges::fill(
        context.send_segment(0),
        wire_entry{0, world.rank(), world.rank()});
    context.start();
  }
  async_protocol_probe::active = false;

  REQUIRE(async_protocol_probe::immediate_payload_calls == 3);
  REQUIRE(async_protocol_probe::immediate_payload_c_calls == 0);
  REQUIRE(async_protocol_probe::wait_calls == 3);
  REQUIRE(async_protocol_probe::tracked_type_free_calls == 1);
  REQUIRE(async_protocol_probe::tracked_communicator_free_calls == 1);
  REQUIRE(async_protocol_probe::point_to_point_calls == 0);
}

TEST_CASE("required persistence rejects a layout that needs bounded rounds",
          "[unit][mpi][neighbor][async][context][bounded][persistent]") {
#if KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_INIT && \
    !KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_INIT_C
  communicator_view const world{MPI_COMM_WORLD};
  distributed_graph graph{world, {world.rank()}};
  require_collective_semantic_error(
      [&] {
        neighbor_all_to_all_v_context<int> context{
            graph,
            {5},
            context_options{
                .collective = collective_options{.mpi3_round_ceiling = 2},
                .persistence = persistence_policy::required}};
        static_cast<void>(context);
      },
      "persistent neighborhood exchange requires a single representable "
      "payload",
      world);
#endif
}

TEST_CASE("asymmetric backend masks select one common backend collectively",
          "[unit][mpi][neighbor][async][backend][agreement]") {
  communicator_view const world{MPI_COMM_WORLD};
  if (world.size() < 2) {
    return;
  }

  using parhip::mpi::detail::agree_neighbor_backend_masks;
  using parhip::mpi::detail::backend_bit;
  using parhip::mpi::detail::choose_direct_backend;
  using parhip::mpi::detail::filter_local_backend_masks;
  using parhip::mpi::detail::neighbor_backend_mask;
  using parhip::mpi::detail::neighbor_backend_masks;
  using parhip::mpi::detail::neighbor_direct_backend;

  auto const bit = [](neighbor_direct_backend backend) {
    return backend_bit(backend);
  };
  auto const immediate_legacy = bit(neighbor_direct_backend::immediate_legacy);
  auto const immediate_large =
      bit(neighbor_direct_backend::immediate_large_count);
  auto const persistent_legacy =
      bit(neighbor_direct_backend::persistent_legacy);
  auto const persistent_large =
      bit(neighbor_direct_backend::persistent_large_count);
  auto const all_backends =
      immediate_legacy | immediate_large | persistent_legacy | persistent_large;
  struct agreement_case final {
    std::string_view name;
    persistence_policy policy;
    neighbor_backend_mask even_available;
    neighbor_backend_mask odd_available;
    bool even_physical_legacy = true;
    bool odd_physical_legacy = true;
    neighbor_backend_mask expected_allowed;
    neighbor_backend_mask expected_physical;
    std::optional<neighbor_direct_backend> expected;
  };
  auto const cases = std::array{
      agreement_case{
          .name = "disabled keeps only immediate backends",
          .policy = persistence_policy::disabled,
          .even_available = all_backends,
          .odd_available = immediate_large | persistent_legacy,
          .expected_allowed = immediate_large,
          .expected_physical = immediate_large | persistent_legacy,
          .expected = neighbor_direct_backend::immediate_large_count,
      },
      agreement_case{
          .name = "prefer gives persistent precedence",
          .policy = persistence_policy::prefer,
          .even_available = all_backends,
          .odd_available = persistent_legacy | immediate_legacy,
          .expected_allowed = persistent_legacy | immediate_legacy,
          .expected_physical = persistent_legacy | immediate_legacy,
          .expected = neighbor_direct_backend::persistent_legacy,
      },
      agreement_case{
          .name = "required removes immediate backends",
          .policy = persistence_policy::required,
          .even_available = all_backends,
          .odd_available = persistent_legacy | immediate_legacy,
          .expected_allowed = persistent_legacy,
          .expected_physical = persistent_legacy | immediate_legacy,
          .expected = neighbor_direct_backend::persistent_legacy,
      },
      agreement_case{
          .name = "complementary capabilities have no common backend",
          .policy = persistence_policy::prefer,
          .even_available = immediate_legacy,
          .odd_available = immediate_large,
          .expected_allowed = 0,
          .expected_physical = 0,
          .expected = std::nullopt,
      },
      agreement_case{
          .name = "required persistent layout failure leaves immediate "
                  "physically available",
          .policy = persistence_policy::required,
          .even_available = immediate_large | persistent_legacy,
          .odd_available = immediate_large | persistent_legacy,
          .even_physical_legacy = true,
          .odd_physical_legacy = false,
          .expected_allowed = 0,
          .expected_physical = immediate_large,
          .expected = std::nullopt,
      },
  };

  for (auto const& test_case : cases) {
    INFO(test_case.name);
    auto const even = world.rank() % 2 == 0;
    auto const local = filter_local_backend_masks(
        even ? test_case.even_available : test_case.odd_available,
        test_case.policy, true,
        even ? test_case.even_physical_legacy : test_case.odd_physical_legacy,
        true);
    auto common = neighbor_backend_masks{};
    {
      backend_agreement_probe::activation observation{};
      common = agree_neighbor_backend_masks(local, world);
      REQUIRE(backend_agreement_probe::band_calls == 1);
    }
    auto const selected = choose_direct_backend(common.allowed);
    REQUIRE(selected == test_case.expected);
    REQUIRE(common.allowed == test_case.expected_allowed);
    REQUIRE(common.physical == test_case.expected_physical);

    auto const encoded = selected.has_value()
                             ? static_cast<std::uint64_t>(*selected)
                             : std::numeric_limits<std::uint64_t>::max();
    auto minimum = std::uint64_t{0};
    auto maximum = std::uint64_t{0};
    REQUIRE(PMPI_Allreduce(&encoded, &minimum, 1, MPI_UINT64_T, MPI_MIN,
                           world.native_handle()) == MPI_SUCCESS);
    REQUIRE(PMPI_Allreduce(&encoded, &maximum, 1, MPI_UINT64_T, MPI_MAX,
                           world.native_handle()) == MPI_SUCCESS);
    REQUIRE(minimum == maximum);
  }
}
}  // namespace
