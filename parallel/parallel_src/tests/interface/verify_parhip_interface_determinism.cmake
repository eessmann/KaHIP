cmake_minimum_required(VERSION 4.0)

if(
    NOT DEFINED MPIEXEC_EXECUTABLE
    OR NOT DEFINED MPIEXEC_NUMPROC_FLAG
    OR NOT DEFINED PROBE
)
    message(FATAL_ERROR "MPI launcher and PROBE are required")
endif()

foreach(run_mode IN ITEMS fresh contaminated wrapped)
    execute_process(
        COMMAND
            "${MPIEXEC_EXECUTABLE}" "${MPIEXEC_NUMPROC_FLAG}" 2
            ${MPIEXEC_PREFLAGS}
            "${PROBE}"
            ${MPIEXEC_POSTFLAGS}
            "${run_mode}"
        RESULT_VARIABLE ${run_mode}_result
        OUTPUT_VARIABLE ${run_mode}_stdout
        ERROR_VARIABLE ${run_mode}_stderr
        TIMEOUT 60
    )
    if(NOT "${${run_mode}_result}" STREQUAL "0")
        message(
            FATAL_ERROR
            "${run_mode} determinism probe failed: ${${run_mode}_result}\n${${run_mode}_stdout}\n${${run_mode}_stderr}"
        )
    endif()
    string(
        REGEX MATCHALL
        "PARHIP_TARGET [0-9 ]+"
        ${run_mode}_targets
        "${${run_mode}_stdout}"
    )
endforeach()

list(LENGTH fresh_targets fresh_count)
list(LENGTH contaminated_targets contaminated_count)
list(LENGTH wrapped_targets wrapped_count)
if(NOT fresh_count EQUAL 1)
    message(
        FATAL_ERROR
        "expected one fresh target result; found ${fresh_count}\n${fresh_stdout}"
    )
endif()
if(NOT contaminated_count EQUAL 2)
    message(
        FATAL_ERROR
        "expected two repeated target results; found ${contaminated_count}\n${contaminated_stdout}"
    )
endif()
if(NOT wrapped_count EQUAL 2)
    message(
        FATAL_ERROR
        "expected two wrapped-seed target results; found ${wrapped_count}\n${wrapped_stdout}"
    )
endif()
list(GET fresh_targets 0 fresh_target)
list(GET contaminated_targets 0 first_contaminated_target)
list(GET contaminated_targets 1 second_contaminated_target)
list(GET wrapped_targets 0 first_wrapped_target)
list(GET wrapped_targets 1 second_wrapped_target)
if(NOT first_contaminated_target STREQUAL second_contaminated_target)
    message(
        FATAL_ERROR
        "same-process repeated calls diverged\nfresh=${fresh_target}\nfirst=${first_contaminated_target}\nsecond=${second_contaminated_target}"
    )
endif()
if(NOT fresh_target STREQUAL first_contaminated_target)
    message(
        FATAL_ERROR
        "prior calls changed deterministic target semantics\nfresh=${fresh_target}\ncontaminated=${first_contaminated_target}"
    )
endif()
if(NOT first_wrapped_target STREQUAL second_wrapped_target)
    message(
        FATAL_ERROR
        "same-process wrapped-seed calls diverged\nfirst=${first_wrapped_target}\nsecond=${second_wrapped_target}"
    )
endif()

string(SHA256 wrapped_target_sha256 "${first_wrapped_target}")
set(
    expected_wrapped_target_sha256
    "6f4e493d31b96144dabfd50ba90ed99b78d304c6f75b5e8cffec30fef11f0d95"
)
if(NOT wrapped_target_sha256 STREQUAL expected_wrapped_target_sha256)
    message(
        FATAL_ERROR
        "wrapped-seed target changed: expected ${expected_wrapped_target_sha256}, got ${wrapped_target_sha256}\n${first_wrapped_target}"
    )
endif()
