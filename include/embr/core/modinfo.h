// modinfo.h
// what a script module says about itself, found by reading its source without running any of it: a `__module__` map at the
// top level, the plugins and modules it imports, its functions with their signatures, and the comment above each function.
// a plugin says the same in its exported description (plugininfo.h), see describe.h.
//
// usage, at the top of a module
//   __module__ = {
//       "version": "1.2.0",
//       "description": "reads and writes the config file",
//       "capabilities": ["filesystem"],
//   }
//
// good to know
//   - the right-hand side must be a literal (strings, numbers, arrays and maps of those), because it is read from the file,
//     not from a running module. keys: name (default: the file name), version, description, capabilities (names, see plugininfo.h)
//   - the loader keeps `__module__` out of what an import gives the importer, so two modules never overwrite each other's
//   - capabilities are INFERRED from the literal imports (a plugin by its own description, a module by its own, all the way
//     down). a declaration may claim more, but one that leaves out something the module imports is an error. a computed
//     import (`import "plugins/" + name`, `load_module(x)`) cannot be followed: `dynamicImports` says so, and only a
//     declaration can account for it
//   - this is for early refusal and for reading a module before trusting it. the boundary itself is still the plugin
//     import: a module that imports a denied plugin is refused there, whatever it declared
//   - the doc line of a function is the block of `#` comment lines directly above it (no blank line in between)

#ifndef EMBR_CORE_MODINFO_H
#define EMBR_CORE_MODINFO_H

#include "describe.h"
#include "module.h"
#include "lexer.h"
#include "parser.h"
#include "vmbr_meta.h"

#include <algorithm>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace embr {

inline constexpr const char* kModuleMetaName = "__module__";

struct ModuleImportInfo {
    std::string path;
    bool        plugin = false;     // a native plugin (anything not ending in .embr)
    bool        found = false;      // the file was found, so its capabilities are known
    uint64_t    caps = 0;           // what it needs (a module: what it declares and infers)
    int         line = 0;
};

struct ModuleFunctionInfo {
    std::string name, signature, doc;
    int         line = 0;
};

struct ModuleProblem {
    std::string text;
    SourceRange range;
};

struct ModuleInfo {
    std::string path, name, version, description;
    bool        parsed = false;           // false when the file does not parse: nothing else is known
    bool        declared = false;         // has a `__module__`
    bool        dynamicImports = false;   // an import or load_module whose path is computed
    bool        compiled = false;         // a .vmbr file
    bool        noMetadata = false;       // a .vmbr file written without a description: nothing is known about it
    uint64_t    declaredCaps = 0, inferredCaps = 0;
    std::vector<ModuleImportInfo>   imports;
    std::vector<ModuleFunctionInfo> functions;
    std::vector<ModuleProblem>      problems;
    uint64_t effectiveCaps() const { return declaredCaps | inferredCaps; }
};

