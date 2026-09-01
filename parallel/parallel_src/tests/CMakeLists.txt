include(Catch)

add_library(
    kahip_cube_fixture_obj
    OBJECT
        fixtures/cube_graph.cpp
        fixtures/cube_partition.cpp
)
target_sources(
    kahip_cube_fixture_obj
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
        FILES
            fixtures/cube_graph.h
            fixtures/cube_partition.h
)
target_link_libraries(
    kahip_cube_fixture_obj
    PRIVATE kahip_options kahip_warnings
)

add_executable(cube_graph_test fixtures/cube_graph_test.cpp)
target_link_libraries(
    cube_graph_test
    PRIVATE
        Catch2::Catch2WithMain
        kahip_cube_fixture_obj
        kahip_options
        kahip_warnings
)
catch_discover_tests(
    cube_graph_test
    TEST_PREFIX "unit-"
    OUTPUT_DIR .
    OUTPUT_PREFIX "unit-"
)

add_executable(cube_partition_test fixtures/cube_partition_test.cpp)
target_link_libraries(
    cube_partition_test
    PRIVATE
        Catch2::Catch2WithMain
        kahip_cube_fixture_obj
        kahip_options
        kahip_warnings
)
catch_discover_tests(
    cube_partition_test
    TEST_PREFIX "unit-"
    OUTPUT_DIR .
    OUTPUT_PREFIX "unit-"
)

add_executable(kahip_cube_generator fixtures/cube_graph_generator.cpp)
target_link_libraries(
    kahip_cube_generator
    PRIVATE kahip_cube_fixture_obj fmt::fmt kahip_options kahip_warnings
)

add_executable(
    kahip_cube_partition_verify
    fixtures/cube_partition_verify.cpp
)
target_link_libraries(
    kahip_cube_partition_verify
    PRIVATE kahip_cube_fixture_obj fmt::fmt kahip_options kahip_warnings
)
add_test(
    NAME integration-cube-fixture-cli
    COMMAND
        "${CMAKE_COMMAND}"
        "-DGENERATOR=$<TARGET_FILE:kahip_cube_generator>"
        "-DVERIFIER=$<TARGET_FILE:kahip_cube_partition_verify>"
        "-DWORK_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/cube-fixture"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/cube_fixture_cli.cmake"
)
set_tests_properties(
    integration-cube-fixture-cli
    PROPERTIES TIMEOUT 10 LABELS "integration;fixture"
)

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
    NAME unit-parallel-ghost-cnode-transaction-3-rank
    COMMAND
        "${MPIEXEC_EXECUTABLE}" "${MPIEXEC_NUMPROC_FLAG}" 3
        ${MPIEXEC_PREFLAGS}
        $<TARGET_FILE:parallel_contraction_mpi_test>
        "ghost CNode receive failures preserve the complete mapping and trace"
        ${MPIEXEC_POSTFLAGS}
)
set_tests_properties(
    unit-parallel-ghost-cnode-transaction-3-rank
    PROPERTIES PROCESSORS 3 RUN_SERIAL TRUE TIMEOUT 20
)

add_test(
    NAME unit-parallel-ghost-cnode-zero-domain-3-rank
    COMMAND
        "${MPIEXEC_EXECUTABLE}" "${MPIEXEC_NUMPROC_FLAG}" 3
        ${MPIEXEC_PREFLAGS}
        $<TARGET_FILE:parallel_contraction_mpi_test>
        "zero coarse domain with local work fails before sparse payload"
        ${MPIEXEC_POSTFLAGS}
)
set_tests_properties(
    unit-parallel-ghost-cnode-zero-domain-3-rank
    PROPERTIES PROCESSORS 3 RUN_SERIAL TRUE TIMEOUT 10
)

add_test(
    NAME unit-parallel-ghost-cnode-asymmetric-2-rank
    COMMAND
        "${MPIEXEC_EXECUTABLE}" "${MPIEXEC_NUMPROC_FLAG}" 2
        ${MPIEXEC_PREFLAGS}
        $<TARGET_FILE:parallel_contraction_mpi_test>
        "asymmetric ghost topology fails commonly before payload"
        ${MPIEXEC_POSTFLAGS}
)
set_tests_properties(
    unit-parallel-ghost-cnode-asymmetric-2-rank
    PROPERTIES PROCESSORS 2 RUN_SERIAL TRUE TIMEOUT 10
)

add_test(
    NAME unit-parallel-ghost-weight-transaction-3-rank
    COMMAND
        "${MPIEXEC_EXECUTABLE}" "${MPIEXEC_NUMPROC_FLAG}" 3
        ${MPIEXEC_PREFLAGS}
        $<TARGET_FILE:parallel_contraction_mpi_test>
        "ghost weight receive failures preserve every weight and trace"
        ${MPIEXEC_POSTFLAGS}
)
set_tests_properties(
    unit-parallel-ghost-weight-transaction-3-rank
    PROPERTIES PROCESSORS 3 RUN_SERIAL TRUE TIMEOUT 20
)

add_test(
    NAME unit-parallel-ghost-weight-asymmetric-2-rank
    COMMAND
        "${MPIEXEC_EXECUTABLE}" "${MPIEXEC_NUMPROC_FLAG}" 2
        ${MPIEXEC_PREFLAGS}
        $<TARGET_FILE:parallel_contraction_mpi_test>
        "asymmetric ghost-weight topology fails commonly before payload"
        ${MPIEXEC_POSTFLAGS}
)
set_tests_properties(
    unit-parallel-ghost-weight-asymmetric-2-rank
    PROPERTIES PROCESSORS 2 RUN_SERIAL TRUE TIMEOUT 10
)

add_test(
    NAME unit-parallel-ghost-weight-reuse-2-rank
    COMMAND
        "${MPIEXEC_EXECUTABLE}" "${MPIEXEC_NUMPROC_FLAG}" 2
        ${MPIEXEC_PREFLAGS}
        $<TARGET_FILE:parallel_contraction_mpi_test>
        "ghost weight exchange reuses a prewarmed plan and refreshes values"
        ${MPIEXEC_POSTFLAGS}
)
set_tests_properties(
    unit-parallel-ghost-weight-reuse-2-rank
    PROPERTIES PROCESSORS 2 RUN_SERIAL TRUE TIMEOUT 10
)

