cmake_minimum_required(VERSION 4.0...4.3)

foreach(
    required_variable
    IN ITEMS
        TEST_CASE
        ORACLE_SCRIPT
        MANIFEST
        GENERATOR
        PARHIP
        VERIFIER
        MPIEXEC_EXECUTABLE
        MPIEXEC_NUMPROC_FLAG
        WORK_DIRECTORY
)
    if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR "${required_variable} is required")
    endif()
endforeach()

foreach(required_file IN ITEMS ORACLE_SCRIPT MANIFEST GENERATOR PARHIP VERIFIER)
    if(NOT EXISTS "${${required_file}}")
        message(FATAL_ERROR
            "${required_file} does not exist: ${${required_file}}"
        )
    endif()
endforeach()

file(READ "${MANIFEST}" valid_manifest)
set(mutated_manifest "${valid_manifest}")

if(TEST_CASE STREQUAL "malformed-provenance")
    set(search "upstream_compiler=GNU-16.2.1")
    set(replacement "upstream_compiler")
    set(expected_diagnostic
        "malformed cube oracle manifest line 'upstream_compiler'"
    )
elseif(TEST_CASE STREQUAL "duplicate-provenance")
    string(APPEND mutated_manifest
        "\nupstream_mpi=Open-MPI-5.0.10-MPI-3.1\n"
    )
    set(expected_diagnostic "duplicate cube oracle key 'upstream_mpi'")
elseif(TEST_CASE STREQUAL "missing-provenance")
    set(search "cell_id=x+nx*(y+ny*z)\n")
    set(replacement "")
    set(expected_diagnostic "cube oracle manifest is missing 'cell_id'")
elseif(TEST_CASE STREQUAL "unknown-provenance")
    string(APPEND mutated_manifest "\nupstream_build_host=unrecorded\n")
    set(expected_diagnostic
        "unknown cube oracle manifest key 'upstream_build_host'"
    )
elseif(TEST_CASE STREQUAL "invalid-compiler")
    set(search "upstream_compiler=GNU-16.2.1")
    set(replacement "upstream_compiler=GNU")
    set(expected_diagnostic "invalid upstream compiler provenance 'GNU'")
elseif(TEST_CASE STREQUAL "invalid-mpi")
    set(search "upstream_mpi=Open-MPI-5.0.10-MPI-3.1")
    set(replacement "upstream_mpi=Open-MPI")
    set(expected_diagnostic "invalid upstream MPI provenance 'Open-MPI'")
elseif(TEST_CASE STREQUAL "invalid-cell-id")
    set(search "cell_id=x+nx*(y+ny*z)")
    set(replacement "cell_id=z+nz*(y+ny*x)")
    set(expected_diagnostic "unsupported cube cell-id recipe")
elseif(TEST_CASE STREQUAL "invalid-adjacency")
    set(search "adjacency=sorted-six-face-neighborhood")
    set(replacement "adjacency=unsorted-six-face-neighborhood")
    set(expected_diagnostic "unsupported cube adjacency recipe")
elseif(TEST_CASE STREQUAL "invalid-preconfiguration")
    set(search "cube4.preconfiguration=fastmesh")
    set(replacement "cube4.preconfiguration=eco")
    set(expected_diagnostic
        "cube4.preconfiguration is 'eco', expected 'fastmesh'"
    )
elseif(TEST_CASE STREQUAL "invalid-seed")
    set(search "cube4.seed=1")
    set(replacement "cube4.seed=2")
    set(expected_diagnostic "cube4.seed is 2, expected 1")
elseif(TEST_CASE STREQUAL "invalid-imbalance")
    set(search "cube4.imbalance_percent=3")
    set(replacement "cube4.imbalance_percent=4")
    set(expected_diagnostic "cube4.imbalance_percent is 4, expected 3")
elseif(TEST_CASE STREQUAL "exact-cut")
    set(search "cube4.rank1.weighted_cut=28")
    set(replacement "cube4.rank1.weighted_cut=2")
    set(expected_diagnostic "weighted cut is 28, expected 2")
