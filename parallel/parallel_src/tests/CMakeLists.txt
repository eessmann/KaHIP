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

add_executable(
    bipartition_candidate_test
    initial_partitioning/bipartition_candidate_test.cpp
)
target_sources(
    bipartition_candidate_test
    PRIVATE
        FILE_SET bipartition_candidate_headers
        TYPE HEADERS
        BASE_DIRS "${PROJECT_SOURCE_DIR}/lib"
        FILES
            "${PROJECT_SOURCE_DIR}/lib/partition/initial_partitioning/bipartition_candidate.h"
)
target_link_libraries(
    bipartition_candidate_test
    PRIVATE kahip_options kahip_warnings
)
add_test(
    NAME unit-bipartition-candidate-ordering
    COMMAND $<TARGET_FILE:bipartition_candidate_test>
)
set_tests_properties(
    unit-bipartition-candidate-ordering
    PROPERTIES TIMEOUT 5 LABELS "unit;initial-partitioning;mathematics"
)

add_executable(
    root_bipartition_invariant_test
    initial_partitioning/root_bipartition_invariant_test.cpp
)
target_sources(
    root_bipartition_invariant_test
    PRIVATE
        FILE_SET bipartition_invariant_headers
        TYPE HEADERS
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/initial_partitioning"
        FILES initial_partitioning/bipartition_invariant_cases.h
)
target_link_libraries(
    root_bipartition_invariant_test
    PRIVATE
        Catch2::Catch2WithMain
        kahip_core_obj
        kahip_options
        kahip_warnings
)
catch_discover_tests(
    root_bipartition_invariant_test
    TEST_PREFIX "unit-"
    OUTPUT_DIR .
    OUTPUT_PREFIX "unit-"
)

add_executable(
    modified_bipartition_invariant_test
    initial_partitioning/modified_bipartition_invariant_test.cpp
)
target_sources(
    modified_bipartition_invariant_test
    PRIVATE
        FILE_SET bipartition_invariant_headers
        TYPE HEADERS
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/initial_partitioning"
        FILES initial_partitioning/bipartition_invariant_cases.h
)
target_link_libraries(
    modified_bipartition_invariant_test
    PRIVATE
        Catch2::Catch2WithMain
        modified_kahip_core_obj
        kahip_options
        kahip_warnings
)
catch_discover_tests(
    modified_bipartition_invariant_test
    TEST_PREFIX "unit-"
    OUTPUT_DIR .
    OUTPUT_PREFIX "unit-"
)

add_executable(kahip_cube_generator fixtures/cube_graph_generator.cpp)
target_link_libraries(
    kahip_cube_generator
    PRIVATE kahip_cube_fixture_obj kahip_options kahip_warnings
)