namespace modinfo_detail {

inline void walkExpr(const Expr* e, const std::function<void(const Expr*)>& fe, const std::function<void(const Stmt*)>& fs);
inline void walkStmts(const std::vector<StmtPtr>& v, const std::function<void(const Stmt*)>& fs, const std::function<void(const Expr*)>& fe) {
    for (const auto& s : v) {
        if (!s) continue;
        fs(s.get());
        switch (s->kind) {
            case Stmt::Kind::Assign: { auto* a = static_cast<const AssignStmt*>(s.get()); walkExpr(a->target.get(), fe, fs); walkExpr(a->value.get(), fe, fs); break; }
            case Stmt::Kind::MultiAssign: walkExpr(static_cast<const MultiAssignStmt*>(s.get())->value.get(), fe, fs); break;
            case Stmt::Kind::Local: walkExpr(static_cast<const LocalStmt*>(s.get())->value.get(), fe, fs); break;
            case Stmt::Kind::If: { auto* i = static_cast<const IfStmt*>(s.get()); walkExpr(i->cond.get(), fe, fs); walkStmts(i->thenBlock, fs, fe); walkStmts(i->elseBlock, fs, fe); break; }
            case Stmt::Kind::Fn: walkStmts(static_cast<const FnStmt*>(s.get())->body, fs, fe); break;
            case Stmt::Kind::Return: walkExpr(static_cast<const ReturnStmt*>(s.get())->value.get(), fe, fs); break;
            case Stmt::Kind::While: { auto* w = static_cast<const WhileStmt*>(s.get()); walkExpr(w->cond.get(), fe, fs); walkStmts(w->body, fs, fe); break; }
            case Stmt::Kind::For: { auto* f = static_cast<const ForStmt*>(s.get()); walkExpr(f->iterable.get(), fe, fs); walkStmts(f->body, fs, fe); break; }
            case Stmt::Kind::Import: walkExpr(static_cast<const ImportStmt*>(s.get())->pathExpr.get(), fe, fs); break;
            case Stmt::Kind::Expr: walkExpr(static_cast<const ExprStmt*>(s.get())->expr.get(), fe, fs); break;
            case Stmt::Kind::Try: { auto* t = static_cast<const TryStmt*>(s.get()); walkStmts(t->tryBlock, fs, fe); walkStmts(t->catchBlock, fs, fe); break; }
            default: break;
        }
    }
}
inline void walkExpr(const Expr* e, const std::function<void(const Expr*)>& fe, const std::function<void(const Stmt*)>& fs) {
    if (!e) return;
    fe(e);
    switch (e->kind) {
        case Expr::Kind::Unary: walkExpr(static_cast<const UnaryExpr*>(e)->right.get(), fe, fs); break;
        case Expr::Kind::Binary: { auto* b = static_cast<const BinaryExpr*>(e); walkExpr(b->left.get(), fe, fs); walkExpr(b->right.get(), fe, fs); break; }
        case Expr::Kind::And: case Expr::Kind::Or: { auto* l = static_cast<const LogicExpr*>(e); walkExpr(l->left.get(), fe, fs); walkExpr(l->right.get(), fe, fs); break; }
        case Expr::Kind::Call: { auto* c = static_cast<const CallExpr*>(e); walkExpr(c->callee.get(), fe, fs); for (auto& a : c->args) walkExpr(a.get(), fe, fs); break; }
        case Expr::Kind::Array: for (auto& x : static_cast<const ArrayExpr*>(e)->elements) walkExpr(x.get(), fe, fs); break;
        case Expr::Kind::Map: for (auto& kv : static_cast<const MapExpr*>(e)->entries) { walkExpr(kv.first.get(), fe, fs); walkExpr(kv.second.get(), fe, fs); } break;
        case Expr::Kind::Index: { auto* i = static_cast<const IndexExpr*>(e); walkExpr(i->object.get(), fe, fs); walkExpr(i->index.get(), fe, fs); break; }
        case Expr::Kind::Ref: walkExpr(static_cast<const RefExpr*>(e)->target.get(), fe, fs); break;
        case Expr::Kind::Lambda: walkStmts(static_cast<const LambdaExpr*>(e)->ownedBody, fs, fe); break;   // the parser fills ownedBody; `body` is not set
        default: break;
    }
}

// the lines of a text, without the line ends
inline std::vector<std::string> splitLines(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i)
        if (i == s.size() || s[i] == '\n') {
            size_t end = i;
            if (end > start && s[end - 1] == '\r') --end;
            out.push_back(s.substr(start, end - start));
            start = i + 1;
        }
    return out;
}

// the block of `#` lines directly above 1-based line `line`, as one text. a `#[ ... ]#` block comment is not a doc
inline std::string commentAbove(const std::vector<std::string>& lines, int line) {
    std::vector<std::string> block;
    for (int i = line - 2; i >= 0 && i < (int)lines.size(); --i) {
        const std::string& l = lines[(size_t)i];
        size_t p = l.find_first_not_of(" \t");
        if (p == std::string::npos || l[p] != '#' || (p + 1 < l.size() && l[p + 1] == '[')) break;
        std::string text = l.substr(p + 1);
        if (!text.empty() && text[0] == ' ') text.erase(0, 1);
        while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.pop_back();
        block.push_back(text);
    }
    std::reverse(block.begin(), block.end());
    std::string out;
    for (const auto& b : block) {
        if (b.empty()) continue;
        out += (out.empty() ? "" : " ") + b;
    }
    return out;
}

