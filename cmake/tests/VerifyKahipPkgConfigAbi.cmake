cmake_minimum_required(VERSION 4.0...4.3)

foreach(
    required
    IN ITEMS KAHIP_SOURCE_DIR PKG_CONFIG_EXECUTABLE WORK_DIRECTORY
)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK_DIRECTORY}")
file(MAKE_DIRECTORY "${WORK_DIRECTORY}")
list(APPEND CMAKE_MODULE_PATH "${KAHIP_SOURCE_DIR}/cmake")
include(KahipPkgConfig)

function(verify_kahip_abi_case case_name expect_64_bit expect_metis)
    set(case_directory "${WORK_DIRECTORY}/${case_name}")
    file(MAKE_DIRECTORY "${case_directory}")
    set(KAHIP_PKGCONFIG_PREFIX_FROM_PCFILEDIR "../..")
    set(CMAKE_INSTALL_INCLUDEDIR include)
    set(CMAKE_INSTALL_LIBDIR lib)
    set(PROJECT_VERSION 3.24)
    set(api_options "")
    if(expect_64_bit)
        list(APPEND api_options 64BIT)
    endif()
    if(expect_metis)
        list(APPEND api_options METIS)
    endif()
    kahip_format_serial_api_pkg_config_cflags(
        KAHIP_PKGCONFIG_CFLAGS
        ${api_options}
    )
    configure_file(
        "${KAHIP_SOURCE_DIR}/lib/kahip.pc.in"
        "${case_directory}/kahip.pc"
        @ONLY
    )

    execute_process(
        COMMAND
            "${CMAKE_COMMAND}" -E env
            "PKG_CONFIG_PATH=${case_directory}"
            "PKG_CONFIG_LIBDIR=${case_directory}"
            "${PKG_CONFIG_EXECUTABLE}" --cflags kahip
        RESULT_VARIABLE pkg_config_result
        OUTPUT_VARIABLE pkg_config_output
        ERROR_VARIABLE pkg_config_stderr
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(NOT pkg_config_result EQUAL 0)
        message(
            FATAL_ERROR
            "pkg-config failed for ${case_name}\n${pkg_config_stderr}"
        )
    endif()

    separate_arguments(parsed_cflags UNIX_COMMAND "${pkg_config_output}")
    list(FIND parsed_cflags -DKAHIP_64BIT definition_index)
    if(expect_64_bit AND definition_index EQUAL -1)
        message(
            FATAL_ERROR
            "64-bit kahip.pc omitted -DKAHIP_64BIT: ${pkg_config_output}"
        )
    elseif(NOT expect_64_bit AND NOT definition_index EQUAL -1)
        message(
            FATAL_ERROR
            "32-bit kahip.pc unexpectedly advertised -DKAHIP_64BIT: ${pkg_config_output}"
        )
    endif()

    list(FIND parsed_cflags -DUSEMETIS metis_definition_index)
    if(expect_metis AND metis_definition_index EQUAL -1)
        message(
            FATAL_ERROR
            "Metis-enabled kahip.pc omitted -DUSEMETIS: ${pkg_config_output}"
        )
    elseif(NOT expect_metis AND NOT metis_definition_index EQUAL -1)
        message(
            FATAL_ERROR
            "non-Metis kahip.pc unexpectedly advertised -DUSEMETIS: ${pkg_config_output}"
        )
    endif()
endfunction()

verify_kahip_abi_case(32-bit FALSE FALSE)
verify_kahip_abi_case(64-bit TRUE FALSE)
verify_kahip_abi_case(metis-32-bit FALSE TRUE)
verify_kahip_abi_case(metis-64-bit TRUE TRUE)
