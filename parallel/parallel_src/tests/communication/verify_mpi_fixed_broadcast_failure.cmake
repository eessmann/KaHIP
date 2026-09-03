cmake_minimum_required(VERSION 4.0)

if(
    NOT DEFINED MPIEXEC_EXECUTABLE
    OR NOT DEFINED MPIEXEC_NUMPROC_FLAG
    OR NOT DEFINED PROBE
    OR NOT DEFINED MODE
    OR NOT DEFINED EXPECTED_DIAGNOSTIC
    OR NOT DEFINED FIXTURE
)
    message(
        FATAL_ERROR
        "MPI launcher, PROBE, MODE, EXPECTED_DIAGNOSTIC, and FIXTURE are required"
    )
endif()

file(REMOVE "${FIXTURE}")
execute_process(
    COMMAND
        "${MPIEXEC_EXECUTABLE}" "${MPIEXEC_NUMPROC_FLAG}" 2
        ${MPIEXEC_PREFLAGS}
        "${PROBE}"
        ${MPIEXEC_POSTFLAGS}
        "${MODE}" "${FIXTURE}"
    RESULT_VARIABLE probe_result
    OUTPUT_VARIABLE probe_stdout
    ERROR_VARIABLE probe_stderr
    TIMEOUT 8
)
file(REMOVE "${FIXTURE}")

set(probe_output "${probe_stdout}\n${probe_stderr}")
if("${probe_result}" STREQUAL "0")
    message(FATAL_ERROR "failure probe returned success\n${probe_output}")
endif()
if("${probe_result}" MATCHES "[Tt]imeout")
    message(FATAL_ERROR "failure probe timed out\n${probe_output}")
endif()

string(
    REGEX MATCHALL
    "observed fixed-broadcast MPI_Abort on affected communicator"
    abort_markers
    "${probe_output}"
)
list(LENGTH abort_markers abort_count)
if(NOT abort_count EQUAL 2)
    message(
        FATAL_ERROR
        "expected exactly 2 affected-communicator abort markers; found ${abort_count}\n${probe_output}"
    )
endif()

string(FIND "${probe_output}" "${EXPECTED_DIAGNOSTIC}" diagnostic_offset)
if(diagnostic_offset EQUAL -1)
    message(
        FATAL_ERROR
        "missing '${EXPECTED_DIAGNOSTIC}' diagnostic\n${probe_output}"
    )
endif()

foreach(
    forbidden
    IN ITEMS
        "unexpected state"
        "returned without fail-fast"
)
    string(FIND "${probe_output}" "${forbidden}" forbidden_offset)
    if(NOT forbidden_offset EQUAL -1)
        message(FATAL_ERROR "failure probe used ${forbidden}\n${probe_output}")
    endif()
endforeach()