// the capabilities of a plugin file, read without running it. thread_local: async workers analyse modules too
inline bool pluginCaps(const std::string& rawPath, const std::string& baseDir, const PluginPolicy& policy, uint64_t& caps) {
    static thread_local std::map<std::string, uint64_t> cache;
    std::vector<std::filesystem::path> cands;
    if (policy.restricted) {   // only the places the host controls, like importNativePlugin (a planted library must not be opened)
        for (const auto& d : extraDirs()) {
            cands.push_back(d / (rawPath + PLUGIN_EXT));
            cands.push_back(d / "plugins" / (rawPath + PLUGIN_EXT));
        }
    } else {
        cands = fileCandidates(rawPath, baseDir, PLUGIN_EXT, "plugins");
    }
    {   // a package (core/package.h) named this: what it declares is what it needs
        std::vector<std::filesystem::path> pcands;
        if (policy.restricted) {
            for (const auto& d : extraDirs()) { pcands.push_back(d / (rawPath + ".pmbr")); pcands.push_back(d / "packages" / (rawPath + ".pmbr")); }
        } else {
            pcands = fileCandidates(rawPath, baseDir, ".pmbr", "packages");
        }
        std::filesystem::path pk = findExistingCandidate(pcands);
        if (!pk.empty()) {
            PackageInfo pi = analyzePackageFile(pk);
            if (!pi.parsed || !pi.problems.empty()) return false;
            caps = pi.meta.caps;
            return true;
        }
    }
    std::filesystem::path found = findExistingCandidate(cands);
    if (found.empty()) return false;
    auto it = cache.find(found.string());
    if (it != cache.end()) { caps = it->second; return true; }
    PluginFileInfo info = readPluginFile(found);
    if (!info.declared) return false;
    cache[found.string()] = info.caps;
    caps = info.caps;
    return true;
}

} // namespace modinfo_detail

// reads a module's source. `path` names the file (and is where its imports are looked for). `policy` decides where plugins
// may be looked for: a restricted policy only opens plugins from the places the host controls
inline ModuleInfo analyzeModule(const std::string& source, const std::string& path, const PluginPolicy& policy,
                                std::set<std::string>& visiting, int depth = 0);

inline ModuleInfo analyzeCompiledFile(const std::filesystem::path& file, const PluginPolicy& policy, std::set<std::string>& visiting, int depth = 0);

// a script (.embr) or a compiled module (.vmbr)
inline ModuleInfo analyzeModuleFile(const std::filesystem::path& file, const PluginPolicy& policy, std::set<std::string>& visiting, int depth = 0) {
    if (file.extension() == ".vmbr") return analyzeCompiledFile(file, policy, visiting, depth);
    ModuleInfo info;
    info.path = file.string();
    info.name = file.stem().string();
    auto src = tryReadFile(file);
    if (!src) { info.problems.push_back({"cannot open the file", {}}); return info; }
    return analyzeModule(*src, file.string(), policy, visiting, depth);
}

