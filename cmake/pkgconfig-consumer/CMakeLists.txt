cmake_minimum_required(VERSION 4.0...4.3)
project(KaHIPPkgConfigConsumer LANGUAGES C CXX)

include(CTest)

if(NOT DEFINED KAHIP_64BIT)
    message(FATAL_ERROR "KAHIP_64BIT must match the staged KaHIP package")
endif()

find_package(PkgConfig REQUIRED)
find_package(OpenMP QUIET)

function(add_c_api_consumer target)
    add_executable(${target} ${ARGN})
    target_compile_features(${target} PRIVATE c_std_17)
    # Compile the public headers as C, but use the C++ linker for KaHIP's C++
    # implementation and its transitive runtime dependencies.
    set_target_properties(${target} PROPERTIES LINKER_LANGUAGE CXX)
endfunction()

pkg_check_modules(KAHIP REQUIRED IMPORTED_TARGET kahip)
add_executable(kahip_pkgconfig_consumer kahip_consumer.cpp)
target_compile_features(kahip_pkgconfig_consumer PRIVATE cxx_std_23)
target_link_libraries(kahip_pkgconfig_consumer PRIVATE PkgConfig::KAHIP)
add_test(NAME run-kahip-pkgconfig-consumer COMMAND kahip_pkgconfig_consumer)

add_c_api_consumer(
    kahip_c_pkgconfig_consumer
    kahip_c_consumer.c
    kahip_c_constants.c
)
target_link_libraries(
    kahip_c_pkgconfig_consumer
    PRIVATE PkgConfig::KAHIP
)
add_test(
    NAME run-kahip-c-pkgconfig-consumer
    COMMAND kahip_c_pkgconfig_consumer
)

add_library(kahip_installed_static STATIC IMPORTED)
set_target_properties(
    kahip_installed_static
    PROPERTIES
        IMPORTED_LOCATION "${STAGE_LIBDIR}/${KAHIP_STATIC_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${STAGE_INCLUDEDIR}"
)
if(KAHIP_64BIT)
    target_compile_definitions(kahip_installed_static INTERFACE KAHIP_64BIT)
endif()
if(OpenMP_CXX_FOUND)
    target_link_libraries(
        kahip_installed_static
        INTERFACE OpenMP::OpenMP_CXX
    )
endif()
add_executable(kahip_static_consumer kahip_consumer.cpp)
target_compile_features(kahip_static_consumer PRIVATE cxx_std_23)
target_link_libraries(kahip_static_consumer PRIVATE kahip_installed_static)
add_test(NAME run-kahip-static-consumer COMMAND kahip_static_consumer)

add_c_api_consumer(
    kahip_c_static_consumer
    kahip_c_consumer.c
    kahip_c_constants.c
)
target_link_libraries(
    kahip_c_static_consumer
    PRIVATE kahip_installed_static
)
add_test(NAME run-kahip-c-static-consumer COMMAND kahip_c_static_consumer)

