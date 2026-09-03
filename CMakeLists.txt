cmake_minimum_required(VERSION 4.0...4.3)
include(CheckCXXCompilerFlag)
project(KaHIP VERSION 3.24 LANGUAGES C CXX)
include(CTest)
include(GNUInstallDirs)

foreach(
    install_directory_variable
    IN ITEMS CMAKE_INSTALL_BINDIR CMAKE_INSTALL_LIBDIR CMAKE_INSTALL_INCLUDEDIR
)
    if(IS_ABSOLUTE "${${install_directory_variable}}")
        message(
            FATAL_ERROR
            "${install_directory_variable} must be relative so KaHIP installs and pkg-config metadata remain relocatable"
        )
    endif()
endforeach()

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(KaHIPSettings)
include(KahipPkgConfig)

# Check dependencies
find_package(OpenMP)
if(OpenMP_CXX_FOUND)
    message(STATUS "OpenMP support detected")
else()
    message(WARNING "OpenMP not available, activating workaround")
    add_library(OpenMP::OpenMP_CXX IMPORTED INTERFACE)
    set_property(
        TARGET OpenMP::OpenMP_CXX
        PROPERTY INTERFACE_COMPILE_OPTIONS ""
    )
    target_include_directories(
        OpenMP::OpenMP_CXX
        INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/misc
    )
endif()
find_package(metis CONFIG)
if(metis_FOUND)
    message(STATUS "Metis support detected")
    find_library(GKlib CONFIG)
    if(NOT GKlib_FOUND)
        message(STATUS "Metis requires GKlib, but GKlib was not found")
        set(metis_FOUND OFF)
    else()
        target_compile_definitions(kahip_options INTERFACE USEMETIS)
    endif()
endif()

# Windows compatibility
if(WIN32)
    target_compile_definitions(kahip_options INTERFACE NOMINMAX)
endif()

# 64 Bit option
option(64BITMODE "64 bit mode" OFF)
if(64BITMODE)
    target_compile_definitions(
        kahip_options
        INTERFACE MODE64BITEDGES KAHIP_64BIT POINTER64=1
    )
endif()

# optimized output
option(OPTIMIZED_OUTPUT "optimized output" OFF)
if(OPTIMIZED_OUTPUT)
    target_compile_definitions(kahip_options INTERFACE KAFFPAOUTPUT)
endif()

# Optionally disable all MPI-dependent targets
option(NOMPI "disable all targets that depend on MPI (kaffpaE, ParHIP)" OFF)

# ParHIP
option(PARHIP "build ParHIP" ON)
option(DETERMINISTIC_PARHIP "enforce deterministic computations in ParHIP" OFF)
option(KAHIP_ENABLE_MPI_TRACE "enable canonical ParHIP MPI stage tracing" OFF)

# Look for MPI (needed for ParHIP and kaffpaE)
# Report which MPI we actually found,
# may need use MPI_HOME hint to get the proper one (eg, on openSUSE)
if(NOT NOMPI)
    add_library(kahip_fatal_diagnostics INTERFACE)
    target_compile_features(kahip_fatal_diagnostics INTERFACE cxx_std_23)
    target_sources(
        kahip_fatal_diagnostics
        INTERFACE
            FILE_SET fatal_diagnostics_headers
            TYPE HEADERS
            BASE_DIRS "${PROJECT_SOURCE_DIR}/lib"
            FILES "${PROJECT_SOURCE_DIR}/lib/tools/fatal_diagnostics.h"
    )
    target_include_directories(
        kahip_fatal_diagnostics
        INTERFACE "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/lib>"
    )

    # Always look for MPI since kaffpaE also requires it
    find_package(MPI 3.1 REQUIRED COMPONENTS C CXX)
    include(CheckCXXSourceCompiles)
    include(CMakePushCheckState)
    cmake_push_check_state(RESET)
    set(CMAKE_REQUIRED_LIBRARIES MPI::MPI_CXX)
    check_cxx_source_compiles(
        [=[
            #include <mpi.h>
            int main() {
                return MPI_Bcast_c(
                    nullptr, MPI_Count{0}, MPI_BYTE, 0, MPI_COMM_WORLD
                );
            }
        ]=]
        KAHIP_HAVE_MPI_BCAST_C
    )
    cmake_pop_check_state()
    target_compile_definitions(
        kahip_options
        INTERFACE
            KAHIP_HAVE_MPI_BCAST_C=$<BOOL:${KAHIP_HAVE_MPI_BCAST_C}>
    )

    if(MPI_C_FOUND)
        message(
            "MPI detected (can use MPI_HOME hint to direct the detection...)"
        )
        message(STATUS "MPI include: ${MPI_C_INCLUDE_DIRS}")
        message(STATUS "MPI library: ${MPI_C_LIBRARIES}")
    endif()

    if(PARHIP)
        message(STATUS "ParHIP build requested")
    else()
        message(STATUS "ParHIP build disabled")
    endif()
else()
    message(STATUS "Build without MPI dependency: kaffpaE and ParHIP disabled")
endif()

# tcmalloc
option(USE_TCMALLOC "if available, link against tcmalloc" OFF)

# ILP improver
option(USE_ILP "build local ILP improver - introduces dependency on Gurobi" OFF)

# Argtable3
add_subdirectory(extern/argtable3-3.2.2)

add_library(kahip_version INTERFACE)
target_sources(
    kahip_version
    INTERFACE
        FILE_SET HEADERS
        TYPE HEADERS
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/lib/version"
        FILES "${CMAKE_CURRENT_SOURCE_DIR}/lib/version/version.h"
)