// reads what the source says by itself: the declaration, the imports as written, the functions. nothing is looked up, so it needs
// no policy and the result can be saved in a compiled file (encodeModuleInfo)
inline ModuleInfo analyzeSource(const std::string& source, const std::string& path) {
    namespace fs = std::filesystem;
    using namespace modinfo_detail;
    ModuleInfo info;
    info.path = path;
    info.name = fs::path(path).stem().string();

    std::vector<StmtPtr> prog;
    std::vector<EmbrError> errors;
    try {
        Lexer lex(source);
        auto tokens = lex.tokenize();
        SourceMap sm = lex.sourceMap();
        Parser parser(std::move(tokens), sm);
        parser.collectErrors(&errors);
        prog = parser.parse();
    } catch (const EmbrError&) {
        return info;
    }
    if (!errors.empty()) return info;
    info.parsed = true;

    const std::vector<std::string> lines = splitLines(source);

    // the declaration: a top-level `__module__ = {literal}`
    auto problem = [&](const std::string& text, const SourceRange& r) { info.problems.push_back({text, r}); };
    bool seenDecl = false;
    for (const auto& st : prog) {
        if (st->kind != Stmt::Kind::Assign) continue;
        auto* a = static_cast<const AssignStmt*>(st.get());
        if (!a->target || a->target->kind != Expr::Kind::Var || static_cast<const VarExpr*>(a->target.get())->name != kModuleMetaName) continue;
        if (seenDecl) { problem(std::string(kModuleMetaName) + " is assigned more than once", st->range); continue; }
        seenDecl = true;
        info.declared = true;
        if (!a->value || a->value->kind != Expr::Kind::Map) {
            problem(std::string(kModuleMetaName) + " must be a map written out in the file, like {\"version\": \"1.0.0\"}", st->range);
            continue;
        }
        for (const auto& kv : static_cast<const MapExpr*>(a->value.get())->entries) {
            if (!kv.first || kv.first->kind != Expr::Kind::String) { problem("a key of " + std::string(kModuleMetaName) + " must be a string", kv.first ? kv.first->range : st->range); continue; }
            const std::string& key = static_cast<const StringExpr*>(kv.first.get())->v;
            const Expr* v = kv.second.get();
            auto asString = [&](std::string& out) {
                if (!v || v->kind != Expr::Kind::String) { problem("\"" + key + "\" in " + kModuleMetaName + " must be a string written out in the file", v ? v->range : st->range); return; }
                out = static_cast<const StringExpr*>(v)->v;
            };
            if (key == "name") asString(info.name);
            else if (key == "version") asString(info.version);
            else if (key == "description") asString(info.description);
            else if (key == "capabilities") {
                if (!v || v->kind != Expr::Kind::Array || static_cast<const ArrayExpr*>(v)->isRange) {
                    problem("\"capabilities\" in " + std::string(kModuleMetaName) + " must be an array of names, like [\"network\"]", v ? v->range : st->range);
                    continue;
                }
                for (const auto& el : static_cast<const ArrayExpr*>(v)->elements) {
                    uint64_t bit = 0;
                    std::string bad;
                    if (!el || el->kind != Expr::Kind::String) { problem("a capability name must be a string written out in the file", el ? el->range : st->range); continue; }
                    if (!capMask(static_cast<const StringExpr*>(el.get())->v, bit, bad) || bit == 0)
                        problem("'" + static_cast<const StringExpr*>(el.get())->v + "' is not a capability (filesystem, network, process, env, threads, native)", el->range);
                    else info.declaredCaps |= bit;
                }
            } else problem("unknown key \"" + key + "\" in " + kModuleMetaName + " (name, version, description, capabilities)", kv.first->range);
        }
    }
    // a `__module__` assigned anywhere but the top level is not read, and would be a silent surprise
    {
        std::set<const Stmt*> topLevel;
        for (const auto& st : prog) topLevel.insert(st.get());
        walkStmts(prog, [&](const Stmt* s) {
            if (s->kind != Stmt::Kind::Assign || topLevel.count(s)) return;
            auto* a = static_cast<const AssignStmt*>(s);
            if (a->target && a->target->kind == Expr::Kind::Var && static_cast<const VarExpr*>(a->target.get())->name == kModuleMetaName)
                problem(std::string(kModuleMetaName) + " must be assigned at the top level of the file, not inside a block or function", s->range);
        }, [&](const Expr*) {});
    }

    // imports: every `import "x"` and `load_module("x")` anywhere in the file. a computed path cannot be followed
    auto addImport = [&](const std::string& raw, int line) {
        ModuleImportInfo imp;
        imp.path = raw;
        imp.line = line;
        const fs::path ext = fs::path(raw).extension();
        imp.plugin = ext != ".embr" && ext != ".vmbr";
        info.imports.push_back(std::move(imp));
    };
    walkStmts(prog,
        [&](const Stmt* s) {
            if (s->kind != Stmt::Kind::Import) return;
            const Expr* p = static_cast<const ImportStmt*>(s)->pathExpr.get();
            if (p && p->kind == Expr::Kind::String) addImport(static_cast<const StringExpr*>(p)->v, s->range.startLine);
            else info.dynamicImports = true;
        },
        [&](const Expr* e) {
            if (e->kind != Expr::Kind::Call) return;
            auto* c = static_cast<const CallExpr*>(e);
            if (!c->callee || c->callee->kind != Expr::Kind::Var || static_cast<const VarExpr*>(c->callee.get())->name != "load_module") return;
            if (!c->args.empty() && c->args[0] && c->args[0]->kind == Expr::Kind::String) addImport(static_cast<const StringExpr*>(c->args[0].get())->v, e->range.startLine);
            else info.dynamicImports = true;
        });

    // the top-level functions, with the comment above each
    for (const auto& st : prog) {
        if (st->kind != Stmt::Kind::Fn) continue;
        auto* f = static_cast<const FnStmt*>(st.get());
        if (f->name.rfind("__", 0) == 0) continue;     // a helper the module keeps to itself
        ModuleFunctionInfo fn;
        fn.name = f->name;
        fn.signature = signatureText(f->name, f->params, f->returnType, f->returnElem);
        fn.line = st->range.startLine;
        fn.doc = commentAbove(lines, st->range.startLine);
        info.functions.push_back(std::move(fn));
    }
    return info;
}

// the second half: looks the imports up (a plugin by its description, a module by reading it) and works out the capabilities.
// `info.path` says where the imports are looked for
inline void resolveModule(ModuleInfo& info, const PluginPolicy& policy, std::set<std::string>& visiting, int depth);

inline ModuleInfo analyzeModule(const std::string& source, const std::string& path, const PluginPolicy& policy,
                                std::set<std::string>& visiting, int depth) {
    namespace fs = std::filesystem;
    const std::string canon = canonicalOrSelf(fs::path(path));
    if (!visiting.insert(canon).second) { ModuleInfo cyc; cyc.path = path; cyc.name = fs::path(path).stem().string(); cyc.parsed = true; return cyc; }   // an import cycle: nothing more to learn from this edge
    ModuleInfo info = analyzeSource(source, path);
    if (info.parsed) resolveModule(info, policy, visiting, depth);
    visiting.erase(canon);
    return info;
}

