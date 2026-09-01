cmake_minimum_required(VERSION 4.0)

foreach(required IN ITEMS
        MANIFEST_PATH
        PATCH_PATH
        REPOSITORY_ROOT
        GRAPH_PATH
        PARTITION_PATH
        TRACE_BASE
        TRACE_RUN_ID
        EXPECTED_RANKS
        EXPECTED_K
        EXPECTED_PRECONFIGURATION
        EXPECTED_SEED)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

foreach(required_file IN ITEMS MANIFEST_PATH PATCH_PATH GRAPH_PATH PARTITION_PATH)
    if(NOT EXISTS "${${required_file}}")
        message(FATAL_ERROR "${required_file} does not exist: ${${required_file}}")
    endif()
endforeach()

function(manifest_variable_name key output)
    string(REPLACE "." "_" variable_name "${key}")
    string(REPLACE "-" "_" variable_name "${variable_name}")
    set(${output} "oracle_${variable_name}" PARENT_SCOPE)
endfunction()

function(require_unsigned name value)
    if(NOT value MATCHES "^[0-9]+$")
        message(FATAL_ERROR "${name} must be an unsigned integer, got '${value}'")
    endif()
endfunction()

function(require_lower_hex name value length)
    string(LENGTH "${value}" actual_length)
    if(NOT actual_length EQUAL length OR NOT value MATCHES "^[0-9a-f]+$")
        message(FATAL_ERROR
            "${name} must be ${length} lowercase hexadecimal characters"
        )
    endif()
endfunction()

require_unsigned("EXPECTED_RANKS" "${EXPECTED_RANKS}")
require_unsigned("EXPECTED_K" "${EXPECTED_K}")
require_unsigned("EXPECTED_SEED" "${EXPECTED_SEED}")
if(EXPECTED_RANKS LESS 1)
    message(FATAL_ERROR "EXPECTED_RANKS must be positive")
endif()

set(fixed_manifest_keys
    upstream_revision
    instrumentation_patch
    instrumentation_patch_sha256
    tuple.graph
    tuple.ranks
    tuple.k
    tuple.preconfiguration
    tuple.seed
    partition_sha256
    trace_format
    canonical_rank_aggregate_records
    canonical_rank_aggregate_sha256
)
set(allowed_stage_keys
    stage.graph-distribution-node
    stage.graph-distribution-edge
    stage.contraction-label
    stage.quotient-node-weight
    stage.quotient-edge
    stage.projection-request
    stage.projection-reply
    stage.ghost-update
    stage.block-propagation
    stage.final-partition
)
set(dynamic_rank_keys "")
math(EXPR last_expected_rank "${EXPECTED_RANKS} - 1")
foreach(rank RANGE 0 ${last_expected_rank})
    list(APPEND dynamic_rank_keys
        "upstream_rank${rank}_sha256"
        "candidate_rank${rank}_sha256"
    )
endforeach()
set(allowed_manifest_keys
    ${fixed_manifest_keys}
    ${dynamic_rank_keys}
    ${allowed_stage_keys}
)

file(STRINGS "${MANIFEST_PATH}" manifest_lines ENCODING UTF-8)
set(seen_manifest_keys "")
set(manifest_stage_keys "")
set(saw_manifest_title FALSE)
foreach(raw_line IN LISTS manifest_lines)
    string(STRIP "${raw_line}" line)
    if(line STREQUAL "" OR line MATCHES "^#")
        continue()
    endif()
    if(NOT line MATCHES "=")
        if(saw_manifest_title OR seen_manifest_keys OR
           NOT line MATCHES "^KaHIP .+ oracle .+$")
            message(FATAL_ERROR "malformed oracle manifest line: '${line}'")
        endif()
        set(saw_manifest_title TRUE)
        continue()
    endif()
    if(NOT line MATCHES "^([A-Za-z0-9_.-]+)=(.+)$")
        message(FATAL_ERROR "malformed oracle manifest entry: '${line}'")
    endif()
    set(key "${CMAKE_MATCH_1}")
    set(value "${CMAKE_MATCH_2}")
    list(FIND allowed_manifest_keys "${key}" allowed_index)
    if(allowed_index EQUAL -1)
        message(FATAL_ERROR "unknown oracle manifest key '${key}'")
    endif()
    list(FIND seen_manifest_keys "${key}" seen_index)
    if(NOT seen_index EQUAL -1)
        message(FATAL_ERROR "duplicate manifest key '${key}'")
    endif()
    list(APPEND seen_manifest_keys "${key}")
    if(key MATCHES "^stage\\.")
        list(APPEND manifest_stage_keys "${key}")
    endif()
    manifest_variable_name("${key}" variable_name)
    set("${variable_name}" "${value}")
endforeach()

if(NOT saw_manifest_title)
    message(FATAL_ERROR "oracle manifest is missing its provenance title")
endif()
foreach(required_key IN LISTS fixed_manifest_keys dynamic_rank_keys)
    list(FIND seen_manifest_keys "${required_key}" required_index)
    if(required_index EQUAL -1)
        message(FATAL_ERROR "oracle manifest is missing '${required_key}'")
    endif()
