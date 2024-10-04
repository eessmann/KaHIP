add_library(argtable3 argtable3.c)
target_include_directories(argtable3 SYSTEM PUBLIC ${CMAKE_SOURCE_DIR}/extern/argtable3-3.2.2/)