// plugins.cpp
// reads the table of a C plugin. see plugins.h.

#include "plugins.h"

#include <embr/embr.h>

#include <dlfcn.h>

namespace aembr {

bool readPlugin(const std::string& name, const std::string& scriptDir, PluginDesc& out, std::string& err) {
    namespace fs = std::filesystem;
    const auto cands = embr::fileCandidates(name, scriptDir, embr::PLUGIN_EXT, "plugins");
    const fs::path found = embr::findExistingCandidate(cands);
    if (found.empty()) {
        err = "cannot find the plugin \"" + name + "\", tried:" + embr::describeCandidates(cands);
        return false;
    }
    std::error_code ec;
    const fs::path abs = fs::weakly_canonical(found, ec);
    out.name = name;
    out.path = (ec ? found : abs).string();

    void* lib = dlopen(out.path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!lib) { err = "cannot open the plugin \"" + name + "\": " + std::string(dlerror()); return false; }
    auto closeLib = [&] { dlclose(lib); };
    if (dlsym(lib, "embr_register")) {
        err = "the plugin \"" + name + "\" is a C++ plugin: it needs the interpreter, so compiled code cannot call it (a C plugin can be called)";
        closeLib();
        return false;
    }
    EmbrPluginInfoFn fn;
    *reinterpret_cast<void**>(&fn) = dlsym(lib, "embr_plugin_info");
    const EmbrPluginInfo* info = fn ? fn() : nullptr;
    if (!info) { err = "\"" + out.path + "\" is not an embr plugin (it has no embr_plugin_info)"; closeLib(); return false; }
    if (info->abi_version != EMBR_PLUGIN_ABI_VERSION) {
        err = "the plugin \"" + name + "\" was built for plugin interface " + std::to_string(info->abi_version) + ", aembr uses " +
              std::to_string(EMBR_PLUGIN_ABI_VERSION) + ": rebuild the plugin";
        closeLib();
        return false;
    }
    out.caps = info->capabilities;
    for (uint32_t i = 0; i < info->nfuncs; ++i) {
        const EmbrFunc& f = info->funcs[i];
        if (!f.fn) continue;
        PluginFnDesc d;
        d.name = f.name;
        for (uint32_t p = 0; p < f.nparams; ++p) d.params.push_back({f.params[p].name, f.params[p].types, f.params[p].flags});
        d.resultTypes = f.result_types;
        d.resultElem = f.result_elem;
        out.funcs.push_back(std::move(d));
    }
    closeLib();
    return true;
}

}  // namespace aembr
