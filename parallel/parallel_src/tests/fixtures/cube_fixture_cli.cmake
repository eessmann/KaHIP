cmake_minimum_required(VERSION 4.0)

foreach(required IN ITEMS GENERATOR VERIFIER WORK_DIRECTORY)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

file(MAKE_DIRECTORY "${WORK_DIRECTORY}")
set(graph_path "${WORK_DIRECTORY}/cube-2x2x1.graph")
set(partition_path "${WORK_DIRECTORY}/cube-2x2x1.txtp")

execute_process(
    COMMAND "${GENERATOR}" 2 2 1 "${graph_path}"
    RESULT_VARIABLE generator_result
    OUTPUT_VARIABLE generator_stdout
    ERROR_VARIABLE generator_stderr
)
if(NOT generator_result EQUAL 0)
    message(
        FATAL_ERROR
        "cube generator failed\n${generator_stdout}\n${generator_stderr}"
    )
endif()

file(READ "${graph_path}" graph_contents)
set(expected_graph "4 4\n2 3\n1 4\n1 4\n2 3\n")
if(NOT graph_contents STREQUAL expected_graph)
    message(FATAL_ERROR "cube generator produced noncanonical adjacency")
endif()

file(WRITE "${partition_path}" "0\n0\n1\n1\n")
execute_process(
    COMMAND "${VERIFIER}" 2 2 1 2 0 "${partition_path}"
    RESULT_VARIABLE verifier_result
    OUTPUT_VARIABLE verifier_stdout
    ERROR_VARIABLE verifier_stderr
)
if(NOT verifier_result EQUAL 0)
    message(
        FATAL_ERROR
        "cube verifier failed\n${verifier_stdout}\n${verifier_stderr}"
    )
endif()
foreach(expected IN ITEMS "block-weights=[2,2]" "weighted-cut=2")
    string(FIND "${verifier_stdout}" "${expected}" expected_index)
    if(expected_index EQUAL -1)
        message(
            FATAL_ERROR
            "cube verifier omitted '${expected}'\n${verifier_stdout}"
        )
    endif()
endforeach()
