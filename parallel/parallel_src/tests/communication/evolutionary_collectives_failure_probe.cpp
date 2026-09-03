#include <mpi.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string_view>
#include <type_traits>

#include "parallel_mh/evolutionary_collectives.h"

// KAHIP_PMPI_CALLBACK_REGION_BEGIN
namespace evolutionary_failure_probe {
enum class mode : unsigned char {
  permutation,
  rank,
  communicator_size,
  feasibility,
  objective,
  weight,
  root,
  signature_minimum,
  signature_maximum,
  signature_validity,
  payload,
  count_mismatch,
  null_payload,
};

inline bool active = false;
inline mode selected = mode::objective;
inline MPI_Comm expected_communicator = MPI_COMM_NULL;
inline int all_reductions = 0;
inline int broadcasts = 0;
inline int rank_queries = 0;
inline int size_queries = 0;
inline bool invalid_mpi_call = false;
inline bool forbidden_communication = false;

[[nodiscard]] auto objective_datatype() noexcept -> MPI_Datatype {
  if constexpr (std::is_same_v<std::int64_t, long>) {
    return MPI_LONG;
  } else {
    static_assert(std::is_same_v<std::int64_t, long> ||
                  std::is_same_v<std::int64_t, long long>);
    return MPI_LONG_LONG_INT;
  }
}

[[nodiscard]] auto valid_allreduce(int index,
                                   void const* send_buffer,
                                   void* receive_buffer,
                                   int count,
                                   MPI_Datatype datatype,
                                   MPI_Op reduction,
                                   MPI_Comm communicator) noexcept -> bool {
  if (communicator != expected_communicator || send_buffer == nullptr ||
      receive_buffer == nullptr) {
    return false;
  }
  switch (index) {
    case 1:
      return count == 1 && datatype == MPI_INT && reduction == MPI_MIN;
    case 2:
      return count == 1 && datatype == objective_datatype() &&
             reduction == MPI_MIN;
    case 3:
      return count == 1 && datatype == MPI_UNSIGNED && reduction == MPI_MIN;
    case 4:
    case 7:
      return count == 1 && datatype == MPI_INT && reduction == MPI_MIN;
    case 5:
      return count == 4 && datatype == MPI_UINT64_T && reduction == MPI_MIN;
    case 6:
      return count == 4 && datatype == MPI_UINT64_T && reduction == MPI_MAX;
    default:
      return false;
  }
}

[[nodiscard]] auto expected_abort_state() noexcept -> bool {
  if (invalid_mpi_call || forbidden_communication) {
    return false;
  }
  switch (selected) {
    case mode::permutation:
      return rank_queries == 0 && size_queries == 0 && all_reductions == 0 &&
             broadcasts == 1;
    case mode::rank:
      return rank_queries == 1 && size_queries == 0 && all_reductions == 0 &&
             broadcasts == 0;
    case mode::feasibility:
      return rank_queries == 1 && size_queries == 0 && all_reductions == 1 &&
             broadcasts == 0;
    case mode::objective:
      return rank_queries == 1 && size_queries == 0 && all_reductions == 2 &&
             broadcasts == 0;
    case mode::weight:
      return rank_queries == 1 && size_queries == 0 && all_reductions == 3 &&
             broadcasts == 0;
    case mode::root:
      return rank_queries == 1 && size_queries == 0 && all_reductions == 4 &&
             broadcasts == 0;
    case mode::communicator_size:
      return rank_queries == 1 && size_queries == 1 && all_reductions == 4 &&
             broadcasts == 0;
    case mode::signature_minimum:
      return rank_queries == 1 && size_queries == 1 && all_reductions == 5 &&
             broadcasts == 0;
    case mode::signature_maximum:
      return rank_queries == 1 && size_queries == 1 && all_reductions == 6 &&
             broadcasts == 0;
    case mode::signature_validity:
      return rank_queries == 1 && size_queries == 1 && all_reductions == 7 &&
             broadcasts == 0;
    case mode::payload:
      return rank_queries == 1 && size_queries == 1 && all_reductions == 7 &&
             broadcasts == 1;
    case mode::count_mismatch:
    case mode::null_payload:
      return rank_queries == 1 && size_queries == 1 && all_reductions == 7 &&
             broadcasts == 0;
  }
  return false;
}

void write_text(std::string_view text) noexcept {
  static_cast<void>(::write(STDERR_FILENO, text.data(), text.size()));
}

[[noreturn]] void observed_abort(MPI_Comm communicator,
                                 int error_code) noexcept {
  int relation = MPI_UNEQUAL;
  if (error_code != EXIT_FAILURE ||
      PMPI_Comm_compare(communicator, expected_communicator, &relation) !=
          MPI_SUCCESS ||
      relation != MPI_IDENT || !expected_abort_state()) {
    write_text("observed evolutionary MPI_Abort with unexpected state\n");
    std::_Exit(91);
  }
  write_text("observed evolutionary MPI_Abort on affected communicator\n");
  std::_Exit(86);
}
}  // namespace evolutionary_failure_probe

