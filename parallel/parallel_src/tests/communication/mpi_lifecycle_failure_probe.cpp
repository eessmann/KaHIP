#include <mpi.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string_view>

#include "communication/mpi_failure.h"

namespace {
enum class failure_mode {
  initialized_query,
  finalized_query,
  passthrough,
};

auto selected_mode = failure_mode::initialized_query;

[[noreturn]] void forbidden_mpi_call(char const* name) noexcept {
  std::fprintf(stderr, "forbidden MPI call after lifecycle-query failure: %s\n",
               name);
  std::_Exit(90);
}

[[noreturn]] void observed_abort(int) noexcept {
  std::fputs("observed SIGABRT from lifecycle-query failure\n", stderr);
  std::_Exit(86);
}
}  // namespace

extern "C" int MPI_Initialized(int* flag) {
  if (selected_mode == failure_mode::initialized_query) {
    return MPI_ERR_OTHER;
  }
  if (selected_mode == failure_mode::passthrough) {
    return PMPI_Initialized(flag);
  }
  *flag = 1;
  return MPI_SUCCESS;
}

extern "C" int MPI_Finalized(int* flag) {
  if (selected_mode == failure_mode::finalized_query) {
    return MPI_ERR_OTHER;
  }
  if (selected_mode == failure_mode::passthrough) {
    return PMPI_Finalized(flag);
  }
  forbidden_mpi_call("MPI_Finalized");
}

extern "C" int MPI_Error_string(int, char*, int*) {
  forbidden_mpi_call("MPI_Error_string");
}

extern "C" int MPI_Abort(MPI_Comm, int) {
  forbidden_mpi_call("MPI_Abort");
}

int main(int argc, char* argv[]) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: mpi_lifecycle_failure_probe MODE\n");
    return 64;
  }
  auto const mode = std::string_view{argv[1]};
  if (mode == "initialized") {
    selected_mode = failure_mode::initialized_query;
  } else if (mode == "finalized") {
    selected_mode = failure_mode::finalized_query;
  } else if (mode == "before-initialization") {
    selected_mode = failure_mode::passthrough;
    if (parhip::mpi::runtime_is_active()) {
      std::fputs("MPI unexpectedly active before initialization\n", stderr);
      return 2;
    }
    return 0;
  } else if (mode == "post-finalization") {
    selected_mode = failure_mode::passthrough;
    auto const init_result = MPI_Init(&argc, &argv);
    if (init_result != MPI_SUCCESS) {
      std::fprintf(stderr, "MPI_Init returned raw error %d\n", init_result);
      return 70;
    }
    if (!parhip::mpi::runtime_is_active()) {
      std::fputs("MPI unexpectedly inactive after initialization\n", stderr);
      return 2;
    }
    auto const finalize_result = MPI_Finalize();
    if (finalize_result != MPI_SUCCESS) {
      std::fprintf(
          stderr, "MPI_Finalize returned raw error %d\n", finalize_result);
      return 70;
    }
    if (parhip::mpi::runtime_is_active()) {
      std::fputs("MPI unexpectedly active after finalization\n", stderr);
      return 2;
    }
    return 0;
  } else {
    std::fprintf(stderr, "unknown lifecycle failure mode: %s\n", argv[1]);
    return 64;
  }

  if (std::signal(SIGABRT, observed_abort) == SIG_ERR) {
    std::fputs("could not install SIGABRT observation handler\n", stderr);
    return 70;
  }
  auto const active = parhip::mpi::runtime_is_active();
  std::fprintf(stderr,
               "runtime_is_active returned unexpectedly: %s\n",
               active ? "active" : "inactive");
  return active ? 2 : 0;
}
