include(Catch)

add_executable(random_functions_test tools/random_functions_test.cpp)
target_link_libraries(
    random_functions_test
    PRIVATE Catch2::Catch2WithMain parallel kahip_options kahip_warnings
)
catch_discover_tests(
    random_functions_test
    TEST_PREFIX "unit-"
    OUTPUT_DIR .
    OUTPUT_PREFIX "unit-"
)

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

foreach(mpi_ranks RANGE 1 5)
    add_test(
        NAME unit-parallel-contraction-mpi-${mpi_ranks}-rank
        COMMAND
            "${MPIEXEC_EXECUTABLE}" "${MPIEXEC_NUMPROC_FLAG}" ${mpi_ranks}
            ${MPIEXEC_PREFLAGS}
            $<TARGET_FILE:parallel_contraction_mpi_test>
            ${MPIEXEC_POSTFLAGS}
    )
    set_tests_properties(
        unit-parallel-contraction-mpi-${mpi_ranks}-rank
        PROPERTIES
            PROCESSORS ${mpi_ranks}
            RUN_SERIAL TRUE
            TIMEOUT 10
    )
endforeach()

add_test(
    NAME unit-parallel-label-invalid-3-rank
    COMMAND
        "${MPIEXEC_EXECUTABLE}" "${MPIEXEC_NUMPROC_FLAG}" 3
        ${MPIEXEC_PREFLAGS}
        $<TARGET_FILE:parallel_contraction_mpi_test>
        "label mapping rejects an out-of-domain local label collectively"
        ${MPIEXEC_POSTFLAGS}
)

add_test(
    NAME unit-parallel-label-domain-skew-3-rank
    COMMAND
        "${MPIEXEC_EXECUTABLE}" "${MPIEXEC_NUMPROC_FLAG}" 3
        ${MPIEXEC_PREFLAGS}
        $<TARGET_FILE:parallel_contraction_mpi_test>
        "label mapping rejects an empty-payload global-count mismatch collectively"
        ${MPIEXEC_POSTFLAGS}
)
set_tests_properties(
    unit-parallel-label-domain-skew-3-rank
    PROPERTIES
        PROCESSORS 3
        RUN_SERIAL TRUE
        TIMEOUT 5
)
set_tests_properties(
    unit-parallel-label-invalid-3-rank
    PROPERTIES
        PROCESSORS 3
        RUN_SERIAL TRUE
        TIMEOUT 5
)

foreach(
    failure_case
    IN ITEMS
        edge-source
        edge-target
        node-weight
)
    if(failure_case STREQUAL "edge-source")
        set(
            failure_filter
            "quotient redistribution rejects a tail-padding edge source collectively"
        )
    elseif(failure_case STREQUAL "edge-target")
        set(
            failure_filter
            "quotient redistribution rejects a tail-padding edge target collectively"
        )
    else()
        set(
            failure_filter
            "quotient redistribution rejects a tail-padding node weight collectively"
        )
    endif()
    add_test(
        NAME unit-parallel-quotient-invalid-${failure_case}-3-rank
        COMMAND
            "${MPIEXEC_EXECUTABLE}" "${MPIEXEC_NUMPROC_FLAG}" 3
            ${MPIEXEC_PREFLAGS}
            $<TARGET_FILE:parallel_contraction_mpi_test>
            "${failure_filter}"
            ${MPIEXEC_POSTFLAGS}
    )
    set_tests_properties(
        unit-parallel-quotient-invalid-${failure_case}-3-rank
        PROPERTIES
            PROCESSORS 3
            RUN_SERIAL TRUE
            TIMEOUT 5
    )
endforeach()

add_test(
    NAME unit-parallel-quotient-domain-skew-3-rank
    COMMAND
        "${MPIEXEC_EXECUTABLE}" "${MPIEXEC_NUMPROC_FLAG}" 3
        ${MPIEXEC_PREFLAGS}
        $<TARGET_FILE:parallel_contraction_mpi_test>
        "quotient redistribution rejects an empty-payload coarse-count mismatch collectively"
        ${MPIEXEC_POSTFLAGS}
)

