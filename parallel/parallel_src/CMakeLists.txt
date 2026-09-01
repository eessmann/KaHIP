if(NOT OPTIMIZED_OUTPUT)
    add_definitions("-DNOOUTPUT")
endif()

if(DETERMINISTIC_PARHIP)
    add_definitions("-DDETERMINISTIC_PARHIP")
endif()

include(CheckCXXSourceCompiles)
include(CMakePushCheckState)
cmake_push_check_state(RESET)
set(CMAKE_REQUIRED_LIBRARIES MPI::MPI_CXX)
check_cxx_source_compiles(
    [=[
        #include <mpi.h>
        int main() {
            MPI_Count counts[1]{};
            MPI_Aint displacements[1]{};
            return MPI_Alltoallv_c(
                nullptr, counts, displacements, MPI_BYTE,
                nullptr, counts, displacements, MPI_BYTE, MPI_COMM_WORLD
            );
        }
    ]=]
    KAHIP_HAVE_MPI_ALLTOALLV_C
)
check_cxx_source_compiles(
    [=[
        #include <mpi.h>
        int main() {
            MPI_Count counts[1]{};
            MPI_Aint displacements[1]{};
            return MPI_Neighbor_alltoallv_c(
                nullptr, counts, displacements, MPI_BYTE,
                nullptr, counts, displacements, MPI_BYTE, MPI_COMM_WORLD
            );
        }
    ]=]
    KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_C
)
cmake_pop_check_state()
set(PARHIP_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${PARHIP_GENERATED_INCLUDE_DIR}")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/kahip_mpi_capabilities.h.in"
    "${PARHIP_GENERATED_INCLUDE_DIR}/kahip_mpi_capabilities.h"
)

set(
    PARHIP_PRIVATE_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/app
    ${CMAKE_CURRENT_SOURCE_DIR}/lib
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/tools
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/partition
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/io
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/partition/uncoarsening/refinement/quotient_graph_refinement/flow_refinement
)

function(kahip_configure_parhip_object target)
    target_include_directories(
        ${target}
        BEFORE PUBLIC
            ${PARHIP_PRIVATE_INCLUDE_DIRS}
            "${PARHIP_GENERATED_INCLUDE_DIR}"
    )
    target_link_libraries(
        ${target}
        PUBLIC kahip_options MPI::MPI_CXX
        PRIVATE kahip_warnings
    )
endfunction()

