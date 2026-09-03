cmake_minimum_required(VERSION 4.0)

if(
    NOT DEFINED MPIEXEC_EXECUTABLE
    OR NOT DEFINED MPIEXEC_NUMPROC_FLAG
    OR NOT DEFINED PROBE
)
    message(FATAL_ERROR "MPI launcher and population-size PROBE are required")
endif()

execute_process(
    COMMAND
        "${MPIEXEC_EXECUTABLE}" "${MPIEXEC_NUMPROC_FLAG}" 2
        ${MPIEXEC_PREFLAGS}
        "${PROBE}"
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
        "population-size failure probe returned success unexpectedly\n${probe_output}"
    )
endif()
if("${probe_result}" MATCHES "[Tt]imeout")
    message(
        FATAL_ERROR
        "population-size failure probe timed out instead of aborting\n${probe_output}"
    )
endif()

string(
    FIND
    "${probe_output}"
    "MPI backend failure: MPI_Bcast(population size)"
    diagnostic_offset
)
if(diagnostic_offset EQUAL -1)
    message(
        FATAL_ERROR
        "missing population-size fail-fast diagnostic\n${probe_output}"
    )
endif()

string(
    FIND
    "${probe_output}"
    "observed population-size MPI_Abort on the affected subcommunicator"
    abort_offset
)
if(abort_offset EQUAL -1)
    message(
        FATAL_ERROR
        "missing population-size affected-communicator abort marker\n${probe_output}"
    )
endif()

foreach(
    forbidden_marker
    IN ITEMS
        "unexpected state"
        "returned without fail-fast"
)
    string(FIND "${probe_output}" "${forbidden_marker}" forbidden_offset)
    if(NOT forbidden_offset EQUAL -1)
        message(
            FATAL_ERROR
            "population-size failure used '${forbidden_marker}'\n${probe_output}"
        )
    endif()
endforeach()
