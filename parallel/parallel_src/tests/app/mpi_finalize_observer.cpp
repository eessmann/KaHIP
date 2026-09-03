#include <mpi.h>

#include <cstdlib>
#include <string_view>
#include <unistd.h>

namespace {
auto initialized = false;
auto finalized = false;

void write_text(std::string_view text) noexcept {
  static_cast<void>(::write(STDERR_FILENO, text.data(), text.size()));
}

__attribute__((destructor)) void verify_mpi_was_finalized() {
  if (initialized && !finalized) {
    write_text("executable returned without MPI_Finalize\n");
    std::_Exit(88);
  }
}
}  // namespace

extern "C" int MPI_Init(int* argument_count, char*** argument_values) {
  auto const result = PMPI_Init(argument_count, argument_values);
  initialized = result == MPI_SUCCESS;
  return result;
}

extern "C" int MPI_Finalize() {
  auto const result = PMPI_Finalize();
  finalized = result == MPI_SUCCESS;
  return result;
}
