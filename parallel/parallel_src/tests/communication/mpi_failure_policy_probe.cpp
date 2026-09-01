#include <mpi.h>

#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <string_view>

#include "communication/mpi_adapter.h"

namespace {
enum class failure_mode {
  pre_init_error,
  semantic_factory_resource,
  error_string_secondary,
  wrong_topology,
  capacity_resolver,
};

auto selected_mode = failure_mode::pre_init_error;
auto pre_initialization_control_is_active = false;
auto pre_initialization_mpi_calls = 0;
auto injection_is_armed = false;
auto track_next_duplicate = false;
auto tracked_communicator = MPI_COMM_NULL;
auto error_string_attempts = 0;
auto cleanup_attempts = 0;
auto capacity_allreduce_attempts = 0;
auto cached_rank = -1;

constexpr auto original_backend_error = 17291;
constexpr auto secondary_formatter_error = 17292;

void record_pre_initialization_mpi_call(char const* operation) noexcept {
  if (!pre_initialization_control_is_active) {
    return;
  }
  ++pre_initialization_mpi_calls;
  std::fprintf(stderr,
               "forbidden MPI call while constructing pre-init mpi_error: %s\n",
               operation);
}

[[noreturn]] void forbidden_failure_path_call(char const* operation) noexcept {
  std::fprintf(stderr, "forbidden MPI call or cleanup after injection: %s\n",
               operation);
  std::_Exit(90);
}

[[nodiscard]] auto affected_name(MPI_Comm communicator) noexcept
    -> std::string_view {
  if (communicator == tracked_communicator) {
    switch (selected_mode) {
      case failure_mode::semantic_factory_resource:
        return "semantic";
      case failure_mode::error_string_secondary:
        return "backend";
      case failure_mode::wrong_topology:
        return "topology";
      case failure_mode::capacity_resolver:
        return "capacity";
      case failure_mode::pre_init_error:
        break;
    }
  }
  if (communicator == MPI_COMM_WORLD) {
    return "world";
  }
  if (communicator == MPI_COMM_NULL) {
    return "null";
  }
  return "other";
}

[[noreturn]] void returned_from_failure(char const* mode) noexcept {
  std::fprintf(stderr, "returned-from-failure: %s\n", mode);
  std::_Exit(2);
}
}  // namespace

extern "C" int MPI_Error_string(int error_code,
                                char* error_text,
                                int* error_text_length) {
  if (pre_initialization_control_is_active) {
    record_pre_initialization_mpi_call("MPI_Error_string");
    if (error_text != nullptr) {
      error_text[0] = '\0';
    }
    if (error_text_length != nullptr) {
      *error_text_length = 0;
    }
    return MPI_ERR_OTHER;
  }
  if (injection_is_armed) {
    ++error_string_attempts;
    if (selected_mode != failure_mode::error_string_secondary ||
        error_string_attempts != 1) {
      forbidden_failure_path_call("MPI_Error_string");
    }
    std::fprintf(stderr,
                 "injected MPI_Error_string failure original=%d secondary=%d\n",
                 original_backend_error, secondary_formatter_error);
    return secondary_formatter_error;
  }
  return PMPI_Error_string(error_code, error_text, error_text_length);
}

extern "C" int MPI_Error_class(int error_code, int* error_class) {
  if (pre_initialization_control_is_active) {
    record_pre_initialization_mpi_call("MPI_Error_class");
    if (error_class != nullptr) {
      *error_class = error_code;
    }
    return MPI_SUCCESS;
  }
  if (injection_is_armed) {
    forbidden_failure_path_call("MPI_Error_class");
  }
  return PMPI_Error_class(error_code, error_class);
}

extern "C" int MPI_Initialized(int* initialized) {
  if (pre_initialization_control_is_active) {
    record_pre_initialization_mpi_call("MPI_Initialized");
    if (initialized != nullptr) {
      *initialized = 0;
    }
    return MPI_SUCCESS;
  }
  return PMPI_Initialized(initialized);
}

