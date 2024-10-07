include(${CMAKE_SOURCE_DIR}/cmake/Cache.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/CompilerWarnings.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/InterproceduralOptimization.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/Sanitizers.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/StaticAnalyzers.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/Utilities.cmake)

kahip_supports_sanitizers()

option(kahip_ENABLE_IPO "Enable IPO/LTO" OFF)
option(kahip_WARNINGS_AS_ERRORS "Treat Warnings As Errors" OFF)
option(kahip_ENABLE_SANITIZERS "Enable sanitizers" OFF)
option(kahip_ENABLE_SANITIZER_ADDRESS "Enable address sanitizer" ${SUPPORTS_ASAN})
option(kahip_ENABLE_SANITIZER_LEAK "Enable leak sanitizer" OFF)
option(kahip_ENABLE_SANITIZER_UNDEFINED "Enable undefined sanitizer" ${SUPPORTS_UBSAN})
option(kahip_ENABLE_SANITIZER_THREAD "Enable thread sanitizer" OFF)
option(kahip_ENABLE_SANITIZER_MEMORY "Enable memory sanitizer" OFF)
option(kahip_ENABLE_CLANG_TIDY "Enable clang-tidy" OFF)
option(kahip_ENABLE_CPPCHECK "Enable cpp-check analysis" OFF)
option(kahip_ENABLE_IWYU "Enable `include_what_you_use`" OFF)
option(kahip_ENABLE_CACHE "Enable ccache" OFF)


if(kahip_ENABLE_IPO)
    kahip_enable_ipo()
endif()

add_library(kahip_warnings INTERFACE)
add_library(kahip_options INTERFACE)

kahip_set_project_warnings(
        kahip_warnings
        ${kahip_WARNINGS_AS_ERRORS}
        ""
        ""
        ""
        "")

if (kahip_ENABLE_SANITIZERS)
    kahip_enable_sanitizers(
            kahip_options
            ${kahip_ENABLE_SANITIZER_ADDRESS}
            ${kahip_ENABLE_SANITIZER_LEAK}
            ${kahip_ENABLE_SANITIZER_UNDEFINED}
            ${kahip_ENABLE_SANITIZER_THREAD}
            ${kahip_ENABLE_SANITIZER_MEMORY})
endif ()

if(kahip_ENABLE_CLANG_TIDY)
    kahip_enable_clang_tidy(kahip_options ${kahip_WARNINGS_AS_ERRORS})
endif()

if(kahip_ENABLE_CPPCHECK)
    kahip_enable_cppcheck(${kahip_WARNINGS_AS_ERRORS} "" # override cppcheck options
    )
endif()

if(kahip_ENABLE_IWYU)
    kahip_enable_include_what_you_use()
endif()