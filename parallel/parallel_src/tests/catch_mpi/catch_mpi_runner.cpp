//
// Created by Erich Essmann on 16/08/2024.
//
#include <mpi.h>
#include <catch2/catch_session.hpp>

int main(int argc, char* argv[]) {
    Catch::Session session;  // There must be exactly one instance
    // writing to session.configData() here sets defaults
    // this is the preferred way to set them
    int returnCode = session.applyCommandLine(argc, argv);
    if (returnCode != 0)  // Indicates a command line error
        return returnCode;
    // global setup...
    MPI_Init(&argc, &argv);
	  MPI_Comm_set_errhandler(MPI_COMM_WORLD, MPI_ERRORS_RETURN);
    int result = session.run();
    // global clean-up...
    MPI_Finalize();
    return result;
}