cmake_minimum_required(VERSION 4.0)

if(
    NOT DEFINED VERIFIER
    OR NOT DEFINED FAKE_LAUNCHER
    OR NOT DEFINED WORK_DIRECTORY
)
    message(
        FATAL_ERROR
        "VERIFIER, FAKE_LAUNCHER, and WORK_DIRECTORY are required"
    )
endif()

file(MAKE_DIRECTORY "${WORK_DIRECTORY}")
execute_process(
    COMMAND
        "${CMAKE_COMMAND}"
        "-DMPIEXEC_EXECUTABLE=${FAKE_LAUNCHER}"
        "-DMPIEXEC_NUMPROC_FLAG=--ranks"
        "-DPROBE=parhip-interface"
        "-DMODE=zero-k"
        "-DEXPECTED_DIAGNOSTIC=MPI adapter programming failure: synthetic ParHIP diagnostic"
        -P
        "${VERIFIER}"
    RESULT_VARIABLE verifier_result
    OUTPUT_VARIABLE verifier_stdout
    ERROR_VARIABLE verifier_stderr
    TIMEOUT 5
)

set(verifier_output "${verifier_stdout}\n${verifier_stderr}")
if("${verifier_result}" STREQUAL "0")
    message(
        FATAL_ERROR
        "ParHIP verifier accepted a single abort marker\n${verifier_output}"
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
        "ParHIP verifier failed for the wrong reason\n${verifier_output}"
    )
endif()
