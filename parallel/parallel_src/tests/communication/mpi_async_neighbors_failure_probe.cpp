#include <mpi.h>

#include <unistd.h>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <utility>
#include <vector>

#include "communication/mpi_adapter.h"
#include "kahip_mpi_capabilities.h"

namespace async_failure_support {
struct wire_entry {
  std::uint64_t value;
  int rank;
};
}  // namespace async_failure_support

template <>
struct parhip::mpi::wire_members<async_failure_support::wire_entry> {
  inline static constexpr auto value =
      boost::hana::make_tuple(&async_failure_support::wire_entry::value,
                              &async_failure_support::wire_entry::rank);
};

namespace {
enum class failure_mode {
  immediate_init,
  test,
  wait,
  destructor_wait,
  persistent_init,
  persistent_start,
  persistent_test,
  persistent_wait,
  request_free,
  active_restart,
  inactive_test,
  bounded_inactive_test,
  bounded_inactive_wait,
  bounded_later_init,
  bounded_later_test,
  bounded_later_wait,
  send_while_active,
  receive_while_active,
  post_finalize_active_request,
  post_finalize_complete_request,
  post_finalize_complete_wait,
  post_finalize_immediate_context,
  post_finalize_persistent_context,
  initialized_query,
  finalized_query,
};

constexpr auto injected_mpi_error = MPI_ERR_OTHER;
constexpr auto injected_lifecycle_error = 17295;
auto selected_mode = failure_mode::immediate_init;
auto operation_communicator = MPI_COMM_NULL;
auto operation_datatype = MPI_DATATYPE_NULL;
auto operation_request = MPI_REQUEST_NULL;
auto failure_was_injected = false;
auto runtime_was_finalized = false;
auto inject_initialized_query = false;
auto inject_finalized_query = false;
auto underlying_request_completed = false;
auto bounded_init_attempts = 0;

[[nodiscard]] auto mode_name() noexcept -> std::string_view {
  switch (selected_mode) {
    case failure_mode::immediate_init:
      return "immediate-init";
    case failure_mode::test:
      return "test";
    case failure_mode::wait:
      return "wait";
    case failure_mode::destructor_wait:
      return "destructor-wait";
    case failure_mode::persistent_init:
      return "persistent-init";
    case failure_mode::persistent_start:
      return "persistent-start";
    case failure_mode::persistent_test:
      return "persistent-test";
    case failure_mode::persistent_wait:
      return "persistent-wait";
    case failure_mode::request_free:
      return "request-free";
    case failure_mode::active_restart:
      return "active-restart";
    case failure_mode::inactive_test:
      return "inactive-test";
    case failure_mode::bounded_inactive_test:
      return "bounded-inactive-test";
    case failure_mode::bounded_inactive_wait:
      return "bounded-inactive-wait";
    case failure_mode::bounded_later_init:
      return "bounded-later-init";
    case failure_mode::bounded_later_test:
      return "bounded-later-test";
    case failure_mode::bounded_later_wait:
      return "bounded-later-wait";
    case failure_mode::send_while_active:
      return "send-while-active";
    case failure_mode::receive_while_active:
      return "receive-while-active";
    case failure_mode::post_finalize_active_request:
      return "post-finalize-active-request";
    case failure_mode::post_finalize_complete_request:
      return "post-finalize-complete-request";
    case failure_mode::post_finalize_complete_wait:
      return "post-finalize-complete-wait";
    case failure_mode::post_finalize_immediate_context:
      return "post-finalize-immediate-context";
    case failure_mode::post_finalize_persistent_context:
      return "post-finalize-persistent-context";
    case failure_mode::initialized_query:
      return "initialized-query";
    case failure_mode::finalized_query:
      return "finalized-query";
  }
  return "unknown";
}

[[nodiscard]] auto is_persistent_mode() noexcept -> bool {
  return selected_mode == failure_mode::persistent_init ||
         selected_mode == failure_mode::persistent_start ||
         selected_mode == failure_mode::persistent_test ||
         selected_mode == failure_mode::persistent_wait ||
         selected_mode == failure_mode::request_free ||
         selected_mode == failure_mode::post_finalize_persistent_context;
}

[[nodiscard]] auto expects_raw_abort() noexcept -> bool {
  return selected_mode == failure_mode::post_finalize_active_request ||
         selected_mode == failure_mode::post_finalize_complete_request ||
         selected_mode == failure_mode::post_finalize_complete_wait ||
         selected_mode == failure_mode::post_finalize_immediate_context ||
         selected_mode == failure_mode::post_finalize_persistent_context ||
         selected_mode == failure_mode::initialized_query ||
         selected_mode == failure_mode::finalized_query;
}

[[noreturn]] void forbidden_cleanup(char const* operation) noexcept {
  auto const mode = mode_name();
  std::fprintf(stderr, "forbidden cleanup after %.*s failure: %s\n",
               static_cast<int>(mode.size()), mode.data(), operation);
  std::_Exit(90);
}

void announce_injection() noexcept {
  auto const mode = mode_name();
  std::fprintf(stderr, "injecting raw MPI error %d for %.*s\n",
               injected_mpi_error, static_cast<int>(mode.size()), mode.data());
  failure_was_injected = true;
}

[[noreturn]] void observed_raw_abort(int) noexcept {
  constexpr char prefix[] = "observed SIGABRT for ";
  constexpr char suffix[] = " raw-abort path\n";
  static_cast<void>(::write(STDERR_FILENO, prefix, sizeof(prefix) - 1));
  auto const mode = mode_name();
  static_cast<void>(::write(STDERR_FILENO, mode.data(), mode.size()));
  static_cast<void>(::write(STDERR_FILENO, suffix, sizeof(suffix) - 1));
  std::_Exit(86);
}

void track_operation(MPI_Comm communicator, MPI_Datatype datatype) noexcept {
  operation_communicator = communicator;
  operation_datatype = datatype;
}

void track_request(MPI_Request request) noexcept {
  operation_request = request;
}

[[nodiscard]] auto is_operation_request(MPI_Request const* request) noexcept
    -> bool {
  return request != nullptr && operation_request != MPI_REQUEST_NULL &&
         *request == operation_request;
}

void forbid_target_cleanup_when_terminating(char const* operation) {
  if (failure_was_injected || runtime_was_finalized) {
    forbidden_cleanup(operation);
  }
}
}  // namespace

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
  track_operation(communicator, send_datatype);
  if (selected_mode == failure_mode::bounded_later_init ||
      selected_mode == failure_mode::bounded_later_test ||
      selected_mode == failure_mode::bounded_later_wait) {
    ++bounded_init_attempts;
  }
  if (selected_mode == failure_mode::immediate_init) {
    announce_injection();
    return injected_mpi_error;
  }
  if (selected_mode == failure_mode::bounded_later_init &&
      bounded_init_attempts == 2) {
    announce_injection();
    return injected_mpi_error;
  }
  auto const result = PMPI_Ineighbor_alltoallv(
      send_buffer, send_counts, send_displacements, send_datatype,
      receive_buffer, receive_counts, receive_displacements, receive_datatype,
      communicator, request);
  if (result == MPI_SUCCESS) {
    track_request(*request);
  }
  return result;
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
  track_operation(communicator, send_datatype);
  if (selected_mode == failure_mode::immediate_init) {
    announce_injection();
    return injected_mpi_error;
  }
  auto const result = PMPI_Ineighbor_alltoallv_c(
      send_buffer, send_counts, send_displacements, send_datatype,
      receive_buffer, receive_counts, receive_displacements, receive_datatype,
      communicator, request);
  if (result == MPI_SUCCESS) {
    track_request(*request);
  }
  return result;
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
  track_operation(communicator, send_datatype);
  if (selected_mode == failure_mode::persistent_init) {
    announce_injection();
    return injected_mpi_error;
  }
  auto const result = PMPI_Neighbor_alltoallv_init(
      send_buffer, send_counts, send_displacements, send_datatype,
      receive_buffer, receive_counts, receive_displacements, receive_datatype,
      communicator, info, request);
  if (result == MPI_SUCCESS) {
    track_request(*request);
  }
  return result;
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
  track_operation(communicator, send_datatype);
  if (selected_mode == failure_mode::persistent_init) {
    announce_injection();
    return injected_mpi_error;
  }
  auto const result = PMPI_Neighbor_alltoallv_init_c(
      send_buffer, send_counts, send_displacements, send_datatype,
      receive_buffer, receive_counts, receive_displacements, receive_datatype,
      communicator, info, request);
  if (result == MPI_SUCCESS) {
    track_request(*request);
  }
  return result;
}
#endif