extern "C" int MPI_Finalized(int* finalized) {
  if (pre_initialization_control_is_active) {
    record_pre_initialization_mpi_call("MPI_Finalized");
    if (finalized != nullptr) {
      *finalized = 0;
    }
    return MPI_SUCCESS;
  }
  return PMPI_Finalized(finalized);
}

extern "C" int MPI_Abort(MPI_Comm communicator, int error_code) {
  if (pre_initialization_control_is_active) {
    record_pre_initialization_mpi_call("MPI_Abort");
    return MPI_ERR_OTHER;
  }
  if (injection_is_armed) {
    auto const affected = affected_name(communicator);
    std::fprintf(stderr,
                 "observed MPI_Abort rank=%d affected=%.*s "
                 "error-string-attempts=%d cleanup-attempts=%d "
                 "capacity-allreduce-attempts=%d\n",
                 cached_rank, static_cast<int>(affected.size()),
                 affected.data(), error_string_attempts, cleanup_attempts,
                 capacity_allreduce_attempts);
    std::_Exit(86);
  }
  return PMPI_Abort(communicator, error_code);
}

extern "C" int MPI_Allreduce(void const* send_buffer,
                             void* receive_buffer,
                             int count,
                             MPI_Datatype datatype,
                             MPI_Op operation,
                             MPI_Comm communicator) {
  if (injection_is_armed) {
    if (selected_mode != failure_mode::capacity_resolver) {
      forbidden_failure_path_call("MPI_Allreduce");
    }
    ++capacity_allreduce_attempts;
    if (capacity_allreduce_attempts != 1 || send_buffer == nullptr ||
        receive_buffer == nullptr || count != 2 || datatype != MPI_UINT64_T ||
        operation != MPI_BOR || communicator != tracked_communicator) {
      forbidden_failure_path_call("MPI_Allreduce(capacity resolver shape)");
    }
  }
  return PMPI_Allreduce(send_buffer, receive_buffer, count, datatype, operation,
                        communicator);
}

extern "C" int MPI_Comm_dup(MPI_Comm communicator,
                            MPI_Comm* duplicate_communicator) {
  auto const result = PMPI_Comm_dup(communicator, duplicate_communicator);
  if (result == MPI_SUCCESS && track_next_duplicate &&
      duplicate_communicator != nullptr) {
    tracked_communicator = *duplicate_communicator;
    track_next_duplicate = false;
    injection_is_armed = true;
    std::fputs("captured wrong-topology internal duplicate\n", stderr);
  }
  return result;
}

extern "C" int MPI_Comm_free(MPI_Comm* communicator) {
  if (injection_is_armed && communicator != nullptr &&
      *communicator == tracked_communicator) {
    ++cleanup_attempts;
    forbidden_failure_path_call("MPI_Comm_free(tracked duplicate)");
  }
  return PMPI_Comm_free(communicator);
}

