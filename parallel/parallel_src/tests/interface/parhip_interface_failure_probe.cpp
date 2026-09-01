#include <mpi.h>
#include <unistd.h>

#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string_view>
#include <vector>

#include "kahip_mpi_capabilities.h"
#include "parhip_interface.h"

// KAHIP_PMPI_CALLBACK_REGION_BEGIN
namespace parhip_interface_failure_probe {
enum class mode : unsigned char {
  backend_reduction,
  null_distribution,
  zero_k,
  mismatched_k,
  invalid_imbalance,
  mismatched_distribution,
  invalid_offsets,
  missing_adjacency,
  invalid_neighbor,
  mismatched_vertex_weights,
  missing_partition,
  invalid_mode,
  global_weight_overflow,
  imbalanced_result,
  intercommunicator,
};

inline bool active = false;
inline mode selected = mode::backend_reduction;
inline MPI_Comm caller_communicator = MPI_COMM_NULL;
inline MPI_Comm owned_communicator = MPI_COMM_NULL;
inline int duplications = 0;
inline int error_handler_sets = 0;
inline int owned_queries = 0;
inline int validation_reductions = 0;
inline int allgathers = 0;
inline int finalizations = 0;
inline bool callback_error = false;

void write_text(std::string_view text) noexcept {
  static_cast<void>(::write(STDERR_FILENO, text.data(), text.size()));
}

[[nodiscard]] auto expects_duplicate() noexcept -> bool {
  return selected != mode::intercommunicator;
}

[[nodiscard]] auto exercises_full_algorithm() noexcept -> bool {
  return selected == mode::imbalanced_result;
}

[[nodiscard]] auto expected_abort_state() noexcept -> bool {
  if (callback_error || finalizations != 0) {
    return false;
  }
  if (!expects_duplicate()) {
    return duplications == 0 && error_handler_sets == 0 && owned_queries == 1 &&
           validation_reductions == 0;
  }
  if (exercises_full_algorithm()) {
    return duplications >= 1 && error_handler_sets >= 1 && owned_queries >= 2 &&
           owned_communicator != MPI_COMM_NULL && validation_reductions >= 1;
  }
  auto const avoided_rank_count_storage =
      selected != mode::global_weight_overflow || allgathers == 0;
  return duplications == 1 && error_handler_sets == 1 &&
         owned_communicator != MPI_COMM_NULL && validation_reductions >= 1 &&
         avoided_rank_count_storage;
}

[[noreturn]] void observed_abort(MPI_Comm communicator,
                                 int error_code) noexcept {
  auto const expected =
      expects_duplicate() ? owned_communicator : caller_communicator;
  auto relation = int{MPI_UNEQUAL};
  if (error_code != EXIT_FAILURE || expected == MPI_COMM_NULL ||
      PMPI_Comm_compare(communicator, expected, &relation) != MPI_SUCCESS ||
      relation != MPI_IDENT || !expected_abort_state()) {
    write_text("observed ParHIP interface MPI_Abort with unexpected state\n");
    std::_Exit(91);
  }
  write_text("observed ParHIP interface MPI_Abort on affected communicator\n");
  if (PMPI_Barrier(MPI_COMM_WORLD) != MPI_SUCCESS) {
    std::_Exit(90);
  }
  std::_Exit(86);
}
}  // namespace parhip_interface_failure_probe

static_assert(noexcept(parhip_interface_failure_probe::write_text({})));
static_assert(noexcept(parhip_interface_failure_probe::expects_duplicate()));
static_assert(
    noexcept(parhip_interface_failure_probe::exercises_full_algorithm()));
static_assert(noexcept(parhip_interface_failure_probe::expected_abort_state()));
static_assert(
    noexcept(parhip_interface_failure_probe::observed_abort(MPI_COMM_NULL, 0)));

extern "C" int MPI_Comm_dup(MPI_Comm communicator, MPI_Comm* duplicate) {
  using namespace parhip_interface_failure_probe;
  if (!active) {
    return PMPI_Comm_dup(communicator, duplicate);
  }
  ++duplications;
  auto const first_duplicate = duplications == 1;
  auto const valid_source = first_duplicate
                                ? communicator == caller_communicator
                                : exercises_full_algorithm() &&
                                      communicator != caller_communicator &&
                                      communicator != MPI_COMM_NULL;
  if (!expects_duplicate() || !valid_source || duplicate == nullptr) {
    callback_error = true;
    return MPI_ERR_OTHER;
  }
  auto const result = PMPI_Comm_dup(communicator, duplicate);
  if (result == MPI_SUCCESS && first_duplicate) {
    owned_communicator = *duplicate;
  }
  return result;
}

