set(
    MODIFIED_KAHIP_HEADER_BASE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/lib
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/tools
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/partition
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/io
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/partition/uncoarsening/refinement/quotient_graph_refinement/flow_refinement
)

function(kahip_configure_modified_object target)
    kahip_add_header_root_file_sets(
        ${target}
        modified_header_root
        ${MODIFIED_KAHIP_HEADER_BASE_DIRS}
    )
    target_link_libraries(
        ${target}
        PUBLIC kahip_options MPI::MPI_CXX
        PRIVATE kahip_warnings
    )
endfunction()

file(
    GLOB_RECURSE MODIFIED_KAHIP_CORE_HEADERS
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/app/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/*.h"
)
list(
    FILTER MODIFIED_KAHIP_CORE_HEADERS
    EXCLUDE
    REGEX "/lib/parallel_mh/"
)
file(
    GLOB_RECURSE MODIFIED_KAHIP_COLLECTIVE_HEADERS
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/parallel_mh/*.h"
)

set(LIBMODIFIED_KAFFPA_SOURCE_FILES
    lib/data_structure/graph_hierarchy.cpp
    lib/algorithms/strongly_connected_components.cpp
    lib/algorithms/topological_sort.cpp
    lib/io/graph_io.cpp
    lib/tools/quality_metrics.cpp
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
    lib/partition/uncoarsening/uncoarsening.cpp
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
    lib/partition/uncoarsening/refinement/quotient_graph_refinement/flow_refinement/flow_solving_kernel/flow_solver.cpp
    lib/partition/uncoarsening/refinement/quotient_graph_refinement/flow_refinement/flow_solving_kernel/edge_cut_flow_solver.cpp
    lib/partition/uncoarsening/refinement/quotient_graph_refinement/flow_refinement/flow_solving_kernel/timer.cpp
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
    lib/algorithms/cycle_search.cpp
    lib/partition/uncoarsening/refinement/cycle_improvements/cycle_refinement.cpp
    lib/partition/uncoarsening/refinement/tabu_search/tabu_search.cpp
)
add_library(
    modified_kahip_core_obj
    OBJECT
    ${LIBMODIFIED_KAFFPA_SOURCE_FILES}
)
kahip_configure_modified_object(modified_kahip_core_obj)
kahip_add_private_header_set(
    modified_kahip_core_obj
    "${CMAKE_CURRENT_SOURCE_DIR}"
    ${MODIFIED_KAHIP_CORE_HEADERS}
)
target_sources(
    modified_kahip_core_obj
    PRIVATE
        FILE_SET shared_bipartition_headers
        TYPE HEADERS
        BASE_DIRS "${PROJECT_SOURCE_DIR}/lib"
        FILES
            "${PROJECT_SOURCE_DIR}/lib/partition/initial_partitioning/bipartition_candidate.h"
)

set(LIBMODIFIED_KAFFPA_PARALLEL_SOURCE_FILES
    lib/parallel_mh/parallel_mh_async.cpp
    lib/parallel_mh/population.cpp
    lib/parallel_mh/galinier_combine/gal_combine.cpp
    lib/parallel_mh/galinier_combine/construct_partition.cpp
    lib/parallel_mh/exchange/exchanger.cpp
)
add_library(
    modified_kahip_collective_obj
    OBJECT
    ${LIBMODIFIED_KAFFPA_PARALLEL_SOURCE_FILES}
)
kahip_configure_modified_object(modified_kahip_collective_obj)
kahip_add_private_header_set(
    modified_kahip_collective_obj
    "${CMAKE_CURRENT_SOURCE_DIR}"
    ${MODIFIED_KAHIP_COLLECTIVE_HEADERS}
)
target_sources(
    modified_kahip_collective_obj
    PRIVATE
        FILE_SET shared_parallel_mh_headers
        TYPE HEADERS
        BASE_DIRS "${PROJECT_SOURCE_DIR}/lib"
        FILES
            "${PROJECT_SOURCE_DIR}/lib/parallel_mh/evolutionary_collectives.h"
            "${PROJECT_SOURCE_DIR}/lib/parallel_mh/population_size_broadcast.h"
)
target_link_libraries(
    modified_kahip_collective_obj
    PRIVATE kahip_fatal_diagnostics
)

add_library(
    modified_kahip_evolutionary_interface_obj
    OBJECT
    interface/kaHIP_evolutionary_interface.cpp
)
kahip_configure_modified_object(modified_kahip_evolutionary_interface_obj)
kahip_add_private_header_set(
    modified_kahip_evolutionary_interface_obj
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/interface/kaHIP_interface.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/interface/kaHIP_evolutionary_interface_internal.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/interface/kaHIP_interface_internal.h"
)
target_link_libraries(
    modified_kahip_evolutionary_interface_obj
    PRIVATE kahip_fatal_diagnostics
)

add_library(libmodified_kahip_interface STATIC interface/kaHIP_interface.cpp)
kahip_add_private_header_set(
    libmodified_kahip_interface
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/interface/kaHIP_interface.h"
)
target_link_libraries(
    libmodified_kahip_interface
    PRIVATE
        modified_kahip_core_obj
        modified_kahip_collective_obj
        modified_kahip_evolutionary_interface_obj
)
target_link_libraries(libmodified_kahip_interface PRIVATE OpenMP::OpenMP_CXX)
target_link_libraries(libmodified_kahip_interface PRIVATE MPI::MPI_CXX)
target_link_libraries(libmodified_kahip_interface PRIVATE kahip_options kahip_warnings)
kahip_add_header_root_file_sets(
    libmodified_kahip_interface
    modified_interface_header_root
    "${CMAKE_CURRENT_SOURCE_DIR}/interface"
)
