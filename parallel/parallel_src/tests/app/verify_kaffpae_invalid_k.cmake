if(NOT DEFINED KAFFPAE)
    message(FATAL_ERROR "KAFFPAE is required")
endif()

execute_process(
    COMMAND "${KAFFPAE}" missing.graph --k=0
    RESULT_VARIABLE kaffpae_result
    OUTPUT_VARIABLE kaffpae_stdout
    ERROR_VARIABLE kaffpae_stderr
    TIMEOUT 10
)
set(kaffpae_output "${kaffpae_stdout}\n${kaffpae_stderr}")
if("${kaffpae_result}" STREQUAL "0")
    message(FATAL_ERROR "invalid k incorrectly returned success\n${kaffpae_output}")
endif()
if(NOT "${kaffpae_result}" MATCHES "^[0-9]+$")
    message(
        FATAL_ERROR
        "invalid k did not return a normal failure status (result=${kaffpae_result})\n${kaffpae_output}"
    )
endif()
if(NOT kaffpae_output MATCHES "Number of blocks must be a positive int")
    message(FATAL_ERROR "missing invalid-k diagnostic\n${kaffpae_output}")
endif()