add_test(
    NAME unit-parallel-quotient-arithmetic-3-rank
    COMMAND
        "${MPIEXEC_EXECUTABLE}" "${MPIEXEC_NUMPROC_FLAG}" 3
        ${MPIEXEC_PREFLAGS}
        $<TARGET_FILE:parallel_contraction_mpi_test>
        "[arithmetic]"
        ${MPIEXEC_POSTFLAGS}
)
set_tests_properties(
    unit-parallel-quotient-arithmetic-3-rank
    PROPERTIES PROCESSORS 3 RUN_SERIAL TRUE TIMEOUT 15
)

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
    NAME unit-pmpi-callback-safety
    COMMAND
        "${CMAKE_COMMAND}"
        "-DSOURCE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/parallel_projection/parallel_projection_mpi_test.cpp"
        "-DPROFILE=projection"
        -P
        "${CMAKE_CURRENT_SOURCE_DIR}/communication/verify_pmpi_callback_safety.cmake"
)
set_tests_properties(unit-pmpi-callback-safety PROPERTIES TIMEOUT 5)

add_test(
    NAME unit-pmpi-callback-safety-ghost-label
    COMMAND
        "${CMAKE_COMMAND}"
        "-DSOURCE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/distributed_consistency/ghost_label_exchange_mpi_test.cpp"
        "-DPROFILE=ghost-label"
        -P
        "${CMAKE_CURRENT_SOURCE_DIR}/communication/verify_pmpi_callback_safety.cmake"
)
set_tests_properties(
    unit-pmpi-callback-safety-ghost-label
    PROPERTIES TIMEOUT 5
)

add_test(
    NAME unit-pmpi-callback-safety-ghost-label-failure
    COMMAND
        "${CMAKE_COMMAND}"
        "-DSOURCE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/distributed_consistency/ghost_label_failure_probe.cpp"
        "-DPROFILE=ghost-label-failure"
        -P
        "${CMAKE_CURRENT_SOURCE_DIR}/communication/verify_pmpi_callback_safety.cmake"
)
set_tests_properties(
    unit-pmpi-callback-safety-ghost-label-failure
    PROPERTIES TIMEOUT 5
)

add_executable(
    parallel_block_down_mpi_test
    parallel_projection/parallel_block_down_mpi_test.cpp
)
target_link_libraries(
    parallel_block_down_mpi_test
    PRIVATE catch_mpi_runner parallel kahip_options kahip_warnings
)
function(add_parallel_block_down_mpi_test test_name ranks test_filter)
    add_test(
        NAME "${test_name}"
        COMMAND
            "${MPIEXEC_EXECUTABLE}" "${MPIEXEC_NUMPROC_FLAG}" "${ranks}"
            ${MPIEXEC_PREFLAGS}
            $<TARGET_FILE:parallel_block_down_mpi_test>
            "${test_filter}"
            ${MPIEXEC_POSTFLAGS}
    )
    set_tests_properties(
        "${test_name}"
        PROPERTIES PROCESSORS "${ranks}" RUN_SERIAL TRUE TIMEOUT 15
    )
endfunction()

add_parallel_block_down_mpi_test(
    unit-parallel-block-down-protocol-2-rank 2
    "block-down uses one typed dense and one blocking neighborhood transaction"
)
add_parallel_block_down_mpi_test(
    unit-parallel-block-down-datatype-1-rank 1
    "block-down wire datatype has exact semantic extent"
)
foreach(rank RANGE 1 5)
    add_parallel_block_down_mpi_test(
        "unit-parallel-block-down-matrix-${rank}-rank" "${rank}"
        "block-down covers distributed rank-one through rank-five shapes"
    )
endforeach()
add_parallel_block_down_mpi_test(
    unit-parallel-block-down-empty-1-rank 1
    "globally empty block-down participates without sentinels"
)
add_parallel_block_down_mpi_test(
    unit-parallel-block-down-reuse-2-rank 2
    "block-down reuses a warm topology and refreshes staged blocks"
)
add_parallel_block_down_mpi_test(
    unit-parallel-block-down-dense-failure-2-rank 2
    "dense block-down receive failures preserve state and retry"
)
add_parallel_block_down_mpi_test(
    unit-parallel-block-down-dense-coverage-3-rank 3
    "dense block-down exact coverage rejects a replaced owner record"
)
foreach(rank IN ITEMS 2 3 4)
    add_parallel_block_down_mpi_test(
        "unit-parallel-block-down-neighbor-failure-${rank}-rank" "${rank}"
        "neighborhood block-down failures preserve state and retry"
    )
endforeach()
add_parallel_block_down_mpi_test(
    unit-parallel-block-down-local-domain-2-rank 2
    "block-down rejects a rank-local block equal to k before topology"
)
add_parallel_block_down_mpi_test(
    unit-parallel-block-down-skewed-k-2-rank 2
    "block-down rejects rank-skewed k before topology"
)
add_parallel_block_down_mpi_test(
    unit-parallel-block-down-zero-k-2-rank 2
    "block-down rejects zero k before topology"
)
add_parallel_block_down_mpi_test(
    unit-parallel-block-down-communicator-2-rank 2
    "block-down accepts congruent and rejects similar communicators"
)
add_parallel_block_down_mpi_test(
    unit-parallel-block-down-coarse-domain-3-rank 3
    "block-down rejects skewed coarse domains before topology"
)
add_parallel_block_down_mpi_test(
    unit-parallel-block-down-ownership-2-rank 2
    "block-down rejects skewed ownership metadata before topology"
)
add_parallel_block_down_mpi_test(
    unit-parallel-block-down-asymmetric-2-rank 2
    "block-down rejects asymmetric ghost topology before dense payload"
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
    unit-parallel-vcycle-complete-graph-distribution-2-rank 2
    "complete-graph distribution preserves root vcycle blocks while replicating structure"
)

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