inline void resolveModule(ModuleInfo& info, const PluginPolicy& policy, std::set<std::string>& visiting, int depth) {
    namespace fs = std::filesystem;
    using namespace modinfo_detail;
    const std::string dir = fs::path(info.path).parent_path().string();
    for (auto& imp : info.imports) {
        if (imp.plugin) {
            imp.found = pluginCaps(imp.path, dir, policy, imp.caps);
        } else if (depth < 8) {
            const bool compiledImp = fs::path(imp.path).extension() == ".vmbr";
            auto cands = fileCandidates(imp.path, dir, compiledImp ? ".vmbr" : ".embr", "modules");
            fs::path found = findExistingCandidate(cands);
            if (!found.empty()) {
                ModuleInfo sub = compiledImp ? analyzeCompiledFile(found, policy, visiting, depth + 1)
                                             : analyzeModuleFile(found, policy, visiting, depth + 1);
                imp.found = sub.parsed && !sub.noMetadata;
                imp.caps = sub.effectiveCaps();
                if (sub.noMetadata) info.dynamicImports = true;   // a compiled module with no description cannot be followed
            }
        }
        info.inferredCaps |= imp.caps;
    }
    // a declaration that leaves out something the module imports is wrong
    if (info.declared) {
        for (const auto& imp : info.imports) {
            uint64_t missing = imp.caps & ~info.declaredCaps;
            if (missing) info.problems.push_back({"declares " + capListText(info.declaredCaps) + " but imports \"" + imp.path + "\", which needs " + capListText(missing) +
                                 ". add it to \"capabilities\" in " + kModuleMetaName, {imp.line, 1, imp.line, 1}});
        }
    }
}

inline ModuleInfo analyzeModule(const std::string& source, const std::string& path, const PluginPolicy& policy) {
    std::set<std::string> visiting;
    return analyzeModule(source, path, policy, visiting, 0);
}

// ---- compiled modules ----
// a .vmbr file carries what analyzeSource found (not the capabilities: those are worked out again when it is read, because the
// plugins it imports may have changed). the layout is private to this file and starts with its own number

namespace modinfo_detail {
inline void putU32(std::string& o, uint32_t v) { for (int i = 0; i < 4; ++i) o.push_back((char)((v >> (8 * i)) & 0xFF)); }
inline void putStr(std::string& o, const std::string& v) { putU32(o, (uint32_t)v.size()); o += v; }
struct MetaReader {
    const std::string& in;
    size_t pos = 0;
    bool ok = true;
    explicit MetaReader(const std::string& s) : in(s) {}
    uint32_t u32() {
        if (!ok || in.size() - pos < 4) { ok = false; return 0; }
        uint32_t v = vmbr::readU32(in, pos);
        pos += 4;
        return v;
    }
    uint64_t u64() { uint64_t lo = u32(); return lo | ((uint64_t)u32() << 32); }
    std::string str() {
        uint32_t n = u32();
        if (!ok || in.size() - pos < n) { ok = false; return ""; }
        std::string v = in.substr(pos, n);
        pos += n;
        return v;
    }
    // a count of items that each take at least `each` bytes
    uint32_t count(size_t each) {
        uint32_t n = u32();
        if (ok && (size_t)n > (in.size() - pos) / each) ok = false;
        return ok ? n : 0;
    }
};
constexpr uint32_t kMetaVersion = 1;
} // namespace modinfo_detail

// the part of a module's description that goes into a compiled file
inline std::string encodeModuleInfo(const ModuleInfo& m) {
    using namespace modinfo_detail;
    std::string o;
    putU32(o, kMetaVersion);
    putU32(o, (m.declared ? 1u : 0u) | (m.dynamicImports ? 2u : 0u));
    putStr(o, m.name); putStr(o, m.version); putStr(o, m.description);
    putU32(o, (uint32_t)(m.declaredCaps & 0xFFFFFFFFu)); putU32(o, (uint32_t)(m.declaredCaps >> 32));
    putU32(o, (uint32_t)m.imports.size());
    for (const auto& i : m.imports) { putStr(o, i.path); putU32(o, i.plugin ? 1u : 0u); putU32(o, (uint32_t)i.line); }
    putU32(o, (uint32_t)m.functions.size());
    for (const auto& f : m.functions) { putStr(o, f.name); putStr(o, f.signature); putStr(o, f.doc); putU32(o, (uint32_t)f.line); }
    putU32(o, (uint32_t)m.problems.size());
    for (const auto& p : m.problems) {
        putStr(o, p.text);
        putU32(o, (uint32_t)p.range.startLine); putU32(o, (uint32_t)p.range.startCol);
        putU32(o, (uint32_t)p.range.endLine); putU32(o, (uint32_t)p.range.endCol);
    }
    return o;
}

