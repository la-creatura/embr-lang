# amalgamate.cmake
#
# custom command that regenerates OUTPUT whenever ENTRY or any file under SOURCE_DIR changes
# plus an INTERFACE target <name> that consumers can link against to get a build-order dependency on the generation step and PUBLIC_INCLUDE_DIR on their include path.
#
# PUBLIC_INCLUDE_DIR for #include <embr/embr.h> rather than #include <embr.h>
# put OUTPUT at .../generated/embr/embr.h and pass PUBLIC_INCLUDE_DIR .../generated so the embr/ path component in the #include still resolves
# defaults to OUTPUT's dir for #include <embr.h>
#
# usage:
#   embr_amalgamate(embr_single_header
#       ENTRY              ${CMAKE_SOURCE_DIR}/include/embr/core/root.h
#       OUTPUT             ${CMAKE_BINARY_DIR}/generated/embr/embr.h
#       SOURCE_DIR         ${CMAKE_SOURCE_DIR}/include/embr
#       INCLUDE_DIRS       ${CMAKE_SOURCE_DIR}/include
#       PUBLIC_INCLUDE_DIR ${CMAKE_BINARY_DIR}/generated
#       BANNER             "embr - single header build"
#   )
#   target_link_libraries(embr PRIVATE embr_single_header)

function(embr_amalgamate name)
    set(options "")
    set(oneValueArgs ENTRY OUTPUT SOURCE_DIR BANNER PUBLIC_INCLUDE_DIR)
    set(multiValueArgs INCLUDE_DIRS DEFINES)
    cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT ARG_ENTRY OR NOT ARG_OUTPUT OR NOT ARG_SOURCE_DIR)
        message(FATAL_ERROR "embr_amalgamate(${name}): ENTRY, OUTPUT and SOURCE_DIR are required")
    endif()

    if(NOT ARG_INCLUDE_DIRS)
        set(ARG_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/include")
    endif()

    find_package(Python3 COMPONENTS Interpreter REQUIRED)

    # the script only inlines files reachable from ENTRY, but tracking that exact set at configure time means re-running CMake on every header add/remove
    # globbing every header under SOURCE_DIR is an over-approximation
    # it costs a few unnecessary reruns of the (cheap, pure-python) script on unrelated header edits, in exchange for never silently going stale
    file(GLOB_RECURSE ARG_DEPS CONFIGURE_DEPENDS
        "${ARG_SOURCE_DIR}/*.h" "${ARG_SOURCE_DIR}/*.hpp" "${ARG_SOURCE_DIR}/*.inl")

    # headers that sit directly in an include dir (include/embr_plugin.h) are pulled in by the script too
    foreach(dir ${ARG_INCLUDE_DIRS})
        file(GLOB top_headers CONFIGURE_DEPENDS "${dir}/*.h")
        list(APPEND ARG_DEPS ${top_headers})
    endforeach()

    set(SCRIPT "${CMAKE_SOURCE_DIR}/tools/amalgamate.py")

    set(CMD ${Python3_EXECUTABLE} "${SCRIPT}" "${ARG_ENTRY}" -o "${ARG_OUTPUT}")
    foreach(dir ${ARG_INCLUDE_DIRS})
        list(APPEND CMD -I "${dir}")
    endforeach()
    foreach(def ${ARG_DEFINES})
        list(APPEND CMD -D "${def}")
    endforeach()
    if(ARG_BANNER)
        list(APPEND CMD --banner "${ARG_BANNER}")
    endif()

    add_custom_command(
        OUTPUT  "${ARG_OUTPUT}"
        COMMAND ${CMD}
        DEPENDS "${SCRIPT}" "${ARG_ENTRY}" ${ARG_DEPS}
        COMMENT "amalgamating ${name} -> ${ARG_OUTPUT}"
        VERBATIM
    )

    add_custom_target(${name}_generate DEPENDS "${ARG_OUTPUT}")

    add_library(${name} INTERFACE)
    add_dependencies(${name} ${name}_generate)

    if(ARG_PUBLIC_INCLUDE_DIR)
        set(PUB_DIR "${ARG_PUBLIC_INCLUDE_DIR}")
    else()
        get_filename_component(PUB_DIR "${ARG_OUTPUT}" DIRECTORY)
    endif()
    target_include_directories(${name} INTERFACE "${PUB_DIR}")
endfunction()
