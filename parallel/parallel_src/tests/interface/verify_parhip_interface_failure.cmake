cmake_minimum_required(VERSION 4.0)

if(
    NOT DEFINED MPIEXEC_EXECUTABLE
    OR NOT DEFINED MPIEXEC_NUMPROC_FLAG
    OR NOT DEFINED PROBE
    OR NOT DEFINED MODE
    OR NOT DEFINED EXPECTED_DIAGNOSTIC
)
    message(
        FATAL_ERROR
        "MPI launcher, PROBE, MODE, and EXPECTED_DIAGNOSTIC are required"
    )
endif()

execute_process(
    COMMAND
        "${MPIEXEC_EXECUTABLE}" "${MPIEXEC_NUMPROC_FLAG}" 2
        ${MPIEXEC_PREFLAGS}
        "${PROBE}"
        ${MPIEXEC_POSTFLAGS}
        "${MODE}"
    RESULT_VARIABLE probe_result
    OUTPUT_VARIABLE probe_stdout
    ERROR_VARIABLE probe_stderr
    TIMEOUT 20
)

set(probe_output "${probe_stdout}\n${probe_stderr}")
if("${probe_result}" STREQUAL "0")
    message(FATAL_ERROR "failure probe returned success\n${probe_output}")
endif()
if("${probe_result}" MATCHES "[Tt]imeout")
    message(FATAL_ERROR "failure probe timed out\n${probe_output}")
endif()

string(
    REGEX MATCHALL
    "observed ParHIP interface MPI_Abort on affected communicator"
    abort_markers
    "${probe_output}"
)
list(LENGTH abort_markers abort_count)
if(NOT abort_count EQUAL 2)
    message(
        FATAL_ERROR
        "expected exactly 2 affected-communicator abort markers; found ${abort_count}\n${probe_output}"
    )
endif()

string(FIND "${probe_output}" "${EXPECTED_DIAGNOSTIC}" diagnostic_offset)
if(diagnostic_offset EQUAL -1)
    message(
        FATAL_ERROR
        "missing ParHIP interface failure diagnostic '${EXPECTED_DIAGNOSTIC}'\n${probe_output}"
    )
endif()

if(MODE STREQUAL "imbalanced-result")
    string(
        REGEX MATCHALL
        "PARHIP_DETAIL rank=0 ParHIP partition balance failure: total weight=13, block count=2, raw imbalance=0\\.03, quantized imbalance=3%, configured bound=7, heaviest block=0, actual weight=10, excess=3"
        detail_lines
        "${probe_output}"
    )
    list(LENGTH detail_lines detail_count)
    if(NOT detail_count EQUAL 1)
        message(
            FATAL_ERROR
            "expected exactly one complete rank-zero ParHIP balance detail; found ${detail_count}\n${probe_output}"
        )
    endif()
    string(REGEX MATCHALL "PARHIP_DETAIL rank=0 " rank_zero_details "${probe_output}")
    list(LENGTH rank_zero_details rank_zero_detail_count)
    if(NOT rank_zero_detail_count EQUAL 1)
        message(FATAL_ERROR "expected exactly one rank-zero detail\n${probe_output}")
    endif()
    string(REGEX MATCHALL "PARHIP_DETAIL rank=1 " rank_one_details "${probe_output}")
    list(LENGTH rank_one_details rank_one_detail_count)
    if(NOT rank_one_detail_count EQUAL 0)
        message(FATAL_ERROR "unexpected rank-one detail\n${probe_output}")
    endif()
    string(REGEX MATCHALL "PARHIP_DETAIL_FLUSH rank=0" rank_zero_flushes "${probe_output}")
    list(LENGTH rank_zero_flushes rank_zero_flush_count)
    if(NOT rank_zero_flush_count EQUAL 1)
        message(FATAL_ERROR "expected exactly one rank-zero detail flush\n${probe_output}")
    endif()
    string(FIND "${probe_output}" "PARHIP_DETAIL_FLUSH rank=1" rank_one_flush)
    if(NOT rank_one_flush EQUAL -1)
        message(FATAL_ERROR "unexpected rank-one detail flush\n${probe_output}")
    endif()
endif()

foreach(
    forbidden
    IN ITEMS
        "unexpected state"
        "returned without fail-fast"
        "MPI_Finalize"
)
    string(FIND "${probe_output}" "${forbidden}" forbidden_offset)
    if(NOT forbidden_offset EQUAL -1)
        message(FATAL_ERROR "failure probe used ${forbidden}\n${probe_output}")
    endif()
endforeach()