add_test(
    NAME unit-mpi-semantic-exit-policy
    COMMAND
        "${CMAKE_COMMAND}"
        "-DCOMMUNICATION_ROOT=${CMAKE_CURRENT_SOURCE_DIR}/../lib/communication"
        -P
        "${CMAKE_CURRENT_SOURCE_DIR}/communication/verify_mpi_semantic_exit_policy.cmake"
)
set_tests_properties(unit-mpi-semantic-exit-policy PROPERTIES TIMEOUT 5)

add_executable(
    mpi_failure_policy_probe
    communication/mpi_failure_policy_probe.cpp
)
target_link_libraries(
    mpi_failure_policy_probe
    PRIVATE parallel kahip_options kahip_warnings
)

add_test(
    NAME unit-mpi-failure-policy-pre-init-error
    COMMAND
        "${CMAKE_COMMAND}"
        "-DPROBE=$<TARGET_FILE:mpi_failure_policy_probe>"
        "-DMODE=pre-init-error"
        "-DEXPECT_FAILURE=FALSE"
        "-DEXPECTED_MARKER=pre-init mpi_error remained MPI-free"
        -P
        "${CMAKE_CURRENT_SOURCE_DIR}/communication/verify_mpi_fail_fast_probe.cmake"
)
set_tests_properties(
    unit-mpi-failure-policy-pre-init-error
    PROPERTIES RUN_SERIAL TRUE TIMEOUT 10
)

function(
    add_mpi_failure_policy_death_test
    test_suffix
    mode
    expected_diagnostic
    expected_detail
    expected_injection
    expected_marker
)
    set(mpi_ranks 2)
    if(ARGC GREATER 6)
        set(mpi_ranks "${ARGV6}")
    endif()
    add_test(
        NAME unit-mpi-failure-policy-${test_suffix}
        COMMAND
            "${CMAKE_COMMAND}"
            "-DMPIEXEC_EXECUTABLE=${MPIEXEC_EXECUTABLE}"
            "-DMPIEXEC_NUMPROC_FLAG=${MPIEXEC_NUMPROC_FLAG}"
            "-DMPIEXEC_PREFLAGS=${MPIEXEC_PREFLAGS}"
            "-DMPIEXEC_POSTFLAGS=${MPIEXEC_POSTFLAGS}"
            "-DMPI_RANKS=${mpi_ranks}"
            "-DPROBE=$<TARGET_FILE:mpi_failure_policy_probe>"
            "-DMODE=${mode}"
            "-DEXPECT_FAILURE=TRUE"
            "-DEXPECTED_DIAGNOSTIC=${expected_diagnostic}"
            "-DEXPECTED_DETAIL=${expected_detail}"
            "-DEXPECTED_INJECTION=${expected_injection}"
            "-DEXPECTED_MARKER=${expected_marker}"
            -P
            "${CMAKE_CURRENT_SOURCE_DIR}/communication/verify_mpi_fail_fast_probe.cmake"
    )
    set_tests_properties(
        unit-mpi-failure-policy-${test_suffix}
        PROPERTIES PROCESSORS ${mpi_ranks} RUN_SERIAL TRUE TIMEOUT 10
    )
endfunction()

add_mpi_failure_policy_death_test(
    semantic-factory-resource-failure
    semantic-factory-resource
    "MPI semantic error construction:"
    "std::bad_alloc"
    "injected semantic factory bad_alloc"
    "affected=semantic error-string-attempts=0 cleanup-attempts=0"
)
add_mpi_failure_policy_death_test(
    error-string-secondary-failure
    error-string-secondary
    "MPI backend failure: backend formatter failure"
    "original raw code 17291, MPI_Error_string secondary raw code 17292"
    "injected MPI_Error_string failure original=17291 secondary=17292"
    "affected=backend error-string-attempts=1 cleanup-attempts=0"
)
add_mpi_failure_policy_death_test(
    wrong-topology-failure
    wrong-topology
    "MPI adapter programming failure: topology requires an MPI topology communicator"
    ""
    "captured wrong-topology internal duplicate"
    "affected=topology error-string-attempts=0 cleanup-attempts=0"
)
add_mpi_failure_policy_death_test(
    capacity-resolver-failure
    capacity-resolver
    "MPI adapter capacity failure:"
    "capacity resolver probe: cumulative element offset exceeds local size_t capacity"
    "injected rank-zero fatal capacity issue"
    "affected=capacity error-string-attempts=0 cleanup-attempts=0 capacity-allreduce-attempts=1"
)
add_mpi_failure_policy_death_test(
    dense-receive-offset-capacity-failure
    dense-receive-offset-capacity
    "MPI adapter capacity failure:"
    "all_to_all_v: cumulative element offset exceeds local size_t capacity"
    "injected rank-zero dense receive offset capacity"
    "affected=dense-operation error-string-attempts=0 cleanup-attempts=0 capacity-allreduce-attempts=1 dense-count-exchange-attempts=1 dense-payload-attempts=0 dense-datatype-attempts=0"
)

add_mpi_failure_policy_death_test(
    dense-receive-byte-capacity-failure
    dense-receive-byte-capacity
    "MPI adapter capacity failure:"
    "all_to_all_v: element storage byte size exceeds local size_t capacity"
    "injected rank-zero dense receive byte capacity"
    "affected=dense-operation error-string-attempts=0 cleanup-attempts=0 capacity-allreduce-attempts=1 dense-count-exchange-attempts=1 dense-payload-attempts=0 dense-datatype-attempts=0"
)

foreach(mpi_ranks RANGE 1 5)
    add_mpi_failure_policy_death_test(
        distributed-graph-degree-capacity-failure-${mpi_ranks}-rank
        distributed-graph-degree-capacity
        "MPI adapter capacity failure:"
        "distributed graph construction: distributed graph outdegree exceeds MPI int capacity"
        "injected rank-zero distributed graph degree capacity"
        "affected=world error-string-attempts=0 cleanup-attempts=0 capacity-allreduce-attempts=1 dense-count-exchange-attempts=0 dense-payload-attempts=0 dense-datatype-attempts=0 graph-semantic-validation-attempts=1 graph-create-attempts=0"
        ${mpi_ranks}
    )
endforeach()

