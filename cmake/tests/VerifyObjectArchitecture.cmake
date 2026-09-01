cmake_minimum_required(VERSION 4.0)

if(NOT DEFINED KAHIP_SOURCE_DIR)
    message(FATAL_ERROR "KAHIP_SOURCE_DIR is required")
endif()

function(require_source_pattern file pattern diagnostic)
    file(READ "${file}" source)
    if(NOT source MATCHES "${pattern}")
        message(FATAL_ERROR "${diagnostic}")
    endif()
endfunction()

function(reject_source_pattern file pattern diagnostic)
    file(READ "${file}" source)
    if(source MATCHES "${pattern}")
        message(FATAL_ERROR "${diagnostic}")
    endif()
endfunction()

set(root_cmake "${KAHIP_SOURCE_DIR}/CMakeLists.txt")
set(modified_cmake "${KAHIP_SOURCE_DIR}/parallel/modified_kahip/CMakeLists.txt")
set(parhip_cmake "${KAHIP_SOURCE_DIR}/parallel/parallel_src/CMakeLists.txt")

require_source_pattern(
    "${root_cmake}"
    "cmake_minimum_required\\(VERSION 4\\.0\\.\\.\\.4\\.3\\)"
    "KaHIP must require the tested CMake 4 policy range"
)
foreach(
    object_target
    IN ITEMS
        kahip_core_obj
        kahip_collective_obj
        kahip_mapping_obj
        kahip_spac_obj
        kahip_ordering_obj
)
    require_source_pattern(
        "${root_cmake}"
        "add_library\\([^\\)]*${object_target}[^\\)]*OBJECT"
        "missing root object target ${object_target}"
    )
endforeach()

foreach(
    object_target
    IN ITEMS
        modified_kahip_core_obj
        modified_kahip_collective_obj
        modified_kahip_evolutionary_interface_obj
)
    require_source_pattern(
        "${modified_cmake}"
        "add_library\\([^\\)]*${object_target}[^\\)]*OBJECT"
        "missing modified-KaHIP object target ${object_target}"
    )
endforeach()

foreach(
    object_target
    IN ITEMS
        parhip_graph_obj
        parhip_mpi_obj
        parhip_mpi_application_obj
        parhip_core_obj
        parhip_dspac_obj
)
    require_source_pattern(
        "${parhip_cmake}"
        "add_library\\([^\\)]*${object_target}[^\\)]*OBJECT"
        "missing ParHIP object target ${object_target}"
    )
endforeach()

foreach(cmake_file IN ITEMS "${root_cmake}" "${modified_cmake}" "${parhip_cmake}")
    require_source_pattern(
        "${cmake_file}"
        "FILE_SET"
        "${cmake_file} must declare target-local header file sets"
    )
endforeach()

foreach(
    legacy_include_variable
    IN ITEMS
        KAHIP_PRIVATE_INCLUDE_DIRS
        MODIFIED_KAHIP_PRIVATE_INCLUDE_DIRS
        PARHIP_PRIVATE_INCLUDE_DIRS
)
    foreach(
        cmake_file
        IN ITEMS "${root_cmake}" "${modified_cmake}" "${parhip_cmake}"
    )
        reject_source_pattern(
            "${cmake_file}"
            "${legacy_include_variable}"
            "legacy include-directory aggregate ${legacy_include_variable} must be represented by target-local header file sets"
        )
    endforeach()
endforeach()

require_source_pattern(
    "${root_cmake}"
    "function\\(kahip_add_header_root_file_sets"
    "KaHIP must model legacy header roots with target-local CMake file sets"
)

foreach(
    configured_target
    IN ITEMS
        kahip_core_obj
        kahip_collective_obj
        kahip_mapping_obj
        kahip_spac_obj
        kahip_ordering_obj
)
    require_source_pattern(
        "${root_cmake}"
        "kahip_configure_root_object\\(${configured_target}\\)[ \t\r\n]+kahip_add_private_header_set\\([ \t\r\n]+${configured_target}"
        "${configured_target} header roots must precede its catch-all private header file set"
    )
endforeach()

require_source_pattern(
    "${parhip_cmake}"
    "FILE_SET[ \t\r\n]+parhip_generated_headers[ \t\r\n]+TYPE[ \t\r\n]+HEADERS[ \t\r\n]+BASE_DIRS[ \t\r\n]+\"\\\$\\{PARHIP_GENERATED_INCLUDE_DIR\\}\"[ \t\r\n]+FILES[ \t\r\n]+\"\\\$\\{PARHIP_GENERATED_INCLUDE_DIR\\}/kahip_mpi_capabilities\\.h\""
    "the generated MPI capability header must be an actual FILE_SET member"
)

foreach(
    configured_target
    IN ITEMS
        modified_kahip_core_obj
        modified_kahip_collective_obj
        modified_kahip_evolutionary_interface_obj
)
    require_source_pattern(
        "${modified_cmake}"
        "kahip_configure_modified_object\\(${configured_target}\\)[ \t\r\n]+kahip_add_private_header_set\\([ \t\r\n]+${configured_target}"
        "${configured_target} header roots must precede shared or catch-all header file sets"
    )
endforeach()

foreach(
    configured_target
    IN ITEMS
        parhip_graph_obj
        parhip_mpi_obj
        parhip_mpi_application_obj
        parhip_core_obj
        parhip_dspac_obj
)
    require_source_pattern(
        "${parhip_cmake}"
        "kahip_configure_parhip_object\\(${configured_target}\\)[ \t\r\n]+kahip_add_private_header_set\\([ \t\r\n]+${configured_target}"
        "${configured_target} header roots must precede its catch-all private header file set"
    )
endforeach()

foreach(
    internal_target
    IN ITEMS
        interface_test
        kahip
        libmodified_kahip_interface
        parallel
        libedgelist
        libdspac
)
    foreach(
        cmake_file
        IN ITEMS "${root_cmake}" "${modified_cmake}" "${parhip_cmake}"
    )
        reject_source_pattern(
            "${cmake_file}"
            "target_include_directories\\([ \t\r\n]*${internal_target}([ \t\r\n]|\\))"
            "internal target ${internal_target} must express project headers through FILE_SET HEADERS"
        )
    endforeach()
endforeach()

reject_source_pattern(
    "${root_cmake}"
    "lib/tools/(graph_communication|mpi_tools)\\.cpp"
    "dead root point-to-point MPI helpers remain in a target"
)
reject_source_pattern(
    "${modified_cmake}"
    "lib/tools/graph_communication\\.cpp"
    "dead modified-KaHIP graph broadcast remains in a target"
)
reject_source_pattern(
    "${parhip_cmake}"
    "parhip_(graph_io|edge_list)_obj"
    "duplicate graph-I/O object targets must be replaced by parhip_graph_obj"
)