// false if the text is not a description this version wrote
inline bool decodeModuleInfo(const std::string& blob, ModuleInfo& m) {
    using namespace modinfo_detail;
    MetaReader r(blob);
    if (r.u32() != kMetaVersion) return false;
    uint32_t flags = r.u32();
    m.declared = flags & 1;
    m.dynamicImports = flags & 2;
    m.name = r.str(); m.version = r.str(); m.description = r.str();
    m.declaredCaps = r.u64();
    uint32_t n = r.count(12);
    for (uint32_t i = 0; i < n; ++i) {
        ModuleImportInfo imp;
        imp.path = r.str(); imp.plugin = r.u32() != 0; imp.line = (int)r.u32();
        m.imports.push_back(std::move(imp));
    }
    n = r.count(16);
    for (uint32_t i = 0; i < n; ++i) {
        ModuleFunctionInfo f;
        f.name = r.str(); f.signature = r.str(); f.doc = r.str(); f.line = (int)r.u32();
        m.functions.push_back(std::move(f));
    }
    n = r.count(20);
    for (uint32_t i = 0; i < n; ++i) {
        ModuleProblem p;
        p.text = r.str();
        p.range.startLine = (int)r.u32(); p.range.startCol = (int)r.u32(); p.range.endLine = (int)r.u32(); p.range.endCol = (int)r.u32();
        m.problems.push_back(std::move(p));
    }
    return r.ok && r.pos == blob.size();
}

// what `embr compile` writes into the file: the description of the source, found without running or looking anything up
inline std::string moduleMetaBlob(const std::string& source, const std::string& path) {
    ModuleInfo info = analyzeSource(source, path);
    return info.parsed ? encodeModuleInfo(info) : std::string();
}

inline std::optional<std::string> readBinaryFile(const std::filesystem::path& file) {
    std::ifstream f(file, std::ios::binary);
    if (!f) return std::nullopt;
    return std::string((std::istreambuf_iterator<char>(f)), {});
}

// a compiled module's description from its bytes. the capabilities are worked out from the imports, as for a script
inline ModuleInfo analyzeCompiledBytes(const std::string& bytes, const std::string& path, const PluginPolicy& policy,
                                       std::set<std::string>& visiting, int depth) {
    namespace fs = std::filesystem;
    ModuleInfo info;
    info.path = path;
    info.name = fs::path(path).stem().string();
    info.compiled = true;
    std::string blob;
    if (!vmbr::readMeta(bytes, blob)) { info.problems.push_back({"not a compiled module this embr can read", {}}); return info; }
    info.parsed = true;
    if (blob.empty()) { info.noMetadata = true; return info; }
    ModuleInfo d;
    if (!decodeModuleInfo(blob, d)) { info.problems.push_back({"the description inside the compiled module is damaged", {}}); info.parsed = false; return info; }
    d.path = path;
    d.parsed = true;
    d.compiled = true;
    if (d.name.empty()) d.name = info.name;
    const std::string canon = canonicalOrSelf(fs::path(path));
    if (!visiting.insert(canon).second) return d;
    resolveModule(d, policy, visiting, depth);
    visiting.erase(canon);
    return d;
}

inline ModuleInfo analyzeCompiledFile(const std::filesystem::path& file, const PluginPolicy& policy, std::set<std::string>& visiting, int depth) {
    auto bytes = readBinaryFile(file);
    if (!bytes) {
        ModuleInfo info;
        info.path = file.string();
        info.name = file.stem().string();
        info.compiled = true;
        info.problems.push_back({"cannot open the file", {}});
        return info;
    }
    return analyzeCompiledBytes(*bytes, file.string(), policy, visiting, depth);
}

// the modules found in the places `import` looks (next to embr, its modules/ folder, lib/embr, EMBR_PATH, and ./modules where
// `embr pkg` installs), one per file name, sorted by name. where a name is in two places the first one wins
inline std::vector<ModuleInfo> installedModules(const PluginPolicy& policy) {
    namespace fs = std::filesystem;
    std::vector<ModuleInfo> out;
    std::set<std::string> seenFiles;
    std::vector<fs::path> dirs;
    for (const auto& d : extraDirs()) { dirs.push_back(d); dirs.push_back(d / "modules"); }
    dirs.push_back(fs::current_path() / "modules");
    for (const auto& d : dirs) {
        std::error_code ec;
        if (!fs::is_directory(d, ec)) continue;
        std::vector<fs::path> files;
        for (const auto& e : fs::directory_iterator(d, ec))
            if (e.is_regular_file(ec) && (e.path().extension() == ".embr" || e.path().extension() == ".vmbr")) files.push_back(e.path());
        std::sort(files.begin(), files.end());
        for (const auto& f : files) {
            if (!seenFiles.insert(f.filename().string()).second) continue;
            std::set<std::string> visiting;
            out.push_back(analyzeModuleFile(f, policy, visiting));
        }
    }
    std::sort(out.begin(), out.end(), [](const ModuleInfo& a, const ModuleInfo& b) { return a.name < b.name; });
    return out;
}

