if(NOT DEFINED PARHIP_EXECUTABLE OR NOT DEFINED MPIEXEC_EXECUTABLE OR
   NOT DEFINED MPIEXEC_NUMPROC_FLAG OR NOT DEFINED GRAPH_PATH OR
   NOT DEFINED TRACE_BASE)
    message(FATAL_ERROR "MPI trace smoke is missing a required path")
endif()

set(trace_run_id "smoke")
set(work_directory "${TRACE_BASE}.work")
set(partition_path "${work_directory}/tmppartition.txtp")
get_filename_component(
    repository_root
    "${CMAKE_CURRENT_LIST_DIR}/../../../.."
    ABSOLUTE
)
set(oracle_directory
    "${repository_root}/.superpowers/sdd/kahip-collective-mpi-modernization"
)
set(oracle_manifest "${oracle_directory}/task-5-oracle-golden.txt")
set(oracle_patch "${oracle_directory}/task-5-upstream-trace.patch")
set(oracle_verifier
    "${CMAKE_CURRENT_LIST_DIR}/verify_mpi_trace_oracle.cmake"
)

file(MAKE_DIRECTORY "${work_directory}")
file(REMOVE "${partition_path}")
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
        ${MPIEXEC_PREFLAGS}
        "${PARHIP_EXECUTABLE}"
        ${MPIEXEC_POSTFLAGS}
        "${GRAPH_PATH}"
        --k=2 --preconfiguration=ultrafastmesh --seed=0 --save_partition
    WORKING_DIRECTORY "${work_directory}"
    RESULT_VARIABLE run_result
    OUTPUT_VARIABLE run_output
    ERROR_VARIABLE run_error
)
if(NOT run_result EQUAL 0)
    message(FATAL_ERROR
        "trace smoke failed (${run_result})\n${run_output}\n${run_error}"
    )
endif()

execute_process(
    COMMAND
        "${CMAKE_COMMAND}"
        "-DMANIFEST_PATH=${oracle_manifest}"
        "-DPATCH_PATH=${oracle_patch}"
        "-DREPOSITORY_ROOT=${repository_root}"
        "-DGRAPH_PATH=${GRAPH_PATH}"
        "-DPARTITION_PATH=${partition_path}"
        "-DTRACE_BASE=${TRACE_BASE}"
        "-DTRACE_RUN_ID=${trace_run_id}"
        "-DEXPECTED_RANKS=2"
        "-DEXPECTED_K=2"
        "-DEXPECTED_PRECONFIGURATION=ultrafastmesh"
        "-DEXPECTED_SEED=0"
        -P "${oracle_verifier}"
    RESULT_VARIABLE verifier_result
    OUTPUT_VARIABLE verifier_output
    ERROR_VARIABLE verifier_error
)
if(NOT verifier_result EQUAL 0)
    message(FATAL_ERROR
        "exact MPI oracle verification failed (${verifier_result})\n"
        "${verifier_output}${verifier_error}"
    )
endif()
