cmake_minimum_required(VERSION 4.0...4.3)
project(parhip_example LANGUAGES CXX)

set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(
    parhip
    REQUIRED
    IMPORTED_TARGET
    parhip_interface
)

add_executable(parhip_test parhip_test.cpp)
target_compile_features(parhip_test PRIVATE cxx_std_23)
target_link_libraries(parhip_test PRIVATE PkgConfig::parhip)