static_assert(noexcept(evolutionary_failure_probe::write_text({})));
static_assert(noexcept(evolutionary_failure_probe::observed_abort(MPI_COMM_NULL,
                                                                  0)));
static_assert(noexcept(evolutionary_failure_probe::objective_datatype()));
static_assert(
    noexcept(evolutionary_failure_probe::valid_allreduce(0,
                                                         nullptr,
                                                         nullptr,
                                                         0,
                                                         MPI_DATATYPE_NULL,
                                                         MPI_OP_NULL,
                                                         MPI_COMM_NULL)));
static_assert(noexcept(evolutionary_failure_probe::expected_abort_state()));

extern "C" int MPI_Comm_rank(MPI_Comm communicator, int* rank) {
  if (evolutionary_failure_probe::active) {
    ++evolutionary_failure_probe::rank_queries;
    if (communicator != evolutionary_failure_probe::expected_communicator ||
        rank == nullptr) {
      evolutionary_failure_probe::invalid_mpi_call = true;
    }
    if (evolutionary_failure_probe::selected ==
        evolutionary_failure_probe::mode::rank) {
      return MPI_ERR_OTHER;
    }
  }
  return PMPI_Comm_rank(communicator, rank);
}

extern "C" int MPI_Allreduce(void const* send_buffer,
                             void* receive_buffer,
                             int count,
                             MPI_Datatype datatype,
                             MPI_Op reduction,
                             MPI_Comm communicator) {
  if (evolutionary_failure_probe::active) {
    auto const index = ++evolutionary_failure_probe::all_reductions;
    if (!evolutionary_failure_probe::valid_allreduce(
            index, send_buffer, receive_buffer, count, datatype, reduction,
            communicator)) {
      evolutionary_failure_probe::invalid_mpi_call = true;
      return MPI_ERR_OTHER;
    }
    auto const selected = evolutionary_failure_probe::selected;
    if ((selected == evolutionary_failure_probe::mode::feasibility &&
         index == 1) ||
        (selected == evolutionary_failure_probe::mode::objective &&
         index == 2) ||
        (selected == evolutionary_failure_probe::mode::weight && index == 3) ||
        (selected == evolutionary_failure_probe::mode::root && index == 4) ||
        (selected == evolutionary_failure_probe::mode::signature_minimum &&
         index == 5) ||
        (selected == evolutionary_failure_probe::mode::signature_maximum &&
         index == 6) ||
        (selected == evolutionary_failure_probe::mode::signature_validity &&
         index == 7)) {
      return MPI_ERR_OTHER;
    }
  }
  return PMPI_Allreduce(send_buffer, receive_buffer, count, datatype, reduction,
                        communicator);
}

