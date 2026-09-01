cmake_minimum_required(VERSION 4.0)

if(NOT DEFINED COMMUNICATION_ROOT)
    message(FATAL_ERROR "COMMUNICATION_ROOT is required")
endif()

set(
    region_files
    mpi_collectives.h
    mpi_collectives.h
    mpi_collectives.h
    mpi_neighbors.cpp
    mpi_neighbors.h
    mpi_async_neighbors.h
)
set(
    region_names
    validate-collectively
    agree-collectively
    dense-all-to-all
    distributed-graph-rank-domain
    sync-neighbor
    async-direct
)

function(count_literal text literal output_variable)
    string(LENGTH "${literal}" literal_length)
    if(literal_length EQUAL 0)
        message(FATAL_ERROR "cannot count an empty literal")
    endif()

    set(remaining "${text}")
    set(count 0)
    while(TRUE)
        string(FIND "${remaining}" "${literal}" literal_index)
        if(literal_index EQUAL -1)
            break()
        endif()
        math(EXPR count "${count} + 1")
        math(EXPR next_index "${literal_index} + ${literal_length}")
        string(SUBSTRING "${remaining}" ${next_index} -1 remaining)
    endwhile()
    set(${output_variable} ${count} PARENT_SCOPE)
endfunction()

set(policy_violations)
list(LENGTH region_files file_count)
list(LENGTH region_names region_count)
if(NOT file_count EQUAL region_count)
    message(
        FATAL_ERROR
        "semantic exit audit configuration mismatch: ${file_count} files, ${region_count} names"
    )
endif()
math(EXPR last_region "${region_count} - 1")
foreach(region_index RANGE 0 ${last_region})
    list(GET region_files ${region_index} region_file)
    list(GET region_names ${region_index} region_name)
    file(READ "${COMMUNICATION_ROOT}/${region_file}" source)

    set(begin_marker "// KAHIP_SEMANTIC_EXIT_BEGIN(${region_name})")
    set(end_marker "// KAHIP_SEMANTIC_EXIT_END(${region_name})")
    count_literal("${source}" "${begin_marker}" begin_count)
    count_literal("${source}" "${end_marker}" end_count)
    if(NOT begin_count EQUAL 1 OR NOT end_count EQUAL 1)
        list(
            APPEND
            policy_violations
            "${region_name}: expected one ordered marker pair, found ${begin_count}/${end_count}"
        )
        continue()
    endif()

    string(FIND "${source}" "${begin_marker}" begin_index)
    string(FIND "${source}" "${end_marker}" end_index)
    string(LENGTH "${begin_marker}" begin_length)
    math(EXPR content_begin "${begin_index} + ${begin_length}")
    if(end_index LESS content_begin)
        list(APPEND policy_violations "${region_name}: marker order is reversed")
        continue()
    endif()
    math(EXPR content_length "${end_index} - ${content_begin}")
    string(SUBSTRING "${source}" ${content_begin} ${content_length} region)
    string(REGEX REPLACE "[ \t\r\n]" "" normalized_region "${region}")

    string(FIND "${normalized_region}" "throwmpi_error" direct_throw)
    string(
        FIND
        "${normalized_region}"
        "throwparhip::mpi::mpi_error"
        qualified_direct_throw
    )
    if(NOT direct_throw EQUAL -1 OR NOT qualified_direct_throw EQUAL -1)
        list(APPEND policy_violations "${region_name}: direct throw mpi_error")
    endif()

    count_literal(
        "${normalized_region}"
        "throw_collectively_agreed_semantic_error("
        helper_count
    )
    if(NOT helper_count EQUAL 1)
        list(
            APPEND
            policy_violations
            "${region_name}: expected one central semantic helper call, found ${helper_count}"
        )
    endif()
endforeach()

if(policy_violations)
    string(JOIN "\n  - " formatted_violations ${policy_violations})
    message(
        FATAL_ERROR
        "collectively agreed semantic exit policy violations:\n  - ${formatted_violations}"
    )
endif()
