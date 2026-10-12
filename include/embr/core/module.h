#ifndef EMBR_CORE_MODULE_H
#define EMBR_CORE_MODULE_H

// shared file lookup for anything that loads a file relative to the running script: embr script modules
// (`import "foo.embr"`, load_module()) and native plugins (`import "foo"`) all search the same way, next to
// the script, in a subdirectory, or in the current working directory
// it lives here so every backend and load_module() can't drift apart
// it only builds and searches candidate lists. each caller formats its own errors

#include "registry.h"
#include "package.h"
#ifdef _WIN32
#  include <windows.h>
#endif

#if defined(__APPLE__)
#  include <mach-o/dyld.h>
#endif
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace embr {

inline std::filesystem::path withFileExtension(std::filesystem::path base, const std::string& ext) {
    if (base.extension() == ext) return base;
    return std::filesystem::path(base.string() + ext);
}

// directory containing the running executable, or empty if it can't be determined
inline std::filesystem::path executableDir() {
#if defined(__linux__)
    std::error_code ec;
    auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec) return p.parent_path();
#elif defined(__APPLE__)
    char buf[4096];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) {
        std::error_code ec;
        auto p = std::filesystem::canonical(buf, ec);        // resolves symlinks and ".." in the raw path
        return (ec ? std::filesystem::path(buf) : p).parent_path();
    }
#elif defined(_WIN32)
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return std::filesystem::path(buf).parent_path();
#endif
    return {};
}

// entries of the colon-separated (';' on Windows) EMBR_PATH environment variable
inline std::vector<std::filesystem::path> embrPathDirs() {
    std::vector<std::filesystem::path> dirs;
    const char* env = std::getenv("EMBR_PATH");
    if (!env) return dirs;
#ifdef _WIN32
    const char sep = ';';
#else
    const char sep = ':';
#endif
    std::string s = env, cur;
    for (char c : s + sep) {
        if (c == sep) { if (!cur.empty()) dirs.emplace_back(cur); cur.clear(); }
        else cur += c;
    }
    return dirs;
}

// ordered candidate list for a raw import/load path:
//   absolute path          -> just that path (with ext appended if missing)
//   relative w/ a parent   -> scriptDir/<path>, exeDir/<path>, EMBR_PATH dirs/<path>, cwd/<path>
//   bare name              -> scriptDir/<name>, scriptDir/<subdir>/<name>, then the same two
//                             under exeDir and each EMBR_PATH dir, cwd/<name>, cwd/<subdir>/<name>
// cwd is searched last, kept only for back-compat
// (scriptDir candidates are skipped when scriptDir is empty, e.g. REPL input)
// rawPath is whatever the import path evaluated to at runtime, a literal or a computed string, both searched the same
inline std::vector<std::filesystem::path> extraDirs() {
    std::vector<std::filesystem::path> dirs;
    auto exe = executableDir();
    if (!exe.empty()) {
        dirs.push_back(exe);
        dirs.push_back((exe / ".." / "lib" / "embr").lexically_normal());   // an installed prefix: bin/embr next to lib/embr/{plugins,modules}
    }
    for (auto& d : embrPathDirs()) dirs.push_back(d);
    return dirs;
}

inline std::vector<std::filesystem::path> fileCandidates(
    const std::string& rawPath, const std::string& scriptDir,
    const std::string& ext, const std::string& subdir)
{
    namespace fs = std::filesystem;
    fs::path p(rawPath);
    std::vector<fs::path> cands;
    if (p.is_absolute()) {
        cands.push_back(withFileExtension(p, ext));
    } else if (p.has_parent_path()) {
        fs::path withE = withFileExtension(p, ext);
        if (!scriptDir.empty())
            cands.push_back((fs::path(scriptDir) / withE).lexically_normal());
        for (const auto& d : extraDirs()) cands.push_back((d / withE).lexically_normal());
        cands.push_back((fs::current_path() / withE).lexically_normal());
    } else {
        fs::path name = withFileExtension(p, ext);
        if (!scriptDir.empty()) {
            fs::path base(scriptDir);
            cands.push_back(base / name);
            cands.push_back(base / subdir / name);
        }
        for (const auto& d : extraDirs()) {
            cands.push_back(d / name);
            cands.push_back(d / subdir / name);
        }
        cands.push_back(fs::current_path() / name);
        cands.push_back(fs::current_path() / subdir / name);
    }
    return cands;
}