add_executable(
    kahip_cube_partition_verify
    fixtures/cube_partition_verify.cpp
)
target_link_libraries(
    kahip_cube_partition_verify
    PRIVATE kahip_cube_fixture_obj kahip_options kahip_warnings
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

function(
    add_cube_partition_oracle_test
    fixture
    nx
    ny
    nz
    blocks
    ranks
    test_labels
    test_timeout
)
    add_test(
        NAME integration-cube-oracle-${fixture}-${ranks}-rank
        COMMAND
            "${CMAKE_COMMAND}"
            "-DGENERATOR=$<TARGET_FILE:kahip_cube_generator>"
            "-DPARHIP=$<TARGET_FILE:parhip>"
            "-DVERIFIER=$<TARGET_FILE:kahip_cube_partition_verify>"
            "-DMPIEXEC_EXECUTABLE=${MPIEXEC_EXECUTABLE}"
            "-DMPIEXEC_NUMPROC_FLAG=${MPIEXEC_NUMPROC_FLAG}"
            "-DMPIEXEC_PREFLAGS=${MPIEXEC_PREFLAGS}"
            "-DMPIEXEC_POSTFLAGS=${MPIEXEC_POSTFLAGS}"
            "-DMANIFEST=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/cube_partition_oracle.txt"
            "-DWORK_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/cube-oracle-${fixture}-${ranks}-rank"
            "-DFIXTURE=${fixture}"
            "-DNX=${nx}"
            "-DNY=${ny}"
            "-DNZ=${nz}"
            "-DBLOCKS=${blocks}"
            "-DRANKS=${ranks}"
            -P
            "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/cube_partition_oracle.cmake"
    )
    set_tests_properties(
        integration-cube-oracle-${fixture}-${ranks}-rank
        PROPERTIES
            PROCESSORS ${ranks}
            RUN_SERIAL TRUE
            TIMEOUT ${test_timeout}
            LABELS "${test_labels}"
    )
endfunction()

foreach(ranks RANGE 1 5)
    add_cube_partition_oracle_test(
        cube4 4 4 4 2 ${ranks} "integration;fixture;mpi;oracle" 30
    )
endforeach()

add_test(
    NAME unit-mpi-tools-profile-aggregate-overflow-aborts-before-payload
    COMMAND
        "${CMAKE_COMMAND}"
        "-DPROBE=$<TARGET_FILE:mpi_tools_failure_probe>"
        -DMODE=profile-aggregate-overflow
        -DEXPECT_FAILURE=ON
        -DMPI_RANKS=2
        "-DMPIEXEC_EXECUTABLE=${MPIEXEC_EXECUTABLE}"
        "-DMPIEXEC_NUMPROC_FLAG=${MPIEXEC_NUMPROC_FLAG}"
        "-DMPIEXEC_PREFLAGS=${MPIEXEC_PREFLAGS}"
        "-DMPIEXEC_POSTFLAGS=${MPIEXEC_POSTFLAGS}"
        "-DEXPECTED_MARKER=mpi-tools-profile-aggregate-overflow affected-communicator; payload all-to-all calls are zero"
        "-DEXPECTED_DIAGNOSTIC=ParHIP serial kernel profile failure: reason=collective-aggregate-overflow"
        -P
        "${CMAKE_CURRENT_SOURCE_DIR}/communication/verify_mpi_fail_fast_probe.cmake"
)
set_tests_properties(
    unit-mpi-tools-profile-aggregate-overflow-aborts-before-payload
    PROPERTIES PROCESSORS 2 RUN_SERIAL TRUE TIMEOUT 12
)

add_executable(
    edge_balanced_graph_io_layout_test
    io/edge_balanced_graph_io_layout_test.cpp
)
target_link_libraries(
    edge_balanced_graph_io_layout_test
    PRIVATE libdspac parallel kahip_options kahip_warnings
)
add_test(
    NAME unit-edge-balanced-graph-io-layout
    COMMAND $<TARGET_FILE:edge_balanced_graph_io_layout_test>
)
set_tests_properties(
    unit-edge-balanced-graph-io-layout
    PROPERTIES TIMEOUT 5 LABELS "unit;parallel-io;edge-balanced"
)

add_executable(
    edge_balanced_graph_io_mpi_test
    io/edge_balanced_graph_io_mpi_test.cpp
)
target_link_libraries(
    edge_balanced_graph_io_mpi_test
    PRIVATE
        catch_mpi_runner
        libdspac
        parallel
        kahip_options
        kahip_warnings
)
add_test(
    NAME unit-edge-balanced-graph-io-5-rank
    COMMAND
        "${MPIEXEC_EXECUTABLE}" "${MPIEXEC_NUMPROC_FLAG}" 5
        ${MPIEXEC_PREFLAGS}
        $<TARGET_FILE:edge_balanced_graph_io_mpi_test>
        "[edge-balanced]"
        ${MPIEXEC_POSTFLAGS}
)
set_tests_properties(
    unit-edge-balanced-graph-io-5-rank
    PROPERTIES
        PROCESSORS 5
        RUN_SERIAL TRUE
        TIMEOUT 20
        LABELS "unit;mpi;parallel-io;edge-balanced"
)

add_executable(
    edge_balanced_graph_io_failure_probe
    io/edge_balanced_graph_io_failure_probe.cpp
)
target_link_libraries(
    edge_balanced_graph_io_failure_probe
    PRIVATE libdspac parallel kahip_options kahip_warnings
)
add_test(
    NAME unit-pmpi-callback-safety-edge-balanced-graph-io-failure
    COMMAND
        "${CMAKE_COMMAND}"
        "-DSOURCE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/io/edge_balanced_graph_io_failure_probe.cpp"
        "-DPROFILE=edge-balanced-graph-io-failure"
        -P
        "${CMAKE_CURRENT_SOURCE_DIR}/communication/verify_pmpi_callback_safety.cmake"
)
set_tests_properties(
    unit-pmpi-callback-safety-edge-balanced-graph-io-failure
    PROPERTIES TIMEOUT 5 LABELS "unit;mpi;parallel-io;edge-balanced"
)

foreach(
    failure_mode
    IN ITEMS
        truncated
        nonmonotone
        unaligned
        wrong-terminal
        invalid-target
        window-zero
)
    if(failure_mode STREQUAL "truncated")
        set(
            expected_diagnostic
            "Distributed backend failure: edge-balanced binary graph has an invalid file extent"
        )
    elseif(failure_mode STREQUAL "wrong-terminal")
        set(
            expected_diagnostic
            "Distributed backend failure: edge-balanced binary offset table is invalid"
        )
    elseif(failure_mode STREQUAL "invalid-target")
        set(
            expected_diagnostic
            "Distributed backend failure: edge-balanced binary graph payload validation failed"
        )
    elseif(failure_mode STREQUAL "window-zero")
        set(
            expected_diagnostic
            "MPI adapter programming failure: edge-balanced graph I/O window must be positive"
        )
    else()
        set(
            expected_diagnostic
            "Distributed backend failure: edge-balanced binary graph offset validation failed"
        )
    endif()
    add_test(
        NAME unit-edge-balanced-graph-io-${failure_mode}-failure
        COMMAND
            "${CMAKE_COMMAND}"
            "-DPROBE=$<TARGET_FILE:edge_balanced_graph_io_failure_probe>"
            "-DMODE=${failure_mode}"
            -DEXPECT_FAILURE=ON
            -DMPI_RANKS=2
            "-DMPIEXEC_EXECUTABLE=${MPIEXEC_EXECUTABLE}"
            "-DMPIEXEC_NUMPROC_FLAG=${MPIEXEC_NUMPROC_FLAG}"
            "-DMPIEXEC_PREFLAGS=${MPIEXEC_PREFLAGS}"
            "-DMPIEXEC_POSTFLAGS=${MPIEXEC_POSTFLAGS}"
            "-DEXPECTED_MARKER=edge-balanced-${failure_mode} affected-communicator; internal MPI_Finalize counter is zero"
            "-DEXPECTED_DIAGNOSTIC=${expected_diagnostic}"
            -P
            "${CMAKE_CURRENT_SOURCE_DIR}/communication/verify_mpi_fail_fast_probe.cmake"
    )
    set_tests_properties(
        unit-edge-balanced-graph-io-${failure_mode}-failure
        PROPERTIES
            PROCESSORS 2
            RUN_SERIAL TRUE
            TIMEOUT 12
            LABELS "unit;mpi;parallel-io;edge-balanced;failure"
    )
endforeach()

set(
    MODIFIED_EVOLUTIONARY_TEST_HEADERS
    "${PROJECT_SOURCE_DIR}/parallel/modified_kahip/app/configuration.h"
    "${PROJECT_SOURCE_DIR}/parallel/modified_kahip/lib/data_structure/graph_access.h"
    "${PROJECT_SOURCE_DIR}/parallel/modified_kahip/lib/parallel_mh/exchange/exchanger.h"
    "${PROJECT_SOURCE_DIR}/parallel/modified_kahip/lib/parallel_mh/parallel_mh_async.h"
    "${PROJECT_SOURCE_DIR}/parallel/modified_kahip/lib/parallel_mh/population.h"
    "${PROJECT_SOURCE_DIR}/parallel/modified_kahip/lib/tools/quality_metrics.h"
    "${PROJECT_SOURCE_DIR}/parallel/modified_kahip/lib/tools/random_functions.h"
    "${PROJECT_SOURCE_DIR}/lib/parallel_mh/evolutionary_collectives.h"
    "${PROJECT_SOURCE_DIR}/lib/parallel_mh/population_size_broadcast.h"
)

add_executable(
    evolutionary_population_estimate_test
    evolutionary/evolutionary_population_estimate_test.cpp
)
target_sources(
    evolutionary_population_estimate_test
    PRIVATE
        FILE_SET evolutionary_population_headers
        TYPE HEADERS
        BASE_DIRS "${PROJECT_SOURCE_DIR}/lib"
        FILES
            "${PROJECT_SOURCE_DIR}/lib/parallel_mh/evolutionary_collectives.h"
            "${PROJECT_SOURCE_DIR}/lib/parallel_mh/population_size_broadcast.h"
)
target_link_libraries(
    evolutionary_population_estimate_test
    PRIVATE
        Catch2::Catch2WithMain
        MPI::MPI_CXX
        kahip_fatal_diagnostics
        kahip_options
        kahip_warnings
)
catch_discover_tests(
    evolutionary_population_estimate_test
    TEST_PREFIX "unit-"
    OUTPUT_DIR .
    OUTPUT_PREFIX "unit-"
)

foreach(evolutionary_target IN ITEMS evolutionary_mpi_test evolutionary_failure_probe)
    add_executable(
        ${evolutionary_target}
        evolutionary/${evolutionary_target}.cpp
    )
    target_sources(
        ${evolutionary_target}
        PRIVATE
            FILE_SET modified_evolutionary_headers
            TYPE HEADERS
            BASE_DIRS
                "${PROJECT_SOURCE_DIR}/parallel/modified_kahip/app"
                "${PROJECT_SOURCE_DIR}/parallel/modified_kahip/lib"
                "${PROJECT_SOURCE_DIR}/lib"
            FILES ${MODIFIED_EVOLUTIONARY_TEST_HEADERS}
    )
    target_include_directories(
        ${evolutionary_target}
        PRIVATE
            "${PROJECT_SOURCE_DIR}/parallel/modified_kahip/interface"
            "${PROJECT_SOURCE_DIR}/parallel/modified_kahip/lib/tools"
            "${PROJECT_SOURCE_DIR}/parallel/modified_kahip/lib/partition"
            "${PROJECT_SOURCE_DIR}/parallel/modified_kahip/lib/io"
            "${PROJECT_SOURCE_DIR}/parallel/modified_kahip/lib/partition/uncoarsening/refinement/quotient_graph_refinement/flow_refinement"
    )
    target_link_libraries(
        ${evolutionary_target}
        PRIVATE
            libmodified_kahip_interface
            MPI::MPI_CXX
            kahip_fatal_diagnostics
            kahip_options
            kahip_warnings
    )
endforeach()
target_link_libraries(evolutionary_mpi_test PRIVATE catch_mpi_runner)

add_test(
    NAME unit-pmpi-callback-safety-evolutionary-lifetime
    COMMAND
        "${CMAKE_COMMAND}"
        "-DSOURCE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/evolutionary/evolutionary_mpi_test.cpp"
        "-DPROFILE=evolutionary-lifetime"
        -P
        "${CMAKE_CURRENT_SOURCE_DIR}/communication/verify_pmpi_callback_safety.cmake"
)
set_tests_properties(
    unit-pmpi-callback-safety-evolutionary-lifetime
    PROPERTIES TIMEOUT 5
)
foreach(mpi_ranks IN ITEMS 2 3 5)
    add_test(
        NAME unit-evolutionary-lifetime-${mpi_ranks}-rank
        COMMAND
            "${MPIEXEC_EXECUTABLE}" "${MPIEXEC_NUMPROC_FLAG}" ${mpi_ranks}
            ${MPIEXEC_PREFLAGS}
            $<TARGET_FILE:evolutionary_mpi_test>
            ${MPIEXEC_POSTFLAGS}
    )
    set_tests_properties(
        unit-evolutionary-lifetime-${mpi_ranks}-rank
        PROPERTIES PROCESSORS ${mpi_ranks} RUN_SERIAL TRUE TIMEOUT 45
    )
endforeach()

add_test(
    NAME unit-pmpi-callback-safety-evolutionary-lifetime-failure
    COMMAND
        "${CMAKE_COMMAND}"
        "-DSOURCE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/evolutionary/evolutionary_failure_probe.cpp"
        "-DPROFILE=evolutionary-lifetime-failure"
        -P
        "${CMAKE_CURRENT_SOURCE_DIR}/communication/verify_pmpi_callback_safety.cmake"
)
set_tests_properties(
    unit-pmpi-callback-safety-evolutionary-lifetime-failure
    PROPERTIES TIMEOUT 5
)
foreach(
    failure_mode
    IN ITEMS
        communicator-duplication
        sendrecv
        isend
        wrong-tag
        wrong-count
        wait
        combine-cross-conditional
        invalid-label
        weight-overflow
        upper-bound-narrowing
)
    set(expected_abort_count 2)
    if(failure_mode STREQUAL "communicator-duplication")
        set(
            expected_diagnostic
            "MPI backend failure: MPI_Comm_dup(evolutionary communicator)"
        )
    elseif(failure_mode STREQUAL "sendrecv")
        set(
            expected_diagnostic
            "MPI backend failure: MPI_Sendrecv(evolutionary permutation exchange)"
        )
    elseif(failure_mode STREQUAL "isend")
        set(
            expected_diagnostic
            "MPI backend failure: MPI_Isend(evolutionary rumor)"
        )
    elseif(failure_mode STREQUAL "wrong-tag")
        set(
            expected_diagnostic
            "rumor message tag does not match receiver rank"
        )
    elseif(failure_mode STREQUAL "wrong-count")
        set(
            expected_diagnostic
            "rumor message count does not match the graph order"
        )
    elseif(failure_mode STREQUAL "combine-cross-conditional")
        set(
            expected_diagnostic
            "original-k cross combine is incompatible with asynchronous multi-rank entry"
        )
        set(expected_abort_count 1)
    elseif(failure_mode STREQUAL "upper-bound-narrowing")
        set(
            expected_diagnostic
            "ParHIP upper bound exceeds the modified KaHIP weight domain"
        )
    elseif(
        failure_mode STREQUAL "invalid-label"
        OR failure_mode STREQUAL "weight-overflow"
    )
        set(
            expected_diagnostic
            "partition labels or block-weight sums exceed their valid domains"
        )
    else()
        set(
            expected_diagnostic
            "MPI backend failure: MPI_Wait(evolutionary rumor)"
        )
    endif()
    add_test(
        NAME unit-evolutionary-lifetime-${failure_mode}-failure
        COMMAND
            "${CMAKE_COMMAND}"
            "-DMPIEXEC_EXECUTABLE=${MPIEXEC_EXECUTABLE}"
            "-DMPIEXEC_NUMPROC_FLAG=${MPIEXEC_NUMPROC_FLAG}"
            "-DMPIEXEC_PREFLAGS=${MPIEXEC_PREFLAGS}"
            "-DMPIEXEC_POSTFLAGS=${MPIEXEC_POSTFLAGS}"
            "-DPROBE=$<TARGET_FILE:evolutionary_failure_probe>"
            "-DMODE=${failure_mode}"
            "-DEXPECTED_DIAGNOSTIC=${expected_diagnostic}"
            "-DEXPECTED_ABORT_COUNT=${expected_abort_count}"
            -P
            "${CMAKE_CURRENT_SOURCE_DIR}/evolutionary/verify_evolutionary_failure.cmake"
    )
    set_tests_properties(
        unit-evolutionary-lifetime-${failure_mode}-failure
        PROPERTIES PROCESSORS 2 RUN_SERIAL TRUE TIMEOUT 20
    )
endforeach()

foreach(
    root_evolutionary_target
    IN ITEMS root_evolutionary_mpi_test root_evolutionary_failure_probe
)
    add_executable(
        ${root_evolutionary_target}
        evolutionary/${root_evolutionary_target}.cpp
    )
    target_link_libraries(
        ${root_evolutionary_target}
        PRIVATE
            kahip_core_obj
            kahip_mapping_obj
            kahip_collective_obj
            MPI::MPI_CXX
            kahip_fatal_diagnostics
            kahip_options
            kahip_warnings
    )
endforeach()
target_link_libraries(root_evolutionary_mpi_test PRIVATE catch_mpi_runner)

add_test(
    NAME unit-pmpi-callback-safety-root-evolutionary-lifetime
    COMMAND
        "${CMAKE_COMMAND}"
        "-DSOURCE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/evolutionary/root_evolutionary_mpi_test.cpp"
        "-DPROFILE=evolutionary-lifetime"
        -P
        "${CMAKE_CURRENT_SOURCE_DIR}/communication/verify_pmpi_callback_safety.cmake"
)
set_tests_properties(
    unit-pmpi-callback-safety-root-evolutionary-lifetime
    PROPERTIES TIMEOUT 5 LABELS "unit;mpi;evolutionary;lifetime"
)
foreach(mpi_ranks IN ITEMS 2 3 5)
    add_test(
        NAME unit-root-evolutionary-lifetime-${mpi_ranks}-rank
        COMMAND
            "${MPIEXEC_EXECUTABLE}" "${MPIEXEC_NUMPROC_FLAG}" ${mpi_ranks}
            ${MPIEXEC_PREFLAGS}
            $<TARGET_FILE:root_evolutionary_mpi_test>
            ${MPIEXEC_POSTFLAGS}
    )
    set_tests_properties(
        unit-root-evolutionary-lifetime-${mpi_ranks}-rank
        PROPERTIES
            PROCESSORS ${mpi_ranks}
            RUN_SERIAL TRUE
            TIMEOUT 45
            LABELS "unit;mpi;evolutionary;lifetime"
    )
endforeach()

add_test(
    NAME unit-pmpi-callback-safety-root-evolutionary-lifetime-failure
    COMMAND
        "${CMAKE_COMMAND}"
        "-DSOURCE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/evolutionary/root_evolutionary_failure_probe.cpp"
        "-DPROFILE=evolutionary-lifetime-failure"
        -P
        "${CMAKE_CURRENT_SOURCE_DIR}/communication/verify_pmpi_callback_safety.cmake"
)
set_tests_properties(
    unit-pmpi-callback-safety-root-evolutionary-lifetime-failure
    PROPERTIES TIMEOUT 5 LABELS "unit;mpi;evolutionary;lifetime;failure"
)
foreach(
    failure_mode
    IN ITEMS
        communicator-duplication
        sendrecv
        isend
        wrong-tag
        wrong-count
        wait
        unfinished-teardown
        combine-cross-conditional
        invalid-label
        weight-overflow
)
    set(expected_abort_count 2)
    if(failure_mode STREQUAL "communicator-duplication")
        set(
            expected_diagnostic
            "MPI backend failure: MPI_Comm_dup(evolutionary communicator)"
        )
    elseif(failure_mode STREQUAL "sendrecv")
        set(
            expected_diagnostic
            "MPI backend failure: MPI_Sendrecv(evolutionary permutation exchange)"
        )
    elseif(failure_mode STREQUAL "isend")
        set(
            expected_diagnostic
            "MPI backend failure: MPI_Isend(evolutionary rumor)"
        )
    elseif(failure_mode STREQUAL "wrong-tag")
        set(
            expected_diagnostic
            "rumor message tag does not match receiver rank"
        )
    elseif(failure_mode STREQUAL "wrong-count")
        set(
            expected_diagnostic
            "rumor message count does not match the graph order"
        )
    elseif(failure_mode STREQUAL "unfinished-teardown")
        set(
            expected_diagnostic
            "MPI evolutionary collective failure in evolutionary rumor exchange teardown on rank"
        )
    elseif(failure_mode STREQUAL "combine-cross-conditional")
        set(
            expected_diagnostic
            "original-k cross combine is incompatible with asynchronous multi-rank entry"
        )
        set(expected_abort_count 1)
    elseif(
        failure_mode STREQUAL "invalid-label"
        OR failure_mode STREQUAL "weight-overflow"
    )
        set(
            expected_diagnostic
            "partition labels or block-weight sums exceed their valid domains"
        )
    else()
        set(
            expected_diagnostic
            "MPI backend failure: MPI_Wait(evolutionary rumor)"
        )
    endif()
    add_test(
        NAME unit-root-evolutionary-lifetime-${failure_mode}-failure
        COMMAND
            "${CMAKE_COMMAND}"
            "-DMPIEXEC_EXECUTABLE=${MPIEXEC_EXECUTABLE}"
            "-DMPIEXEC_NUMPROC_FLAG=${MPIEXEC_NUMPROC_FLAG}"
            "-DMPIEXEC_PREFLAGS=${MPIEXEC_PREFLAGS}"
            "-DMPIEXEC_POSTFLAGS=${MPIEXEC_POSTFLAGS}"
            "-DPROBE=$<TARGET_FILE:root_evolutionary_failure_probe>"
            "-DMODE=${failure_mode}"
            "-DEXPECTED_DIAGNOSTIC=${expected_diagnostic}"
            "-DEXPECTED_ABORT_COUNT=${expected_abort_count}"
            -P
            "${CMAKE_CURRENT_SOURCE_DIR}/evolutionary/verify_evolutionary_failure.cmake"
    )
    set_tests_properties(
        unit-root-evolutionary-lifetime-${failure_mode}-failure
        PROPERTIES
            PROCESSORS 2
            RUN_SERIAL TRUE
            TIMEOUT 20
            LABELS "unit;mpi;evolutionary;lifetime;failure"
    )
endforeach()

add_executable(application_math_test app/application_math_test.cpp)
target_link_libraries(
    application_math_test
    PRIVATE kahip_options kahip_warnings
)
target_include_directories(
    application_math_test
    PRIVATE "${PROJECT_SOURCE_DIR}/parallel/shared"
)
add_test(NAME unit-application-math COMMAND application_math_test)
set_tests_properties(unit-application-math PROPERTIES LABELS "unit;app")

add_executable(
    mpi_application_runtime_mpi_test
    app/mpi_application_runtime_mpi_test.cpp
)
target_link_libraries(
    mpi_application_runtime_mpi_test
    PRIVATE
        parhip_mpi_application_obj
        parallel
        kahip_options
        kahip_warnings
)
foreach(mpi_ranks IN ITEMS 1 2)
    add_test(
        NAME unit-mpi-application-runtime-${mpi_ranks}-rank
        COMMAND
            "${MPIEXEC_EXECUTABLE}" "${MPIEXEC_NUMPROC_FLAG}" ${mpi_ranks}
            ${MPIEXEC_PREFLAGS}
            $<TARGET_FILE:mpi_application_runtime_mpi_test>
            ${MPIEXEC_POSTFLAGS}
    )
    set_tests_properties(
        unit-mpi-application-runtime-${mpi_ranks}-rank
        PROPERTIES
            PROCESSORS ${mpi_ranks}
            RUN_SERIAL TRUE
            TIMEOUT 10
            LABELS "unit;app;mpi;lifecycle"
    )
endforeach()

add_executable(
    parser_communicator_mpi_test
    app/parser_communicator_mpi_test.cpp
)
target_compile_definitions(
    parser_communicator_mpi_test
    PRIVATE PARALLEL_LABEL_COMPRESSION
)
target_link_libraries(
    parser_communicator_mpi_test
    PRIVATE parallel argtable3 kahip_options kahip_warnings
)
add_test(
    NAME unit-parser-explicit-communicator-2-rank
    COMMAND
        "${MPIEXEC_EXECUTABLE}" "${MPIEXEC_NUMPROC_FLAG}" 2
        ${MPIEXEC_PREFLAGS}
        $<TARGET_FILE:parser_communicator_mpi_test>
        ${MPIEXEC_POSTFLAGS}
)
set_tests_properties(
    unit-parser-explicit-communicator-2-rank
    PROPERTIES
        PROCESSORS 2
        RUN_SERIAL TRUE
        TIMEOUT 10
        LABELS "unit;app;mpi;parser"
)

add_executable(
    mpi_application_runtime_failure_probe
    app/mpi_application_runtime_failure_probe.cpp
)
target_link_libraries(
    mpi_application_runtime_failure_probe
    PRIVATE
        parhip_mpi_application_obj
        parallel
        kahip_fatal_diagnostics
        kahip_options
        kahip_warnings
)
foreach(
    runtime_failure_mode
    IN ITEMS
        initialization
        finalization
        operation-exception
        backend-error
)
    if(runtime_failure_mode STREQUAL "initialization")
        set(
            runtime_failure_diagnostic
            "MPI lifecycle failure: runtime failure probe: MPI_Init returned raw error 17301 \\(rank unavailable\\)"
        )
        set(runtime_failure_marker "observed process abort after diagnostic flush")
    elseif(runtime_failure_mode STREQUAL "finalization")
        set(
            runtime_failure_diagnostic
            "MPI lifecycle failure: runtime failure probe: MPI_Finalize returned raw error 17302 on rank 0"
        )
        set(runtime_failure_marker "observed process abort after diagnostic flush")
    elseif(runtime_failure_mode STREQUAL "operation-exception")
        set(
            runtime_failure_diagnostic
            "runtime failure probe: injected operation exception \\(rank 0\\)"
        )
        set(
            runtime_failure_marker
            "observed operation communicator MPI_Abort after diagnostic flush"
        )
    else()
        set(
            runtime_failure_diagnostic
            "MPI backend failure: injected application backend error.*world rank 0"
        )
        set(
            runtime_failure_marker
            "observed operation communicator MPI_Abort after diagnostic flush"
        )
    endif()
    add_test(
        NAME unit-mpi-application-${runtime_failure_mode}-failure
        COMMAND
            "${CMAKE_COMMAND}"
            "-DPROBE=$<TARGET_FILE:mpi_application_runtime_failure_probe>"
            "-DMODE=${runtime_failure_mode}"
            "-DEXPECTED_DIAGNOSTIC=${runtime_failure_diagnostic}"
            "-DEXPECTED_MARKER=${runtime_failure_marker}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/app/verify_failure_probe.cmake"
    )
    set_tests_properties(
        unit-mpi-application-${runtime_failure_mode}-failure
        PROPERTIES
            RUN_SERIAL TRUE
            TIMEOUT 10
            LABELS "unit;app;mpi;lifecycle;failure"
    )
endforeach()

add_executable(
    mpi_owned_handle_lifetime_failure_probe
    app/mpi_owned_handle_lifetime_failure_probe.cpp
)
target_link_libraries(
    mpi_owned_handle_lifetime_failure_probe
    PRIVATE parallel kahip_fatal_diagnostics kahip_options kahip_warnings
)
foreach(owned_handle IN ITEMS communicator datatype distributed-graph)
    if(owned_handle STREQUAL "communicator")
        set(owned_handle_diagnostic "communicator destruction")
    elseif(owned_handle STREQUAL "datatype")
        set(owned_handle_diagnostic "owned datatype destruction")
    else()
        set(owned_handle_diagnostic "distributed graph destruction")
    endif()
    add_test(
        NAME unit-mpi-owned-${owned_handle}-outlives-runtime
        COMMAND
            "${CMAKE_COMMAND}"
            "-DPROBE=$<TARGET_FILE:mpi_owned_handle_lifetime_failure_probe>"
            "-DMODE=${owned_handle}"
            "-DEXPECTED_DIAGNOSTIC=MPI adapter ownership outlived the active MPI runtime: ${owned_handle_diagnostic}"
            "-DEXPECTED_MARKER=observed owned-handle abort after diagnostic flush"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/app/verify_failure_probe.cmake"
    )
    set_tests_properties(
        unit-mpi-owned-${owned_handle}-outlives-runtime
        PROPERTIES
            RUN_SERIAL TRUE
            TIMEOUT 10
            LABELS "unit;app;mpi;lifecycle;failure"
    )
endforeach()

if(UNIX AND NOT APPLE)
    add_library(mpi_finalize_observer SHARED app/mpi_finalize_observer.cpp)
    target_link_libraries(
        mpi_finalize_observer
        PRIVATE MPI::MPI_CXX kahip_options kahip_warnings
    )
    add_test(
        NAME integration-kaffpae-help-finalizes-mpi
        COMMAND
            "${CMAKE_COMMAND}"
            "-DEXECUTABLE=$<TARGET_FILE:kaffpaE>"
            "-DOBSERVER=$<TARGET_FILE:mpi_finalize_observer>"
            -P
            "${CMAKE_CURRENT_SOURCE_DIR}/app/verify_executable_finalizes.cmake"
    )
    set_tests_properties(
        integration-kaffpae-help-finalizes-mpi
        PROPERTIES
            RUN_SERIAL TRUE
            TIMEOUT 10
            LABELS "integration;app;mpi;lifecycle"
    )
endif()

add_test(
    NAME integration-kaffpae-invalid-k
    COMMAND
        "${CMAKE_COMMAND}"
        "-DKAFFPAE=$<TARGET_FILE:kaffpaE>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/app/verify_kaffpae_invalid_k.cmake"
)
set_tests_properties(
    integration-kaffpae-invalid-k
    PROPERTIES
        RUN_SERIAL TRUE
        TIMEOUT 10
        LABELS "integration;app;mpi;validation"
)

add_executable(
    kaffpae_runtime_failure_probe
    app/kaffpae_runtime_failure_probe.cpp
    "${PROJECT_SOURCE_DIR}/app/mpi_application_runtime.cpp"
)
target_sources(
    kaffpae_runtime_failure_probe
    PRIVATE
        FILE_SET root_mpi_runtime_headers
        TYPE HEADERS
        BASE_DIRS "${PROJECT_SOURCE_DIR}/app"
        FILES "${PROJECT_SOURCE_DIR}/app/mpi_application_runtime.h"
)
target_link_libraries(
    kaffpae_runtime_failure_probe
    PRIVATE
        MPI::MPI_CXX
        kahip_fatal_diagnostics
        kahip_options
        kahip_warnings
)
add_test(
    NAME unit-pmpi-callback-safety-kaffpae-runtime-failure
    COMMAND
        "${CMAKE_COMMAND}"
        "-DSOURCE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/app/kaffpae_runtime_failure_probe.cpp"
        "-DPROFILE=kaffpae-runtime-failure"
        -P
        "${CMAKE_CURRENT_SOURCE_DIR}/communication/verify_pmpi_callback_safety.cmake"
)
set_tests_properties(
    unit-pmpi-callback-safety-kaffpae-runtime-failure
    PROPERTIES TIMEOUT 5 LABELS "unit;app;mpi;lifecycle;failure"
)
foreach(runtime_failure_mode IN ITEMS rank-query communicator-free)
    if(runtime_failure_mode STREQUAL "rank-query")
        set(
            runtime_failure_diagnostic
            "MPI backend failure: root runtime failure probe: MPI_Comm_rank\\(kaffpaE\\) returned raw error 17401 on rank 0"
        )
        set(
            runtime_failure_marker
            "observed operation communicator MPI_Abort after diagnostic flush"
        )
    else()
        set(
            runtime_failure_diagnostic
            "MPI backend failure: root runtime failure probe: MPI_Comm_free\\(application operation communicator\\) returned raw error 17402 on rank 0"
        )
        set(
            runtime_failure_marker
            "observed MPI_COMM_WORLD fallback abort after diagnostic flush"
        )
    endif()
    add_test(
        NAME unit-kaffpae-runtime-${runtime_failure_mode}-failure
        COMMAND
            "${CMAKE_COMMAND}"
            "-DPROBE=$<TARGET_FILE:kaffpae_runtime_failure_probe>"
            "-DMODE=${runtime_failure_mode}"
            "-DEXPECTED_DIAGNOSTIC=${runtime_failure_diagnostic}"
            "-DEXPECTED_MARKER=${runtime_failure_marker}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/app/verify_failure_probe.cmake"
    )
    set_tests_properties(
        unit-kaffpae-runtime-${runtime_failure_mode}-failure
        PROPERTIES
            RUN_SERIAL TRUE
            TIMEOUT 10
            LABELS "unit;app;mpi;lifecycle;failure"
    )
endforeach()

add_test(
    NAME integration-parhip-invalid-k
    COMMAND
        "${CMAKE_COMMAND}"
        "-DPARHIP=$<TARGET_FILE:parhip>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/app/verify_parhip_invalid_k.cmake"
)
set_tests_properties(
    integration-parhip-invalid-k
    PROPERTIES
        RUN_SERIAL TRUE
        TIMEOUT 10
        LABELS "integration;app;mpi;validation"
)
foreach(ranks IN ITEMS 2 4)
    add_cube_partition_oracle_test(
        cube10 10 10 10 4 ${ranks} "integration;fixture;mpi;oracle" 30
    )
    add_cube_partition_oracle_test(
        cube100 100 100 100 4 ${ranks}
        "integration;fixture;mpi;oracle;large" 120
    )
endforeach()

foreach(
    validation_case
    IN ITEMS
        malformed-provenance
        duplicate-provenance
        missing-provenance
        unknown-provenance
        invalid-compiler
        invalid-mpi
        invalid-cell-id
        invalid-adjacency
        invalid-preconfiguration
        invalid-seed
        invalid-imbalance
        exact-cut
        orphaned-repair-provenance
        missing-repair-provenance
        invalid-repair-provenance
        mismatched-repair-upstream-anchor
)
    add_test(
        NAME unit-cube-oracle-validation-${validation_case}
        COMMAND
            "${CMAKE_COMMAND}"
            "-DTEST_CASE=${validation_case}"
            "-DORACLE_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/cube_partition_oracle.cmake"
            "-DMANIFEST=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/cube_partition_oracle.txt"
            "-DGENERATOR=$<TARGET_FILE:kahip_cube_generator>"
            "-DPARHIP=$<TARGET_FILE:parhip>"
            "-DVERIFIER=$<TARGET_FILE:kahip_cube_partition_verify>"
            "-DMPIEXEC_EXECUTABLE=${MPIEXEC_EXECUTABLE}"
            "-DMPIEXEC_NUMPROC_FLAG=${MPIEXEC_NUMPROC_FLAG}"
            "-DMPIEXEC_PREFLAGS=${MPIEXEC_PREFLAGS}"
            "-DMPIEXEC_POSTFLAGS=${MPIEXEC_POSTFLAGS}"
            "-DWORK_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/cube-oracle-validation/${validation_case}"
            -P
            "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/cube_partition_oracle_validation_test.cmake"
    )
    set_tests_properties(
        unit-cube-oracle-validation-${validation_case}
        PROPERTIES
            PROCESSORS 1
            RUN_SERIAL TRUE
            TIMEOUT 15
            LABELS "oracle;fixture"
    )
endforeach()

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

add_executable(
    partition_config_test
    distributed_partitioning/partition_config_test.cpp
)
target_link_libraries(
    partition_config_test
    PRIVATE Catch2::Catch2WithMain parallel kahip_options kahip_warnings
)
catch_discover_tests(
    partition_config_test
    TEST_PREFIX "unit-"
    OUTPUT_DIR .
    OUTPUT_PREFIX "unit-"
)

# MPI Version of Catch Test Runner
add_library(catch_mpi_runner catch_mpi/catch_mpi_runner.cpp)
target_link_libraries(catch_mpi_runner PUBLIC MPI::MPI_CXX)
target_link_libraries(catch_mpi_runner PUBLIC Catch2::Catch2)
target_link_libraries(catch_mpi_runner PRIVATE kahip_options)

add_executable(
    random_initial_partitioning_mpi_test
    distributed_partitioning/random_initial_partitioning_mpi_test.cpp
)
target_link_libraries(
    random_initial_partitioning_mpi_test
    PRIVATE
        catch_mpi_runner
        parallel
        kahip_options
        kahip_warnings
)
foreach(mpi_ranks RANGE 1 5)
    add_test(
        NAME unit-random-initial-partitioning-${mpi_ranks}-rank
        COMMAND
            "${MPIEXEC_EXECUTABLE}" "${MPIEXEC_NUMPROC_FLAG}" ${mpi_ranks}
            ${MPIEXEC_PREFLAGS}
            $<TARGET_FILE:random_initial_partitioning_mpi_test>
            ${MPIEXEC_POSTFLAGS}
    )
    set_tests_properties(
        unit-random-initial-partitioning-${mpi_ranks}-rank
        PROPERTIES
            PROCESSORS ${mpi_ranks}
            RUN_SERIAL TRUE
            TIMEOUT 15
            LABELS "unit;mpi;distributed-partitioning;random"
    )
endforeach()

add_executable(
    distributed_partitioner_failure_probe
    distributed_partitioning/distributed_partitioner_failure_probe.cpp
)
target_link_libraries(
    distributed_partitioner_failure_probe
    PRIVATE parallel kahip_options kahip_warnings
)
add_test(
    NAME unit-pmpi-callback-safety-distributed-partitioner-failure
    COMMAND
        "${CMAKE_COMMAND}"
        "-DSOURCE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/distributed_partitioning/distributed_partitioner_failure_probe.cpp"
        "-DPROFILE=distributed-partitioner-failure"
        -P
        "${CMAKE_CURRENT_SOURCE_DIR}/communication/verify_pmpi_callback_safety.cmake"
)
set_tests_properties(
    unit-pmpi-callback-safety-distributed-partitioner-failure
    PROPERTIES TIMEOUT 5 LABELS "unit;mpi;distributed-partitioning;failure"
)
foreach(
    failure_mode
    IN ITEMS
        zero-k
        mismatched-k
        zero-cluster-factor
        infinite-cluster-factor
        negative-choice-count
        choice-capacity
        exhausted-choice-cursor
        rank-backend
        mismatched-communicator
)
    if(failure_mode STREQUAL "zero-k")
        set(
            expected_diagnostic
            "random initial partitioning requires k greater than zero"
        )
    elseif(failure_mode STREQUAL "mismatched-k")
        set(
            expected_diagnostic
            "random initial partitioning k differs across communicator"
        )
    elseif(
        failure_mode STREQUAL "zero-cluster-factor"
        OR failure_mode STREQUAL "infinite-cluster-factor"
    )
        set(
            expected_diagnostic
            "distributed partitioner requires a finite positive cluster coarsening factor"
        )
    elseif(failure_mode STREQUAL "negative-choice-count")
        set(
            expected_diagnostic
            "distributed partitioner random-choice counts must be nonnegative"
        )
    elseif(failure_mode STREQUAL "choice-capacity")
        set(
            expected_diagnostic
            "random-choice count exceeds addressable vector capacity"
        )
    elseif(failure_mode STREQUAL "exhausted-choice-cursor")
        set(
            expected_diagnostic
            "distributed partitioning random-choice cursor exceeds generated choices"
        )
    elseif(failure_mode STREQUAL "rank-backend")
        set(expected_diagnostic "MPI backend failure: MPI_Comm_rank")
    else()
        set(
            expected_diagnostic
            "random initial partitioning graph communicator differs in process or rank order"
        )
    endif()
    add_test(
        NAME unit-distributed-partitioner-${failure_mode}-failure
        COMMAND
            "${CMAKE_COMMAND}"
            "-DMPIEXEC_EXECUTABLE=${MPIEXEC_EXECUTABLE}"
            "-DMPIEXEC_NUMPROC_FLAG=${MPIEXEC_NUMPROC_FLAG}"
            "-DMPIEXEC_PREFLAGS=${MPIEXEC_PREFLAGS}"
            "-DMPIEXEC_POSTFLAGS=${MPIEXEC_POSTFLAGS}"
            "-DPROBE=$<TARGET_FILE:distributed_partitioner_failure_probe>"
            "-DMODE=${failure_mode}"
            "-DEXPECTED_DIAGNOSTIC=${expected_diagnostic}"
            -P
            "${CMAKE_CURRENT_SOURCE_DIR}/distributed_partitioning/verify_distributed_partitioner_failure.cmake"
    )
    set_tests_properties(
        unit-distributed-partitioner-${failure_mode}-failure
        PROPERTIES
            PROCESSORS 2
            RUN_SERIAL TRUE
            TIMEOUT 20
            LABELS "unit;mpi;distributed-partitioning;failure"
    )
endforeach()

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
    PRIVATE libmodified_kahip_interface
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
    PRIVATE libmodified_kahip_interface
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

add_executable(
    distributed_quality_metrics_mpi_test
    tools/distributed_quality_metrics_mpi_test.cpp
)
target_link_libraries(
    distributed_quality_metrics_mpi_test
    PRIVATE
        catch_mpi_runner
        parallel
        kahip_options
        kahip_warnings
)
add_test(
    NAME unit-pmpi-callback-safety-quality-metrics
    COMMAND
        "${CMAKE_COMMAND}"
        "-DSOURCE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/tools/distributed_quality_metrics_mpi_test.cpp"
        "-DPROFILE=quality-metrics"
        -P
        "${CMAKE_CURRENT_SOURCE_DIR}/communication/verify_pmpi_callback_safety.cmake"
)
set_tests_properties(
    unit-pmpi-callback-safety-quality-metrics
    PROPERTIES TIMEOUT 5
)
foreach(mpi_ranks RANGE 1 5)
    add_test(
        NAME unit-distributed-quality-metrics-${mpi_ranks}-rank
        COMMAND
            "${MPIEXEC_EXECUTABLE}" "${MPIEXEC_NUMPROC_FLAG}" ${mpi_ranks}
            ${MPIEXEC_PREFLAGS}
            $<TARGET_FILE:distributed_quality_metrics_mpi_test>
            ${MPIEXEC_POSTFLAGS}
    )
    set_tests_properties(
        unit-distributed-quality-metrics-${mpi_ranks}-rank
        PROPERTIES PROCESSORS ${mpi_ranks} RUN_SERIAL TRUE TIMEOUT 12
    )
endforeach()

add_executable(
    distributed_quality_metrics_failure_probe
    tools/distributed_quality_metrics_failure_probe.cpp
)
target_link_libraries(
    distributed_quality_metrics_failure_probe
    PRIVATE parallel kahip_options kahip_warnings
)
add_test(
    NAME unit-pmpi-callback-safety-quality-metrics-failure
    COMMAND
        "${CMAKE_COMMAND}"
        "-DSOURCE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/tools/distributed_quality_metrics_failure_probe.cpp"
        "-DPROFILE=quality-metrics-failure"
        -P
        "${CMAKE_CURRENT_SOURCE_DIR}/communication/verify_pmpi_callback_safety.cmake"
)
set_tests_properties(
    unit-pmpi-callback-safety-quality-metrics-failure
    PROPERTIES TIMEOUT 5
)
foreach(
    failure_mode
    IN ITEMS
        backend
        zero-k
        invalid-label
        invalid-second-label
        invalid-load-label
        mismatched-k
        local-overflow
        global-overflow
        load-overflow
        edge-cut-local-overflow
        edge-cut-second-local-overflow
        local-edge-cut-overflow
        edge-cut-global-overflow
        edge-cut-second-global-overflow
        communication-volume-total-overflow
        distribution-volume-global-overflow
        null-partition-map
)
    if(failure_mode STREQUAL "backend")
        set(
            expected_diagnostic
            "MPI backend failure: MPI_Allreduce(distributed block weights)"
        )
    elseif(failure_mode STREQUAL "zero-k")
        set(
            expected_diagnostic
            "MPI adapter programming failure: distributed balance requires k greater than zero"
        )
    elseif(failure_mode STREQUAL "invalid-label")
        set(
            expected_diagnostic
            "MPI adapter programming failure: distributed balance label is outside [0, k)"
        )
    elseif(failure_mode STREQUAL "invalid-second-label")
        set(
            expected_diagnostic
            "MPI adapter programming failure: distributed second balance label is outside [0, k)"
        )
    elseif(failure_mode STREQUAL "invalid-load-label")
        set(
            expected_diagnostic
            "MPI adapter programming failure: distributed load balance label is outside [0, k)"
        )
    elseif(failure_mode STREQUAL "mismatched-k")
        set(
            expected_diagnostic
            "MPI adapter programming failure: distributed balance k differs across communicator"
        )
    elseif(failure_mode STREQUAL "local-overflow")
        set(
            expected_diagnostic
            "MPI adapter capacity failure: distributed balance: local vertex-weight sum exceeds NodeWeight capacity"
        )
    elseif(failure_mode STREQUAL "global-overflow")
        set(
            expected_diagnostic
            "MPI adapter capacity failure: distributed balance: global block-weight sum exceeds NodeWeight capacity"
        )
    elseif(failure_mode STREQUAL "load-overflow")
        set(
            expected_diagnostic
            "MPI adapter capacity failure: distributed load balance: local node-load sum exceeds NodeWeight capacity"
        )
    elseif(failure_mode STREQUAL "edge-cut-local-overflow")
        set(
            expected_diagnostic
            "MPI adapter capacity failure: distributed edge cut: local edge-cut sum exceeds EdgeWeight capacity"
        )
    elseif(failure_mode STREQUAL "edge-cut-second-local-overflow")
        set(
            expected_diagnostic
            "MPI adapter capacity failure: distributed second edge cut: local edge-cut sum exceeds EdgeWeight capacity"
        )
    elseif(failure_mode STREQUAL "local-edge-cut-overflow")
        set(
            expected_diagnostic
            "MPI adapter capacity failure: local edge cut: local edge-cut sum exceeds EdgeWeight capacity"
        )
    elseif(failure_mode STREQUAL "edge-cut-global-overflow")
        set(
            expected_diagnostic
            "MPI adapter capacity failure: distributed edge cut: global edge-cut sum exceeds EdgeWeight capacity"
        )
    elseif(failure_mode STREQUAL "edge-cut-second-global-overflow")
        set(
            expected_diagnostic
            "MPI adapter capacity failure: distributed second edge cut: global edge-cut sum exceeds EdgeWeight capacity"
        )
    elseif(failure_mode STREQUAL "communication-volume-total-overflow")
        set(
            expected_diagnostic
            "MPI adapter capacity failure: distributed communication volume: total communication volume exceeds EdgeWeight capacity"
        )
    elseif(failure_mode STREQUAL "distribution-volume-global-overflow")
        set(
            expected_diagnostic
            "MPI adapter capacity failure: distributed communication volume by rank: global communication-volume sum exceeds EdgeWeight capacity"
        )
    else()
        set(
            expected_diagnostic
            "MPI adapter programming failure: local maximum block weight requires a partition map for nonempty local storage"
        )
    endif()
    add_test(
        NAME unit-distributed-quality-metrics-${failure_mode}-failure
        COMMAND
            "${CMAKE_COMMAND}"
            "-DMPIEXEC_EXECUTABLE=${MPIEXEC_EXECUTABLE}"
            "-DMPIEXEC_NUMPROC_FLAG=${MPIEXEC_NUMPROC_FLAG}"
            "-DMPIEXEC_PREFLAGS=${MPIEXEC_PREFLAGS}"
            "-DMPIEXEC_POSTFLAGS=${MPIEXEC_POSTFLAGS}"
            "-DPROBE=$<TARGET_FILE:distributed_quality_metrics_failure_probe>"
            "-DMODE=${failure_mode}"
            "-DEXPECTED_DIAGNOSTIC=${expected_diagnostic}"
            -P
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/verify_distributed_quality_metrics_failure.cmake"
    )
    set_tests_properties(
        unit-distributed-quality-metrics-${failure_mode}-failure
        PROPERTIES PROCESSORS 3 RUN_SERIAL TRUE TIMEOUT 12
    )
endforeach()

add_executable(
    balance_management_refinement_failure_probe
    tools/balance_management_refinement_failure_probe.cpp
)
target_link_libraries(
    balance_management_refinement_failure_probe
    PRIVATE parallel kahip_options kahip_warnings
)
add_test(
    NAME unit-pmpi-callback-safety-balance-refinement-failure
    COMMAND
        "${CMAKE_COMMAND}"
        "-DSOURCE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/tools/balance_management_refinement_failure_probe.cpp"
        "-DPROFILE=balance-refinement-failure"
        -P
        "${CMAKE_CURRENT_SOURCE_DIR}/communication/verify_pmpi_callback_safety.cmake"
)
set_tests_properties(
    unit-pmpi-callback-safety-balance-refinement-failure
    PROPERTIES TIMEOUT 5
)
foreach(
    failure_mode
    IN ITEMS backend zero-k invalid-label local-overflow global-overflow
)
    if(failure_mode STREQUAL "backend")
        set(
            expected_diagnostic
            "MPI backend failure: MPI_Allreduce(refinement block weights)"
        )
    elseif(failure_mode STREQUAL "zero-k")
        set(
            expected_diagnostic
            "MPI adapter programming failure: refinement balance management requires at least one block"
        )
    elseif(failure_mode STREQUAL "invalid-label")
        set(
            expected_diagnostic
            "MPI adapter programming failure: refinement balance-management label is outside [0, k)"
        )
    elseif(failure_mode STREQUAL "local-overflow")
        set(
            expected_diagnostic
            "MPI adapter capacity failure: refinement balance management: local block-weight sum exceeds NodeWeight capacity"
        )
    else()
        set(
            expected_diagnostic
            "MPI adapter capacity failure: refinement balance management: global block-weight sum exceeds NodeWeight capacity"
        )
    endif()
    add_test(
        NAME unit-balance-refinement-${failure_mode}-failure
        COMMAND
            "${CMAKE_COMMAND}"
            "-DPROBE=$<TARGET_FILE:balance_management_refinement_failure_probe>"
            "-DMODE=${failure_mode}"
            -DEXPECT_FAILURE=ON
            -DMPI_RANKS=3
            "-DMPIEXEC_EXECUTABLE=${MPIEXEC_EXECUTABLE}"
            "-DMPIEXEC_NUMPROC_FLAG=${MPIEXEC_NUMPROC_FLAG}"
            "-DMPIEXEC_PREFLAGS=${MPIEXEC_PREFLAGS}"
            "-DMPIEXEC_POSTFLAGS=${MPIEXEC_POSTFLAGS}"
            "-DEXPECTED_MARKER=balance-refinement affected-communicator; internal MPI_Finalize counter is zero"
            "-DEXPECTED_DIAGNOSTIC=${expected_diagnostic}"
            -P
            "${CMAKE_CURRENT_SOURCE_DIR}/communication/verify_mpi_fail_fast_probe.cmake"
    )
    set_tests_properties(
        unit-balance-refinement-${failure_mode}-failure
        PROPERTIES PROCESSORS 3 RUN_SERIAL TRUE TIMEOUT 12
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

foreach(rank RANGE 1 5)
    add_projection_validation_test(
        "unit-mpi-tools-complete-graph-${rank}-rank" ${rank}
        "[complete-graph]"
    )
endforeach()

add_executable(
    mpi_tools_failure_probe
    communication/mpi_tools_failure_probe.cpp
)
target_link_libraries(
    mpi_tools_failure_probe
    PRIVATE parallel kahip_options kahip_warnings
)
add_test(
    NAME unit-pmpi-callback-safety-mpi-tools-failure
    COMMAND
        "${CMAKE_COMMAND}"
        "-DSOURCE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/communication/mpi_tools_failure_probe.cpp"
        -DPROFILE=mpi-tools-failure
        -P "${CMAKE_CURRENT_SOURCE_DIR}/communication/verify_pmpi_callback_safety.cmake"
)
set_tests_properties(
    unit-pmpi-callback-safety-mpi-tools-failure
    PROPERTIES TIMEOUT 5
)
add_test(
    NAME unit-mpi-tools-count-exchange-backend-failure
    COMMAND
        "${CMAKE_COMMAND}"
        "-DPROBE=$<TARGET_FILE:mpi_tools_failure_probe>"
        -DMODE=backend
        -DEXPECT_FAILURE=ON
        -DMPI_RANKS=2
        "-DMPIEXEC_EXECUTABLE=${MPIEXEC_EXECUTABLE}"
        "-DMPIEXEC_NUMPROC_FLAG=${MPIEXEC_NUMPROC_FLAG}"
        "-DMPIEXEC_PREFLAGS=${MPIEXEC_PREFLAGS}"
        "-DMPIEXEC_POSTFLAGS=${MPIEXEC_POSTFLAGS}"
        "-DEXPECTED_MARKER=mpi-tools-count-exchange affected-communicator; internal MPI_Finalize counter is zero"
        "-DEXPECTED_DIAGNOSTIC=MPI backend failure: MPI_Alltoall(exchange dense counts)"
        -P
        "${CMAKE_CURRENT_SOURCE_DIR}/communication/verify_mpi_fail_fast_probe.cmake"
)
set_tests_properties(
    unit-mpi-tools-count-exchange-backend-failure
    PROPERTIES PROCESSORS 2 RUN_SERIAL TRUE TIMEOUT 12
)

add_test(
    NAME unit-mpi-tools-unsafe-profile-aborts-before-payload
    COMMAND
        "${CMAKE_COMMAND}"
        "-DPROBE=$<TARGET_FILE:mpi_tools_failure_probe>"
        -DMODE=unsafe-profile
        -DEXPECT_FAILURE=ON
        -DMPI_RANKS=2
        "-DMPIEXEC_EXECUTABLE=${MPIEXEC_EXECUTABLE}"
        "-DMPIEXEC_NUMPROC_FLAG=${MPIEXEC_NUMPROC_FLAG}"
        "-DMPIEXEC_PREFLAGS=${MPIEXEC_PREFLAGS}"
        "-DMPIEXEC_POSTFLAGS=${MPIEXEC_POSTFLAGS}"
        "-DEXPECTED_MARKER=mpi-tools-unsafe-profile affected-communicator; payload all-to-all calls are zero"
        "-DEXPECTED_DIAGNOSTIC=ParHIP serial kernel profile failure: reason=block-count-out-of-range"
        -P
        "${CMAKE_CURRENT_SOURCE_DIR}/communication/verify_mpi_fail_fast_probe.cmake"
)
set_tests_properties(
    unit-mpi-tools-unsafe-profile-aborts-before-payload
    PROPERTIES PROCESSORS 2 RUN_SERIAL TRUE TIMEOUT 12
)

add_test(
    NAME unit-mpi-tools-structural-gate-rejects-self-loop
    COMMAND
        "${CMAKE_COMMAND}"
        "-DPROBE=$<TARGET_FILE:mpi_tools_failure_probe>"
        -DMODE=structural-self-loop
        -DEXPECT_FAILURE=ON
        -DMPI_RANKS=2
        "-DMPIEXEC_EXECUTABLE=${MPIEXEC_EXECUTABLE}"
        "-DMPIEXEC_NUMPROC_FLAG=${MPIEXEC_NUMPROC_FLAG}"
        "-DMPIEXEC_PREFLAGS=${MPIEXEC_PREFLAGS}"
        "-DMPIEXEC_POSTFLAGS=${MPIEXEC_POSTFLAGS}"
        "-DEXPECTED_MARKER=mpi-tools-structural-self-loop affected-communicator"
        "-DEXPECTED_DIAGNOSTIC=ParHIP serial kernel structural validation failure: expected loop-free reciprocal-undirected weighted adjacency"
        -P
        "${CMAKE_CURRENT_SOURCE_DIR}/communication/verify_mpi_fail_fast_probe.cmake"
)
set_tests_properties(
    unit-mpi-tools-structural-gate-rejects-self-loop
    PROPERTIES PROCESSORS 2 RUN_SERIAL TRUE TIMEOUT 12
)

foreach(config_mismatch IN ITEMS vcycle initial-algorithm)
    add_test(
        NAME unit-mpi-tools-${config_mismatch}-mismatch-aborts-before-payload
        COMMAND
            "${CMAKE_COMMAND}"
            "-DPROBE=$<TARGET_FILE:mpi_tools_failure_probe>"
            "-DMODE=config-${config_mismatch}-mismatch"
            -DEXPECT_FAILURE=ON
            -DMPI_RANKS=2
            "-DMPIEXEC_EXECUTABLE=${MPIEXEC_EXECUTABLE}"
            "-DMPIEXEC_NUMPROC_FLAG=${MPIEXEC_NUMPROC_FLAG}"
            "-DMPIEXEC_PREFLAGS=${MPIEXEC_PREFLAGS}"
            "-DMPIEXEC_POSTFLAGS=${MPIEXEC_POSTFLAGS}"
            "-DEXPECTED_MARKER=mpi-tools-config-${config_mismatch}-mismatch affected-communicator; payload all-to-all calls are zero"
            "-DEXPECTED_DIAGNOSTIC=ParHIP serial kernel profile failure: reason=collective-configuration-mismatch"
            -P
            "${CMAKE_CURRENT_SOURCE_DIR}/communication/verify_mpi_fail_fast_probe.cmake"
    )
    set_tests_properties(
        unit-mpi-tools-${config_mismatch}-mismatch-aborts-before-payload
        PROPERTIES PROCESSORS 2 RUN_SERIAL TRUE TIMEOUT 12
    )
endforeach()

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
    null-communicator-failure
    null-communicator
    "MPI adapter programming failure: communicator duplication requires a live intracommunicator"
    ""
    "injected null communicator construction"
    "affected=world error-string-attempts=0 cleanup-attempts=0 capacity-allreduce-attempts=0 dense-count-exchange-attempts=0 dense-payload-attempts=0 dense-datatype-attempts=0 graph-semantic-validation-attempts=0 graph-create-attempts=0 neighbor-count-exchange-attempts=0 neighbor-payload-attempts=0 neighbor-datatype-attempts=0 neighbor-phase-round-attempts=0 neighbor-capacity-after-injection-attempts=0"
)
add_mpi_failure_policy_death_test(
    intercommunicator-failure
    intercommunicator
    "MPI adapter programming failure: communicator duplication requires a live intracommunicator"
    ""
    "injected intercommunicator construction"
    "affected=communicator-guard error-string-attempts=0 cleanup-attempts=0 capacity-allreduce-attempts=0 dense-count-exchange-attempts=0 dense-payload-attempts=0 dense-datatype-attempts=0 graph-semantic-validation-attempts=0 graph-create-attempts=0 neighbor-count-exchange-attempts=0 neighbor-payload-attempts=0 neighbor-datatype-attempts=0 neighbor-phase-round-attempts=0 neighbor-capacity-after-injection-attempts=0"
)
add_mpi_failure_policy_death_test(
    null-distributed-graph-failure
    null-distributed-graph
    "MPI adapter programming failure: distributed graph construction requires a live intracommunicator"
    ""
    "injected null distributed graph construction"
    "affected=world error-string-attempts=0 cleanup-attempts=0 capacity-allreduce-attempts=0 dense-count-exchange-attempts=0 dense-payload-attempts=0 dense-datatype-attempts=0 graph-semantic-validation-attempts=0 graph-create-attempts=0 neighbor-count-exchange-attempts=0 neighbor-payload-attempts=0 neighbor-datatype-attempts=0 neighbor-phase-round-attempts=0 neighbor-capacity-after-injection-attempts=0"
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
        kahip_fatal_diagnostics
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
    PRIVATE
        MPI::MPI_CXX
        kahip_fatal_diagnostics
        kahip_options
        kahip_warnings
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
        kahip_fatal_diagnostics
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
    PRIVATE
        MPI::MPI_CXX
        kahip_fatal_diagnostics
        kahip_options
        kahip_warnings
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
    bounded-inactive-test
    mpi
    "MPI adapter programming failure: neighbor context test requires an active generation"
    FALSE
)
add_mpi_async_neighbors_failure_test(
    bounded-inactive-wait
    mpi
    "MPI adapter programming failure: neighbor context wait requires an active generation"
    FALSE
)
add_mpi_async_neighbors_failure_test(
    bounded-later-init
    mpi
    "MPI backend failure: MPI_Ineighbor_alltoallv(MPI-3 bounded neighborhood round)"
    TRUE
)
add_mpi_async_neighbors_failure_test(
    bounded-later-test
    mpi
    "MPI backend failure: MPI_Test(MPI-3 bounded neighborhood round)"
    TRUE
)
add_mpi_async_neighbors_failure_test(
    bounded-later-wait
    mpi
    "MPI backend failure: MPI_Wait(MPI-3 bounded neighborhood round)"
    TRUE
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

add_executable(
    mpi_fixed_broadcast_mpi_test
    communication/mpi_fixed_broadcast_mpi_test.cpp
)
target_link_libraries(
    mpi_fixed_broadcast_mpi_test
    PRIVATE
        catch_mpi_runner
        parallel
        kahip_options
        kahip_warnings
)
add_test(
    NAME unit-pmpi-callback-safety-fixed-broadcast
    COMMAND
        "${CMAKE_COMMAND}"
        "-DSOURCE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/communication/mpi_fixed_broadcast_mpi_test.cpp"
        "-DPROFILE=fixed-broadcast"
        -P
        "${CMAKE_CURRENT_SOURCE_DIR}/communication/verify_pmpi_callback_safety.cmake"
)
set_tests_properties(
    unit-pmpi-callback-safety-fixed-broadcast
    PROPERTIES TIMEOUT 5
)
foreach(mpi_ranks RANGE 1 5)
    add_test(
        NAME unit-mpi-fixed-broadcast-${mpi_ranks}-rank
        COMMAND
            "${MPIEXEC_EXECUTABLE}" "${MPIEXEC_NUMPROC_FLAG}" ${mpi_ranks}
            ${MPIEXEC_PREFLAGS}
            $<TARGET_FILE:mpi_fixed_broadcast_mpi_test>
            ${MPIEXEC_POSTFLAGS}
    )
    set_tests_properties(
        unit-mpi-fixed-broadcast-${mpi_ranks}-rank
        PROPERTIES PROCESSORS ${mpi_ranks} RUN_SERIAL TRUE TIMEOUT 12
    )
endforeach()

add_executable(
    mpi_fixed_broadcast_failure_probe
    communication/mpi_fixed_broadcast_failure_probe.cpp
)
target_link_libraries(
    mpi_fixed_broadcast_failure_probe
    PRIVATE parallel kahip_options kahip_warnings
)
add_test(
    NAME unit-pmpi-callback-safety-fixed-broadcast-failure
    COMMAND
        "${CMAKE_COMMAND}"
        "-DSOURCE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/communication/mpi_fixed_broadcast_failure_probe.cpp"
        "-DPROFILE=fixed-broadcast-failure"
        -P
        "${CMAKE_CURRENT_SOURCE_DIR}/communication/verify_pmpi_callback_safety.cmake"
)
set_tests_properties(
    unit-pmpi-callback-safety-fixed-broadcast-failure
    PROPERTIES TIMEOUT 5
)
foreach(
    failure_mode
    IN ITEMS
        status
        header
        partition-map
        mismatched-map
        previous-cut
        previous-weight
        missing-file
        truncated-header
        invalid-version
        intercommunicator
)
    if(failure_mode STREQUAL "status")
        set(
            expected_diagnostic
            "MPI backend failure: MPI_Bcast(binary graph read status)"
        )
    elseif(failure_mode STREQUAL "header")
        set(
            expected_diagnostic
            "MPI backend failure: MPI_Bcast(binary graph header)"
        )
    elseif(failure_mode STREQUAL "partition-map")
        set(
            expected_diagnostic
            "MPI backend failure: MPI_Bcast(previous partition map)"
        )
    elseif(failure_mode STREQUAL "mismatched-map")
        set(
            expected_diagnostic
            "MPI adapter programming failure: bounded broadcast arguments differ across communicator"
        )
    elseif(failure_mode STREQUAL "previous-cut")
        set(
            expected_diagnostic
            "MPI backend failure: MPI_Bcast(previous edge cut)"
        )
    elseif(failure_mode STREQUAL "previous-weight")
        set(
            expected_diagnostic
            "MPI backend failure: MPI_Bcast(previous maximum block weight)"
        )
    elseif(failure_mode STREQUAL "missing-file")
        set(
            expected_diagnostic
            "Distributed backend failure: unable to open binary graph file"
        )
    elseif(failure_mode STREQUAL "truncated-header")
        set(
            expected_diagnostic
            "Distributed backend failure: unable to read binary graph header"
        )
    elseif(failure_mode STREQUAL "invalid-version")
        set(
            expected_diagnostic
            "Distributed backend failure: unsupported binary graph version"
        )
    else()
        set(
            expected_diagnostic
            "MPI adapter programming failure: bounded broadcast requires a live intracommunicator"
        )
    endif()
    add_test(
        NAME unit-mpi-fixed-broadcast-${failure_mode}-failure
        COMMAND
            "${CMAKE_COMMAND}"
            "-DMPIEXEC_EXECUTABLE=${MPIEXEC_EXECUTABLE}"
            "-DMPIEXEC_NUMPROC_FLAG=${MPIEXEC_NUMPROC_FLAG}"
            "-DMPIEXEC_PREFLAGS=${MPIEXEC_PREFLAGS}"
            "-DMPIEXEC_POSTFLAGS=${MPIEXEC_POSTFLAGS}"
            "-DPROBE=$<TARGET_FILE:mpi_fixed_broadcast_failure_probe>"
            "-DMODE=${failure_mode}"
            "-DEXPECTED_DIAGNOSTIC=${expected_diagnostic}"
            "-DFIXTURE=${CMAKE_CURRENT_BINARY_DIR}/fixed-broadcast-${failure_mode}.bgf"
            -P
            "${CMAKE_CURRENT_SOURCE_DIR}/communication/verify_mpi_fixed_broadcast_failure.cmake"
    )
    set_tests_properties(
        unit-mpi-fixed-broadcast-${failure_mode}-failure
        PROPERTIES PROCESSORS 2 RUN_SERIAL TRUE TIMEOUT 12
    )
endforeach()

add_executable(vertex_cut_mpi_test dspac/vertex_cut_mpi_test.cpp)
target_link_libraries(
    vertex_cut_mpi_test
    PRIVATE
        catch_mpi_runner
        libdspac
        parallel
        kahip_options
        kahip_warnings
)
add_test(
    NAME unit-pmpi-callback-safety-vertex-cut
    COMMAND
        "${CMAKE_COMMAND}"
        "-DSOURCE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/dspac/vertex_cut_mpi_test.cpp"
        "-DPROFILE=vertex-cut"
        -P
        "${CMAKE_CURRENT_SOURCE_DIR}/communication/verify_pmpi_callback_safety.cmake"
)
set_tests_properties(
    unit-pmpi-callback-safety-vertex-cut
    PROPERTIES TIMEOUT 5
)
foreach(mpi_ranks RANGE 1 5)
    add_test(
        NAME unit-dspac-vertex-cut-${mpi_ranks}-rank
        COMMAND
            "${MPIEXEC_EXECUTABLE}" "${MPIEXEC_NUMPROC_FLAG}" ${mpi_ranks}
            ${MPIEXEC_PREFLAGS}
            $<TARGET_FILE:vertex_cut_mpi_test>
            ${MPIEXEC_POSTFLAGS}
    )
    set_tests_properties(
        unit-dspac-vertex-cut-${mpi_ranks}-rank
        PROPERTIES PROCESSORS ${mpi_ranks} RUN_SERIAL TRUE TIMEOUT 12
    )
endforeach()

add_executable(vertex_cut_failure_probe dspac/vertex_cut_failure_probe.cpp)
target_link_libraries(
    vertex_cut_failure_probe
    PRIVATE libdspac parallel kahip_options kahip_warnings
)
add_test(
    NAME unit-pmpi-callback-safety-vertex-cut-failure
    COMMAND
        "${CMAKE_COMMAND}"
        "-DSOURCE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/dspac/vertex_cut_failure_probe.cpp"
        "-DPROFILE=vertex-cut-failure"
        -P
        "${CMAKE_CURRENT_SOURCE_DIR}/communication/verify_pmpi_callback_safety.cmake"
)
set_tests_properties(
    unit-pmpi-callback-safety-vertex-cut-failure
    PROPERTIES TIMEOUT 5
)
foreach(
    failure_mode
    IN ITEMS
        backend
        zero-k
        undersized-partition
        out-of-range-label
        mismatched-k
        intercommunicator
)
    if(failure_mode STREQUAL "backend")
        set(
            expected_diagnostic
            "MPI backend failure: MPI_Allreduce(vertex cut)"
        )
    elseif(failure_mode STREQUAL "zero-k")
        set(
            expected_diagnostic
            "MPI adapter programming failure: vertex cut requires k greater than zero"
        )
    elseif(failure_mode STREQUAL "undersized-partition")
        set(
            expected_diagnostic
            "MPI adapter programming failure: vertex cut partition extent does not match local edge count"
        )
    elseif(failure_mode STREQUAL "out-of-range-label")
        set(
            expected_diagnostic
            "MPI adapter programming failure: vertex cut partition label is outside [0, k)"
        )
    elseif(failure_mode STREQUAL "mismatched-k")
        set(
            expected_diagnostic
            "MPI adapter programming failure: vertex cut k differs across communicator"
        )
    else()
        set(
            expected_diagnostic
            "MPI adapter programming failure: fixed reduction requires a live intracommunicator"
        )
    endif()
    add_test(
        NAME unit-dspac-vertex-cut-${failure_mode}-failure
        COMMAND
            "${CMAKE_COMMAND}"
            "-DMPIEXEC_EXECUTABLE=${MPIEXEC_EXECUTABLE}"
            "-DMPIEXEC_NUMPROC_FLAG=${MPIEXEC_NUMPROC_FLAG}"
            "-DMPIEXEC_PREFLAGS=${MPIEXEC_PREFLAGS}"
            "-DMPIEXEC_POSTFLAGS=${MPIEXEC_POSTFLAGS}"
            "-DPROBE=$<TARGET_FILE:vertex_cut_failure_probe>"
            "-DMODE=${failure_mode}"
            "-DEXPECTED_DIAGNOSTIC=${expected_diagnostic}"
            -P
            "${CMAKE_CURRENT_SOURCE_DIR}/dspac/verify_vertex_cut_failure.cmake"
    )
    set_tests_properties(
        unit-dspac-vertex-cut-${failure_mode}-failure
        PROPERTIES PROCESSORS 2 RUN_SERIAL TRUE TIMEOUT 12
    )
endforeach()

add_executable(
    abort_marker_fake_launcher
    communication/abort_marker_fake_launcher.cpp
)
target_link_libraries(
    abort_marker_fake_launcher
    PRIVATE kahip_options kahip_warnings
)
foreach(probe_kind IN ITEMS fixed-broadcast vertex-cut)
    if(probe_kind STREQUAL "fixed-broadcast")
        set(
            marker_verifier
            "${CMAKE_CURRENT_SOURCE_DIR}/communication/verify_mpi_fixed_broadcast_failure.cmake"
        )
    else()
        set(
            marker_verifier
            "${CMAKE_CURRENT_SOURCE_DIR}/dspac/verify_vertex_cut_failure.cmake"
        )
    endif()
    add_test(
        NAME unit-${probe_kind}-abort-marker-count
        COMMAND
            "${CMAKE_COMMAND}"
            "-DVERIFIER=${marker_verifier}"
            "-DFAKE_LAUNCHER=$<TARGET_FILE:abort_marker_fake_launcher>"
            "-DPROBE_KIND=${probe_kind}"
            "-DWORK_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/abort-marker-count/${probe_kind}"
            -P
            "${CMAKE_CURRENT_SOURCE_DIR}/communication/abort_marker_count_validation_test.cmake"
    )
    set_tests_properties(
        unit-${probe_kind}-abort-marker-count
        PROPERTIES RUN_SERIAL TRUE TIMEOUT 8
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
add_test(
    NAME unit-mpi-trace-oracle-verifier
    COMMAND
        "${CMAKE_COMMAND}"
        "-DVERIFIER=${CMAKE_CURRENT_SOURCE_DIR}/communication/verify_mpi_trace_oracle.cmake"
        "-DWORK_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/trace-oracle-verifier"
        -P
        "${CMAKE_CURRENT_SOURCE_DIR}/communication/mpi_trace_oracle_verifier_test.cmake"
)
set_tests_properties(
    unit-mpi-trace-oracle-verifier
    PROPERTIES TIMEOUT 10 LABELS "oracle;trace"
)

add_executable(
    mpi_trace_failure_probe
    communication/mpi_trace_failure_probe.cpp
)
target_compile_definitions(
    mpi_trace_failure_probe
    PRIVATE KAHIP_ENABLE_MPI_TRACE=1
)
target_link_libraries(
    mpi_trace_failure_probe
    PRIVATE parhip_mpi_obj kahip_options kahip_warnings
)
foreach(trace_failure_mode IN ITEMS rank allreduce)
    add_test(
        NAME unit-mpi-trace-${trace_failure_mode}-backend-failure
        COMMAND
            "${CMAKE_COMMAND}"
            "-DMPIEXEC_EXECUTABLE=${MPIEXEC_EXECUTABLE}"
            "-DMPIEXEC_NUMPROC_FLAG=${MPIEXEC_NUMPROC_FLAG}"
            "-DMPIEXEC_PREFLAGS=${MPIEXEC_PREFLAGS}"
            "-DMPIEXEC_POSTFLAGS=${MPIEXEC_POSTFLAGS}"
            "-DMPI_RANKS=2"
            "-DPROBE=$<TARGET_FILE:mpi_trace_failure_probe>"
            "-DMODE=${trace_failure_mode}"
            "-DEXPECT_FAILURE=TRUE"
            "-DEXPECTED_DIAGNOSTIC=MPI backend failure: MPI_"
            "-DEXPECTED_DETAIL=original raw code 17411"
            "-DEXPECTED_INJECTION=injected trace MPI_"
            "-DEXPECTED_MARKER=trace-${trace_failure_mode} affected-communicator"
            -P
            "${CMAKE_CURRENT_SOURCE_DIR}/communication/verify_mpi_fail_fast_probe.cmake"
    )
    set_tests_properties(
        unit-mpi-trace-${trace_failure_mode}-backend-failure
        PROPERTIES
            PROCESSORS 2
            RUN_SERIAL TRUE
            TIMEOUT 10
            LABELS "mpi;trace;failure-policy"
    )
endforeach()
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
            "-DMPIEXEC_PREFLAGS=${MPIEXEC_PREFLAGS}"
            "-DMPIEXEC_POSTFLAGS=${MPIEXEC_POSTFLAGS}"
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

foreach(parhip_library IN ITEMS parhip_interface parhip_interface_static)
    set(consumer_target "${parhip_library}_build_tree_target_consumer")
    add_executable(
        ${consumer_target}
        interface/parhip_build_tree_target_consumer.cpp
    )
    target_compile_features(${consumer_target} PRIVATE cxx_std_23)
    target_link_libraries(${consumer_target} PRIVATE ${parhip_library})
    add_test(
        NAME "integration-${parhip_library}-build-tree-target"
        COMMAND ${consumer_target}
    )
    set_tests_properties(
        "integration-${parhip_library}-build-tree-target"
        PROPERTIES LABELS "integration;interface;mpi;cmake"
    )
endforeach()

add_executable(
    parhip_interface_mpi_test
    interface/parhip_interface_mpi_test.cpp
)
target_sources(
    parhip_interface_mpi_test
    PRIVATE
        FILE_SET parhip_configuration_headers
        TYPE HEADERS
        BASE_DIRS
            "${PROJECT_SOURCE_DIR}/parallel/parallel_src/app"
            "${PROJECT_SOURCE_DIR}/parallel/parallel_src/lib"
        FILES
            "${PROJECT_SOURCE_DIR}/parallel/parallel_src/app/configuration.h"
            "${PROJECT_SOURCE_DIR}/parallel/parallel_src/lib/communication/mpi_failure.h"
            "${PROJECT_SOURCE_DIR}/parallel/parallel_src/lib/communication/mpi_handles.h"
)
target_include_directories(
    parhip_interface_mpi_test
    PRIVATE "${PROJECT_SOURCE_DIR}/parallel/parallel_src/lib/tools"
)
target_link_libraries(
    parhip_interface_mpi_test
    PRIVATE
        catch_mpi_runner
        parhip_interface
        MPI::MPI_CXX
        kahip_options
        kahip_warnings
)
foreach(mpi_ranks RANGE 1 5)
    add_test(
        NAME unit-parhip-interface-${mpi_ranks}-rank
        COMMAND
            "${MPIEXEC_EXECUTABLE}" "${MPIEXEC_NUMPROC_FLAG}" ${mpi_ranks}
            ${MPIEXEC_PREFLAGS}
            $<TARGET_FILE:parhip_interface_mpi_test>
            ${MPIEXEC_POSTFLAGS}
    )
    set_tests_properties(
        unit-parhip-interface-${mpi_ranks}-rank
        PROPERTIES PROCESSORS ${mpi_ranks} RUN_SERIAL TRUE TIMEOUT 30
    )
endforeach()

add_executable(
    parhip_interface_determinism_probe
    interface/parhip_interface_determinism_probe.cpp
)
target_link_libraries(
    parhip_interface_determinism_probe
    PRIVATE
        kahip_cube_fixture_obj
        parhip_interface
        MPI::MPI_CXX
        kahip_options
        kahip_warnings
)
add_test(
    NAME unit-parhip-interface-repeated-call-determinism
    COMMAND
        "${CMAKE_COMMAND}"
        "-DMPIEXEC_EXECUTABLE=${MPIEXEC_EXECUTABLE}"
        "-DMPIEXEC_NUMPROC_FLAG=${MPIEXEC_NUMPROC_FLAG}"
        "-DMPIEXEC_PREFLAGS=${MPIEXEC_PREFLAGS}"
        "-DMPIEXEC_POSTFLAGS=${MPIEXEC_POSTFLAGS}"
        "-DPROBE=$<TARGET_FILE:parhip_interface_determinism_probe>"
        -P
        "${CMAKE_CURRENT_SOURCE_DIR}/interface/verify_parhip_interface_determinism.cmake"
)
set_tests_properties(
    unit-parhip-interface-repeated-call-determinism
    PROPERTIES PROCESSORS 2 RUN_SERIAL TRUE TIMEOUT 130
)

add_executable(
    kaffpae_c_boundary_failure_probe
    interface/kaffpae_c_boundary_failure_probe.cpp
)
target_sources(
    kaffpae_c_boundary_failure_probe
    PRIVATE
        FILE_SET modified_kaffpae_interface_headers
        TYPE HEADERS
        BASE_DIRS "${PROJECT_SOURCE_DIR}/parallel/modified_kahip/interface"
        FILES
            "${PROJECT_SOURCE_DIR}/parallel/modified_kahip/interface/kaHIP_interface.h"
)
target_link_libraries(
    kaffpae_c_boundary_failure_probe
    PRIVATE
        libmodified_kahip_interface
        MPI::MPI_CXX
        kahip_fatal_diagnostics
        kahip_options
        kahip_warnings
)
add_test(
    NAME unit-modified-kaffpae-c-boundary-failure
    COMMAND
        "${CMAKE_COMMAND}"
        "-DPROBE=$<TARGET_FILE:kaffpae_c_boundary_failure_probe>"
        "-DMODE=allocation"
        "-DEXPECTED_DIAGNOSTIC=modified kaffpaE C boundary:.*bad_alloc"
        "-DEXPECTED_MARKER=observed kaffpaE communicator abort after diagnostic flush"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/app/verify_failure_probe.cmake"
)
set_tests_properties(
    unit-modified-kaffpae-c-boundary-failure
    PROPERTIES
        RUN_SERIAL TRUE
        TIMEOUT 10
        LABELS "unit;interface;mpi;failure"
)

add_executable(
    parhip_interface_failure_probe
    interface/parhip_interface_failure_probe.cpp
)
target_include_directories(
    parhip_interface_failure_probe
    PRIVATE "${PARHIP_GENERATED_INCLUDE_DIR}"
)
# Keep the probe's MPI wrappers effective on two-level-namespace platforms.
target_link_libraries(
    parhip_interface_failure_probe
    PRIVATE
        parhip_interface_static
        MPI::MPI_CXX
        kahip_fatal_diagnostics
        kahip_options
        kahip_warnings
)
add_test(
    NAME unit-pmpi-callback-safety-parhip-interface-failure
    COMMAND
        "${CMAKE_COMMAND}"
        "-DSOURCE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/interface/parhip_interface_failure_probe.cpp"
        "-DPROFILE=parhip-interface-failure"
        -P
        "${CMAKE_CURRENT_SOURCE_DIR}/communication/verify_pmpi_callback_safety.cmake"
)
set_tests_properties(
    unit-pmpi-callback-safety-parhip-interface-failure
    PROPERTIES TIMEOUT 5
)

add_executable(
    parhip_partition_balance_test
    interface/parhip_partition_balance_test.cpp
)
target_link_libraries(
    parhip_partition_balance_test
    PRIVATE Catch2::Catch2WithMain kahip_options kahip_warnings
)
target_include_directories(
    parhip_partition_balance_test
    PRIVATE
        "${PROJECT_SOURCE_DIR}/parallel/parallel_src"
        "${PROJECT_SOURCE_DIR}/parallel/shared"
        "${PROJECT_SOURCE_DIR}/parallel"
)
catch_discover_tests(
    parhip_partition_balance_test
    TEST_PREFIX "unit-"
    OUTPUT_DIR .
    OUTPUT_PREFIX "unit-"
)

add_executable(
    serial_kernel_profile_test
    distributed_partitioning/serial_kernel_profile_test.cpp
)
target_link_libraries(
    serial_kernel_profile_test
    PRIVATE Catch2::Catch2WithMain kahip_options kahip_warnings
)
target_include_directories(
    serial_kernel_profile_test
    PRIVATE
        "${PROJECT_SOURCE_DIR}/parallel/parallel_src"
        "${PROJECT_SOURCE_DIR}/parallel/shared"
        "${PROJECT_SOURCE_DIR}/parallel"
)
catch_discover_tests(
    serial_kernel_profile_test
    TEST_PREFIX "unit-"
    OUTPUT_DIR .
    OUTPUT_PREFIX "unit-"
)

add_executable(
    serial_kernel_profile_mpi_test
    distributed_partitioning/serial_kernel_profile_mpi_test.cpp
)
target_link_libraries(
    serial_kernel_profile_mpi_test
    PRIVATE catch_mpi_runner parallel kahip_options kahip_warnings
)
foreach(mpi_ranks RANGE 1 5)
    add_test(
        NAME unit-serial-kernel-profile-agreement-${mpi_ranks}-rank
        COMMAND
            "${MPIEXEC_EXECUTABLE}" "${MPIEXEC_NUMPROC_FLAG}" ${mpi_ranks}
            ${MPIEXEC_PREFLAGS}
            $<TARGET_FILE:serial_kernel_profile_mpi_test>
            ${MPIEXEC_POSTFLAGS}
    )
    set_tests_properties(
        unit-serial-kernel-profile-agreement-${mpi_ranks}-rank
        PROPERTIES PROCESSORS ${mpi_ranks} RUN_SERIAL TRUE TIMEOUT 15
                   LABELS "unit;mpi;serial-kernel;profile"
    )
endforeach()

foreach(
    failure_mode
    IN ITEMS
        backend-reduction
        null-distribution
        zero-k
        mismatched-k
        invalid-imbalance
        mismatched-distribution
        invalid-offsets
        missing-adjacency
        invalid-neighbor
        mismatched-vertex-weights
        missing-partition
        invalid-mode
        global-weight-overflow
        imbalanced-result
        intercommunicator
)
    if(failure_mode STREQUAL "backend-reduction")
        set(
            expected_diagnostic
            "MPI backend failure: MPI_Allreduce(ParHIP input validation)"
        )
    elseif(failure_mode STREQUAL "null-distribution")
        set(
            expected_diagnostic
            "MPI adapter programming failure: ParHIP required input pointers are invalid"
        )
    elseif(failure_mode STREQUAL "zero-k")
        set(
            expected_diagnostic
            "MPI adapter programming failure: ParHIP block count must be greater than zero"
        )
    elseif(failure_mode STREQUAL "mismatched-k")
        set(
            expected_diagnostic
            "MPI adapter programming failure: ParHIP block count differs across communicator"
        )
    elseif(failure_mode STREQUAL "invalid-imbalance")
        set(
            expected_diagnostic
            "MPI adapter programming failure: ParHIP imbalance must be finite and nonnegative"
        )
    elseif(failure_mode STREQUAL "mismatched-distribution")
        set(
            expected_diagnostic
            "MPI adapter programming failure: ParHIP vertex distribution differs across communicator"
        )
    elseif(failure_mode STREQUAL "invalid-offsets")
        set(
            expected_diagnostic
            "MPI adapter programming failure: ParHIP local CSR offsets are invalid"
        )
    elseif(failure_mode STREQUAL "missing-adjacency")
        set(
            expected_diagnostic
            "MPI adapter programming failure: ParHIP adjacency pointer is missing for nonempty edge storage"
        )
    elseif(failure_mode STREQUAL "invalid-neighbor")
        set(
            expected_diagnostic
            "MPI adapter programming failure: ParHIP adjacency contains a vertex outside the global domain"
        )
    elseif(failure_mode STREQUAL "mismatched-vertex-weights")
        set(
            expected_diagnostic
            "MPI adapter programming failure: ParHIP optional vertex-weight presence differs across communicator"
        )
    elseif(failure_mode STREQUAL "missing-partition")
        set(
            expected_diagnostic
            "MPI adapter programming failure: ParHIP partition output pointer is missing for nonempty local storage"
        )
    elseif(failure_mode STREQUAL "invalid-mode")
        set(
            expected_diagnostic
            "MPI adapter programming failure: ParHIP mode is outside the supported domain"
        )
    elseif(failure_mode STREQUAL "global-weight-overflow")
        set(
            expected_diagnostic
            "MPI adapter capacity failure: ParHIPPartitionKWay: global vertex-weight sum exceeds the graph-weight domain"
        )
    elseif(failure_mode STREQUAL "imbalanced-result")
        set(
            expected_diagnostic
            "ParHIP partition balance failure: raw imbalance=0.029999999999999999, effective percentage=3%, normalization status=false, total weight=13, block count=2, configured bound=7, lowest-ID heaviest block=0, actual weight=10, excess=3"
        )
    else()
        set(
            expected_diagnostic
            "MPI adapter programming failure: communicator duplication requires a live intracommunicator"
        )
    endif()
    add_test(
        NAME unit-parhip-interface-${failure_mode}-failure
        COMMAND
            "${CMAKE_COMMAND}"
            "-DMPIEXEC_EXECUTABLE=${MPIEXEC_EXECUTABLE}"
            "-DMPIEXEC_NUMPROC_FLAG=${MPIEXEC_NUMPROC_FLAG}"
            "-DMPIEXEC_PREFLAGS=${MPIEXEC_PREFLAGS}"
            "-DMPIEXEC_POSTFLAGS=${MPIEXEC_POSTFLAGS}"
            "-DPROBE=$<TARGET_FILE:parhip_interface_failure_probe>"
            "-DMODE=${failure_mode}"
            "-DEXPECTED_DIAGNOSTIC=${expected_diagnostic}"
            -P
            "${CMAKE_CURRENT_SOURCE_DIR}/interface/verify_parhip_interface_failure.cmake"
    )
    set_tests_properties(
        unit-parhip-interface-${failure_mode}-failure
        PROPERTIES PROCESSORS 2 RUN_SERIAL TRUE TIMEOUT 25
    )
endforeach()

add_executable(
    parhip_abort_marker_fake_launcher
    interface/parhip_abort_marker_fake_launcher.cpp
)
target_link_libraries(
    parhip_abort_marker_fake_launcher
    PRIVATE kahip_options kahip_warnings
)
add_test(
    NAME unit-parhip-interface-abort-marker-count
    COMMAND
        "${CMAKE_COMMAND}"
        "-DVERIFIER=${CMAKE_CURRENT_SOURCE_DIR}/interface/verify_parhip_interface_failure.cmake"
        "-DFAKE_LAUNCHER=$<TARGET_FILE:parhip_abort_marker_fake_launcher>"
        "-DWORK_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/abort-marker-count/parhip-interface"
        -P
        "${CMAKE_CURRENT_SOURCE_DIR}/interface/parhip_abort_marker_count_validation_test.cmake"
)
set_tests_properties(
    unit-parhip-interface-abort-marker-count
    PROPERTIES RUN_SERIAL TRUE TIMEOUT 8
)
target_link_libraries(
    parallel_contraction_test
    PRIVATE kahip_options kahip_warnings
)
target_link_libraries(
    parallel_contraction_mpi_test
    PRIVATE kahip_options kahip_warnings
)

add_executable(first_split_mpi_test dspac/first_split_mpi_test.cpp)
target_link_libraries(
    first_split_mpi_test
    PRIVATE
        catch_mpi_runner
        libdspac
        parallel
        kahip_options
        kahip_warnings
)
add_test(
    NAME unit-pmpi-callback-safety-dspac-first-split
    COMMAND
        "${CMAKE_COMMAND}"
        "-DSOURCE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/dspac/first_split_mpi_test.cpp"
        "-DPROFILE=dspac-first-split"
        -P
        "${CMAKE_CURRENT_SOURCE_DIR}/communication/verify_pmpi_callback_safety.cmake"
)
set_tests_properties(
    unit-pmpi-callback-safety-dspac-first-split
    PROPERTIES TIMEOUT 5
)
foreach(mpi_ranks RANGE 1 5)
    add_test(
        NAME unit-dspac-first-split-${mpi_ranks}-rank
        COMMAND
            "${MPIEXEC_EXECUTABLE}" "${MPIEXEC_NUMPROC_FLAG}" ${mpi_ranks}
            ${MPIEXEC_PREFLAGS}
            $<TARGET_FILE:first_split_mpi_test>
            ${MPIEXEC_POSTFLAGS}
    )
    set_tests_properties(
        unit-dspac-first-split-${mpi_ranks}-rank
        PROPERTIES PROCESSORS ${mpi_ranks} RUN_SERIAL TRUE TIMEOUT 15
    )
endforeach()

add_executable(
    first_split_failure_probe
    dspac/first_split_failure_probe.cpp
)
target_link_libraries(
    first_split_failure_probe
    PRIVATE libdspac parallel kahip_options kahip_warnings
)
add_test(
    NAME unit-pmpi-callback-safety-dspac-first-split-failure
    COMMAND
        "${CMAKE_COMMAND}"
        "-DSOURCE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/dspac/first_split_failure_probe.cpp"
        "-DPROFILE=dspac-first-split-failure"
        -P
        "${CMAKE_CURRENT_SOURCE_DIR}/communication/verify_pmpi_callback_safety.cmake"
)
set_tests_properties(
    unit-pmpi-callback-safety-dspac-first-split-failure
    PROPERTIES TIMEOUT 5
)
foreach(
    failure_mode
    IN ITEMS neighbor-payload projection-permutation projection-barrier
)
    if(failure_mode STREQUAL "neighbor-payload")
        set(failure_test_name unit-dspac-first-split-backend-failure)
    else()
        set(failure_test_name unit-dspac-${failure_mode}-failure)
    endif()
    add_test(
        NAME ${failure_test_name}
        COMMAND
            "${CMAKE_COMMAND}"
            "-DMPIEXEC_EXECUTABLE=${MPIEXEC_EXECUTABLE}"
            "-DMPIEXEC_NUMPROC_FLAG=${MPIEXEC_NUMPROC_FLAG}"
            "-DMPIEXEC_PREFLAGS=${MPIEXEC_PREFLAGS}"
            "-DMPIEXEC_POSTFLAGS=${MPIEXEC_POSTFLAGS}"
            "-DPROBE=$<TARGET_FILE:first_split_failure_probe>"
            "-DMODE=${failure_mode}"
            -P
            "${CMAKE_CURRENT_SOURCE_DIR}/dspac/verify_first_split_failure.cmake"
    )
    set_tests_properties(
        ${failure_test_name}
        PROPERTIES PROCESSORS 2 RUN_SERIAL TRUE TIMEOUT 12
    )
endforeach()

add_executable(parallel_io_mpi_test io/parallel_io_mpi_test.cpp)
target_link_libraries(
    parallel_io_mpi_test
    PRIVATE
        catch_mpi_runner
        parallel
        kahip_options
        kahip_warnings
)
foreach(mpi_ranks RANGE 1 5)
    add_test(
        NAME unit-parallel-io-${mpi_ranks}-rank
        COMMAND
            "${MPIEXEC_EXECUTABLE}" "${MPIEXEC_NUMPROC_FLAG}" ${mpi_ranks}
            ${MPIEXEC_PREFLAGS}
            $<TARGET_FILE:parallel_io_mpi_test>
            "[parallel-io]"
            ${MPIEXEC_POSTFLAGS}
    )
    set_tests_properties(
        unit-parallel-io-${mpi_ranks}-rank
        PROPERTIES PROCESSORS ${mpi_ranks} RUN_SERIAL TRUE TIMEOUT 15
    )
endforeach()

add_executable(parallel_io_failure_probe io/parallel_io_failure_probe.cpp)
target_link_libraries(
    parallel_io_failure_probe
    PRIVATE parallel kahip_options kahip_warnings
)
foreach(
    failure_mode
    IN ITEMS
        vector-missing
        vector-truncated
        graph-truncated
        text-missing
)
    if(failure_mode MATCHES "^vector-")
        set(
            expected_diagnostic
            "Distributed backend failure: partition binary payload I/O failed"
        )
    elseif(failure_mode STREQUAL "graph-truncated")
        set(
            expected_diagnostic
            "Distributed backend failure: binary graph payload I/O failed"
        )
    else()
        set(
            expected_diagnostic
            "Distributed backend failure: METIS graph I/O failed"
        )
    endif()
    add_test(
        NAME unit-parallel-io-${failure_mode}-failure
        COMMAND
            "${CMAKE_COMMAND}"
            "-DPROBE=$<TARGET_FILE:parallel_io_failure_probe>"
            "-DMODE=${failure_mode}"
            -DEXPECT_FAILURE=ON
            -DMPI_RANKS=2
            "-DMPIEXEC_EXECUTABLE=${MPIEXEC_EXECUTABLE}"
            "-DMPIEXEC_NUMPROC_FLAG=${MPIEXEC_NUMPROC_FLAG}"
            "-DMPIEXEC_PREFLAGS=${MPIEXEC_PREFLAGS}"
            "-DMPIEXEC_POSTFLAGS=${MPIEXEC_POSTFLAGS}"
            "-DEXPECTED_MARKER=parallel-io-${failure_mode} affected-communicator; internal MPI_Finalize counter is zero"
            "-DEXPECTED_DIAGNOSTIC=${expected_diagnostic}"
            -P
            "${CMAKE_CURRENT_SOURCE_DIR}/communication/verify_mpi_fail_fast_probe.cmake"
    )
    set_tests_properties(
        unit-parallel-io-${failure_mode}-failure
        PROPERTIES PROCESSORS 2 RUN_SERIAL TRUE TIMEOUT 12
    )
endforeach()
