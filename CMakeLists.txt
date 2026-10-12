cmake_minimum_required(VERSION 3.20)

project(embr VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# the docs live in the GitHub wiki, which is the git submodule in wiki/ (git submodule update --init). CTest uses it
# to check the docs. to use another clone instead:  cmake --preset linux-debug -DEMBR_WIKI_DIR=../some-clone
# (the env var of the same name works too). if neither exists the wiki checks are skipped
if(NOT EMBR_WIKI_DIR AND DEFINED ENV{EMBR_WIKI_DIR})
    set(EMBR_WIKI_DIR "$ENV{EMBR_WIKI_DIR}")
endif()
if(NOT EMBR_WIKI_DIR AND EXISTS "${CMAKE_SOURCE_DIR}/wiki/home.md")
    set(EMBR_WIKI_DIR "${CMAKE_SOURCE_DIR}/wiki")
endif()
set(EMBR_WIKI_DIR "${EMBR_WIKI_DIR}" CACHE PATH "the wiki clone the docs checks in CTest read (default: the wiki/ submodule)")
if(EMBR_WIKI_DIR AND NOT IS_DIRECTORY "${EMBR_WIKI_DIR}")
    message(WARNING "EMBR_WIKI_DIR=${EMBR_WIKI_DIR} is not a folder, the wiki docs checks will be skipped")
    set(EMBR_WIKI_DIR "")
endif()

# unit tests run through CTest
include(CTest)

# the execution backends compiled into embr.h, default one first. a backend called "x" turns on EMBR_WITH_X, lives in
# include/embr/backends/x.h, and gets a case at the "backend switch" comments in core/invoke.h, src/cli.cpp and
# tests/harness.h. every test suite runs once per backend listed here (see tests/CMakeLists.txt)
set(EMBR_BACKENDS vm)

set(EMBR_BACKEND_DEFINES "")
foreach(backend ${EMBR_BACKENDS})
    string(TOUPPER "${backend}" backend_upper)
    list(APPEND EMBR_BACKEND_DEFINES EMBR_WITH_${backend_upper})
endforeach()
list(JOIN EMBR_BACKENDS " + " EMBR_BACKEND_NAMES_STR)

# embr.h is generated at build time
# every target below depends on the regenerated header and rebuilds it automatically
include(cmake/amalgamate.cmake)

embr_amalgamate(embr_single_header
    ENTRY              ${CMAKE_SOURCE_DIR}/include/embr/core/root.h
    OUTPUT             ${CMAKE_BINARY_DIR}/generated/embr/embr.h
    SOURCE_DIR         ${CMAKE_SOURCE_DIR}/include/embr
    INCLUDE_DIRS       ${CMAKE_SOURCE_DIR}/include
    PUBLIC_INCLUDE_DIR ${CMAKE_BINARY_DIR}/generated
    DEFINES            ${EMBR_BACKEND_DEFINES}
    BANNER             "embr - single header build (backends: ${EMBR_BACKEND_NAMES_STR})"
)
# PUBLIC_INCLUDE_DIR is generated/ (not generated/embr/) so that the existing #include <embr/embr.h> keeps resolving

# a precompiled copy of the single header, so most source files don't parse it again. that parse was about two thirds of what a small test or
# plugin cost to compile. one copy for programs and one for plugins (they differ in -fPIC). a target opts in with embr_use_pch(). turn it
# off with -DEMBR_PCH=OFF if a compiler or a sanitizer setup gets in the way
option(EMBR_PCH "reuse a precompiled embr.h in every target that can" ON)
if(EMBR_PCH AND NOT CMAKE_VERSION VERSION_LESS 3.16)
    set(EMBR_PCH_STUB ${CMAKE_BINARY_DIR}/pch/stub.cpp)
    file(WRITE ${EMBR_PCH_STUB} "// compiled only to hold the precompiled copy of embr.h (see CMakeLists.txt)\n")
    foreach(kind exe pic)
        add_library(embr_pch_${kind} OBJECT ${EMBR_PCH_STUB})
        target_link_libraries(embr_pch_${kind} PRIVATE embr_single_header)
        target_precompile_headers(embr_pch_${kind} PRIVATE <embr/embr.h>)
    endforeach()
    set_target_properties(embr_pch_pic PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()
function(embr_use_pch target kind)   # kind: exe or pic
    if(TARGET embr_pch_${kind})
        target_precompile_headers(${target} REUSE_FROM embr_pch_${kind})
    endif()
endfunction()

add_executable(embr src/cli.cpp)
target_link_libraries(embr PRIVATE embr_single_header)

target_compile_options(embr PRIVATE
    $<$<CONFIG:Release>:-O3>
    $<$<CONFIG:Debug>:-g>
)
# the hot loop is one huge switch. starting every case on a 16 byte line makes loops 10 to 20% faster and keeps a change
# somewhere else in the loop from moving the others (see tools/bench/README.md, code layout)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(embr PRIVATE $<$<CONFIG:Release>:-falign-labels=16>)
endif()

if(WIN32)
    set(BIN_DIR ${CMAKE_SOURCE_DIR}/bin/windows)
    foreach(dll
        libstdc++-6.dll
        libgcc_s_seh-1.dll
        libwinpthread-1.dll
    )
        execute_process(
            COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=${dll}
            OUTPUT_VARIABLE DLL_PATH
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )

        # next to embr.exe and the test programs, which can't start without them, and into an install (and so the zip)
        add_custom_command(TARGET embr POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${DLL_PATH}"
                "$<TARGET_FILE_DIR:embr>"
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:embr>/tests"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${DLL_PATH}"
                "$<TARGET_FILE_DIR:embr>/tests"
        )
        install(FILES "${DLL_PATH}" DESTINATION bin)
    endforeach()
elseif(APPLE)
    set(BIN_DIR ${CMAKE_SOURCE_DIR}/bin/macos)
else()
    set(BIN_DIR ${CMAKE_SOURCE_DIR}/bin/linux)
endif()

set_target_properties(embr PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${BIN_DIR}")

# the icon of embr.exe, a resource file the windows linker puts into the program
if(WIN32)
    enable_language(RC)
    configure_file(cmake/embr.rc.in "${CMAKE_BINARY_DIR}/embr.rc" @ONLY)
    target_sources(embr PRIVATE "${CMAKE_BINARY_DIR}/embr.rc")
    set_source_files_properties("${CMAKE_BINARY_DIR}/embr.rc" PROPERTIES OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/assets/embr.ico")
endif()

# which build directory produced this binary
get_filename_component(EMBR_BUILD_NAME "${CMAKE_BINARY_DIR}" NAME)
target_compile_definitions(embr PRIVATE EMBR_BUILD_NAME="${EMBR_BUILD_NAME}" EMBR_VERSION="${PROJECT_VERSION}")

# a stamp next to the binary saying which build directory wrote it last
add_custom_command(TARGET embr POST_BUILD
    COMMAND ${CMAKE_COMMAND} -DNAME=${EMBR_BUILD_NAME} -DOUT=${BIN_DIR}/.built-by -P ${CMAKE_SOURCE_DIR}/cmake/write_stamp.cmake
    VERBATIM
)

# the plain C interface (include/embr_c.h) for programs in C or other languages: libembr_c.a and a shared libembr_c_shared,
# both in bin/<platform>/lib. the static one needs a C++ linker (or -lstdc++), it holds embr's C++ code
add_library(embr_c STATIC src/embr_c.cpp)
add_library(embr_c_shared SHARED src/embr_c.cpp)
foreach(t embr_c embr_c_shared)
    target_link_libraries(${t} PRIVATE embr_single_header)
    target_include_directories(${t} INTERFACE ${CMAKE_SOURCE_DIR}/include)
    set_target_properties(${t} PROPERTIES POSITION_INDEPENDENT_CODE ON
        ARCHIVE_OUTPUT_DIRECTORY "${BIN_DIR}/lib" LIBRARY_OUTPUT_DIRECTORY "${BIN_DIR}/lib" RUNTIME_OUTPUT_DIRECTORY "${BIN_DIR}/lib")
endforeach()
target_compile_definitions(embr_c_shared PUBLIC EMBR_C_SHARED)
install(TARGETS embr_c embr_c_shared ARCHIVE DESTINATION lib LIBRARY DESTINATION lib RUNTIME DESTINATION bin)
install(FILES include/embr_c.h include/embr_plugin.h DESTINATION include)

# how a test driver that runs on the build machine (a cmake -P script, python) starts embr. normally the binary itself.
# a cross build with an emulator (the windows build with -DCMAKE_CROSSCOMPILING_EMULATOR=wine) gets a small script that
# runs it under the emulator, since the build machine can't start an .exe directly
if(CMAKE_CROSSCOMPILING_EMULATOR)
    string(JOIN " " embr_emulator ${CMAKE_CROSSCOMPILING_EMULATOR})
    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/embr-emulated"
         CONTENT "#!/bin/sh\nexec ${embr_emulator} \"$<TARGET_FILE:embr>\" \"$@\"\n"
         FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
    set(EMBR_DRIVER_CMD "${CMAKE_BINARY_DIR}/embr-emulated")
else()
    set(EMBR_DRIVER_CMD "$<TARGET_FILE:embr>")
endif()

add_subdirectory(plugins)
add_subdirectory(modules)
add_subdirectory(examples)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

# cmake --install <build dir> --prefix <dir>
# gives prefix/bin/embr, prefix/lib/embr/{plugins,modules}, prefix/include/embr/embr.h and prefix/share/doc/embr
# the plugins and modules are installed by their own CMakeLists
# the embr binary finds prefix/lib/embr by itself
install(TARGETS embr RUNTIME DESTINATION bin)
install(FILES "${CMAKE_BINARY_DIR}/generated/embr/embr.h" DESTINATION include/embr)
install(FILES README.md LICENSE DESTINATION share/doc/embr)

# what install.sh needs for the menu entry, the file types and the icons. it lives under share/embr so a plain
# cmake --install does not register anything by itself. each file type takes assets/embr-<kind>.svg when that file
# exists (run cmake again after adding one) and the app icon until then
if(UNIX AND NOT APPLE)
    install(PROGRAMS uninstall.sh packaging/linux/open.sh DESTINATION share/embr)
    install(FILES packaging/linux/embr.desktop packaging/linux/embr-mime.xml DESTINATION share/embr/integration)
    set(icon_dir share/embr/integration/icons/hicolor)
    foreach(size 16 32 64 128 256 512)
        install(FILES assets/embr-${size}.png DESTINATION ${icon_dir}/${size}x${size}/apps RENAME embr.png)
    endforeach()
    install(FILES assets/embr.svg DESTINATION ${icon_dir}/scalable/apps)
    foreach(type embr:text-x-embr vmbr:application-x-embr-bytecode pmbr:application-x-embr-package)
        string(REPLACE ":" ";" type "${type}")
        list(GET type 0 kind)
        list(GET type 1 icon_name)
        set(icon_file assets/embr.svg)
        if(EXISTS "${CMAKE_SOURCE_DIR}/assets/embr-${kind}.svg")
            set(icon_file assets/embr-${kind}.svg)
        endif()
        install(FILES ${icon_file} DESTINATION ${icon_dir}/scalable/mimetypes RENAME ${icon_name}.svg)
    endforeach()
endif()

# cmake --build <build dir> --target package     makes embr-<version>-<os>-<arch>.tar.gz
string(TOLOWER "${CMAKE_SYSTEM_NAME}" EMBR_PACKAGE_OS)
set(CPACK_PACKAGE_NAME embr)
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
# a toolchain file that sets CMAKE_SYSTEM_NAME leaves CMAKE_SYSTEM_PROCESSOR empty, so fall back to the host's
set(EMBR_PACKAGE_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
if(NOT EMBR_PACKAGE_ARCH)
    set(EMBR_PACKAGE_ARCH "${CMAKE_HOST_SYSTEM_PROCESSOR}")
endif()
string(TOLOWER "${EMBR_PACKAGE_ARCH}" EMBR_PACKAGE_ARCH)
set(CPACK_PACKAGE_FILE_NAME "embr-${PROJECT_VERSION}-${EMBR_PACKAGE_OS}-${EMBR_PACKAGE_ARCH}")
set(CPACK_GENERATOR TGZ)
if(WIN32)
    set(CPACK_GENERATOR ZIP)
endif()
include(CPack)
