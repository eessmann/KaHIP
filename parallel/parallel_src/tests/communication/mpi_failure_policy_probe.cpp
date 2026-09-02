#include <mpi.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <vector>

#include "communication/mpi_adapter.h"

namespace failure_probe {
struct dense_wire_record final {
  std::uint64_t value;

  auto operator==(dense_wire_record const&) const -> bool = default;
};
}  // namespace failure_probe

template <>
struct parhip::mpi::wire_members<failure_probe::dense_wire_record> {
  inline static constexpr auto value =
      std::tuple{&failure_probe::dense_wire_record::value};
};

static_assert(std::is_standard_layout_v<failure_probe::dense_wire_record>);
static_assert(std::is_trivially_copyable_v<failure_probe::dense_wire_record>);

namespace {
enum class failure_mode {
  pre_init_error,
  semantic_factory_resource,
  error_string_secondary,
  null_communicator,
  intercommunicator,
  null_distributed_graph,
  wrong_topology,
  capacity_resolver,
  dense_receive_offset_capacity,
  dense_receive_byte_capacity,
  distributed_graph_degree_capacity,
  neighbor_receive_offset_capacity,
  neighbor_receive_byte_capacity,
  neighbor_bounded_round_arithmetic,
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
auto dense_count_exchange_attempts = 0;
auto dense_payload_attempts = 0;
auto dense_datatype_attempts = 0;
auto graph_semantic_validation_attempts = 0;
auto graph_create_attempts = 0;
auto neighbor_count_exchange_attempts = 0;
auto neighbor_payload_attempts = 0;
auto neighbor_datatype_attempts = 0;
auto neighbor_phase_round_attempts = 0;
auto neighbor_capacity_after_injection_attempts = 0;
auto neighbor_graph_communicator = MPI_COMM_NULL;
auto cached_rank = -1;
auto cached_size = -1;

constexpr auto original_backend_error = 17291;
constexpr auto secondary_formatter_error = 17292;

[[nodiscard]] auto is_dense_capacity_mode() noexcept -> bool {
  return selected_mode == failure_mode::dense_receive_offset_capacity ||
         selected_mode == failure_mode::dense_receive_byte_capacity;
}

[[nodiscard]] auto is_graph_capacity_mode() noexcept -> bool {
  return selected_mode == failure_mode::distributed_graph_degree_capacity;
}

[[nodiscard]] auto is_neighbor_capacity_mode() noexcept -> bool {
  return selected_mode == failure_mode::neighbor_receive_offset_capacity ||
         selected_mode == failure_mode::neighbor_receive_byte_capacity ||
         selected_mode == failure_mode::neighbor_bounded_round_arithmetic;
}

[[nodiscard]] auto is_neighbor_receive_capacity_mode() noexcept -> bool {
  return selected_mode == failure_mode::neighbor_receive_offset_capacity ||
         selected_mode == failure_mode::neighbor_receive_byte_capacity;
}

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

void record_forbidden_datatype_attempt(char const* operation) noexcept {
  if (is_neighbor_receive_capacity_mode()) {
    ++neighbor_datatype_attempts;
  } else {
    ++dense_datatype_attempts;
  }
  forbidden_failure_path_call(operation);
}

[[nodiscard]] auto affected_name(MPI_Comm communicator) noexcept
    -> std::string_view {
  if (communicator == tracked_communicator) {
    switch (selected_mode) {
      case failure_mode::semantic_factory_resource:
        return "semantic";
      case failure_mode::error_string_secondary:
        return "backend";
      case failure_mode::null_communicator:
      case failure_mode::intercommunicator:
        return "communicator-guard";
      case failure_mode::null_distributed_graph:
        return "graph-guard";
      case failure_mode::wrong_topology:
        return "topology";
      case failure_mode::capacity_resolver:
        return "capacity";
      case failure_mode::dense_receive_offset_capacity:
      case failure_mode::dense_receive_byte_capacity:
        return "dense-operation";
      case failure_mode::distributed_graph_degree_capacity:
        return "graph-validation";
      case failure_mode::neighbor_receive_offset_capacity:
      case failure_mode::neighbor_receive_byte_capacity:
      case failure_mode::neighbor_bounded_round_arithmetic:
        return "neighbor-operation";
      case failure_mode::pre_init_error:
        break;
    }
  }
  if (communicator == neighbor_graph_communicator) {
    return "neighbor-graph";
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
                 "capacity-allreduce-attempts=%d "
                 "dense-count-exchange-attempts=%d "
                 "dense-payload-attempts=%d dense-datatype-attempts=%d "
                 "graph-semantic-validation-attempts=%d "
                 "graph-create-attempts=%d "
                 "neighbor-count-exchange-attempts=%d "
                 "neighbor-payload-attempts=%d "
                 "neighbor-datatype-attempts=%d "
                 "neighbor-phase-round-attempts=%d "
                 "neighbor-capacity-after-injection-attempts=%d\n",
                 cached_rank, static_cast<int>(affected.size()),
                 affected.data(), error_string_attempts, cleanup_attempts,
                 capacity_allreduce_attempts, dense_count_exchange_attempts,
                 dense_payload_attempts, dense_datatype_attempts,
                 graph_semantic_validation_attempts, graph_create_attempts,
                 neighbor_count_exchange_attempts, neighbor_payload_attempts,
                 neighbor_datatype_attempts, neighbor_phase_round_attempts,
                 neighbor_capacity_after_injection_attempts);
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
  if (is_graph_capacity_mode() && communicator == tracked_communicator) {
    if (count == 1 && datatype == MPI_INT && operation == MPI_MIN) {
      ++graph_semantic_validation_attempts;
      return PMPI_Allreduce(send_buffer, receive_buffer, count, datatype,
                            operation, communicator);
    }
    if (count != 2 || datatype != MPI_UINT64_T || operation != MPI_BOR ||
        send_buffer == nullptr || receive_buffer == nullptr) {
      forbidden_failure_path_call(
          "MPI_Allreduce(distributed graph capacity shape)");
    }

    ++capacity_allreduce_attempts;
    if (capacity_allreduce_attempts != 1) {
      forbidden_failure_path_call(
          "MPI_Allreduce(distributed graph capacity count)");
    }
    auto injected =
        std::array{static_cast<std::uint64_t const*>(send_buffer)[0],
                   static_cast<std::uint64_t const*>(send_buffer)[1]};
    if (cached_rank == 0) {
      injected[0] |= parhip::mpi::capacity_issue_mask(
          parhip::mpi::capacity_issue::topology_degree_not_representable);
      std::fputs("injected rank-zero distributed graph degree capacity\n",
                 stderr);
    }
    injection_is_armed = true;
    return PMPI_Allreduce(injected.data(), receive_buffer, count, datatype,
                          operation, communicator);
  }
  if (is_neighbor_capacity_mode() && communicator == tracked_communicator) {
    if (selected_mode == failure_mode::neighbor_bounded_round_arithmetic &&
        count == 1 && datatype == MPI_UINT64_T && operation == MPI_MAX) {
      ++neighbor_phase_round_attempts;
      if (neighbor_phase_round_attempts > cached_size) {
        forbidden_failure_path_call(
            "MPI_Allreduce(neighbor bounded phase count)");
      }
      auto injected = *static_cast<std::uint64_t const*>(send_buffer);
      if (neighbor_phase_round_attempts == 1 && cached_rank == 0) {
        injected = std::numeric_limits<std::uint64_t>::max();
        std::fputs(
            "injected rank-zero bounded neighbor round arithmetic capacity\n",
            stderr);
      }
      if (neighbor_phase_round_attempts == 1) {
        injection_is_armed = true;
      }
      return PMPI_Allreduce(&injected, receive_buffer, count, datatype,
                            operation, communicator);
    }
    if (count == 2 && datatype == MPI_UINT64_T && operation == MPI_BOR) {
      ++capacity_allreduce_attempts;
      if (injection_is_armed) {
        ++neighbor_capacity_after_injection_attempts;
      }
      auto const expected_capacity_attempts =
          selected_mode == failure_mode::neighbor_bounded_round_arithmetic ? 2
                                                                           : 1;
      if (capacity_allreduce_attempts > expected_capacity_attempts ||
          neighbor_capacity_after_injection_attempts > 1 ||
          send_buffer == nullptr || receive_buffer == nullptr) {
        forbidden_failure_path_call(
            "MPI_Allreduce(neighbor capacity resolver shape)");
      }
      return PMPI_Allreduce(send_buffer, receive_buffer, count, datatype,
                            operation, communicator);
    }
    if (injection_is_armed) {
      forbidden_failure_path_call("MPI_Allreduce(neighbor failure path)");
    }
    return PMPI_Allreduce(send_buffer, receive_buffer, count, datatype,
                          operation, communicator);
  }
  if (injection_is_armed) {
    if (selected_mode != failure_mode::capacity_resolver &&
        !is_dense_capacity_mode()) {
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

extern "C" int MPI_Dist_graph_create(MPI_Comm old_communicator,
                                     int source_count,
                                     int const sources[],
                                     int const degrees[],
                                     int const destinations[],
                                     int const weights[],
                                     MPI_Info info,
                                     int reorder,
                                     MPI_Comm* graph_communicator) {
  if (is_graph_capacity_mode()) {
    ++graph_create_attempts;
    if (injection_is_armed) {
      forbidden_failure_path_call("MPI_Dist_graph_create");
    }
  }
  return PMPI_Dist_graph_create(old_communicator, source_count, sources,
                                degrees, destinations, weights, info, reorder,
                                graph_communicator);
}

extern "C" int MPI_Alltoall(void const* send_buffer,
                            int send_count,
                            MPI_Datatype send_datatype,
                            void* receive_buffer,
                            int receive_count,
                            MPI_Datatype receive_datatype,
                            MPI_Comm communicator) {
  auto const result =
      PMPI_Alltoall(send_buffer, send_count, send_datatype, receive_buffer,
                    receive_count, receive_datatype, communicator);
  if (!is_dense_capacity_mode()) {
    return result;
  }

  ++dense_count_exchange_attempts;
  if (result != MPI_SUCCESS || dense_count_exchange_attempts != 1 ||
      cached_size != 2 || receive_buffer == nullptr || send_count != 1 ||
      receive_count != 1 || send_datatype != MPI_UINT64_T ||
      receive_datatype != MPI_UINT64_T ||
      communicator != tracked_communicator) {
    forbidden_failure_path_call("MPI_Alltoall(dense count exchange shape)");
  }

  if (cached_rank == 0) {
    auto* counts = static_cast<std::uint64_t*>(receive_buffer);
    static_assert(sizeof(std::size_t) <= sizeof(std::uint64_t));
    if (selected_mode == failure_mode::dense_receive_offset_capacity) {
      counts[0] = std::numeric_limits<std::size_t>::max();
      counts[1] = std::uint64_t{1};
      std::fputs("injected rank-zero dense receive offset capacity\n", stderr);
    } else {
      counts[0] = std::numeric_limits<std::size_t>::max() /
                      sizeof(failure_probe::dense_wire_record) +
                  std::uint64_t{1};
      counts[1] = std::uint64_t{0};
      std::fputs("injected rank-zero dense receive byte capacity\n", stderr);
    }
  }
  injection_is_armed = true;
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
  if (!is_neighbor_capacity_mode()) {
    return result;
  }

  ++neighbor_count_exchange_attempts;
  if (result != MPI_SUCCESS || neighbor_count_exchange_attempts != 1 ||
      cached_size != 2 || receive_buffer == nullptr || send_count != 1 ||
      receive_count != 1 || send_datatype != MPI_UINT64_T ||
      receive_datatype != MPI_UINT64_T ||
      communicator != tracked_communicator) {
    forbidden_failure_path_call(
        "MPI_Neighbor_alltoall(neighbor count exchange shape)");
  }

  if (is_neighbor_receive_capacity_mode()) {
    if (cached_rank == 0) {
      auto* counts = static_cast<std::uint64_t*>(receive_buffer);
      static_assert(sizeof(std::size_t) <= sizeof(std::uint64_t));
      if (selected_mode == failure_mode::neighbor_receive_offset_capacity) {
        counts[0] = std::numeric_limits<std::size_t>::max();
        counts[1] = std::uint64_t{1};
        std::fputs("injected rank-zero neighbor receive offset capacity\n",
                   stderr);
      } else {
        counts[0] = std::numeric_limits<std::size_t>::max() /
                        sizeof(failure_probe::dense_wire_record) +
                    std::uint64_t{1};
        counts[1] = std::uint64_t{0};
        std::fputs("injected rank-zero neighbor receive byte capacity\n",
                   stderr);
      }
    }
    injection_is_armed = true;
  }
  return result;
}

extern "C" int MPI_Alltoallv(void const* send_buffer,
                             int const* send_counts,
                             int const* send_displacements,
                             MPI_Datatype send_datatype,
                             void* receive_buffer,
                             int const* receive_counts,
                             int const* receive_displacements,
                             MPI_Datatype receive_datatype,
                             MPI_Comm communicator) {
  if (injection_is_armed && is_dense_capacity_mode()) {
    ++dense_payload_attempts;
    forbidden_failure_path_call("MPI_Alltoallv(dense payload)");
  }
  return PMPI_Alltoallv(send_buffer, send_counts, send_displacements,
                        send_datatype, receive_buffer, receive_counts,
                        receive_displacements, receive_datatype, communicator);
}

#if KAHIP_HAVE_MPI_ALLTOALLV_C
extern "C" int MPI_Alltoallv_c(void const* send_buffer,
                               MPI_Count const* send_counts,
                               MPI_Aint const* send_displacements,
                               MPI_Datatype send_datatype,
                               void* receive_buffer,
                               MPI_Count const* receive_counts,
                               MPI_Aint const* receive_displacements,
                               MPI_Datatype receive_datatype,
                               MPI_Comm communicator) {
  if (injection_is_armed && is_dense_capacity_mode()) {
    ++dense_payload_attempts;
    forbidden_failure_path_call("MPI_Alltoallv_c(dense payload)");
  }
  return PMPI_Alltoallv_c(send_buffer, send_counts, send_displacements,
                          send_datatype, receive_buffer, receive_counts,
                          receive_displacements, receive_datatype,
                          communicator);
}
#endif

extern "C" int MPI_Neighbor_alltoallv(void const* send_buffer,
                                      int const* send_counts,
                                      int const* send_displacements,
                                      MPI_Datatype send_datatype,
                                      void* receive_buffer,
                                      int const* receive_counts,
                                      int const* receive_displacements,
                                      MPI_Datatype receive_datatype,
                                      MPI_Comm communicator) {
  if (injection_is_armed && is_neighbor_capacity_mode()) {
    ++neighbor_payload_attempts;
    forbidden_failure_path_call("MPI_Neighbor_alltoallv(neighbor payload)");
  }
  return PMPI_Neighbor_alltoallv(send_buffer, send_counts, send_displacements,
                                 send_datatype, receive_buffer, receive_counts,
                                 receive_displacements, receive_datatype,
                                 communicator);
}

#if KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_C
extern "C" int MPI_Neighbor_alltoallv_c(void const* send_buffer,
                                        MPI_Count const* send_counts,
                                        MPI_Aint const* send_displacements,
                                        MPI_Datatype send_datatype,
                                        void* receive_buffer,
                                        MPI_Count const* receive_counts,
                                        MPI_Aint const* receive_displacements,
                                        MPI_Datatype receive_datatype,
                                        MPI_Comm communicator) {
  if (injection_is_armed && is_neighbor_capacity_mode()) {
    ++neighbor_payload_attempts;
    forbidden_failure_path_call("MPI_Neighbor_alltoallv_c(neighbor payload)");
  }
  return PMPI_Neighbor_alltoallv_c(send_buffer, send_counts, send_displacements,
                                   send_datatype, receive_buffer,
                                   receive_counts, receive_displacements,
                                   receive_datatype, communicator);
}
#endif

extern "C" int MPI_Get_address(void const* location, MPI_Aint* address) {
  if (injection_is_armed &&
      (is_dense_capacity_mode() || is_neighbor_receive_capacity_mode())) {
    record_forbidden_datatype_attempt("MPI_Get_address");
  }
  return PMPI_Get_address(location, address);
}

extern "C" int MPI_Type_create_struct(int count,
                                      int const block_lengths[],
                                      MPI_Aint const displacements[],
                                      MPI_Datatype const datatypes[],
                                      MPI_Datatype* new_datatype) {
  if (injection_is_armed &&
      (is_dense_capacity_mode() || is_neighbor_receive_capacity_mode())) {
    record_forbidden_datatype_attempt("MPI_Type_create_struct");
  }
  return PMPI_Type_create_struct(count, block_lengths, displacements, datatypes,
                                 new_datatype);
}

extern "C" int MPI_Type_create_resized(MPI_Datatype old_datatype,
                                       MPI_Aint lower_bound,
                                       MPI_Aint extent,
                                       MPI_Datatype* new_datatype) {
  if (injection_is_armed &&
      (is_dense_capacity_mode() || is_neighbor_receive_capacity_mode())) {
    record_forbidden_datatype_attempt("MPI_Type_create_resized");
  }
  return PMPI_Type_create_resized(old_datatype, lower_bound, extent,
                                  new_datatype);
}

extern "C" int MPI_Type_commit(MPI_Datatype* datatype) {
  if (injection_is_armed &&
      (is_dense_capacity_mode() || is_neighbor_receive_capacity_mode())) {
    record_forbidden_datatype_attempt("MPI_Type_commit");
  }
  return PMPI_Type_commit(datatype);
}

extern "C" int MPI_Type_free(MPI_Datatype* datatype) {
  if (injection_is_armed &&
      (is_dense_capacity_mode() || is_neighbor_receive_capacity_mode())) {
    record_forbidden_datatype_attempt("MPI_Type_free");
  }
  return PMPI_Type_free(datatype);
}

extern "C" int MPI_Comm_dup(MPI_Comm communicator,
                            MPI_Comm* duplicate_communicator) {
  auto const result = PMPI_Comm_dup(communicator, duplicate_communicator);
  if (result == MPI_SUCCESS && track_next_duplicate &&
      duplicate_communicator != nullptr) {
    tracked_communicator = *duplicate_communicator;
    track_next_duplicate = false;
    if (selected_mode == failure_mode::wrong_topology) {
      injection_is_armed = true;
      std::fputs("captured wrong-topology internal duplicate\n", stderr);
    } else if (is_dense_capacity_mode()) {
      std::fputs("captured dense operation duplicate\n", stderr);
    } else if (is_graph_capacity_mode()) {
      std::fputs("captured distributed-graph validation duplicate\n", stderr);
    } else if (is_neighbor_capacity_mode()) {
      std::fputs("captured neighbor operation duplicate\n", stderr);
    }
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

[[noreturn]] void run_null_communicator_failure() {
  std::fputs("injected null communicator construction\n", stderr);
  injection_is_armed = true;
  auto invalid =
      parhip::mpi::communicator{parhip::mpi::communicator_view{MPI_COMM_NULL}};
  static_cast<void>(invalid);
  returned_from_failure("null-communicator");
}

[[noreturn]] void run_intercommunicator_failure() {
  MPI_Comm local = MPI_COMM_NULL;
  if (PMPI_Comm_split(MPI_COMM_WORLD, cached_rank, 0, &local) != MPI_SUCCESS) {
    returned_from_failure("intercommunicator local split");
  }

  MPI_Comm intercommunicator = MPI_COMM_NULL;
  auto const remote_leader = cached_rank == 0 ? 1 : 0;
  if (PMPI_Intercomm_create(local, 0, MPI_COMM_WORLD, remote_leader, 71,
                            &intercommunicator) != MPI_SUCCESS) {
    returned_from_failure("intercommunicator creation");
  }
  if (PMPI_Comm_free(&local) != MPI_SUCCESS) {
    returned_from_failure("intercommunicator local cleanup");
  }

  tracked_communicator = intercommunicator;
  std::fputs("injected intercommunicator construction\n", stderr);
  injection_is_armed = true;
  auto invalid = parhip::mpi::communicator{
      parhip::mpi::communicator_view{intercommunicator}};
  static_cast<void>(invalid);
  returned_from_failure("intercommunicator");
}

[[noreturn]] void run_null_distributed_graph_failure() {
  std::fputs("injected null distributed graph construction\n", stderr);
  injection_is_armed = true;
  auto invalid = parhip::mpi::distributed_graph{
      parhip::mpi::communicator_view{MPI_COMM_NULL}, {}};
  static_cast<void>(invalid);
  returned_from_failure("null-distributed-graph");
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

[[noreturn]] void run_dense_receive_capacity_failure(char const* mode) {
  auto segments = std::vector<std::vector<failure_probe::dense_wire_record>>(
      static_cast<std::size_t>(cached_size));
  for (auto& segment : segments) {
    segment.push_back(failure_probe::dense_wire_record{
        .value = static_cast<std::uint64_t>(cached_rank + 1)});
  }
  auto sends = parhip::mpi::segmented_buffer<
      failure_probe::dense_wire_record>::from_segments(segments);
  track_next_duplicate = true;
  static_cast<void>(parhip::mpi::all_to_all_v(
      std::move(sends), parhip::mpi::communicator_view{MPI_COMM_WORLD}));
  returned_from_failure(mode);
}

[[noreturn]] void run_distributed_graph_degree_capacity_failure() {
  track_next_duplicate = true;
  auto graph = parhip::mpi::distributed_graph{
      parhip::mpi::communicator_view{MPI_COMM_WORLD}, {cached_rank}};
  static_cast<void>(graph);
  returned_from_failure("distributed-graph-degree-capacity");
}

[[noreturn]] void run_neighbor_capacity_failure(char const* mode) {
  auto graph = parhip::mpi::distributed_graph{
      parhip::mpi::communicator_view{MPI_COMM_WORLD}, {0, 1}};
  neighbor_graph_communicator = graph.native_handle();

  auto const values_per_destination =
      selected_mode == failure_mode::neighbor_bounded_round_arithmetic
          ? std::size_t{3}
          : std::size_t{1};
  auto segments = std::vector<std::vector<failure_probe::dense_wire_record>>(
      graph.destinations().size());
  for (auto& segment : segments) {
    segment.resize(values_per_destination,
                   failure_probe::dense_wire_record{
                       .value = static_cast<std::uint64_t>(cached_rank + 1)});
  }

  track_next_duplicate = true;
  auto const options =
      selected_mode == failure_mode::neighbor_bounded_round_arithmetic
          ? parhip::mpi::collective_options{
                .mpi3_round_ceiling = 2,
                .force_mpi3 = true,
            }
          : parhip::mpi::collective_options{};
  static_cast<void>(parhip::mpi::neighbor_all_to_all_v(
      parhip::mpi::segmented_buffer<
          failure_probe::dense_wire_record>::from_segments(segments),
      graph, options));
  returned_from_failure(mode);
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
  } else if (mode == "null-communicator") {
    selected_mode = failure_mode::null_communicator;
  } else if (mode == "intercommunicator") {
    selected_mode = failure_mode::intercommunicator;
  } else if (mode == "null-distributed-graph") {
    selected_mode = failure_mode::null_distributed_graph;
  } else if (mode == "wrong-topology") {
    selected_mode = failure_mode::wrong_topology;
  } else if (mode == "capacity-resolver") {
    selected_mode = failure_mode::capacity_resolver;
  } else if (mode == "dense-receive-offset-capacity") {
    selected_mode = failure_mode::dense_receive_offset_capacity;
  } else if (mode == "dense-receive-byte-capacity") {
    selected_mode = failure_mode::dense_receive_byte_capacity;
  } else if (mode == "distributed-graph-degree-capacity") {
    selected_mode = failure_mode::distributed_graph_degree_capacity;
  } else if (mode == "neighbor-receive-offset-capacity") {
    selected_mode = failure_mode::neighbor_receive_offset_capacity;
  } else if (mode == "neighbor-receive-byte-capacity") {
    selected_mode = failure_mode::neighbor_receive_byte_capacity;
  } else if (mode == "neighbor-bounded-round-arithmetic") {
    selected_mode = failure_mode::neighbor_bounded_round_arithmetic;
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
  if (PMPI_Comm_size(MPI_COMM_WORLD, &cached_size) != MPI_SUCCESS) {
    std::fputs("PMPI_Comm_size failed before failure injection\n", stderr);
    return 70;
  }

  switch (selected_mode) {
    case failure_mode::semantic_factory_resource:
      run_semantic_factory_resource_failure();
    case failure_mode::error_string_secondary:
      run_error_string_secondary_failure();
    case failure_mode::null_communicator:
      run_null_communicator_failure();
    case failure_mode::intercommunicator:
      run_intercommunicator_failure();
    case failure_mode::null_distributed_graph:
      run_null_distributed_graph_failure();
    case failure_mode::wrong_topology:
      run_wrong_topology_failure();
    case failure_mode::capacity_resolver:
      run_capacity_resolver_failure();
    case failure_mode::dense_receive_offset_capacity:
      run_dense_receive_capacity_failure("dense-receive-offset-capacity");
    case failure_mode::dense_receive_byte_capacity:
      run_dense_receive_capacity_failure("dense-receive-byte-capacity");
    case failure_mode::distributed_graph_degree_capacity:
      run_distributed_graph_degree_capacity_failure();
    case failure_mode::neighbor_receive_offset_capacity:
      run_neighbor_capacity_failure("neighbor-receive-offset-capacity");
    case failure_mode::neighbor_receive_byte_capacity:
      run_neighbor_capacity_failure("neighbor-receive-byte-capacity");
    case failure_mode::neighbor_bounded_round_arithmetic:
      run_neighbor_capacity_failure("neighbor-bounded-round-arithmetic");
    case failure_mode::pre_init_error:
      break;
  }
  returned_from_failure("unknown-active-mode");
}
