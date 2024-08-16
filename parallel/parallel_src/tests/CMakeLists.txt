# Automatically enable catch2 to generate ctest targets
list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(CTest)
include(Catch)

# MPI Version of Catch Test Runner
add_library(catch_mpi_runner catch_mpi/catch_mpi_runner.cpp)
target_link_libraries(catch_mpi_runner PUBLIC MPI::MPI_CXX)
target_link_libraries(catch_mpi_runner PUBLIC Catch2::Catch2)

# Parallel contraction
add_executable(parallel_contraction_test parallel_contraction/parallel_contraction_test.cpp)
target_link_libraries(parallel_contraction_test PRIVATE Catch2::Catch2WithMain libparallel)
target_link_libraries(parallel_contraction_test PRIVATE libmodified_kahip_interface)

catch_discover_tests(
        parallel_contraction_test
        TEST_PREFIX "unit-"
        OUTPUT_DIR .
        OUTPUT_PREFIX "unit-")
