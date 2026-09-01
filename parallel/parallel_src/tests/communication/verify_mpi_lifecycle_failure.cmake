if(NOT DEFINED PROBE OR NOT DEFINED MODE OR NOT DEFINED EXPECTED_QUERY)
    message(FATAL_ERROR "PROBE, MODE, and EXPECTED_QUERY are required")
endif()

execute_process(
    COMMAND "${PROBE}" "${MODE}"
    RESULT_VARIABLE probe_result
    OUTPUT_VARIABLE probe_stdout
    ERROR_VARIABLE probe_stderr
    TIMEOUT 5
)

if(NOT "${probe_result}" STREQUAL "86")
    message(
        FATAL_ERROR
        "lifecycle probe did not observe std::abort (result=${probe_result})\nstdout:\n${probe_stdout}\nstderr:\n${probe_stderr}"
    )
endif()

set(probe_output "${probe_stdout}\n${probe_stderr}")
if(probe_output MATCHES "forbidden MPI call")
    message(FATAL_ERROR "${probe_output}")
endif()
if(NOT probe_output MATCHES "MPI lifecycle query failure: ${EXPECTED_QUERY} returned raw error")
    message(
        FATAL_ERROR
        "missing raw lifecycle diagnostic for ${EXPECTED_QUERY}\n${probe_output}"
    )
endif()
if(NOT probe_output MATCHES "observed SIGABRT from lifecycle-query failure")
    message(FATAL_ERROR "missing SIGABRT observation\n${probe_output}")
endif()