endforeach()
if(NOT manifest_stage_keys)
    message(FATAL_ERROR "oracle manifest contains no stage counts")
endif()

require_lower_hex("upstream revision" "${oracle_upstream_revision}" 40)
require_lower_hex(
    "instrumentation patch SHA-256"
    "${oracle_instrumentation_patch_sha256}"
    64
)
require_lower_hex("partition SHA-256" "${oracle_partition_sha256}" 64)
require_lower_hex(
    "canonical trace aggregate SHA-256"
    "${oracle_canonical_rank_aggregate_sha256}"
    64
)
require_unsigned(
    "canonical trace aggregate record count"
    "${oracle_canonical_rank_aggregate_records}"
)
foreach(rank RANGE 0 ${last_expected_rank})
    foreach(kind IN ITEMS upstream candidate)
        manifest_variable_name("${kind}_rank${rank}_sha256" hash_variable)
        require_lower_hex(
            "${kind} rank ${rank} trace SHA-256"
            "${${hash_variable}}"
            64
        )
    endforeach()
endforeach()
foreach(stage_key IN LISTS manifest_stage_keys)
    manifest_variable_name("${stage_key}" stage_count_variable)
    require_unsigned(
        "${stage_key} count" "${${stage_count_variable}}"
    )
endforeach()

if(NOT oracle_tuple_ranks STREQUAL "${EXPECTED_RANKS}")
    message(FATAL_ERROR
        "oracle tuple ranks are ${oracle_tuple_ranks}, expected ${EXPECTED_RANKS}"
    )
endif()
if(NOT oracle_tuple_k STREQUAL "${EXPECTED_K}")
    message(FATAL_ERROR
        "oracle tuple k is ${oracle_tuple_k}, expected ${EXPECTED_K}"
    )
endif()
if(NOT oracle_tuple_preconfiguration STREQUAL
   "${EXPECTED_PRECONFIGURATION}")
    message(FATAL_ERROR
        "oracle tuple preconfiguration is '${oracle_tuple_preconfiguration}', expected '${EXPECTED_PRECONFIGURATION}'"
    )
endif()
if(NOT oracle_tuple_seed STREQUAL "${EXPECTED_SEED}")
    message(FATAL_ERROR
        "oracle tuple seed is ${oracle_tuple_seed}, expected ${EXPECTED_SEED}"
    )
endif()
if(NOT oracle_trace_format STREQUAL "kahip-mpi-trace-v3")
    message(FATAL_ERROR
        "unsupported oracle trace format '${oracle_trace_format}'"
    )
endif()

get_filename_component(patch_name "${PATCH_PATH}" NAME)
if(NOT patch_name STREQUAL oracle_instrumentation_patch)
    message(FATAL_ERROR
        "instrumentation patch is '${patch_name}', manifest names '${oracle_instrumentation_patch}'"
    )
endif()
file(SHA256 "${PATCH_PATH}" actual_patch_sha256)
if(NOT actual_patch_sha256 STREQUAL oracle_instrumentation_patch_sha256)
    message(FATAL_ERROR
        "instrumentation patch SHA-256 is ${actual_patch_sha256}, expected ${oracle_instrumentation_patch_sha256}"
    )
endif()

file(REAL_PATH "${REPOSITORY_ROOT}/${oracle_tuple_graph}" oracle_graph_path)
file(REAL_PATH "${GRAPH_PATH}" actual_graph_path)
if(NOT actual_graph_path STREQUAL oracle_graph_path)
    message(FATAL_ERROR
        "oracle graph is '${actual_graph_path}', expected '${oracle_graph_path}'"
    )
endif()

file(SHA256 "${PARTITION_PATH}" actual_partition_sha256)
if(NOT actual_partition_sha256 STREQUAL oracle_partition_sha256)
    message(FATAL_ERROR
        "partition SHA-256 is ${actual_partition_sha256}, expected ${oracle_partition_sha256}"
    )
endif()

set(expected_header
    "${oracle_trace_format} upstream=${oracle_upstream_revision}"
)
set(common_trace_stem "")
set(rank_record_files "")
set(actual_total_records 0)
foreach(stage_key IN LISTS manifest_stage_keys)
    manifest_variable_name("${stage_key}" stage_variable)
    set("actual_${stage_variable}" 0)
endforeach()

