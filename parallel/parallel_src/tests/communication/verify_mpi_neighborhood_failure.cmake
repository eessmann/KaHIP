if(
    NOT DEFINED MPIEXEC_EXECUTABLE
    OR NOT DEFINED MPIEXEC_NUMPROC_FLAG
    OR NOT DEFINED PROBE
    OR NOT DEFINED MODE
    OR NOT DEFINED EXPECTED_CONTEXT
    OR NOT DEFINED EXPECTED_AFFECTED
)
    message(
        FATAL_ERROR
        "MPI launcher, PROBE, MODE, EXPECTED_CONTEXT, and EXPECTED_AFFECTED are required"
    )
endif()

execute_process(
    COMMAND
        "${MPIEXEC_EXECUTABLE}" "${MPIEXEC_NUMPROC_FLAG}" 1
        ${MPIEXEC_PREFLAGS}
        "${PROBE}" "${MODE}"
        ${MPIEXEC_POSTFLAGS}
    RESULT_VARIABLE probe_result
    OUTPUT_VARIABLE probe_stdout
    ERROR_VARIABLE probe_stderr
    TIMEOUT 5
)

if("${probe_result}" STREQUAL "0")
    message(
        FATAL_ERROR
        "neighborhood failure probe returned success unexpectedly\nstdout:\n${probe_stdout}\nstderr:\n${probe_stderr}"
    )
endif()

set(probe_output "${probe_stdout}\n${probe_stderr}")
if(NOT probe_output MATCHES "MPI backend failure: ${EXPECTED_CONTEXT}")
    message(
        FATAL_ERROR
        "missing fail-fast diagnostic for ${EXPECTED_CONTEXT}\n${probe_output}"
    )
endif()
if(
    NOT probe_output
        MATCHES
        "observed MPI_Abort from neighborhood-${MODE} failure on ${EXPECTED_AFFECTED} communicator"
)
    message(
        FATAL_ERROR
        "missing affected-communicator abort marker for ${MODE}\n${probe_output}"
    )
endif()
