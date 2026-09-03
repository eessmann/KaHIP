include_guard(GLOBAL)

function(_kahip_pkg_config_escape output_variable value)
    if(value MATCHES "[\r\n]")
        message(FATAL_ERROR "pkg-config flags cannot contain newlines")
    endif()

    string(REPLACE "\\" "\\\\" escaped_value "${value}")
    string(REPLACE " " "\\ " escaped_value "${escaped_value}")
    set(${output_variable} "${escaped_value}" PARENT_SCOPE)
endfunction()

function(_kahip_join_pkg_config_flags output_variable prefix)
    set(rendered_flags "")
    foreach(flag IN LISTS ARGN)
        _kahip_pkg_config_escape(escaped_flag "${flag}")
        list(APPEND rendered_flags "${prefix}${escaped_flag}")
    endforeach()
    string(JOIN " " joined_flags ${rendered_flags})
    set(${output_variable} "${joined_flags}" PARENT_SCOPE)
endfunction()

function(kahip_format_serial_api_pkg_config_cflags output_variable)
    cmake_parse_arguments(PARSE_ARGV 1 api "64BIT;METIS" "" "")
    if(api_UNPARSED_ARGUMENTS)
        message(
            FATAL_ERROR
            "unrecognized serial API pkg-config arguments: ${api_UNPARSED_ARGUMENTS}"
        )
    endif()

    set(public_definitions "")
    if(api_64BIT)
        list(APPEND public_definitions KAHIP_64BIT)
    endif()
    if(api_METIS)
        list(APPEND public_definitions USEMETIS)
    endif()
    _kahip_join_pkg_config_flags(
        rendered_definitions
        -D
        ${public_definitions}
    )
    set(${output_variable} "${rendered_definitions}" PARENT_SCOPE)
endfunction()

function(kahip_format_mpi_pkg_config_flags cflags_output libs_output)
    cmake_parse_arguments(
        PARSE_ARGV
        2
        mpi
        ""
        ""
        "INCLUDE_DIRECTORIES;COMPILE_DEFINITIONS;COMPILE_OPTIONS;LINK_OPTIONS;LIBRARIES"
    )
    if(mpi_UNPARSED_ARGUMENTS)
        message(
            FATAL_ERROR
            "unrecognized MPI pkg-config arguments: ${mpi_UNPARSED_ARGUMENTS}"
        )
    endif()

    _kahip_join_pkg_config_flags(
        include_flags
        -I
        ${mpi_INCLUDE_DIRECTORIES}
    )
    _kahip_join_pkg_config_flags(
        definition_flags
        -D
        ${mpi_COMPILE_DEFINITIONS}
    )
    _kahip_join_pkg_config_flags(
        compile_option_flags
        ""
        ${mpi_COMPILE_OPTIONS}
    )

    set(cflags ${include_flags} ${definition_flags} ${compile_option_flags})
    list(FILTER cflags EXCLUDE REGEX "^$")
    string(JOIN " " cflags ${cflags})

    _kahip_join_pkg_config_flags(
        link_option_flags
        ""
        ${mpi_LINK_OPTIONS}
    )
    set(library_flags "")
    foreach(library IN LISTS mpi_LIBRARIES)
        if(IS_ABSOLUTE "${library}" OR library MATCHES "^-")
            set(library_flag "${library}")
        else()
            set(library_flag "-l${library}")
        endif()
        _kahip_pkg_config_escape(escaped_library "${library_flag}")
        list(APPEND library_flags "${escaped_library}")
    endforeach()

    set(libs ${link_option_flags} ${library_flags})
    list(FILTER libs EXCLUDE REGEX "^$")
    string(JOIN " " libs ${libs})

    set(${cflags_output} "${cflags}" PARENT_SCOPE)
    set(${libs_output} "${libs}" PARENT_SCOPE)
endfunction()
