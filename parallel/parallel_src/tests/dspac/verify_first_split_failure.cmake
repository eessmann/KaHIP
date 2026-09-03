cmake_minimum_required(VERSION 4.0)

if(
    NOT DEFINED MPIEXEC_EXECUTABLE
    OR NOT DEFINED MPIEXEC_NUMPROC_FLAG
    OR NOT DEFINED PROBE
    OR NOT DEFINED MODE
)
    message(FATAL_ERROR "MPI launcher, PROBE, and MODE are required")
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
    message(FATAL_ERROR "DSPAC first-split failure probe returned success\n${probe_output}")
endif()
if("${probe_result}" MATCHES "[Tt]imeout")
    message(FATAL_ERROR "DSPAC first-split failure probe timed out\n${probe_output}")
endif()

string(
    REGEX MATCHALL
    "observed DSPAC MPI_Abort on affected communicator"
    abort_markers
    "${probe_output}"
)
list(LENGTH abort_markers abort_count)
if(NOT abort_count EQUAL 2)
    message(
        FATAL_ERROR
        "expected exactly one DSPAC abort marker per rank; found ${abort_count}\n${probe_output}"
    )
endif()

if(MODE STREQUAL "neighbor-payload")
    set(
        expected_diagnostic
        "MPI backend failure: MPI_Neighbor_alltoallv(_c)?\\(neighbor exchange\\)"
    )
elseif(MODE STREQUAL "projection-permutation")
    set(
        expected_diagnostic
        "MPI adapter programming failure: DSPAC projection permutation must be a bijection over local edges"
    )
elseif(MODE STREQUAL "projection-barrier")
    set(
        expected_diagnostic
        "MPI backend failure: MPI_Barrier\\(after DSPAC projection\\)"
    )
else()
    message(FATAL_ERROR "unknown DSPAC failure mode: ${MODE}")
endif()

if(NOT probe_output MATCHES "${expected_diagnostic}")
    message(
        FATAL_ERROR
        "missing DSPAC ${MODE} failure diagnostic\n${probe_output}"
    )
endif()

foreach(
    forbidden
    IN ITEMS
        "unexpected state"
        "returned without fail-fast"
        "MPI_Finalize"
)
    string(FIND "${probe_output}" "${forbidden}" forbidden_offset)
    if(NOT forbidden_offset EQUAL -1)
        message(
            FATAL_ERROR
            "DSPAC first-split failure used forbidden path '${forbidden}'\n${probe_output}"
        )
    endif()
endforeach()
