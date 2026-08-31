if(NOT DEFINED PARHIP_EXECUTABLE OR NOT DEFINED MPIEXEC_EXECUTABLE OR
   NOT DEFINED MPIEXEC_NUMPROC_FLAG OR NOT DEFINED GRAPH_PATH OR
   NOT DEFINED TRACE_BASE)
    message(FATAL_ERROR "MPI trace smoke is missing a required path")
endif()

set(trace_run_id "smoke")
set(trace_run_base "${TRACE_BASE}.run-${trace_run_id}")
file(REMOVE "${trace_run_base}.rank0.trace" "${trace_run_base}.rank1.trace")
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
foreach(rank RANGE 0 1)
    set(trace_file "${trace_run_base}.rank${rank}.trace")
    if(NOT EXISTS "${trace_file}")
        message(FATAL_ERROR "trace file was not written: ${trace_file}")
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
       "${required_stage} cycle=[0-9]+ level=[0-9]+ epoch=[a-z-]+ round=[0-9]+ global=")
        message(FATAL_ERROR
            "trace smoke is missing stage ${required_stage}"
        )
    endif()
endforeach()
