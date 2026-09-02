#include <mpi.h>

#include <array>
#include <atomic>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string_view>
#include <unistd.h>

#include "kaHIP_interface.h"
#include "tools/fatal_diagnostics.h"

namespace {
std::atomic_bool fail_next_allocation = false;
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
}  // namespace

void* operator new(std::size_t size) {
  if (fail_next_allocation.exchange(false)) {
    throw std::bad_alloc{};
  }
  if (auto* storage = std::malloc(size); storage != nullptr) {
    return storage;
  }
  throw std::bad_alloc{};
}

void operator delete(void* storage) noexcept { std::free(storage); }

void operator delete(void* storage, std::size_t) noexcept {
  std::free(storage);
}

extern "C" int MPI_Abort(MPI_Comm communicator, int) {
  if (diagnostic_was_flushed == 0) {
    write_text("kaffpaE boundary aborted before diagnostic flush\n");
    std::_Exit(91);
  }
  auto comparison = int{MPI_UNEQUAL};
  if (communicator == MPI_COMM_NULL ||
      PMPI_Comm_compare(MPI_COMM_WORLD, communicator, &comparison) !=
          MPI_SUCCESS ||
      comparison != MPI_IDENT) {
    write_text("kaffpaE boundary aborted the wrong communicator\n");
    std::_Exit(92);
  }
  write_text("observed kaffpaE communicator abort after diagnostic flush\n");
  std::_Exit(86);
}

int main(int argc, char** argv) {
  if (argc != 2 || std::string_view{argv[1]} != "allocation") {
    std::fputs("usage: kaffpae_c_boundary_failure_probe allocation\n", stderr);
    return 64;
  }
  if (PMPI_Init(&argc, &argv) != MPI_SUCCESS) {
    std::fputs("could not initialize MPI\n", stderr);
    return 70;
  }

  static_cast<void>(
      kahip::diagnostics::exchange_sink_for_testing(&observing_sink));

  auto n = 1;
  auto xadj = std::array{0, 0};
  auto ignored_adjacency = 0;
  auto nparts = 1;
  auto imbalance = 0.03;
  auto edgecut = 0;
  auto balance = 0.0;
  auto partition = std::array{0};
  fail_next_allocation = true;
  kaffpaE(&n, nullptr, xadj.data(), nullptr, &ignored_adjacency, &nparts,
          &imbalance, false, false, 0, 1, ECO, MPI_COMM_WORLD, &edgecut,
          &balance, partition.data());

  std::fputs("kaffpaE returned after an injected exception\n", stderr);
  return 71;
}
