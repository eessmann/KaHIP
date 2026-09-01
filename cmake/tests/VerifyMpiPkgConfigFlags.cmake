cmake_minimum_required(VERSION 4.0...4.3)

foreach(required IN ITEMS KAHIP_SOURCE_DIR PKG_CONFIG_EXECUTABLE WORK_DIRECTORY)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

include("${KAHIP_SOURCE_DIR}/cmake/KahipPkgConfig.cmake")

kahip_format_mpi_pkg_config_flags(
    mpi_cflags
    mpi_libs
    INCLUDE_DIRECTORIES "/opt/MPI SDK/include"
    COMPILE_DEFINITIONS "MPI_FEATURE=1"
    COMPILE_OPTIONS -pthread -fopenmp
    LINK_OPTIONS -pthread -Wl,--as-needed
    LIBRARIES "/opt/MPI SDK/lib/libmpi.so" mpi_cxx
)

set(
    expected_cflags
    "-I/opt/MPI\\ SDK/include -DMPI_FEATURE=1 -pthread -fopenmp"
)
set(
    expected_libs
    "-pthread -Wl,--as-needed /opt/MPI\\ SDK/lib/libmpi.so -lmpi_cxx"
)

if(NOT mpi_cflags STREQUAL expected_cflags)
    message(
        FATAL_ERROR
        "unexpected MPI Cflags\nexpected: ${expected_cflags}\nactual:   ${mpi_cflags}"
    )
endif()
if(NOT mpi_libs STREQUAL expected_libs)
    message(
        FATAL_ERROR
        "unexpected MPI Libs\nexpected: ${expected_libs}\nactual:   ${mpi_libs}"
    )
endif()

file(REMOVE_RECURSE "${WORK_DIRECTORY}")
file(MAKE_DIRECTORY "${WORK_DIRECTORY}")
set(synthetic_pc "${WORK_DIRECTORY}/kahip-mpi-flags.pc")
file(
    WRITE "${synthetic_pc}"
    "Name: kahip-mpi-flags\n"
    "Description: synthetic MPI flag round-trip fixture\n"
    "Version: 1\n"
    "Cflags: ${mpi_cflags}\n"
    "Libs: ${mpi_libs}\n"
)

set(
    pkg_config_environment
    "PKG_CONFIG_PATH=${WORK_DIRECTORY}"
    "PKG_CONFIG_LIBDIR=${WORK_DIRECTORY}"
)
foreach(flag_kind IN ITEMS cflags libs)
    execute_process(
        COMMAND
            "${CMAKE_COMMAND}" -E env
            ${pkg_config_environment}
            "${PKG_CONFIG_EXECUTABLE}" --${flag_kind} kahip-mpi-flags
        RESULT_VARIABLE pkg_config_result
        OUTPUT_VARIABLE pkg_config_output
        ERROR_VARIABLE pkg_config_error
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(NOT pkg_config_result EQUAL 0)
        message(
            FATAL_ERROR
            "pkg-config --${flag_kind} failed\n${pkg_config_error}"
        )
    endif()
    separate_arguments(parsed_${flag_kind} UNIX_COMMAND "${pkg_config_output}")
endforeach()

set(
    expected_parsed_cflags
    "-I/opt/MPI SDK/include"
    -DMPI_FEATURE=1
    -pthread
    -fopenmp
)
set(
    expected_parsed_libs
    -pthread
    -Wl,--as-needed
    "/opt/MPI SDK/lib/libmpi.so"
    -lmpi_cxx
)
if(NOT parsed_cflags STREQUAL expected_parsed_cflags)
    message(
        FATAL_ERROR
        "pkg-config changed MPI Cflags tokenization\nexpected: ${expected_parsed_cflags}\nactual:   ${parsed_cflags}"
    )
endif()
if(NOT parsed_libs STREQUAL expected_parsed_libs)
    message(
        FATAL_ERROR
        "pkg-config changed MPI Libs tokenization\nexpected: ${expected_parsed_libs}\nactual:   ${parsed_libs}"
    )
endif()