foreach(rank RANGE 0 ${last_expected_rank})
    file(GLOB rank_trace_files
        "${TRACE_BASE}.run-${TRACE_RUN_ID}-*.rank${rank}.trace"
    )
    list(LENGTH rank_trace_files rank_trace_file_count)
    if(NOT rank_trace_file_count EQUAL 1)
        message(FATAL_ERROR
            "expected one trace file for rank ${rank}, found ${rank_trace_file_count}: ${rank_trace_files}"
        )
    endif()
    list(GET rank_trace_files 0 trace_file)
    string(
        REGEX REPLACE "\\.rank[0-9]+\\.trace$" "" trace_stem "${trace_file}"
    )
    if(common_trace_stem STREQUAL "")
        set(common_trace_stem "${trace_stem}")
    elseif(NOT trace_stem STREQUAL common_trace_stem)
        message(FATAL_ERROR
            "trace ranks do not share one run ID: ${common_trace_stem};${trace_stem}"
        )
    endif()

    file(SHA256 "${trace_file}" actual_rank_sha256)
    manifest_variable_name("candidate_rank${rank}_sha256" rank_hash_variable)
    if(NOT actual_rank_sha256 STREQUAL "${${rank_hash_variable}}")
        message(FATAL_ERROR
            "rank ${rank} trace SHA-256 is ${actual_rank_sha256}, expected ${${rank_hash_variable}}"
        )
    endif()

    file(READ "${trace_file}" trace_contents)
    string(FIND "${trace_contents}" "\n" header_end)
    if(header_end LESS 0)
        message(FATAL_ERROR "rank ${rank} trace has no complete header")
    endif()
    string(SUBSTRING "${trace_contents}" 0 ${header_end} rank_header)
    if(NOT rank_header STREQUAL expected_header)
        message(FATAL_ERROR
            "rank ${rank} trace header is '${rank_header}', expected '${expected_header}'"
        )
    endif()
    math(EXPR record_text_begin "${header_end} + 1")
    string(SUBSTRING "${trace_contents}" ${record_text_begin} -1 record_text)
    set(rank_record_file "${TRACE_BASE}.rank${rank}.records")
    file(WRITE "${rank_record_file}" "${record_text}")
    list(APPEND rank_record_files "${rank_record_file}")

    file(STRINGS "${trace_file}" rank_lines ENCODING UTF-8)
    list(POP_FRONT rank_lines parsed_header)
    if(NOT parsed_header STREQUAL expected_header)
        message(FATAL_ERROR "rank ${rank} trace header changed while parsing")
    endif()
    foreach(record IN LISTS rank_lines)
        if(NOT record MATCHES
           "^([a-z][a-z-]*) cycle=[0-9]+ level=[0-9]+ epoch=[a-z-]+ iteration=[0-9]+ round=[0-9]+ global=[0-9]+ owner=(-|[0-9]+) requester=(-|[0-9]+) receiver=(-|[0-9]+) key=[^ ]+( .+)?$")
            message(FATAL_ERROR
                "rank ${rank} trace contains a malformed record: '${record}'"
            )
        endif()
        set(stage_key "stage.${CMAKE_MATCH_1}")
        list(FIND manifest_stage_keys "${stage_key}" stage_index)
        if(stage_index EQUAL -1)
            message(FATAL_ERROR
                "rank ${rank} trace contains unmanifested stage '${stage_key}'"
            )
        endif()
        manifest_variable_name("${stage_key}" stage_variable)
        math(EXPR actual_total_records "${actual_total_records} + 1")
        math(
            EXPR "actual_${stage_variable}"
            "${actual_${stage_variable}} + 1"
        )
    endforeach()
endforeach()

file(GLOB all_trace_files "${TRACE_BASE}.run-${TRACE_RUN_ID}-*.rank*.trace")
list(LENGTH all_trace_files all_trace_file_count)
if(NOT all_trace_file_count EQUAL EXPECTED_RANKS)
    message(FATAL_ERROR
        "trace run produced ${all_trace_file_count} rank files, expected ${EXPECTED_RANKS}"
    )
endif()

if(NOT actual_total_records EQUAL oracle_canonical_rank_aggregate_records)
    message(FATAL_ERROR
        "canonical trace aggregate contains ${actual_total_records} records, expected ${oracle_canonical_rank_aggregate_records}"
    )
endif()
foreach(stage_key IN LISTS manifest_stage_keys)
    manifest_variable_name("${stage_key}" stage_variable)
    if(NOT actual_${stage_variable} EQUAL ${${stage_variable}})
        message(FATAL_ERROR
            "${stage_key} count is ${actual_${stage_variable}}, expected ${${stage_variable}}"
        )
    endif()
endforeach()

find_program(trace_sort_executable NAMES sort REQUIRED)
set(aggregate_path "${TRACE_BASE}.canonical.records")
execute_process(
    COMMAND
        "${CMAKE_COMMAND}" -E env "LC_ALL=C" "${trace_sort_executable}"
        ${rank_record_files}
    RESULT_VARIABLE sort_result
    OUTPUT_FILE "${aggregate_path}"
    ERROR_VARIABLE sort_error
)
if(NOT sort_result EQUAL 0)
    message(FATAL_ERROR
        "canonical trace sort failed (${sort_result}): ${sort_error}"
    )
endif()
file(SHA256 "${aggregate_path}" actual_aggregate_sha256)
if(NOT actual_aggregate_sha256 STREQUAL
   oracle_canonical_rank_aggregate_sha256)
    message(FATAL_ERROR
        "canonical trace aggregate SHA-256 is ${actual_aggregate_sha256}, expected ${oracle_canonical_rank_aggregate_sha256}"
    )
endif()

message(STATUS
    "exact MPI oracle verified: partition=${actual_partition_sha256} records=${actual_total_records} trace=${actual_aggregate_sha256}"
)