extern "C" int MPI_Start(MPI_Request* request) {
  if (is_operation_request(request) &&
      selected_mode == failure_mode::persistent_start) {
    announce_injection();
    return injected_mpi_error;
  }
  return PMPI_Start(request);
}

extern "C" int MPI_Test(MPI_Request* request,
                        int* complete,
                        MPI_Status* status) {
  auto const tracked = is_operation_request(request);
  if (tracked && (selected_mode == failure_mode::test ||
                  selected_mode == failure_mode::persistent_test)) {
    announce_injection();
    return injected_mpi_error;
  }
  if (tracked && selected_mode == failure_mode::bounded_later_test &&
      bounded_init_attempts >= 2) {
    announce_injection();
    return injected_mpi_error;
  }
  auto const result = PMPI_Test(request, complete, status);
  if (tracked && result == MPI_SUCCESS && *complete != 0 &&
      selected_mode == failure_mode::post_finalize_active_request) {
    underlying_request_completed = true;
    *complete = 0;
  }
  return result;
}

extern "C" int MPI_Wait(MPI_Request* request, MPI_Status* status) {
  if (runtime_was_finalized) {
    forbidden_cleanup("MPI_Wait");
  }
  auto const tracked = is_operation_request(request);
  if (tracked && (selected_mode == failure_mode::wait ||
                  selected_mode == failure_mode::destructor_wait ||
                  selected_mode == failure_mode::persistent_wait)) {
    announce_injection();
    return injected_mpi_error;
  }
  if (tracked && selected_mode == failure_mode::bounded_later_wait &&
      bounded_init_attempts >= 2) {
    announce_injection();
    return injected_mpi_error;
  }
  forbid_target_cleanup_when_terminating("MPI_Wait");
  return PMPI_Wait(request, status);
}

