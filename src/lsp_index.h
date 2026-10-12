// lsp_index.h
// what the language server knows about one file: every name it makes, where, and every place a name is used.
// lsp.h answers go-to-definition, hover, completion and the outline from it.
//
// good to know
//   - positions here are embr's: 1-based lines and byte columns. lsp.h converts to and from the protocol's UTF-16 columns
//   - the scoping follows the language (see the wiki, scoping-and-closures): a plain `x = 1` inside a function assigns a
//     name an enclosing function or the top level already has, and otherwise makes a name local to that function
//   - nothing is run. plugins are loaded only to read their function list, and only from the trusted places
//     (next to the embr binary and EMBR_PATH), the same rule as `--sandbox`

#ifndef EMBR_LSP_INDEX_H
#define EMBR_LSP_INDEX_H

#include <embr/embr.h>

#include <climits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace embr_lsp {

using embr::SourceRange;

// the way a script spells a type, a parameter and a signature (core/describe.h, shared with the CLI and embrmeta)
using embr::typeText;
using embr::paramText;
using embr::signatureText;

// (line, col) order. a position is inside a range when start <= pos <= end
inline bool before(int l1, int c1, int l2, int c2) { return l1 < l2 || (l1 == l2 && c1 < c2); }
inline bool contains(const SourceRange& r, int line, int col) {
    return !before(line, col, r.startLine, r.startCol) && !before(r.endLine, r.endCol, line, col);
}

struct Symbol {
    enum class Kind { Function, Variable, Constant, Parameter };
    std::string name;
    Kind        kind = Kind::Variable;
    SourceRange at;          // the name, where it is made
    SourceRange whole;       // the whole statement (all of a function), for the outline
    SourceRange scope;       // where the name can be seen
    bool        topLevel = false;
    int         parent = -1; // the function it is made in, or -1
    std::string detail;      // what hover shows: a signature, or the line that makes it
};

// one place a name is written: read, assigned or made. `symbol` is what it means, -1 when it is not made in this file
struct Use {
    std::string name;
    SourceRange at;
    int         symbol = -1;
};

struct Index {
    std::vector<Symbol>      symbols;
    std::vector<Use>         uses;
    std::vector<std::string> plugins;   // `import "json"`: names of native plugins (a literal path only)
    std::vector<std::string> modules;   // `import "util.embr"`

    // the use under (line, col), or null. the column just past a name counts too, since that is where the cursor sits after typing it
    const Use* useAt(int line, int col) const {
        const Use* edge = nullptr;
        for (const auto& u : uses) {
            if (u.at.startLine != line || col < u.at.startCol || col > u.at.endCol) continue;
            if (col < u.at.endCol) return &u;
            edge = &u;
        }
        return edge;
    }

    // every symbol whose name can be used at (line, col). a function, a top-level name or a parameter is seen in all
    // of its scope, a variable only after the line that makes it
    std::vector<const Symbol*> visibleAt(int line, int col) const {
        std::vector<const Symbol*> out;
        for (const auto& s : symbols) {
            if (!contains(s.scope, line, col)) continue;
            bool everywhere = s.kind == Symbol::Kind::Function || s.kind == Symbol::Kind::Parameter || s.topLevel;
            if (everywhere || before(s.at.startLine, s.at.startCol, line, col)) out.push_back(&s);
        }
        return out;
    }
};

// walks a parsed file and fills an Index
class IndexBuilder {
public:
    IndexBuilder(Index& ix, const std::string& text) : ix_(ix), text_(text) {}

    void build(const std::vector<embr::StmtPtr>& prog) {
        SourceRange file(1, 1, INT_MAX, INT_MAX);
        frames_.push_back({{Block{{}, file}}, -1, file});
        block(prog);
        frames_.clear();
        // a read before the name is made (a function calling one defined further down) is left at -1 by the walk
        for (auto& u : ix_.uses) {
            if (u.symbol >= 0) continue;
            for (size_t i = 0; i < ix_.symbols.size(); ++i)
                if (ix_.symbols[i].topLevel && ix_.symbols[i].name == u.name) { u.symbol = (int)i; break; }
        }
    }

private:
    struct Block { std::map<std::string, int> names; SourceRange range; };
    // one per function body (and one for the file): its blocks, innermost last
    struct Frame { std::vector<Block> blocks; int fnSymbol; SourceRange range; };

