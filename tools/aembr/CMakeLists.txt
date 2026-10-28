# aembr: compiles a typed part of embr to C and builds it with the system C compiler (see DESIGN.md). no LLVM, no new dependency.
# the runtime goes next to the binary, in <bin>/aembr-rt/{rt.h,libaembr_rt.a}, which is where aembr looks for it

add_library(aembr_rt STATIC rt/rt.c rt/rt_nat.c rt/rt_host.c rt/rt_any.c)
set_target_properties(aembr_rt PROPERTIES
    LINKER_LANGUAGE C
    OUTPUT_NAME aembr_rt
    ARCHIVE_OUTPUT_DIRECTORY "${BIN_DIR}/aembr-rt"
    POSITION_INDEPENDENT_CODE ON)
target_include_directories(aembr_rt PRIVATE "${CMAKE_SOURCE_DIR}/include")
target_compile_options(aembr_rt PRIVATE -O2 -fwrapv -fno-strict-overflow -Wall -Wextra -Wpedantic)
# the same runtime with counters (aembr build --stats), see RT_STAT in rt.h
add_library(aembr_rt_stats STATIC rt/rt.c rt/rt_nat.c rt/rt_host.c rt/rt_any.c)
set_target_properties(aembr_rt_stats PROPERTIES
    LINKER_LANGUAGE C
    OUTPUT_NAME aembr_rt_stats
    ARCHIVE_OUTPUT_DIRECTORY "${BIN_DIR}/aembr-rt"
    POSITION_INDEPENDENT_CODE ON)
target_include_directories(aembr_rt_stats PRIVATE "${CMAKE_SOURCE_DIR}/include")
target_compile_definitions(aembr_rt_stats PRIVATE AEMBR_STATS)
target_compile_options(aembr_rt_stats PRIVATE -O2 -fwrapv -fno-strict-overflow -Wall -Wextra -Wpedantic)
# what a program or a plugin is built with, next to the library: the headers of the runtime and the plugin interface. a target of its
# own that always runs (copy_if_different is cheap), so a changed header is never left behind
add_custom_target(aembr_rt_headers ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${BIN_DIR}/aembr-rt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/rt/rt.h" "${BIN_DIR}/aembr-rt/rt.h"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/rt/rt_plugin.h" "${BIN_DIR}/aembr-rt/rt_plugin.h"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/rt/rt_host.h" "${BIN_DIR}/aembr-rt/rt_host.h"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/include/embr_plugin.h" "${BIN_DIR}/aembr-rt/embr_plugin.h"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/std" "${BIN_DIR}/aembr-rt/std"
    VERBATIM)

add_executable(aembr main.cpp front.cpp lower.cpp ir.cpp emit_c.cpp build.cpp plugins.cpp record.cpp pipeline.cpp packaging.cpp)
target_link_libraries(aembr PRIVATE embr_single_header ${CMAKE_DL_LIBS})
add_dependencies(aembr aembr_rt aembr_rt_stats aembr_rt_headers)
embr_use_pch(aembr exe)
set_target_properties(aembr PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${BIN_DIR}")
target_compile_options(aembr PRIVATE $<$<CONFIG:Debug>:-g>)

install(TARGETS aembr RUNTIME DESTINATION bin)
install(FILES "${BIN_DIR}/aembr-rt/libaembr_rt.a" "${BIN_DIR}/aembr-rt/libaembr_rt_stats.a" rt/rt.h rt/rt_plugin.h rt/rt_host.h "${CMAKE_SOURCE_DIR}/include/embr_plugin.h" DESTINATION lib/embr/aembr-rt)
install(DIRECTORY std DESTINATION lib/embr/aembr-rt)