extern "C" int MPI_Request_free(MPI_Request* request) {
  auto const tracked = is_operation_request(request);
  if (tracked) {
    forbid_target_cleanup_when_terminating("MPI_Request_free");
    if (selected_mode == failure_mode::request_free) {
      announce_injection();
      return injected_mpi_error;
    }
  }
  return PMPI_Request_free(request);
}

extern "C" int MPI_Type_free(MPI_Datatype* datatype) {
  auto const tracked = operation_datatype != MPI_DATATYPE_NULL &&
                       *datatype == operation_datatype;
  if (tracked) {
    forbid_target_cleanup_when_terminating("MPI_Type_free");
  }
  return PMPI_Type_free(datatype);
}

extern "C" int MPI_Comm_free(MPI_Comm* communicator) {
  auto const tracked = operation_communicator != MPI_COMM_NULL &&
                       *communicator == operation_communicator;
  if (tracked) {
    forbid_target_cleanup_when_terminating("MPI_Comm_free");
  }
  return PMPI_Comm_free(communicator);
}

extern "C" int MPI_Error_string(int error_code, char* text, int* text_length) {
  if (expects_raw_abort()) {
    forbidden_cleanup("MPI_Error_string");
  }
  return PMPI_Error_string(error_code, text, text_length);
}

extern "C" int MPI_Abort(MPI_Comm communicator, int) {
  if (expects_raw_abort()) {
    forbidden_cleanup("MPI_Abort");
  }
  auto const mode = mode_name();
  auto const affected = communicator == operation_communicator
                            ? std::string_view{"operation"}
                            : std::string_view{"unexpected"};
  std::fprintf(stderr, "observed MPI_Abort for %.*s on %.*s communicator\n",
               static_cast<int>(mode.size()), mode.data(),
               static_cast<int>(affected.size()), affected.data());
  std::_Exit(affected == "operation" ? 86 : 91);
}

extern "C" int MPI_Initialized(int* flag) {
  if (inject_initialized_query) {
    inject_initialized_query = false;
    std::fprintf(stderr,
                 "injecting raw lifecycle error %d for MPI_Initialized\n",
                 injected_lifecycle_error);
    return injected_lifecycle_error;
  }
  return PMPI_Initialized(flag);
}

