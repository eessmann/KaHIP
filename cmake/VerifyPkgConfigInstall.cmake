cmake_minimum_required(VERSION 4.0...4.3)

foreach(
    required_variable
    IN ITEMS
        PROJECT_BINARY_DIR
        KAHIP_SOURCE_DIR
        STAGE_PREFIX
        CONSUMER_SOURCE_DIR
        CONSUMER_BINARY_DIR
        PKG_CONFIG_EXECUTABLE
        CTEST_COMMAND
        CONSUMER_GENERATOR
        C_COMPILER
        CXX_COMPILER
        INSTALL_BINDIR
        INSTALL_LIBDIR
        INSTALL_INCLUDEDIR
        KAHIP_SHARED_LIBRARY
        KAHIP_STATIC_LIBRARY
        KAHIP_64BIT
        TARGET_WINDOWS
        TARGET_UNIX
        WITH_PARHIP
)
    if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR "${required_variable} is required")
    endif()
endforeach()

include("${KAHIP_SOURCE_DIR}/cmake/KahipInstallConsumerArguments.cmake")

if(WITH_PARHIP)
    foreach(
        required_variable
        IN ITEMS
            PARHIP_EXECUTABLE
            PARHIP_SHARED_LIBRARY
            PARHIP_STATIC_LIBRARY
    )
        if(
            NOT DEFINED ${required_variable}
            OR "${${required_variable}}" STREQUAL ""
        )
            message(FATAL_ERROR "${required_variable} is required with ParHIP")
        endif()
    endforeach()
endif()
if(TARGET_WINDOWS)
    if(NOT DEFINED KAHIP_LINKER_LIBRARY OR KAHIP_LINKER_LIBRARY STREQUAL "")
        message(FATAL_ERROR "KAHIP_LINKER_LIBRARY is required on Windows")
    endif()
    if(
        WITH_PARHIP
        AND (
            NOT DEFINED PARHIP_LINKER_LIBRARY
            OR PARHIP_LINKER_LIBRARY STREQUAL ""
        )
    )
        message(
            FATAL_ERROR
            "PARHIP_LINKER_LIBRARY is required with ParHIP on Windows"
        )
    endif()
endif()

file(REMOVE_RECURSE "${STAGE_PREFIX}" "${CONSUMER_BINARY_DIR}")

set(install_command "${CMAKE_COMMAND}" --install "${PROJECT_BINARY_DIR}")
if(NOT "${BUILD_CONFIG}" STREQUAL "")
    list(APPEND install_command --config "${BUILD_CONFIG}")
endif()
list(APPEND install_command --prefix "${STAGE_PREFIX}")
execute_process(
    COMMAND ${install_command}
    RESULT_VARIABLE install_result
    OUTPUT_VARIABLE install_stdout
    ERROR_VARIABLE install_stderr
)
if(NOT install_result EQUAL 0)
    message(
        FATAL_ERROR
        "staged KaHIP install failed\n${install_stdout}\n${install_stderr}"
    )
endif()

set(pkg_config_dir "${STAGE_PREFIX}/${INSTALL_LIBDIR}/pkgconfig")
set(shared_library_directory "${INSTALL_LIBDIR}")
if(TARGET_WINDOWS)
    set(shared_library_directory "${INSTALL_BINDIR}")
endif()
set(required_installed_files
    "${STAGE_PREFIX}/${INSTALL_INCLUDEDIR}/kaHIP_interface.h"
    "${STAGE_PREFIX}/${shared_library_directory}/${KAHIP_SHARED_LIBRARY}"
    "${STAGE_PREFIX}/${INSTALL_LIBDIR}/${KAHIP_STATIC_LIBRARY}"
    "${pkg_config_dir}/kahip.pc"
)
if(TARGET_WINDOWS)
    list(
        APPEND
        required_installed_files
        "${STAGE_PREFIX}/${INSTALL_LIBDIR}/${KAHIP_LINKER_LIBRARY}"
    )
endif()
if(WITH_PARHIP)
    list(
        APPEND
        required_installed_files
        "${STAGE_PREFIX}/${INSTALL_INCLUDEDIR}/parhip_interface.h"
        "${STAGE_PREFIX}/${INSTALL_BINDIR}/${PARHIP_EXECUTABLE}"
        "${STAGE_PREFIX}/${shared_library_directory}/${PARHIP_SHARED_LIBRARY}"
        "${STAGE_PREFIX}/${INSTALL_LIBDIR}/${PARHIP_STATIC_LIBRARY}"
        "${pkg_config_dir}/parhip_interface.pc"
    )
    if(TARGET_WINDOWS)
        list(
            APPEND
            required_installed_files
            "${STAGE_PREFIX}/${INSTALL_LIBDIR}/${PARHIP_LINKER_LIBRARY}"
        )
    endif()
