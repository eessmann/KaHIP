if(NOT DEFINED PARHIP_EXECUTABLE OR NOT DEFINED MPIEXEC_EXECUTABLE OR
   NOT DEFINED MPIEXEC_NUMPROC_FLAG OR NOT DEFINED GRAPH_PATH OR
   NOT DEFINED TRACE_BASE)
    message(FATAL_ERROR "MPI trace smoke is missing a required path")
endif()

set(trace_run_id "smoke")
file(GLOB stale_trace_files
    "${TRACE_BASE}.run-${trace_run_id}-*.rank*.trace"
)
file(REMOVE ${stale_trace_files})
execute_process(
    COMMAND
        "${CMAKE_COMMAND}" -E env
        "KAHIP_MPI_TRACE_PATH=${TRACE_BASE}"
        "KAHIP_MPI_TRACE_RUN_ID=${trace_run_id}"
        "${MPIEXEC_EXECUTABLE}" "${MPIEXEC_NUMPROC_FLAG}" 2
        "${PARHIP_EXECUTABLE}" "${GRAPH_PATH}"
        --k=2 --preconfiguration=ultrafastmesh --seed=0
    RESULT_VARIABLE run_result
    OUTPUT_VARIABLE run_output
    ERROR_VARIABLE run_error
)
if(NOT run_result EQUAL 0)
    message(FATAL_ERROR
        "trace smoke failed (${run_result})\n${run_output}\n${run_error}"
    )
endif()

set(combined_trace "")
set(common_trace_stem "")
foreach(rank RANGE 0 1)
    file(GLOB rank_trace_files
        "${TRACE_BASE}.run-${trace_run_id}-*.rank${rank}.trace"
    )
    list(LENGTH rank_trace_files rank_trace_file_count)
    if(NOT rank_trace_file_count EQUAL 1)
        message(FATAL_ERROR
            "expected one trace file for rank ${rank}, found ${rank_trace_file_count}: ${rank_trace_files}"
        )
    endif()
    list(GET rank_trace_files 0 trace_file)
    string(REGEX REPLACE "\\.rank[0-9]+\\.trace$" "" trace_stem "${trace_file}")
    if(common_trace_stem STREQUAL "")
        set(common_trace_stem "${trace_stem}")
    elseif(NOT trace_stem STREQUAL common_trace_stem)
        message(FATAL_ERROR
            "trace ranks did not resolve a common run ID: ${common_trace_stem};${trace_stem}"
        )
    endif()
    file(READ "${trace_file}" rank_trace)
    string(APPEND combined_trace "${rank_trace}")
endforeach()

foreach(required_stage
        graph-distribution-node
        graph-distribution-edge
        contraction-label
        quotient-node-weight
        quotient-edge
        projection-request
        projection-reply
        ghost-update
        final-partition)
    if(NOT combined_trace MATCHES
       "${required_stage} cycle=[0-9]+ level=[0-9]+ epoch=[a-z-]+ iteration=[0-9]+ round=[0-9]+ global=")
        message(FATAL_ERROR
            "trace smoke is missing stage ${required_stage}"
        )
    endif()
endforeach()
