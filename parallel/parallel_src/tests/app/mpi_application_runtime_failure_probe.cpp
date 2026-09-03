#include <mpi.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>

#include "communication/mpi_application.h"
#include "tools/fatal_diagnostics.h"

namespace {
enum class failure_mode {
  initialization,
  finalization,
  operation_exception,
  backend_error,
};

auto selected_mode = failure_mode::initialization;
volatile std::sig_atomic_t diagnostic_was_flushed = 0;
constexpr auto injected_initialization_error = 17301;
constexpr auto injected_finalization_error = 17302;

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

[[noreturn]] void observed_process_abort(int) noexcept {
  if (diagnostic_was_flushed == 0) {
    write_text("process aborted before diagnostic flush\n");
    std::_Exit(91);
  }
  write_text("observed process abort after diagnostic flush\n");
  std::_Exit(86);
}
}  // namespace

extern "C" int MPI_Init(int* argument_count, char*** argument_values) {
  if (selected_mode == failure_mode::initialization) {
    return injected_initialization_error;
  }
  return PMPI_Init(argument_count, argument_values);
}

extern "C" int MPI_Finalize() {
  if (selected_mode == failure_mode::finalization) {
    return injected_finalization_error;
  }
  return PMPI_Finalize();
}

extern "C" int MPI_Abort(MPI_Comm communicator, int) {
  if (selected_mode != failure_mode::operation_exception &&
      selected_mode != failure_mode::backend_error) {
    write_text("unexpected MPI_Abort for lifecycle failure\n");
    std::_Exit(92);
  }
  if (diagnostic_was_flushed == 0) {
    write_text("MPI_Abort occurred before diagnostic flush\n");
    std::_Exit(91);
  }

  auto comparison = int{MPI_UNEQUAL};
  if (communicator == MPI_COMM_WORLD ||
      PMPI_Comm_compare(MPI_COMM_WORLD, communicator, &comparison) !=
          MPI_SUCCESS ||
      comparison != MPI_CONGRUENT) {
    write_text("MPI_Abort did not target the operation communicator\n");
    std::_Exit(93);
  }
  write_text(
      "observed operation communicator MPI_Abort after diagnostic flush\n");
  std::_Exit(86);
}

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr,
                 "usage: mpi_application_runtime_failure_probe MODE\n");
    return 64;
  }
  auto const mode = std::string_view{argv[1]};
  if (mode == "initialization") {
    selected_mode = failure_mode::initialization;
  } else if (mode == "finalization") {
    selected_mode = failure_mode::finalization;
  } else if (mode == "operation-exception") {
    selected_mode = failure_mode::operation_exception;
  } else if (mode == "backend-error") {
    selected_mode = failure_mode::backend_error;
  } else {
    std::fprintf(stderr, "unknown failure mode: %s\n", argv[1]);
    return 64;
  }

  static_cast<void>(
      kahip::diagnostics::exchange_sink_for_testing(&observing_sink));
  if (std::signal(SIGABRT, observed_process_abort) == SIG_ERR) {
    std::fputs("could not install SIGABRT handler\n", stderr);
    return 70;
  }

  parhip::mpi::application_runtime runtime{
      argc, argv, std::string{"runtime failure probe"}};
  return runtime.execute([](parhip::mpi::communicator_view communicator) -> int {
    if (selected_mode == failure_mode::operation_exception) {
      throw std::runtime_error{"injected operation exception"};
    }
    if (selected_mode == failure_mode::backend_error) {
      parhip::mpi::abort_on_mpi_error(
          communicator.native_handle(), MPI_ERR_OTHER,
          "injected application backend error");
    }
    return EXIT_SUCCESS;
  });
}