endif()
foreach(installed_file IN LISTS required_installed_files)
    if(NOT EXISTS "${installed_file}")
        message(FATAL_ERROR "staged install omitted ${installed_file}")
    endif()
endforeach()

if(TARGET_UNIX AND WITH_PARHIP)
    if(NOT DEFINED NM_EXECUTABLE OR NM_EXECUTABLE STREQUAL "")
        message(FATAL_ERROR "NM_EXECUTABLE is required for ParHIP artifact checks")
    endif()

    foreach(
        parhip_library
        IN ITEMS
            "${STAGE_PREFIX}/${shared_library_directory}/${PARHIP_SHARED_LIBRARY}"
            "${STAGE_PREFIX}/${INSTALL_LIBDIR}/${PARHIP_STATIC_LIBRARY}"
    )
        execute_process(
            COMMAND "${NM_EXECUTABLE}" -u "${parhip_library}"
            RESULT_VARIABLE nm_result
            OUTPUT_VARIABLE undefined_symbols
            ERROR_VARIABLE nm_stderr
        )
        if(NOT nm_result EQUAL 0)
            message(
                FATAL_ERROR
                "could not inspect installed ParHIP artifact ${parhip_library}\n${nm_stderr}"
            )
        endif()

        string(REPLACE "\n" ";" undefined_symbol_lines "${undefined_symbols}")
        set(forbidden_lifecycle_symbols "")
        foreach(undefined_symbol_line IN LISTS undefined_symbol_lines)
            if(
                undefined_symbol_line
                MATCHES
                "(^|[ \t])_?MPI_(Init|Finalize)(@[^ \t]+)?[ \t]*$"
            )
                list(APPEND forbidden_lifecycle_symbols "${undefined_symbol_line}")
            endif()
        endforeach()
        if(forbidden_lifecycle_symbols)
            list(JOIN forbidden_lifecycle_symbols "\n" symbol_diagnostics)
            message(
                FATAL_ERROR
                "installed ParHIP artifact owns the application MPI lifecycle: ${parhip_library}\n${symbol_diagnostics}"
            )
        endif()
    endforeach()
endif()

set(expected_installed_headers kaHIP_interface.h)
if(WITH_PARHIP)
    list(APPEND expected_installed_headers parhip_interface.h)
endif()
file(
    GLOB_RECURSE installed_headers
    LIST_DIRECTORIES FALSE
    RELATIVE "${STAGE_PREFIX}/${INSTALL_INCLUDEDIR}"
    "${STAGE_PREFIX}/${INSTALL_INCLUDEDIR}/*"
)
list(SORT expected_installed_headers)
list(SORT installed_headers)
if(NOT installed_headers STREQUAL expected_installed_headers)
    message(
        FATAL_ERROR
        "staged install exposed unexpected headers: '${installed_headers}'; expected exactly '${expected_installed_headers}'"
    )
endif()

set(pkg_config_environment
    "PKG_CONFIG_PATH=${pkg_config_dir}"
    "PKG_CONFIG_LIBDIR=${pkg_config_dir}"
)
set(pkg_config_modules kahip)
if(WITH_PARHIP)
    list(APPEND pkg_config_modules parhip_interface)