extern "C" int MPI_Bcast(void* buffer,
                         int count,
                         MPI_Datatype datatype,
                         int root,
                         MPI_Comm communicator) {
  if (evolutionary_failure_probe::active) {
    ++evolutionary_failure_probe::broadcasts;
    auto const selected = evolutionary_failure_probe::selected;
    auto const expected_payload =
        selected == evolutionary_failure_probe::mode::permutation ||
        selected == evolutionary_failure_probe::mode::payload;
    if (!expected_payload || count != 1 || datatype != MPI_UNSIGNED ||
        root != 0 ||
        communicator != evolutionary_failure_probe::expected_communicator ||
        buffer == nullptr) {
      evolutionary_failure_probe::invalid_mpi_call = true;
      return MPI_ERR_OTHER;
    }
    if (selected == evolutionary_failure_probe::mode::permutation ||
        selected == evolutionary_failure_probe::mode::payload) {
      return MPI_ERR_OTHER;
    }
  }
  return PMPI_Bcast(buffer, count, datatype, root, communicator);
}

#if KAHIP_HAVE_MPI_BCAST_C
extern "C" int MPI_Bcast_c(void* buffer,
                           MPI_Count count,
                           MPI_Datatype datatype,
                           int root,
                           MPI_Comm communicator) {
  if (evolutionary_failure_probe::active) {
    ++evolutionary_failure_probe::broadcasts;
    if (evolutionary_failure_probe::selected !=
            evolutionary_failure_probe::mode::payload ||
        count != 1 || datatype != MPI_UNSIGNED || root != 0 ||
        communicator != evolutionary_failure_probe::expected_communicator ||
        buffer == nullptr) {
      evolutionary_failure_probe::invalid_mpi_call = true;
    }
    return MPI_ERR_OTHER;
  }
  return PMPI_Bcast_c(buffer, count, datatype, root, communicator);
}
#endif

extern "C" int MPI_Comm_size(MPI_Comm communicator, int* size) {
  if (evolutionary_failure_probe::active) {
    ++evolutionary_failure_probe::size_queries;
    if (communicator != evolutionary_failure_probe::expected_communicator ||
        size == nullptr) {
      evolutionary_failure_probe::invalid_mpi_call = true;
    }
    if (evolutionary_failure_probe::selected ==
        evolutionary_failure_probe::mode::communicator_size) {
      return MPI_ERR_OTHER;
    }
  }
  return PMPI_Comm_size(communicator, size);
}

extern "C" int MPI_Send(void const* buffer,
                        int count,
                        MPI_Datatype datatype,
                        int destination,
                        int tag,
                        MPI_Comm communicator) {
  if (evolutionary_failure_probe::active) {
    evolutionary_failure_probe::forbidden_communication = true;
    return MPI_ERR_OTHER;
  }
  return PMPI_Send(buffer, count, datatype, destination, tag, communicator);
}

extern "C" int MPI_Isend(void const* buffer,
                         int count,
                         MPI_Datatype datatype,
                         int destination,
                         int tag,
                         MPI_Comm communicator,
                         MPI_Request* request) {
  if (evolutionary_failure_probe::active) {
    evolutionary_failure_probe::forbidden_communication = true;
    if (request != nullptr) {
      *request = MPI_REQUEST_NULL;
    }
    return MPI_ERR_OTHER;
  }
  return PMPI_Isend(buffer, count, datatype, destination, tag, communicator,
                    request);
}

extern "C" int MPI_Recv(void* buffer,
                        int count,
                        MPI_Datatype datatype,
                        int source,
                        int tag,
                        MPI_Comm communicator,
                        MPI_Status* status) {
  if (evolutionary_failure_probe::active) {
    evolutionary_failure_probe::forbidden_communication = true;
    return MPI_ERR_OTHER;
  }
  return PMPI_Recv(buffer, count, datatype, source, tag, communicator, status);
}

extern "C" int MPI_Irecv(void* buffer,
                         int count,
                         MPI_Datatype datatype,
                         int source,
                         int tag,
                         MPI_Comm communicator,
                         MPI_Request* request) {
  if (evolutionary_failure_probe::active) {
    evolutionary_failure_probe::forbidden_communication = true;
    if (request != nullptr) {
      *request = MPI_REQUEST_NULL;
    }
    return MPI_ERR_OTHER;
  }
  return PMPI_Irecv(buffer, count, datatype, source, tag, communicator,
                    request);
}

