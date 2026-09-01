cmake_minimum_required(VERSION 4.0)

if(
    NOT DEFINED PROBE
    OR NOT DEFINED MODE
    OR NOT DEFINED EXPECT_FAILURE
    OR NOT DEFINED EXPECTED_MARKER
)
    message(
        FATAL_ERROR
        "PROBE, MODE, EXPECT_FAILURE, and EXPECTED_MARKER are required"
    )
endif()

if(DEFINED MPI_RANKS AND MPI_RANKS GREATER 0)
    if(NOT DEFINED MPIEXEC_EXECUTABLE OR NOT DEFINED MPIEXEC_NUMPROC_FLAG)
        message(FATAL_ERROR "MPI launcher variables are required")
    endif()
    execute_process(
        COMMAND
            "${MPIEXEC_EXECUTABLE}" "${MPIEXEC_NUMPROC_FLAG}" "${MPI_RANKS}"
            ${MPIEXEC_PREFLAGS} "${PROBE}" "${MODE}" ${MPIEXEC_POSTFLAGS}
        RESULT_VARIABLE probe_result
        OUTPUT_VARIABLE probe_stdout
        ERROR_VARIABLE probe_stderr
        TIMEOUT 8
    )
else()
    execute_process(
        COMMAND "${PROBE}" "${MODE}"
        RESULT_VARIABLE probe_result
        OUTPUT_VARIABLE probe_stdout
        ERROR_VARIABLE probe_stderr
        TIMEOUT 8
    )
endif()

set(probe_output "${probe_stdout}\n${probe_stderr}")

if(EXPECT_FAILURE)
    if("${probe_result}" STREQUAL "0")
        message(FATAL_ERROR "failure probe returned successfully\n${probe_output}")
    endif()
else()
    if(NOT "${probe_result}" STREQUAL "0")
        message(
            FATAL_ERROR
            "success control failed (result=${probe_result})\n${probe_output}"
        )
    endif()
endif()

foreach(
    forbidden_literal
    IN ITEMS
        "forbidden MPI call"
        "MPI_Comm_free(tracked duplicate)"
        "cleanup-attempts=1"
        "returned-from-failure"
        "Signal: Aborted"
        "SIGABRT"
)
    string(FIND "${probe_output}" "${forbidden_literal}" forbidden_index)
    if(NOT forbidden_index EQUAL -1)
        message(FATAL_ERROR "${probe_output}")
    endif()
endforeach()

foreach(
    required_literal
    IN ITEMS EXPECTED_DIAGNOSTIC EXPECTED_DETAIL EXPECTED_INJECTION
)
    if(DEFINED ${required_literal} AND NOT "${${required_literal}}" STREQUAL "")
        string(FIND "${probe_output}" "${${required_literal}}" required_index)
        if(required_index EQUAL -1)
            message(
                FATAL_ERROR
                "missing required output '${${required_literal}}'\n${probe_output}"
            )
        endif()
    endif()
endforeach()

function(require_literal_once required_marker)
    set(remaining_output "${probe_output}")
    set(marker_count 0)
    string(LENGTH "${required_marker}" marker_length)
    while(TRUE)
        string(FIND "${remaining_output}" "${required_marker}" marker_index)
        if(marker_index EQUAL -1)
            break()
        endif()
        math(EXPR marker_count "${marker_count} + 1")
        math(EXPR remaining_begin "${marker_index} + ${marker_length}")
        string(SUBSTRING "${remaining_output}" ${remaining_begin} -1 remaining_output)
    endwhile()
    if(NOT marker_count EQUAL 1)
        message(
            FATAL_ERROR
            "expected exactly one '${required_marker}', found ${marker_count}\n${probe_output}"
        )
    endif()
endfunction()

if(EXPECT_FAILURE AND DEFINED MPI_RANKS AND MPI_RANKS GREATER 0)
    math(EXPR last_rank "${MPI_RANKS} - 1")
    foreach(rank RANGE 0 ${last_rank})
        require_literal_once("observed MPI_Abort rank=${rank} ${EXPECTED_MARKER}")
    endforeach()
else()
    require_literal_once("${EXPECTED_MARKER}")
endif()