extern "C" int MPI_Comm_set_errhandler(MPI_Comm communicator,
                                       MPI_Errhandler error_handler) {
  using namespace parhip_interface_failure_probe;
  if (!active) {
    return PMPI_Comm_set_errhandler(communicator, error_handler);
  }
  ++error_handler_sets;
  auto const valid_communicator =
      communicator == owned_communicator ||
      (exercises_full_algorithm() && communicator != caller_communicator &&
       communicator != MPI_COMM_NULL);
  if (!valid_communicator || error_handler != MPI_ERRORS_RETURN) {
    callback_error = true;
    return MPI_ERR_OTHER;
  }
  return PMPI_Comm_set_errhandler(communicator, error_handler);
}

extern "C" int MPI_Comm_rank(MPI_Comm communicator, int* rank) {
  using namespace parhip_interface_failure_probe;
  if (active) {
    ++owned_queries;
    auto const diagnostic_world_query =
        selected == mode::backend_reduction && communicator == MPI_COMM_WORLD;
    auto const valid_communicator =
        diagnostic_world_query ||
        (!expects_duplicate() && communicator == caller_communicator) ||
        communicator == owned_communicator ||
        (exercises_full_algorithm() && communicator != caller_communicator &&
         communicator != MPI_COMM_NULL);
    auto const has_expected_communicator =
        !expects_duplicate() || owned_communicator != MPI_COMM_NULL;
    if (!has_expected_communicator || !valid_communicator) {
      callback_error = true;
      return MPI_ERR_OTHER;
    }
  }
  return PMPI_Comm_rank(communicator, rank);
}

extern "C" int MPI_Comm_size(MPI_Comm communicator, int* size) {
  using namespace parhip_interface_failure_probe;
  if (active) {
    ++owned_queries;
    auto const valid_communicator =
        communicator == owned_communicator ||
        (exercises_full_algorithm() && communicator != caller_communicator &&
         communicator != MPI_COMM_NULL);
    if (owned_communicator == MPI_COMM_NULL || !valid_communicator) {
      callback_error = true;
      return MPI_ERR_OTHER;
    }
  }
  return PMPI_Comm_size(communicator, size);
}

extern "C" int MPI_Allgather(void const* send_buffer,
                             int send_count,
                             MPI_Datatype send_datatype,
                             void* receive_buffer,
                             int receive_count,
                             MPI_Datatype receive_datatype,
                             MPI_Comm communicator) {
  using namespace parhip_interface_failure_probe;
  if (active) {
    ++allgathers;
  }
  return PMPI_Allgather(send_buffer, send_count, send_datatype, receive_buffer,
                        receive_count, receive_datatype, communicator);
}

extern "C" int MPI_Allreduce(void const* send_buffer,
                             void* receive_buffer,
                             int count,
                             MPI_Datatype datatype,
                             MPI_Op operation,
                             MPI_Comm communicator) {
  using namespace parhip_interface_failure_probe;
  if (!active) {
    return PMPI_Allreduce(send_buffer, receive_buffer, count, datatype,
                          operation, communicator);
  }
  ++validation_reductions;
  auto const valid_communicator =
      communicator == owned_communicator ||
      (exercises_full_algorithm() && communicator != caller_communicator &&
       communicator != MPI_COMM_NULL);
  if (owned_communicator == MPI_COMM_NULL || !valid_communicator ||
      send_buffer == nullptr || receive_buffer == nullptr ||
      send_buffer == receive_buffer || count <= 0 ||
      (operation != MPI_MIN && operation != MPI_MAX && operation != MPI_BOR &&
       operation != MPI_SUM && operation != MPI_BAND)) {
    callback_error = true;
    return MPI_ERR_OTHER;
  }
  if (selected == mode::backend_reduction && validation_reductions == 1) {
    return MPI_ERR_OTHER;
  }
  return PMPI_Allreduce(send_buffer, receive_buffer, count, datatype, operation,
                        communicator);
}