add_mpi_failure_policy_death_test(
    neighbor-receive-offset-capacity-failure
    neighbor-receive-offset-capacity
    "MPI adapter capacity failure:"
    "neighbor_all_to_all_v: cumulative element offset exceeds local size_t capacity"
    "injected rank-zero neighbor receive offset capacity"
    "affected=neighbor-graph error-string-attempts=0 cleanup-attempts=0 capacity-allreduce-attempts=1 dense-count-exchange-attempts=0 dense-payload-attempts=0 dense-datatype-attempts=0 graph-semantic-validation-attempts=0 graph-create-attempts=0 neighbor-count-exchange-attempts=1 neighbor-payload-attempts=0 neighbor-datatype-attempts=0 neighbor-phase-round-attempts=0 neighbor-capacity-after-injection-attempts=1"
)

add_mpi_failure_policy_death_test(
    neighbor-receive-byte-capacity-failure
    neighbor-receive-byte-capacity
    "MPI adapter capacity failure:"
    "neighbor_all_to_all_v: element storage byte size exceeds local size_t capacity"
    "injected rank-zero neighbor receive byte capacity"
    "affected=neighbor-graph error-string-attempts=0 cleanup-attempts=0 capacity-allreduce-attempts=1 dense-count-exchange-attempts=0 dense-payload-attempts=0 dense-datatype-attempts=0 graph-semantic-validation-attempts=0 graph-create-attempts=0 neighbor-count-exchange-attempts=1 neighbor-payload-attempts=0 neighbor-datatype-attempts=0 neighbor-phase-round-attempts=0 neighbor-capacity-after-injection-attempts=1"
)

add_mpi_failure_policy_death_test(
    neighbor-bounded-round-arithmetic-failure
    neighbor-bounded-round-arithmetic
    "MPI adapter capacity failure:"
    "neighbor_all_to_all_v bounded MPI-3 plan: bounded MPI-3 chunk arithmetic exceeds local size_t capacity"
    "injected rank-zero bounded neighbor round arithmetic capacity"
    "affected=neighbor-graph error-string-attempts=0 cleanup-attempts=0 capacity-allreduce-attempts=2 dense-count-exchange-attempts=0 dense-payload-attempts=0 dense-datatype-attempts=0 graph-semantic-validation-attempts=0 graph-create-attempts=0 neighbor-count-exchange-attempts=1 neighbor-payload-attempts=0 neighbor-datatype-attempts=0 neighbor-phase-round-attempts=2 neighbor-capacity-after-injection-attempts=1"
)

add_executable(
    mpi_async_capacity_failure_probe
    communication/mpi_async_capacity_failure_probe.cpp
)
target_link_libraries(
    mpi_async_capacity_failure_probe
    PRIVATE parallel kahip_options kahip_warnings
)

function(add_mpi_async_capacity_death_test mode expected_detail expected_injection)
    add_test(
        NAME unit-mpi-async-capacity-${mode}-failure
        COMMAND
            "${CMAKE_COMMAND}"
            "-DMPIEXEC_EXECUTABLE=${MPIEXEC_EXECUTABLE}"
            "-DMPIEXEC_NUMPROC_FLAG=${MPIEXEC_NUMPROC_FLAG}"
            "-DMPIEXEC_PREFLAGS=${MPIEXEC_PREFLAGS}"
            "-DMPIEXEC_POSTFLAGS=${MPIEXEC_POSTFLAGS}"
            "-DMPI_RANKS=2"
            "-DPROBE=$<TARGET_FILE:mpi_async_capacity_failure_probe>"
            "-DMODE=${mode}"
            "-DEXPECT_FAILURE=TRUE"
            "-DEXPECTED_DIAGNOSTIC=MPI adapter capacity failure:"
            "-DEXPECTED_DETAIL=direct neighborhood exchange: ${expected_detail}"
            "-DEXPECTED_INJECTION=${expected_injection}"
            "-DEXPECTED_MARKER=affected=async-operation error-string-attempts=0 cleanup-attempts=0 capacity-bor-attempts=1 backend-band-attempts=1 count-exchange-attempts=1 payload-allocation-attempts=0 datatype-attempts=0 immediate-init-attempts=0 persistent-init-attempts=0 payload-collective-attempts=0 operation-duplicate-attempts=1"
            -P
            "${CMAKE_CURRENT_SOURCE_DIR}/communication/verify_mpi_fail_fast_probe.cmake"
    )
    set_tests_properties(
        unit-mpi-async-capacity-${mode}-failure
        PROPERTIES PROCESSORS 2 RUN_SERIAL TRUE TIMEOUT 10
    )
endfunction()

add_mpi_async_capacity_death_test(
    one-shot-receive-offset
    "cumulative element offset exceeds local size_t capacity"
    "injected rank-zero async receive offset capacity"
)
add_mpi_async_capacity_death_test(
    one-shot-receive-byte
    "element storage byte size exceeds local size_t capacity"
    "injected rank-zero async receive byte capacity"
)
add_mpi_async_capacity_death_test(
    fixed-send-offset
    "cumulative element offset exceeds local size_t capacity"
    "armed rank-zero async fixed-send offset capacity"
)
add_mpi_async_capacity_death_test(
    fixed-send-byte
    "element storage byte size exceeds local size_t capacity"
    "armed rank-zero async fixed-send byte capacity"
)
add_mpi_async_capacity_death_test(
    fixed-direct-backend
    "direct neighborhood payload has no representable MPI backend"
    "armed rank-zero async direct backend capacity"
)

add_executable(
    parallel_graph_consistency_mpi_test
    distributed_consistency/parallel_graph_consistency_mpi_test.cpp
)
target_link_libraries(
    parallel_graph_consistency_mpi_test
    PRIVATE
        catch_mpi_runner
        parallel
        kahip_options
        kahip_warnings
)

foreach(mpi_ranks RANGE 1 5)
    add_test(
        NAME unit-parallel-graph-consistency-${mpi_ranks}-rank
        COMMAND
            "${MPIEXEC_EXECUTABLE}" "${MPIEXEC_NUMPROC_FLAG}" ${mpi_ranks}
            ${MPIEXEC_PREFLAGS}
            $<TARGET_FILE:parallel_graph_consistency_mpi_test>
            ${MPIEXEC_POSTFLAGS}
    )
    set_tests_properties(
        unit-parallel-graph-consistency-${mpi_ranks}-rank
        PROPERTIES
            PROCESSORS ${mpi_ranks}
            RUN_SERIAL TRUE
            TIMEOUT 20
    )
