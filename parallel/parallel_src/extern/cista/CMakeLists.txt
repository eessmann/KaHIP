cmake_minimum_required(VERSION 3.8)

if (NOT DEFINED PROJECT_NAME AND CISTA_HASH STREQUAL "FNV1A")
  set(CISTA_INSTALL ON)
endif()

project(cista LANGUAGES CXX VERSION 0.7)

include(GNUInstallDirs)

if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang" OR
    "${CMAKE_CXX_COMPILER_ID}" STREQUAL "AppleClang")
  set(cista-compile-flags
     -Wno-unknown-warning-option
     -Wno-global-constructors
     -Wno-exit-time-destructors
     -fno-strict-aliasing
     -Weverything
     -Wno-c++98-compat
     -Wno-c++98-compat-pedantic
     -Wno-newline-eof
     -Wno-missing-prototypes
     -Wno-padded
     -Wno-double-promotion
     -Wno-undef
     -Wno-undefined-reinterpret-cast
     -Wno-float-conversion
     -Wno-gnu-zero-variadic-macro-arguments
     -Wno-unknown-pragmas
     -Wno-documentation-unknown-command
     -Wno-reserved-identifier
     -Wno-weak-vtables
     -Wno-unneeded-member-function
     -Wno-unused-member-function
     -Wno-unsafe-buffer-usage
     -Wno-deprecated-declarations
     -Wno-ctad-maybe-unsupported
     -Wno-self-assign-overloaded
     -Werror
  )
elseif("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
  set(cista-compile-flags -Wall -Wextra)
endif()

option(CISTA_ZERO_OUT "zero out fresh memory for valgrind" OFF)
option(CISTA_COVERAGE "generate coverage report" OFF)
option(CISTA_GENERATE_TO_TUPLE "generate include/cista/reflection/to_tuple.h" OFF)
option(CISTA_USE_MIMALLOC "compile with mimalloc support" OFF)
set(CISTA_HASH "FNV1A" CACHE STRING "Options: FNV1A XXH3 WYHASH WYHASH_FASTEST")

add_library(cista INTERFACE)
if (CISTA_HASH STREQUAL "XXH3")
  add_subdirectory(tools/xxh3)
  target_link_libraries(cista INTERFACE xxh3)
elseif(CISTA_HASH STREQUAL "WYHASH" OR CISTA_HASH STREQUAL "WYHASH_FASTEST")
  add_subdirectory(tools/wyhash)
  target_link_libraries(cista INTERFACE wyhash)
endif()
target_compile_definitions(cista INTERFACE CISTA_${CISTA_HASH}=1)
if (CISTA_ZERO_OUT)
  target_compile_definitions(cista INTERFACE CISTA_ZERO_OUT=1)
endif()
if (CISTA_USE_MIMALLOC)
  target_compile_definitions(cista INTERFACE CISTA_USE_MIMALLOC=1)
endif()
target_include_directories(cista SYSTEM INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(cista INTERFACE cxx_std_17)

if (${CISTA_GENERATE_TO_TUPLE})
  add_subdirectory(tools/to_tuple_generator EXCLUDE_FROM_ALL)
  add_custom_target(generate_to_tuple
    COMMAND to_tuple_generator
      64  # max number of supported member fields
    > ${CMAKE_CURRENT_SOURCE_DIR}/include/cista/reflection/to_tuple.h
  )
  add_dependencies(cista generate_to_tuple)
endif()

file(GLOB_RECURSE cista-include-files include/*.h*)

add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/cista.h
  COMMAND uniter
    ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include/cista/serialization.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/cista/reflection/comparable.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/cista/reflection/printable.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/cista/reflection/member_index.h
  > ${CMAKE_CURRENT_BINARY_DIR}/cista.h
  DEPENDS ${cista-include-files}
)

add_library(cista::cista ALIAS cista)

# Export targets when not used via `add_subdirectory`
if (CISTA_INSTALL)
  include(CMakePackageConfigHelpers)
  set(CISTA_CMAKE_CONFIG_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/cista")

  configure_package_config_file(
    ${CMAKE_CURRENT_LIST_DIR}/CMake/cistaConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/cistaConfig.cmake
    INSTALL_DESTINATION ${CISTA_CMAKE_CONFIG_DESTINATION}
  )

  install(
    TARGETS cista
    EXPORT cistaTargets
    DESTINATION ${CMAKE_INSTALL_LIBDIR}
  )

  install(
    EXPORT cistaTargets
    NAMESPACE cista::
    DESTINATION ${CISTA_CMAKE_CONFIG_DESTINATION}
  )

  install(
    DIRECTORY "include/"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  )

  write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/cistaConfigVersion.cmake"
    COMPATIBILITY SameMajorVersion
  )

  install(
    FILES
      "${CMAKE_CURRENT_BINARY_DIR}/cistaConfig.cmake"
      "${CMAKE_CURRENT_BINARY_DIR}/cistaConfigVersion.cmake"
    DESTINATION ${CISTA_CMAKE_CONFIG_DESTINATION}
  )
endif()