foreach(
    receive_case
    IN ITEMS
        label-request-owner
        label-reply-correlation
        label-reply-domain
        quotient-edge-owner
        quotient-edge-target-domain
        quotient-edge-sequence
        quotient-weight-owner
)
    if(receive_case STREQUAL "label-request-owner")
        set(
            receive_filter
            "label request receive validation rejects a valid wrong-owner record collectively"
        )
    elseif(receive_case STREQUAL "label-reply-correlation")
        set(
            receive_filter
            "label reply receive validation rejects bad keyed correlation collectively"
        )
    elseif(receive_case STREQUAL "label-reply-domain")
        set(
            receive_filter
            "label reply receive validation rejects an out-of-domain coarse ID collectively"
        )
    elseif(receive_case STREQUAL "quotient-edge-owner")
        set(
            receive_filter
            "quotient edge receive validation rejects a valid wrong-owner source collectively"
        )
    elseif(receive_case STREQUAL "quotient-edge-target-domain")
        set(
            receive_filter
            "quotient edge receive validation rejects an out-of-domain target collectively"
        )
    elseif(receive_case STREQUAL "quotient-edge-sequence")
        set(
            receive_filter
            "quotient edge receive validation rejects a sender-sequence gap collectively"
        )
    else()
        set(
            receive_filter
            "quotient node-weight receive validation rejects a valid wrong-owner ID collectively"
        )
    endif()
    add_test(
        NAME unit-parallel-receive-${receive_case}-3-rank
        COMMAND
            "${MPIEXEC_EXECUTABLE}" "${MPIEXEC_NUMPROC_FLAG}" 3
            ${MPIEXEC_PREFLAGS}
            $<TARGET_FILE:parallel_contraction_mpi_test>
            "${receive_filter}"
            ${MPIEXEC_POSTFLAGS}
    )
    set_tests_properties(
        unit-parallel-receive-${receive_case}-3-rank
        PROPERTIES
            PROCESSORS 3
            RUN_SERIAL TRUE
            TIMEOUT 5
    )
endforeach()
set_tests_properties(
    unit-parallel-quotient-domain-skew-3-rank
    PROPERTIES
        PROCESSORS 3
        RUN_SERIAL TRUE
        TIMEOUT 5
)

add_executable(
    parallel_projection_mpi_test
    parallel_projection/parallel_projection_mpi_test.cpp
)
target_link_libraries(
    parallel_projection_mpi_test
    PRIVATE catch_mpi_runner parallel kahip_options kahip_warnings
)
add_test(
    NAME unit-parallel-projection-mpi
    COMMAND
        "${MPIEXEC_EXECUTABLE}" "${MPIEXEC_NUMPROC_FLAG}" 2
        ${MPIEXEC_PREFLAGS}
        $<TARGET_FILE:parallel_projection_mpi_test>
        ${MPIEXEC_POSTFLAGS}
)
set_tests_properties(
    unit-parallel-projection-mpi
    PROPERTIES
        PROCESSORS 2
        RUN_SERIAL TRUE
        TIMEOUT 10
)

function(add_projection_validation_test test_name mpi_ranks catch_filter)
    add_test(
        NAME ${test_name}
        COMMAND
            "${MPIEXEC_EXECUTABLE}" "${MPIEXEC_NUMPROC_FLAG}" ${mpi_ranks}
            ${MPIEXEC_PREFLAGS}
            $<TARGET_FILE:parallel_projection_mpi_test>
            "${catch_filter}"
            ${MPIEXEC_POSTFLAGS}
    )
    set_tests_properties(
        ${test_name}
        PROPERTIES
            PROCESSORS ${mpi_ranks}
            RUN_SERIAL TRUE
            TIMEOUT 5
    )
endfunction()

add_projection_validation_test(
    unit-parallel-projection-domain-skew-3-rank
    3
    "projection rejects an empty-payload coarse-count mismatch collectively"
)
add_projection_validation_test(
    unit-parallel-projection-zero-3-rank
    3
    "zero-node projection performs two empty dense exchanges"
)
add_projection_validation_test(
    unit-parallel-projection-invalid-tail-3-rank
    3
    "projection rejects a tail coarse node before exchanging or mutating"
)
add_projection_validation_test(
    unit-parallel-projection-uneven-3-rank
    3
    "projection routes an uneven coarse domain by exact ownership"
)
add_projection_validation_test(
    unit-parallel-projection-request-corruption-2-rank
    2
    "projection request corruption fails before replies and preserves labels"
)
add_projection_validation_test(
    unit-parallel-projection-reply-corruption-2-rank
    2
    "projection reply corruption fails transactionally"
)
add_projection_validation_test(
    unit-parallel-projection-reply-duplicate-2-rank
    2
    "projection rejects duplicate replies without partial label writes"
)

