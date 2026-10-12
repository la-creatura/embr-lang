# .embr "script plugins" shipped with the project -- interpreted modules,
# not compiled .so's, but otherwise meant to be import'able the same way a
# native plugin is (see modules/ffi.embr's own header for why it's built
# this way instead of as a plugins/*.cpp).
#
# copied (not compiled) into BIN_DIR/modules/, which is exactly one of the
# bare-name candidate paths core/module.h's fileCandidates() already checks
# from the process's cwd -- so `import "ffi.embr"` resolves here the same
# way `import "ffi"` resolves a native plugin under BIN_DIR/plugins/.
set(MODULE_DIR "${BIN_DIR}/modules")

function(add_embr_module name)
    set(src "${CMAKE_CURRENT_SOURCE_DIR}/${name}.embr")
    set(dst "${MODULE_DIR}/${name}.embr")
    add_custom_command(
        OUTPUT  "${dst}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${MODULE_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${src}" "${dst}"
        DEPENDS "${src}"
        COMMENT "copying embr module ${name}.embr -> ${MODULE_DIR}"
        VERBATIM
    )
    add_custom_target(${name}_module ALL DEPENDS "${dst}")
    install(FILES "${src}" DESTINATION lib/embr/modules)
endfunction()

add_embr_module(cheader)
add_embr_module(ffi)
add_embr_module(parallel)
add_embr_module(pkg)
add_embr_module(testing)