    Index&             ix_;
    const std::string& text_;
    std::vector<Frame> frames_;

    using K = Symbol::Kind;

    bool atTop() const { return frames_.size() == 1; }

    std::string lineText(int line) const {
        size_t start = 0;
        for (int l = 1; l < line; ++l) {
            size_t nl = text_.find('\n', start);
            if (nl == std::string::npos) return "";
            start = nl + 1;
        }
        size_t end = text_.find('\n', start);
        std::string s = text_.substr(start, end == std::string::npos ? std::string::npos : end - start);
        size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
        s = a == std::string::npos ? "" : s.substr(a, b - a + 1);
        if (s.size() > 160) s = s.substr(0, 157) + "...";
        return s;
    }

    int define(const std::string& name, K kind, SourceRange at, SourceRange whole, Block& into, std::string detail) {
        Symbol s;
        s.name = name; s.kind = kind; s.at = at; s.whole = whole.valid() ? whole : at;
        s.scope = into.range; s.topLevel = atTop() && &into == &frames_[0].blocks[0];
        s.parent = frames_.back().fnSymbol; s.detail = std::move(detail);
        ix_.symbols.push_back(std::move(s));
        int idx = (int)ix_.symbols.size() - 1;
        into.names[name] = idx;
        ix_.uses.push_back({name, at, idx});
        return idx;
    }

    static int findIn(const Frame& f, const std::string& name) {
        for (auto b = f.blocks.rbegin(); b != f.blocks.rend(); ++b) {
            auto it = b->names.find(name);
            if (it != b->names.end()) return it->second;
        }
        return -1;
    }

    // what a read of `name` means here: the innermost name in any enclosing scope, the file's included
    int lookup(const std::string& name) const {
        for (auto f = frames_.rbegin(); f != frames_.rend(); ++f) {
            int s = findIn(*f, name);
            if (s >= 0) return s;
        }
        return -1;
    }

    // `name = value`: assigns a name this function, an enclosing one (a closure) or the top level already has, else makes a
    // new one for the whole function. at top level it is a file-wide name
    int assign(const std::string& name, SourceRange at, SourceRange whole, K kind, std::string detail) {
        for (size_t i = frames_.size(); i-- > 0; ) {
            int s = findIn(frames_[i], name);
            if (s >= 0) { ix_.uses.push_back({name, at, s}); return s; }
        }
        return define(name, kind, at, whole, frames_.back().blocks.front(), std::move(detail));
    }

    void pushBlock(SourceRange r) { frames_.back().blocks.push_back({{}, r}); }
    void popBlock() { frames_.back().blocks.pop_back(); }

    void block(const std::vector<embr::StmtPtr>& stmts) {
        for (const auto& s : stmts) if (s) stmt(*s);
    }

    void function(int sym, SourceRange range, const std::vector<embr::Param>& params,
                  const std::vector<SourceRange>& paramRanges, const std::vector<embr::StmtPtr>& body) {
        frames_.push_back({{Block{{}, range}}, sym, range});
        for (size_t i = 0; i < params.size() && i < paramRanges.size(); ++i)
            define(params[i].name, K::Parameter, paramRanges[i], {}, frames_.back().blocks.front(),
                   "(parameter) " + paramText(params[i]));
        block(body);
        frames_.pop_back();
    }