// first candidate that exists on disk, or an empty path if none do
inline std::filesystem::path findExistingCandidate(const std::vector<std::filesystem::path>& cands) {
    for (const auto& c : cands) {
        std::error_code ec;
        if (std::filesystem::exists(c, ec) && !ec) return c;
    }
    return {};
}

// "\n    <candidate>" per entry, for a "tried:" error message suffix
inline std::string describeCandidates(const std::vector<std::filesystem::path>& cands) {
    std::string tried;
    for (const auto& c : cands) tried += "\n    " + c.string();
    return tried;
}

// canonicalizes p, falling back to p itself if canonicalization fails (e.g. a dangling symlink)
inline std::string canonicalOrSelf(const std::filesystem::path& p) {
    std::error_code ec;
    auto canon = std::filesystem::canonical(p, ec);
    return (ec ? p : canon).string();
}

// what a package says its plugin is. the library must say the same, and need no more than the package declares
struct PluginClaim {
    std::string name, version;
    uint64_t    caps = 0;
};

inline std::string checkPluginClaim(const PluginClaim* claim, const EmbrPluginInfo* info) {
    if (!claim) return "";
    if (!info) return "the package's library does not describe itself (no embr_plugin_info)";
    if (!info->name || claim->name != info->name)
        return "the package is called \"" + claim->name + "\" but its library says it is \"" + (info->name ? info->name : "") + "\"";
    if (!info->version || claim->version != info->version)
        return "the package says version " + claim->version + " but its library says " + (info->version ? info->version : "none");
    uint64_t needs = info->capabilities;
    if (info->struct_size >= offsetof(EmbrPluginInfo, optional_capabilities) + sizeof(info->optional_capabilities)) needs |= info->optional_capabilities;
    if (needs & ~claim->caps) {
        std::string names;
        for (const auto& n : capNames(needs & ~claim->caps)) names += (names.empty() ? "" : ", ") + n;
        return "the package does not declare " + names + ", which its library needs";
    }
    return "";
}

// a package (core/package.h) found by importNativePlugin. defined in core/packageimport.h, which needs the module analyser
inline std::string importPackageFile(Interpreter& interp, const std::filesystem::path& file, bool runModules);
inline std::string importPluginFrom(Interpreter& interp, const std::string& rawPath, const std::vector<std::filesystem::path>& cands, const PluginClaim* claim);

// the one place a native plugin is located, policy-checked, opened and registered. both backends' `import`
// call it, so neither can bypass the sandbox policy (PluginPolicy). returns "" on success, else a message
// for the caller to raise with its own source location
// unrestricted: the usual search (script dir, exe dir, EMBR_PATH, cwd)
// restricted: only the executable's directory and EMBR_PATH, which the host controls. never the script's
// directory or the cwd, where a sandboxed script could have planted a same-named library
// a package (name.pmbr) is looked for in the same places, before a loose library. runModules false: a package whose entry is a
// script module is left alone (an editor or the type checker only wants plugin signatures, and must not run a module)
inline std::string importNativePlugin(Interpreter& interp, const std::string& rawPath, bool runModules = true) {
    std::string denied = interp.pluginPolicy.check(rawPath);
    if (!denied.empty()) return denied;

    std::vector<std::filesystem::path> cands;
    {
        std::vector<std::filesystem::path> pcands;
        const bool explicitPackage = std::filesystem::path(rawPath).extension() == ".pmbr";
        const std::string stem = explicitPackage ? std::filesystem::path(rawPath).stem().string() : rawPath;
        if (interp.pluginPolicy.restricted) {
            if (!explicitPackage || !std::filesystem::path(rawPath).has_parent_path())
                for (const auto& d : extraDirs()) {
                    pcands.push_back(d / (stem + ".pmbr"));
                    pcands.push_back(d / "packages" / (stem + ".pmbr"));
                }
        } else {
            pcands = fileCandidates(explicitPackage ? rawPath : rawPath, interp.scriptDir, ".pmbr", "packages");
        }
        std::filesystem::path pkg = findExistingCandidate(pcands);
        if (!pkg.empty()) return importPackageFile(interp, pkg, runModules);
        if (explicitPackage) return "cannot find package \"" + rawPath + "\"\n  tried:" + describeCandidates(pcands);
    }
    if (interp.pluginPolicy.restricted) {
        for (const auto& d : extraDirs()) {
            cands.push_back(d / (rawPath + PLUGIN_EXT));
            cands.push_back(d / "plugins" / (rawPath + PLUGIN_EXT));
        }
    } else {
        cands = fileCandidates(rawPath, interp.scriptDir, PLUGIN_EXT, "plugins");
    }
    return importPluginFrom(interp, rawPath, cands, nullptr);
}

