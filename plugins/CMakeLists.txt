set(PLUGIN_DIR "${BIN_DIR}/plugins")

# a plugin written in C against include/embr_plugin.h: links nothing from embr, so it needs no generated header and no precompiled copy
enable_language(C)
function(add_c_plugin name source)
    set(target "${name}_plugin")
    add_library(${target} SHARED "${name}/${source}")
    foreach(extra IN LISTS ARGN)
        target_sources(${target} PRIVATE "${name}/${extra}")
    endforeach()
    target_include_directories(${target} PRIVATE ${CMAKE_SOURCE_DIR}/include)
    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME "${name}"
        PREFIX ""
        C_STANDARD 99
        C_VISIBILITY_PRESET hidden
        LIBRARY_OUTPUT_DIRECTORY "${PLUGIN_DIR}"
        RUNTIME_OUTPUT_DIRECTORY "${PLUGIN_DIR}"
    )
    install(TARGETS ${target} LIBRARY DESTINATION lib/embr/plugins RUNTIME DESTINATION lib/embr/plugins)
endfunction()

function(add_plugin name source)

    # cmake target names must be unique across the whole build
    set(target "${name}_plugin")

    add_library(${target} SHARED "${name}/${source}")

    # plugins consume the generated single header, so they rebuild automatically whenever a modular header changes
    target_link_libraries(${target} PRIVATE embr_single_header)
    embr_use_pch(${target} pic)

    # macOS refuses to link a shared library with unresolved symbols unless told they will be provided at load time
    if(APPLE)
        target_link_options(${target} PRIVATE -undefined dynamic_lookup)
    endif()

    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME "${name}"
        PREFIX ""
        LIBRARY_OUTPUT_DIRECTORY "${PLUGIN_DIR}"
        RUNTIME_OUTPUT_DIRECTORY "${PLUGIN_DIR}"
    )
    install(TARGETS ${target} LIBRARY DESTINATION lib/embr/plugins RUNTIME DESTINATION lib/embr/plugins)
endfunction()

add_plugin(embrlib embrlib.cpp)
add_plugin(str str.cpp)
add_c_plugin(unicode unicode.c)           # written in C against include/embr_plugin.h
add_c_plugin(json json.c)             # written in C against include/embr_plugin.h
add_plugin(embrtypes embrtypes.cpp)
add_plugin(ffi ffi.cpp)
add_plugin(embrmath embrmath.cpp)
add_c_plugin(fs fs.c)                   # written in C against include/embr_plugin.h
add_c_plugin(path path.c)               # written in C against include/embr_plugin.h
add_c_plugin(os os.c)                   # written in C against include/embr_plugin.h
add_c_plugin(random random.c)               # written in C against include/embr_plugin.h
add_c_plugin(encoding encoding.c)       # written in C against include/embr_plugin.h, see the wiki, writing-plugins
add_c_plugin(csv csv.c)               # written in C against include/embr_plugin.h
add_c_plugin(time time.c)                 # written in C against include/embr_plugin.h
add_c_plugin(regex regex.c rx.c)        # written in C against include/embr_plugin.h, the engine is rx.c
add_plugin(embrmeta embrmeta.cpp)
add_plugin(package package.cpp)
add_plugin(pmbr pmbr.cpp)
add_plugin(table table.cpp)
add_plugin(vec vec.cpp)
add_plugin(parallel parallel.cpp)
add_plugin(async async.cpp)
add_plugin(coroutine coroutine.cpp)
add_plugin(http http.cpp)
add_plugin(gc gc.cpp)

find_package(Threads REQUIRED)
target_link_libraries(parallel_plugin PRIVATE Threads::Threads)
target_link_libraries(async_plugin PRIVATE Threads::Threads)

# http plugin: the vendored single-header cpp-httplib (include/httplib/httplib.h, pinned at v0.58.0, MIT) does the actual HTTP/HTTPS client work,
# http.cpp is a thin embr-facing wrapper around it. TLS is only compiled in when OpenSSL is found
# without it the plugin still builds and serves plain http and https raises a runtime error instead of failing to configure or build
# same idea as the ffi plugin's mingw-vs-pkgconfig libffi split, keyed on OpenSSL instead of the platform
target_include_directories(http_plugin PRIVATE ${CMAKE_SOURCE_DIR}/include/httplib)
target_link_libraries(http_plugin PRIVATE Threads::Threads)

# homebrew's openssl@3 is keg-only: point find_package at it unless the user already did
if(APPLE AND NOT OPENSSL_ROOT_DIR)
    foreach(prefix /opt/homebrew/opt/openssl@3 /usr/local/opt/openssl@3)
        if(EXISTS "${prefix}")
            set(OPENSSL_ROOT_DIR "${prefix}")
            break()
        endif()
    endforeach()
endif()
# mingw has no system OpenSSL, so it links the static libs vendored in include/openssl
if(MINGW)
    set(EMBR_VENDORED_OPENSSL ON)
    set(OpenSSL_FOUND TRUE)
else()
    find_package(OpenSSL)
endif()
if(OpenSSL_FOUND)
    target_compile_definitions(http_plugin PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)
    if(EMBR_VENDORED_OPENSSL)
        target_include_directories(http_plugin SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/include/openssl/include)
        target_link_libraries(http_plugin PRIVATE
            ${CMAKE_SOURCE_DIR}/include/openssl/lib/libssl.a
            ${CMAKE_SOURCE_DIR}/include/openssl/lib/libcrypto.a)
    else()
        target_link_libraries(http_plugin PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    endif()
    if(APPLE)
        # cpp-httplib reads the root certificates from the macOS keychain by default.
        # on the CI runner that call overflows its stack inside the Security framework and kills the process (SIGBUS), 
        # so use OpenSSL's own CA paths here too, like every other platform does
        target_compile_definitions(http_plugin PRIVATE CPPHTTPLIB_DISABLE_MACOSX_AUTOMATIC_ROOT_CERTIFICATES)
    endif()
else()
    message(STATUS "OpenSSL not found -- http plugin will build without TLS support (https:// will raise a runtime error)")
endif()

if(WIN32)
    target_link_libraries(http_plugin PRIVATE ws2_32)
    if(OpenSSL_FOUND)
        target_link_libraries(http_plugin PRIVATE crypt32 gdi32 advapi32 user32)
        # the vendored httplib passes NULL for a HCRYPTPROV_LEGACY (an integer) when it opens the system cert store
        target_compile_options(http_plugin PRIVATE -Wno-conversion-null)
    endif()
endif()

# mingw needs packaged libffi because pkg-config pulls in glibc headers that don't exist in windows environment
if(MINGW)

    target_include_directories(ffi_plugin PRIVATE
        ${CMAKE_SOURCE_DIR}/include/libffi/include
    )

    target_link_libraries(ffi_plugin PRIVATE
        ${CMAKE_SOURCE_DIR}/include/libffi/lib/libffi.a
    )

# linux can use system libffi via pkg-config.
else()

    find_package(PkgConfig REQUIRED)

    # homebrew's libffi is keg-only (not on the default pkg-config path)
    if(APPLE)
        set(ENV{PKG_CONFIG_PATH}
            "$ENV{PKG_CONFIG_PATH}:/opt/homebrew/opt/libffi/lib/pkgconfig:/usr/local/opt/libffi/lib/pkgconfig")
    endif()
    pkg_check_modules(LIBFFI REQUIRED libffi)

    target_link_libraries(ffi_plugin PRIVATE ${LIBFFI_LIBRARIES})

    target_include_directories(ffi_plugin PRIVATE
        ${LIBFFI_INCLUDE_DIRS}
    )

endif()
