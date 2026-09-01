if(NOT DEFINED PROBE OR NOT DEFINED MODE)
    message(FATAL_ERROR "PROBE and MODE are required")
endif()

execute_process(
    COMMAND "${PROBE}" "${MODE}"
    RESULT_VARIABLE probe_result
    OUTPUT_VARIABLE probe_stdout
    ERROR_VARIABLE probe_stderr
    TIMEOUT 5
)

set(probe_output "${probe_stdout}\n${probe_stderr}")
if(MODE STREQUAL "no-plan")
    if(NOT "${probe_result}" STREQUAL "0")
        message(
            FATAL_ERROR
            "graph without a plan failed after finalization\n${probe_output}"
        )
    endif()
elseif(MODE STREQUAL "cached-plan")
    if("${probe_result}" STREQUAL "0")
        message(
            FATAL_ERROR
            "cached graph plan survived finalization unexpectedly"
        )
    endif()
    if(
        NOT probe_output
            MATCHES
            "MPI adapter ownership outlived the active MPI runtime: parallel graph cached ghost plan destruction"
    )
        message(
            FATAL_ERROR
            "cached graph plan missed raw-abort diagnostic\n${probe_output}"
        )
    endif()
elseif(MODE STREQUAL "active-destructor" OR MODE STREQUAL "active-reset")
    if("${probe_result}" STREQUAL "0")
        message(
            FATAL_ERROR
            "active graph generation ${MODE} returned success unexpectedly"
        )
    endif()
    if(
        NOT probe_output
            MATCHES
            "MPI adapter programming failure: parallel graph (destroyed with an active ghost generation|reset requires idle ghost communication)"
    )
        message(
            FATAL_ERROR
            "active graph generation missed fail-fast diagnostic\n${probe_output}"
        )
    endif()
else()
    message(FATAL_ERROR "unknown lifecycle probe mode: ${MODE}")
endif()