namespace {
auto run_pre_initialization_control() -> int {
  pre_initialization_control_is_active = true;
  pre_initialization_mpi_calls = 0;

  constexpr auto context = std::string_view{"pre-init structured error"};
  auto const failure =
      parhip::mpi::mpi_error{MPI_ERR_ARG, std::string{context}};
  auto const message = std::string_view{failure.what()};
  auto const structured = failure.error_code() == MPI_ERR_ARG &&
                          failure.context() == context &&
                          failure.location().line() > 0 &&
                          message.find(context) != std::string_view::npos;
  pre_initialization_control_is_active = false;

  if (!structured || pre_initialization_mpi_calls != 0) {
    std::fprintf(
        stderr,
        "pre-init mpi_error control failed: structured=%d mpi-calls=%d\n",
        structured ? 1 : 0, pre_initialization_mpi_calls);
    return 2;
  }

  std::fprintf(stderr,
               "pre-init mpi_error remained MPI-free: raw=%d mpi-calls=0\n",
               failure.error_code());
  return 0;
}

[[noreturn]] void run_semantic_factory_resource_failure() {
  auto affected =
      parhip::mpi::communicator{parhip::mpi::communicator_view{MPI_COMM_WORLD}};
  tracked_communicator = affected.native_handle();
  injection_is_armed = true;

  parhip::mpi::detail::throw_collectively_agreed_semantic_error_from(
      affected.native_handle(), []() -> parhip::mpi::mpi_error {
        std::fputs("injected semantic factory bad_alloc\n", stderr);
        throw std::bad_alloc{};
      });
  returned_from_failure("semantic-factory-resource");
}

[[noreturn]] void run_error_string_secondary_failure() {
  auto affected =
      parhip::mpi::communicator{parhip::mpi::communicator_view{MPI_COMM_WORLD}};
  tracked_communicator = affected.native_handle();
  injection_is_armed = true;

  parhip::mpi::abort_on_mpi_error(affected.native_handle(),
                                  original_backend_error,
                                  "backend formatter failure");
}

[[noreturn]] void run_wrong_topology_failure() {
  track_next_duplicate = true;
  try {
    auto invalid =
        parhip::mpi::topology{parhip::mpi::communicator_view{MPI_COMM_WORLD}};
    static_cast<void>(invalid);
  } catch (...) {
    returned_from_failure("wrong-topology was catchable");
  }
  returned_from_failure("wrong-topology");
}

[[noreturn]] void run_capacity_resolver_failure() {
  auto affected =
      parhip::mpi::communicator{parhip::mpi::communicator_view{MPI_COMM_WORLD}};
  tracked_communicator = affected.native_handle();
  injection_is_armed = true;

  auto local = parhip::mpi::capacity_result{};
  if (cached_rank == 0) {
    local = parhip::mpi::with_fatal_capacity_issue(
        local, parhip::mpi::capacity_issue::cumulative_offset_overflow);
    std::fputs("injected rank-zero fatal capacity issue\n", stderr);
  } else {
    local = parhip::mpi::with_bounded_capacity_issue(
        local,
        parhip::mpi::capacity_issue::collective_layout_not_representable);
  }
  static_cast<void>(parhip::mpi::resolve_capacity_collectively(
      local, affected.native_handle(), affected.native_handle(),
      "capacity resolver probe"));
  returned_from_failure("capacity-resolver");
}
}  // namespace

int main(int argc, char* argv[]) {
  if (argc != 2) {
    std::fputs("usage: mpi_failure_policy_probe MODE\n", stderr);
    return 64;
  }
  auto const mode = std::string_view{argv[1]};
  if (mode == "pre-init-error") {
    selected_mode = failure_mode::pre_init_error;
    return run_pre_initialization_control();
  }
  if (mode == "semantic-factory-resource") {
    selected_mode = failure_mode::semantic_factory_resource;
  } else if (mode == "error-string-secondary") {
    selected_mode = failure_mode::error_string_secondary;
  } else if (mode == "wrong-topology") {
    selected_mode = failure_mode::wrong_topology;
  } else if (mode == "capacity-resolver") {
    selected_mode = failure_mode::capacity_resolver;
  } else {
    std::fprintf(stderr, "unknown failure-policy mode: %s\n", argv[1]);
    return 64;
  }

  auto const initialization_result = MPI_Init(&argc, &argv);
  if (initialization_result != MPI_SUCCESS) {
    std::fprintf(stderr, "MPI_Init returned raw error %d\n",
                 initialization_result);
    return 70;
  }
  if (PMPI_Comm_rank(MPI_COMM_WORLD, &cached_rank) != MPI_SUCCESS) {
    std::fputs("PMPI_Comm_rank failed before failure injection\n", stderr);
    return 70;
  }

  switch (selected_mode) {
    case failure_mode::semantic_factory_resource:
      run_semantic_factory_resource_failure();
    case failure_mode::error_string_secondary:
      run_error_string_secondary_failure();
    case failure_mode::wrong_topology:
      run_wrong_topology_failure();
    case failure_mode::capacity_resolver:
      run_capacity_resolver_failure();
    case failure_mode::pre_init_error:
      break;
  }
  returned_from_failure("unknown-active-mode");
}
