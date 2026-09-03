cmake_minimum_required(VERSION 4.0)

if(
    NOT DEFINED VERIFIER
    OR NOT DEFINED FAKE_LAUNCHER
    OR NOT DEFINED PROBE_KIND
    OR NOT DEFINED WORK_DIRECTORY
)
    message(
        FATAL_ERROR
        "VERIFIER, FAKE_LAUNCHER, PROBE_KIND, and WORK_DIRECTORY are required"
    )
endif()

file(MAKE_DIRECTORY "${WORK_DIRECTORY}")
if(PROBE_KIND STREQUAL "fixed-broadcast")
    set(expected_diagnostic "synthetic fixed-broadcast diagnostic")
    execute_process(
        COMMAND
            "${CMAKE_COMMAND}"
            "-DMPIEXEC_EXECUTABLE=${FAKE_LAUNCHER}"
            "-DMPIEXEC_NUMPROC_FLAG=--ranks"
            "-DPROBE=fixed-broadcast"
            "-DMODE=status"
            "-DEXPECTED_DIAGNOSTIC=${expected_diagnostic}"
            "-DFIXTURE=${WORK_DIRECTORY}/unused.bgf"
            -P
            "${VERIFIER}"
        RESULT_VARIABLE verifier_result
        OUTPUT_VARIABLE verifier_stdout
        ERROR_VARIABLE verifier_stderr
        TIMEOUT 5
    )
elseif(PROBE_KIND STREQUAL "vertex-cut")
    execute_process(
        COMMAND
            "${CMAKE_COMMAND}"
            "-DMPIEXEC_EXECUTABLE=${FAKE_LAUNCHER}"
            "-DMPIEXEC_NUMPROC_FLAG=--ranks"
            "-DPROBE=vertex-cut"
            "-DMODE=backend"
            "-DEXPECTED_DIAGNOSTIC=synthetic vertex-cut diagnostic"
            -P
            "${VERIFIER}"
        RESULT_VARIABLE verifier_result
        OUTPUT_VARIABLE verifier_stdout
        ERROR_VARIABLE verifier_stderr
        TIMEOUT 5
    )
else()
    message(FATAL_ERROR "unknown PROBE_KIND: ${PROBE_KIND}")
endif()

set(verifier_output "${verifier_stdout}\n${verifier_stderr}")
if("${verifier_result}" STREQUAL "0")
    message(
        FATAL_ERROR
        "${PROBE_KIND} verifier accepted a single abort marker\n${verifier_output}"
    )
endif()
string(
    FIND
    "${verifier_output}"
    "expected exactly 2 affected-communicator abort markers; found 1"
    marker_count_diagnostic
)
if(marker_count_diagnostic EQUAL -1)
    message(
        FATAL_ERROR
        "${PROBE_KIND} verifier failed for the wrong reason\n${verifier_output}"
    )
endif()
