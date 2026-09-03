if(NOT DEFINED PROBE OR NOT EXISTS "${PROBE}")
    message(FATAL_ERROR "cube scale probe executable is missing")
endif()

function(require_cli_failure case_name)
    execute_process(
        COMMAND "${PROBE}" ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
        TIMEOUT 10
    )
    if(result EQUAL 0)
        message(
            FATAL_ERROR
            "cube scale probe unexpectedly accepted ${case_name}: ${output}${error}"
        )
    endif()
endfunction()

require_cli_failure(no-arguments)
require_cli_failure(missing-side-value --side)
require_cli_failure(zero-side --side 0)
require_cli_failure(non-numeric-side --side not-a-number)
require_cli_failure(duplicate-side --side 4 --side 4)
require_cli_failure(unknown-option --side 4 --unknown 1)
require_cli_failure(missing-expected-ranks-value --side 4 --expected-ranks)
require_cli_failure(zero-expected-ranks --side 4 --expected-ranks 0)
require_cli_failure(singleton-rank-mismatch --side 4 --expected-ranks 2)

execute_process(
    COMMAND "${PROBE}" --side 1
    RESULT_VARIABLE omission_result
    OUTPUT_VARIABLE omission_output
    ERROR_VARIABLE omission_error
    TIMEOUT 20
)
if(NOT omission_result EQUAL 0)
    message(
        FATAL_ERROR
        "optional --expected-ranks omission failed: ${omission_output}${omission_error}"
    )
endif()
string(STRIP "${omission_output}" omission_output)
string(JSON omission_status GET "${omission_output}" status)
string(JSON omission_side GET "${omission_output}" side)
string(JSON omission_expected GET "${omission_output}" expected_ranks)
string(JSON omission_provided GET "${omission_output}" expected_ranks_provided)
if(NOT omission_status STREQUAL "pass" OR
   NOT omission_side EQUAL 1 OR
   NOT omission_expected EQUAL 1 OR
   omission_provided)
    message(
        FATAL_ERROR
        "optional --expected-ranks evidence is incorrect: ${omission_output}"
    )
endif()