// a module by the name `import` would use, or by path. the file is not run
inline bool findModuleFile(const std::string& nameOrPath, const std::string& baseDir, std::filesystem::path& out) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (fs::is_regular_file(nameOrPath, ec)) { out = nameOrPath; return true; }
    fs::path found = findExistingCandidate(fileCandidates(nameOrPath, baseDir, ".embr", "modules"));
    if (found.empty()) found = findExistingCandidate(fileCandidates(nameOrPath, baseDir, ".vmbr", "modules"));
    if (found.empty()) return false;
    out = found;
    return true;
}

// the decision, from an analysed module. "" lets it run
inline std::string refuseModule(const Interpreter& interp, const ModuleInfo& info, const std::filesystem::path& file, bool declares) {
    if (!info.parsed || info.noMetadata) return "";    // a syntax error shows when it is compiled, with the usual message and source line
    if (declares && !info.problems.empty())
        return "module \"" + info.name + "\" (" + file.string() + ", line " + std::to_string(info.problems[0].range.startLine) + "): " + info.problems[0].text;
    uint64_t hit = info.effectiveCaps() & interp.pluginPolicy.deniedCaps;
    if (hit) return "module \"" + info.name + "\" is not allowed in this sandbox: it needs " + capListText(hit);
    return "";
}

// before a module runs (both import paths call this): a declaration that is wrong, or a module that needs a capability the host
// denies, is refused with a message. "" lets it run. a module that declares nothing and whose host denies nothing costs nothing
// declaredOnly is for the script you run: one that says nothing about itself may import a plugin inside a `try`, to see what is
// allowed, so only one that declares `__module__` is refused up front
inline std::string checkModuleBeforeRun(const Interpreter& interp, const std::filesystem::path& file, const std::string& source,
                                        bool declaredOnly = false) {
    const bool declares = source.find(kModuleMetaName) != std::string::npos;
    if (!declares && (declaredOnly || !interp.pluginPolicy.deniedCaps)) return "";
    return refuseModule(interp, analyzeModule(source, file.string(), interp.pluginPolicy), file, declares);
}

// the same for a compiled module (the bytes of a .vmbr). one written without a description runs: the plugin import is still the boundary
inline std::string checkCompiledBeforeRun(const Interpreter& interp, const std::filesystem::path& file, const std::string& bytes,
                                          bool declaredOnly = false) {
    std::set<std::string> visiting;
    ModuleInfo info = analyzeCompiledBytes(bytes, file.string(), interp.pluginPolicy, visiting, 0);
    if (declaredOnly && !info.declared) return "";
    return refuseModule(interp, info, file, info.declared);
}

// ---- output ----

inline Value moduleValue(const ModuleInfo& m, bool withFunctions) {
    Value::map_type out;
    auto names = [](uint64_t mask) {
        Value::array_type a;
        for (const auto& n : capNames(mask)) a.push_back(Value(n));
        return Value(std::move(a));
    };
    out["name"] = Value(m.name);
    out["version"] = Value(m.version);
    out["description"] = Value(m.description);
    out["path"] = Value(m.path);
    out["declared"] = Value(m.declared ? (int64_t)1 : (int64_t)0);
    out["dynamic_imports"] = Value(m.dynamicImports ? (int64_t)1 : (int64_t)0);
    out["compiled"] = Value(m.compiled ? (int64_t)1 : (int64_t)0);
    out["has_description"] = Value(!m.noMetadata);
    out["capabilities"] = names(m.effectiveCaps());
    out["declared_capabilities"] = names(m.declaredCaps);
    out["inferred_capabilities"] = names(m.inferredCaps);
    Value::array_type problems;
    for (const auto& p : m.problems) problems.push_back(Value(p.text));
    out["problems"] = Value(std::move(problems));
    Value::array_type imports;
    for (const auto& i : m.imports) {
        Value::map_type im;
        im["path"] = Value(i.path);
        im["kind"] = Value(std::string(i.plugin ? "plugin" : "module"));
        im["found"] = Value(i.found ? (int64_t)1 : (int64_t)0);
        im["capabilities"] = names(i.caps);
        imports.push_back(Value(std::move(im)));
    }
    out["imports"] = Value(std::move(imports));
    if (withFunctions) {
        Value::array_type fns;
        for (const auto& f : m.functions) {
            Value::map_type fm;
            fm["name"] = Value(f.name);
            fm["signature"] = Value(f.signature);
            fm["doc"] = Value(f.doc);
            fns.push_back(Value(std::move(fm)));
        }
        out["functions"] = Value(std::move(fns));
    }
    return Value(std::move(out));
}