    void stmt(const embr::Stmt& st) {
        using SK = embr::Stmt::Kind;
        switch (st.kind) {
        case SK::Assign: {
            auto& a = static_cast<const embr::AssignStmt&>(st);
            if (a.value) expr(a.value.get());
            if (a.target && a.target->kind == embr::Expr::Kind::Var) {
                auto* v = static_cast<const embr::VarExpr*>(a.target.get());
                if (a.value && a.value->kind == embr::Expr::Kind::Lambda) {
                    auto* lam = static_cast<const embr::LambdaExpr*>(a.value.get());
                    assign(v->name, v->range, st.range, K::Function,
                           signatureText(v->name, lam->params, lam->returnType, lam->returnElem));
                } else {
                    assign(v->name, v->range, st.range, a.isConst ? K::Constant : K::Variable, lineText(st.range.startLine));
                }
            } else if (a.target) {
                expr(a.target.get());
            }
            break;
        }
        case SK::MultiAssign: {
            auto& m = static_cast<const embr::MultiAssignStmt&>(st);
            if (m.value) expr(m.value.get());
            for (size_t i = 0; i < m.names.size() && i < m.nameRanges.size(); ++i)
                assign(m.names[i], m.nameRanges[i], st.range, K::Variable, lineText(st.range.startLine));
            break;
        }
        case SK::Local: {
            auto& l = static_cast<const embr::LocalStmt&>(st);
            if (l.value) expr(l.value.get());
            for (const auto& t : l.targets)
                define(t.name, K::Variable, t.range, st.range, frames_.back().blocks.back(), lineText(st.range.startLine));
            break;
        }
        case SK::If: {
            auto& i = static_cast<const embr::IfStmt&>(st);
            if (i.cond) expr(i.cond.get());
            pushBlock(st.range); block(i.thenBlock); popBlock();
            pushBlock(st.range); block(i.elseBlock); popBlock();
            break;
        }
        case SK::Fn: {
            auto& f = static_cast<const embr::FnStmt&>(st);
            int sym = assign(f.name, f.nameRange, st.range, K::Function,
                             signatureText(f.name, f.params, f.returnType, f.returnElem));
            function(sym, st.range, f.params, f.paramRanges, f.body);
            break;
        }
        case SK::Return: {
            auto& r = static_cast<const embr::ReturnStmt&>(st);
            if (r.value) expr(r.value.get());
            break;
        }
        case SK::While: {
            auto& w = static_cast<const embr::WhileStmt&>(st);
            if (w.cond) expr(w.cond.get());
            pushBlock(st.range); block(w.body); popBlock();
            break;
        }
        case SK::For: {
            auto& f = static_cast<const embr::ForStmt&>(st);
            if (f.iterable) expr(f.iterable.get());
            pushBlock(st.range);
            if (!f.keyName.empty()) define(f.keyName, K::Variable, f.keyRange, st.range, frames_.back().blocks.back(), lineText(st.range.startLine));
            define(f.varName, K::Variable, f.varRange, st.range, frames_.back().blocks.back(), lineText(st.range.startLine));
            block(f.body);
            popBlock();
            break;
        }
        case SK::Try: {
            auto& t = static_cast<const embr::TryStmt&>(st);
            pushBlock(st.range); block(t.tryBlock); popBlock();
            pushBlock(st.range);
            define(t.catchVar, K::Variable, t.catchRange, st.range, frames_.back().blocks.back(), "catch " + t.catchVar);
            block(t.catchBlock);
            popBlock();
            break;
        }
        case SK::Import: {
            auto& im = static_cast<const embr::ImportStmt&>(st);
            if (!im.pathExpr) break;
            if (im.pathExpr->kind == embr::Expr::Kind::String) {
                const std::string& p = static_cast<const embr::StringExpr*>(im.pathExpr.get())->v;
                if (p.size() > 5 && p.compare(p.size() - 5, 5, ".embr") == 0) ix_.modules.push_back(p);
                else ix_.plugins.push_back(p);
            } else {
                expr(im.pathExpr.get());
            }
            break;
        }
        case SK::Expr: {
            auto& e = static_cast<const embr::ExprStmt&>(st);
            if (e.expr) expr(e.expr.get());
            break;
        }
        case SK::Break: case SK::Continue: break;
        }
    }