extern "C" int MPI_Finalized(int* flag) {
  if (inject_finalized_query) {
    inject_finalized_query = false;
    std::fprintf(stderr, "injecting raw lifecycle error %d for MPI_Finalized\n",
                 injected_lifecycle_error);
    return injected_lifecycle_error;
  }
  return PMPI_Finalized(flag);
}

namespace {
using async_failure_support::wire_entry;
using parhip::mpi::collective_options;
using parhip::mpi::communicator_view;
using parhip::mpi::context_options;
using parhip::mpi::distributed_graph;
using parhip::mpi::neighbor_all_to_all_v_context;
using parhip::mpi::persistence_policy;
using parhip::mpi::segmented_buffer;
using parhip::mpi::start_neighbor_all_to_all_v;

[[nodiscard]] auto parse_mode(std::string_view mode) -> bool {
#define KAHIP_PARSE_MODE(text, value)    \
  if (mode == text) {                    \
    selected_mode = failure_mode::value; \
    return true;                         \
  }
  KAHIP_PARSE_MODE("immediate-init", immediate_init)
  KAHIP_PARSE_MODE("test", test)
  KAHIP_PARSE_MODE("wait", wait)
  KAHIP_PARSE_MODE("destructor-wait", destructor_wait)
  KAHIP_PARSE_MODE("persistent-init", persistent_init)
  KAHIP_PARSE_MODE("persistent-start", persistent_start)
  KAHIP_PARSE_MODE("persistent-test", persistent_test)
  KAHIP_PARSE_MODE("persistent-wait", persistent_wait)
  KAHIP_PARSE_MODE("request-free", request_free)
  KAHIP_PARSE_MODE("active-restart", active_restart)
  KAHIP_PARSE_MODE("inactive-test", inactive_test)
  KAHIP_PARSE_MODE("bounded-inactive-test", bounded_inactive_test)
  KAHIP_PARSE_MODE("bounded-inactive-wait", bounded_inactive_wait)
  KAHIP_PARSE_MODE("bounded-later-init", bounded_later_init)
  KAHIP_PARSE_MODE("bounded-later-test", bounded_later_test)
  KAHIP_PARSE_MODE("bounded-later-wait", bounded_later_wait)
  KAHIP_PARSE_MODE("send-while-active", send_while_active)
  KAHIP_PARSE_MODE("receive-while-active", receive_while_active)
  KAHIP_PARSE_MODE("post-finalize-active-request", post_finalize_active_request)
  KAHIP_PARSE_MODE("post-finalize-complete-request",
                   post_finalize_complete_request)
  KAHIP_PARSE_MODE("post-finalize-complete-wait", post_finalize_complete_wait)
  KAHIP_PARSE_MODE("post-finalize-immediate-context",
                   post_finalize_immediate_context)
  KAHIP_PARSE_MODE("post-finalize-persistent-context",
                   post_finalize_persistent_context)
  KAHIP_PARSE_MODE("initialized-query", initialized_query)
  KAHIP_PARSE_MODE("finalized-query", finalized_query)
#undef KAHIP_PARSE_MODE
  return false;
}

[[nodiscard]] auto sends() -> segmented_buffer<wire_entry> {
  return segmented_buffer<wire_entry>::from_segments(
      std::vector<std::vector<wire_entry>>{{{7, 0}}});
}

void finalize_for_raw_abort() {
  auto const result = MPI_Finalize();
  if (result != MPI_SUCCESS) {
    std::fprintf(stderr, "MPI_Finalize returned raw error %d\n", result);
    std::_Exit(70);
  }
  runtime_was_finalized = true;
}

void run_one_shot(distributed_graph const& graph) {
  auto request = start_neighbor_all_to_all_v(sends(), graph);
  switch (selected_mode) {
    case failure_mode::test:
      static_cast<void>(request.test());
      return;
    case failure_mode::wait:
      static_cast<void>(std::move(request).wait());
      return;
    case failure_mode::destructor_wait:
      return;
    case failure_mode::post_finalize_active_request:
      while (!underlying_request_completed) {
        if (request.test()) {
          std::fputs("active request completed visibly before finalization\n",
                     stderr);
          std::_Exit(2);
        }
      }
      finalize_for_raw_abort();
      return;
    case failure_mode::post_finalize_complete_request:
      while (!request.test()) {
      }
      finalize_for_raw_abort();
      static_cast<void>(request.test());
      return;
    case failure_mode::post_finalize_complete_wait:
      while (!request.test()) {
      }
      finalize_for_raw_abort();
      static_cast<void>(std::move(request).wait());
      return;
    default:
      return;
  }
}

void run_context(distributed_graph const& graph) {
  auto const policy = is_persistent_mode() ? persistence_policy::required
                                           : persistence_policy::disabled;
  auto const bounded_mode =
      selected_mode == failure_mode::bounded_inactive_test ||
      selected_mode == failure_mode::bounded_inactive_wait ||
      selected_mode == failure_mode::bounded_later_init ||
      selected_mode == failure_mode::bounded_later_test ||
      selected_mode == failure_mode::bounded_later_wait;
  auto const options =
      bounded_mode
          ? context_options{
                .collective = collective_options{.mpi3_round_ceiling = 2,
                                                  .force_mpi3 = true},
                .persistence = persistence_policy::disabled}
          : context_options{.persistence = policy};
  neighbor_all_to_all_v_context<wire_entry> context{
      graph, {bounded_mode ? std::size_t{5} : std::size_t{1}}, options};

  switch (selected_mode) {
    case failure_mode::persistent_init:
      return;
    case failure_mode::active_restart:
      context.start();
      context.start();
      return;
    case failure_mode::inactive_test:
      context.start();
      context.wait();
      static_cast<void>(context.test());
      return;
    case failure_mode::bounded_inactive_test:
      context.start();
      context.wait();
      static_cast<void>(context.test());
      return;
    case failure_mode::bounded_inactive_wait:
      context.start();
      context.wait();
      context.wait();
      return;
    case failure_mode::bounded_later_init:
    case failure_mode::bounded_later_wait:
      context.start();
      context.wait();
      return;
    case failure_mode::bounded_later_test:
      context.start();
      while (!context.test()) {
      }
      return;
    case failure_mode::send_while_active:
      context.start();
      static_cast<void>(context.send_segment(0));
      return;
    case failure_mode::receive_while_active:
      context.start();
      static_cast<void>(context.received_segment(0));
      return;
    case failure_mode::persistent_start:
      context.start();
      return;
    case failure_mode::persistent_test:
      context.start();
      static_cast<void>(context.test());
      return;
    case failure_mode::persistent_wait:
      context.start();
      context.wait();
      return;
    case failure_mode::request_free:
      return;
    case failure_mode::post_finalize_immediate_context:
    case failure_mode::post_finalize_persistent_context:
      context.start();
      context.wait();
      finalize_for_raw_abort();
      return;
    case failure_mode::initialized_query:
    case failure_mode::finalized_query:
      context.start();
      context.wait();
      inject_initialized_query =
          selected_mode == failure_mode::initialized_query;
      inject_finalized_query = selected_mode == failure_mode::finalized_query;
      return;
    default:
      return;
  }
}
}  // namespace