file(
    GLOB_RECURSE PARHIP_CORE_HEADERS
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/app/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/*.h"
)
list(
    FILTER PARHIP_CORE_HEADERS
    EXCLUDE
    REGEX "/lib/dspac/"
)
file(
    GLOB_RECURSE PARHIP_GRAPH_IO_HEADERS
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/data_structure/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/io/*.h"
)
file(
    GLOB_RECURSE PARHIP_DSPAC_HEADERS
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/dspac/*.h"
)

set(LIBPARALLEL_SOURCE_FILES
    lib/data_structure/parallel_graph_access.cpp
    lib/data_structure/balance_management.cpp
    lib/data_structure/balance_management_refinement.cpp
    lib/data_structure/balance_management_coarsening.cpp
    lib/parallel_label_compress/node_ordering.cpp
    lib/parallel_contraction_projection/parallel_contraction.cpp
    lib/parallel_contraction_projection/parallel_block_down_propagation.cpp
    lib/parallel_contraction_projection/parallel_projection.cpp
    lib/distributed_partitioning/distributed_partitioner.cpp
    lib/distributed_partitioning/initial_partitioning/initial_partitioning.cpp
    lib/distributed_partitioning/initial_partitioning/distributed_evolutionary_partitioning.cpp
    lib/distributed_partitioning/initial_partitioning/random_initial_partitioning.cpp
    lib/communication/mpi_adapter.cpp
    lib/communication/mpi_neighbors.cpp
    lib/communication/mpi_failure.cpp
    lib/communication/mpi_tools.cpp
    lib/communication/dummy_operations.cpp
    lib/io/parallel_graph_io.cpp
    lib/io/parallel_vector_io.cpp
    lib/tools/distributed_quality_metrics.cpp
)
add_library(parhip_core_obj OBJECT ${LIBPARALLEL_SOURCE_FILES})
kahip_add_private_header_set(
    parhip_core_obj
    "${CMAKE_CURRENT_SOURCE_DIR}"
    ${PARHIP_CORE_HEADERS}
)
kahip_configure_parhip_object(parhip_core_obj)
target_compile_definitions(
    parhip_core_obj
    PRIVATE KAHIP_ENABLE_MPI_TRACE=$<BOOL:${KAHIP_ENABLE_MPI_TRACE}>
)
target_link_libraries(
    parhip_core_obj
    PRIVATE
        libmodified_kahip_interface
        argtable3
        kahip_version
        fmt::fmt
        spdlog::spdlog
        Boost::hana
        Boost::mp11
)

add_library(parallel)
target_include_directories(
    parallel
    BEFORE PUBLIC
        ${PARHIP_PRIVATE_INCLUDE_DIRS}
        "${PARHIP_GENERATED_INCLUDE_DIR}"
)
target_link_libraries(
    parallel
    PRIVATE parhip_core_obj kahip_options kahip_warnings
)
target_compile_definitions(
    parallel
    PUBLIC KAHIP_ENABLE_MPI_TRACE=$<BOOL:${KAHIP_ENABLE_MPI_TRACE}>
)
target_link_libraries(parallel PUBLIC libmodified_kahip_interface)
target_link_libraries(parallel PUBLIC argtable3)
target_link_libraries(parallel PUBLIC kahip_version)
target_link_libraries(parallel PUBLIC MPI::MPI_CXX)
target_link_libraries(
    parallel
    PRIVATE fmt::fmt spdlog::spdlog
    PUBLIC Boost::hana Boost::mp11
)

set(LIBGRAPH2BGF_SOURCE_FILES
    lib/data_structure/parallel_graph_access.cpp
    lib/io/parallel_graph_io.cpp
    lib/data_structure/balance_management.cpp
    lib/data_structure/balance_management_refinement.cpp
    lib/data_structure/balance_management_coarsening.cpp
)
add_library(parhip_graph_io_obj OBJECT ${LIBGRAPH2BGF_SOURCE_FILES})
kahip_add_private_header_set(
    parhip_graph_io_obj
    "${CMAKE_CURRENT_SOURCE_DIR}"
    ${PARHIP_GRAPH_IO_HEADERS}
)
kahip_configure_parhip_object(parhip_graph_io_obj)

set(LIBEDGELIST_SOURCE_FILES
    lib/data_structure/parallel_graph_access.cpp
    lib/io/parallel_graph_io.cpp
    lib/data_structure/balance_management.cpp
    lib/data_structure/balance_management_refinement.cpp
    lib/data_structure/balance_management_coarsening.cpp
)
add_library(parhip_edge_list_obj OBJECT ${LIBEDGELIST_SOURCE_FILES})
kahip_add_private_header_set(
    parhip_edge_list_obj
    "${CMAKE_CURRENT_SOURCE_DIR}"
    ${PARHIP_GRAPH_IO_HEADERS}
)
kahip_configure_parhip_object(parhip_edge_list_obj)

add_library(libedgelist STATIC)
target_link_libraries(
    libedgelist
    PRIVATE parhip_edge_list_obj kahip_options kahip_warnings
)
target_link_libraries(libedgelist PUBLIC MPI::MPI_CXX argtable3)
target_include_directories(
    libedgelist
    BEFORE PUBLIC ${PARHIP_PRIVATE_INCLUDE_DIRS}
)

set(LIBDSPAC_SOURCE_FILES
    lib/dspac/dspac.cpp
    lib/dspac/edge_balanced_graph_io.cpp
)
add_library(parhip_dspac_obj OBJECT ${LIBDSPAC_SOURCE_FILES})
kahip_add_private_header_set(
    parhip_dspac_obj
    "${CMAKE_CURRENT_SOURCE_DIR}"
    ${PARHIP_DSPAC_HEADERS}
)
kahip_configure_parhip_object(parhip_dspac_obj)

add_library(libdspac STATIC)
target_link_libraries(
    libdspac
    PRIVATE parhip_dspac_obj kahip_options kahip_warnings
)
target_link_libraries(libdspac PUBLIC MPI::MPI_CXX)
target_include_directories(
    libdspac
    BEFORE PUBLIC ${PARHIP_PRIVATE_INCLUDE_DIRS}
)

add_executable(parhip app/parhip.cpp)
target_compile_definitions(
    parhip
    PRIVATE
        "-DGRAPH_GENERATOR_MPI -DGRAPHGEN_DISTRIBUTED_MEMORY -DPARALLEL_LABEL_COMPRESSION"
)
target_link_libraries(parhip PRIVATE kahip_options kahip_warnings)
target_link_libraries(parhip PRIVATE parallel)
target_link_libraries(parhip PRIVATE MPI::MPI_CXX)
target_link_libraries(parhip PRIVATE argtable3)
install(TARGETS parhip DESTINATION bin)

add_executable(toolbox app/toolbox.cpp)
target_compile_definitions(
    toolbox
    PRIVATE "-DGRAPH_GENERATOR_MPI -DGRAPHGEN_DISTRIBUTED_MEMORY -DTOOLBOX"
)
target_link_libraries(toolbox PRIVATE parallel)
target_link_libraries(toolbox PRIVATE MPI::MPI_CXX)
target_link_libraries(toolbox PRIVATE kahip_options kahip_warnings)
install(TARGETS toolbox DESTINATION bin)

add_executable(graph2binary app/graph2binary.cpp)
target_compile_definitions(
    graph2binary
    PRIVATE
        "-DGRAPH_GENERATOR_MPI -DGRAPHGEN_DISTRIBUTED_MEMORY -DPARALLEL_LABEL_COMPRESSION -DGRAPH2DGF"
)
target_link_libraries(graph2binary PRIVATE parhip_graph_io_obj)
target_link_libraries(graph2binary PRIVATE kahip_version)
target_link_libraries(graph2binary PRIVATE MPI::MPI_CXX)
target_link_libraries(graph2binary PRIVATE kahip_options kahip_warnings)
install(TARGETS graph2binary DESTINATION bin)

add_executable(graph2binary_external app/graph2binary_external.cpp)
target_compile_definitions(
    graph2binary_external
    PRIVATE
        "-DGRAPHGEN_DISTRIBUTED_MEMORY -DPARALLEL_LABEL_COMPRESSION -DGRAPH2DGF"
)
target_link_libraries(graph2binary_external PRIVATE parhip_graph_io_obj)
target_link_libraries(graph2binary_external PRIVATE kahip_version)
target_link_libraries(graph2binary_external PRIVATE kahip_options kahip_warnings)
install(TARGETS graph2binary_external DESTINATION bin)

add_executable(readbgf app/readbgf.cpp)
target_compile_definitions(
    readbgf
    PRIVATE
        "-DGRAPH_GENERATOR_MPI -DGRAPHGEN_DISTRIBUTED_MEMORY -DPARALLEL_LABEL_COMPRESSION"
)
target_link_libraries(readbgf PRIVATE parhip_graph_io_obj)
target_link_libraries(readbgf PRIVATE kahip_version)
target_link_libraries(readbgf PRIVATE kahip_options kahip_warnings)
install(TARGETS readbgf DESTINATION bin)

add_executable(edge_list_to_metis_graph app/edge_list_to_metis_graph.cpp)
target_compile_definitions(
    edge_list_to_metis_graph
    PRIVATE
        "-DGRAPH_GENERATOR_MPI -DGRAPHGEN_DISTRIBUTED_MEMORY -DKRONECKER_GENERATOR_PROGRAM"
)
target_link_libraries(
    edge_list_to_metis_graph
    PRIVATE libmodified_kahip_interface
)
target_link_libraries(edge_list_to_metis_graph PRIVATE libedgelist)
target_link_libraries(edge_list_to_metis_graph PRIVATE kahip_version)
target_link_libraries(edge_list_to_metis_graph PRIVATE kahip_options kahip_warnings)
install(TARGETS edge_list_to_metis_graph DESTINATION bin)

add_executable(dspac app/dspac.cpp)
target_compile_definitions(
    dspac
    PRIVATE
        "-DGRAPH_GENERATOR_MPI -DGRAPHGEN_DISTRIBUTED_MEMORY -DPARALLEL_LABEL_COMPRESSION"
)
target_link_libraries(dspac PRIVATE parallel)
target_link_libraries(dspac PRIVATE libdspac)
target_link_libraries(dspac PRIVATE kahip_options kahip_warnings)
install(TARGETS dspac DESTINATION bin)

add_library(parhip_interface SHARED interface/parhip_interface.cpp)
target_sources(
    parhip_interface
    PUBLIC
        FILE_SET public_headers
        TYPE HEADERS
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/interface"
        FILES "${CMAKE_CURRENT_SOURCE_DIR}/interface/parhip_interface.h"
)
target_compile_definitions(
    parhip_interface
    PRIVATE
        "-DGRAPH_GENERATOR_MPI -DGRAPHGEN_DISTRIBUTED_MEMORY -DPARALLEL_LABEL_COMPRESSION"
)
target_include_directories(
    parhip_interface
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/interface>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(parhip_interface PRIVATE parallel)
target_link_libraries(parhip_interface PRIVATE kahip_options kahip_warnings)
install(
    TARGETS parhip_interface
    LIBRARY DESTINATION lib
    FILE_SET public_headers DESTINATION include
)

# pkg-config
# Collect MPI flags for the .pc file
string(REPLACE ";" " -I" PARHIP_MPI_CFLAGS "-I${MPI_CXX_INCLUDE_DIRS}")
set(PARHIP_MPI_LIBS "")
foreach(_lib ${MPI_CXX_LIBRARIES})
    if(IS_ABSOLUTE "${_lib}")
        get_filename_component(_dir "${_lib}" DIRECTORY)
        get_filename_component(_name "${_lib}" NAME_WE)
        string(REGEX REPLACE "^lib" "" _name "${_name}")
        string(APPEND PARHIP_MPI_LIBS " -L${_dir} -l${_name}")
    else()
        string(APPEND PARHIP_MPI_LIBS " ${_lib}")
    endif()
endforeach()
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/parhip_interface.pc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/parhip_interface.pc"
    @ONLY
)
install(
    FILES "${CMAKE_CURRENT_BINARY_DIR}/parhip_interface.pc"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig"
)

add_library(parhip_interface_static interface/parhip_interface.cpp)
target_compile_definitions(
    parhip_interface_static
    PRIVATE
        "-DGRAPH_GENERATOR_MPI -DGRAPHGEN_DISTRIBUTED_MEMORY -DPARALLEL_LABEL_COMPRESSION"
)
target_include_directories(
    parhip_interface_static
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/interface>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(parhip_interface_static PRIVATE parallel)
target_link_libraries(parhip_interface_static PRIVATE kahip_options kahip_warnings)
install(TARGETS parhip_interface_static DESTINATION lib)

if(BUILD_TESTING)
    message("Building Tests")
    add_subdirectory(tests)
endif()
