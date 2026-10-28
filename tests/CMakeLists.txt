# each test suite is a small executable from a single .cpp and harness.h
# editing one plugin's tests only recompiles and relinks that one binary
# and so CMake/CTest can build and run them in parallel

# if another preset overwrote the shared bin/, this says so once, clearly, before other tests fail
# in a confusing way (see "the shared bin/ folder" in README.md)
add_test(NAME bin_dir_stamp COMMAND ${CMAKE_COMMAND}
    -DSTAMP=${BIN_DIR}/.built-by -DEXPECTED=${EMBR_BUILD_NAME} -P ${CMAKE_CURRENT_SOURCE_DIR}/check_bin_stamp.cmake)

# the VM, compiled once for the suites that only run scripts (see harness_vm.cpp)
add_library(embr_test_support STATIC harness_vm.cpp)
target_link_libraries(embr_test_support PUBLIC embr_single_header)
embr_use_pch(embr_test_support exe)

function(add_embr_test name)
    add_executable(test_${name} ${name}_test.cpp)
    target_link_libraries(test_${name} PRIVATE embr_single_header embr_test_support)
    embr_use_pch(test_${name} exe)
    set_target_properties(test_${name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${BIN_DIR}/tests"
    )
    # every registered test gets its OWN working directory, one per backend. suites write fixture files (modules,
    # manifests, scratch dirs) into the cwd, and the registrations of one suite run concurrently under `ctest -j`:
    # a shared cwd let one process truncate a fixture another was loading (async flaked that way). the test
    # binaries live in bin/<platform>/tests/, so plugins/ and modules/ (one level up) are reached via EMBR_PATH.
    # harness.h's runSourceAny() picks the backend from EMBR_TEST_BACKEND
    set(work_root "${BIN_DIR}/test-work")
    foreach(backend ${EMBR_BACKENDS})
        file(MAKE_DIRECTORY "${work_root}/${name}_${backend}")
        add_test(NAME ${name}_${backend} COMMAND test_${name} WORKING_DIRECTORY "${work_root}/${name}_${backend}")
        set_tests_properties(${name}_${backend} PROPERTIES ENVIRONMENT "EMBR_PATH=${BIN_DIR};EMBR_TEST_BACKEND=${backend}")
    endforeach()
endfunction()

add_embr_test(core)  # language + embrlib (the "batteries" stdlib)
add_embr_test(str)
add_embr_test(unicode)
add_embr_test(json)
add_embr_test(embrtypes)
add_embr_test(ffi)
add_embr_test(embrmath)
add_embr_test(fs)
add_embr_test(path)
add_embr_test(os)
add_embr_test(random)
add_embr_test(encoding)
add_embr_test(csv)
add_embr_test(module_path)
add_embr_test(time)
add_embr_test(regex)
add_embr_test(embrmeta)
add_embr_test(ffi_embr)
add_embr_test(table)
add_embr_test(vec)
add_embr_test(parallel)
add_embr_test(parallel_embr)
add_embr_test(async)
add_embr_test(dunder)
add_embr_test(http)
add_embr_test(pkg_embr)
add_embr_test(gc)
add_embr_test(coroutine)
add_embr_test(vm_internals)
add_embr_test(bytecode)
add_embr_test(ed25519)   # core/ed25519.h against known signatures and digests
add_embr_test(typecheck)

# a regression here (VmRunner::inject() mis-splicing on a second call, see
# vm_internals_test.cpp's own header) manifests as an infinite loop, not a
# clean failure -- a short TIMEOUT turns that into a fast, legible CTest
# failure instead of hanging the whole suite/CI.
foreach(backend ${EMBR_BACKENDS})
    set_tests_properties(vm_internals_${backend} PROPERTIES TIMEOUT 10)
endforeach()

add_dependencies(test_core       embrlib_plugin)
add_dependencies(test_bytecode   embrlib_plugin)
add_dependencies(test_str        str_plugin embrlib_plugin)
add_dependencies(test_unicode    unicode_plugin)
add_dependencies(test_json       json_plugin embrlib_plugin)
add_dependencies(test_embrtypes  embrtypes_plugin ffi_plugin)
add_dependencies(test_ffi        ffi_plugin embrtypes_plugin embrlib_plugin)

# a small C library the ffi tests call, for callbacks libc can't test (see ffi_fixture.cpp). the test opens it by a
# path relative to its working folder, bin/<platform>/test-work/ffi_<backend>
add_library(ffi_fixture SHARED ffi_fixture.cpp)
set_target_properties(ffi_fixture PROPERTIES PREFIX "" LIBRARY_OUTPUT_DIRECTORY "${BIN_DIR}/tests" RUNTIME_OUTPUT_DIRECTORY "${BIN_DIR}/tests")
add_dependencies(test_ffi ffi_fixture)
target_compile_definitions(test_ffi PRIVATE EMBR_TEST_FIXTURE="../../tests/$<TARGET_FILE_NAME:ffi_fixture>")
add_dependencies(test_embrmath   embrmath_plugin embrlib_plugin json_plugin)
add_dependencies(test_fs         fs_plugin embrlib_plugin)
add_dependencies(test_path       path_plugin)
add_dependencies(test_os         os_plugin embrlib_plugin)
add_dependencies(test_random     random_plugin embrlib_plugin)
add_dependencies(test_encoding   encoding_plugin embrlib_plugin)
add_dependencies(test_csv        csv_plugin embrlib_plugin)
add_dependencies(test_module_path os_plugin fs_plugin path_plugin embrlib_plugin)
add_dependencies(test_time       embrlib_plugin str_plugin time_plugin)
add_dependencies(test_regex      str_plugin regex_plugin)
add_dependencies(test_embrmeta   embrmeta_plugin)
add_dependencies(test_ffi_embr   ffi_module embrmeta_plugin embrtypes_plugin ffi_plugin regex_plugin str_plugin embrlib_plugin)
add_dependencies(test_table      table_plugin embrlib_plugin)
add_dependencies(test_vec        vec_plugin embrlib_plugin table_plugin json_plugin csv_plugin random_plugin)
add_dependencies(test_parallel   parallel_plugin embrlib_plugin table_plugin)
add_dependencies(test_parallel_embr parallel_module parallel_plugin embrlib_plugin)
add_dependencies(test_async      async_plugin embrlib_plugin table_plugin time_plugin)
add_dependencies(test_dunder     embrtypes_plugin json_plugin table_plugin embrlib_plugin ffi_plugin str_plugin)
add_dependencies(test_http       http_plugin)
add_dependencies(test_pkg_embr   pkg_module http_plugin json_plugin fs_plugin str_plugin embrlib_plugin embrmeta_plugin)
add_dependencies(test_gc         gc_plugin table_plugin vec_plugin ffi_plugin embrtypes_plugin embrlib_plugin)
add_dependencies(test_coroutine  coroutine_plugin)

# end-to-end https:// test against a local TLS server (needs python3 + the openssl CLI; skips itself
# (exit 77) when either is missing or the http plugin was built without OpenSSL). shell-driven because
# the C++ harness can't host a server. registered per backend like every other suite.
find_program(BASH_EXE bash)
if(BASH_EXE AND NOT WIN32)
    set(https_work "${BIN_DIR}/test-work")
    foreach(backend ${EMBR_BACKENDS})
        file(MAKE_DIRECTORY "${https_work}/https_${backend}")
        add_test(NAME https_${backend} COMMAND ${BASH_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/https_smoke.sh $<TARGET_FILE:embr> --backend=${backend}
                 WORKING_DIRECTORY "${https_work}/https_${backend}")
        set_tests_properties(https_${backend} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
    endforeach()
    add_dependencies(embr http_plugin)

    # the response size limit, against a local plain-http server (skips without python3)
    foreach(backend ${EMBR_BACKENDS})
        file(MAKE_DIRECTORY "${https_work}/http_limit_${backend}")
        add_test(NAME http_limit_${backend} COMMAND ${BASH_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/http_limit_smoke.sh $<TARGET_FILE:embr> --backend=${backend}
                 WORKING_DIRECTORY "${https_work}/http_limit_${backend}")
        set_tests_properties(http_limit_${backend} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
    endforeach()
endif()

# the docs live in the GitHub wiki (the wiki/ submodule). the docs checks below read it through EMBR_WIKI_DIR, see the
# top-level CMakeLists. without a wiki they are skipped

# editor grammar (tools/editor/vscode): regexes compile, keyword list matches the wiki's lexical-grammar page, sample
# snippets and every repo .embr file lex cleanly under the grammar's rules
find_program(PYTHON3_EXE python3)
if(PYTHON3_EXE)
    add_test(NAME editor_grammar COMMAND ${PYTHON3_EXE} ${CMAKE_SOURCE_DIR}/tools/editor/check_grammar.py)
    if(EMBR_WIKI_DIR)
        set_tests_properties(editor_grammar PROPERTIES ENVIRONMENT "EMBR_WIKI_DIR=${EMBR_WIKI_DIR}")
    endif()
endif()

if(PYTHON3_EXE)
    add_test(NAME lsp_smoke COMMAND ${PYTHON3_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/lsp_smoke.py ${EMBR_DRIVER_CMD})
endif()
# types into the REPL through a pseudo-terminal, so its line editor runs (posix only: python's pty module)
if(PYTHON3_EXE AND NOT WIN32)
    add_test(NAME repl_smoke COMMAND ${PYTHON3_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/repl_smoke.py $<TARGET_FILE:embr>)
    set_tests_properties(repl_smoke PROPERTIES TIMEOUT 60)
endif()
# runs the tagged code blocks in the wiki pages on every backend, checks the numbers the wiki states, and checks
# that every plugin and module has a wiki page (skipped, exit 77, without EMBR_WIKI_DIR)
if(PYTHON3_EXE)
    add_test(NAME docs_examples COMMAND ${PYTHON3_EXE} ${CMAKE_SOURCE_DIR}/tools/docs/check_wiki.py
             --embr ${EMBR_DRIVER_CMD} --include ${CMAKE_BINARY_DIR}/generated --wiki "${EMBR_WIKI_DIR}")
    set_tests_properties(docs_examples PROPERTIES SKIP_RETURN_CODE 77)
endif()

# differential test: the regex engine (plugins/regex/rx.c) vs std::regex on random patterns.
# standalone (no embr header / plugins needed), so it is a plain executable
add_executable(test_regex_engine regex_engine_test.cpp ${CMAKE_SOURCE_DIR}/plugins/regex/rx.c)
set_target_properties(test_regex_engine PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${BIN_DIR}/tests")
add_test(NAME regex_engine_diff COMMAND test_regex_engine)
set_tests_properties(regex_engine_diff PROPERTIES SKIP_RETURN_CODE 77)

# the real `embr` binary on malformed / edge-case scripts: must report an error, never crash (see cli_smoke.sh)
if(BASH_EXE AND NOT WIN32)
    foreach(backend ${EMBR_BACKENDS})
        add_test(NAME cli_smoke_${backend} COMMAND ${BASH_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_smoke.sh $<TARGET_FILE:embr> --backend=${backend})
    endforeach()
endif()

# aembr: programs built by it match the VM, refusals, runtime errors, same input same output (see aembr_smoke.sh)
if(BASH_EXE AND NOT WIN32 AND TARGET aembr)
    add_test(NAME aembr_smoke COMMAND ${BASH_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/aembr_smoke.sh $<TARGET_FILE:aembr> $<TARGET_FILE:embr> ${CMAKE_CURRENT_SOURCE_DIR}/aembr)
    add_test(NAME aembr_package_smoke COMMAND ${BASH_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/aembr_package_smoke.sh $<TARGET_FILE:aembr> $<TARGET_FILE:embr> ${CMAKE_CURRENT_SOURCE_DIR}/aembr)
    add_test(NAME aembr_plugin_smoke COMMAND ${BASH_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/aembr_plugin_smoke.sh $<TARGET_FILE:aembr> $<TARGET_FILE:embr> ${CMAKE_CURRENT_SOURCE_DIR}/aembr)
    add_test(NAME aembr_ports_smoke COMMAND ${BASH_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/aembr_ports_smoke.sh $<TARGET_FILE:aembr> $<TARGET_FILE:embr> ${CMAKE_SOURCE_DIR}/tools/aembr/ports)
    add_test(NAME aembr_stats_smoke COMMAND ${BASH_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/aembr_stats_smoke.sh $<TARGET_FILE:aembr> ${CMAKE_SOURCE_DIR})
    # it builds and runs a few hundred C programs: well over the 120 s ci gives a test by default
    set_tests_properties(aembr_smoke PROPERTIES TIMEOUT 600)
endif()

# the .vmbr cache of compiled scripts: written only with --cache-dir, reused while unchanged, damage is a miss (see bytecode_smoke.sh)
if(BASH_EXE AND NOT WIN32)
    add_test(NAME bytecode_smoke COMMAND ${BASH_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/bytecode_smoke.sh $<TARGET_FILE:embr> ${CMAKE_SOURCE_DIR}/examples)
endif()

# `cmake --install` into a temp prefix, then run scripts from there: plugins and modules must be found (see install_smoke.sh)
if(BASH_EXE AND NOT WIN32)
    add_test(NAME install_smoke COMMAND ${BASH_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/install_smoke.sh ${CMAKE_BINARY_DIR} ${CMAKE_PROJECT_VERSION})
endif()

# install.sh and uninstall.sh in a fake home folder, from a tarball made out of `cmake --install` (see installer_smoke.sh)
if(BASH_EXE AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_test(NAME installer_smoke COMMAND ${BASH_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/installer_smoke.sh ${CMAKE_BINARY_DIR} ${CMAKE_PROJECT_VERSION} ${CMAKE_SOURCE_DIR})
endif()

# `embr fmt`: golden file, idempotence over every repo .embr file, --check (see fmt_smoke.sh)
if(BASH_EXE AND NOT WIN32)
    add_test(NAME fmt_smoke COMMAND ${BASH_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/fmt_smoke.sh $<TARGET_FILE:embr> ${CMAKE_SOURCE_DIR})
endif()

# small C plugins the sandbox smoke test loads: one built for another ABI, one that asks for the filesystem
enable_language(C)
foreach(cp cp_badabi cp_needs_fs)
    add_library(${cp}_plugin SHARED cplugins/${cp}.c)
    target_include_directories(${cp}_plugin PRIVATE ${CMAKE_SOURCE_DIR}/include)
    set_target_properties(${cp}_plugin PROPERTIES OUTPUT_NAME ${cp} PREFIX "" C_VISIBILITY_PRESET hidden
        LIBRARY_OUTPUT_DIRECTORY "${BIN_DIR}/plugins" RUNTIME_OUTPUT_DIRECTORY "${BIN_DIR}/plugins")
endforeach()

# C++ plugins for the sandbox and plugin-info smoke tests: one that does not describe itself, one with a wrong C++ ABI, one with the right
# ABI from another toolchain (loaded with a warning), one that
# needs the network (refused before its embr_register runs)
foreach(cp cp_undeclared cp_badbuild cp_sameabi cp_caps)
    add_library(${cp}_plugin SHARED cplugins/${cp}.cpp)
    target_link_libraries(${cp}_plugin PRIVATE embr_single_header)
    embr_use_pch(${cp}_plugin pic)
    set_target_properties(${cp}_plugin PROPERTIES OUTPUT_NAME ${cp} PREFIX ""
        LIBRARY_OUTPUT_DIRECTORY "${BIN_DIR}/plugins" RUNTIME_OUTPUT_DIRECTORY "${BIN_DIR}/plugins")
endforeach()

# sandbox / plugin allow-list, including escape attempts (see sandbox_smoke.sh)
if(BASH_EXE AND NOT WIN32)
    foreach(backend ${EMBR_BACKENDS})
        add_test(NAME sandbox_smoke_${backend} COMMAND ${BASH_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/sandbox_smoke.sh $<TARGET_FILE:embr> --backend=${backend})
    endforeach()
endif()

# `embr plugins` / `embr plugin NAME`, their JSON, and the doc line in the language server's hover (see plugin_info_smoke.sh)
if(BASH_EXE AND NOT WIN32)
    add_test(NAME plugin_info_smoke COMMAND ${BASH_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/plugin_info_smoke.sh $<TARGET_FILE:embr>)
endif()

# `embr module` / `embr modules` / `embr check` on a module's declaration, and the editor hover on a module function
# (see module_info_smoke.sh)
if(BASH_EXE AND NOT WIN32)
    add_test(NAME module_info_smoke COMMAND ${BASH_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/module_info_smoke.sh $<TARGET_FILE:embr>)
endif()

# `embr pack` / `embr package` and `import` of a .pmbr package: plugin, module, module with a bundled library, and what is refused
# (see package_smoke.sh)
if(BASH_EXE AND NOT WIN32)
    add_test(NAME package_smoke COMMAND ${BASH_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/package_smoke.sh $<TARGET_FILE:embr>)
endif()

# the package registry index: its schema (tools/pkg/registry.schema.json), the checker, and the example index
if(PYTHON3_EXE)
    add_test(NAME pkg_registry_schema COMMAND ${PYTHON3_EXE} ${CMAKE_SOURCE_DIR}/tools/pkg/check_registry.py --selftest)
    add_test(NAME pkg_registry_example COMMAND ${PYTHON3_EXE} ${CMAKE_SOURCE_DIR}/tools/pkg/check_registry.py ${CMAKE_SOURCE_DIR}/tools/pkg/example-index.json)
endif()

# the version of every plugin and shipped module against tools/plugins/api.lock.json (the version rule is in api_lock.py)
if(PYTHON3_EXE)
    add_test(NAME plugin_api_lock COMMAND ${PYTHON3_EXE} ${CMAKE_SOURCE_DIR}/tools/plugins/api_lock.py check ${EMBR_DRIVER_CMD})
endif()

# script tests: real embr test files (tests/script_tests/test_*.embr) run by `embr test`, dogfooding the runner
# on each backend
foreach(backend ${EMBR_BACKENDS})
    file(MAKE_DIRECTORY "${BIN_DIR}/test-work/script_tests_${backend}")
    add_test(NAME script_tests_${backend} COMMAND embr --backend=${backend} test ${CMAKE_CURRENT_SOURCE_DIR}/script_tests
             WORKING_DIRECTORY "${BIN_DIR}/test-work/script_tests_${backend}")
    set_tests_properties(script_tests_${backend} PROPERTIES ENVIRONMENT "EMBR_PATH=${BIN_DIR}")
endforeach()