foreach(mpi_ranks RANGE 1 5)
    add_test(
        NAME unit-parallel-block-owner-${mpi_ranks}-rank
        COMMAND
            "${MPIEXEC_EXECUTABLE}" "${MPIEXEC_NUMPROC_FLAG}" ${mpi_ranks}
            ${MPIEXEC_PREFLAGS}
            $<TARGET_FILE:parallel_projection_mpi_test>
            "block-down dense owner phase uses keyed collective and exact updates"
            ${MPIEXEC_POSTFLAGS}
    )
    set_tests_properties(
        unit-parallel-block-owner-${mpi_ranks}-rank
        PROPERTIES
            PROCESSORS ${mpi_ranks}
            RUN_SERIAL TRUE
            TIMEOUT 10
    )
endforeach()

add_test(
    NAME unit-parallel-block-owner-local-conflict-3-rank
    COMMAND
        "${MPIEXEC_EXECUTABLE}" "${MPIEXEC_NUMPROC_FLAG}" 3
        ${MPIEXEC_PREFLAGS}
        $<TARGET_FILE:parallel_projection_mpi_test>
        "block-down rejects a rank-local conflicting coarse block collectively"
        ${MPIEXEC_POSTFLAGS}
)

add_test(
    NAME unit-parallel-block-owner-identical-duplicate-3-rank
    COMMAND
        "${MPIEXEC_EXECUTABLE}" "${MPIEXEC_NUMPROC_FLAG}" 3
        ${MPIEXEC_PREFLAGS}
        $<TARGET_FILE:parallel_projection_mpi_test>
        "block-down accepts an identical same-sender duplicate"
        ${MPIEXEC_POSTFLAGS}
)
set_tests_properties(
    unit-parallel-block-owner-identical-duplicate-3-rank
    PROPERTIES
        PROCESSORS 3
        RUN_SERIAL TRUE
        TIMEOUT 10
)

add_test(
    NAME unit-parallel-block-owner-domain-skew-3-rank
    COMMAND
        "${MPIEXEC_EXECUTABLE}" "${MPIEXEC_NUMPROC_FLAG}" 3
        ${MPIEXEC_PREFLAGS}
        $<TARGET_FILE:parallel_projection_mpi_test>
        "block-down rejects an empty-payload coarse-count mismatch collectively"
        ${MPIEXEC_POSTFLAGS}
)
set_tests_properties(
    unit-parallel-block-owner-domain-skew-3-rank
    PROPERTIES
        PROCESSORS 3
        RUN_SERIAL TRUE
        TIMEOUT 5
)
set_tests_properties(
    unit-parallel-block-owner-local-conflict-3-rank
    PROPERTIES
        PROCESSORS 3
        RUN_SERIAL TRUE
        TIMEOUT 5
)

foreach(
    failure_case
    IN ITEMS
        invalid-id
        cross-rank-conflict
        missing
)
    if(failure_case STREQUAL "invalid-id")
        set(
            failure_filter
            "block-down rejects a tail-padding coarse ID collectively"
        )
    elseif(failure_case STREQUAL "cross-rank-conflict")
        set(
            failure_filter
            "block-down rejects a cross-rank conflicting coarse block collectively"
        )
    else()
        set(
            failure_filter
            "block-down rejects a missing coarse block collectively"
        )
    endif()
    add_test(
        NAME unit-parallel-block-owner-${failure_case}-3-rank
        COMMAND
            "${MPIEXEC_EXECUTABLE}" "${MPIEXEC_NUMPROC_FLAG}" 3
            ${MPIEXEC_PREFLAGS}
            $<TARGET_FILE:parallel_projection_mpi_test>
            "${failure_filter}"
            ${MPIEXEC_POSTFLAGS}
    )
    set_tests_properties(
        unit-parallel-block-owner-${failure_case}-3-rank
        PROPERTIES
            PROCESSORS 3
            RUN_SERIAL TRUE
            TIMEOUT 5
    )
endforeach()