endforeach()

add_executable(
    ghost_label_exchange_mpi_test
    distributed_consistency/ghost_label_exchange_mpi_test.cpp
)
target_link_libraries(
    ghost_label_exchange_mpi_test
    PRIVATE
        catch_mpi_runner
        parallel
        kahip_options
        kahip_warnings
)

foreach(mpi_ranks RANGE 1 5)
    add_test(
        NAME unit-ghost-label-exchange-${mpi_ranks}-rank
        COMMAND
            "${MPIEXEC_EXECUTABLE}" "${MPIEXEC_NUMPROC_FLAG}" ${mpi_ranks}
            ${MPIEXEC_PREFLAGS}
            $<TARGET_FILE:ghost_label_exchange_mpi_test>
            ${MPIEXEC_POSTFLAGS}
    )
    set_tests_properties(
        unit-ghost-label-exchange-${mpi_ranks}-rank
        PROPERTIES
            PROCESSORS ${mpi_ranks}
            RUN_SERIAL TRUE
            TIMEOUT 25
    )
endforeach()

add_executable(
    ghost_label_failure_probe
    distributed_consistency/ghost_label_failure_probe.cpp
)
target_link_libraries(
    ghost_label_failure_probe
    PRIVATE parallel kahip_options kahip_warnings
)

function(
    add_ghost_label_failure_test
    mode
    expected_diagnostic
    expected_abort_marker
)
    add_test(
        NAME unit-ghost-label-${mode}-failure
        COMMAND
            "${CMAKE_COMMAND}"
            "-DMPIEXEC_EXECUTABLE=${MPIEXEC_EXECUTABLE}"
            "-DMPIEXEC_NUMPROC_FLAG=${MPIEXEC_NUMPROC_FLAG}"
            "-DMPIEXEC_PREFLAGS=${MPIEXEC_PREFLAGS}"
            "-DMPIEXEC_POSTFLAGS=${MPIEXEC_POSTFLAGS}"
            "-DPROBE=$<TARGET_FILE:ghost_label_failure_probe>"
            "-DMODE=${mode}"
            "-DEXPECTED_DIAGNOSTIC=${expected_diagnostic}"
            "-DEXPECTED_ABORT_MARKER=${expected_abort_marker}"
            -P
            "${CMAKE_CURRENT_SOURCE_DIR}/distributed_consistency/verify_ghost_label_failure.cmake"
    )
    set_tests_properties(
        unit-ghost-label-${mode}-failure
        PROPERTIES PROCESSORS 2 RUN_SERIAL TRUE TIMEOUT 12
    )
endfunction()

add_ghost_label_failure_test(
    active-incremental-then-global
    "MPI adapter programming failure: global ghost label exchange requires no active incremental exchange"
    "observed active-incremental/global MPI_Abort before blocking payload"
)
add_ghost_label_failure_test(
    skewed-incremental-protocol
    "MPI adapter programming failure: ghost label incremental protocol diverged across ranks"
    "observed skewed incremental-protocol MPI_Abort before first payload"
)
add_ghost_label_failure_test(
    corrupted-incremental-completion
    "MPI adapter programming failure: incremental ghost label receive validation failed after payload completion"
    "observed corrupted incremental-completion terminal MPI_Abort"
)

add_executable(
    population_size_broadcast_mpi_test
    communication/population_size_broadcast_mpi_test.cpp
)
target_sources(
    population_size_broadcast_mpi_test
    PRIVATE
        FILE_SET shared_population_size_headers
        TYPE HEADERS
        BASE_DIRS "${PROJECT_SOURCE_DIR}/lib"
        FILES
            "${PROJECT_SOURCE_DIR}/lib/parallel_mh/population_size_broadcast.h"
)
target_link_libraries(
    population_size_broadcast_mpi_test
    PRIVATE
        catch_mpi_runner
        MPI::MPI_CXX
        spdlog::spdlog
        kahip_options
        kahip_warnings
)
add_test(
    NAME unit-pmpi-callback-safety-population-size
    COMMAND
        "${CMAKE_COMMAND}"
        "-DSOURCE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/communication/population_size_broadcast_mpi_test.cpp"
        "-DPROFILE=population-size"
        -P
        "${CMAKE_CURRENT_SOURCE_DIR}/communication/verify_pmpi_callback_safety.cmake"
)
set_tests_properties(
    unit-pmpi-callback-safety-population-size
    PROPERTIES TIMEOUT 5
)
foreach(mpi_ranks RANGE 1 5)
    add_test(
        NAME unit-population-size-broadcast-${mpi_ranks}-rank
        COMMAND
            "${MPIEXEC_EXECUTABLE}" "${MPIEXEC_NUMPROC_FLAG}" ${mpi_ranks}
            ${MPIEXEC_PREFLAGS}
            $<TARGET_FILE:population_size_broadcast_mpi_test>
            ${MPIEXEC_POSTFLAGS}
    )
    set_tests_properties(
        unit-population-size-broadcast-${mpi_ranks}-rank
        PROPERTIES
            PROCESSORS ${mpi_ranks}
            RUN_SERIAL TRUE
            TIMEOUT 10
    )
endforeach()