inline std::string moduleJson(const ModuleInfo& m, bool withFunctions) {
    auto arr = [](uint64_t mask) {
        std::string s = "[";
        bool first = true;
        for (const auto& n : capNames(mask)) { s += (first ? "" : ", ") + jsonQuote(n); first = false; }
        return s + "]";
    };
    std::string s = "{\"name\": " + jsonQuote(m.name) + ", \"version\": " + jsonQuote(m.version) + ", \"description\": " + jsonQuote(m.description);
    s += ", \"path\": " + jsonQuote(m.path) + ", \"parsed\": " + (m.parsed ? "true" : "false") + ", \"declared\": " + (m.declared ? "true" : "false");
    s += std::string(", \"dynamic_imports\": ") + (m.dynamicImports ? "true" : "false") + ", \"compiled\": " + (m.compiled ? "true" : "false") +
         ", \"has_description\": " + (m.noMetadata ? "false" : "true");
    s += ", \"capabilities\": " + arr(m.effectiveCaps()) + ", \"declared_capabilities\": " + arr(m.declaredCaps) + ", \"inferred_capabilities\": " + arr(m.inferredCaps);
    s += ", \"problems\": [";
    for (size_t i = 0; i < m.problems.size(); ++i) s += (i ? ", " : "") + jsonQuote(m.problems[i].text);
    s += "], \"imports\": [";
    for (size_t i = 0; i < m.imports.size(); ++i)
        s += std::string(i ? ", " : "") + "{\"path\": " + jsonQuote(m.imports[i].path) + ", \"kind\": " + jsonQuote(m.imports[i].plugin ? "plugin" : "module") +
             ", \"found\": " + (m.imports[i].found ? "true" : "false") + ", \"capabilities\": " + arr(m.imports[i].caps) + "}";
    s += "]";
    if (withFunctions) {
        s += ", \"functions\": [";
        for (size_t i = 0; i < m.functions.size(); ++i)
            s += std::string(i ? ", " : "") + "{\"name\": " + jsonQuote(m.functions[i].name) + ", \"signature\": " + jsonQuote(m.functions[i].signature) +
                 ", \"doc\": " + jsonQuote(m.functions[i].doc) + ", \"line\": " + std::to_string(m.functions[i].line) + "}";
        s += "]";
    }
    return s + "}";
}

inline std::string moduleText(const ModuleInfo& m) {
    std::string s = m.name + " " + (m.version.empty() ? "(no version)" : m.version) + "  " + m.description + "\n";
    if (m.noMetadata) return s + "  a compiled module written without a description: nothing is known about it\n  file: " + m.path + "\n";
    s += std::string("  ") + (m.compiled ? "compiled module, " : "") + (m.declared ? "declares __module__" : "no __module__ (everything below is inferred)") + "\n";
    s += "  needs: " + capListText(m.effectiveCaps());
    if (m.declared && m.declaredCaps != m.inferredCaps) s += "   (declared: " + capListText(m.declaredCaps) + ", from its imports: " + capListText(m.inferredCaps) + ")";
    s += "\n";
    if (m.dynamicImports) s += "  has an import or load_module with a computed path, which cannot be followed: declare its capabilities\n";
    s += "  file: " + m.path + "\n";
    if (!m.imports.empty()) {
        s += "  imports:\n";
        for (const auto& i : m.imports)
            s += "    " + i.path + (i.plugin ? " (plugin)" : " (module)") + (i.found ? "  needs " + capListText(i.caps) : "  not found") + "\n";
    }
    for (const auto& p : m.problems) s += "  PROBLEM line " + std::to_string(p.range.startLine) + ": " + p.text + "\n";
    s += "\nfunctions\n";
    for (const auto& f : m.functions) {
        s += "  " + f.signature.substr(f.signature.find(' ') + 1) + "\n";
        if (!f.doc.empty()) s += "      " + f.doc + "\n";
    }
    return s;
}

} // namespace embr

#endif // EMBR_CORE_MODINFO_H