add_executable(mpi_adapter_test communication/mpi_adapter_test.cpp)
target_link_libraries(
    mpi_adapter_test
    PRIVATE
        catch_mpi_runner
        parallel
        parhip_interface
        kahip_options
        kahip_warnings
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

add_executable(
    mpi_lifecycle_failure_probe
    communication/mpi_lifecycle_failure_probe.cpp
)
target_link_libraries(
    mpi_lifecycle_failure_probe
    PRIVATE parallel kahip_options kahip_warnings
)

foreach(lifecycle_query IN ITEMS Initialized Finalized)
    string(TOLOWER "${lifecycle_query}" lifecycle_mode)
    add_test(
        NAME unit-mpi-lifecycle-${lifecycle_mode}-query-failure
        COMMAND
            "${CMAKE_COMMAND}"
            "-DPROBE=$<TARGET_FILE:mpi_lifecycle_failure_probe>"
            "-DMODE=${lifecycle_mode}"
            "-DEXPECTED_QUERY=MPI_${lifecycle_query}"
            -P
            "${CMAKE_CURRENT_SOURCE_DIR}/communication/verify_mpi_lifecycle_failure.cmake"
    )
    set_tests_properties(
        unit-mpi-lifecycle-${lifecycle_mode}-query-failure
        PROPERTIES
            RUN_SERIAL TRUE
            TIMEOUT 10
    )
endforeach()

foreach(lifecycle_state IN ITEMS before-initialization post-finalization)
    add_test(
        NAME unit-mpi-lifecycle-${lifecycle_state}-inactive
        COMMAND
            $<TARGET_FILE:mpi_lifecycle_failure_probe>
            "${lifecycle_state}"
    )
    set_tests_properties(
        unit-mpi-lifecycle-${lifecycle_state}-inactive
        PROPERTIES
            RUN_SERIAL TRUE
            TIMEOUT 5
    )
endforeach()

add_executable(mpi_trace_test communication/mpi_trace_test.cpp)
target_link_libraries(
    mpi_trace_test
    PRIVATE Catch2::Catch2WithMain parallel kahip_options kahip_warnings
)
catch_discover_tests(
    mpi_trace_test
    TEST_PREFIX "unit-"
    OUTPUT_DIR .
    OUTPUT_PREFIX "unit-"
)
if(KAHIP_ENABLE_MPI_TRACE)
    add_executable(
        mpi_trace_writer_mpi_test
        communication/mpi_trace_writer_mpi_test.cpp
    )
    target_link_libraries(
        mpi_trace_writer_mpi_test
        PRIVATE catch_mpi_runner parallel kahip_options kahip_warnings
    )

    function(add_mpi_trace_writer_test test_name catch_test_name)
        add_test(
            NAME ${test_name}
            COMMAND
                "${CMAKE_COMMAND}" -E env
                "KAHIP_TRACE_WRITER_TEST_BASE=${CMAKE_CURRENT_BINARY_DIR}/trace-writer"
                "${MPIEXEC_EXECUTABLE}" "${MPIEXEC_NUMPROC_FLAG}" 2
                ${MPIEXEC_PREFLAGS}
                $<TARGET_FILE:mpi_trace_writer_mpi_test>
                "${catch_test_name}"
                ${MPIEXEC_POSTFLAGS}
        )
        set_tests_properties(
            ${test_name}
            PROPERTIES
                PROCESSORS 2
                RUN_SERIAL TRUE
                TIMEOUT 4
                LABELS "mpi;trace"
        )
    endfunction()

    add_mpi_trace_writer_test(
        unit-mpi-trace-writer-presence-mismatch
        "trace writer rejects rank-local enablement mismatch"
    )
    add_mpi_trace_writer_test(
        unit-mpi-trace-writer-path-mismatch
        "trace writer rejects different rank-local base paths"
    )
    add_mpi_trace_writer_test(
        unit-mpi-trace-writer-all-unset
        "trace writer returns collectively when tracing is unset"
    )
    add_mpi_trace_writer_test(
        unit-mpi-trace-writer-common-path
        "trace writer uses one agreed base path"
    )

    add_test(
        NAME integration-mpi-trace-smoke
        COMMAND
            "${CMAKE_COMMAND}"
            "-DPARHIP_EXECUTABLE=$<TARGET_FILE:parhip>"
            "-DMPIEXEC_EXECUTABLE=${MPIEXEC_EXECUTABLE}"
            "-DMPIEXEC_NUMPROC_FLAG=${MPIEXEC_NUMPROC_FLAG}"
            "-DGRAPH_PATH=${PROJECT_SOURCE_DIR}/examples/rgg_n_2_15_s0.graph"
            "-DTRACE_BASE=${CMAKE_CURRENT_BINARY_DIR}/trace-smoke"
            -P
            "${CMAKE_CURRENT_SOURCE_DIR}/communication/mpi_trace_smoke.cmake"
    )
    set_tests_properties(
        integration-mpi-trace-smoke
        PROPERTIES
            PROCESSORS 2
            RUN_SERIAL TRUE
            TIMEOUT 60
            LABELS "integration;mpi;trace"
    )
endif()
target_link_libraries(
    parallel_contraction_test
    PRIVATE kahip_options kahip_warnings
)
target_link_libraries(
    parallel_contraction_mpi_test
    PRIVATE kahip_options kahip_warnings
)