add_executable(
    population_size_broadcast_failure_probe
    communication/population_size_broadcast_failure_probe.cpp
)
target_sources(
    population_size_broadcast_failure_probe
    PRIVATE
        FILE_SET shared_population_size_headers
        TYPE HEADERS
        BASE_DIRS "${PROJECT_SOURCE_DIR}/lib"
        FILES
            "${PROJECT_SOURCE_DIR}/lib/parallel_mh/population_size_broadcast.h"
)
target_link_libraries(
    population_size_broadcast_failure_probe
    PRIVATE MPI::MPI_CXX spdlog::spdlog kahip_options kahip_warnings
)
add_test(
    NAME unit-pmpi-callback-safety-population-size-failure
    COMMAND
        "${CMAKE_COMMAND}"
        "-DSOURCE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/communication/population_size_broadcast_failure_probe.cpp"
        "-DPROFILE=population-size-failure"
        -P
        "${CMAKE_CURRENT_SOURCE_DIR}/communication/verify_pmpi_callback_safety.cmake"
)
set_tests_properties(
    unit-pmpi-callback-safety-population-size-failure
    PROPERTIES TIMEOUT 5
)
add_test(
    NAME unit-population-size-broadcast-failure
    COMMAND
        "${CMAKE_COMMAND}"
        "-DMPIEXEC_EXECUTABLE=${MPIEXEC_EXECUTABLE}"
        "-DMPIEXEC_NUMPROC_FLAG=${MPIEXEC_NUMPROC_FLAG}"
        "-DMPIEXEC_PREFLAGS=${MPIEXEC_PREFLAGS}"
        "-DMPIEXEC_POSTFLAGS=${MPIEXEC_POSTFLAGS}"
        "-DPROBE=$<TARGET_FILE:population_size_broadcast_failure_probe>"
        -P
        "${CMAKE_CURRENT_SOURCE_DIR}/communication/verify_population_size_broadcast_failure.cmake"
)
set_tests_properties(
    unit-population-size-broadcast-failure
    PROPERTIES PROCESSORS 2 RUN_SERIAL TRUE TIMEOUT 12
)

add_executable(
    evolutionary_collectives_mpi_test
    communication/evolutionary_collectives_mpi_test.cpp
)
target_sources(
    evolutionary_collectives_mpi_test
    PRIVATE
        FILE_SET shared_evolutionary_headers
        TYPE HEADERS
        BASE_DIRS "${PROJECT_SOURCE_DIR}/lib"
        FILES
            "${PROJECT_SOURCE_DIR}/lib/parallel_mh/evolutionary_collectives.h"
)
target_link_libraries(
    evolutionary_collectives_mpi_test
    PRIVATE
        catch_mpi_runner
        MPI::MPI_CXX
        spdlog::spdlog
        kahip_options
        kahip_warnings
)
add_test(
    NAME unit-pmpi-callback-safety-evolutionary
    COMMAND
        "${CMAKE_COMMAND}"
        "-DSOURCE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/communication/evolutionary_collectives_mpi_test.cpp"
        "-DPROFILE=evolutionary"
        -P
        "${CMAKE_CURRENT_SOURCE_DIR}/communication/verify_pmpi_callback_safety.cmake"
)
set_tests_properties(
    unit-pmpi-callback-safety-evolutionary
    PROPERTIES TIMEOUT 5
)
foreach(mpi_ranks RANGE 1 5)
    add_test(
        NAME unit-evolutionary-collectives-${mpi_ranks}-rank
        COMMAND
            "${MPIEXEC_EXECUTABLE}" "${MPIEXEC_NUMPROC_FLAG}" ${mpi_ranks}
            ${MPIEXEC_PREFLAGS}
            $<TARGET_FILE:evolutionary_collectives_mpi_test>
            ${MPIEXEC_POSTFLAGS}
    )
    set_tests_properties(
        unit-evolutionary-collectives-${mpi_ranks}-rank
        PROPERTIES PROCESSORS ${mpi_ranks} RUN_SERIAL TRUE TIMEOUT 12
    )
endforeach()

add_executable(
    evolutionary_collectives_failure_probe
    communication/evolutionary_collectives_failure_probe.cpp
)
target_sources(
    evolutionary_collectives_failure_probe
    PRIVATE
        FILE_SET shared_evolutionary_headers
        TYPE HEADERS
        BASE_DIRS "${PROJECT_SOURCE_DIR}/lib"
        FILES
            "${PROJECT_SOURCE_DIR}/lib/parallel_mh/evolutionary_collectives.h"
)
target_link_libraries(
    evolutionary_collectives_failure_probe
    PRIVATE MPI::MPI_CXX spdlog::spdlog kahip_options kahip_warnings
)
add_test(
    NAME unit-pmpi-callback-safety-evolutionary-failure
    COMMAND
        "${CMAKE_COMMAND}"
        "-DSOURCE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/communication/evolutionary_collectives_failure_probe.cpp"
        "-DPROFILE=evolutionary-failure"
        -P
        "${CMAKE_CURRENT_SOURCE_DIR}/communication/verify_pmpi_callback_safety.cmake"
)
set_tests_properties(
    unit-pmpi-callback-safety-evolutionary-failure
    PROPERTIES TIMEOUT 5
)
foreach(
    failure_mode
    IN ITEMS
        permutation
        rank
        communicator-size
        feasibility
        objective
        weight
        root
        signature-minimum
        signature-maximum
        signature-validity
        payload
        count-mismatch
        null-payload
)
    if(failure_mode STREQUAL "permutation")
        set(
            expected_diagnostic
            "MPI backend failure: MPI_Bcast(evolutionary permutation)"
        )
    elseif(failure_mode STREQUAL "rank")
        set(
            expected_diagnostic
            "MPI backend failure: MPI_Comm_rank(evolutionary best partition)"
        )
    elseif(failure_mode STREQUAL "communicator-size")
        set(
            expected_diagnostic
            "MPI backend failure: MPI_Comm_size(evolutionary partition broadcast)"
        )
    elseif(failure_mode STREQUAL "feasibility")
        set(
            expected_diagnostic
            "MPI backend failure: MPI_Allreduce(evolutionary feasibility)"
        )
    elseif(failure_mode STREQUAL "objective")
        set(
            expected_diagnostic
            "MPI backend failure: MPI_Allreduce(evolutionary objective)"
        )
    elseif(failure_mode STREQUAL "weight")
        set(
            expected_diagnostic
            "MPI backend failure: MPI_Allreduce(evolutionary maximum block weight)"
        )
    elseif(failure_mode STREQUAL "root")
        set(
            expected_diagnostic
            "MPI backend failure: MPI_Allreduce(evolutionary broadcaster rank)"
        )
    elseif(failure_mode STREQUAL "signature-minimum")
        set(
            expected_diagnostic
            "MPI backend failure: MPI_Allreduce(evolutionary partition signature minimum)"
        )
    elseif(failure_mode STREQUAL "signature-maximum")
        set(
            expected_diagnostic
            "MPI backend failure: MPI_Allreduce(evolutionary partition signature maximum)"
        )
    elseif(failure_mode STREQUAL "signature-validity")
        set(
            expected_diagnostic
            "MPI backend failure: MPI_Allreduce(evolutionary partition signature validity)"
        )
    elseif(failure_mode STREQUAL "payload")
        if(KAHIP_HAVE_MPI_BCAST_C)
            set(
                expected_diagnostic
                "MPI backend failure: MPI_Bcast_c(evolutionary best partition)"
            )
        else()
            set(
                expected_diagnostic
                "MPI backend failure: MPI_Bcast(evolutionary best partition MPI-3 round)"
            )
        endif()
    elseif(failure_mode STREQUAL "count-mismatch")
        set(
            expected_diagnostic
            "partition broadcast arguments differ across communicator"
        )
    else()
        set(
            expected_diagnostic
            "partition broadcast arguments are invalid"
        )
    endif()
    add_test(
        NAME unit-evolutionary-collectives-${failure_mode}-failure
        COMMAND
            "${CMAKE_COMMAND}"
            "-DMPIEXEC_EXECUTABLE=${MPIEXEC_EXECUTABLE}"
            "-DMPIEXEC_NUMPROC_FLAG=${MPIEXEC_NUMPROC_FLAG}"
            "-DMPIEXEC_PREFLAGS=${MPIEXEC_PREFLAGS}"
            "-DMPIEXEC_POSTFLAGS=${MPIEXEC_POSTFLAGS}"
            "-DPROBE=$<TARGET_FILE:evolutionary_collectives_failure_probe>"
            "-DMODE=${failure_mode}"
            "-DEXPECTED_DIAGNOSTIC=${expected_diagnostic}"
            -P
            "${CMAKE_CURRENT_SOURCE_DIR}/communication/verify_evolutionary_collectives_failure.cmake"
    )
    set_tests_properties(
        unit-evolutionary-collectives-${failure_mode}-failure
        PROPERTIES PROCESSORS 2 RUN_SERIAL TRUE TIMEOUT 12
    )