// opens the first library in `cands` that opens and registers it. `claim`: it comes from a package, which says what it is
inline std::string importPluginFrom(Interpreter& interp, const std::string& rawPath, const std::vector<std::filesystem::path>& cands, const PluginClaim* claim) {
    PluginHandle handle = nullptr;
    std::string  loadedFrom;
    for (const auto& c : cands) {
        handle = pluginOpen(c.string().c_str());
        if (handle) { loadedFrom = c.string(); break; }
    }
    if (!handle)
        return "cannot load plugin \"" + rawPath + "\": " + pluginError() + "\n  tried:" + describeCandidates(cands);

    if (!interp.loadedPlugins_.insert(loadedFrom).second) {   // already registered here
        pluginClose(handle);
        return "";
    }

    // the description a plugin exports (embr_plugin_info) is read before anything of the plugin runs. a plugin written in C
    // has only that, and embr binds the functions in its table (cplugin.h). a C++ plugin also has embr_register
    auto infoFn = reinterpret_cast<EmbrPluginInfoFn>(pluginSym(handle, "embr_plugin_info"));
    using RegFn = void(*)(Interpreter*);
    auto reg = reinterpret_cast<RegFn>(pluginSym(handle, "embr_register"));
    auto refuse = [&](const std::string& why) {
        interp.loadedPlugins_.erase(loadedFrom);
        pluginClose(handle);
        return why;
    };
    if (!infoFn && !reg) return refuse("neither 'embr_register' nor 'embr_plugin_info' found in \"" + loadedFrom + "\"");

    if (claim) {
        std::string why = checkPluginClaim(claim, infoFn ? infoFn() : nullptr);
        if (!why.empty()) return refuse(why);
    }

    if (infoFn && !reg) {   // a C plugin
        std::string why;
        try { why = loadCPlugin(interp, infoFn(), loadedFrom); }
        catch (...) { interp.loadedPlugins_.erase(loadedFrom); pluginClose(handle); throw; }
        if (!why.empty()) return refuse(why);
        interp.pluginHandles_.push_back(handle);
        std::cerr << "[runtime] loaded plugin: " << loadedFrom << "\n";
        return "";
    }

    // a C++ plugin. one that does not describe itself cannot be checked against the capabilities the host denies, so a
    // host that denies any refuses it
    const EmbrPluginInfo* info = infoFn ? infoFn() : nullptr;
    if (info) {
        std::string why = checkPluginInfo(interp, info, loadedFrom, /*isCxx=*/true);
        if (!why.empty()) return refuse(why);
    } else if (interp.pluginPolicy.deniedCaps) {
        return refuse("plugin \"" + rawPath + "\" does not declare its capabilities (it has no embr_plugin_info), "
                      "which a sandbox that denies capabilities needs. rebuild it with EMBR_PLUGIN_INFO");
    }
    std::set<std::string> before;   // to find what the plugin binds: the names that are new afterwards
    for (const auto& n : interp.nativeNames()) before.insert(n);
    try {
        reg(&interp);
        interp.pluginHandles_.push_back(handle);
    } catch (...) {
        interp.loadedPlugins_.erase(loadedFrom);
        pluginClose(handle);
        throw;
    }
    recordPlugin(interp, info, loadedFrom, /*isCxx=*/true);
    for (const auto& n : interp.nativeNames())
        if (!before.count(n)) interp.plugins.back().bound.push_back(n);
    std::cerr << "[runtime] loaded plugin: " << loadedFrom << "\n";
    return "";
}

// what an installed plugin file says about itself, read without running anything of the plugin but embr_plugin_info
// (so a plugin built with another toolchain, or one embr would refuse, can still be listed with the reason)
struct PluginFileInfo {
    std::string path, name, version, description, buildId, abiTag, problem;   // problem: why embr would not load it, "" if it would
    bool        declared = false, cxx = false;
    uint32_t    abi = 0;
    uint64_t    caps = 0, optionalCaps = 0;
    size_t      functions = 0;
};

