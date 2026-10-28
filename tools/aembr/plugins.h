// plugins.h
// reads what a C plugin says about itself (embr_plugin_info, include/embr_plugin.h), so that a call to one of its functions in compiled code can be
// typed and checked while the program is compiled.
//
// usage
//   aembr::PluginDesc d;
//   if (!aembr::readPlugin("encoding", "/dir/of/the/script", d, err)) ...     // d.funcs, d.caps, d.path
//
// good to know
//   - the library is opened to read its table, which runs its start-up code. it is a plugin the program imports, so it is one the
//     program would run anyway. a C++ plugin (it needs the interpreter) is refused

#ifndef AEMBR_PLUGINS_H
#define AEMBR_PLUGINS_H

#include <cstdint>
#include <string>
#include <vector>

namespace aembr {

struct PluginParam {
    std::string name;
    uint32_t    types = 0;     // EMBR_T_* bits
    uint32_t    flags = 0;     // EMBR_PARAM_*
};

struct PluginFnDesc {
    std::string              name;
    std::vector<PluginParam> params;
    uint32_t                 resultTypes = 0;     // EMBR_T_* bits
    uint32_t                 resultElem = 0;
};

struct PluginDesc {
    std::string               name, path;         // `path` is where it was found, absolute
    uint64_t                  caps = 0;           // EMBR_CAP_* bits
    std::vector<PluginFnDesc> funcs;
    const PluginFnDesc* find(const std::string& fn) const {
        for (const PluginFnDesc& f : funcs) if (f.name == fn) return &f;
        return nullptr;
    }
};

bool readPlugin(const std::string& name, const std::string& scriptDir, PluginDesc& out, std::string& err);

}  // namespace aembr

#endif