endforeach()

add_executable(
    parallel_graph_lifecycle_probe
    distributed_consistency/parallel_graph_lifecycle_probe.cpp
)
target_link_libraries(
    parallel_graph_lifecycle_probe
    PRIVATE parallel kahip_options kahip_warnings
)
foreach(
    graph_lifecycle
    IN ITEMS
        no-plan
        cached-plan
        active-destructor
        active-reset
        cnode-size-mismatch
)
    add_test(
        NAME unit-parallel-graph-lifecycle-${graph_lifecycle}
        COMMAND
            "${CMAKE_COMMAND}"
            "-DPROBE=$<TARGET_FILE:parallel_graph_lifecycle_probe>"
            "-DMODE=${graph_lifecycle}"
            -P
            "${CMAKE_CURRENT_SOURCE_DIR}/distributed_consistency/verify_parallel_graph_lifecycle.cmake"
    )
    set_tests_properties(
        unit-parallel-graph-lifecycle-${graph_lifecycle}
        PROPERTIES RUN_SERIAL TRUE TIMEOUT 10
    )
endforeach()

add_executable(
    distributed_consistency_mpi_test
    distributed_consistency/distributed_consistency_mpi_test.cpp
)
target_link_libraries(
    distributed_consistency_mpi_test
    PRIVATE
        catch_mpi_runner
        parallel
        kahip_options
        kahip_warnings
)

foreach(mpi_ranks RANGE 1 5)
    add_test(
        NAME unit-distributed-consistency-${mpi_ranks}-rank
        COMMAND
            "${MPIEXEC_EXECUTABLE}" "${MPIEXEC_NUMPROC_FLAG}" ${mpi_ranks}
            ${MPIEXEC_PREFLAGS}
            $<TARGET_FILE:distributed_consistency_mpi_test>
            ${MPIEXEC_POSTFLAGS}
    )
    set_tests_properties(
        unit-distributed-consistency-${mpi_ranks}-rank
        PROPERTIES
            PROCESSORS ${mpi_ranks}
            RUN_SERIAL TRUE
            TIMEOUT 25
    )
endforeach()

add_executable(
    mpi_async_neighbors_test
    communication/mpi_async_neighbors_test.cpp
)
target_link_libraries(
    mpi_async_neighbors_test
    PRIVATE
        catch_mpi_runner
        parallel
        kahip_options
        kahip_warnings
)

foreach(mpi_ranks RANGE 1 5)
    add_test(
        NAME unit-mpi-async-neighbors-${mpi_ranks}-rank
        COMMAND
            "${MPIEXEC_EXECUTABLE}" "${MPIEXEC_NUMPROC_FLAG}" ${mpi_ranks}
            ${MPIEXEC_PREFLAGS}
            $<TARGET_FILE:mpi_async_neighbors_test>
            ${MPIEXEC_POSTFLAGS}
    )
    set_tests_properties(
        unit-mpi-async-neighbors-${mpi_ranks}-rank
        PROPERTIES
            PROCESSORS ${mpi_ranks}
            RUN_SERIAL TRUE
            TIMEOUT 15
    )
endforeach()

add_executable(
    mpi_async_neighbors_failure_probe
    communication/mpi_async_neighbors_failure_probe.cpp
)
target_link_libraries(
    mpi_async_neighbors_failure_probe
    PRIVATE parallel kahip_options kahip_warnings
)

function(
    add_mpi_async_neighbors_failure_test
    mode
    abort_kind
    expected_diagnostic
    expect_injected_mpi_error
)
    add_test(
        NAME unit-mpi-async-neighbors-${mode}-failure
        COMMAND
            "${CMAKE_COMMAND}"
            "-DMPIEXEC_EXECUTABLE=${MPIEXEC_EXECUTABLE}"
            "-DMPIEXEC_NUMPROC_FLAG=${MPIEXEC_NUMPROC_FLAG}"
            "-DMPIEXEC_PREFLAGS=${MPIEXEC_PREFLAGS}"
            "-DMPIEXEC_POSTFLAGS=${MPIEXEC_POSTFLAGS}"
            "-DPROBE=$<TARGET_FILE:mpi_async_neighbors_failure_probe>"
            "-DMODE=${mode}"
            "-DABORT_KIND=${abort_kind}"
            "-DEXPECTED_DIAGNOSTIC=${expected_diagnostic}"
            "-DEXPECT_INJECTED_MPI_ERROR=${expect_injected_mpi_error}"
            -P
            "${CMAKE_CURRENT_SOURCE_DIR}/communication/verify_mpi_async_neighbors_failure.cmake"
    )
    set_tests_properties(
        unit-mpi-async-neighbors-${mode}-failure
        PROPERTIES
            RUN_SERIAL TRUE
            TIMEOUT 12
    )
