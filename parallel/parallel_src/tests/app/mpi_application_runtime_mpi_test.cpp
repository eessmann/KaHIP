#include <mpi.h>

#include <cstdlib>
#include <iostream>

#include "communication/mpi_application.h"

namespace {
void verify_finalization() {
  auto finalized = 0;
  if (MPI_Finalized(&finalized) != MPI_SUCCESS || finalized == 0) {
    std::cerr << "application runtime did not finalize MPI\n";
    std::_Exit(EXIT_FAILURE);
  }
}
}  // namespace

int main(int argc, char** argv) {
  if (std::atexit(verify_finalization) != 0) {
    std::cerr << "could not install MPI finalization verifier\n";
    return EXIT_FAILURE;
  }

  parhip::mpi::application_runtime runtime{argc, argv, "runtime smoke test"};
  return runtime.execute([](parhip::mpi::communicator_view communicator) {
    auto comparison = int{MPI_UNEQUAL};
    parhip::mpi::check_or_abort(
        MPI_Comm_compare(MPI_COMM_WORLD, communicator.native_handle(),
                         &comparison),
        communicator.native_handle(), "MPI_Comm_compare(runtime smoke test)");
    if (communicator.native_handle() == MPI_COMM_WORLD ||
        comparison != MPI_CONGRUENT) {
      std::cerr << "operation communicator is not a distinct duplicate\n";
      return EXIT_FAILURE;
    }

    auto handler = MPI_Errhandler{};
    parhip::mpi::check_or_abort(
        MPI_Comm_get_errhandler(communicator.native_handle(), &handler),
        communicator.native_handle(),
        "MPI_Comm_get_errhandler(runtime smoke test)");
    if (handler != MPI_ERRORS_RETURN) {
      std::cerr << "operation communicator does not return MPI errors\n";
      return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
  });
}
