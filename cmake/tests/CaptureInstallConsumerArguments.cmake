cmake_minimum_required(VERSION 4.0...4.3)

if(NOT DEFINED OUTPUT_FILE OR OUTPUT_FILE STREQUAL "")
    message(FATAL_ERROR "OUTPUT_FILE is required")
endif()

file(
    WRITE "${OUTPUT_FILE}"
    "architectures=${CMAKE_OSX_ARCHITECTURES}\n"
    "sysroot=${CMAKE_OSX_SYSROOT}\n"
    "deployment_target=${CMAKE_OSX_DEPLOYMENT_TARGET}\n"
    "vcpkg_installed_dir=${VCPKG_INSTALLED_DIR}\n"
    "vcpkg_target_triplet=${VCPKG_TARGET_TRIPLET}\n"
    "kahip_64bit=${KAHIP_64BIT}\n"
    "target_windows=${TARGET_WINDOWS}\n"
)
