cmake_minimum_required(VERSION 4.0)

foreach(required IN ITEMS
        GENERATOR
        PARHIP
        VERIFIER
        MPIEXEC_EXECUTABLE
        MPIEXEC_NUMPROC_FLAG
        MANIFEST
        WORK_DIRECTORY
        FIXTURE
        NX
        NY
        NZ
        BLOCKS
        RANKS)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

foreach(required_file IN ITEMS GENERATOR PARHIP VERIFIER MANIFEST)
    if(NOT EXISTS "${${required_file}}")
        message(FATAL_ERROR
            "${required_file} does not exist: ${${required_file}}"
        )
    endif()
endforeach()

file(STRINGS "${MANIFEST}" manifest_lines ENCODING UTF-8)
list(POP_FRONT manifest_lines manifest_title)
if(NOT manifest_title STREQUAL "KaHIP cube partition oracle provenance")
    message(FATAL_ERROR "invalid cube oracle provenance title")
endif()

set(provenance_keys
    upstream_revision
    upstream_compiler
    upstream_mpi
    cell_id
    adjacency
)
set(repair_provenance_keys
    repair_semantics
    repair_revision
    repair_compiler
    repair_mpi
)
set(manifest_keys "")
foreach(line IN LISTS manifest_lines)
    if(line STREQUAL "")
        continue()
    endif()

    string(FIND "${line}" "=" separator)
    if(separator LESS 1)
        message(FATAL_ERROR
            "malformed cube oracle manifest line '${line}'"
        )
    endif()
    string(SUBSTRING "${line}" 0 ${separator} key)
    math(EXPR value_begin "${separator} + 1")
    string(SUBSTRING "${line}" ${value_begin} -1 value)
    if(value STREQUAL "")
        message(FATAL_ERROR
            "malformed cube oracle manifest line '${line}'"
        )
    endif()

    list(FIND manifest_keys "${key}" duplicate_index)
    if(NOT duplicate_index EQUAL -1)
        message(FATAL_ERROR "duplicate cube oracle key '${key}'")
    endif()
    list(APPEND manifest_keys "${key}")

    if(key IN_LIST provenance_keys)
        continue()
    endif()
    if(key IN_LIST repair_provenance_keys)
        continue()
    endif()
    if(key MATCHES
       "^[A-Za-z][A-Za-z0-9_-]*\\.(nx|ny|nz|blocks|preconfiguration|seed|imbalance_percent|graph_file_sha256)$")
        continue()
    endif()
    if(key MATCHES
       "^[A-Za-z][A-Za-z0-9_-]*\\.rank[1-9][0-9]*\\.(partition_file_sha256|partition_sha256|partition|block_weights|weighted_cut)$")
        continue()
    endif()
    if(key MATCHES
       "^[A-Za-z][A-Za-z0-9_-]*\\.rank[1-9][0-9]*\\.repaired_(upstream_partition_sha256|partition_file_sha256|partition_sha256|partition|block_weights|weighted_cut)$")
        continue()
    endif()
    message(FATAL_ERROR "unknown cube oracle manifest key '${key}'")
endforeach()

function(manifest_find key output found_output)
    set(found FALSE)
    set(value "")
    foreach(line IN LISTS manifest_lines)
        string(FIND "${line}" "=" separator)
        if(separator LESS 1)
            continue()
        endif()
        string(SUBSTRING "${line}" 0 ${separator} candidate_key)
        if(NOT candidate_key STREQUAL key)
            continue()
        endif()
        if(found)
            message(FATAL_ERROR "duplicate cube oracle key '${key}'")
        endif()
        math(EXPR value_begin "${separator} + 1")
        string(SUBSTRING "${line}" ${value_begin} -1 value)
        set(found TRUE)
    endforeach()
    set(${output} "${value}" PARENT_SCOPE)
    set(${found_output} ${found} PARENT_SCOPE)
endfunction()

function(manifest_require key output)
    manifest_find("${key}" value found)
    if(NOT found)
        message(FATAL_ERROR "cube oracle manifest is missing '${key}'")
    endif()
    set(${output} "${value}" PARENT_SCOPE)
