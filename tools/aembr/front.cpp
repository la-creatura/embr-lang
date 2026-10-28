// front.cpp
// lex, parse and type check one file for aembr. see front.h.

#include "front.h"


#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace aembr {

namespace {

bool readText(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

// where `import "raw"` looks for a script module: next to the file that imports it, then each EMBR_PATH folder, then the current folder
// (an absolute path is only itself). the first that exists wins, as in the VM
std::vector<std::string> moduleCandidates(const std::string& raw, const std::string& dir) {
    std::vector<std::string> c;
    if (!raw.empty() && raw[0] == '/') { c.push_back(raw); return c; }
    c.push_back((dir.empty() ? "." : dir) + "/" + raw);
    if (const char* e = std::getenv("EMBR_PATH")) {
        std::string s = e;
        size_t i = 0;
        while (i <= s.size()) {
            size_t j = s.find(':', i);
            if (j == std::string::npos) j = s.size();
            if (j > i) c.push_back(s.substr(i, j - i) + "/" + raw);
            i = j + 1;
        }
    }
    c.push_back("./" + raw);
    return c;
}

bool isScriptModule(const std::string& raw) {
    auto ends = [&](const char* x) { const size_t n = strlen(x); return raw.size() >= n && raw.compare(raw.size() - n, n, x) == 0; };
    return ends(".embr") || ends(".aembr");
}

// the script modules of a program. each is read, its own imports are loaded first, and it is type checked on its own, as the VM checks a module
// when it is imported (its names are its own, the importer's variables are not visible to it). the checker of a file asks `exportsFor` what an
// `import "x.embr"` brings in. the statements are put in place of the imports afterwards, see `splice`
struct Loader {
    struct Mod {
        std::string                path;
        std::vector<embr::StmtPtr> stmts;       // with its imports still in, until splice() runs
        embr::typecheck::ModuleExports exports;
    };
    Front&                             f;
    std::map<std::string, std::unique_ptr<Mod>> byCanon;
    std::set<std::string>              loading;
    std::string                        err;

    explicit Loader(Front& front) : f(front) {}

    static bool literalModule(const embr::Stmt* s, std::string& raw) {
        if (!s || s->kind != embr::Stmt::Kind::Import) return false;
        const auto* imp = static_cast<const embr::ImportStmt*>(s);
        if (!imp->pathExpr || imp->pathExpr->kind != embr::Expr::Kind::String) return false;
        raw = static_cast<const embr::StringExpr*>(imp->pathExpr.get())->v;
        return isScriptModule(raw);
    }
    // the file `raw` means from a file in `dir`: where it was found, its text and its canonical name. false: not found
    bool find(const std::string& raw, const std::string& dir, std::string& found, std::string& text, std::string& canon, std::vector<std::string>* tried = nullptr) {
        const std::vector<std::string> cands = moduleCandidates(raw, dir);
        if (tried) *tried = cands;
        for (const std::string& c : cands) if (readText(c, text)) { found = c; break; }
        if (found.empty()) return false;
        std::error_code ec;
        canon = std::filesystem::weakly_canonical(found, ec).string();
        if (ec) canon = found;
        return true;
    }
    const embr::typecheck::ModuleExports* exportsFor(const std::string& raw, const std::string& dir) {
        std::string found, text, canon;
        if (!find(raw, dir, found, text, canon)) return nullptr;
        auto it = byCanon.find(canon);
        return it == byCanon.end() ? nullptr : &it->second->exports;
    }
    bool check(std::vector<embr::StmtPtr>& stmts, const embr::SourceMap& sm, const std::string& dir, embr::typecheck::ModuleExports* exportsOut) {
        embr::typecheck::Options opt;
        opt.closedWorld = false;
        opt.isModule = true;
        opt.source = &sm;
        opt.typesOut = &f.types;
        opt.exportsOut = exportsOut;
        opt.importModule = [this, dir](const std::string& raw) { return exportsFor(raw, dir); };
        auto errors = embr::typecheck::check(stmts, opt);
        for (auto& e : errors) err += e.what();
        return errors.empty();
    }
    // loads, checks and keeps every module the top level of `stmts` imports (and what they import)
    bool loadImports(const std::vector<embr::StmtPtr>& stmts, const std::string& dir, int depth) {
        for (const embr::StmtPtr& s : stmts) {
            std::string raw;
            if (!literalModule(s.get(), raw)) continue;
            std::string found, text, canon;
            std::vector<std::string> tried;
            if (!find(raw, dir, found, text, canon, &tried)) {
                err += "aembr: cannot find the module \"" + raw + "\", tried:";
                for (const std::string& c : tried) err += "\n    " + c;
                err += "\n";
                return false;
            }
            if (byCanon.count(canon) || loading.count(canon)) continue;
            if (depth > 32) { err += "aembr: modules import each other too deeply (" + raw + ")\n"; return false; }
            loading.insert(canon);
            f.modules.push_back(found);
            auto mod = std::make_unique<Mod>();
            mod->path = found;
            try {
                embr::Lexer lex(text);
                auto toks = lex.tokenize();
                embr::SourceMap sm = lex.sourceMap();
                const std::string mdir = embr::dirOfFile(found);
                sm.setBaseDir(mdir);
                embr::Parser parser(std::move(toks), sm);
                std::vector<embr::EmbrError> perrs;
                parser.collectErrors(&perrs);
                mod->stmts = parser.parse();
                if (!perrs.empty()) { for (auto& e : perrs) err += found + ": " + e.what(); return false; }
                if (!loadImports(mod->stmts, mdir, depth + 1)) return false;
                if (!check(mod->stmts, sm, mdir, &mod->exports)) return false;
            } catch (const embr::EmbrError& e) {
                err += found + ": " + e.what();
                return false;
            }
            loading.erase(canon);
            byCanon[canon] = std::move(mod);
        }
        return true;
    }
    // puts the statements of each imported module in place of its import (the first time the file is seen), nested ones first. a statement keeps
    // its own line, and `f.stmtFile` says which file it came from
    void splice(std::vector<embr::StmtPtr>& stmts, const std::string& dir, std::set<std::string>& seen) {
        std::vector<embr::StmtPtr> out;
        for (embr::StmtPtr& s : stmts) {
            std::string raw;
            if (!literalModule(s.get(), raw)) { out.push_back(std::move(s)); continue; }
            std::string found, text, canon;
            if (!find(raw, dir, found, text, canon)) continue;
            auto it = byCanon.find(canon);
            if (it == byCanon.end() || !seen.insert(canon).second) continue;     // importing a file again does nothing
            Mod& m = *it->second;
            splice(m.stmts, embr::dirOfFile(m.path), seen);
            for (embr::StmtPtr& t : m.stmts) {
                if (t->kind != embr::Stmt::Kind::Import && f.stmtFile.find(t.get()) == f.stmtFile.end()) f.stmtFile[t.get()] = m.path;
                out.push_back(std::move(t));
            }
        }
        stmts = std::move(out);
    }
};

// `import "str"` brings in std/str.embr, the natives of that plugin written in aembr, before the statements of the file. the statements take the
// range of the import, as `const import` does. a plugin with no file there keeps its natives in the compiler
void spliceStd(Front& f) {
    if (f.stdDir.empty()) return;
    std::set<std::string>& done = f.stdDone;
    std::vector<embr::StmtPtr> add;
    for (const embr::StmtPtr& s : f.ast) {
        if (!s || s->kind != embr::Stmt::Kind::Import) continue;
        const auto* imp = static_cast<const embr::ImportStmt*>(s.get());
        if (!imp->pathExpr || imp->pathExpr->kind != embr::Expr::Kind::String) continue;
        const std::string name = static_cast<const embr::StringExpr*>(imp->pathExpr.get())->v;
        if (!done.insert(name).second) continue;
        std::ifstream in(f.stdDir + "/" + name + ".embr", std::ios::binary);
        if (!in) continue;
        std::stringstream ss;
        ss << in.rdbuf();
        const std::string text = ss.str();      // (the lexer keeps looking at it)
        embr::Lexer lex(text);
        auto toks = lex.tokenize();
        embr::SourceMap sm = lex.sourceMap();
        embr::Parser parser(std::move(toks), sm);
        std::vector<embr::StmtPtr> stmts = parser.parse();
        embr::parser_detail::collapseStmts(stmts, s->range);
        for (embr::StmtPtr& t : stmts) { f.stdStmts.insert(t.get()); add.push_back(std::move(t)); }
    }
    f.ast.insert(f.ast.begin(), std::make_move_iterator(add.begin()), std::make_move_iterator(add.end()));
}

}  // namespace

bool readFrontText(Front& out, std::string& err) {
    try {
        embr::Lexer lex(out.source);
        auto toks = lex.tokenize();
        embr::SourceMap sm = lex.sourceMap();
        sm.setBaseDir(embr::dirOfFile(out.path));
        embr::Parser parser(std::move(toks), sm);
        std::vector<embr::EmbrError> perrs;
        parser.collectErrors(&perrs);
        out.ast = parser.parse();
        if (!perrs.empty()) {
            for (auto& e : perrs) err += e.what();
            return false;
        }
        Loader loader(out);
        {
            std::error_code ec;
            loader.loading.insert(std::filesystem::weakly_canonical(out.path, ec).string());      // (a module that imports the program does not load it again)
        }
        if (!loader.loadImports(out.ast, embr::dirOfFile(out.path), 0)) { err += loader.err; return false; }
        spliceStd(out);
        embr::typecheck::Options opt;
        opt.closedWorld = out.modules.empty();       // (a module may call the program's functions)
        opt.source = &sm;
        opt.typesOut = &out.types;
        opt.importModule = [&loader, &out](const std::string& raw) { return loader.exportsFor(raw, embr::dirOfFile(out.path)); };
        auto errors = embr::typecheck::check(out.ast, opt);
        if (!errors.empty()) {
            for (auto& e : errors) err += e.what();
            return false;
        }
        {
            std::set<std::string> seen;
            seen.insert(*loader.loading.begin());
            loader.splice(out.ast, embr::dirOfFile(out.path), seen);
            spliceStd(out);       // (the plugins the modules import)
        }
    } catch (const embr::EmbrError& e) {
        err += e.what();
        return false;
    }
    return true;
}

bool readFront(const std::string& path, Front& out, std::string& err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { err = "aembr: cannot read " + path + "\n"; return false; }
    std::stringstream ss;
    ss << in.rdbuf();
    out.path = path;
    out.source = ss.str();
    return readFrontText(out, err);
}

}  // namespace aembr
