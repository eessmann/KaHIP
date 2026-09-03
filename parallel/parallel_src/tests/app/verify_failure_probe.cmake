if(
    NOT DEFINED PROBE
    OR NOT DEFINED MODE
    OR NOT DEFINED EXPECTED_DIAGNOSTIC
    OR NOT DEFINED EXPECTED_MARKER
)
    message(
        FATAL_ERROR
        "PROBE, MODE, EXPECTED_DIAGNOSTIC, and EXPECTED_MARKER are required"
    )
endif()

execute_process(
    COMMAND "${PROBE}" "${MODE}"
    RESULT_VARIABLE probe_result
    OUTPUT_VARIABLE probe_stdout
    ERROR_VARIABLE probe_stderr
    TIMEOUT 10
)
set(probe_output "${probe_stdout}\n${probe_stderr}")
if(NOT "${probe_result}" STREQUAL "86")
    message(
        FATAL_ERROR
        "failure probe returned ${probe_result}, expected 86\n${probe_output}"
    )
endif()
if(NOT probe_output MATCHES "${EXPECTED_DIAGNOSTIC}")
    message(FATAL_ERROR "missing fail-fast diagnostic\n${probe_output}")
endif()
if(NOT probe_output MATCHES "observed synchronous diagnostic flush")
    message(FATAL_ERROR "diagnostic was not synchronously flushed\n${probe_output}")
endif()
if(NOT probe_output MATCHES "${EXPECTED_MARKER}")
    message(FATAL_ERROR "missing termination marker\n${probe_output}")
endif()
if(probe_output MATCHES "forbidden MPI call")
    message(FATAL_ERROR "${probe_output}")
endif()