endfunction()

add_mpi_async_neighbors_failure_test(
    immediate-init
    mpi
    "MPI backend failure: MPI_Ineighbor_alltoallv"
    TRUE
)
add_mpi_async_neighbors_failure_test(
    test
    mpi
    "MPI backend failure: MPI_Test(neighborhood exchange)"
    TRUE
)
add_mpi_async_neighbors_failure_test(
    wait
    mpi
    "MPI backend failure: MPI_Wait(neighborhood exchange)"
    TRUE
)
add_mpi_async_neighbors_failure_test(
    destructor-wait
    mpi
    "MPI backend failure: MPI_Wait(active neighborhood destruction)"
    TRUE
)
add_mpi_async_neighbors_failure_test(
    active-restart
    mpi
    "MPI adapter programming failure: neighbor context start requires an inactive generation"
    FALSE
)
add_mpi_async_neighbors_failure_test(
    inactive-test
    mpi
    "MPI adapter programming failure: neighbor context test requires an active generation"
    FALSE
)
add_mpi_async_neighbors_failure_test(
    send-while-active
    mpi
    "MPI adapter programming failure: neighbor context send mutation requires an inactive generation"
    FALSE
)
add_mpi_async_neighbors_failure_test(
    receive-while-active
    mpi
    "MPI adapter programming failure: neighbor context receive access requires a completed generation"
    FALSE
)

if(
    KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_INIT
    OR KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_INIT_C
)
    add_mpi_async_neighbors_failure_test(
        persistent-init
        mpi
        "MPI backend failure: MPI_Neighbor_alltoallv_init"
        TRUE
    )
    add_mpi_async_neighbors_failure_test(
        persistent-start
        mpi
        "MPI backend failure: MPI_Start(persistent neighborhood exchange)"
        TRUE
    )
    add_mpi_async_neighbors_failure_test(
        persistent-test
        mpi
        "MPI backend failure: MPI_Test(neighborhood exchange)"
        TRUE
    )
    add_mpi_async_neighbors_failure_test(
        persistent-wait
        mpi
        "MPI backend failure: MPI_Wait(neighborhood exchange)"
        TRUE
    )
    add_mpi_async_neighbors_failure_test(
        request-free
        mpi
        "MPI backend failure: MPI_Request_free(persistent neighborhood exchange)"
        TRUE
    )
endif()

foreach(
    post_finalize_mode
    IN ITEMS
        post-finalize-active-request
        post-finalize-immediate-context
)
    add_mpi_async_neighbors_failure_test(
        ${post_finalize_mode}
        raw
        "MPI adapter ownership outlived the active MPI runtime: neighborhood operation destruction"
        FALSE
    )
endforeach()
add_mpi_async_neighbors_failure_test(
    post-finalize-complete-request
    raw
    "MPI adapter ownership outlived the active MPI runtime: one-shot neighborhood test"
    FALSE
)
add_mpi_async_neighbors_failure_test(
    post-finalize-complete-wait
    raw
    "MPI adapter ownership outlived the active MPI runtime: one-shot neighborhood wait"
    FALSE
)
if(
    KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_INIT
    OR KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_INIT_C
)
    add_mpi_async_neighbors_failure_test(
        post-finalize-persistent-context
        raw
        "MPI adapter ownership outlived the active MPI runtime: neighborhood operation destruction"
        FALSE
    )
endif()
add_mpi_async_neighbors_failure_test(
    initialized-query
    raw
    "MPI lifecycle query failure: MPI_Initialized returned raw error 17295"
    FALSE
)
add_mpi_async_neighbors_failure_test(
    finalized-query
    raw
    "MPI lifecycle query failure: MPI_Finalized returned raw error 17295"
    FALSE
)

add_executable(
    mpi_neighborhood_failure_probe
    communication/mpi_neighborhood_failure_probe.cpp
)
target_link_libraries(
    mpi_neighborhood_failure_probe
    PRIVATE parallel kahip_options kahip_warnings
)

foreach(neighborhood_failure IN ITEMS create query free)
    if(neighborhood_failure STREQUAL "create")
        set(neighborhood_failure_context "MPI_Dist_graph_create")
        set(neighborhood_failure_affected "internal")
    elseif(neighborhood_failure STREQUAL "query")
        set(neighborhood_failure_context "MPI_Dist_graph_neighbors_count")
        set(neighborhood_failure_affected "graph")
    else()
        set(neighborhood_failure_context "MPI_Comm_free\\(distributed graph\\)")
        set(neighborhood_failure_affected "world")
    endif()
    add_test(
        NAME unit-mpi-neighborhood-${neighborhood_failure}-failure
        COMMAND
            "${CMAKE_COMMAND}"
            "-DMPIEXEC_EXECUTABLE=${MPIEXEC_EXECUTABLE}"
            "-DMPIEXEC_NUMPROC_FLAG=${MPIEXEC_NUMPROC_FLAG}"
            "-DMPIEXEC_PREFLAGS=${MPIEXEC_PREFLAGS}"
            "-DMPIEXEC_POSTFLAGS=${MPIEXEC_POSTFLAGS}"
            "-DPROBE=$<TARGET_FILE:mpi_neighborhood_failure_probe>"
            "-DMODE=${neighborhood_failure}"
            "-DEXPECTED_CONTEXT=${neighborhood_failure_context}"
            "-DEXPECTED_AFFECTED=${neighborhood_failure_affected}"
            -P
            "${CMAKE_CURRENT_SOURCE_DIR}/communication/verify_mpi_neighborhood_failure.cmake"
    )
    set_tests_properties(
        unit-mpi-neighborhood-${neighborhood_failure}-failure
        PROPERTIES
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
            "-DEXPECTED_ERROR_CODE=17293"
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
