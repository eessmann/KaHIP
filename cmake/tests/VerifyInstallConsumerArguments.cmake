cmake_minimum_required(VERSION 4.0...4.3)

foreach(required IN ITEMS KAHIP_SOURCE_DIR WORK_DIRECTORY)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

include("${KAHIP_SOURCE_DIR}/cmake/KahipInstallConsumerArguments.cmake")

file(REMOVE_RECURSE "${WORK_DIRECTORY}")
file(MAKE_DIRECTORY "${WORK_DIRECTORY}")
set(captured_arguments "${WORK_DIRECTORY}/captured-arguments.txt")
set(
    configure_command
    "${CMAKE_COMMAND}"
    "-DOUTPUT_FILE=${captured_arguments}"
)
kahip_append_consumer_cache_argument(
    configure_command
    CMAKE_OSX_ARCHITECTURES
    "arm64;x86_64"
)
kahip_append_consumer_cache_argument(
    configure_command
    CMAKE_OSX_SYSROOT
    "/SDKs/MacOSX.sdk"
)
kahip_append_consumer_cache_argument(
    configure_command
    CMAKE_OSX_DEPLOYMENT_TARGET
    "14.0"
)
kahip_append_consumer_cache_argument(
    configure_command
    VCPKG_INSTALLED_DIR
    "/tmp/vcpkg installed"
)
kahip_append_consumer_cache_argument(
    configure_command
    VCPKG_TARGET_TRIPLET
    "arm64-osx"
)
kahip_append_consumer_cache_argument(configure_command KAHIP_64BIT "ON")
kahip_append_consumer_cache_argument(configure_command TARGET_WINDOWS "OFF")
list(
    APPEND
    configure_command
    -P
    "${KAHIP_SOURCE_DIR}/cmake/tests/CaptureInstallConsumerArguments.cmake"
)

execute_process(
    COMMAND ${configure_command}
    RESULT_VARIABLE capture_result
    OUTPUT_VARIABLE capture_stdout
    ERROR_VARIABLE capture_stderr
)
if(NOT capture_result EQUAL 0)
    message(
        FATAL_ERROR
        "consumer argument capture failed\n${capture_stdout}\n${capture_stderr}"
    )
endif()

file(READ "${captured_arguments}" actual_arguments)
string(
    CONCAT expected_arguments
    "architectures=arm64;x86_64\n"
    "sysroot=/SDKs/MacOSX.sdk\n"
    "deployment_target=14.0\n"
    "vcpkg_installed_dir=/tmp/vcpkg installed\n"
    "vcpkg_target_triplet=arm64-osx\n"
    "kahip_64bit=ON\n"
    "target_windows=OFF\n"
)
if(NOT actual_arguments STREQUAL expected_arguments)
    message(
        FATAL_ERROR
        "consumer cache arguments changed in transit\nexpected:\n${expected_arguments}actual:\n${actual_arguments}"
    )
endif()
