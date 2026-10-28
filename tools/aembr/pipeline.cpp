// pipeline.cpp
// see pipeline.h.

#include "pipeline.h"

#include "front.h"
#include "lower.h"

#include <embr/embr.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace aembr {

namespace {

// what a plugin says about itself: the name is the file it is written to (what a script imports), the rest is the file's `__module__`
bool pluginMeta(const std::string& source, const std::string& path, const std::string& name, PluginMeta& meta, std::string& err) {
    const embr::ModuleInfo info = embr::analyzeModule(source, path, embr::PluginPolicy{});
    for (const embr::ModuleProblem& p : info.problems)
        err += path + ":" + std::to_string(p.range.startLine) + ": " + p.text + "\n";
    if (!err.empty()) return false;
    meta.name = name;
    meta.version = info.declared && !info.version.empty() ? info.version : "0.0.0";
    meta.description = info.description;
    meta.caps = info.effectiveCaps();
    for (const embr::ModuleFunctionInfo& f : info.functions) {
        // the host prints the signature itself, so the doc is the comment above the function, or the signature when there is none
        const std::string sig = f.signature.compare(0, 3, "fn ") == 0 ? f.signature.substr(3) : f.signature;
        meta.docs[f.name] = f.doc.empty() ? sig : f.doc;
    }
    return true;
}

}  // namespace

int compileSource(const CompileRequest& req, Compiled& out, std::string& err) {
    Front front;
    front.stdDir = runtimeDir(req.bo, req.exeDir) + "/std";
    if (!req.srcText.empty()) {
        front.path = req.srcPath;
        front.source = req.srcText;
        // the same steps readFront takes after it has the text
        if (!readFrontText(front, err)) return 1;
    } else if (!readFront(req.srcPath, front, err)) {
        return 1;
    }
    const std::string base = std::filesystem::path(req.srcPath).filename().string();

    LowerOptions lo;
    lo.plugin = req.plugin;
    lo.denyCaps = req.denyCaps;
    lo.infer = req.bo.infer;
    lo.fatalOverflow = req.bo.fatalOverflow;
    if (req.plugin && req.bo.fatalOverflow) { err = "aembr: --fatal-overflow is for programs: a plugin runs inside another program, which it cannot stop\n"; return 2; }
    try {
        out.mod = lower(front, lo);
    } catch (const Refusal& r) {
        err = formatRefusal(req.srcPath, r);
        return 1;
    }
    if (req.stage != Stage::C) return 0;

    // a plugin is moved between machines, so nothing about this machine goes into it: the file is named by its name only, and the plugins it
    // calls are looked for by name when it runs
    if (req.plugin) {
        out.mod.file = base;
        out.mod.scriptDir.clear();
        for (ir::PluginUse& p : out.mod.plugins) p.path.clear();
        if (!pluginMeta(front.source, req.srcPath, req.pluginName, out.meta, err)) return 1;
    }

    BuildRecord& r = out.rec;
    r.aembr = kAembrVersion;
    r.kind = req.plugin ? "plugin" : "program";
    r.name = req.plugin ? req.pluginName : "";
    r.source = base;
    // the source is the file and every module it imports (by their names relative to its folder, so a package that carries them gives the same hash)
    std::string everything = front.source;
    for (const std::string& mp : front.modules) {
        std::error_code mec;
        const std::string rel = std::filesystem::path(mp).lexically_normal().lexically_relative(std::filesystem::path(front.path).lexically_normal().parent_path()).string();
        std::string text;
        { std::ifstream in(mp, std::ios::binary); std::stringstream ss; ss << in.rdbuf(); text = ss.str(); }
        everything += std::string(1, '\0') + rel + std::string(1, '\0') + text;
        out.modules.emplace_back(rel, mp);
    }
    unsigned char d[32];
    embr::sha256(everything.data(), everything.size(), d);
    r.sourceSha256 = embr::hexOf(d, 32);
    uint64_t caps = req.plugin ? out.meta.caps : embr::analyzeModule(front.source, req.srcPath, embr::PluginPolicy{}).effectiveCaps();
    r.capabilities = embr::capNames(caps);
    std::sort(r.capabilities.begin(), r.capabilities.end());
    BuildOptions bo = req.bo;
    bo.shared = req.plugin;
    r.cc = compilerVersion(bo);
    r.flags = compileFlags(bo);
    r.runtimeSha256 = runtimeSha256(runtimeDir(bo, req.exeDir));
    r.target = embr::hostTarget();
    const std::string json = recordJson(r);

    if (req.plugin) {
        if (!emitPluginC(out.mod, out.meta, out.c, err, json)) { err = req.srcPath + ": " + err + "\n"; return 1; }
    } else {
        out.c = emitC(out.mod, json);
    }
    return 0;
}

}  // namespace aembr