extern "C" int MPI_Sendrecv(void const* send_buffer,
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
                            MPI_Status* status) {
  if (evolutionary_failure_probe::active) {
    evolutionary_failure_probe::forbidden_communication = true;
    return MPI_ERR_OTHER;
  }
  return PMPI_Sendrecv(send_buffer, send_count, send_datatype, destination,
                       send_tag, receive_buffer, receive_count,
                       receive_datatype, source, receive_tag, communicator,
                       status);
}

extern "C" int MPI_Barrier(MPI_Comm communicator) {
  if (evolutionary_failure_probe::active) {
    evolutionary_failure_probe::forbidden_communication = true;
    return MPI_ERR_OTHER;
  }
  return PMPI_Barrier(communicator);
}

extern "C" int MPI_Abort(MPI_Comm communicator, int error_code) {
  evolutionary_failure_probe::observed_abort(communicator, error_code);
}
// KAHIP_PMPI_CALLBACK_REGION_END

namespace {
auto parse_mode(std::string_view value) -> evolutionary_failure_probe::mode {
  using mode = evolutionary_failure_probe::mode;
  if (value == "permutation")
    return mode::permutation;
  if (value == "rank")
    return mode::rank;
  if (value == "communicator-size")
    return mode::communicator_size;
  if (value == "feasibility")
    return mode::feasibility;
  if (value == "objective")
    return mode::objective;
  if (value == "weight")
    return mode::weight;
  if (value == "root")
    return mode::root;
  if (value == "signature-minimum")
    return mode::signature_minimum;
  if (value == "signature-maximum")
    return mode::signature_maximum;
  if (value == "signature-validity")
    return mode::signature_validity;
  if (value == "payload")
    return mode::payload;
  if (value == "count-mismatch")
    return mode::count_mismatch;
  if (value == "null-payload")
    return mode::null_payload;
  std::_Exit(4);
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 2 || MPI_Init(&argc, &argv) != MPI_SUCCESS) {
    return 2;
  }
  auto communicator = MPI_COMM_NULL;
  if (PMPI_Comm_dup(MPI_COMM_WORLD, &communicator) != MPI_SUCCESS ||
      communicator == MPI_COMM_NULL ||
      PMPI_Comm_set_errhandler(communicator, MPI_ERRORS_RETURN) !=
          MPI_SUCCESS) {
    return 3;
  }

  evolutionary_failure_probe::selected = parse_mode(argv[1]);
  evolutionary_failure_probe::expected_communicator = communicator;
  evolutionary_failure_probe::active = true;

  using mode = evolutionary_failure_probe::mode;
  if (evolutionary_failure_probe::selected == mode::permutation) {
    auto permutation = std::array<unsigned, 1>{0};
    kahip::parallel_mh::broadcast_permutation(communicator, permutation, 0);
  }

  constexpr auto objective =
      static_cast<std::int64_t>(std::numeric_limits<int>::max()) + 4096;
  constexpr auto weight =
      static_cast<unsigned>(std::numeric_limits<int>::max()) + 1024U;
  auto rank = -1;
  if (PMPI_Comm_rank(communicator, &rank) != MPI_SUCCESS) {
    return 5;
  }
  auto partition = std::array<unsigned, 2>{weight, weight + 1U};
  auto const count =
      evolutionary_failure_probe::selected == mode::count_mismatch && rank != 0
          ? std::size_t{2}
          : std::size_t{1};
  auto* partition_data =
      evolutionary_failure_probe::selected == mode::null_payload
          ? static_cast<unsigned*>(nullptr)
          : partition.data();
  static_cast<void>(kahip::parallel_mh::select_and_broadcast_best_partition(
      communicator, objective, weight, weight - 1U, partition_data, count));

  evolutionary_failure_probe::write_text(
      "evolutionary collective returned without fail-fast\n");
  std::_Exit(92);
}