if(WITH_PARHIP)
    find_package(MPI 3.1 REQUIRED COMPONENTS C CXX)
    find_package(fmt CONFIG REQUIRED)
    find_package(spdlog CONFIG REQUIRED)
    pkg_check_modules(PARHIP REQUIRED IMPORTED_TARGET parhip_interface)
    add_executable(parhip_pkgconfig_consumer parhip_consumer.cpp)
    target_compile_features(parhip_pkgconfig_consumer PRIVATE cxx_std_23)
    target_link_libraries(parhip_pkgconfig_consumer PRIVATE PkgConfig::PARHIP)
    add_test(
        NAME run-parhip-pkgconfig-consumer
        COMMAND parhip_pkgconfig_consumer
    )

    add_c_api_consumer(
        parhip_c_pkgconfig_consumer
        parhip_c_consumer.c
        parhip_c_constants.c
    )
    target_link_libraries(
        parhip_c_pkgconfig_consumer
        PRIVATE PkgConfig::PARHIP
    )
    add_test(
        NAME run-parhip-c-pkgconfig-consumer
        COMMAND parhip_c_pkgconfig_consumer
    )

    add_executable(combined_pkgconfig_consumer combined_static_consumer.cpp)
    target_compile_features(combined_pkgconfig_consumer PRIVATE cxx_std_23)
    target_link_libraries(
        combined_pkgconfig_consumer
        PRIVATE PkgConfig::KAHIP PkgConfig::PARHIP
    )
    add_test(
        NAME run-combined-pkgconfig-consumer
        COMMAND combined_pkgconfig_consumer
    )
    add_executable(
        combined_reverse_pkgconfig_consumer
        combined_reverse_consumer.cpp
    )
    target_compile_features(
        combined_reverse_pkgconfig_consumer
        PRIVATE cxx_std_23
    )
    target_link_libraries(
        combined_reverse_pkgconfig_consumer
        PRIVATE PkgConfig::KAHIP PkgConfig::PARHIP
    )
    add_test(
        NAME run-combined-reverse-pkgconfig-consumer
        COMMAND combined_reverse_pkgconfig_consumer
    )
    add_c_api_consumer(
        combined_c_pkgconfig_consumer
        combined_c_consumer.c
        combined_reverse_c_constants.c
    )
    target_link_libraries(
        combined_c_pkgconfig_consumer
        PRIVATE PkgConfig::KAHIP PkgConfig::PARHIP
    )
    add_test(
        NAME run-combined-c-pkgconfig-consumer
        COMMAND combined_c_pkgconfig_consumer
    )

    add_library(parhip_installed_static STATIC IMPORTED)
    set_target_properties(
        parhip_installed_static
        PROPERTIES
            IMPORTED_LOCATION "${STAGE_LIBDIR}/${PARHIP_STATIC_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${STAGE_INCLUDEDIR}"
    )
    target_link_libraries(
        parhip_installed_static
        INTERFACE MPI::MPI_CXX fmt::fmt spdlog::spdlog
    )
    if(OpenMP_CXX_FOUND)
        target_link_libraries(
            parhip_installed_static
            INTERFACE OpenMP::OpenMP_CXX
        )
    endif()
    add_executable(parhip_static_consumer parhip_consumer.cpp)
    target_compile_features(parhip_static_consumer PRIVATE cxx_std_23)
    target_link_libraries(
        parhip_static_consumer
        PRIVATE parhip_installed_static
    )
    add_test(NAME run-parhip-static-consumer COMMAND parhip_static_consumer)

    add_c_api_consumer(
        parhip_c_static_consumer
        parhip_c_consumer.c
        parhip_c_constants.c
    )
    target_link_libraries(
        parhip_c_static_consumer
        PRIVATE parhip_installed_static
    )
    add_test(
        NAME run-parhip-c-static-consumer
        COMMAND parhip_c_static_consumer
    )

    add_executable(combined_static_consumer combined_static_consumer.cpp)
    target_compile_features(combined_static_consumer PRIVATE cxx_std_23)
    target_link_libraries(
        combined_static_consumer
        PRIVATE kahip_installed_static parhip_installed_static
    )
    add_test(NAME run-combined-static-consumer COMMAND combined_static_consumer)
    add_executable(
        combined_reverse_static_consumer
        combined_reverse_consumer.cpp
    )
    target_compile_features(
        combined_reverse_static_consumer
        PRIVATE cxx_std_23
    )
    target_link_libraries(
        combined_reverse_static_consumer
        PRIVATE kahip_installed_static parhip_installed_static
    )
    add_test(
        NAME run-combined-reverse-static-consumer
        COMMAND combined_reverse_static_consumer
    )
    add_c_api_consumer(
        combined_c_static_consumer
        combined_c_consumer.c
        combined_reverse_c_constants.c
    )
    target_link_libraries(
        combined_c_static_consumer
        PRIVATE kahip_installed_static parhip_installed_static
    )
    add_test(
        NAME run-combined-c-static-consumer
        COMMAND combined_c_static_consumer
    )
endif()

if(WIN32)
    set(loader_environment "PATH=path_list_prepend:${STAGE_BINDIR}")
elseif(APPLE)
    set(
        loader_environment
        "DYLD_LIBRARY_PATH=path_list_prepend:${STAGE_LIBDIR}"
    )
else()
    set(
        loader_environment
        "LD_LIBRARY_PATH=path_list_prepend:${STAGE_LIBDIR}"
    )
endif()
set_tests_properties(
    run-kahip-pkgconfig-consumer
    run-kahip-c-pkgconfig-consumer
    run-kahip-static-consumer
    run-kahip-c-static-consumer
    PROPERTIES ENVIRONMENT_MODIFICATION "${loader_environment}"
)
if(WITH_PARHIP)
    set_tests_properties(
        run-parhip-pkgconfig-consumer
        run-parhip-c-pkgconfig-consumer
        run-combined-pkgconfig-consumer
        run-combined-reverse-pkgconfig-consumer
        run-combined-c-pkgconfig-consumer
        run-parhip-static-consumer
        run-parhip-c-static-consumer
        run-combined-static-consumer
        run-combined-reverse-static-consumer
        run-combined-c-static-consumer
        PROPERTIES ENVIRONMENT_MODIFICATION "${loader_environment}"
    )
endif()
