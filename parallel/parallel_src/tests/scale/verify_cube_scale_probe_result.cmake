foreach(
    required
    IN ITEMS
        PROBE
        MPIEXEC_EXECUTABLE
        MPIEXEC_NUMPROC_FLAG
        WORK_DIRECTORY
        SIDE
        RANKS
        EXPECTED_VERTICES
        EXPECTED_UNDIRECTED_EDGES
        EXPECTED_DIRECTED_EDGES
        EXPECTED_MAXIMUM_LOCAL_NODES
        EXPECTED_BOUND
        EXPECTED_PROJECT_VERSION
        EXPECTED_PLATFORM
        EXECUTION_TIMEOUT
)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "missing required probe verification input ${required}")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK_DIRECTORY}")
file(MAKE_DIRECTORY "${WORK_DIRECTORY}")
execute_process(
    COMMAND
        "${MPIEXEC_EXECUTABLE}" "${MPIEXEC_NUMPROC_FLAG}" "${RANKS}"
        ${MPIEXEC_PREFLAGS}
        "${PROBE}" --side "${SIDE}" --expected-ranks "${RANKS}"
        ${MPIEXEC_POSTFLAGS}
    WORKING_DIRECTORY "${WORK_DIRECTORY}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
    TIMEOUT "${EXECUTION_TIMEOUT}"
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "cube scale probe failed (${result}): ${output}${error}")
endif()
string(STRIP "${output}" output)
string(FIND "${output}" "
" newline)
if(newline GREATER_EQUAL 0 OR output STREQUAL "")
    message(FATAL_ERROR "probe must emit exactly one nonempty stdout line: ${output}")
endif()

set(ordered_keys
    schema
    status
    probe_version
    project_version
    source_revision
    kahip_version
    compiler_id
    compiler_version
    build_type
    cxx_standard
    mpi_standard_major
    mpi_standard_minor
    deterministic_parhip
    mpi_library
    platform
    peak_rss_source
    peak_rss_native_unit
    peak_rss_output_unit
    distribution_recipe
    cell_id_recipe
    neighbor_recipe
    digest_algorithm
    digest_collective
    side
    world_ranks
    expected_ranks
    expected_ranks_provided
    blocks
    seed
    mode
    suppress_output
    unit_node_weights
    unit_edge_weights
    global_nodes
    global_undirected_edges
    global_directed_edges
    maximum_local_nodes
    local_source_window
    cut_exchange_rounds
    cut_protocol_max_send
    cut_protocol_max_receive
    raw_imbalance
    raw_imbalance_bits
    effective_imbalance_percent
    imbalance_was_normalized
    expected_absolute_bound
    total_weight
    block_weight_sum
    heaviest_block
    heaviest_weight
    sentinel_remaining
    invalid_labels
    observer_profile_count
    serial_kernel_profiles
    profile_sequence_digest
    selected_profile_index
    selected_profile_global_nodes
    selected_profile_global_directed_edges
    selected_profile_total_node_weight
    selected_profile_maximum_node_weight
    selected_profile_total_directed_edge_weight
    selected_profile_maximum_directed_edge_weight
    selected_profile_block_count
    selected_profile_absolute_bound
    selected_profile_wire_record_bytes
    selected_profile_csr_bytes
    selected_profile_partition_bytes
    selected_profile_serial_input_bytes
    selected_profile_complete_graph_bytes
    selected_profile_structural_validation_bytes
    selected_profile_base_memory_bytes
    selected_profile_flat_payload_elements
    selected_profile_reason
    independent_cut
    c_edge_cut
    graph_digest
    partition_digest
    generation_seconds
    partition_seconds
    validation_seconds
    elapsed_seconds
    max_rank_rss_bytes
)
set(previous_position -1)
foreach(key IN LISTS ordered_keys)
    string(FIND "${output}" "\"${key}\":" position)
    if(position LESS_EQUAL previous_position)
        message(FATAL_ERROR "JSON key '${key}' is missing or out of order: ${output}")
    endif()
    set(previous_position ${position})
endforeach()

function(require_json_equal key expected)
    string(JSON actual GET "${output}" "${key}")
    if(NOT "${actual}" STREQUAL "${expected}")
        message(
            FATAL_ERROR
            "unexpected ${key}: expected '${expected}', got '${actual}'"
        )
    endif()
endfunction()

require_json_equal(schema "parhip_cube_scale_probe.v1")
require_json_equal(status "pass")
require_json_equal(probe_version "1")
require_json_equal(project_version "${EXPECTED_PROJECT_VERSION}")
require_json_equal(cxx_standard "23")
require_json_equal(peak_rss_source "getrusage(RUSAGE_SELF)")
require_json_equal(peak_rss_output_unit "bytes")
if(EXPECTED_PLATFORM STREQUAL "Linux")
    require_json_equal(platform "Linux")
    require_json_equal(peak_rss_native_unit "KiB")
elseif(EXPECTED_PLATFORM STREQUAL "Darwin")
    require_json_equal(platform "macOS")
    require_json_equal(peak_rss_native_unit "bytes")
else()
    message(FATAL_ERROR "unsupported peak RSS platform ${EXPECTED_PLATFORM}")
endif()
require_json_equal(distribution_recipe "floor-iN-over-P")
require_json_equal(cell_id_recipe "x+side*(y+side*z)")
require_json_equal(neighbor_recipe "sorted-six-face-global-ids")
require_json_equal(digest_algorithm "semantic-splitmix64-xor-v1")
require_json_equal(digest_collective "MPI_BXOR")

foreach(
    provenance_key
    IN ITEMS
        source_revision
        kahip_version
        compiler_id
        compiler_version
        build_type
        mpi_library
)
    string(JSON provenance_value GET "${output}" "${provenance_key}")
    if(provenance_value STREQUAL "")
        message(FATAL_ERROR "empty ${provenance_key} provenance: ${output}")
    endif()
endforeach()
foreach(mpi_key IN ITEMS mpi_standard_major mpi_standard_minor)
    string(JSON mpi_value GET "${output}" "${mpi_key}")
    if(mpi_value LESS 0)
        message(FATAL_ERROR "invalid ${mpi_key} provenance: ${output}")
    endif()
endforeach()
require_json_equal(side "${SIDE}")
require_json_equal(world_ranks "${RANKS}")
require_json_equal(expected_ranks "${RANKS}")
require_json_equal(expected_ranks_provided "ON")
require_json_equal(blocks "${RANKS}")
require_json_equal(seed "2022")
require_json_equal(mode "PARHIP_FASTSOCIAL")
require_json_equal(suppress_output "ON")
require_json_equal(unit_node_weights "ON")
require_json_equal(unit_edge_weights "ON")
require_json_equal(global_nodes "${EXPECTED_VERTICES}")
require_json_equal(global_undirected_edges "${EXPECTED_UNDIRECTED_EDGES}")
require_json_equal(global_directed_edges "${EXPECTED_DIRECTED_EDGES}")
require_json_equal(maximum_local_nodes "${EXPECTED_MAXIMUM_LOCAL_NODES}")
require_json_equal(local_source_window "65536")
require_json_equal(raw_imbalance_bits "3f9eb851e0000000")
require_json_equal(effective_imbalance_percent "3")
require_json_equal(imbalance_was_normalized "ON")
require_json_equal(expected_absolute_bound "${EXPECTED_BOUND}")
require_json_equal(total_weight "${EXPECTED_VERTICES}")
require_json_equal(block_weight_sum "${EXPECTED_VERTICES}")
require_json_equal(sentinel_remaining "0")
require_json_equal(invalid_labels "0")
require_json_equal(observer_profile_count "2")
require_json_equal(raw_imbalance "0.029999999329447746")
require_json_equal(selected_profile_total_node_weight "${EXPECTED_VERTICES}")
require_json_equal(selected_profile_block_count "${RANKS}")
require_json_equal(selected_profile_absolute_bound "${EXPECTED_BOUND}")
require_json_equal(selected_profile_reason "none")

string(JSON profile_count LENGTH "${output}" serial_kernel_profiles)
if(NOT profile_count EQUAL 2)
    message(FATAL_ERROR "serial_kernel_profiles must retain two entries: ${output}")
endif()
foreach(profile_index RANGE 0 1)
    foreach(
        profile_key
        IN ITEMS
            global_nodes
            global_directed_edges
            total_node_weight
            maximum_node_weight
            total_directed_edge_weight
            maximum_directed_edge_weight
            block_count
            absolute_bound
            wire_record_bytes
            csr_bytes
            partition_bytes
            serial_input_bytes
            complete_graph_bytes
            structural_validation_bytes
            base_memory_bytes
            flat_payload_elements
            reason
    )
        string(
            JSON profile_value
            ERROR_VARIABLE profile_error
            GET "${output}" serial_kernel_profiles ${profile_index} ${profile_key}
        )
        if(profile_error)
            message(
                FATAL_ERROR
                "profile ${profile_index} lacks ${profile_key}: ${output}"
            )
        endif()
    endforeach()
    string(
        JSON profile_reason
        GET "${output}" serial_kernel_profiles ${profile_index} reason
    )
    if(NOT profile_reason STREQUAL "none")
        message(FATAL_ERROR "profile ${profile_index} is unsafe: ${output}")
    endif()
endforeach()

math(EXPR expected_rounds "(${EXPECTED_MAXIMUM_LOCAL_NODES} + 65535) / 65536")
require_json_equal(cut_exchange_rounds "${expected_rounds}")
string(JSON protocol_send GET "${output}" cut_protocol_max_send)
string(JSON protocol_receive GET "${output}" cut_protocol_max_receive)
math(EXPR maximum_protocol_receive "3 * ${EXPECTED_MAXIMUM_LOCAL_NODES}")
if(protocol_send GREATER 196608 OR
   protocol_receive GREATER maximum_protocol_receive)
    message(FATAL_ERROR "cut protocol exceeded its proven envelope: ${output}")
endif()

set(profile_keys
    global_nodes
    global_directed_edges
    total_node_weight
    maximum_node_weight
    total_directed_edge_weight
    maximum_directed_edge_weight
    block_count
    absolute_bound
    wire_record_bytes
    csr_bytes
    partition_bytes
    serial_input_bytes
    complete_graph_bytes
    structural_validation_bytes
    base_memory_bytes
    flat_payload_elements
    reason
)
if(SIDE EQUAL 4 AND RANKS EQUAL 2)
    set(expected_profile_0 "32,186,64,3,224,3,2,32,4000,1748,128,1876,4296,4464,12760,437,none")
    set(expected_profile_1 "30,166,64,3,224,3,2,32,3616,1572,120,1692,3896,3984,11496,393,none")
    set(expected_profile_digest "c19db7ed8127646b,6a2fdc590bdde738,8aa1342e2808d868,fba2a9c2865c5ead")
    set(expected_partition_digest "f39009bde03f7ec0,4f1ab477ad78702b,f634b3157ca7c3c3,891f24907abaf739")
    set(expected_cut 21)
    set(expected_heaviest_block 0)
    set(expected_heaviest_weight 32)
    set(expected_protocol_send 16)
    set(expected_protocol_receive 16)
elseif(SIDE EQUAL 10 AND RANKS EQUAL 5)
    set(expected_profile_0 "127,1110,1000,20,2832,11,5,206,21824,9900,508,10408,22880,26640,71344,2475,none")
    set(expected_profile_1 "122,1056,1000,24,2770,15,5,206,20800,9428,488,9916,21816,25344,67960,2357,none")
    set(expected_profile_digest "d1cc017d6c52f1a1,b941604446a7c5ba,c9bf2c7cc32cdb58,c3d7c0808ca2cfda")
    set(expected_partition_digest "c946b0f827d53b9c,ce54601b2c5d2095,fcee7eaf8dbcd55e,3e6721a5a42fc216")
    set(expected_cut 286)
    set(expected_heaviest_block 0)
    set(expected_heaviest_weight 205)
    set(expected_protocol_send 100)
    set(expected_protocol_receive 100)
endif()
if(DEFINED expected_profile_0)
    foreach(profile_index RANGE 0 1)
        set(expected_profile_name "expected_profile_${profile_index}")
        string(REPLACE "," ";" expected_values "${${expected_profile_name}}")
        set(field_index 0)
        foreach(profile_key IN LISTS profile_keys)
            list(GET expected_values ${field_index} expected_value)
            string(
                JSON actual_value
                GET "${output}" serial_kernel_profiles ${profile_index} ${profile_key}
            )
            if(NOT actual_value STREQUAL expected_value)
                message(
                    FATAL_ERROR
                    "profile ${profile_index} ${profile_key}: expected ${expected_value}, got ${actual_value}"
                )
            endif()
            if(profile_index EQUAL 0)
                string(
                    JSON selected_value
                    GET "${output}" "selected_profile_${profile_key}"
                )
                if(NOT selected_value STREQUAL expected_value)
                    message(
                        FATAL_ERROR
                        "selected profile ${profile_key}: expected ${expected_value}, got ${selected_value}"
                    )
                endif()
            endif()
            math(EXPR field_index "${field_index} + 1")
        endforeach()
    endforeach()
    require_json_equal(selected_profile_index "0")
    require_json_equal(heaviest_block "${expected_heaviest_block}")
    require_json_equal(heaviest_weight "${expected_heaviest_weight}")
    require_json_equal(independent_cut "${expected_cut}")
    require_json_equal(c_edge_cut "${expected_cut}")
    require_json_equal(cut_protocol_max_send "${expected_protocol_send}")
    require_json_equal(cut_protocol_max_receive "${expected_protocol_receive}")
endif()

string(JSON heaviest_block GET "${output}" heaviest_block)
string(JSON heaviest_weight GET "${output}" heaviest_weight)
if(heaviest_block GREATER_EQUAL RANKS OR heaviest_weight GREATER EXPECTED_BOUND)
    message(FATAL_ERROR "invalid heaviest block evidence: ${output}")
endif()
string(JSON independent_cut GET "${output}" independent_cut)
string(JSON c_edge_cut GET "${output}" c_edge_cut)
if(NOT independent_cut STREQUAL c_edge_cut OR
   independent_cut GREATER EXPECTED_UNDIRECTED_EDGES)
    message(FATAL_ERROR "cut evidence disagrees: ${output}")
endif()

foreach(digest_key IN ITEMS profile_sequence_digest graph_digest partition_digest)
    string(JSON lane_count LENGTH "${output}" "${digest_key}")
    if(NOT lane_count EQUAL 4)
        message(FATAL_ERROR "${digest_key} must have four lanes: ${output}")
    endif()
    foreach(lane RANGE 0 3)
        string(JSON value GET "${output}" "${digest_key}" ${lane})
        if(NOT value MATCHES "^[0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f]$")
            message(FATAL_ERROR "${digest_key} lane is not fixed lowercase hex")
        endif()
    endforeach()
endforeach()

if(DEFINED expected_profile_digest)
    foreach(digest_case IN ITEMS profile partition)
        if(digest_case STREQUAL "profile")
            set(actual_digest_key profile_sequence_digest)
            set(expected_digest "${expected_profile_digest}")
        else()
            set(actual_digest_key partition_digest)
            set(expected_digest "${expected_partition_digest}")
        endif()
        string(REPLACE "," ";" expected_lanes "${expected_digest}")
        foreach(lane RANGE 0 3)
            list(GET expected_lanes ${lane} expected_lane)
            string(JSON actual_lane GET "${output}" "${actual_digest_key}" ${lane})
            if(NOT actual_lane STREQUAL expected_lane)
                message(
                    FATAL_ERROR
                    "${actual_digest_key} lane ${lane}: expected ${expected_lane}, got ${actual_lane}"
                )
            endif()
        endforeach()
    endforeach()
endif()

if(DEFINED EXPECTED_GRAPH_DIGEST AND NOT EXPECTED_GRAPH_DIGEST STREQUAL "")
    string(REPLACE "," ";" expected_graph_digest "${EXPECTED_GRAPH_DIGEST}")
    foreach(lane RANGE 0 3)
        list(GET expected_graph_digest ${lane} expected)
        string(JSON actual GET "${output}" graph_digest ${lane})
        if(NOT actual STREQUAL expected)
            message(FATAL_ERROR "graph digest literal mismatch: ${output}")
        endif()
    endforeach()
endif()

foreach(timing IN ITEMS generation_seconds partition_seconds validation_seconds elapsed_seconds)
    string(JSON value GET "${output}" "${timing}")
    if(value LESS 0)
        message(FATAL_ERROR "negative ${timing}: ${output}")
    endif()
endforeach()
string(JSON rss GET "${output}" max_rank_rss_bytes)
if(rss LESS_EQUAL 0)
    message(FATAL_ERROR "maximum RSS must be positive: ${output}")
endif()

file(GLOB unexpected_outputs LIST_DIRECTORIES TRUE "${WORK_DIRECTORY}/*")
if(unexpected_outputs)
    message(FATAL_ERROR "probe created output files: ${unexpected_outputs}")
endif()
