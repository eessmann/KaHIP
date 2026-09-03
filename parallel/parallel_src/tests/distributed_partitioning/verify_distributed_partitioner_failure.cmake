cmake_minimum_required(VERSION 4.0)

if(
    NOT DEFINED MPIEXEC_EXECUTABLE
    OR NOT DEFINED MPIEXEC_NUMPROC_FLAG
    OR NOT DEFINED PROBE
    OR NOT DEFINED MODE
    OR NOT DEFINED EXPECTED_DIAGNOSTIC
)
    message(
        FATAL_ERROR
        "MPI launcher, PROBE, MODE, and EXPECTED_DIAGNOSTIC are required"
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
    TIMEOUT 15
)

set(probe_output "${probe_stdout}\n${probe_stderr}")
if("${probe_result}" STREQUAL "0")
    message(FATAL_ERROR "failure probe returned success\n${probe_output}")
endif()
if("${probe_result}" MATCHES "[Tt]imeout")
    message(FATAL_ERROR "failure probe timed out\n${probe_output}")
endif()

string(
    REGEX MATCHALL
    "observed distributed-partitioner MPI_Abort on affected communicator before graph mutation"
    abort_markers
    "${probe_output}"
)
list(LENGTH abort_markers abort_count)
if(NOT abort_count EQUAL 2)
    message(
        FATAL_ERROR
        "expected exactly two affected-communicator abort markers; found ${abort_count}\n${probe_output}"
    )
endif()

string(FIND "${probe_output}" "${EXPECTED_DIAGNOSTIC}" diagnostic_offset)
if(diagnostic_offset EQUAL -1)
    message(
        FATAL_ERROR
        "missing distributed-partitioner failure diagnostic '${EXPECTED_DIAGNOSTIC}'\n${probe_output}"
    )
endif()

foreach(
    forbidden
    IN ITEMS
        "unexpected state"
        "returned without fail-fast termination"
        "MPI_Finalize"
)
    string(FIND "${probe_output}" "${forbidden}" forbidden_offset)
    if(NOT forbidden_offset EQUAL -1)
        message(FATAL_ERROR "failure probe used ${forbidden}\n${probe_output}")
    endif()
endforeach()
