if(NOT DEFINED EXECUTABLE OR NOT DEFINED OBSERVER)
    message(FATAL_ERROR "EXECUTABLE and OBSERVER are required")
endif()

execute_process(
    COMMAND
        "${CMAKE_COMMAND}" -E env "LD_PRELOAD=${OBSERVER}" "${EXECUTABLE}"
        --help
    RESULT_VARIABLE executable_result
    OUTPUT_VARIABLE executable_stdout
    ERROR_VARIABLE executable_stderr
    TIMEOUT 10
)
set(executable_output "${executable_stdout}\n${executable_stderr}")
if(NOT "${executable_result}" STREQUAL "0")
    message(
        FATAL_ERROR
        "help path returned ${executable_result}\n${executable_output}"
    )
endif()
if(executable_output MATCHES "returned without MPI_Finalize")
    message(FATAL_ERROR "${executable_output}")
endif()