endif()
foreach(module IN LISTS pkg_config_modules)
    execute_process(
        COMMAND
            "${CMAKE_COMMAND}" -E env
            ${pkg_config_environment}
            "${PKG_CONFIG_EXECUTABLE}" --variable=prefix "${module}"
        RESULT_VARIABLE prefix_result
        OUTPUT_VARIABLE reported_prefix
        ERROR_VARIABLE prefix_stderr
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(NOT prefix_result EQUAL 0)
        message(
            FATAL_ERROR
            "pkg-config could not inspect ${module}\n${prefix_stderr}"
        )
    endif()
    file(REAL_PATH "${reported_prefix}" normalized_reported_prefix)
    file(REAL_PATH "${STAGE_PREFIX}" normalized_stage_prefix)
    if(NOT normalized_reported_prefix STREQUAL normalized_stage_prefix)
        message(
            FATAL_ERROR
            "${module}.pc reports prefix '${reported_prefix}', expected staged prefix '${STAGE_PREFIX}'"
        )
    endif()
endforeach()

set(
    configure_command
    "${CMAKE_COMMAND}"
    -S "${CONSUMER_SOURCE_DIR}"
    -B "${CONSUMER_BINARY_DIR}"
    -G "${CONSUMER_GENERATOR}"
)
if(NOT "${CONSUMER_GENERATOR_PLATFORM}" STREQUAL "")
    list(APPEND configure_command -A "${CONSUMER_GENERATOR_PLATFORM}")
endif()
if(NOT "${CONSUMER_GENERATOR_TOOLSET}" STREQUAL "")
    list(APPEND configure_command -T "${CONSUMER_GENERATOR_TOOLSET}")
endif()
if(NOT "${CONSUMER_GENERATOR_INSTANCE}" STREQUAL "")
    list(
        APPEND
        configure_command
        "-DCMAKE_GENERATOR_INSTANCE=${CONSUMER_GENERATOR_INSTANCE}"
    )
endif()
if(NOT "${TOOLCHAIN_FILE}" STREQUAL "")
    list(APPEND configure_command "-DCMAKE_TOOLCHAIN_FILE=${TOOLCHAIN_FILE}")
endif()
list(
    APPEND
    configure_command
    "-DCMAKE_C_COMPILER=${C_COMPILER}"
    "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}"
    "-DCMAKE_BUILD_TYPE=${BUILD_CONFIG}"
    "-DPKG_CONFIG_EXECUTABLE=${PKG_CONFIG_EXECUTABLE}"
    "-DWITH_PARHIP=${WITH_PARHIP}"
    "-DSTAGE_BINDIR=${STAGE_PREFIX}/${INSTALL_BINDIR}"
    "-DSTAGE_INCLUDEDIR=${STAGE_PREFIX}/${INSTALL_INCLUDEDIR}"
    "-DSTAGE_LIBDIR=${STAGE_PREFIX}/${INSTALL_LIBDIR}"
    "-DKAHIP_STATIC_LIBRARY=${KAHIP_STATIC_LIBRARY}"
    "-DPARHIP_STATIC_LIBRARY=${PARHIP_STATIC_LIBRARY}"
)
kahip_append_consumer_cache_argument(
    configure_command
    KAHIP_64BIT
    "${KAHIP_64BIT}"
)
foreach(
    context_variable
    IN ITEMS
        CMAKE_C_FLAGS
        CMAKE_CXX_FLAGS
        CMAKE_EXE_LINKER_FLAGS
        CMAKE_POSITION_INDEPENDENT_CODE
        CMAKE_OSX_ARCHITECTURES
        CMAKE_OSX_SYSROOT
        CMAKE_OSX_DEPLOYMENT_TARGET
        VCPKG_INSTALLED_DIR
        VCPKG_TARGET_TRIPLET
)
    if(
        DEFINED ${context_variable}
        AND NOT "${${context_variable}}" STREQUAL ""
    )
        kahip_append_consumer_cache_argument(
            configure_command
            "${context_variable}"
            "${${context_variable}}"
        )
    endif()
endforeach()

execute_process(
    COMMAND
        "${CMAKE_COMMAND}" -E env
        ${pkg_config_environment}
        ${configure_command}
    RESULT_VARIABLE configure_result
    OUTPUT_VARIABLE configure_stdout
    ERROR_VARIABLE configure_stderr
)
if(NOT configure_result EQUAL 0)
    message(
        FATAL_ERROR
        "pkg-config consumer configure failed\n${configure_stdout}\n${configure_stderr}"
    )
endif()

set(build_command "${CMAKE_COMMAND}" --build "${CONSUMER_BINARY_DIR}")
if(NOT "${BUILD_CONFIG}" STREQUAL "")
    list(APPEND build_command --config "${BUILD_CONFIG}")
endif()
list(APPEND build_command --parallel 2)
execute_process(
    COMMAND ${build_command}
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_stdout
    ERROR_VARIABLE build_stderr
)
if(NOT build_result EQUAL 0)
    message(
        FATAL_ERROR
        "pkg-config consumer build failed\n${build_stdout}\n${build_stderr}"
    )
endif()

set(
    consumer_test_command
    "${CTEST_COMMAND}"
    --test-dir "${CONSUMER_BINARY_DIR}"
    --output-on-failure
)
if(NOT "${BUILD_CONFIG}" STREQUAL "")
    list(APPEND consumer_test_command --build-config "${BUILD_CONFIG}")
endif()
execute_process(
    COMMAND ${consumer_test_command}
    RESULT_VARIABLE consumer_test_result
    OUTPUT_VARIABLE consumer_test_stdout
    ERROR_VARIABLE consumer_test_stderr
)
if(NOT consumer_test_result EQUAL 0)
    message(
        FATAL_ERROR
        "pkg-config consumers failed at runtime\n${consumer_test_stdout}\n${consumer_test_stderr}"
    )
endif()
