cmake_minimum_required(VERSION 4.0)

if(
    NOT DEFINED MPIEXEC_EXECUTABLE
    OR NOT DEFINED MPIEXEC_NUMPROC_FLAG
    OR NOT DEFINED PROBE
    OR NOT DEFINED MODE
    OR NOT DEFINED EXPECTED_DIAGNOSTIC
    OR NOT DEFINED EXPECTED_ABORT_MARKER
)
    message(
        FATAL_ERROR
        "MPI launcher, PROBE, MODE, EXPECTED_DIAGNOSTIC, and EXPECTED_ABORT_MARKER are required"
    )
endif()

execute_process(
    COMMAND
        "${MPIEXEC_EXECUTABLE}" "${MPIEXEC_NUMPROC_FLAG}" 2
        ${MPIEXEC_PREFLAGS}
        "${PROBE}" "${MODE}"
        ${MPIEXEC_POSTFLAGS}
    RESULT_VARIABLE probe_result
    OUTPUT_VARIABLE probe_stdout
    ERROR_VARIABLE probe_stderr
    TIMEOUT 8
)

set(probe_output "${probe_stdout}\n${probe_stderr}")
if("${probe_result}" STREQUAL "0")
    message(
        FATAL_ERROR
        "ghost-label failure probe returned success unexpectedly for ${MODE}\n${probe_output}"
    )
endif()
if("${probe_result}" MATCHES "[Tt]imeout")
    message(
        FATAL_ERROR
        "ghost-label failure probe timed out instead of terminating for ${MODE}\n${probe_output}"
    )
endif()

string(FIND "${probe_output}" "${EXPECTED_DIAGNOSTIC}" diagnostic_offset)
if(diagnostic_offset EQUAL -1)
    message(
        FATAL_ERROR
        "missing exact ghost-label fail-fast diagnostic '${EXPECTED_DIAGNOSTIC}' for ${MODE}\n${probe_output}"
    )
endif()

string(FIND "${probe_output}" "${EXPECTED_ABORT_MARKER}" abort_offset)
if(abort_offset EQUAL -1)
    message(
        FATAL_ERROR
        "missing ghost-label abort-state marker '${EXPECTED_ABORT_MARKER}' for ${MODE}\n${probe_output}"
    )
endif()

foreach(
    forbidden_marker
    IN ITEMS
        "unexpected callback state"
        "returned without fail-fast"
        "posted its first payload"
        "returned normally"
        "escaped as a recoverable mpi_error"
        "escaped as a C++ exception"
)
    string(FIND "${probe_output}" "${forbidden_marker}" forbidden_offset)
    if(NOT forbidden_offset EQUAL -1)
        message(
            FATAL_ERROR
            "ghost-label failure used a non-terminal path '${forbidden_marker}' for ${MODE}\n${probe_output}"
        )
    endif()
endforeach()