set(
    KAHIP_ROOT_HEADER_BASE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/app
    ${CMAKE_CURRENT_SOURCE_DIR}/lib
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/io
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/partition
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/partition/uncoarsening/refinement/quotient_graph_refinement/flow_refinement
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/tools
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/version
)

function(kahip_add_private_header_set target base_dir)
    target_sources(
        ${target}
        PRIVATE
            FILE_SET private_headers
            TYPE HEADERS
            BASE_DIRS "${base_dir}"
            FILES ${ARGN}
    )
endfunction()

function(kahip_add_header_root_file_sets target prefix)
    # KaHIP's legacy flat includes require several nested search roots.  CMake
    # forbids nested BASE_DIRS within one file set, so each build-only root is
    # represented by its own empty HEADERS set.  The headers themselves remain
    # declared once in the target's private catch-all set.  These compatibility
    # sets are internal usage requirements and must not be installed or exported.
    set(root_index 0)
    foreach(base_dir IN LISTS ARGN)
        target_sources(
            ${target}
            PUBLIC
                FILE_SET "${prefix}_${root_index}"
                TYPE HEADERS
                BASE_DIRS "${base_dir}"
        )
        math(EXPR root_index "${root_index} + 1")
    endforeach()
endfunction()

function(kahip_configure_root_object target)
    kahip_add_header_root_file_sets(
        ${target}
        root_header_root
        ${KAHIP_ROOT_HEADER_BASE_DIRS}
    )
    target_link_libraries(
        ${target}
        PUBLIC kahip_options
        PRIVATE kahip_warnings
    )
endfunction()

