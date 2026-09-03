#include <mpi.h>
#include <unistd.h>

#include <cstddef>
#include <cstdlib>
#include <string>
#include <string_view>

#include "communication/mpi_trace.h"

namespace {
enum class failure_mode {
  rank,
  allreduce,
};

auto mode = failure_mode::rank;
auto active = false;
auto injected_calls = 0;
auto finalize_calls = 0;
auto cached_rank = -1;

constexpr auto injected_error = 17411;

void write_text(std::string_view value) noexcept {
  static_cast<void>(::write(STDERR_FILENO, value.data(), value.size()));
}

[[noreturn]] void unexpected_abort() noexcept {
  write_text("observed trace MPI_Abort with unexpected state\n");
  std::_Exit(91);
}
}  // namespace

extern "C" int MPI_Comm_rank(MPI_Comm communicator, int* rank) {
  if (active && mode == failure_mode::rank && injected_calls == 0) {
    ++injected_calls;
    write_text(cached_rank == 0
                   ? "injected trace MPI_Comm_rank failure rank=0\n"
                   : "injected trace MPI_Comm_rank failure rank=1\n");
    return injected_error;
  }
  return PMPI_Comm_rank(communicator, rank);
}

extern "C" int MPI_Allreduce(void const* send_buffer,
                              void* receive_buffer,
                              int count,
                              MPI_Datatype datatype,
                              MPI_Op operation,
                              MPI_Comm communicator) {
  if (active && mode == failure_mode::allreduce && injected_calls == 0) {
    ++injected_calls;
    write_text(cached_rank == 0
                   ? "injected trace MPI_Allreduce failure rank=0\n"
                   : "injected trace MPI_Allreduce failure rank=1\n");
    return injected_error;
  }
  return PMPI_Allreduce(send_buffer, receive_buffer, count, datatype,
                        operation, communicator);
}

extern "C" int MPI_Finalize() {
  if (active) {
    ++finalize_calls;
  }
  return PMPI_Finalize();
}

extern "C" int MPI_Error_string(int error_code,
                                char* error_text,
                                int* error_text_length) {
  if (active && error_code == injected_error) {
    constexpr char injected_text[] = "injected trace backend failure";
    static_assert(sizeof(injected_text) <= MPI_MAX_ERROR_STRING);
    if (error_text == nullptr || error_text_length == nullptr) {
      return MPI_ERR_ARG;
    }
    for (auto index = std::size_t{0}; index < sizeof(injected_text); ++index) {
      error_text[index] = injected_text[index];
    }
    *error_text_length = static_cast<int>(sizeof(injected_text) - 1);
    return MPI_SUCCESS;
  }
  return PMPI_Error_string(error_code, error_text, error_text_length);
}

extern "C" int MPI_Abort(MPI_Comm communicator, int error_code) {
  int relation = MPI_UNEQUAL;
  if (PMPI_Comm_compare(communicator, MPI_COMM_WORLD, &relation) !=
          MPI_SUCCESS ||
      (relation != MPI_IDENT && relation != MPI_CONGRUENT) ||
      error_code != EXIT_FAILURE || injected_calls != 1 ||
      finalize_calls != 0 || (cached_rank != 0 && cached_rank != 1)) {
    unexpected_abort();
  }
  if (cached_rank == 0) {
    write_text(mode == failure_mode::rank
                   ? "observed MPI_Abort rank=0 trace-rank affected-communicator\n"
                   : "observed MPI_Abort rank=0 trace-allreduce affected-communicator\n");
  } else {
    write_text(mode == failure_mode::rank
                   ? "observed MPI_Abort rank=1 trace-rank affected-communicator\n"
                   : "observed MPI_Abort rank=1 trace-allreduce affected-communicator\n");
  }
  if (PMPI_Barrier(MPI_COMM_WORLD) != MPI_SUCCESS) {
    std::_Exit(90);
  }
  std::_Exit(86);
}

int main(int argument_count, char** argument_values) {
  if (argument_count != 2 || MPI_Init(&argument_count, &argument_values) !=
                                 MPI_SUCCESS) {
    return 2;
  }
  auto size = 0;
  if (PMPI_Comm_rank(MPI_COMM_WORLD, &cached_rank) != MPI_SUCCESS ||
      PMPI_Comm_size(MPI_COMM_WORLD, &size) != MPI_SUCCESS || size != 2) {
    return 3;
  }
  auto const requested_mode = std::string_view{argument_values[1]};
  if (requested_mode == "rank") {
    mode = failure_mode::rank;
  } else if (requested_mode == "allreduce") {
    mode = failure_mode::allreduce;
  } else {
    return 4;
  }

  active = true;
  static_cast<void>(parhip::mpi::trace::resolve_run_id_collectively(
      MPI_COMM_WORLD, std::string{"common-trace-run"}));
  write_text("returned-from-failure\n");
  std::_Exit(92);
}
