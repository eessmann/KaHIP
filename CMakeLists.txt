cmake_minimum_required(VERSION 4.0...4.3)
include(CheckCXXCompilerFlag)
project(KaHIP VERSION 3.24 LANGUAGES C CXX)
include(CTest)
include(GNUInstallDirs)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(KaHIPSettings)

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
        add_definitions("-DUSEMETIS")
        if(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
            include_directories(/usr/local/include)
        endif()
    endif()
endif()

# Windows compatibility
if(WIN32)
  add_definitions(-DNOMINMAX)
  # MSVC: use /W3 instead of /Wall (which is extremely pedantic)
  string(REGEX REPLACE "/W[0-4]" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
  add_compile_options(/W3)
endif()

# 64 Bit option
option(64BITMODE "64 bit mode" OFF)
if(64BITMODE)
    add_definitions("-DMODE64BITEDGES")
    add_definitions("-DKAHIP_64BIT")
    add_definitions("-DPOINTER64=1")
endif()

# optimized output
option(OPTIMIZED_OUTPUT "optimized output" OFF)
if(OPTIMIZED_OUTPUT)
    add_definitions("-DKAFFPAOUTPUT")
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
    # Always look for MPI since kaffpaE also requires it
    find_package(MPI REQUIRED)

    if(${MPI_C_FOUND})
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
target_include_directories(
    kahip_version
    INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/lib/version
)

set(
    KAHIP_PRIVATE_INCLUDE_DIRS
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

function(kahip_configure_root_object target)
    target_include_directories(${target} PUBLIC ${KAHIP_PRIVATE_INCLUDE_DIRS})
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
list(
    APPEND KAHIP_COLLECTIVE_HEADERS
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/tools/graph_communication.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/tools/mpi_tools.h"
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
kahip_add_private_header_set(
    kahip_core_obj
    "${CMAKE_CURRENT_SOURCE_DIR}"
    ${KAHIP_CORE_HEADERS}
)
kahip_configure_root_object(kahip_core_obj)
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
        lib/tools/graph_communication.cpp
        lib/tools/mpi_tools.cpp
    )
    add_library(
        kahip_collective_obj
        OBJECT
        ${LIBKAFFPA_PARALLEL_SOURCE_FILES}
    )
    kahip_add_private_header_set(
        kahip_collective_obj
        "${CMAKE_CURRENT_SOURCE_DIR}"
        ${KAHIP_COLLECTIVE_HEADERS}
    )
    kahip_configure_root_object(kahip_collective_obj)
    target_link_libraries(
        kahip_collective_obj
        PUBLIC MPI::MPI_CXX
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
kahip_add_private_header_set(
    kahip_mapping_obj
    "${CMAKE_CURRENT_SOURCE_DIR}"
    ${KAHIP_MAPPING_HEADERS}
)
kahip_configure_root_object(kahip_mapping_obj)

set(LIBSPAC_SOURCE_FILES lib/spac/spac.cpp)
add_library(kahip_spac_obj OBJECT ${LIBSPAC_SOURCE_FILES})
kahip_add_private_header_set(
    kahip_spac_obj
    "${CMAKE_CURRENT_SOURCE_DIR}"
    ${KAHIP_SPAC_HEADERS}
)
kahip_configure_root_object(kahip_spac_obj)

set(NODE_ORDERING_SOURCE_FILES
    lib/node_ordering/min_degree_ordering.cpp
    lib/node_ordering/nested_dissection.cpp
    lib/node_ordering/ordering_tools.cpp
    lib/node_ordering/reductions.cpp
)
add_library(kahip_ordering_obj OBJECT ${NODE_ORDERING_SOURCE_FILES})
kahip_add_private_header_set(
    kahip_ordering_obj
    "${CMAKE_CURRENT_SOURCE_DIR}"
    ${KAHIP_ORDERING_HEADERS}
)
kahip_configure_root_object(kahip_ordering_obj)

# generate targets for each binary
add_executable(kaffpa app/kaffpa.cpp)
target_link_libraries(kaffpa PRIVATE kahip_core_obj kahip_mapping_obj)
target_compile_definitions(kaffpa PRIVATE "-DMODE_KAFFPA")
target_link_libraries(kaffpa PRIVATE OpenMP::OpenMP_CXX)
target_link_libraries(
    kaffpa
    PRIVATE kahip_options kahip_warnings argtable3
)
install(TARGETS kaffpa DESTINATION bin)
if(USE_TCMALLOC)
    find_library(TCMALLOC_LIB tcmalloc)
    if(TCMALLOC_LIB)
        target_link_libraries(kaffpa ${TCMALLOC_LIB})
        message(STATUS "Using tcmalloc: ${TCMALLOC_LIB}")
    else()
        message(STATUS "tcmalloc enabled but unavailable on this system")
    endif()
endif()

add_executable(global_multisection app/global_multisection.cpp)
target_link_libraries(global_multisection PRIVATE kahip_core_obj kahip_mapping_obj)
target_compile_definitions(
    global_multisection
    PRIVATE "-DMODE_KAFFPA" "-DMODE_GLOBALMS"
)
target_link_libraries(global_multisection PRIVATE OpenMP::OpenMP_CXX)
target_link_libraries(
    global_multisection
    PRIVATE kahip_options kahip_warnings argtable3
)
install(TARGETS global_multisection DESTINATION bin)
if(USE_TCMALLOC)
    find_library(TCMALLOC_LIB tcmalloc)
    if(TCMALLOC_LIB)
        target_link_libraries(global_multisection ${TCMALLOC_LIB})
        message(STATUS "Using tcmalloc: ${TCMALLOC_LIB}")
    else()
        message(STATUS "tcmalloc enabled but unavailable on this system")
    endif()
endif()

add_executable(evaluator app/evaluator.cpp)
target_link_libraries(evaluator PRIVATE kahip_core_obj kahip_mapping_obj)
target_compile_definitions(evaluator PRIVATE "-DMODE_EVALUATOR")
target_link_libraries(evaluator PRIVATE OpenMP::OpenMP_CXX)
target_link_libraries(
    evaluator
    PRIVATE kahip_options kahip_warnings argtable3
)
install(TARGETS evaluator DESTINATION bin)

add_executable(edge_evaluator app/edge_evaluator.cpp)
target_link_libraries(edge_evaluator PRIVATE kahip_core_obj kahip_mapping_obj)
target_compile_definitions(edge_evaluator PRIVATE "-DMODE_EVALUATOR")
target_link_libraries(edge_evaluator PRIVATE OpenMP::OpenMP_CXX)
target_link_libraries(
    edge_evaluator
    PRIVATE kahip_options kahip_warnings
)
target_link_libraries(edge_evaluator PRIVATE argtable3)
install(TARGETS edge_evaluator DESTINATION bin)

add_executable(node_separator app/node_separator_ml.cpp)
target_link_libraries(node_separator PRIVATE kahip_core_obj kahip_mapping_obj)
target_compile_definitions(node_separator PRIVATE "-DMODE_NODESEP")
target_link_libraries(node_separator PRIVATE OpenMP::OpenMP_CXX)
target_link_libraries(
    node_separator
    PRIVATE kahip_options kahip_warnings argtable3
)
install(TARGETS node_separator DESTINATION bin)

add_executable(label_propagation app/label_propagation.cpp)
target_link_libraries(label_propagation PRIVATE kahip_core_obj kahip_mapping_obj)
target_compile_definitions(label_propagation PRIVATE "-DMODE_LABELPROPAGATION")
target_link_libraries(label_propagation PRIVATE OpenMP::OpenMP_CXX)
target_link_libraries(
    label_propagation
    PRIVATE kahip_options kahip_warnings argtable3
)
install(TARGETS label_propagation DESTINATION bin)

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
    PRIVATE "-DMODE_PARTITIONTOVERTEXSEPARATOR"
)
target_link_libraries(partition_to_vertex_separator PRIVATE OpenMP::OpenMP_CXX)
target_link_libraries(
    partition_to_vertex_separator
    PRIVATE kahip_options kahip_warnings argtable3
)
install(TARGETS partition_to_vertex_separator DESTINATION bin)

add_executable(
    interface_test
    misc/example_library_call/interface_test.cpp
    interface/kaHIP_interface.cpp
)
target_link_libraries(
    interface_test
    PRIVATE kahip_core_obj kahip_mapping_obj kahip_ordering_obj kahip_spac_obj
)
target_include_directories(
    interface_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/interface
)
target_compile_definitions(interface_test PRIVATE "-DMODE_KAFFPA")
target_link_libraries(interface_test PRIVATE OpenMP::OpenMP_CXX)
target_link_libraries(
    interface_test
    PRIVATE kahip_options kahip_warnings argtable3
)
if(metis_FOUND)
    target_link_libraries(interface_test PUBLIC metis GKlib)
endif()
install(TARGETS interface_test DESTINATION bin)

if(NOT NOMPI)
    add_executable(kaffpaE app/kaffpaE.cpp)
    target_link_libraries(
        kaffpaE
        PRIVATE kahip_core_obj kahip_mapping_obj kahip_collective_obj
    )
    target_compile_definitions(kaffpaE PRIVATE "-DMODE_KAFFPAE")
    target_link_libraries(kaffpaE PRIVATE MPI::MPI_CXX OpenMP::OpenMP_CXX)
    target_link_libraries(
        kaffpaE
        PRIVATE kahip_options kahip_warnings argtable3
    )
    install(TARGETS kaffpaE DESTINATION bin)
endif()

add_executable(graphchecker app/graphchecker.cpp)
target_link_libraries(graphchecker PRIVATE kahip_core_obj kahip_mapping_obj)
target_compile_definitions(graphchecker PRIVATE "-DMODE_GRAPHCHECKER")
target_link_libraries(graphchecker PRIVATE OpenMP::OpenMP_CXX)
target_link_libraries(
    graphchecker
    PRIVATE kahip_options kahip_warnings argtable3
)
install(TARGETS graphchecker DESTINATION bin)

add_executable(edge_partitioning app/spac.cpp)
target_link_libraries(edge_partitioning PRIVATE kahip_core_obj kahip_mapping_obj kahip_spac_obj)
target_compile_definitions(edge_partitioning PRIVATE "-DMODE_KAFFPA")
target_link_libraries(edge_partitioning PRIVATE OpenMP::OpenMP_CXX)
target_link_libraries(
    edge_partitioning
    PRIVATE kahip_options kahip_warnings argtable3
)
install(TARGETS edge_partitioning DESTINATION bin)

add_executable(node_ordering app/node_ordering.cpp)
target_link_libraries(node_ordering PRIVATE kahip_core_obj kahip_ordering_obj)
target_compile_definitions(
    node_ordering
    PRIVATE "-DMODE_NODESEP" "-DMODE_NODEORDERING"
)
target_link_libraries(node_ordering PRIVATE OpenMP::OpenMP_CXX)
target_link_libraries(
    node_ordering
    PRIVATE kahip_options kahip_warnings argtable3
)
install(TARGETS node_ordering DESTINATION bin)

if(metis_FOUND)
    add_executable(fast_node_ordering app/fast_node_ordering.cpp)
    target_link_libraries(
        fast_node_ordering
        PRIVATE kahip_core_obj kahip_mapping_obj kahip_ordering_obj
    )
    target_compile_definitions(
        fast_node_ordering
        PRIVATE "-DMODE_NODESEP" "-DMODE_NODEORDERING" "-DFASTORDERING"
    )
    target_link_libraries(
        fast_node_ordering
        PRIVATE OpenMP::OpenMP_CXX metis GKlib
    )
    target_link_libraries(
        fast_node_ordering
        PRIVATE kahip_options kahip_warnings argtable3
    )
    install(TARGETS fast_node_ordering DESTINATION bin)
endif()

# Shared interface library
add_library(kahip SHARED interface/kaHIP_interface.cpp)
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
target_include_directories(
    kahip
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/interface>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_definitions(kahip PRIVATE "-DMODE_KAFFPA")
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
    DESTINATION lib
    FILE_SET public_headers DESTINATION include
)

# pkg-config
configure_file("lib/kahip.pc.in" "${CMAKE_CURRENT_BINARY_DIR}/kahip.pc" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/kahip.pc" DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")


# Static interface library
add_library(kahip_static interface/kaHIP_interface.cpp)
target_link_libraries(
    kahip_static
    PRIVATE kahip_core_obj kahip_mapping_obj kahip_ordering_obj kahip_spac_obj
)
target_include_directories(
    kahip_static
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/interface>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_definitions(kahip_static PRIVATE "-DMODE_KAFFPA")
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
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)

# ParHIP
if(NOT NOMPI AND PARHIP)
    find_package(fmt CONFIG REQUIRED)
    find_package(spdlog CONFIG REQUIRED)
    find_package(Boost CONFIG REQUIRED COMPONENTS hana mp11)
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
    target_include_directories(ilp_improve PUBLIC ${GUROBI_INCLUDE_DIR})
    target_compile_definitions(ilp_improve PRIVATE "-DMODE_ILPIMPROVE")
    target_link_libraries(
        ilp_improve
        PRIVATE OpenMP::OpenMP_CXX ${GUROBI_LIBRARIES}
    )

    add_executable(ilp_exact app/ilp_exact.cpp)
    target_link_libraries(ilp_exact PRIVATE kahip_core_obj kahip_mapping_obj)
    target_include_directories(ilp_exact PUBLIC ${GUROBI_INCLUDE_DIR})
    target_compile_definitions(
        ilp_exact
        PRIVATE "-DMODE_ILPIMPROVE" "-DMODE_ILPEXACT"
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