int main(int argc, char* argv[]) {
  if (argc != 2 || !parse_mode(argv[1])) {
    std::fputs("usage: mpi_async_neighbors_failure_probe MODE\n", stderr);
    return 64;
  }
  if (std::signal(SIGABRT, observed_raw_abort) == SIG_ERR) {
    std::fputs("could not install SIGABRT observation handler\n", stderr);
    return 70;
  }
  auto const init_result = MPI_Init(&argc, &argv);
  if (init_result != MPI_SUCCESS) {
    std::fprintf(stderr, "MPI_Init returned raw error %d\n", init_result);
    return 70;
  }

  {
    communicator_view const world{MPI_COMM_WORLD};
    distributed_graph graph{world, {world.rank()}};
    if (selected_mode == failure_mode::immediate_init ||
        selected_mode == failure_mode::test ||
        selected_mode == failure_mode::wait ||
        selected_mode == failure_mode::destructor_wait ||
        selected_mode == failure_mode::post_finalize_active_request ||
        selected_mode == failure_mode::post_finalize_complete_request ||
        selected_mode == failure_mode::post_finalize_complete_wait) {
      run_one_shot(graph);
    } else {
      run_context(graph);
    }
  }

  auto const mode = mode_name();
  std::fprintf(stderr, "%.*s failure did not abort\n",
               static_cast<int>(mode.size()), mode.data());
  if (!runtime_was_finalized) {
    static_cast<void>(MPI_Finalize());
  }
  return 2;
}
