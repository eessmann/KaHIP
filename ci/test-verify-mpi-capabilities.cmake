cmake_minimum_required(VERSION 4.0)

if(NOT DEFINED SOURCE_DIR)
  get_filename_component(SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()

set(verifier "${SOURCE_DIR}/ci/verify-mpi-capabilities.cmake")
if(NOT EXISTS "${verifier}")
  message(FATAL_ERROR "MPI capability verifier is missing: ${verifier}")
endif()

if(NOT DEFINED TEST_ROOT)
  string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef test_suffix)
  set(TEST_ROOT "/tmp/kahip-mpi-capabilities-${test_suffix}")
endif()

function(write_fixture fixture bcast_c alltoallv_c allreduce_c reduce_c
         neighbor_alltoallv_c ineighbor_alltoallv ineighbor_alltoallv_c
         neighbor_alltoallv_init neighbor_alltoallv_init_c)
  set(generated_dir
      "${TEST_ROOT}/${fixture}/parallel/parallel_src/generated")
  file(MAKE_DIRECTORY "${generated_dir}")
  file(WRITE "${generated_dir}/kahip_mpi_capabilities.h"
    "#define KAHIP_HAVE_MPI_ALLTOALLV_C ${alltoallv_c}\n"
    "#define KAHIP_HAVE_MPI_ALLREDUCE_C ${allreduce_c}\n"
    "#define KAHIP_HAVE_MPI_REDUCE_C ${reduce_c}\n"
    "#define KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_C ${neighbor_alltoallv_c}\n"
    "#define KAHIP_HAVE_MPI_INEIGHBOR_ALLTOALLV ${ineighbor_alltoallv}\n"
    "#define KAHIP_HAVE_MPI_INEIGHBOR_ALLTOALLV_C ${ineighbor_alltoallv_c}\n"
    "#define KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_INIT ${neighbor_alltoallv_init}\n"
    "#define KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_INIT_C ${neighbor_alltoallv_init_c}\n")
  file(WRITE "${TEST_ROOT}/${fixture}/CMakeCache.txt"
    "KAHIP_HAVE_MPI_BCAST_C:INTERNAL=${bcast_c}\n")
endfunction()

function(expect_profile fixture profile should_pass)
  execute_process(
    COMMAND "${CMAKE_COMMAND}"
      "-DBUILD_DIR=${TEST_ROOT}/${fixture}"
      "-DPROFILE=${profile}"
      -P "${verifier}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error)

  if(should_pass AND NOT result EQUAL 0)
    message(FATAL_ERROR
      "${profile} unexpectedly rejected ${fixture}:\n${output}${error}")
  elseif(NOT should_pass AND result EQUAL 0)
    message(FATAL_ERROR
      "${profile} unexpectedly accepted ${fixture}:\n${output}${error}")
  endif()
endfunction()

write_fixture(mpi3 "" 0 0 0 0 1 0 0 0)
expect_profile(mpi3 mpi3-floor TRUE)

write_fixture(mpi3_with_legacy_persistent "" 0 0 0 0 1 0 1 0)
expect_profile(mpi3_with_legacy_persistent mpi3-floor TRUE)

write_fixture(mpi3_with_large_count 1 1 0 0 0 1 0 0 0)
expect_profile(mpi3_with_large_count mpi3-floor FALSE)

write_fixture(mpi3_with_reduction_large_count "" 0 1 1 0 1 0 0 0)
expect_profile(mpi3_with_reduction_large_count mpi3-floor FALSE)

write_fixture(mpi4 1 1 1 1 1 1 1 1 1)
expect_profile(mpi4 mpi4 TRUE)

write_fixture(mpi4_without_allreduce_c 1 1 0 1 1 1 1 1 1)
expect_profile(mpi4_without_allreduce_c mpi4 FALSE)

write_fixture(mpi4_without_reduce_c 1 1 1 0 1 1 1 1 1)
expect_profile(mpi4_without_reduce_c mpi4 FALSE)

write_fixture(mpi4_without_persistent_c 1 1 1 1 1 1 1 1 0)
expect_profile(mpi4_without_persistent_c mpi4 FALSE)

file(REMOVE_RECURSE "${TEST_ROOT}")
message(STATUS "MPI capability profile verifier self-test passed")
