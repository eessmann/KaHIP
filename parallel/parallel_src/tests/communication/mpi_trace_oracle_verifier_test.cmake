cmake_minimum_required(VERSION 4.0)

foreach(required IN ITEMS VERIFIER WORK_DIRECTORY)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

set(fixture_root "${WORK_DIRECTORY}/fixture")
set(trace_base "${fixture_root}/trace")
set(run_id "smoke")
set(upstream_revision "5935f349f65f1788a9b68fcf6d853e698d86956d")
set(trace_header "kahip-mpi-trace-v3 upstream=${upstream_revision}\n")
set(rank_zero_records [=[graph-distribution-node cycle=0 level=0 epoch=input iteration=0 round=0 global=7 owner=0 requester=- receiver=0 key=owner:0 weight=3
contraction-label cycle=0 level=2 epoch=contraction iteration=0 round=0 global=7 owner=0 requester=- receiver=0 key=label:19 coarse=4
quotient-edge cycle=0 level=2 epoch=contraction iteration=0 round=0 global=4 owner=0 requester=- receiver=0 key=target:2 weight=5
projection-reply cycle=0 level=2 epoch=projection iteration=0 round=0 global=8 owner=1 requester=0 receiver=0 key=request:3 requester=0 owner=1 label=41
final-partition cycle=0 level=0 epoch=final-partition iteration=0 round=0 global=9 owner=0 requester=- receiver=0 key=partition block=1
]=])
set(rank_one_records [=[graph-distribution-edge cycle=0 level=0 epoch=input iteration=0 round=0 global=7 owner=1 requester=- receiver=1 key=target:8 weight=4
quotient-node-weight cycle=0 level=2 epoch=contraction iteration=0 round=0 global=4 owner=1 requester=- receiver=1 key=node weight=6
projection-request cycle=0 level=2 epoch=projection iteration=0 round=0 global=8 owner=1 requester=0 receiver=1 key=request:3 requester=0 owner=1
ghost-update cycle=0 level=2 epoch=projection iteration=0 round=0 global=8 owner=1 requester=- receiver=0 key=label label=41
]=])

file(REMOVE_RECURSE "${WORK_DIRECTORY}")
file(MAKE_DIRECTORY "${fixture_root}")
file(WRITE "${fixture_root}/fixture.graph" "2 1\n2\n1\n")
file(WRITE "${fixture_root}/partition.txtp" "0\n1\n")
file(WRITE "${fixture_root}/oracle.patch" "trace-only oracle patch\n")
file(WRITE
    "${trace_base}.run-${run_id}-fixture.rank0.trace"
    "${trace_header}${rank_zero_records}"
)
file(WRITE
    "${trace_base}.run-${run_id}-fixture.rank1.trace"
    "${trace_header}${rank_one_records}"
)

file(SHA256 "${fixture_root}/partition.txtp" partition_sha256)
file(SHA256 "${fixture_root}/oracle.patch" patch_sha256)
file(
    SHA256
    "${trace_base}.run-${run_id}-fixture.rank0.trace"
    rank_zero_sha256
)
file(
    SHA256
    "${trace_base}.run-${run_id}-fixture.rank1.trace"
    rank_one_sha256
)

find_program(sort_executable NAMES sort REQUIRED)
file(WRITE "${fixture_root}/rank0.records" "${rank_zero_records}")
file(WRITE "${fixture_root}/rank1.records" "${rank_one_records}")
execute_process(
    COMMAND
        "${CMAKE_COMMAND}" -E env "LC_ALL=C" "${sort_executable}"
        "${fixture_root}/rank0.records" "${fixture_root}/rank1.records"
    OUTPUT_FILE "${fixture_root}/aggregate.records"
    COMMAND_ERROR_IS_FATAL ANY
)
file(SHA256 "${fixture_root}/aggregate.records" aggregate_sha256)

set(manifest [=[KaHIP focused MPI oracle fixture
upstream_revision=@UPSTREAM@
instrumentation_patch=oracle.patch
instrumentation_patch_sha256=@PATCH_SHA@

tuple.graph=fixture.graph
tuple.ranks=2
tuple.k=2
tuple.preconfiguration=ultrafastmesh
tuple.seed=0

partition_sha256=@PARTITION_SHA@
trace_format=kahip-mpi-trace-v3
canonical_rank_aggregate_records=9
canonical_rank_aggregate_sha256=@AGGREGATE_SHA@
upstream_rank0_sha256=@RANK_ZERO_SHA@
upstream_rank1_sha256=@RANK_ONE_SHA@
candidate_rank0_sha256=@RANK_ZERO_SHA@
candidate_rank1_sha256=@RANK_ONE_SHA@

stage.graph-distribution-node=1
stage.graph-distribution-edge=1
stage.contraction-label=1
stage.quotient-node-weight=1
stage.quotient-edge=1
stage.projection-request=1
stage.projection-reply=1
stage.ghost-update=1
stage.final-partition=1
]=])
string(REPLACE "@UPSTREAM@" "${upstream_revision}" manifest "${manifest}")
string(REPLACE "@PATCH_SHA@" "${patch_sha256}" manifest "${manifest}")
string(
    REPLACE "@PARTITION_SHA@" "${partition_sha256}" manifest "${manifest}"
)
string(
    REPLACE "@AGGREGATE_SHA@" "${aggregate_sha256}" manifest "${manifest}"
)
string(
    REPLACE "@RANK_ZERO_SHA@" "${rank_zero_sha256}" manifest "${manifest}"
)
string(
    REPLACE "@RANK_ONE_SHA@" "${rank_one_sha256}" manifest "${manifest}"
)
file(WRITE "${fixture_root}/oracle.txt" "${manifest}")