file(
    GLOB_RECURSE KAHIP_CORE_HEADERS
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/app/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/*.h"
)
list(
    FILTER KAHIP_CORE_HEADERS
    EXCLUDE
    REGEX "/lib/(mapping|node_ordering|parallel_mh|spac)/"
)
file(
    GLOB_RECURSE KAHIP_COLLECTIVE_HEADERS
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/parallel_mh/*.h"
)
file(
    GLOB_RECURSE KAHIP_MAPPING_HEADERS
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/mapping/*.h"
)
file(
    GLOB_RECURSE KAHIP_SPAC_HEADERS
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/spac/*.h"
)
file(
    GLOB_RECURSE KAHIP_ORDERING_HEADERS
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/node_ordering/*.h"
)

set(LIBKAFFPA_SOURCE_FILES
    lib/data_structure/graph_hierarchy.cpp
    lib/algorithms/strongly_connected_components.cpp
    lib/algorithms/topological_sort.cpp
    lib/algorithms/push_relabel.cpp
    lib/io/graph_io.cpp
    lib/tools/quality_metrics.cpp
    lib/tools/random_functions.cpp
    lib/tools/graph_extractor.cpp
    lib/tools/misc.cpp
    lib/tools/partition_snapshooter.cpp
    lib/partition/graph_partitioner.cpp
    lib/partition/w_cycles/wcycle_partitioner.cpp
    lib/partition/coarsening/coarsening.cpp
    lib/partition/coarsening/contraction.cpp
    lib/partition/coarsening/edge_rating/edge_ratings.cpp
    lib/partition/coarsening/matching/matching.cpp
    lib/partition/coarsening/matching/random_matching.cpp
    lib/partition/coarsening/matching/gpa/path.cpp
    lib/partition/coarsening/matching/gpa/gpa_matching.cpp
    lib/partition/coarsening/matching/gpa/path_set.cpp
    lib/partition/coarsening/clustering/node_ordering.cpp
    lib/partition/coarsening/clustering/size_constraint_label_propagation.cpp
    lib/partition/initial_partitioning/initial_partitioning.cpp
    lib/partition/initial_partitioning/initial_partitioner.cpp
    lib/partition/initial_partitioning/initial_partition_bipartition.cpp
    lib/partition/initial_partitioning/initial_refinement/initial_refinement.cpp
    lib/partition/initial_partitioning/bipartition.cpp
    lib/partition/initial_partitioning/initial_node_separator.cpp
    lib/partition/uncoarsening/uncoarsening.cpp
    lib/partition/uncoarsening/separator/area_bfs.cpp
    lib/partition/uncoarsening/separator/vertex_separator_algorithm.cpp
    lib/partition/uncoarsening/separator/vertex_separator_flow_solver.cpp
    lib/partition/uncoarsening/refinement/cycle_improvements/greedy_neg_cycle.cpp
    lib/partition/uncoarsening/refinement/cycle_improvements/problem_factory.cpp
    lib/partition/uncoarsening/refinement/cycle_improvements/augmented_Qgraph.cpp
    lib/partition/uncoarsening/refinement/mixed_refinement.cpp
    lib/partition/uncoarsening/refinement/label_propagation_refinement/label_propagation_refinement.cpp
    lib/partition/uncoarsening/refinement/refinement.cpp
    lib/partition/uncoarsening/refinement/quotient_graph_refinement/2way_fm_refinement/two_way_fm.cpp
    lib/partition/uncoarsening/refinement/quotient_graph_refinement/flow_refinement/two_way_flow_refinement.cpp
    lib/partition/uncoarsening/refinement/quotient_graph_refinement/flow_refinement/boundary_bfs.cpp
    lib/partition/uncoarsening/refinement/quotient_graph_refinement/flow_refinement/flow_solving_kernel/cut_flow_problem_solver.cpp
    lib/partition/uncoarsening/refinement/quotient_graph_refinement/flow_refinement/most_balanced_minimum_cuts/most_balanced_minimum_cuts.cpp
    lib/partition/uncoarsening/refinement/quotient_graph_refinement/quotient_graph_refinement.cpp
    lib/partition/uncoarsening/refinement/quotient_graph_refinement/complete_boundary.cpp
    lib/partition/uncoarsening/refinement/quotient_graph_refinement/partial_boundary.cpp
    lib/partition/uncoarsening/refinement/quotient_graph_refinement/quotient_graph_scheduling/quotient_graph_scheduling.cpp
    lib/partition/uncoarsening/refinement/quotient_graph_refinement/quotient_graph_scheduling/simple_quotient_graph_scheduler.cpp
    lib/partition/uncoarsening/refinement/quotient_graph_refinement/quotient_graph_scheduling/active_block_quotient_graph_scheduler.cpp
    lib/partition/uncoarsening/refinement/kway_graph_refinement/kway_graph_refinement.cpp
    lib/partition/uncoarsening/refinement/kway_graph_refinement/kway_graph_refinement_core.cpp
    lib/partition/uncoarsening/refinement/kway_graph_refinement/kway_graph_refinement_commons.cpp
    lib/partition/uncoarsening/refinement/cycle_improvements/augmented_Qgraph_fabric.cpp
    lib/partition/uncoarsening/refinement/cycle_improvements/advanced_models.cpp
    lib/partition/uncoarsening/refinement/kway_graph_refinement/multitry_kway_fm.cpp
    lib/partition/uncoarsening/refinement/node_separators/greedy_ns_local_search.cpp
    lib/partition/uncoarsening/refinement/node_separators/fm_ns_local_search.cpp
    lib/partition/uncoarsening/refinement/node_separators/localized_fm_ns_local_search.cpp
    lib/algorithms/cycle_search.cpp
    lib/partition/uncoarsening/refinement/cycle_improvements/cycle_refinement.cpp
    lib/partition/uncoarsening/refinement/tabu_search/tabu_search.cpp
)
add_library(kahip_core_obj OBJECT ${LIBKAFFPA_SOURCE_FILES})
kahip_configure_root_object(kahip_core_obj)
kahip_add_private_header_set(
    kahip_core_obj
    "${CMAKE_CURRENT_SOURCE_DIR}"
    ${KAHIP_CORE_HEADERS}
)
target_link_libraries(
    kahip_core_obj
    PUBLIC kahip_version
    PRIVATE argtable3
)

if(NOT NOMPI)
    set(LIBKAFFPA_PARALLEL_SOURCE_FILES
        lib/parallel_mh/parallel_mh_async.cpp
        lib/parallel_mh/population.cpp
        lib/parallel_mh/galinier_combine/gal_combine.cpp
        lib/parallel_mh/galinier_combine/construct_partition.cpp
        lib/parallel_mh/exchange/exchanger.cpp
    )
    add_library(
        kahip_collective_obj
        OBJECT
        ${LIBKAFFPA_PARALLEL_SOURCE_FILES}
    )
    kahip_configure_root_object(kahip_collective_obj)
    kahip_add_private_header_set(
        kahip_collective_obj
        "${CMAKE_CURRENT_SOURCE_DIR}"
        ${KAHIP_COLLECTIVE_HEADERS}
    )
    target_link_libraries(
        kahip_collective_obj
        PUBLIC MPI::MPI_CXX
        PRIVATE kahip_fatal_diagnostics
    )
endif()

set(LIBMAPPING_SOURCE_FILES
    lib/mapping/local_search_mapping.cpp
    lib/mapping/full_search_space.cpp
    lib/mapping/full_search_space_pruned.cpp
    lib/mapping/communication_graph_search_space.cpp
    lib/mapping/fast_construct_mapping.cpp
    lib/mapping/construct_distance_matrix.cpp
    lib/mapping/mapping_algorithms.cpp
    lib/mapping/construct_mapping.cpp
)
add_library(kahip_mapping_obj OBJECT ${LIBMAPPING_SOURCE_FILES})
kahip_configure_root_object(kahip_mapping_obj)
kahip_add_private_header_set(
    kahip_mapping_obj
    "${CMAKE_CURRENT_SOURCE_DIR}"
    ${KAHIP_MAPPING_HEADERS}
)

set(LIBSPAC_SOURCE_FILES lib/spac/spac.cpp)
add_library(kahip_spac_obj OBJECT ${LIBSPAC_SOURCE_FILES})
kahip_configure_root_object(kahip_spac_obj)
kahip_add_private_header_set(
    kahip_spac_obj
    "${CMAKE_CURRENT_SOURCE_DIR}"
    ${KAHIP_SPAC_HEADERS}
)

set(NODE_ORDERING_SOURCE_FILES
    lib/node_ordering/min_degree_ordering.cpp
    lib/node_ordering/nested_dissection.cpp
    lib/node_ordering/ordering_tools.cpp
    lib/node_ordering/reductions.cpp
)
add_library(kahip_ordering_obj OBJECT ${NODE_ORDERING_SOURCE_FILES})
kahip_configure_root_object(kahip_ordering_obj)
kahip_add_private_header_set(
    kahip_ordering_obj
    "${CMAKE_CURRENT_SOURCE_DIR}"
    ${KAHIP_ORDERING_HEADERS}
)

# generate targets for each binary
add_executable(kaffpa app/kaffpa.cpp)
target_link_libraries(kaffpa PRIVATE kahip_core_obj kahip_mapping_obj)
target_compile_definitions(kaffpa PRIVATE MODE_KAFFPA)
target_link_libraries(kaffpa PRIVATE OpenMP::OpenMP_CXX)
target_link_libraries(
    kaffpa
    PRIVATE kahip_options kahip_warnings argtable3
)
install(TARGETS kaffpa RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
if(USE_TCMALLOC)
    find_library(TCMALLOC_LIB tcmalloc)
    if(TCMALLOC_LIB)
        target_link_libraries(kaffpa PRIVATE ${TCMALLOC_LIB})
        message(STATUS "Using tcmalloc: ${TCMALLOC_LIB}")
    else()
        message(STATUS "tcmalloc enabled but unavailable on this system")
    endif()
endif()

add_executable(global_multisection app/global_multisection.cpp)
target_link_libraries(global_multisection PRIVATE kahip_core_obj kahip_mapping_obj)
target_compile_definitions(
    global_multisection
    PRIVATE MODE_KAFFPA MODE_GLOBALMS
)
target_link_libraries(global_multisection PRIVATE OpenMP::OpenMP_CXX)
target_link_libraries(
    global_multisection
    PRIVATE kahip_options kahip_warnings argtable3
)
install(
    TARGETS global_multisection
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
if(USE_TCMALLOC)
    find_library(TCMALLOC_LIB tcmalloc)
    if(TCMALLOC_LIB)
        target_link_libraries(global_multisection PRIVATE ${TCMALLOC_LIB})
        message(STATUS "Using tcmalloc: ${TCMALLOC_LIB}")
    else()
        message(STATUS "tcmalloc enabled but unavailable on this system")
    endif()
endif()

add_executable(evaluator app/evaluator.cpp)
target_link_libraries(evaluator PRIVATE kahip_core_obj kahip_mapping_obj)
target_compile_definitions(evaluator PRIVATE MODE_EVALUATOR)
target_link_libraries(evaluator PRIVATE OpenMP::OpenMP_CXX)
target_link_libraries(
    evaluator
    PRIVATE kahip_options kahip_warnings argtable3
)
install(TARGETS evaluator RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

add_executable(edge_evaluator app/edge_evaluator.cpp)
target_link_libraries(edge_evaluator PRIVATE kahip_core_obj kahip_mapping_obj)
target_compile_definitions(edge_evaluator PRIVATE MODE_EVALUATOR)
target_link_libraries(edge_evaluator PRIVATE OpenMP::OpenMP_CXX)
target_link_libraries(
    edge_evaluator
    PRIVATE kahip_options kahip_warnings
)
target_link_libraries(edge_evaluator PRIVATE argtable3)
install(
    TARGETS edge_evaluator
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

add_executable(node_separator app/node_separator_ml.cpp)
target_link_libraries(node_separator PRIVATE kahip_core_obj kahip_mapping_obj)
target_compile_definitions(node_separator PRIVATE MODE_NODESEP)
target_link_libraries(node_separator PRIVATE OpenMP::OpenMP_CXX)
target_link_libraries(
    node_separator
    PRIVATE kahip_options kahip_warnings argtable3
)
install(
    TARGETS node_separator
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

add_executable(label_propagation app/label_propagation.cpp)
target_link_libraries(label_propagation PRIVATE kahip_core_obj kahip_mapping_obj)
target_compile_definitions(label_propagation PRIVATE MODE_LABELPROPAGATION)
target_link_libraries(label_propagation PRIVATE OpenMP::OpenMP_CXX)
target_link_libraries(
    label_propagation
    PRIVATE kahip_options kahip_warnings argtable3
)
install(
    TARGETS label_propagation
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

add_executable(
    partition_to_vertex_separator
    app/partition_to_vertex_separator.cpp
)
target_link_libraries(
    partition_to_vertex_separator
    PRIVATE kahip_core_obj kahip_mapping_obj
)
target_compile_definitions(
    partition_to_vertex_separator
    PRIVATE MODE_PARTITIONTOVERTEXSEPARATOR
)
target_link_libraries(partition_to_vertex_separator PRIVATE OpenMP::OpenMP_CXX)
target_link_libraries(
    partition_to_vertex_separator
    PRIVATE kahip_options kahip_warnings argtable3
)
install(
    TARGETS partition_to_vertex_separator
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

add_executable(
    interface_test
    misc/example_library_call/interface_test.cpp
    interface/kaHIP_interface.cpp
)
target_link_libraries(
    interface_test
    PRIVATE kahip_core_obj kahip_mapping_obj kahip_ordering_obj kahip_spac_obj
)
kahip_add_private_header_set(
    interface_test
    "${CMAKE_CURRENT_SOURCE_DIR}/interface"
    "${CMAKE_CURRENT_SOURCE_DIR}/interface/kaHIP_interface.h"
)
target_compile_definitions(interface_test PRIVATE MODE_KAFFPA)
target_link_libraries(interface_test PRIVATE OpenMP::OpenMP_CXX)
target_link_libraries(
    interface_test
    PRIVATE kahip_options kahip_warnings argtable3
)
if(metis_FOUND)
    target_link_libraries(interface_test PRIVATE metis GKlib)
endif()
install(TARGETS interface_test RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
if(BUILD_TESTING)
    add_executable(
        serial_c_boundary_failure_probe
        tests/interface/serial_c_boundary_failure_probe.cpp
    )
    target_link_libraries(
        serial_c_boundary_failure_probe
        PRIVATE
            kahip_static
            kahip_options
            kahip_warnings
    )
    add_test(
        NAME unit-serial-kaffpa-c-boundary-failure
        COMMAND
            "${CMAKE_COMMAND}"
            "-DPROBE=$<TARGET_FILE:serial_c_boundary_failure_probe>"
            "-DMODE=allocation"
            "-DEXPECTED_DIAGNOSTIC=KaHIP serial C boundary kaffpa:"
            "-DEXPECTED_INJECTION=armed serial allocation failure"
            "-DEXPECTED_MARKER=observed serial C boundary abort after restoring std::cout"
            -P
            "${PROJECT_SOURCE_DIR}/cmake/tests/VerifySerialCBoundaryFailure.cmake"
    )
    set_tests_properties(
        unit-serial-kaffpa-c-boundary-failure
        PROPERTIES
            RUN_SERIAL TRUE
            TIMEOUT 10
            LABELS "unit;interface;serial;failure"
    )
    add_test(
        NAME cmake-object-architecture
        COMMAND
            ${CMAKE_COMMAND}
            -DKAHIP_SOURCE_DIR=${PROJECT_SOURCE_DIR}
            -P ${PROJECT_SOURCE_DIR}/cmake/tests/VerifyObjectArchitecture.cmake
    )
    set_tests_properties(
        cmake-object-architecture
        PROPERTIES LABELS "cmake;architecture"
    )
    add_test(
        NAME cmake-mpi-capability-profiles
        COMMAND
            "${CMAKE_COMMAND}"
            "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
            "-DTEST_ROOT=${PROJECT_BINARY_DIR}/test-mpi-capabilities"
            -P
            "${PROJECT_SOURCE_DIR}/ci/test-verify-mpi-capabilities.cmake"
    )
    set_tests_properties(
        cmake-mpi-capability-profiles
        PROPERTIES LABELS "cmake;mpi"
    )
    if(UNIX)
        find_program(KAHIP_TEST_BASH_EXECUTABLE NAMES bash REQUIRED)
        add_test(
            NAME unit-cirrus-scale-runners
            COMMAND
                "${CMAKE_COMMAND}"
                "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
                "-DBASH_EXECUTABLE=${KAHIP_TEST_BASH_EXECUTABLE}"
                "-DTEST_ROOT=${PROJECT_BINARY_DIR}/test-cirrus-scale-runners"
                -P
                "${PROJECT_SOURCE_DIR}/ci/cirrus/VerifyScaleRunners.cmake"
        )
        set_tests_properties(
            unit-cirrus-scale-runners
            PROPERTIES
                TIMEOUT 30
                LABELS "unit;script;cirrus;scale"
        )
    endif()
    add_test(NAME integration-kahip-interface COMMAND interface_test)
    set_tests_properties(
        integration-kahip-interface
        PROPERTIES LABELS "integration;serial"
    )
endif()

if(NOT NOMPI)
    add_executable(
        kaffpaE
        app/kaffpaE.cpp
        app/mpi_application_runtime.cpp
    )
    target_link_libraries(
        kaffpaE
        PRIVATE kahip_core_obj kahip_mapping_obj kahip_collective_obj
    )
    target_compile_definitions(kaffpaE PRIVATE MODE_KAFFPAE)
    target_link_libraries(
        kaffpaE
        PRIVATE
            MPI::MPI_CXX
            OpenMP::OpenMP_CXX
            kahip_fatal_diagnostics
    )
    target_link_libraries(
        kaffpaE
        PRIVATE kahip_options kahip_warnings argtable3
    )
    install(TARGETS kaffpaE RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()

add_executable(graphchecker app/graphchecker.cpp)
target_link_libraries(graphchecker PRIVATE kahip_core_obj kahip_mapping_obj)
target_compile_definitions(graphchecker PRIVATE MODE_GRAPHCHECKER)
target_link_libraries(graphchecker PRIVATE OpenMP::OpenMP_CXX)
target_link_libraries(
    graphchecker
    PRIVATE kahip_options kahip_warnings argtable3
)
install(TARGETS graphchecker RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

add_executable(edge_partitioning app/spac.cpp)
target_link_libraries(edge_partitioning PRIVATE kahip_core_obj kahip_mapping_obj kahip_spac_obj)
target_compile_definitions(edge_partitioning PRIVATE MODE_KAFFPA)
target_link_libraries(edge_partitioning PRIVATE OpenMP::OpenMP_CXX)
target_link_libraries(
    edge_partitioning
    PRIVATE kahip_options kahip_warnings argtable3
)
install(
    TARGETS edge_partitioning
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

add_executable(node_ordering app/node_ordering.cpp)
target_link_libraries(node_ordering PRIVATE kahip_core_obj kahip_ordering_obj)
target_compile_definitions(
    node_ordering
    PRIVATE MODE_NODESEP MODE_NODEORDERING
)
target_link_libraries(node_ordering PRIVATE OpenMP::OpenMP_CXX)
target_link_libraries(
    node_ordering
    PRIVATE kahip_options kahip_warnings argtable3
)
install(TARGETS node_ordering RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

if(metis_FOUND)
    add_executable(fast_node_ordering app/fast_node_ordering.cpp)
    target_link_libraries(
        fast_node_ordering
        PRIVATE kahip_core_obj kahip_mapping_obj kahip_ordering_obj
    )
    target_compile_definitions(
        fast_node_ordering
        PRIVATE MODE_NODESEP MODE_NODEORDERING FASTORDERING
    )
    target_link_libraries(
        fast_node_ordering
        PRIVATE OpenMP::OpenMP_CXX metis GKlib
    )
    target_link_libraries(
        fast_node_ordering
        PRIVATE kahip_options kahip_warnings argtable3
    )
    install(
        TARGETS fast_node_ordering
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    )
endif()

# Shared interface library
add_library(kahip SHARED interface/kaHIP_interface.cpp)
set_target_properties(kahip PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
target_sources(
    kahip
    PUBLIC
        FILE_SET public_headers
        TYPE HEADERS
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/interface"
        FILES "${CMAKE_CURRENT_SOURCE_DIR}/interface/kaHIP_interface.h"
)
target_link_libraries(
    kahip
    PRIVATE kahip_core_obj kahip_mapping_obj kahip_ordering_obj kahip_spac_obj
)
target_compile_definitions(
    kahip
    PUBLIC
        $<$<BOOL:${64BITMODE}>:KAHIP_64BIT>
        $<$<BOOL:${metis_FOUND}>:USEMETIS>
    PRIVATE MODE_KAFFPA
)
target_link_libraries(kahip PRIVATE OpenMP::OpenMP_CXX)
target_link_libraries(
    kahip
    PRIVATE kahip_options kahip_warnings argtable3
)
if(metis_FOUND)
    target_link_libraries(kahip PRIVATE metis GKlib)
endif()
install(
    TARGETS kahip
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    FILE_SET public_headers DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)

# pkg-config
set(KAHIP_INSTALL_PKGCONFIGDIR "${CMAKE_INSTALL_LIBDIR}/pkgconfig")
set(KAHIP_PKGCONFIG_PREFIX "${CMAKE_INSTALL_PREFIX}")
cmake_path(
    RELATIVE_PATH KAHIP_PKGCONFIG_PREFIX
    BASE_DIRECTORY "${CMAKE_INSTALL_FULL_LIBDIR}/pkgconfig"
    OUTPUT_VARIABLE KAHIP_PKGCONFIG_PREFIX_FROM_PCFILEDIR
)
set(KAHIP_PKGCONFIG_API_OPTIONS "")
if(64BITMODE)
    list(APPEND KAHIP_PKGCONFIG_API_OPTIONS 64BIT)
endif()
if(metis_FOUND)
    list(APPEND KAHIP_PKGCONFIG_API_OPTIONS METIS)
endif()
kahip_format_serial_api_pkg_config_cflags(
    KAHIP_PKGCONFIG_CFLAGS
    ${KAHIP_PKGCONFIG_API_OPTIONS}
)
configure_file("lib/kahip.pc.in" "${CMAKE_CURRENT_BINARY_DIR}/kahip.pc" @ONLY)
install(
    FILES "${CMAKE_CURRENT_BINARY_DIR}/kahip.pc"
    DESTINATION "${KAHIP_INSTALL_PKGCONFIGDIR}"
)


# Static interface library
add_library(kahip_static STATIC interface/kaHIP_interface.cpp)
target_sources(
    kahip_static
    PUBLIC
        FILE_SET public_headers
        TYPE HEADERS
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/interface"
        FILES "${CMAKE_CURRENT_SOURCE_DIR}/interface/kaHIP_interface.h"
)
target_link_libraries(
    kahip_static
    PRIVATE kahip_core_obj kahip_mapping_obj kahip_ordering_obj kahip_spac_obj
)
target_compile_definitions(
    kahip_static
    PUBLIC
        $<$<BOOL:${64BITMODE}>:KAHIP_64BIT>
        $<$<BOOL:${metis_FOUND}>:USEMETIS>
    PRIVATE MODE_KAFFPA
)
target_link_libraries(kahip_static PRIVATE OpenMP::OpenMP_CXX)
target_link_libraries(
    kahip_static
    PRIVATE kahip_options kahip_warnings argtable3
)
if(metis_FOUND)
    target_link_libraries(kahip_static PRIVATE metis GKlib)
endif()

install(
    TARGETS kahip_static
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
)

if(BUILD_TESTING)
    foreach(serial_library IN ITEMS kahip kahip_static)
        set(consumer_target "${serial_library}_build_tree_abi_consumer")
        add_executable(
            ${consumer_target}
            tests/interface/kahip_build_tree_abi_consumer.c
        )
        target_compile_features(${consumer_target} PRIVATE c_std_17)
        set_target_properties(
            ${consumer_target}
            PROPERTIES LINKER_LANGUAGE CXX
        )
        target_link_libraries(${consumer_target} PRIVATE ${serial_library})
        target_link_options(
            ${consumer_target}
            PRIVATE
                $<TARGET_GENEX_EVAL:kahip_options,$<TARGET_PROPERTY:kahip_options,INTERFACE_LINK_OPTIONS>>
        )
        add_test(
            NAME "integration-${serial_library}-build-tree-abi"
            COMMAND ${consumer_target}
        )
        set_tests_properties(
            "integration-${serial_library}-build-tree-abi"
            PROPERTIES LABELS "integration;interface;serial;abi"
        )
    endforeach()
endif()

# ParHIP
if(NOT NOMPI AND PARHIP)
    if(BUILD_TESTING)
        find_package(Catch2 3 CONFIG REQUIRED)
    endif()

    add_subdirectory(parallel/modified_kahip)
    add_subdirectory(parallel/parallel_src)
endif()

if(USE_ILP)
    find_package(Gurobi REQUIRED)
    message("Using Gurobi for ILP solver in ilp_improve")
    add_executable(ilp_improve app/ilp_improve.cpp)
    target_link_libraries(ilp_improve PRIVATE kahip_core_obj kahip_mapping_obj)
    target_include_directories(ilp_improve PRIVATE ${GUROBI_INCLUDE_DIR})
    target_compile_definitions(ilp_improve PRIVATE MODE_ILPIMPROVE)
    target_link_libraries(
        ilp_improve
        PRIVATE OpenMP::OpenMP_CXX ${GUROBI_LIBRARIES}
    )

    add_executable(ilp_exact app/ilp_exact.cpp)
    target_link_libraries(ilp_exact PRIVATE kahip_core_obj kahip_mapping_obj)
    target_include_directories(ilp_exact PRIVATE ${GUROBI_INCLUDE_DIR})
    target_compile_definitions(
        ilp_exact
        PRIVATE MODE_ILPIMPROVE MODE_ILPEXACT
    )
    target_link_libraries(
        ilp_exact
        PRIVATE OpenMP::OpenMP_CXX ${GUROBI_LIBRARIES}
    )
endif()

# pybind11 module
option(BUILDPYTHONMODULE "Build Python binding." OFF)
if (BUILDPYTHONMODULE)
  find_package(pybind11 REQUIRED)
  pybind11_add_module(kahip_python_binding ${CMAKE_CURRENT_SOURCE_DIR}/misc/pymodule/kahip.cpp)

  # Install to the kahip Python package directory
  if(SKBUILD)
    # When building with scikit-build-core, install directly to kahip/
    install(TARGETS kahip_python_binding DESTINATION kahip)
  else()
    # Standard installation path
    install(TARGETS kahip_python_binding DESTINATION "${CMAKE_INSTALL_PYTHONDIR}/kahip")
  endif()

  target_link_libraries(kahip_python_binding PUBLIC kahip_static)
  target_link_libraries(kahip_python_binding PRIVATE kahip_options)
  set_target_properties(kahip_python_binding PROPERTIES OUTPUT_NAME "kahip")
endif ()

if(BUILD_TESTING)
    find_package(PkgConfig REQUIRED)
    include(
        "${PROJECT_SOURCE_DIR}/cmake/KahipInstallConsumerArguments.cmake"
    )
    add_test(
        NAME pkg-config-mpi-flags
        COMMAND
            "${CMAKE_COMMAND}"
            "-DKAHIP_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
            "-DPKG_CONFIG_EXECUTABLE=${PKG_CONFIG_EXECUTABLE}"
            "-DWORK_DIRECTORY=${PROJECT_BINARY_DIR}/test-install/pkgconfig-flags"
            -P
            "${PROJECT_SOURCE_DIR}/cmake/tests/VerifyMpiPkgConfigFlags.cmake"
    )
    set_tests_properties(pkg-config-mpi-flags PROPERTIES LABELS install)

    add_test(
        NAME pkg-config-kahip-abi
        COMMAND
            "${CMAKE_COMMAND}"
            "-DKAHIP_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
            "-DPKG_CONFIG_EXECUTABLE=${PKG_CONFIG_EXECUTABLE}"
            "-DWORK_DIRECTORY=${PROJECT_BINARY_DIR}/test-install/pkgconfig-abi"
            -P
            "${PROJECT_SOURCE_DIR}/cmake/tests/VerifyKahipPkgConfigAbi.cmake"
    )
    add_test(
        NAME install-consumer-argument-forwarding
        COMMAND
            "${CMAKE_COMMAND}"
            "-DKAHIP_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
            "-DWORK_DIRECTORY=${PROJECT_BINARY_DIR}/test-install/consumer-arguments"
            -P
            "${PROJECT_SOURCE_DIR}/cmake/tests/VerifyInstallConsumerArguments.cmake"
    )
    set_tests_properties(
        pkg-config-kahip-abi
        install-consumer-argument-forwarding
        PROPERTIES LABELS install
    )

    set(KAHIP_INSTALL_TEST_WITH_PARHIP OFF)
    set(KAHIP_INSTALL_TEST_PARHIP_EXECUTABLE "")
    set(KAHIP_INSTALL_TEST_PARHIP_SHARED_LIBRARY "")
    set(KAHIP_INSTALL_TEST_PARHIP_LINKER_LIBRARY "")
    set(KAHIP_INSTALL_TEST_PARHIP_STATIC_LIBRARY "")
    if(TARGET parhip_interface)
        set(KAHIP_INSTALL_TEST_WITH_PARHIP ON)
        set(
            KAHIP_INSTALL_TEST_PARHIP_SHARED_LIBRARY
            "$<TARGET_FILE_NAME:parhip_interface>"
        )
        set(
            KAHIP_INSTALL_TEST_PARHIP_LINKER_LIBRARY
            "$<TARGET_LINKER_FILE_NAME:parhip_interface>"
        )
        set(
            KAHIP_INSTALL_TEST_PARHIP_STATIC_LIBRARY
            "$<TARGET_FILE_NAME:parhip_interface_static>"
        )
    endif()
    if(TARGET parhip)
        set(
            KAHIP_INSTALL_TEST_PARHIP_EXECUTABLE
            "$<TARGET_FILE_NAME:parhip>"
        )
    endif()

    set(KAHIP_INSTALL_TEST_TARGET_WINDOWS OFF)
    if(WIN32)
        set(KAHIP_INSTALL_TEST_TARGET_WINDOWS ON)
    endif()
    set(KAHIP_INSTALL_TEST_TARGET_UNIX OFF)
    if(UNIX)
        set(KAHIP_INSTALL_TEST_TARGET_UNIX ON)
    endif()
    set(KAHIP_INSTALL_TEST_CONTEXT_ARGUMENTS "")
    set(
        KAHIP_INSTALL_TEST_COMPILE_OPTIONS
        "$<JOIN:$<TARGET_GENEX_EVAL:kahip_options,$<TARGET_PROPERTY:kahip_options,INTERFACE_COMPILE_OPTIONS>>, >"
    )
    set(
        KAHIP_INSTALL_TEST_LINK_OPTIONS
        "$<JOIN:$<TARGET_GENEX_EVAL:kahip_options,$<TARGET_PROPERTY:kahip_options,INTERFACE_LINK_OPTIONS>>, >"
    )
    kahip_append_consumer_cache_argument(
        KAHIP_INSTALL_TEST_CONTEXT_ARGUMENTS
        TARGET_WINDOWS
        "${KAHIP_INSTALL_TEST_TARGET_WINDOWS}"
    )
    kahip_append_consumer_cache_argument(
        KAHIP_INSTALL_TEST_CONTEXT_ARGUMENTS
        TARGET_UNIX
        "${KAHIP_INSTALL_TEST_TARGET_UNIX}"
    )
    kahip_append_consumer_cache_argument(
        KAHIP_INSTALL_TEST_CONTEXT_ARGUMENTS
        KAHIP_64BIT
        "${64BITMODE}"
    )
    kahip_append_consumer_cache_argument(
        KAHIP_INSTALL_TEST_CONTEXT_ARGUMENTS
        CMAKE_C_FLAGS
        "${CMAKE_C_FLAGS} ${KAHIP_INSTALL_TEST_COMPILE_OPTIONS}"
    )
    kahip_append_consumer_cache_argument(
        KAHIP_INSTALL_TEST_CONTEXT_ARGUMENTS
        CMAKE_CXX_FLAGS
        "${CMAKE_CXX_FLAGS} ${KAHIP_INSTALL_TEST_COMPILE_OPTIONS}"
    )
    kahip_append_consumer_cache_argument(
        KAHIP_INSTALL_TEST_CONTEXT_ARGUMENTS
        CMAKE_EXE_LINKER_FLAGS
        "${CMAKE_EXE_LINKER_FLAGS} ${KAHIP_INSTALL_TEST_LINK_OPTIONS}"
    )
    foreach(
        context_variable
        IN ITEMS
            CMAKE_POSITION_INDEPENDENT_CODE
            CMAKE_OSX_ARCHITECTURES
            CMAKE_OSX_SYSROOT
            CMAKE_OSX_DEPLOYMENT_TARGET
            VCPKG_INSTALLED_DIR
            VCPKG_TARGET_TRIPLET
    )
        if(
            DEFINED ${context_variable}
            AND NOT "${${context_variable}}" STREQUAL ""
        )
            kahip_append_consumer_cache_argument(
                KAHIP_INSTALL_TEST_CONTEXT_ARGUMENTS
                "${context_variable}"
                "${${context_variable}}"
            )
        endif()
    endforeach()

    add_test(
        NAME install-pkg-config-consumer
        COMMAND
            "${CMAKE_COMMAND}"
            "-DPROJECT_BINARY_DIR=${PROJECT_BINARY_DIR}"
            "-DKAHIP_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
            "-DSTAGE_PREFIX=${PROJECT_BINARY_DIR}/test-install/pkgconfig-prefix"
            "-DCONSUMER_SOURCE_DIR=${PROJECT_SOURCE_DIR}/cmake/pkgconfig-consumer"
            "-DCONSUMER_BINARY_DIR=${PROJECT_BINARY_DIR}/test-install/pkgconfig-consumer"
            "-DPKG_CONFIG_EXECUTABLE=${PKG_CONFIG_EXECUTABLE}"
            "-DCTEST_COMMAND=${CMAKE_CTEST_COMMAND}"
            "-DCONSUMER_GENERATOR=${CMAKE_GENERATOR}"
            "-DCONSUMER_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
            "-DCONSUMER_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
            "-DCONSUMER_GENERATOR_INSTANCE=${CMAKE_GENERATOR_INSTANCE}"
            "-DC_COMPILER=${CMAKE_C_COMPILER}"
            "-DCXX_COMPILER=${CMAKE_CXX_COMPILER}"
            "-DNM_EXECUTABLE=${CMAKE_NM}"
            "-DTOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}"
            "-DBUILD_CONFIG=$<CONFIG>"
            "-DINSTALL_BINDIR=${CMAKE_INSTALL_BINDIR}"
            "-DINSTALL_LIBDIR=${CMAKE_INSTALL_LIBDIR}"
            "-DINSTALL_INCLUDEDIR=${CMAKE_INSTALL_INCLUDEDIR}"
            "-DKAHIP_SHARED_LIBRARY=$<TARGET_FILE_NAME:kahip>"
            "-DKAHIP_LINKER_LIBRARY=$<TARGET_LINKER_FILE_NAME:kahip>"
            "-DKAHIP_STATIC_LIBRARY=$<TARGET_FILE_NAME:kahip_static>"
            "-DWITH_PARHIP=${KAHIP_INSTALL_TEST_WITH_PARHIP}"
            "-DPARHIP_EXECUTABLE=${KAHIP_INSTALL_TEST_PARHIP_EXECUTABLE}"
            "-DPARHIP_SHARED_LIBRARY=${KAHIP_INSTALL_TEST_PARHIP_SHARED_LIBRARY}"
            "-DPARHIP_LINKER_LIBRARY=${KAHIP_INSTALL_TEST_PARHIP_LINKER_LIBRARY}"
            "-DPARHIP_STATIC_LIBRARY=${KAHIP_INSTALL_TEST_PARHIP_STATIC_LIBRARY}"
            ${KAHIP_INSTALL_TEST_CONTEXT_ARGUMENTS}
            -P
            "${PROJECT_SOURCE_DIR}/cmake/VerifyPkgConfigInstall.cmake"
    )
    set_tests_properties(
        install-pkg-config-consumer
        PROPERTIES
            LABELS install
            RUN_SERIAL TRUE
            TIMEOUT 180
    )
endif()
