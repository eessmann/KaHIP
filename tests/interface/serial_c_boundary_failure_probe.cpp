#include <array>
#include <atomic>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <new>
#include <streambuf>
#include <string_view>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include "kaHIP_interface.h"

namespace {
std::atomic_bool fail_next_allocation = false;
std::streambuf* original_cout_buffer = nullptr;
std::array<char, 4096> stderr_buffer{};

void write_text(std::string_view text) noexcept {
#ifdef _WIN32
  static_cast<void>(::_write(2, text.data(),
                             static_cast<unsigned int>(text.size())));
#else
  static_cast<void>(::write(STDERR_FILENO, text.data(), text.size()));
#endif
}

void observe_abort(int) {
  if (std::cout.rdbuf() != original_cout_buffer) {
    write_text("serial C boundary aborted before restoring std::cout\n");
    std::_Exit(92);
  }
  write_text("observed serial C boundary abort after restoring std::cout\n");
  std::_Exit(86);
}
}  // namespace

void* operator new(std::size_t size) {
  if (fail_next_allocation.load() && original_cout_buffer != nullptr &&
      std::cout.rdbuf() != original_cout_buffer &&
      fail_next_allocation.exchange(false)) {
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

int main(int argc, char** argv) {
  if (argc != 2 || std::string_view{argv[1]} != "allocation") {
    std::fputs("usage: serial_c_boundary_failure_probe allocation\n", stderr);
    return 64;
  }

  if (std::setvbuf(stderr, stderr_buffer.data(), _IOFBF,
                   stderr_buffer.size()) != 0) {
    std::fputs("could not configure buffered stderr\n", stderr);
    return 65;
  }
  original_cout_buffer = std::cout.rdbuf();
  std::signal(SIGABRT, observe_abort);

  auto n = 1;
  auto xadj = std::array<kahip_idx, 2>{0, 0};
  auto ignored_adjacency = kahip_idx{0};
  auto nparts = 1;
  auto imbalance = 0.03;
  auto edgecut = kahip_idx{17};
  auto partition = std::array{23};
  write_text("armed serial allocation failure\n");
  fail_next_allocation = true;
  kaffpa(&n, nullptr, xadj.data(), nullptr, &ignored_adjacency, &nparts,
         &imbalance, true, 1, ECO, &edgecut, partition.data());

  std::fputs("kaffpa returned after an injected exception\n", stderr);
  return 71;
}