#if KAHIP_HAVE_MPI_ALLREDUCE_C
extern "C" int MPI_Allreduce_c(void const* send_buffer,
                               void* receive_buffer,
                               MPI_Count count,
                               MPI_Datatype datatype,
                               MPI_Op operation,
                               MPI_Comm communicator) {
  using namespace parhip_interface_failure_probe;
  if (!active) {
    return PMPI_Allreduce_c(send_buffer, receive_buffer, count, datatype,
                            operation, communicator);
  }
  ++validation_reductions;
  auto const valid_communicator =
      communicator == owned_communicator ||
      (exercises_full_algorithm() && communicator != caller_communicator &&
       communicator != MPI_COMM_NULL);
  if (owned_communicator == MPI_COMM_NULL || !valid_communicator ||
      send_buffer == nullptr || receive_buffer == nullptr ||
      send_buffer == receive_buffer || count <= 0 ||
      (operation != MPI_MIN && operation != MPI_MAX && operation != MPI_BOR &&
       operation != MPI_SUM && operation != MPI_BAND)) {
    callback_error = true;
    return MPI_ERR_OTHER;
  }
  if (selected == mode::backend_reduction && validation_reductions == 1) {
    return MPI_ERR_OTHER;
  }
  return PMPI_Allreduce_c(send_buffer, receive_buffer, count, datatype,
                          operation, communicator);
}
#endif

extern "C" int MPI_Finalize() {
  if (parhip_interface_failure_probe::active) {
    ++parhip_interface_failure_probe::finalizations;
    return MPI_SUCCESS;
  }
  return PMPI_Finalize();
}

extern "C" int MPI_Abort(MPI_Comm communicator, int error_code) {
  parhip_interface_failure_probe::observed_abort(communicator, error_code);
}
// KAHIP_PMPI_CALLBACK_REGION_END

namespace {
[[nodiscard]] auto parse_mode(std::string_view value)
    -> parhip_interface_failure_probe::mode {
  using mode = parhip_interface_failure_probe::mode;
  if (value == "backend-reduction")
    return mode::backend_reduction;
  if (value == "null-distribution")
    return mode::null_distribution;
  if (value == "zero-k")
    return mode::zero_k;
  if (value == "mismatched-k")
    return mode::mismatched_k;
  if (value == "invalid-imbalance")
    return mode::invalid_imbalance;
  if (value == "mismatched-distribution")
    return mode::mismatched_distribution;
  if (value == "invalid-offsets")
    return mode::invalid_offsets;
  if (value == "missing-adjacency")
    return mode::missing_adjacency;
  if (value == "invalid-neighbor")
    return mode::invalid_neighbor;
  if (value == "mismatched-vertex-weights") {
    return mode::mismatched_vertex_weights;
  }
  if (value == "missing-partition")
    return mode::missing_partition;
  if (value == "invalid-mode")
    return mode::invalid_mode;
  if (value == "global-weight-overflow")
    return mode::global_weight_overflow;
  if (value == "imbalanced-result")
    return mode::imbalanced_result;
  if (value == "intercommunicator")
    return mode::intercommunicator;
  parhip_interface_failure_probe::write_text(
      "unknown ParHIP interface probe mode\n");
  std::_Exit(2);
}

[[nodiscard]] auto make_intercommunicator(int world_rank) -> MPI_Comm {
  auto local = MPI_COMM_NULL;
  if (PMPI_Comm_split(MPI_COMM_WORLD, world_rank, 0, &local) != MPI_SUCCESS ||
      local == MPI_COMM_NULL) {
    std::_Exit(7);
  }
  auto intercommunicator = MPI_COMM_NULL;
  if (PMPI_Intercomm_create(local, 0, MPI_COMM_WORLD, 1 - world_rank, 947,
                            &intercommunicator) != MPI_SUCCESS ||
      intercommunicator == MPI_COMM_NULL) {
    std::_Exit(8);
  }
  if (PMPI_Comm_free(&local) != MPI_SUCCESS) {
    std::_Exit(9);
  }
  return intercommunicator;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 2 || MPI_Init(&argc, &argv) != MPI_SUCCESS) {
    return 2;
  }
  auto world_rank = -1;
  auto world_size = 0;
  if (MPI_Comm_rank(MPI_COMM_WORLD, &world_rank) != MPI_SUCCESS ||
      MPI_Comm_size(MPI_COMM_WORLD, &world_size) != MPI_SUCCESS ||
      world_size != 2) {
    return 3;
  }

