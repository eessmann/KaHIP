if(
    NOT DEFINED MPIEXEC_EXECUTABLE
    OR NOT DEFINED MPIEXEC_NUMPROC_FLAG
    OR NOT DEFINED PROBE
    OR NOT DEFINED MODE
    OR NOT DEFINED ABORT_KIND
    OR NOT DEFINED EXPECTED_DIAGNOSTIC
)
    message(
        FATAL_ERROR
        "MPI launcher, PROBE, MODE, ABORT_KIND, and EXPECTED_DIAGNOSTIC are required"
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
    TIMEOUT 8
)

if("${probe_result}" STREQUAL "0")
    message(
        FATAL_ERROR
        "async neighborhood failure probe returned success unexpectedly\nstdout:\n${probe_stdout}\nstderr:\n${probe_stderr}"
    )
endif()

set(probe_output "${probe_stdout}\n${probe_stderr}")
string(FIND "${probe_output}" "${EXPECTED_DIAGNOSTIC}" diagnostic_offset)
if(diagnostic_offset EQUAL -1)
    message(
        FATAL_ERROR
        "missing fail-fast diagnostic '${EXPECTED_DIAGNOSTIC}' for ${MODE}\n${probe_output}"
    )
endif()

if(ABORT_KIND STREQUAL "mpi")
    string(FIND
        "${probe_output}"
        "observed MPI_Abort for ${MODE} on operation communicator"
        abort_offset
    )
    if(abort_offset EQUAL -1)
        message(
            FATAL_ERROR
            "missing operation-communicator abort marker for ${MODE}\n${probe_output}"
        )
    endif()
elseif(ABORT_KIND STREQUAL "raw")
    string(FIND
        "${probe_output}"
        "observed SIGABRT for ${MODE} raw-abort path"
        abort_offset
    )
    if(abort_offset EQUAL -1)
        message(
            FATAL_ERROR
            "missing raw-abort marker for ${MODE}\n${probe_output}"
        )
    endif()
    string(FIND "${probe_output}" "observed MPI_Abort" mpi_abort_offset)
    if(NOT mpi_abort_offset EQUAL -1)
        message(
            FATAL_ERROR
            "raw-abort path called MPI_Abort for ${MODE}\n${probe_output}"
        )
    endif()
else()
    message(FATAL_ERROR "unknown ABORT_KIND: ${ABORT_KIND}")
endif()

if(DEFINED EXPECT_INJECTED_MPI_ERROR AND EXPECT_INJECTED_MPI_ERROR)
    string(REGEX MATCH
        "injecting raw MPI error ([0-9]+) for ${MODE}"
        injection_marker
        "${probe_output}"
    )
    if(NOT injection_marker)
        message(
            FATAL_ERROR
            "missing injected MPI error marker for ${MODE}\n${probe_output}"
        )
    endif()
    set(injected_code "${CMAKE_MATCH_1}")
    string(FIND "${probe_output}" "MPI error ${injected_code}" code_offset)
    if(code_offset EQUAL -1)
        message(
            FATAL_ERROR
            "diagnostic did not retain injected MPI error ${injected_code} for ${MODE}\n${probe_output}"
        )
    endif()
endif()

string(FIND "${probe_output}" "forbidden cleanup" forbidden_offset)
if(NOT forbidden_offset EQUAL -1)
    message(
        FATAL_ERROR
        "cleanup recursion or forbidden MPI call observed for ${MODE}\n${probe_output}"
    )
endif()
