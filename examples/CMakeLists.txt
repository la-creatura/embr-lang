# runnable examples
# every examples/*.embr has a golden examples/<name>.out and is executed by CTest on each backend

set(PLUGIN_DIR "${BIN_DIR}/plugins")
add_plugin(hello hello.cpp)            # examples/hello/hello.cpp -> bin/<platform>/plugins/hello.so

# a plugin written in C against include/embr_plugin.h. it links nothing from embr and uses no C++
enable_language(C)
add_library(hello_c_plugin SHARED hello_c/hello_c.c)
target_include_directories(hello_c_plugin PRIVATE ${CMAKE_SOURCE_DIR}/include)
set_target_properties(hello_c_plugin PROPERTIES OUTPUT_NAME hello_c PREFIX ""
    C_VISIBILITY_PRESET hidden LIBRARY_OUTPUT_DIRECTORY "${PLUGIN_DIR}" RUNTIME_OUTPUT_DIRECTORY "${PLUGIN_DIR}")

if(NOT BUILD_TESTING)
    return()
endif()

set(example_work "${BIN_DIR}/test-work")
file(GLOB example_scripts CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.embr")
foreach(script ${example_scripts})
    get_filename_component(ex "${script}" NAME_WE)
    # the first line can say what an example needs: "# requires: posix" means it uses something windows lacks
    # (os_exec), so skip it there. "# requires: <backend>" runs it on that backend only (coroutines needs vm)
    file(STRINGS "${script}" first_line LIMIT_COUNT 1)
    set(requires "")
    if(first_line MATCHES "requires: ([a-z]+)")
        set(requires "${CMAKE_MATCH_1}")
    endif()
    if(requires STREQUAL "posix" AND WIN32)
        continue()
    endif()

    foreach(backend ${EMBR_BACKENDS})
        if(requires IN_LIST EMBR_BACKENDS AND NOT requires STREQUAL backend)
            continue()
        endif()
        set(test_name example_${ex}_${backend})
        file(MAKE_DIRECTORY "${example_work}/${test_name}")
        add_test(NAME ${test_name}
            COMMAND ${CMAKE_COMMAND}
                -DEMBR=${EMBR_DRIVER_CMD} -DBACKEND=${backend}
                -DSCRIPT=${script} -DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/${ex}.out
                -P ${CMAKE_CURRENT_SOURCE_DIR}/run_example.cmake
            WORKING_DIRECTORY "${example_work}/${test_name}")
        set_tests_properties(${test_name} PROPERTIES ENVIRONMENT "EMBR_PATH=${BIN_DIR}" TIMEOUT 60)
    endforeach()
endforeach()

# a C program that embeds embr through the C interface (include/embr_c.h). it checks its own results and prints
# "embed host OK". it is linked as C++ because the static library holds embr's C++ code
add_executable(embed_host embed/host.c)
target_link_libraries(embed_host PRIVATE embr_c)
set_target_properties(embed_host PROPERTIES LINKER_LANGUAGE CXX RUNTIME_OUTPUT_DIRECTORY "${BIN_DIR}/tests")
add_dependencies(embed_host str_plugin)
file(MAKE_DIRECTORY "${BIN_DIR}/test-work/embed_host")
add_test(NAME embed_host COMMAND embed_host WORKING_DIRECTORY "${BIN_DIR}/test-work/embed_host")
set_tests_properties(embed_host PROPERTIES ENVIRONMENT "EMBR_PATH=${BIN_DIR}" PASS_REGULAR_EXPRESSION "embed host OK")