  using mode = parhip_interface_failure_probe::mode;
  auto const selected = parse_mode(argv[1]);
  auto communicator = MPI_COMM_NULL;
  if (selected == mode::intercommunicator) {
    communicator = make_intercommunicator(world_rank);
  } else if (MPI_Comm_split(MPI_COMM_WORLD, 0, world_size - world_rank,
                            &communicator) != MPI_SUCCESS ||
             communicator == MPI_COMM_NULL) {
    return 4;
  }
  if (MPI_Comm_set_errhandler(communicator, MPI_ERRORS_RETURN) != MPI_SUCCESS) {
    return 5;
  }

  auto rank = -1;
  auto size = 0;
  if (selected != mode::intercommunicator &&
      (MPI_Comm_rank(communicator, &rank) != MPI_SUCCESS ||
       MPI_Comm_size(communicator, &size) != MPI_SUCCESS || size != 2)) {
    return 6;
  }

  auto distribution = std::array<idxtype, 3>{0, 2, 4};
  auto offsets = std::array<idxtype, 3>{0, 2, 4};
  auto neighbors = rank == 0 ? std::array<idxtype, 4>{1, 3, 0, 2}
                             : std::array<idxtype, 4>{1, 3, 0, 2};
  auto vertex_weights = std::array<idxtype, 2>{1, 1};
  auto partition = std::array<idxtype, 2>{};
  auto blocks = 2;
  auto imbalance = 0.03;
  auto edge_cut = -1;

  if (selected == mode::global_weight_overflow) {
    distribution = {0, 1, 2};
    offsets = {0, 0, 0};
    vertex_weights = {
        std::numeric_limits<idxtype>::max() / 2 + 1,
        std::numeric_limits<idxtype>::max() / 2 + 1,
    };
    blocks = 1;
  }

  if (selected == mode::mismatched_k)
    blocks = rank + 2;
  if (selected == mode::zero_k)
    blocks = 0;
  if (selected == mode::invalid_imbalance) {
    imbalance = std::numeric_limits<double>::infinity();
  }
  if (selected == mode::mismatched_distribution && rank == 1) {
    distribution[1] = 1;
  }
  if (selected == mode::invalid_offsets && rank == 0) {
    offsets = {0, 3, 2};
  }
  if (selected == mode::invalid_neighbor && rank == 0) {
    neighbors[0] = 4;
  }

  auto* distribution_pointer = distribution.data();
  auto* adjacency_pointer = neighbors.data();
  auto* vertex_weight_pointer = static_cast<idxtype*>(nullptr);
  auto* partition_pointer = partition.data();
  if (selected == mode::null_distribution && rank == 0) {
    distribution_pointer = nullptr;
  }
  if (selected == mode::missing_adjacency && rank == 0) {
    adjacency_pointer = nullptr;
  }
  if (selected == mode::mismatched_vertex_weights && rank == 0) {
    vertex_weight_pointer = vertex_weights.data();
  }
  if (selected == mode::missing_partition && rank == 0) {
    partition_pointer = nullptr;
  }
  if (selected == mode::global_weight_overflow) {
    adjacency_pointer = nullptr;
    vertex_weight_pointer = vertex_weights.data();
  }
  if (selected == mode::imbalanced_result) {
    // Make the requested two-block balance mathematically infeasible: the
    // weight-10 vertex exceeds the exact upper bound for total weight 13, no
    // matter which block the partitioner selects.  The postcondition check
    // must therefore reject every possible partition deterministically.
    if (rank == 0) {
      vertex_weights.front() = 10;
    }
    vertex_weight_pointer = vertex_weights.data();
  }

  parhip_interface_failure_probe::selected = selected;
  parhip_interface_failure_probe::caller_communicator = communicator;
  parhip_interface_failure_probe::active = true;
  auto const partition_mode = selected == mode::invalid_mode ? 947 : FASTMESH;
  ParHIPPartitionKWay(distribution_pointer, offsets.data(), adjacency_pointer,
                      vertex_weight_pointer, nullptr, &blocks, &imbalance, true,
                      1, partition_mode, &edge_cut, partition_pointer,
                      &communicator);

  parhip_interface_failure_probe::write_text(
      "ParHIP interface operation returned without fail-fast\n");
  std::_Exit(92);
}
