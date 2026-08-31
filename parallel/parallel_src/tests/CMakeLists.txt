include(Catch)

# MPI Version of Catch Test Runner
add_library(catch_mpi_runner catch_mpi/catch_mpi_runner.cpp)
target_link_libraries(catch_mpi_runner PUBLIC MPI::MPI_CXX)
target_link_libraries(catch_mpi_runner PUBLIC Catch2::Catch2)
target_link_libraries(catch_mpi_runner PRIVATE kahip_options)

# Parallel contraction
add_executable(
    parallel_contraction_test
    parallel_contraction/parallel_contraction_test.cpp
)
target_link_libraries(
    parallel_contraction_test
    PRIVATE Catch2::Catch2WithMain parallel
)
target_link_libraries(
    parallel_contraction_test
    PRIVATE libmodified_kahip_interface fmt::fmt
)

catch_discover_tests(
        parallel_contraction_test
        TEST_PREFIX "unit-"
        OUTPUT_DIR .
        OUTPUT_PREFIX "unit-"
)

# Parallel contraction
add_executable(
    parallel_contraction_mpi_test
    parallel_contraction/parallel_contraction_mpi_test.cpp
)
target_link_libraries(parallel_contraction_mpi_test PRIVATE catch_mpi_runner)
target_link_libraries(parallel_contraction_mpi_test PRIVATE parallel)
target_link_libraries(
    parallel_contraction_mpi_test
    PRIVATE libmodified_kahip_interface fmt::fmt
)

add_test(
    NAME unit-parallel-contraction-mpi
    COMMAND
        "${MPIEXEC_EXECUTABLE}" "${MPIEXEC_NUMPROC_FLAG}" 5
        ${MPIEXEC_PREFLAGS}
        $<TARGET_FILE:parallel_contraction_mpi_test>
        ${MPIEXEC_POSTFLAGS}
)

add_executable(mpi_adapter_test communication/mpi_adapter_test.cpp)
target_link_libraries(
    mpi_adapter_test
    PRIVATE catch_mpi_runner parallel kahip_options kahip_warnings
)

foreach(mpi_ranks RANGE 1 5)
    add_test(
        NAME unit-mpi-adapter-${mpi_ranks}-rank
        COMMAND
            "${MPIEXEC_EXECUTABLE}" "${MPIEXEC_NUMPROC_FLAG}" ${mpi_ranks}
            ${MPIEXEC_PREFLAGS}
            $<TARGET_FILE:mpi_adapter_test>
            ${MPIEXEC_POSTFLAGS}
    )
    set_tests_properties(
        unit-mpi-adapter-${mpi_ranks}-rank
        PROPERTIES
            PROCESSORS ${mpi_ranks}
            RUN_SERIAL TRUE
            TIMEOUT 10
    )
endforeach()
set_tests_properties(
    unit-parallel-contraction-mpi
    PROPERTIES
        PROCESSORS 5
        RUN_SERIAL TRUE
)

target_link_libraries(
    parallel_contraction_test
    PRIVATE kahip_options kahip_warnings
)
target_link_libraries(
    parallel_contraction_mpi_test
    PRIVATE kahip_options kahip_warnings
)
