if(NOT OPTIMIZED_OUTPUT)
    target_compile_definitions(kahip_options INTERFACE NOOUTPUT)
endif()

if(DETERMINISTIC_PARHIP)
    target_compile_definitions(kahip_options INTERFACE DETERMINISTIC_PARHIP)
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
            unsigned long long local{};
            unsigned long long global{};
            return MPI_Allreduce_c(
                &local, &global, MPI_Count{1}, MPI_UNSIGNED_LONG_LONG,
                MPI_SUM, MPI_COMM_WORLD
            );
        }
    ]=]
    KAHIP_HAVE_MPI_ALLREDUCE_C
)
check_cxx_source_compiles(
    [=[
        #include <mpi.h>
        int main() {
            unsigned long long local{};
            unsigned long long global{};
            return MPI_Reduce_c(
                &local, &global, MPI_Count{1}, MPI_UNSIGNED_LONG_LONG,
                MPI_SUM, 0, MPI_COMM_WORLD
            );
        }
    ]=]
    KAHIP_HAVE_MPI_REDUCE_C
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
check_cxx_source_compiles(
    [=[
        #include <mpi.h>
        int main() {
            int counts[1]{};
            int displacements[1]{};
            MPI_Request request = MPI_REQUEST_NULL;
            return MPI_Ineighbor_alltoallv(
                nullptr, counts, displacements, MPI_BYTE,
                nullptr, counts, displacements, MPI_BYTE, MPI_COMM_WORLD,
                &request
            );
        }
    ]=]
    KAHIP_HAVE_MPI_INEIGHBOR_ALLTOALLV
)
if(NOT KAHIP_HAVE_MPI_INEIGHBOR_ALLTOALLV)
    message(
        FATAL_ERROR
        "KaHIP requires the MPI 3.1 MPI_Ineighbor_alltoallv binding"
    )
endif()
check_cxx_source_compiles(
    [=[
        #include <mpi.h>
        int main() {
            MPI_Count counts[1]{};
            MPI_Aint displacements[1]{};
            MPI_Request request = MPI_REQUEST_NULL;
            return MPI_Ineighbor_alltoallv_c(
                nullptr, counts, displacements, MPI_BYTE,
                nullptr, counts, displacements, MPI_BYTE, MPI_COMM_WORLD,
                &request
            );
        }
    ]=]
    KAHIP_HAVE_MPI_INEIGHBOR_ALLTOALLV_C
)
check_cxx_source_compiles(
    [=[
        #include <mpi.h>
        int main() {
            int counts[1]{};
            int displacements[1]{};
            MPI_Request request = MPI_REQUEST_NULL;
            return MPI_Neighbor_alltoallv_init(
                nullptr, counts, displacements, MPI_BYTE,
                nullptr, counts, displacements, MPI_BYTE, MPI_COMM_WORLD,
                MPI_INFO_NULL, &request
            );
        }
    ]=]
    KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_INIT
)
check_cxx_source_compiles(
    [=[
        #include <mpi.h>
        int main() {
            MPI_Count counts[1]{};
            MPI_Aint displacements[1]{};
            MPI_Request request = MPI_REQUEST_NULL;
            return MPI_Neighbor_alltoallv_init_c(
                nullptr, counts, displacements, MPI_BYTE,
                nullptr, counts, displacements, MPI_BYTE, MPI_COMM_WORLD,
                MPI_INFO_NULL, &request
            );
        }
    ]=]
    KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_INIT_C
)
cmake_pop_check_state()
set(PARHIP_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${PARHIP_GENERATED_INCLUDE_DIR}")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/kahip_mpi_capabilities.h.in"
    "${PARHIP_GENERATED_INCLUDE_DIR}/kahip_mpi_capabilities.h"
)

function(kahip_add_parhip_generated_header_set target)
    target_sources(
        ${target}
        PUBLIC
            FILE_SET parhip_generated_headers
            TYPE HEADERS
            BASE_DIRS "${PARHIP_GENERATED_INCLUDE_DIR}"
            FILES
                "${PARHIP_GENERATED_INCLUDE_DIR}/kahip_mpi_capabilities.h"
    )
endfunction()

set(
    PARHIP_HEADER_BASE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/app
    ${CMAKE_CURRENT_SOURCE_DIR}/lib
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/tools
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/partition
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/io
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/partition/uncoarsening/refinement/quotient_graph_refinement/flow_refinement
)

function(kahip_configure_parhip_object target)
    kahip_add_header_root_file_sets(
        ${target}
        parhip_header_root
        ${PARHIP_HEADER_BASE_DIRS}
    )
    kahip_add_parhip_generated_header_set(${target})
    target_link_libraries(
        ${target}
        PUBLIC kahip_options MPI::MPI_CXX
        PRIVATE kahip_warnings
    )
    target_compile_definitions(
        ${target}
        PRIVATE KAHIP_ENABLE_MPI_TRACE=$<BOOL:${KAHIP_ENABLE_MPI_TRACE}>
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
    GLOB PARHIP_MPI_HEADERS
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/communication/*.h"
)
set(
    PARHIP_APPLICATION_HEADERS
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/communication/mpi_application.h"
)
list(REMOVE_ITEM PARHIP_MPI_HEADERS ${PARHIP_APPLICATION_HEADERS})
file(
    GLOB_RECURSE PARHIP_GRAPH_HEADERS
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/data_structure/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/io/*.h"
)
list(
    REMOVE_ITEM PARHIP_CORE_HEADERS
    ${PARHIP_MPI_HEADERS}
    ${PARHIP_APPLICATION_HEADERS}
    ${PARHIP_GRAPH_HEADERS}
)
file(
    GLOB_RECURSE PARHIP_DSPAC_HEADERS
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/dspac/*.h"
)

set(LIBPARALLEL_SOURCE_FILES
    lib/parallel_label_compress/node_ordering.cpp
    lib/parallel_contraction_projection/parallel_contraction.cpp
    lib/parallel_contraction_projection/parallel_block_down_propagation.cpp
    lib/parallel_contraction_projection/parallel_projection.cpp
    lib/distributed_partitioning/distributed_partitioner.cpp
    lib/distributed_partitioning/initial_partitioning/initial_partitioning.cpp
    lib/distributed_partitioning/initial_partitioning/distributed_evolutionary_partitioning.cpp
    lib/distributed_partitioning/initial_partitioning/random_initial_partitioning.cpp
    lib/communication/mpi_tools.cpp
    lib/communication/dummy_operations.cpp
    lib/io/parallel_vector_io.cpp
    lib/tools/distributed_quality_metrics.cpp
)

set(
    PARHIP_GRAPH_SOURCE_FILES
    lib/data_structure/parallel_graph_access.cpp
    lib/data_structure/balance_management.cpp
    lib/data_structure/balance_management_refinement.cpp
    lib/data_structure/balance_management_coarsening.cpp
    lib/io/parallel_graph_io.cpp
)
add_library(parhip_graph_obj OBJECT ${PARHIP_GRAPH_SOURCE_FILES})
kahip_configure_parhip_object(parhip_graph_obj)
kahip_add_private_header_set(
    parhip_graph_obj
    "${CMAKE_CURRENT_SOURCE_DIR}"
    ${PARHIP_GRAPH_HEADERS}
)
set(
    PARHIP_MPI_SOURCE_FILES
    lib/communication/mpi_adapter.cpp
    lib/communication/mpi_neighbors.cpp
    lib/communication/mpi_failure.cpp
    lib/communication/ghost_exchange_plan.cpp
)
add_library(parhip_mpi_obj OBJECT ${PARHIP_MPI_SOURCE_FILES})
kahip_configure_parhip_object(parhip_mpi_obj)
kahip_add_private_header_set(
    parhip_mpi_obj
    "${CMAKE_CURRENT_SOURCE_DIR}"
    ${PARHIP_MPI_HEADERS}
)
target_link_libraries(
    parhip_mpi_obj
    PRIVATE kahip_fatal_diagnostics
)

add_library(
    parhip_mpi_application_obj
    OBJECT lib/communication/mpi_application.cpp
)
kahip_configure_parhip_object(parhip_mpi_application_obj)
kahip_add_private_header_set(
    parhip_mpi_application_obj
    "${CMAKE_CURRENT_SOURCE_DIR}"
    ${PARHIP_APPLICATION_HEADERS}
)
target_link_libraries(
    parhip_mpi_application_obj
    PRIVATE kahip_fatal_diagnostics
)

add_library(parhip_core_obj OBJECT ${LIBPARALLEL_SOURCE_FILES})
kahip_configure_parhip_object(parhip_core_obj)
kahip_add_private_header_set(
    parhip_core_obj
    "${CMAKE_CURRENT_SOURCE_DIR}"
    ${PARHIP_CORE_HEADERS}
)
target_link_libraries(
    parhip_core_obj
    PRIVATE
        libmodified_kahip_interface
        argtable3
        kahip_version
        kahip_fatal_diagnostics
)

add_library(parallel STATIC)
kahip_add_header_root_file_sets(
    parallel
    parhip_header_root
    ${PARHIP_HEADER_BASE_DIRS}
)
kahip_add_parhip_generated_header_set(parallel)
target_link_libraries(
    parallel
    PRIVATE
        parhip_graph_obj
        parhip_core_obj
        parhip_mpi_obj
        kahip_options
        kahip_warnings
)
target_compile_definitions(
    parallel
    PUBLIC KAHIP_ENABLE_MPI_TRACE=$<BOOL:${KAHIP_ENABLE_MPI_TRACE}>
)
target_link_libraries(parallel PUBLIC libmodified_kahip_interface)
target_link_libraries(parallel PUBLIC argtable3)
target_link_libraries(parallel PUBLIC kahip_version)
target_link_libraries(parallel PUBLIC MPI::MPI_CXX)

add_library(libedgelist STATIC)
target_link_libraries(
    libedgelist
    PRIVATE
        parhip_graph_obj
        parhip_mpi_obj
        kahip_options
        kahip_warnings
)
target_link_libraries(libedgelist PUBLIC MPI::MPI_CXX argtable3)
kahip_add_header_root_file_sets(
    libedgelist
    parhip_header_root
    ${PARHIP_HEADER_BASE_DIRS}
)

set(LIBDSPAC_SOURCE_FILES
    lib/dspac/dspac.cpp
    lib/dspac/edge_balanced_graph_io.cpp
)
add_library(parhip_dspac_obj OBJECT ${LIBDSPAC_SOURCE_FILES})
kahip_configure_parhip_object(parhip_dspac_obj)
kahip_add_private_header_set(
    parhip_dspac_obj
    "${CMAKE_CURRENT_SOURCE_DIR}"
    ${PARHIP_DSPAC_HEADERS}
)
add_library(libdspac STATIC)
target_link_libraries(
    libdspac
    PRIVATE parhip_dspac_obj kahip_options kahip_warnings
)
target_link_libraries(libdspac PUBLIC MPI::MPI_CXX)
kahip_add_header_root_file_sets(
    libdspac
    parhip_header_root
    ${PARHIP_HEADER_BASE_DIRS}
)

add_executable(parhip app/parhip.cpp)
target_compile_definitions(
    parhip
    PRIVATE
        GRAPH_GENERATOR_MPI
        GRAPHGEN_DISTRIBUTED_MEMORY
        PARALLEL_LABEL_COMPRESSION
)
target_link_libraries(parhip PRIVATE kahip_options kahip_warnings)
target_link_libraries(parhip PRIVATE parhip_mpi_application_obj parallel)
target_link_libraries(parhip PRIVATE MPI::MPI_CXX)
target_link_libraries(parhip PRIVATE argtable3)
install(TARGETS parhip RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

add_executable(toolbox app/toolbox.cpp)
target_compile_definitions(
    toolbox
    PRIVATE GRAPH_GENERATOR_MPI GRAPHGEN_DISTRIBUTED_MEMORY TOOLBOX
)
target_link_libraries(toolbox PRIVATE parhip_mpi_application_obj parallel)
target_link_libraries(toolbox PRIVATE MPI::MPI_CXX)
target_link_libraries(toolbox PRIVATE kahip_options kahip_warnings)
install(TARGETS toolbox RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

add_executable(graph2binary app/graph2binary.cpp)
target_compile_definitions(
    graph2binary
    PRIVATE
        GRAPH_GENERATOR_MPI
        GRAPHGEN_DISTRIBUTED_MEMORY
        PARALLEL_LABEL_COMPRESSION
        GRAPH2DGF
)
target_link_libraries(
    graph2binary
    PRIVATE parhip_mpi_application_obj parhip_graph_obj parhip_mpi_obj
)
target_link_libraries(graph2binary PRIVATE kahip_version)
target_link_libraries(graph2binary PRIVATE MPI::MPI_CXX)
target_link_libraries(graph2binary PRIVATE kahip_options kahip_warnings)
install(TARGETS graph2binary RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

add_executable(graph2binary_external app/graph2binary_external.cpp)
target_compile_definitions(
    graph2binary_external
    PRIVATE
        GRAPHGEN_DISTRIBUTED_MEMORY
        PARALLEL_LABEL_COMPRESSION
        GRAPH2DGF
)
target_link_libraries(
    graph2binary_external
    PRIVATE parhip_mpi_application_obj parhip_graph_obj parhip_mpi_obj
)
target_link_libraries(graph2binary_external PRIVATE kahip_version)
target_link_libraries(graph2binary_external PRIVATE kahip_options kahip_warnings)
install(
    TARGETS graph2binary_external
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

add_executable(readbgf app/readbgf.cpp)
target_compile_definitions(
    readbgf
    PRIVATE
        GRAPH_GENERATOR_MPI
        GRAPHGEN_DISTRIBUTED_MEMORY
        PARALLEL_LABEL_COMPRESSION
)
target_link_libraries(
    readbgf
    PRIVATE parhip_mpi_application_obj parhip_graph_obj parhip_mpi_obj
)
target_link_libraries(readbgf PRIVATE kahip_version)
target_link_libraries(readbgf PRIVATE kahip_options kahip_warnings)
install(TARGETS readbgf RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

add_executable(edge_list_to_metis_graph app/edge_list_to_metis_graph.cpp)
target_compile_definitions(
    edge_list_to_metis_graph
    PRIVATE
        GRAPH_GENERATOR_MPI
        GRAPHGEN_DISTRIBUTED_MEMORY
        KRONECKER_GENERATOR_PROGRAM
)
target_link_libraries(
    edge_list_to_metis_graph
    PRIVATE libmodified_kahip_interface
)
target_link_libraries(edge_list_to_metis_graph PRIVATE libedgelist)
target_link_libraries(
    edge_list_to_metis_graph
    PRIVATE parhip_mpi_application_obj
)
target_link_libraries(edge_list_to_metis_graph PRIVATE kahip_version)
target_link_libraries(edge_list_to_metis_graph PRIVATE kahip_options kahip_warnings)
install(
    TARGETS edge_list_to_metis_graph
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

add_executable(dspac app/dspac.cpp)
target_compile_definitions(
    dspac
    PRIVATE
        GRAPH_GENERATOR_MPI
        GRAPHGEN_DISTRIBUTED_MEMORY
        PARALLEL_LABEL_COMPRESSION
)
target_link_libraries(dspac PRIVATE parhip_mpi_application_obj parallel)
target_link_libraries(dspac PRIVATE libdspac)
target_link_libraries(dspac PRIVATE kahip_options kahip_warnings)
install(TARGETS dspac RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

add_library(parhip_interface SHARED interface/parhip_interface.cpp)
set_target_properties(
    parhip_interface
    PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON
)
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
        GRAPH_GENERATOR_MPI
        GRAPHGEN_DISTRIBUTED_MEMORY
        PARALLEL_LABEL_COMPRESSION
)
target_link_libraries(
    parhip_interface
    PUBLIC MPI::MPI_CXX
    PRIVATE parallel
)
target_link_libraries(
    parhip_interface PRIVATE kahip_fatal_diagnostics kahip_options kahip_warnings
)
install(
    TARGETS parhip_interface
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    FILE_SET public_headers DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)

# pkg-config
include(KahipPkgConfig)
separate_arguments(
    PARHIP_MPI_LINK_OPTIONS
    NATIVE_COMMAND
    "${MPI_CXX_LINK_FLAGS}"
)
kahip_format_mpi_pkg_config_flags(
    PARHIP_MPI_CFLAGS
    PARHIP_MPI_LIBS
    INCLUDE_DIRECTORIES ${MPI_CXX_INCLUDE_DIRS}
    COMPILE_DEFINITIONS ${MPI_CXX_COMPILE_DEFINITIONS}
    COMPILE_OPTIONS ${MPI_CXX_COMPILE_OPTIONS}
    LINK_OPTIONS ${PARHIP_MPI_LINK_OPTIONS}
    LIBRARIES ${MPI_CXX_LIBRARIES}
)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/parhip_interface.pc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/parhip_interface.pc"
    @ONLY
)
install(
    FILES "${CMAKE_CURRENT_BINARY_DIR}/parhip_interface.pc"
    DESTINATION "${KAHIP_INSTALL_PKGCONFIGDIR}"
)

add_library(
    parhip_interface_static
    STATIC
    interface/parhip_interface.cpp
)
target_sources(
    parhip_interface_static
    PUBLIC
        FILE_SET public_headers
        TYPE HEADERS
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/interface"
        FILES "${CMAKE_CURRENT_SOURCE_DIR}/interface/parhip_interface.h"
)
target_compile_definitions(
    parhip_interface_static
    PRIVATE
        GRAPH_GENERATOR_MPI
        GRAPHGEN_DISTRIBUTED_MEMORY
        PARALLEL_LABEL_COMPRESSION
)
target_link_libraries(
    parhip_interface_static
    PUBLIC MPI::MPI_CXX
    PRIVATE
        parhip_core_obj
        parhip_graph_obj
        parhip_mpi_obj
        modified_kahip_core_obj
        modified_kahip_collective_obj
        modified_kahip_evolutionary_interface_obj
        parallel
)
target_link_libraries(
    parhip_interface_static
    PRIVATE kahip_fatal_diagnostics kahip_options kahip_warnings
)
install(
    TARGETS parhip_interface_static
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
)

if(BUILD_TESTING)
    message("Building Tests")
    add_subdirectory(tests)
endif()
