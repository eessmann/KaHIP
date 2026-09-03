cmake_minimum_required(VERSION 4.0)

if(NOT DEFINED BUILD_DIR OR BUILD_DIR STREQUAL "")
  message(FATAL_ERROR "BUILD_DIR is required")
endif()
if(NOT PROFILE MATCHES "^(mpi3-floor|mpi4)$")
  message(FATAL_ERROR "PROFILE must be mpi3-floor or mpi4")
endif()

cmake_path(ABSOLUTE_PATH BUILD_DIR NORMALIZE OUTPUT_VARIABLE build_dir)
set(capability_header
    "${build_dir}/parallel/parallel_src/generated/kahip_mpi_capabilities.h")
set(cache_file "${build_dir}/CMakeCache.txt")

if(NOT EXISTS "${capability_header}")
  message(FATAL_ERROR "Missing generated MPI capability header: ${capability_header}")
endif()
if(NOT EXISTS "${cache_file}")
  message(FATAL_ERROR "Missing CMake cache: ${cache_file}")
endif()

function(read_header_capability name output)
  file(STRINGS "${capability_header}" definition
       REGEX "^#define ${name} [01]$")
  list(LENGTH definition definition_count)
  if(NOT definition_count EQUAL 1)
    message(FATAL_ERROR
      "Expected exactly one 0/1 definition for ${name} in ${capability_header}")
  endif()
  string(REGEX REPLACE "^#define ${name} ([01])$" "\\1" value "${definition}")
  set("${output}" "${value}" PARENT_SCOPE)
endfunction()

function(read_cache_boolean name output)
  file(STRINGS "${cache_file}" cache_entry REGEX "^${name}:[^=]*=")
  list(LENGTH cache_entry entry_count)
  if(NOT entry_count EQUAL 1)
    message(FATAL_ERROR
      "Expected exactly one cache entry for ${name} in ${cache_file}")
  endif()
  string(REGEX REPLACE "^[^=]*=" "" raw_value "${cache_entry}")
  string(TOUPPER "${raw_value}" normalized_value)
  if(normalized_value MATCHES "^(1|ON|TRUE|YES|Y)$")
    set(value 1)
  elseif(normalized_value MATCHES "^(|0|OFF|FALSE|NO|N|IGNORE|NOTFOUND)$"
         OR normalized_value MATCHES "-NOTFOUND$")
    set(value 0)
  else()
    message(FATAL_ERROR
      "${name} has non-boolean cache value '${raw_value}' in ${cache_file}")
  endif()
  set("${output}" "${value}" PARENT_SCOPE)
endfunction()

set(header_capabilities
    KAHIP_HAVE_MPI_ALLTOALLV_C
    KAHIP_HAVE_MPI_ALLREDUCE_C
    KAHIP_HAVE_MPI_REDUCE_C
    KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_C
    KAHIP_HAVE_MPI_INEIGHBOR_ALLTOALLV
    KAHIP_HAVE_MPI_INEIGHBOR_ALLTOALLV_C
    KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_INIT
    KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_INIT_C)

foreach(capability IN LISTS header_capabilities)
  read_header_capability("${capability}" "${capability}")
endforeach()
read_cache_boolean(KAHIP_HAVE_MPI_BCAST_C KAHIP_HAVE_MPI_BCAST_C)

if(PROFILE STREQUAL "mpi3-floor")
  set(expected_KAHIP_HAVE_MPI_BCAST_C 0)
  set(expected_KAHIP_HAVE_MPI_ALLTOALLV_C 0)
  set(expected_KAHIP_HAVE_MPI_ALLREDUCE_C 0)
  set(expected_KAHIP_HAVE_MPI_REDUCE_C 0)
  set(expected_KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_C 0)
  set(expected_KAHIP_HAVE_MPI_INEIGHBOR_ALLTOALLV 1)
  set(expected_KAHIP_HAVE_MPI_INEIGHBOR_ALLTOALLV_C 0)
  set(expected_KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_INIT_C 0)
else()
  set(expected_KAHIP_HAVE_MPI_BCAST_C 1)
  foreach(capability IN LISTS header_capabilities)
    set("expected_${capability}" 1)
  endforeach()
endif()

set(all_capabilities KAHIP_HAVE_MPI_BCAST_C ${header_capabilities})
foreach(capability IN LISTS all_capabilities)
  set(expected_variable "expected_${capability}")
  if(NOT DEFINED "${expected_variable}")
    message(STATUS "${capability}=${${capability}} (optional for ${PROFILE})")
    continue()
  endif()
  if(NOT "${${capability}}" STREQUAL "${${expected_variable}}")
    message(FATAL_ERROR
      "${PROFILE} requires ${capability}=${${expected_variable}}, "
      "but ${build_dir} detected ${${capability}}")
  endif()
  message(STATUS "${capability}=${${capability}}")
endforeach()

message(STATUS "MPI capability profile '${PROFILE}' verified")
