#include <mpi.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string_view>
#include <vector>
#include <unistd.h>

#include "communication/mpi_adapter.h"
#include "tools/fatal_diagnostics.h"

namespace {
auto after_finalization = false;
volatile std::sig_atomic_t diagnostic_was_flushed = 0;

void write_text(std::string_view text) noexcept {
  static_cast<void>(::write(STDERR_FILENO, text.data(), text.size()));
}

void observe_diagnostic(std::string_view message) noexcept {
  write_text(message);
  write_text("\n");
}

void observe_flush() noexcept {
  diagnostic_was_flushed = 1;
  write_text("observed synchronous diagnostic flush\n");
}

constexpr auto observing_sink = kahip::diagnostics::sink{
    .write = observe_diagnostic,
    .flush = observe_flush,
};

[[noreturn]] void forbidden_post_finalize_call(char const* operation) noexcept {
  std::fprintf(stderr, "forbidden MPI call after finalization: %s\n",
               operation);
  std::_Exit(90);
}

[[noreturn]] void observed_abort(int) noexcept {
  if (diagnostic_was_flushed == 0) {
    write_text("owned handle aborted before diagnostic flush\n");
    std::_Exit(91);
  }
  write_text("observed owned-handle abort after diagnostic flush\n");
  std::_Exit(86);
}
}  // namespace

extern "C" int MPI_Finalize() {
  auto const result = PMPI_Finalize();
  after_finalization = result == MPI_SUCCESS;
  return result;
}

extern "C" int MPI_Comm_free(MPI_Comm* communicator) {
  if (after_finalization) {
    forbidden_post_finalize_call("MPI_Comm_free");
  }
  return PMPI_Comm_free(communicator);
}

extern "C" int MPI_Type_free(MPI_Datatype* datatype) {
  if (after_finalization) {
    forbidden_post_finalize_call("MPI_Type_free");
  }
  return PMPI_Type_free(datatype);
}

extern "C" int MPI_Abort(MPI_Comm communicator, int error_code) {
  if (after_finalization) {
    forbidden_post_finalize_call("MPI_Abort");
  }
  return PMPI_Abort(communicator, error_code);
}

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fputs("usage: mpi_owned_handle_lifetime_failure_probe MODE\n",
               stderr);
    return 64;
  }
  auto const mode = std::string_view{argv[1]};
  if (mode != "communicator" && mode != "datatype" &&
      mode != "distributed-graph") {
    std::fprintf(stderr, "unknown mode: %s\n", argv[1]);
    return 64;
  }
  static_cast<void>(
      kahip::diagnostics::exchange_sink_for_testing(&observing_sink));
  if (std::signal(SIGABRT, observed_abort) == SIG_ERR) {
    return 70;
  }
  if (MPI_Init(&argc, &argv) != MPI_SUCCESS) {
    return 70;
  }

  auto communicator = std::unique_ptr<parhip::mpi::communicator>{};
  auto datatype = std::unique_ptr<parhip::mpi::datatype>{};
  auto graph = std::unique_ptr<parhip::mpi::distributed_graph>{};
  if (mode == "communicator") {
    communicator = std::make_unique<parhip::mpi::communicator>(
        parhip::mpi::communicator_view{MPI_COMM_WORLD});
  } else if (mode == "datatype") {
    auto handle = MPI_DATATYPE_NULL;
    if (MPI_Type_contiguous(2, MPI_INT, &handle) != MPI_SUCCESS ||
        MPI_Type_commit(&handle) != MPI_SUCCESS) {
      return 70;
    }
    datatype = std::make_unique<parhip::mpi::datatype>(
        parhip::mpi::datatype::owned(handle));
  } else {
    graph = std::make_unique<parhip::mpi::distributed_graph>(
        parhip::mpi::communicator_view{MPI_COMM_WORLD}, std::vector<int>{});
  }

  if (MPI_Finalize() != MPI_SUCCESS) {
    return 70;
  }
  communicator.reset();
  datatype.reset();
  graph.reset();
  return 2;
}
