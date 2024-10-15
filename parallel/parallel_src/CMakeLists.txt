if(NOT OPTIMIZED_OUTPUT)
    add_definitions("-DNOOUTPUT")
endif()

if(DETERMINISTIC_PARHIP)
    add_definitions("-DDETERMINISTIC_PARHIP")
endif()

add_subdirectory(extern/cista)

add_library(parhip_headers INTERFACE)
target_include_directories(
    parhip_headers
    BEFORE
    INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/app
)
target_include_directories(
    parhip_headers
    BEFORE
    INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/lib
)
target_include_directories(
    parhip_headers
    BEFORE
    INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/lib/tools
)
target_include_directories(
    parhip_headers
    BEFORE
    INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/lib/partition
)
target_include_directories(
    parhip_headers
    BEFORE
    INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/lib/io
)
target_include_directories(
    parhip_headers
    BEFORE
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/partition/uncoarsening/refinement/quotient_graph_refinement/flow_refinement
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
    lib/communication/mpi_tools.cpp
    lib/communication/dummy_operations.cpp
    lib/io/parallel_graph_io.cpp
    lib/io/parallel_vector_io.cpp
    lib/tools/distributed_quality_metrics.cpp
)
add_library(parallel ${LIBPARALLEL_SOURCE_FILES})
target_link_libraries(parallel PRIVATE kahip_options kahip_warnings)
target_link_libraries(parallel PUBLIC parhip_headers)
target_link_libraries(parallel PUBLIC libmodified_kahip_interface)
target_link_libraries(parallel PUBLIC argtable3)
target_link_libraries(parallel PUBLIC kahip_version)
target_link_libraries(parallel PUBLIC MPI::MPI_CXX cista::cista)

set(LIBGRAPH2BGF_SOURCE_FILES
    lib/data_structure/parallel_graph_access.cpp
    lib/io/parallel_graph_io.cpp
    lib/data_structure/balance_management.cpp
    lib/data_structure/balance_management_refinement.cpp
    lib/data_structure/balance_management_coarsening.cpp
)
add_library(libgraph2bgf OBJECT ${LIBGRAPH2BGF_SOURCE_FILES})
target_link_libraries(libgraph2bgf PUBLIC MPI::MPI_CXX)
target_link_libraries(libgraph2bgf PUBLIC parhip_headers)

set(LIBEDGELIST_SOURCE_FILES
    lib/data_structure/parallel_graph_access.cpp
    lib/io/parallel_graph_io.cpp
    lib/data_structure/balance_management.cpp
    lib/data_structure/balance_management_refinement.cpp
    lib/data_structure/balance_management_coarsening.cpp
)
add_library(libedgelist STATIC ${LIBEDGELIST_SOURCE_FILES})
target_link_libraries(libedgelist PUBLIC MPI::MPI_CXX argtable3)
target_link_libraries(libedgelist PUBLIC parhip_headers)

set(LIBDSPAC_SOURCE_FILES
    lib/dspac/dspac.cpp
    lib/dspac/edge_balanced_graph_io.cpp
)
add_library(libdspac STATIC ${LIBDSPAC_SOURCE_FILES})
target_link_libraries(libdspac PUBLIC MPI::MPI_CXX)
target_link_libraries(libdspac PUBLIC parhip_headers)

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
install(TARGETS toolbox DESTINATION bin)

add_executable(graph2binary app/graph2binary.cpp)
target_compile_definitions(
    graph2binary
    PRIVATE
        "-DGRAPH_GENERATOR_MPI -DGRAPHGEN_DISTRIBUTED_MEMORY -DPARALLEL_LABEL_COMPRESSION -DGRAPH2DGF"
)
target_link_libraries(graph2binary PRIVATE libgraph2bgf)
target_link_libraries(graph2binary PRIVATE kahip_version)
target_link_libraries(graph2binary PRIVATE MPI::MPI_CXX)
install(TARGETS graph2binary DESTINATION bin)

add_executable(graph2binary_external app/graph2binary_external.cpp)
target_compile_definitions(
    graph2binary_external
    PRIVATE
        "-DGRAPHGEN_DISTRIBUTED_MEMORY -DPARALLEL_LABEL_COMPRESSION -DGRAPH2DGF"
)
target_link_libraries(graph2binary_external PRIVATE libgraph2bgf)
target_link_libraries(graph2binary_external PRIVATE kahip_version)
install(TARGETS graph2binary_external DESTINATION bin)

add_executable(readbgf app/readbgf.cpp)
target_compile_definitions(
    readbgf
    PRIVATE
        "-DGRAPH_GENERATOR_MPI -DGRAPHGEN_DISTRIBUTED_MEMORY -DPARALLEL_LABEL_COMPRESSION"
)
target_link_libraries(readbgf PRIVATE libgraph2bgf)
target_link_libraries(readbgf PRIVATE kahip_version)
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
install(TARGETS edge_list_to_metis_graph DESTINATION bin)

#add_executable(friendster_list_to_metis_graph app/friendster_list_to_metis_graph.cpp $<TARGET_OBJECTS:libedgelist>)
#target_compile_definitions(friendster_list_to_metis_graph PRIVATE "-DGRAPH_GENERATOR_MPI -DGRAPHGEN_DISTRIBUTED_MEMORY -DKRONECKER_GENERATOR_PROGRAM")
#target_link_libraries(edge_list_to_metis_graph PRIVATE libmodified_kahip_interface)
#install(TARGETS friendster_list_to_metis_graph DESTINATION bin)

add_executable(dspac app/dspac.cpp)
target_compile_definitions(
    dspac
    PRIVATE
        "-DGRAPH_GENERATOR_MPI -DGRAPHGEN_DISTRIBUTED_MEMORY -DPARALLEL_LABEL_COMPRESSION"
)
target_link_libraries(dspac PRIVATE parallel)
target_link_libraries(dspac PRIVATE libdspac)
install(TARGETS dspac DESTINATION bin)

add_library(parhip_interface SHARED interface/parhip_interface.cpp)
target_compile_definitions(
    parhip_interface
    PRIVATE
        "-DGRAPH_GENERATOR_MPI -DGRAPHGEN_DISTRIBUTED_MEMORY -DPARALLEL_LABEL_COMPRESSION"
)
target_include_directories(
    parhip_interface
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/interface
)
target_link_libraries(parhip_interface PRIVATE parallel)
set_target_properties(
    parhip_interface
    PROPERTIES PUBLIC_HEADER interface/parhip_interface.h
)
install(
    TARGETS parhip_interface
    LIBRARY DESTINATION lib
    PUBLIC_HEADER DESTINATION include
)

add_library(parhip_interface_static interface/parhip_interface.cpp)
target_compile_definitions(
    parhip_interface_static
    PRIVATE
        "-DGRAPH_GENERATOR_MPI -DGRAPHGEN_DISTRIBUTED_MEMORY -DPARALLEL_LABEL_COMPRESSION"
)
target_include_directories(
    parhip_interface_static
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/interface
)
target_link_libraries(parhip_interface_static PRIVATE parallel)
install(TARGETS parhip_interface_static DESTINATION lib)

# Temporary Testing
set(ENABLE_TESTING ON)
if(ENABLE_TESTING)
    find_package(Catch2 REQUIRED CONFIG)
    enable_testing()
    message("Building Tests")
    add_subdirectory(tests)
endif()