    void expr(const embr::Expr* e) {
        using EK = embr::Expr::Kind;
        if (!e) return;
        switch (e->kind) {
        case EK::Var: {
            auto* v = static_cast<const embr::VarExpr*>(e);
            ix_.uses.push_back({v->name, v->range, lookup(v->name)});
            break;
        }
        case EK::Unary:  expr(static_cast<const embr::UnaryExpr*>(e)->right.get()); break;
        case EK::Binary: { auto* b = static_cast<const embr::BinaryExpr*>(e); expr(b->left.get()); expr(b->right.get()); break; }
        case EK::And: case EK::Or: { auto* l = static_cast<const embr::LogicExpr*>(e); expr(l->left.get()); expr(l->right.get()); break; }
        case EK::Call: {
            auto* c = static_cast<const embr::CallExpr*>(e);
            expr(c->callee.get());
            for (const auto& a : c->args) expr(a.get());
            break;
        }
        case EK::Array: for (const auto& x : static_cast<const embr::ArrayExpr*>(e)->elements) expr(x.get()); break;
        case EK::Map:   for (const auto& kv : static_cast<const embr::MapExpr*>(e)->entries) { expr(kv.first.get()); expr(kv.second.get()); } break;
        case EK::Index: { auto* i = static_cast<const embr::IndexExpr*>(e); expr(i->object.get()); expr(i->index.get()); break; }
        case EK::Ref:   expr(static_cast<const embr::RefExpr*>(e)->target.get()); break;
        case EK::Lambda: {
            auto* lam = static_cast<const embr::LambdaExpr*>(e);
            const auto& body = !lam->ownedBody.empty() || !lam->body ? lam->ownedBody : *lam->body;
            function(frames_.back().fnSymbol, e->range, lam->params, lam->paramRanges, body);
            break;
        }
        case EK::Number: case EK::Int: case EK::String: break;
        }
    }
};

// parses text and indexes it. false when the lexer gives up (the file can't be read at all); `clean` says the parse had no errors
inline bool buildIndex(const std::string& text, Index& out, bool* clean = nullptr) {
    try {
        embr::Lexer lex(text);
        auto tokens = lex.tokenize();
        embr::SourceMap sm = lex.sourceMap();
        embr::Parser parser(std::move(tokens), sm);
        std::vector<embr::EmbrError> errors;
        parser.collectErrors(&errors);
        auto prog = parser.parse();
        Index ix;
        IndexBuilder(ix, text).build(prog);
        out = std::move(ix);
        if (clean) *clean = errors.empty();
        return true;
    } catch (const embr::EmbrError&) {
        return false;
    }
}

// a function made in C++: a builtin every script has, or one a plugin adds
struct NativeInfo {
    std::string name;
    std::string signature;
    std::string from;      // "builtin" or "plugin json"
    std::string doc;       // what the plugin says about it, "" for a builtin or an undocumented function
};

// the natives a script can call: the builtins, plus what each imported plugin adds. a plugin is loaded once, into an
// interpreter that never runs anything, and only from the trusted places (a restricted PluginPolicy, see the file header)
class NativeCatalog {
public:
    NativeCatalog() {
        interp_.pluginPolicy.restricted = true;
        interp_.pluginPolicy.fileIo = false;
        builtins_ = newNatives("builtin");
    }

    const std::vector<NativeInfo>& builtins() const { return builtins_; }

    // what `import "name"` adds. empty when it is not a bare name or can't be found
    const std::vector<NativeInfo>& plugin(const std::string& name) {
        auto it = plugins_.find(name);
        if (it != plugins_.end()) return it->second;
        std::vector<NativeInfo> found;
        interp_.pluginPolicy.allowed.insert(name);
        try {
            if (embr::importNativePlugin(interp_, name, /*runModules=*/false).empty()) {
                found = newNatives("plugin " + name);
                if (!interp_.plugins.empty()) {   // the doc line each function has in the plugin's own description
                    std::map<std::string, std::string> docs;
                    for (const auto& f : embr::pluginFunctions(interp_, interp_.plugins.back())) docs[f.name] = f.doc;
                    for (auto& n : found) { auto d = docs.find(n.name); if (d != docs.end()) n.doc = d->second; }
                }
            }
        } catch (...) {}
        return plugins_[name] = std::move(found);
    }

private:
    embr::Interpreter interp_;
    std::set<std::string> seen_;
    std::vector<NativeInfo> builtins_;
    std::map<std::string, std::vector<NativeInfo>> plugins_;

    // the natives bound since the last call
    std::vector<NativeInfo> newNatives(const std::string& from) {
        std::vector<NativeInfo> out;
        for (const auto& [name, v] : interp_.globals()) {
            if (seen_.count(name) || v.tag() != embr::TypeTag::Callable) continue;
            seen_.insert(name);
            const auto& c = v.asCallable();
            if (!c.isNative()) continue;
            out.push_back({name, signatureText(name, c.sig, c.returns, c.returnsElem), from, ""});
        }
        return out;
    }
};

} // namespace embr_lsp

#endif // EMBR_LSP_INDEX_H