endfunction()

function(require_lower_hex name value length)
    string(LENGTH "${value}" actual_length)
    if(NOT actual_length EQUAL length OR NOT value MATCHES "^[0-9a-f]+$")
        message(FATAL_ERROR
            "${name} must be ${length} lowercase hexadecimal characters"
        )
    endif()
endfunction()

manifest_require(upstream_revision upstream_revision)
require_lower_hex("upstream revision" "${upstream_revision}" 40)
manifest_require(upstream_compiler upstream_compiler)
if(NOT upstream_compiler MATCHES
   "^[A-Za-z][A-Za-z0-9+._-]*-[0-9]+(\\.[0-9]+)+$")
    message(FATAL_ERROR
        "invalid upstream compiler provenance '${upstream_compiler}'"
    )
endif()
manifest_require(upstream_mpi upstream_mpi)
if(NOT upstream_mpi MATCHES
   "^[A-Za-z][A-Za-z0-9+._-]*-[0-9]+(\\.[0-9]+)+-MPI-[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "invalid upstream MPI provenance '${upstream_mpi}'")
endif()
manifest_require(cell_id cell_id_recipe)
if(NOT cell_id_recipe STREQUAL "x+nx*(y+ny*z)")
    message(FATAL_ERROR "unsupported cube cell-id recipe '${cell_id_recipe}'")
endif()
manifest_require(adjacency adjacency_recipe)
if(NOT adjacency_recipe STREQUAL "sorted-six-face-neighborhood")
    message(FATAL_ERROR
        "unsupported cube adjacency recipe '${adjacency_recipe}'"
    )
endif()

set(repair_tuples "")
foreach(key IN LISTS manifest_keys)
    if(key MATCHES
       "^([A-Za-z][A-Za-z0-9_-]*\\.rank[1-9][0-9]*)\\.repaired_")
        list(APPEND repair_tuples "${CMAKE_MATCH_1}")
    endif()
endforeach()
list(REMOVE_DUPLICATES repair_tuples)
set(repair_provenance_present FALSE)
foreach(key IN LISTS repair_provenance_keys)
    list(FIND manifest_keys "${key}" repair_provenance_index)
    if(NOT repair_provenance_index EQUAL -1)
        set(repair_provenance_present TRUE)
    endif()
endforeach()
if(repair_provenance_present AND NOT repair_tuples)
    message(FATAL_ERROR
        "cube oracle repair provenance has no repair overlay"
    )
endif()
if(repair_tuples)
    foreach(key IN LISTS repair_provenance_keys)
        manifest_find("${key}" repair_${key} has_repair_${key})
        if(NOT has_repair_${key})
            message(FATAL_ERROR
                "cube oracle repair provenance is missing '${key}'"
            )
        endif()
    endforeach()
    if(NOT repair_repair_semantics STREQUAL "weighted-feasibility")
        message(FATAL_ERROR
            "unsupported repair semantics '${repair_repair_semantics}'"
        )
    endif()
    require_lower_hex("repair revision" "${repair_repair_revision}" 40)
    if(NOT repair_repair_revision STREQUAL
       "8b26fa29dece9e268c98106c315e47fdbeaf1c1b")
        message(FATAL_ERROR
            "unsupported repair revision '${repair_repair_revision}'"
        )
    endif()
    if(NOT repair_repair_compiler MATCHES
       "^[A-Za-z][A-Za-z0-9+._-]*-[0-9]+(\\.[0-9]+)+$")
        message(FATAL_ERROR
            "invalid repair compiler provenance '${repair_repair_compiler}'"
        )
    endif()
    if(NOT repair_repair_compiler STREQUAL "GNU-16.2.1")
        message(FATAL_ERROR
            "unsupported repair compiler '${repair_repair_compiler}'"
        )
    endif()
    if(NOT repair_repair_mpi MATCHES
       "^[A-Za-z][A-Za-z0-9+._-]*-[0-9]+(\\.[0-9]+)+-MPI-[0-9]+\\.[0-9]+$")
        message(FATAL_ERROR
            "invalid repair MPI provenance '${repair_repair_mpi}'"
        )
    endif()
    if(NOT repair_repair_mpi STREQUAL "MPICH-5.0.1-MPI-5.0")
        message(FATAL_ERROR
            "unsupported repair MPI '${repair_repair_mpi}'"
        )
    endif()

    set(allowed_repair_tuples cube4.rank5 cube10.rank2 cube10.rank4)
    foreach(repair_tuple IN LISTS repair_tuples)
        if(NOT repair_tuple IN_LIST allowed_repair_tuples)
            message(FATAL_ERROR
                "repair overlay is not allowed for '${repair_tuple}'"
            )
        endif()
        foreach(field IN ITEMS
                upstream_partition_sha256
                partition_file_sha256
                partition_sha256
                block_weights
                weighted_cut)
            manifest_require(
                "${repair_tuple}.repaired_${field}"
                "${repair_tuple}_repaired_${field}"
            )
        endforeach()
        require_lower_hex(
            "${repair_tuple} repaired upstream partition SHA-256"
            "${${repair_tuple}_repaired_upstream_partition_sha256}" 64
        )
        require_lower_hex(
            "${repair_tuple} repaired partition file SHA-256"
            "${${repair_tuple}_repaired_partition_file_sha256}" 64
        )
        require_lower_hex(
            "${repair_tuple} repaired partition SHA-256"
            "${${repair_tuple}_repaired_partition_sha256}" 64
        )
        if(NOT "${${repair_tuple}_repaired_block_weights}" MATCHES
           "^[0-9]+(,[0-9]+)*$")
            message(FATAL_ERROR
                "${repair_tuple}.repaired_block_weights is not a canonical integer list"
            )
        endif()
        if(NOT "${${repair_tuple}_repaired_weighted_cut}" MATCHES "^[0-9]+$")
            message(FATAL_ERROR
                "${repair_tuple}.repaired_weighted_cut is not an unsigned integer"
            )
        endif()
        manifest_require(
            "${repair_tuple}.partition_sha256"
            "${repair_tuple}_upstream_partition_sha256"
        )
        if(NOT "${${repair_tuple}_repaired_upstream_partition_sha256}" STREQUAL
           "${${repair_tuple}_upstream_partition_sha256}")
            message(FATAL_ERROR
                "${repair_tuple} repaired upstream partition SHA-256 does not match its pristine upstream record"
            )
        endif()
    endforeach()
    foreach(allowed_repair_tuple IN LISTS allowed_repair_tuples)
        if(NOT allowed_repair_tuple IN_LIST repair_tuples)
            message(FATAL_ERROR
                "cube oracle manifest is missing repair overlay '${allowed_repair_tuple}'"
            )
        endif()
    endforeach()
endif()

foreach(field IN ITEMS nx ny nz blocks)
    manifest_require("${FIXTURE}.${field}" manifest_${field})
endforeach()
set(manifest_dimension_fields nx ny nz blocks)
set(argument_dimension_fields NX NY NZ BLOCKS)
foreach(field expected IN ZIP_LISTS
        manifest_dimension_fields
        argument_dimension_fields)
    if(NOT manifest_${field} STREQUAL "${${expected}}")
        message(FATAL_ERROR
            "${FIXTURE}.${field} is ${manifest_${field}}, expected ${${expected}}"
        )
    endif()
endforeach()

manifest_require("${FIXTURE}.preconfiguration" preconfiguration)
manifest_require("${FIXTURE}.seed" seed)
manifest_require("${FIXTURE}.imbalance_percent" imbalance_percent)
if(NOT preconfiguration STREQUAL "fastmesh")
    message(FATAL_ERROR
        "${FIXTURE}.preconfiguration is '${preconfiguration}', expected 'fastmesh'"
    )
endif()
if(NOT seed STREQUAL "1")
    message(FATAL_ERROR "${FIXTURE}.seed is ${seed}, expected 1")
endif()
if(NOT imbalance_percent STREQUAL "3")
    message(FATAL_ERROR
        "${FIXTURE}.imbalance_percent is ${imbalance_percent}, expected 3"
    )
endif()
manifest_require("${FIXTURE}.graph_file_sha256" expected_graph_file_sha256)
require_lower_hex(
    "graph file SHA-256" "${expected_graph_file_sha256}" 64
)

set(tuple "${FIXTURE}.rank${RANKS}")
manifest_require("${tuple}.partition_file_sha256" expected_file_sha256)
manifest_require("${tuple}.partition_sha256" expected_partition_sha256)
manifest_require("${tuple}.block_weights" expected_block_weights)
manifest_require("${tuple}.weighted_cut" expected_weighted_cut)
require_lower_hex("partition file SHA-256" "${expected_file_sha256}" 64)
require_lower_hex("partition SHA-256" "${expected_partition_sha256}" 64)
if(NOT expected_block_weights MATCHES "^[0-9]+(,[0-9]+)*$")
    message(FATAL_ERROR
        "${tuple}.block_weights is not a canonical integer list"
    )
endif()
if(NOT expected_weighted_cut MATCHES "^[0-9]+$")
    message(FATAL_ERROR "${tuple}.weighted_cut is not an unsigned integer")
endif()

set(oracle_semantics "pristine upstream")
set(exact_partition_key "${tuple}.partition")
if(tuple IN_LIST repair_tuples)
    set(oracle_semantics "repaired weighted-feasibility")
    set(expected_file_sha256
        "${${tuple}_repaired_partition_file_sha256}"
    )
    set(expected_partition_sha256
        "${${tuple}_repaired_partition_sha256}"
    )
    set(expected_block_weights "${${tuple}_repaired_block_weights}")
    set(expected_weighted_cut "${${tuple}_repaired_weighted_cut}")
    set(exact_partition_key "${tuple}.repaired_partition")
endif()

file(MAKE_DIRECTORY "${WORK_DIRECTORY}")
set(graph_path "${WORK_DIRECTORY}/${FIXTURE}.graph")
set(partition_path "${WORK_DIRECTORY}/tmppartition.txtp")
file(REMOVE "${graph_path}" "${partition_path}")

execute_process(
    COMMAND "${GENERATOR}" ${NX} ${NY} ${NZ} "${graph_path}"
    RESULT_VARIABLE generator_result
    OUTPUT_VARIABLE generator_output
    ERROR_VARIABLE generator_error
)
if(NOT generator_result EQUAL 0)
    message(FATAL_ERROR
        "cube generator failed (${generator_result})\n"
        "${generator_output}${generator_error}"
    )
endif()
file(SHA256 "${graph_path}" actual_graph_file_sha256)
if(NOT WIN32 AND NOT actual_graph_file_sha256 STREQUAL
   expected_graph_file_sha256)
    message(FATAL_ERROR
        "generated graph SHA-256 is ${actual_graph_file_sha256}, expected ${expected_graph_file_sha256}"
    )
endif()

execute_process(
    COMMAND
        "${MPIEXEC_EXECUTABLE}" "${MPIEXEC_NUMPROC_FLAG}" ${RANKS}
        ${MPIEXEC_PREFLAGS}
        "${PARHIP}"
        ${MPIEXEC_POSTFLAGS}
        "${graph_path}"
        "--k=${BLOCKS}"
        "--preconfiguration=${preconfiguration}"
        "--seed=${seed}"
        "--imbalance=${imbalance_percent}"
        --save_partition
    WORKING_DIRECTORY "${WORK_DIRECTORY}"
    RESULT_VARIABLE partition_result
    OUTPUT_VARIABLE partition_output
    ERROR_VARIABLE partition_error
)
if(NOT partition_result EQUAL 0)
    message(FATAL_ERROR
        "ParHIP cube oracle run failed (${partition_result})\n"
        "${partition_output}${partition_error}"
    )
endif()
if(NOT EXISTS "${partition_path}")
    message(FATAL_ERROR "ParHIP did not produce ${partition_path}")
endif()

file(STRINGS "${partition_path}" partition ENCODING UTF-8)
math(EXPR expected_vertices "${NX} * ${NY} * ${NZ}")
list(LENGTH partition actual_vertices)
if(NOT actual_vertices EQUAL expected_vertices)
    message(FATAL_ERROR
        "partition contains ${actual_vertices} vertices, expected ${expected_vertices}"
    )
endif()
list(JOIN partition "," canonical_partition)
string(SHA256 actual_partition_sha256 "${canonical_partition}")
if(NOT actual_partition_sha256 STREQUAL expected_partition_sha256)
    message(FATAL_ERROR
        "canonical partition SHA-256 is ${actual_partition_sha256}, expected ${expected_partition_sha256}"
    )
endif()

file(SHA256 "${partition_path}" actual_file_sha256)
if(NOT WIN32 AND NOT actual_file_sha256 STREQUAL expected_file_sha256)
    message(FATAL_ERROR
        "partition file SHA-256 is ${actual_file_sha256}, expected ${expected_file_sha256}"
    )
endif()

manifest_find("${exact_partition_key}" exact_partition has_exact_partition)
if(has_exact_partition AND NOT canonical_partition STREQUAL exact_partition)
    message(FATAL_ERROR "${tuple} differs from its exact ${oracle_semantics} vector")
endif()

execute_process(
    COMMAND
        "${VERIFIER}" ${NX} ${NY} ${NZ} ${BLOCKS}
        ${imbalance_percent} "${partition_path}"
    RESULT_VARIABLE verifier_result
    OUTPUT_VARIABLE verifier_output
    ERROR_VARIABLE verifier_error
)
if(NOT verifier_result EQUAL 0)
    message(FATAL_ERROR
        "cube partition invariant verification failed (${verifier_result})\n"
        "${verifier_output}${verifier_error}"
    )
endif()
string(STRIP "${verifier_output}" verifier_record)
set(verifier_record_pattern
    "^verified vertices=([0-9]+) blocks=([0-9]+) maximum-block-weight=[0-9]+ block-weights=\\[([0-9,]+)\\] weighted-cut=([0-9]+)$"
)
if(NOT verifier_record MATCHES "${verifier_record_pattern}")
    message(FATAL_ERROR
        "cube verifier produced a malformed result record\n${verifier_output}"
    )
endif()
set(actual_verifier_vertices "${CMAKE_MATCH_1}")
set(actual_verifier_blocks "${CMAKE_MATCH_2}")
set(actual_block_weights "${CMAKE_MATCH_3}")
set(actual_weighted_cut "${CMAKE_MATCH_4}")
if(NOT actual_verifier_vertices STREQUAL "${expected_vertices}")
    message(FATAL_ERROR
        "verified vertex count is ${actual_verifier_vertices}, expected ${expected_vertices}"
    )
endif()
if(NOT actual_verifier_blocks STREQUAL "${BLOCKS}")
    message(FATAL_ERROR
        "verified block count is ${actual_verifier_blocks}, expected ${BLOCKS}"
    )
endif()
if(NOT actual_block_weights STREQUAL expected_block_weights)
    message(FATAL_ERROR
        "block weights are [${actual_block_weights}], expected [${expected_block_weights}]"
    )
endif()
if(NOT actual_weighted_cut STREQUAL expected_weighted_cut)
    message(FATAL_ERROR
        "weighted cut is ${actual_weighted_cut}, expected ${expected_weighted_cut}"
    )
endif()

if(oracle_semantics STREQUAL "repaired weighted-feasibility")
    message(STATUS
        "verified ${tuple} against repaired weighted-feasibility semantics: repair=${repair_repair_revision} compiler=${repair_repair_compiler} mpi=${repair_repair_mpi} upstream-anchor=${${tuple}_repaired_upstream_partition_sha256} partition=${actual_partition_sha256} cut=${expected_weighted_cut}"
    )
else()
    message(STATUS
        "verified ${tuple} against upstream ${upstream_revision}: partition=${actual_partition_sha256} cut=${expected_weighted_cut}"
    )
endif()
