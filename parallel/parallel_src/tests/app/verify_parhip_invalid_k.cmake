if(NOT DEFINED PARHIP)
    message(FATAL_ERROR "PARHIP is required")
endif()

execute_process(
    COMMAND
        "${PARHIP}" missing.graph --k=0 --preconfiguration=fastmesh
    RESULT_VARIABLE parhip_result
    OUTPUT_VARIABLE parhip_stdout
    ERROR_VARIABLE parhip_stderr
    TIMEOUT 10
)
set(parhip_output "${parhip_stdout}\n${parhip_stderr}")
if("${parhip_result}" STREQUAL "0")
    message(
        FATAL_ERROR
        "invalid k incorrectly returned success\n${parhip_output}"
    )
endif()
if(NOT "${parhip_result}" MATCHES "^[0-9]+$")
    message(
        FATAL_ERROR
        "invalid k did not return a normal failure status (result=${parhip_result})\n${parhip_output}"
    )
endif()
if(NOT parhip_output MATCHES "Number of blocks must be positive")
    message(FATAL_ERROR "missing invalid-k diagnostic\n${parhip_output}")
endif()
if(parhip_output MATCHES "Floating point exception|divide-by-zero")
    message(FATAL_ERROR "invalid k reached arithmetic\n${parhip_output}")
endif()