inline PluginFileInfo readPluginFile(const std::filesystem::path& file) {
    PluginFileInfo r;
    r.path = file.string();
    r.name = file.stem().string();
    PluginHandle h = pluginOpen(r.path.c_str());
    if (!h) { r.problem = pluginError(); return r; }
    auto infoFn = reinterpret_cast<EmbrPluginInfoFn>(pluginSym(h, "embr_plugin_info"));
    r.cxx = pluginSym(h, "embr_register") != nullptr;
    if (!infoFn) {
        if (!r.cxx) r.problem = "neither embr_register nor embr_plugin_info is exported";
        pluginClose(h);
        return r;
    }
    const EmbrPluginInfo* info = infoFn();
    if (info && info->abi_version == EMBR_PLUGIN_ABI_VERSION && info->struct_size >= offsetof(EmbrPluginInfo, nfuncs) + sizeof(info->nfuncs)) {
        r.declared = true;
        r.abi = info->abi_version;
        if (info->name) r.name = info->name;
        if (info->version) r.version = info->version;
        if (info->description) r.description = info->description;
        r.caps = info->capabilities;
        r.functions = info->nfuncs;
        if (info->struct_size >= offsetof(EmbrPluginInfo, optional_capabilities) + sizeof(info->optional_capabilities)) r.optionalCaps = info->optional_capabilities;
        if (info->struct_size >= offsetof(EmbrPluginInfo, build_id) + sizeof(info->build_id) && info->build_id) r.buildId = info->build_id;
        if (info->struct_size >= offsetof(EmbrPluginInfo, abi_tag) + sizeof(info->abi_tag) && info->abi_tag) r.abiTag = info->abi_tag;
        if (r.cxx && r.abiTag != abiTag()) r.problem = "built with a different compiler, standard library or embr C++ interface (" + (r.abiTag.empty() ? std::string("no ABI tag") : r.abiTag) + ", this embr is " + abiTag() + ")";
    } else if (info) {
        r.abi = info->abi_version;
        r.problem = "built for plugin ABI " + std::to_string(info->abi_version) + ", this embr uses ABI " + std::to_string(EMBR_PLUGIN_ABI_VERSION);
    }
    pluginClose(h);
    return r;
}

// the plugins found in the places `import` looks (next to embr, its plugins/ folder, lib/embr, EMBR_PATH), one per
// name, sorted by name. where a name is in two places the first one wins, as it does for import
inline std::vector<PluginFileInfo> installedPlugins() {
    std::vector<PluginFileInfo> out;
    std::set<std::string> seenFiles;
    std::vector<std::filesystem::path> dirs;
    for (const auto& d : extraDirs()) { dirs.push_back(d); dirs.push_back(d / "plugins"); }
    for (const auto& d : dirs) {
        std::error_code ec;
        if (!std::filesystem::is_directory(d, ec)) continue;
        std::vector<std::filesystem::path> files;
        for (const auto& e : std::filesystem::directory_iterator(d, ec))
            if (e.is_regular_file(ec) && e.path().extension() == PLUGIN_EXT) files.push_back(e.path());
        std::sort(files.begin(), files.end());
        for (const auto& f : files) {
            std::string fileName = f.filename().string();
            if (!seenFiles.insert(fileName).second) continue;   // the same file name earlier on the search path wins
            out.push_back(readPluginFile(f));
        }
    }
    std::sort(out.begin(), out.end(), [](const PluginFileInfo& a, const PluginFileInfo& b) { return a.name < b.name; });
    return out;
}

// reads resolved's full contents. caller checks the returned optional and
// raises its own "cannot open file" error on nullopt (kept out of this
// header for the same reason as the rest of it, see file header comment)
inline std::optional<std::string> tryReadFile(const std::filesystem::path& resolved) {
    std::ifstream f(resolved);
    if (!f) return std::nullopt;
    return std::string((std::istreambuf_iterator<char>(f)), {});
}

// after a module's statements have run inside a pushed module scope (see Interpreter::pushModuleScope/
// popModuleScope), decides what the importer gets
//
// mutateCallerScope=true (the `import` statement): merges every non-local top-level name into the caller's
// current scope, a side effect like a plugin's embr_register binding functions into globals
//
// mutateCallerScope=false (load_module()): collects those names into a map and returns it, leaving the
// caller's scope alone, like ffi_open() handing back a handle
inline Value exportModuleScope(Interpreter& interp, Interpreter::ModuleScopeResult&& res,
                               bool mutateCallerScope) {
    // `__module__` describes the module (core/modinfo.h). it is not an export: importing two modules must not make them
    // overwrite each other's description in the importer
    if (mutateCallerScope) {
        for (auto& [name, val] : res.scope)
            if (!res.locals.count(name) && name != "__module__")
                interp.define(name, std::move(val));
        return Value(0.0);
    }
    Value::map_type m;
    for (auto& [name, val] : res.scope)
        if (!res.locals.count(name) && name != "__module__")
            m[name] = std::move(val);
    return Value(std::move(m));
}

} // namespace embr

#endif // EMBR_CORE_MODULE_H