elseif(TEST_CASE STREQUAL "orphaned-repair-provenance")
    string(REGEX REPLACE
        "[^\n]*\\.repaired_[^\n]*\n?" ""
        mutated_manifest "${valid_manifest}"
    )
    set(expected_diagnostic
        "cube oracle repair provenance has no repair overlay"
    )
elseif(TEST_CASE STREQUAL "missing-repair-provenance")
    set(search "repair_mpi=MPICH-5.0.1-MPI-5.0\n")
    set(replacement "")
    set(expected_diagnostic
        "cube oracle repair provenance is missing 'repair_mpi'"
    )
    set(test_ranks 5)
elseif(TEST_CASE STREQUAL "invalid-repair-provenance")
    set(search "repair_compiler=GNU-16.2.1")
    set(replacement "repair_compiler=GNU")
    set(expected_diagnostic "invalid repair compiler provenance 'GNU'")
    set(test_ranks 5)
elseif(TEST_CASE STREQUAL "mismatched-repair-upstream-anchor")
    set(search
        "cube4.rank5.repaired_upstream_partition_sha256=05d39781dac8fc7e376085023430956bd3b63e14b97ce44b59334aee059cc85a"
    )
    set(replacement
        "cube4.rank5.repaired_upstream_partition_sha256=0000000000000000000000000000000000000000000000000000000000000000"
    )
    set(expected_diagnostic
        "cube4.rank5 repaired upstream partition SHA-256 does not match"
    )
    set(test_ranks 5)
else()
    message(FATAL_ERROR "unknown cube oracle validation case '${TEST_CASE}'")
endif()

if(NOT DEFINED test_ranks)
    set(test_ranks 1)
endif()

if(DEFINED search)
    string(FIND "${valid_manifest}" "${search}" search_index)
    if(search_index EQUAL -1)
        message(FATAL_ERROR
            "validation fixture no longer contains '${search}'"
        )
    endif()
    string(REPLACE "${search}" "${replacement}"
        mutated_manifest "${valid_manifest}"
    )
endif()

file(MAKE_DIRECTORY "${WORK_DIRECTORY}")
set(mutated_manifest_path "${WORK_DIRECTORY}/${TEST_CASE}.txt")
file(WRITE "${mutated_manifest_path}" "${mutated_manifest}")

execute_process(
    COMMAND
        "${CMAKE_COMMAND}"
        "-DGENERATOR=${GENERATOR}"
        "-DPARHIP=${PARHIP}"
        "-DVERIFIER=${VERIFIER}"
        "-DMPIEXEC_EXECUTABLE=${MPIEXEC_EXECUTABLE}"
        "-DMPIEXEC_NUMPROC_FLAG=${MPIEXEC_NUMPROC_FLAG}"
        "-DMPIEXEC_PREFLAGS=${MPIEXEC_PREFLAGS}"
        "-DMPIEXEC_POSTFLAGS=${MPIEXEC_POSTFLAGS}"
        "-DMANIFEST=${mutated_manifest_path}"
        "-DWORK_DIRECTORY=${WORK_DIRECTORY}/${TEST_CASE}-run"
        -DFIXTURE=cube4
        -DNX=4
        -DNY=4
        -DNZ=4
        -DBLOCKS=2
        "-DRANKS=${test_ranks}"
        -P "${ORACLE_SCRIPT}"
    RESULT_VARIABLE oracle_result
    OUTPUT_VARIABLE oracle_output
    ERROR_VARIABLE oracle_error
)
set(oracle_log "${oracle_output}${oracle_error}")
if(oracle_result EQUAL 0)
    message(FATAL_ERROR
        "cube oracle accepted invalid '${TEST_CASE}' manifest\n${oracle_log}"
    )
endif()
string(FIND "${oracle_log}" "${expected_diagnostic}" diagnostic_index)
if(diagnostic_index EQUAL -1)
    message(FATAL_ERROR
        "cube oracle rejected '${TEST_CASE}' for the wrong reason; expected '${expected_diagnostic}'\n${oracle_log}"
    )
endif()