function(run_verifier manifest_path patch_path result_output log_output)
    execute_process(
        COMMAND
            "${CMAKE_COMMAND}"
            "-DMANIFEST_PATH=${manifest_path}"
            "-DPATCH_PATH=${patch_path}"
            "-DREPOSITORY_ROOT=${fixture_root}"
            "-DGRAPH_PATH=${fixture_root}/fixture.graph"
            "-DPARTITION_PATH=${fixture_root}/partition.txtp"
            "-DTRACE_BASE=${trace_base}"
            "-DTRACE_RUN_ID=${run_id}"
            "-DEXPECTED_RANKS=2"
            "-DEXPECTED_K=2"
            "-DEXPECTED_PRECONFIGURATION=ultrafastmesh"
            "-DEXPECTED_SEED=0"
            -P "${VERIFIER}"
        RESULT_VARIABLE verifier_result
        OUTPUT_VARIABLE verifier_output
        ERROR_VARIABLE verifier_error
    )
    set(${result_output} "${verifier_result}" PARENT_SCOPE)
    set(${log_output} "${verifier_output}${verifier_error}" PARENT_SCOPE)
endfunction()

run_verifier(
    "${fixture_root}/oracle.txt" "${fixture_root}/oracle.patch"
    valid_result valid_log
)
if(NOT valid_result EQUAL 0)
    message(FATAL_ERROR "valid exact oracle fixture failed\n${valid_log}")
endif()

string(
    REPLACE
        "candidate_rank0_sha256=${rank_zero_sha256}"
        "candidate_rank0_sha256=0000000000000000000000000000000000000000000000000000000000000000"
        wrong_rank_manifest
        "${manifest}"
)
file(WRITE "${fixture_root}/wrong-rank.txt" "${wrong_rank_manifest}")
run_verifier(
    "${fixture_root}/wrong-rank.txt" "${fixture_root}/oracle.patch"
    wrong_rank_result wrong_rank_log
)
if(wrong_rank_result EQUAL 0 OR
   NOT wrong_rank_log MATCHES "rank 0 trace SHA-256")
    message(FATAL_ERROR
        "rank-trace corruption did not fail closed\n${wrong_rank_log}"
    )
endif()

file(WRITE
    "${fixture_root}/duplicate.txt"
    "${manifest}tuple.seed=0\n"
)
run_verifier(
    "${fixture_root}/duplicate.txt" "${fixture_root}/oracle.patch"
    duplicate_result duplicate_log
)
if(duplicate_result EQUAL 0 OR
   NOT duplicate_log MATCHES "duplicate manifest key")
    message(FATAL_ERROR
        "duplicate manifest key did not fail closed\n${duplicate_log}"
    )
endif()

file(MAKE_DIRECTORY "${fixture_root}/corrupt")
file(WRITE "${fixture_root}/corrupt/oracle.patch" "different patch\n")
run_verifier(
    "${fixture_root}/oracle.txt" "${fixture_root}/corrupt/oracle.patch"
    patch_result patch_log
)
if(patch_result EQUAL 0 OR
   NOT patch_log MATCHES "instrumentation patch SHA-256")
    message(FATAL_ERROR
        "patch-provenance corruption did not fail closed\n${patch_log}"
    )
endif()

string(
    REPLACE
        "canonical_rank_aggregate_sha256=${aggregate_sha256}"
        "canonical_rank_aggregate_sha256=0000000000000000000000000000000000000000000000000000000000000000"
        wrong_aggregate_manifest
        "${manifest}"
)
file(WRITE "${fixture_root}/wrong-aggregate.txt" "${wrong_aggregate_manifest}")
run_verifier(
    "${fixture_root}/wrong-aggregate.txt" "${fixture_root}/oracle.patch"
    aggregate_result aggregate_log
)
if(aggregate_result EQUAL 0 OR
   NOT aggregate_log MATCHES "canonical trace aggregate SHA-256")
    message(FATAL_ERROR
        "aggregate corruption did not fail closed\n${aggregate_log}"
    )
endif()
