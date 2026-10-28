#ifndef EMBR_CORE_PARSER_H
#define EMBR_CORE_PARSER_H

#include <algorithm>
#include <initializer_list>
#include "diagnostics.h"
#include "types.h"
#include "token.h"
#include "ast.h"
#include "lexer.h"

#include <vector>
#include <memory>
#include <iostream>
#include <string>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace embr {

namespace parser_detail {

// every node of an included file is given the range of the `const import` that brought it in: the lines of another file mean nothing in
// the including file's text, which is what error messages and line numbers are made from
inline void collapseExpr(Expr* e, const SourceRange& r);
inline void collapseStmts(std::vector<StmtPtr>& v, const SourceRange& r) {
    for (auto& sp : v) {
        Stmt* s = sp.get();
        if (!s) continue;
        s->range = r;
        switch (s->kind) {
        case Stmt::Kind::Assign: { auto* a = static_cast<AssignStmt*>(s); collapseExpr(a->target.get(), r); collapseExpr(a->value.get(), r); break; }
        case Stmt::Kind::MultiAssign: collapseExpr(static_cast<MultiAssignStmt*>(s)->value.get(), r); break;
        case Stmt::Kind::Local: { auto* l = static_cast<LocalStmt*>(s); collapseExpr(l->value.get(), r); for (auto& t : l->targets) t.range = r; break; }
        case Stmt::Kind::If: { auto* i = static_cast<IfStmt*>(s); collapseExpr(i->cond.get(), r); collapseStmts(i->thenBlock, r); collapseStmts(i->elseBlock, r); break; }
        case Stmt::Kind::Fn: { auto* f = static_cast<FnStmt*>(s); f->nameRange = r; for (auto& pr : f->paramRanges) pr = r; collapseStmts(f->body, r); break; }
        case Stmt::Kind::Return: collapseExpr(static_cast<ReturnStmt*>(s)->value.get(), r); break;
        case Stmt::Kind::While: { auto* w = static_cast<WhileStmt*>(s); collapseExpr(w->cond.get(), r); collapseStmts(w->body, r); break; }
        case Stmt::Kind::For: { auto* f = static_cast<ForStmt*>(s); f->keyRange = f->varRange = r; collapseExpr(f->iterable.get(), r); collapseStmts(f->body, r); break; }
        case Stmt::Kind::Import: collapseExpr(static_cast<ImportStmt*>(s)->pathExpr.get(), r); break;
        case Stmt::Kind::Expr: collapseExpr(static_cast<ExprStmt*>(s)->expr.get(), r); break;
        case Stmt::Kind::Try: { auto* t = static_cast<TryStmt*>(s); t->catchRange = r; collapseStmts(t->tryBlock, r); collapseStmts(t->catchBlock, r); break; }
        default: break;
        }
    }
}
inline void collapseExpr(Expr* e, const SourceRange& r) {
    if (!e) return;
    e->range = r;
    switch (e->kind) {
    case Expr::Kind::Unary: collapseExpr(static_cast<UnaryExpr*>(e)->right.get(), r); break;
    case Expr::Kind::Binary: { auto* b = static_cast<BinaryExpr*>(e); collapseExpr(b->left.get(), r); collapseExpr(b->right.get(), r); break; }
    case Expr::Kind::And: case Expr::Kind::Or: { auto* l = static_cast<LogicExpr*>(e); collapseExpr(l->left.get(), r); collapseExpr(l->right.get(), r); break; }
    case Expr::Kind::Call: { auto* c = static_cast<CallExpr*>(e); collapseExpr(c->callee.get(), r); for (auto& a : c->args) collapseExpr(a.get(), r); break; }
    case Expr::Kind::Array: for (auto& x : static_cast<ArrayExpr*>(e)->elements) collapseExpr(x.get(), r); break;
    case Expr::Kind::Map: for (auto& kv : static_cast<MapExpr*>(e)->entries) { collapseExpr(kv.first.get(), r); collapseExpr(kv.second.get(), r); } break;
    case Expr::Kind::Index: { auto* i = static_cast<IndexExpr*>(e); collapseExpr(i->object.get(), r); collapseExpr(i->index.get(), r); break; }
    case Expr::Kind::Ref: collapseExpr(static_cast<RefExpr*>(e)->target.get(), r); break;
    case Expr::Kind::Lambda: { auto* l = static_cast<LambdaExpr*>(e); for (auto& pr : l->paramRanges) pr = r; collapseStmts(l->ownedBody, r); break; }
    default: break;
    }
}

}  // namespace parser_detail

// a number that changes when a file the text reads with `const import` / `const c import` changes (0 when it reads none). for cache keys
inline uint64_t includeDigest(const std::string& src, const std::string& baseDir, int depth = 0) {
    if (depth > 16 || src.find("import") == std::string::npos) return 0;
    uint64_t d = 0;
    try {
        Lexer lex(src);
        auto toks = lex.tokenize();
        for (size_t i = 0; i + 2 < toks.size(); ++i) {
            if (toks[i].type != TokenType::Const) continue;
            size_t j = i + 1;
            bool asC = false;
            if (toks[j].type == TokenType::Identifier && toks[j].text == "c") {
                asC = true; ++j;
                if (j < toks.size() && toks[j].type == TokenType::Identifier && toks[j].text == "file") ++j;
            }
            if (j + 1 >= toks.size() || toks[j].type != TokenType::Import || toks[j + 1].type != TokenType::String) continue;
            std::filesystem::path p(toks[j + 1].text);
            if (p.is_relative() && !baseDir.empty()) p = std::filesystem::path(baseDir) / p;
            std::ifstream in(p, std::ios::binary);
            std::ostringstream ss;
            if (in) ss << in.rdbuf();
            uint64_t h = 1469598103934665603ULL;                      // fnv-1a of the path and the text
            for (unsigned char c : p.string() + "\n" + ss.str()) { h ^= c; h *= 1099511628211ULL; }
            d = d * 31 + h;
            if (!asC) d += includeDigest(ss.str(), p.parent_path().string(), depth + 1);
        }
    } catch (const EmbrError&) {}
    return d;
}

class Parser {
public:
    Parser(std::vector<Token> toks, SourceMap src)
        : toks_(std::move(toks)), src_(std::move(src)) {}

    // by default parse errors are printed to std::cerr as they are found (the parser recovers and keeps
    // going, so one bad statement doesn't hide the rest). a tool that wants them as data, `embr check`,
    // an editor/LSP integration, passes a vector here and they are appended to it instead, nothing printed
    void collectErrors(std::vector<EmbrError>* sink) { errors_ = sink; }

    std::vector<StmtPtr> parse() {
        std::vector<StmtPtr> out;
        while (!check(TokenType::Eof)) {
            try { stmtInto(out); }
            catch (const EmbrError& e) { report(e); loopDepth_ = 0; sync(); }
            catch (const std::exception& e) {
                report(EmbrError(std::string("[parser] ") + e.what() + "\n", false, {}, e.what(), "parser"));
                loopDepth_ = 0; sync();
            }
        }
        return out;
    }

private:
    std::vector<EmbrError>* errors_ = nullptr;     // see collectErrors()
    void report(const EmbrError& e) { if (errors_) errors_->push_back(e); else std::cerr << e.what(); }

    std::vector<Token> toks_;
    SourceMap          src_;
    size_t             i_ = 0;
    bool               hostOk_ = false;   // the type being read may be `host` (see parseTypeSpec)
    int                loopDepth_ = 0; // reset across fn/lambda boundaries so break/continue can't cross them

    // hard limits that turn a native stack overflow (an uncatchable process kill) on pathological input into
    // an ordinary parser error. both are far above what real code needs and far below where an 8 MB stack gives out
    // parse()'s error recovery also resets loopDepth_ to 0, because a statement that throws midway skips the
    // matching restore. without that, a stray `continue` after an error was accepted with no loop to attach to
    static constexpr int kMaxNesting   = 2500;   // simultaneous recursive parse calls (stmt/expr/unary)
    static constexpr int kMaxExprDepth = 4000;   // height of any single expression tree
    int                nest_ = 0;

    struct NestGuard {
        Parser& p;
        explicit NestGuard(Parser& p_) : p(p_) {
            if (++p.nest_ > kMaxNesting) {
                --p.nest_;
                raiseError("parser", "nesting too deep (max " + std::to_string(kMaxNesting) + ")",
                           SourceRange::fromToken(p.peek()), p.src_);
            }
        }
        ~NestGuard() { --p.nest_; }
    };

    // records n's tree height from its children and enforces kMaxExprDepth
    void setDepth(Expr* n, std::initializer_list<const Expr*> kids) {
        int d = 0;
        for (const Expr* k : kids) if (k && k->depth > d) d = k->depth;
        n->depth = d + 1;
        if (n->depth > kMaxExprDepth)
            raiseError("parser", "expression too deeply nested or too long (max depth " +
                       std::to_string(kMaxExprDepth) + ")", n->range, src_);
    }

    Token& peek() { return toks_[i_]; }
    Token& prev() { return toks_[i_-1]; }
    bool match(TokenType t) { if (peek().type==t){++i_;return true;} return false; }
    bool check(TokenType t) const { return toks_[i_].type==t; }
    bool checkAt(size_t off, TokenType t) const {
        return i_+off < toks_.size() && toks_[i_+off].type == t;
    }
    Token consume(TokenType t, const char* msg) {
        if (match(t)) return prev();
        raiseError("parser", msg, SourceRange::fromToken(peek()), src_);
    }
    SourceRange tr(const Token& tok) const { return SourceRange::fromToken(tok); }

    void sync() {
        while (!check(TokenType::Eof)) {
            switch (peek().type) {
                case TokenType::If:     case TokenType::Elif:  case TokenType::Else:
                case TokenType::End:
                case TokenType::Fn:     case TokenType::While: case TokenType::For:
                case TokenType::Break:  case TokenType::Continue:
                case TokenType::Try:    case TokenType::Catch:
                case TokenType::Import:
                case TokenType::Local:  case TokenType::Const: case TokenType::Return: return;
                default: ++i_;
            }
        }
    }

    TypeSet parseTypeName(const Token& t) {
        TypeSet ts = TypeSet::fromName(t.text);
        if (t.text == "auto")
            raiseError("parser", "'auto' was removed: a plain assignment already takes its type from the value, "
                       "write `local x = 5` (or `local int x = 5` to pick the type yourself)", tr(t), src_);
        if (ts.isNone())
            raiseError("parser", "unknown type '" + t.text +
                       "' expected: num, str, arr, map, fn, ptr, any", tr(t), src_);
        return ts;
    }
    // a type is a name, and for arr and map an element type in angle brackets: `arr<int>`, `map<arr<str>>`. the closing `>` must not touch an `=`
    // (`arr<int>= x` lexes as `>=`)
    TypeSet parseTypeSpec(const Token& nameTok, TypeElem* el) {
        const bool hostOk = hostOk_;      // only the type of a parameter or of a result, not inside another type
        hostOk_ = false;
        if (nameTok.type == TokenType::Fn) return parseFnType(nameTok, el);
        if (nameTok.type == TokenType::Identifier && nameTok.text == "host") {
            if (!hostOk) raiseError("parser", "'host' is the type of a parameter or of the result of a function (for a plugin written in aembr), not of a variable or an element", tr(nameTok), src_);
            if (check(TokenType::Less)) raiseError("parser", "'host' takes no element type", tr(peek()), src_);
            auto ann = std::make_shared<TypeAnn>();
            ann->isHost = true;
            if (el) *el = ann;
            return TypeSet::Any();
        }
        TypeSet ts = parseTypeName(nameTok);
        if (!check(TokenType::Less)) return ts;
        if (ts.mask != TS::Arr.mask && ts.mask != TS::Map.mask)
            raiseError("parser", "only arr and map take an element type, like arr<int> or map<str>", tr(peek()), src_);
        ++i_;
        Token inner = consumeTypeStart("expected an element type after '<'");
        TypeElem sub;
        TypeSet innerType = parseTypeSpec(inner, &sub);
        consume(TokenType::Greater, "expected '>' to close the element type");
        if (el) *el = makeAnn(innerType, std::move(sub));
        return ts;
    }
    // `fn(int, str) -> int`, the `fn` already read. the result is optional, and a result that is itself a function type reads to the right:
    // `fn(int) -> fn(int) -> int` is a function that returns a function. only types are written, no parameter names
    TypeSet parseFnType(const Token& fnTok, TypeElem* el) {
        NestGuard guard(*this);
        consume(TokenType::LParen, "expected '(' after 'fn' in a type, like fn(int) -> int");
        auto ann = std::make_shared<TypeAnn>();
        ann->t = TS::Fn;
        ann->isFn = true;
        if (!check(TokenType::RParen)) {
            do {
                Token pt = consumeTypeStart("expected a parameter type (a function type is written with types only, like fn(int, str) -> int)");
                if (check(TokenType::Colon))
                    raiseError("parser", "a function type is written with types only, like fn(int, str) -> int, without parameter names", tr(pt), src_);
                TypeElem sub;
                TypeSet ts = parseTypeSpec(pt, &sub);
                ann->params.push_back(makeAnn(ts, std::move(sub)));
            } while (match(TokenType::Comma));
        }
        consume(TokenType::RParen, "expected ')' to close the parameter types of a function type");
        if (match(TokenType::RArrow)) {
            Token rt = consumeTypeStart("expected a result type after '->'");
            TypeElem sub;
            TypeSet ts = parseTypeSpec(rt, &sub);
            ann->ret = makeAnn(ts, std::move(sub));
        }
        (void)fnTok;
        if (el) *el = ann;
        return TS::Fn;
    }
    // the first token of a type: a name, or `fn` for a function type
    Token consumeTypeStart(const char* msg) {
        if (check(TokenType::Fn)) return toks_[i_++];
        return consume(TokenType::Identifier, msg);
    }
    // a function type written before a name: `local fn(int) -> int add = ...`
    bool fnTypeAhead() { return check(TokenType::Fn) && checkAt(1, TokenType::LParen); }
    // an `arr<` or `map<` ahead: a type with an element type, written before a name in `local arr<int> xs = ...`
    bool genericTypeAhead() {
        if (!check(TokenType::Identifier) || !checkAt(1, TokenType::Less)) return false;
        const std::string& n = peek().text;
        return n == "arr" || n == "array" || n == "map";
    }
    // name : type
    TypeSet parseTypeAnnotation(bool* written = nullptr, TypeElem* el = nullptr) {
        if (written) *written = false;
        if (!match(TokenType::Colon)) return TypeSet::Any();
        if (written) *written = true;
        Token t = consumeTypeStart("expected type name after ':'");
        return parseTypeSpec(t, el);
    }

    // the literal after `=` in `fn f(b: int = 5)`: a number (with a minus) or a string. it makes the parameter optional
    void parseParamDefault(Param& p, const Token& nm) {
        bool neg = false;
        if (match(TokenType::Minus)) neg = true;
        if (check(TokenType::String) && !neg) {
            const Token t = peek(); ++i_;
            p.def.kind = ParamDefault::Str; p.def.s = t.text;
        } else if (check(TokenType::Number)) {
            const Token t = peek(); ++i_;
            if (isRadixLiteral(t.text)) {
                p.def.kind = ParamDefault::Int; p.def.i = neg ? -*intLiteral(t.text) : *intLiteral(t.text);
            } else if (t.text.find_first_of(".eE") == std::string::npos) {
                int64_t v = 0;
                auto r = std::from_chars(t.text.data(), t.text.data() + t.text.size(), v);
                if (r.ec != std::errc()) raiseError("parser", "the default value of '" + nm.text + "' does not fit an int", tr(t), src_);
                p.def.kind = ParamDefault::Int; p.def.i = neg ? -v : v;
            } else {
                double d = 0.0;
                parse_double(t.text.data(), t.text.data() + t.text.size(), d);
                p.def.kind = ParamDefault::Float; p.def.f = neg ? -d : d;
            }
        } else {
            raiseError("parser", "the default value of '" + nm.text + "' has to be a number or a string written out", tr(peek()), src_);
        }
        p.optional = true;
    }

    // caller already eatid (
    // `ranges` (if given) gets where each parameter's name is written
    std::vector<Param> parseParamList(std::vector<SourceRange>* ranges = nullptr) {
        std::vector<Param> params;
        if (!check(TokenType::RParen)) {
            do {
                // ...name[: type]
                // collects every remaining argument into an array bound to name. must be the last parameter
                if (match(TokenType::Ellipsis)) {
                    Token nm = consume(TokenType::Identifier, "expected parameter name after '...'");
                    if (ranges) ranges->push_back(tr(nm));
                    bool written = false;
                    TypeElem pel;
                    TypeSet mask = parseTypeAnnotation(&written, &pel);
                    params.push_back(Param::rest(nm.text, mask));
                    params.back().explicitType = written;
                    params.back().elem = pel;
                    if (check(TokenType::Comma))
                        raiseError("parser", "variadic parameter '...' must be the last parameter",
                                   tr(peek()), src_);
                    break;
                }
                // &name: the function changes the caller's variable (the call passes &name). only aembr (the compiler) runs these
                const bool inout = match(TokenType::Amp);
                Token nm = consume(TokenType::Identifier, "expected parameter name");
                if (ranges) ranges->push_back(tr(nm));
                bool written = false;
                TypeElem pel;
                hostOk_ = true;
                TypeSet mask = parseTypeAnnotation(&written, &pel);
                hostOk_ = false;
                params.push_back(Param::req(nm.text, mask));
                params.back().explicitType = written;
                params.back().elem = pel;
                params.back().inout = inout;
                // name[: type] = literal: the call may leave it out
                if (check(TokenType::Equal)) {
                    if (inout) raiseError("parser", "an in-out parameter cannot have a default value", tr(peek()), src_);
                    ++i_;
                    parseParamDefault(params.back(), nm);
                } else if (!params.back().optional) {
                    for (const Param& before : params)
                        if (before.def.kind != ParamDefault::None)
                            raiseError("parser", "a parameter without a default value cannot follow one with a default value", tr(nm), src_);
                }
            } while (match(TokenType::Comma));
        }
        return params;
    }

    // function -> type
    TypeSet parseReturnAnnotation(TypeElem* el = nullptr) {
        if (!match(TokenType::RArrow)) return TypeSet::Any();
        Token t = consumeTypeStart("expected return type after '->'");
        hostOk_ = true;
        return parseTypeSpec(t, el);
    }

    // a statement goes into `out`. `const import "file"` and `const c import "file"` bring in a whole file, so they can add any number of statements
    void stmtInto(std::vector<StmtPtr>& out) {
        if (check(TokenType::Const) &&
            (checkAt(1, TokenType::Import) || (checkAt(1, TokenType::Identifier) && toks_[i_ + 1].text == "c" &&
             (checkAt(2, TokenType::Import) || (checkAt(2, TokenType::Identifier) && toks_[i_ + 2].text == "file" && checkAt(3, TokenType::Import)))))) {
            includeInto(out);
            return;
        }
        out.push_back(stmt());
    }

    // const import "path"     the statements of the file, parsed here, as if they were written in place
    // const c import "path"   the text of the file as an inline C block (`c """ ... """`)
    // const c file import "path"   the same, for the whole C file instead of a block (`c file """ ... """`)
    // the path is relative to the folder of the file being read. the file is read when the script is read, not when it runs
    void includeInto(std::vector<StmtPtr>& out) {
        NestGuard guard(*this);
        const Token kw = consume(TokenType::Const, "expected 'const'");
        bool asC = false, fileScope = false;
        if (check(TokenType::Identifier) && peek().text == "c") {
            ++i_; asC = true;
            if (check(TokenType::Identifier) && peek().text == "file") { ++i_; fileScope = true; }
        }
        consume(TokenType::Import, "expected 'import' after 'const'");
        if (!check(TokenType::String))
            raiseError("parser", "const import needs the path written as a string, because the file is read before the script runs", tr(peek()), src_);
        const Token pathTok = consume(TokenType::String, "expected a path");
        const SourceRange at = SourceRange::merge(tr(kw), tr(pathTok));
        namespace fs = std::filesystem;
        fs::path p(pathTok.text);
        if (p.is_relative() && !src_.baseDir().empty()) p = fs::path(src_.baseDir()) / p;
        std::string text;
        {
            std::ifstream in(p, std::ios::binary);
            if (!in) raiseError("parser", "const import cannot read the file " + p.string(), at, src_);
            std::ostringstream ss; ss << in.rdbuf(); text = ss.str();
        }
        if (asC) {
            auto s = std::make_unique<ExprStmt>();
            s->range = at;
            s->inlineC = true;
            s->cFileScope = fileScope;
            s->cFile = pathTok.text;
            s->expr = std::make_unique<StringExpr>(std::move(text), at);
            out.push_back(std::move(s));
            return;
        }
        // a file that includes itself, directly or through others, would never end
        std::error_code ec;
        const std::string canon = fs::weakly_canonical(p, ec).string();
        auto& stack = includeStack();
        for (const std::string& f : stack)
            if (f == canon) raiseError("parser", "const import of " + p.string() + " includes itself", at, src_);
        if (stack.size() >= 16) raiseError("parser", "const imports are nested too deeply (16 files)", at, src_);
        struct Pop { std::vector<std::string>& s; ~Pop() { s.pop_back(); } } pop{stack};
        stack.push_back(canon);
        Lexer lex(text);
        auto toks = lex.tokenize();
        SourceMap sm = lex.sourceMap();
        sm.setBaseDir(p.parent_path().string());
        Parser sub(std::move(toks), std::move(sm));
        sub.errors_ = errors_;
        auto stmts = sub.parse();
        parser_detail::collapseStmts(stmts, at);
        for (auto& s : stmts) out.push_back(std::move(s));
    }
    static std::vector<std::string>& includeStack() { static thread_local std::vector<std::string> st; return st; }

    StmtPtr stmt() {
        NestGuard guard(*this);
        if (match(TokenType::If))       return parseIf();
        if (match(TokenType::While))    return parseWhile();
        if (match(TokenType::For))      return parseFor();
        if (match(TokenType::Try))      return parseTry();
        if (match(TokenType::Break))    return parseBreak();
        if (match(TokenType::Continue)) return parseContinue();
        if (match(TokenType::Return))   return parseReturn();
        if (match(TokenType::Import))   return parseImport();
        if (match(TokenType::Local))    return parseLocal();
        if (match(TokenType::Const))    return parseConst();
        if (match(TokenType::Fn))       return parseFnBlock();
        // c """ ... """  and  c file """ ... """  (C text for aembr: in place, or for the whole file)
        if (checkAt(0, TokenType::Identifier) && toks_[i_].text == "c" &&
            (checkAt(1, TokenType::RawString) || (checkAt(1, TokenType::Identifier) && toks_[i_ + 1].text == "file" && checkAt(2, TokenType::RawString)))) {
            const Token& open = toks_[i_];
            const bool fileScope = !checkAt(1, TokenType::RawString);
            const Token& txt = toks_[i_ + (fileScope ? 2 : 1)];
            auto s = std::make_unique<ExprStmt>();
            s->range = SourceRange(open.line, open.col, txt.line, txt.col + 3);
            s->inlineC = true;
            s->cFileScope = fileScope;
            s->expr = std::make_unique<StringExpr>(txt.text, s->range);
            i_ += fileScope ? 3 : 2;
            return s;
        }
        // name(params) = expr
        // IDENT ( [IDENT {, IDENT}] ) = (not ==)
        if (isFnAssignHead()) return parseFnAssign();

        // a, b, c = expr (reassigns existing bare names by unpacking expr)
        if (isMultiAssignHead()) return parseMultiAssign();

        auto lhs = expr();
        if (match(TokenType::Equal)) {
            auto rhs = expr();
            auto s = std::make_unique<AssignStmt>();
            s->range = SourceRange::merge(lhs->range, rhs->range);
            s->target = std::move(lhs); s->value = std::move(rhs);
            return s;
        }

        // name +=, -=, *=, /= expr
        //  \/\/\/
        // name = name +, -, *, / expr
        {
            std::string binOp;
            if      (match(TokenType::PlusEq))  binOp = "+";
            else if (match(TokenType::MinusEq)) binOp = "-";
            else if (match(TokenType::StarEq))  binOp = "*";
            else if (match(TokenType::SlashEq)) binOp = "/";
            if (!binOp.empty()) {
                auto rhs = expr();
                auto bin = std::make_unique<BinaryExpr>();
                bin->op    = binOp;
                bin->range = SourceRange::merge(lhs->range, rhs->range);
                if (lhs->kind == Expr::Kind::Var) {
                    auto* v = static_cast<VarExpr*>(lhs.get());
                    bin->left  = std::make_unique<VarExpr>(v->name, v->range);
                    bin->right = std::move(rhs);
                    auto s = std::make_unique<AssignStmt>();
                    s->range   = bin->range;
                    s->target  = std::move(lhs);
                    s->value   = std::move(bin);
                    return s;
                }
                raiseError("parser", "compound assignment target must be a simple variable",
                           lhs->range, src_);
            }
        }
        auto s = std::make_unique<ExprStmt>();
        s->range = lhs->range; s->expr = std::move(lhs);
        return s;
    }

    // scan tokens without consuming owo to detect fn assign
    bool isFnAssignHead() const {
        if (!checkAt(0, TokenType::Identifier)) return false;
        if (!checkAt(1, TokenType::LParen))     return false;
        size_t j = i_ + 2;

        auto skipParam = [&]() -> bool {
            // ident
            if (j >= toks_.size() || toks_[j].type != TokenType::Identifier) return false;
            ++j;
            // : type
            if (j < toks_.size() && toks_[j].type == TokenType::Colon) {
                ++j; // skip :
                if (j >= toks_.size() || toks_[j].type != TokenType::Identifier) return false;
                ++j; // skip type
            }
            return true;
        };

        // f() = ...
        if (j < toks_.size() && toks_[j].type == TokenType::RParen) {
            ++j;
            return j < toks_.size() && toks_[j].type == TokenType::Equal &&
                   (j+1 >= toks_.size() || toks_[j+1].type != TokenType::Equal);
        }
        // params
        if (!skipParam()) return false;
        while (j < toks_.size() && toks_[j].type == TokenType::Comma) {
            ++j;
            if (!skipParam()) return false;
        }
        if (j >= toks_.size() || toks_[j].type != TokenType::RParen) return false;
        ++j;
        // -> type
        if (j < toks_.size() && toks_[j].type == TokenType::RArrow) {
            ++j; // skip ->
            if (j >= toks_.size() || toks_[j].type != TokenType::Identifier) return false;
            ++j; // skip type
        }
        return j < toks_.size() && toks_[j].type == TokenType::Equal &&
               (j+1 >= toks_.size() || toks_[j+1].type != TokenType::Equal);
    }

    // IDENT (, IDENT)+ = (not ==) at least one comma required, otherwise this is just the ordinary single-variable assignment path in stmt()
    bool isMultiAssignHead() const {
        if (!checkAt(0, TokenType::Identifier)) return false;
        size_t j = i_ + 1;
        if (j >= toks_.size() || toks_[j].type != TokenType::Comma) return false;
        while (j < toks_.size() && toks_[j].type == TokenType::Comma) {
            ++j;
            if (j >= toks_.size() || toks_[j].type != TokenType::Identifier) return false;
            ++j;
        }
        return j < toks_.size() && toks_[j].type == TokenType::Equal &&
               (j+1 >= toks_.size() || toks_[j+1].type != TokenType::Equal);
    }

    // a, b, c = expr
    StmtPtr parseMultiAssign() {
        SourceRange start = tr(peek());
        auto s = std::make_unique<MultiAssignStmt>();
        do {
            Token nm = consume(TokenType::Identifier, "expected variable name");
            s->names.push_back(nm.text);
            s->nameRanges.push_back(tr(nm));
        } while (match(TokenType::Comma));
        consume(TokenType::Equal, "expected '='");
        auto e = expr();
        s->range = SourceRange::merge(start, e->range);
        s->value = std::move(e);
        return s;
    }

    // name(params) = expr
    //  \/\/\/
    // fn name(params) return expr end
    StmtPtr parseFnAssign() {
        Token nameTok = toks_[i_++];
        SourceRange start = tr(nameTok);
        consume(TokenType::LParen, "expected '('");
        std::vector<SourceRange> paramRanges;
        auto params = parseParamList(&paramRanges);
        consume(TokenType::RParen, "expected ')'");
        TypeElem retElem;
        const bool retWritten = check(TokenType::RArrow);
        TypeSet retType = parseReturnAnnotation(&retElem);
        consume(TokenType::Equal,  "expected '='");
        auto bodyExpr = expr();
        // wrap in return stmt
        auto ret   = std::make_unique<ReturnStmt>(); ret->range = bodyExpr->range; ret->value = std::move(bodyExpr);
        auto fn    = std::make_unique<FnStmt>();
        fn->name   = nameTok.text; fn->params = std::move(params); fn->returnType = retType; fn->returnElem = retElem; fn->returnWritten = retWritten;
        fn->body.push_back(std::move(ret));
        fn->isExprForm = true;
        fn->nameRange = start; fn->paramRanges = std::move(paramRanges);
        fn->range  = SourceRange::merge(start, fn->body.back()->range);
        return fn;
    }

    // const name = expr          a value worked out at compile time
    // const fn name(params) ... end   a function the compiler copies into its calls
    StmtPtr parseConst() {
        SourceRange start = tr(prev());
        if (match(TokenType::Fn)) return parseConstFn(start);
        Token name = consume(TokenType::Identifier, "expected a name or 'fn' after 'const'");
        consume(TokenType::Equal, "expected '=' and a value after the name of a const");
        auto value = expr();
        auto s = std::make_unique<AssignStmt>();
        s->target = std::make_unique<VarExpr>(name.text, tr(name));
        s->range = SourceRange::merge(start, value->range);
        s->value = std::move(value);
        s->isConst = true;
        return s;
    }
    StmtPtr parseConstFn(SourceRange start) {
        auto st = parseFnBlock();
        auto* fn = static_cast<FnStmt*>(st.get());
        fn->isTemplate = true;
        fn->range = SourceRange::merge(start, fn->range);
        if (!fn->returnType.isAny() || fn->returnElem)
            raiseError("parser", "a const fn cannot have a return type", fn->range, src_);
        for (auto& p : fn->params)
            if (p.optional || p.variadic || p.inout || (p.elem && !p.elem->isHost))
                raiseError("parser", "a const fn takes plain parameters: no '...', optional or arr<T> ones (host is fine)", fn->range, src_);
        if (const char* why = templateProblem(fn->body))
            raiseError("parser", std::string("a const fn cannot contain ") + why, fn->range, src_);
        return st;
    }
    // what a const fn cannot hold (it is copied into the calls, so no code that keeps its own state): null when the body is fine
    static const char* templateProblem(const std::vector<StmtPtr>& body) {
        for (auto& s : body) {
            switch (s->kind) {
            case Stmt::Kind::Fn:          return "a nested function";
            case Stmt::Kind::Try:         return "a try block";
            case Stmt::Kind::Import:      return "an import";
            case Stmt::Kind::MultiAssign: return "an assignment to several names";
            case Stmt::Kind::Local: {
                auto* l = static_cast<LocalStmt*>(s.get());
                if (l->targets.size() != 1 || l->bracketed || !l->value) return "a local that unpacks or has no value";
                if (hasLambdaOrRef(l->value.get())) return "a function value or '&' argument";
                break;
            }
            case Stmt::Kind::Assign: {
                auto* a = static_cast<AssignStmt*>(s.get());
                if (a->isConst) return "a const";
                if (hasLambdaOrRef(a->target.get()) || hasLambdaOrRef(a->value.get())) return "a function value or '&' argument";
                break;
            }
            case Stmt::Kind::Expr:   if (hasLambdaOrRef(static_cast<ExprStmt*>(s.get())->expr.get())) return "a function value or '&' argument"; break;
            case Stmt::Kind::Return: { auto* r = static_cast<ReturnStmt*>(s.get()); if (r->value && hasLambdaOrRef(r->value.get())) return "a function value or '&' argument"; break; }
            case Stmt::Kind::If: {
                auto* i = static_cast<IfStmt*>(s.get());
                if (hasLambdaOrRef(i->cond.get())) return "a function value or '&' argument";
                if (auto* w = templateProblem(i->thenBlock)) return w;
                if (auto* w = templateProblem(i->elseBlock)) return w;
                break;
            }
            case Stmt::Kind::While: {
                auto* w = static_cast<WhileStmt*>(s.get());
                if (hasLambdaOrRef(w->cond.get())) return "a function value or '&' argument";
                if (auto* p = templateProblem(w->body)) return p;
                break;
            }
            case Stmt::Kind::For: {
                auto* f = static_cast<ForStmt*>(s.get());
                if (hasLambdaOrRef(f->iterable.get())) return "a function value or '&' argument";
                if (auto* p = templateProblem(f->body)) return p;
                break;
            }
            default: break;
            }
        }
        return nullptr;
    }

    // fn name(params) body end
    StmtPtr parseFnBlock() {
        SourceRange start = tr(prev());
        Token name = consume(TokenType::Identifier, "expected function name after 'fn'");
        consume(TokenType::LParen, "expected '('");
        std::vector<SourceRange> paramRanges;
        auto params = parseParamList(&paramRanges);
        consume(TokenType::RParen, "expected ')'");
        TypeElem retElem;
        const bool retWritten = check(TokenType::RArrow);
        TypeSet retType = parseReturnAnnotation(&retElem);
        std::vector<StmtPtr> body;
        int savedLoopDepth = loopDepth_; loopDepth_ = 0; // break/continue can't cross a fn boundary
        while (!check(TokenType::End) && !check(TokenType::Eof)) stmtInto(body);
        loopDepth_ = savedLoopDepth;
        Token endTok = consume(TokenType::End, "expected 'end' to close 'fn'");
        auto fn = std::make_unique<FnStmt>();
        fn->name = name.text; fn->params = std::move(params); fn->returnType = retType; fn->returnElem = retElem; fn->returnWritten = retWritten;
        fn->body = std::move(body);
        fn->nameRange = tr(name); fn->paramRanges = std::move(paramRanges);
        fn->range = SourceRange::merge(start, tr(endTok));
        return fn;
    }

    // one target in a local declaration list [type] name
    // IDENT with another IDENT means the first is type and second is the variable (local int x)
    // IDENT with no other IDENT is just the variable name untyped
    DestructureTarget parseDestructureTarget() {
        DestructureTarget t;
        if ((check(TokenType::Identifier) && checkAt(1, TokenType::Identifier)) || genericTypeAhead() || fnTypeAhead()) {
            Token typeTok = toks_[i_++];
            t.type = parseTypeSpec(typeTok, &t.elem);
            t.explicitType = true;
        }
        Token nm = consume(TokenType::Identifier, "expected variable name");
        t.name = nm.text;
        t.range = tr(nm);
        return t;
    }

    // local [type] name = expr
    // local [type] name1, [type] name2, ... = expr
    // local [type] [name1, name2, ...] = expr
    StmtPtr parseLocal() {
        SourceRange start = tr(prev());
        auto s = std::make_unique<LocalStmt>();

        bool bracketNext =
            check(TokenType::LBracket) ||
            (check(TokenType::Identifier) && checkAt(1, TokenType::LBracket));

        if (bracketNext) {
            s->bracketed = true;
            TypeSet  sharedType = TypeSet::Any();
            bool     sharedExplicit = false;
            if (check(TokenType::Identifier)) {
                Token typeTok = toks_[i_++];
                sharedType = parseTypeName(typeTok);
                sharedExplicit = true;
            }
            consume(TokenType::LBracket, "expected '['");
            do {
                DestructureTarget t;
                Token nm = consume(TokenType::Identifier, "expected variable name");
                t.name   = nm.text;
                t.range  = tr(nm);
                t.type   = sharedType;
                t.explicitType = sharedExplicit;
                s->targets.push_back(std::move(t));
            } while (match(TokenType::Comma));
            consume(TokenType::RBracket, "expected ']'");
        } else {
            s->targets.push_back(parseDestructureTarget());
            while (match(TokenType::Comma))
                s->targets.push_back(parseDestructureTarget());
        }

        consume(TokenType::Equal, "expected '=' in 'local' declaration");
        auto e = expr();
        s->range = SourceRange::merge(start, e->range);
        s->value = std::move(e);
        return s;
    }

    StmtPtr parseIf() {
        SourceRange start = tr(prev());
        auto s = parseIfHead(start);
        Token endTok = consume(TokenType::End, "expected 'end' to close 'if'");
        s->range = SourceRange::merge(s->range, tr(endTok));
        return s;
    }

    // parses "cond thenBlock" plus any chained elif/else
    // but doesn't consume the closing end
    // an elif chain desugars into nested IfStmt, all sharing the single end token the outermost caller (parseIf) consumes owo
    StmtPtr parseIfHead(SourceRange start) {
        auto cond = expr();
        std::vector<StmtPtr> thenB;
        while (!check(TokenType::Elif) && !check(TokenType::Else) &&
               !check(TokenType::End)  && !check(TokenType::Eof))
            stmtInto(thenB);

        auto s = std::make_unique<IfStmt>();
        s->cond = std::move(cond);
        s->thenBlock = std::move(thenB);
        s->range = start;

        if (match(TokenType::Elif)) {
            SourceRange elifStart = tr(prev());
            std::vector<StmtPtr> elseB;
            elseB.push_back(parseIfHead(elifStart));
            s->elseBlock = std::move(elseB);
        } else if (match(TokenType::Else)) {
            std::vector<StmtPtr> elseB;
            while (!check(TokenType::End) && !check(TokenType::Eof)) stmtInto(elseB);
            s->elseBlock = std::move(elseB);
        }
        return s;
    }

    StmtPtr parseReturn() {
        SourceRange start = tr(prev());
        auto e = expr();
        auto s = std::make_unique<ReturnStmt>();
        s->range = SourceRange::merge(start, e->range); s->value = std::move(e);
        return s;
    }

    StmtPtr parseWhile() {
        SourceRange start = tr(prev());
        auto cond = expr();
        ++loopDepth_;
        std::vector<StmtPtr> body;
        while (!check(TokenType::End) && !check(TokenType::Eof)) stmtInto(body);
        --loopDepth_;
        Token endTok = consume(TokenType::End, "expected 'end' to close 'while'");
        auto s = std::make_unique<WhileStmt>();
        s->cond = std::move(cond); s->body = std::move(body);
        s->range = SourceRange::merge(start, tr(endTok));
        return s;
    }

    // for varName in iterable ... end
    // for keyName, varName in iterable ... end
    StmtPtr parseFor() {
        SourceRange start = tr(prev());
        Token first = consume(TokenType::Identifier, "expected loop variable after 'for'");
        std::string keyName, varName;
        SourceRange keyRange, varRange;
        if (match(TokenType::Comma)) {
            Token second = consume(TokenType::Identifier, "expected second loop variable after ','");
            keyName = first.text;  keyRange = tr(first);
            varName = second.text; varRange = tr(second);
        } else {
            varName = first.text;  varRange = tr(first);
        }
        consume(TokenType::In, "expected 'in' after for-loop variable(s)");
        auto iter = expr();
        ++loopDepth_;
        std::vector<StmtPtr> body;
        while (!check(TokenType::End) && !check(TokenType::Eof)) stmtInto(body);
        --loopDepth_;
        Token endTok = consume(TokenType::End, "expected 'end' to close 'for'");
        auto s = std::make_unique<ForStmt>();
        s->keyName = std::move(keyName); s->varName = std::move(varName);
        s->keyRange = keyRange; s->varRange = varRange;
        s->iterable = std::move(iter); s->body = std::move(body);
        s->range = SourceRange::merge(start, tr(endTok));
        return s;
    }

    // try ... catch e ... end
    StmtPtr parseTry() {
        SourceRange start = tr(prev());
        std::vector<StmtPtr> tryBlock;
        while (!check(TokenType::Catch) && !check(TokenType::Eof)) stmtInto(tryBlock);
        consume(TokenType::Catch, "expected 'catch' to close 'try'");
        Token var = consume(TokenType::Identifier, "expected a variable name after 'catch'");
        std::vector<StmtPtr> catchBlock;
        while (!check(TokenType::End) && !check(TokenType::Eof)) stmtInto(catchBlock);
        Token endTok = consume(TokenType::End, "expected 'end' to close 'try'/'catch'");
        auto s = std::make_unique<TryStmt>();
        s->tryBlock = std::move(tryBlock);
        s->catchVar = var.text;
        s->catchRange = tr(var);
        s->catchBlock = std::move(catchBlock);
        s->range = SourceRange::merge(start, tr(endTok));
        return s;
    }

    StmtPtr parseBreak() {
        Token tok = prev();
        if (loopDepth_ == 0)
            raiseError("parser", "'break' used outside of a loop", tr(tok), src_);
        auto s = std::make_unique<BreakStmt>();
        s->range = tr(tok);
        return s;
    }

    StmtPtr parseContinue() {
        Token tok = prev();
        if (loopDepth_ == 0)
            raiseError("parser", "'continue' used outside of a loop", tr(tok), src_);
        auto s = std::make_unique<ContinueStmt>();
        s->range = tr(tok);
        return s;
    }

    // import <expr>
    // path is a full expression (not just a string literal) so a script can
    // compute its own import path exactly like it already can for
    // load_module(), e.g. `import "plugins/" + name`
    StmtPtr parseImport() {
        SourceRange start = tr(prev());
        auto pathExpr = expr();
        auto s = std::make_unique<ImportStmt>();
        s->range = SourceRange::merge(start, pathExpr->range);
        s->pathExpr = std::move(pathExpr);
        return s;
    }

    ExprPtr expr(int minPrec = 0) {
        NestGuard guard(*this);
        ExprPtr left = unary();
        while (true) {
            if (minPrec <= 0 && check(TokenType::Or)) {
                ++i_;
                auto right = expr(1); // and binds tighter than or
                auto log = std::make_unique<LogicExpr>(Expr::Kind::Or);
                log->range = SourceRange::merge(left->range, right->range);
                log->left  = std::move(left);
                log->right = std::move(right);
                setDepth(log.get(), {log->left.get(), log->right.get()});
                left = std::move(log);
                continue;
            }
            if (minPrec <= 1 && check(TokenType::And)) {
                ++i_;
                auto right = expr(2); // and is right-associative at prec 1
                auto log = std::make_unique<LogicExpr>(Expr::Kind::And);
                log->range = SourceRange::merge(left->range, right->range);
                log->left  = std::move(left);
                log->right = std::move(right);
                setDepth(log.get(), {log->left.get(), log->right.get()});
                left = std::move(log);
                continue;
            }

            // `>>` is two `>` tokens (the lexer cannot make it one: `arr<arr<int>>` closes two types). a `>` right after a
            // `>` in an operator position can't be anything else, as no expression starts with `>`
            const bool shiftRight = check(TokenType::Greater) && checkAt(1, TokenType::Greater);
            int prec = shiftRight ? kShiftPrec : infixPrec(peek().type);
            if (prec < minPrec) break;
            Token op = toks_[i_++];
            if (shiftRight) { ++i_; op.text = ">>"; }
            auto bin  = std::make_unique<BinaryExpr>();
            bin->op   = op.text;
            bin->left = std::move(left);
            bin->right = expr(prec + 1);
            bin->range = SourceRange::merge(bin->left->range, bin->right->range);
            setDepth(bin.get(), {bin->left.get(), bin->right.get()});
            left = std::move(bin);
        }
        return left;
    }

    // `&target` as a call argument, the `&` already consumed. the target must be a variable or an element of one
    ExprPtr refArg(const Token& amp) {
        auto target = unary();
        const Expr* root = target.get();
        while (root->kind == Expr::Kind::Index) root = static_cast<const IndexExpr*>(root)->object.get();
        if (root->kind != Expr::Kind::Var)
            raiseError("parser", "'&' needs a variable or an element of one, like &list or &grid[i]",
                       SourceRange::merge(tr(amp), target->range), src_);
        auto r = std::make_unique<RefExpr>();
        r->range = SourceRange::merge(tr(amp), target->range);
        setDepth(r.get(), {target.get()});
        r->target = std::move(target);
        return r;
    }

    ExprPtr unary() {
        NestGuard guard(*this);
        if (match(TokenType::Minus) || match(TokenType::Bang) || match(TokenType::Tilde)) {
            Token op = prev(); auto right = unary();
            auto u = std::make_unique<UnaryExpr>();
            u->op = op.text; u->right = std::move(right);
            u->range = SourceRange::merge(tr(op), u->right->range);
            setDepth(u.get(), {u->right.get()});
            return u;
        }
        auto expr_ = primary();
        // handle postfix operators: [ ... ], identifier() and .identifier
        while (true) {
            if (match(TokenType::LParen)) {
                auto call = std::make_unique<CallExpr>();
                call->callee = std::move(expr_);

                if (!check(TokenType::RParen))
                    do {
                        call->args.push_back(match(TokenType::Amp) ? refArg(prev()) : expr());
                    } while (match(TokenType::Comma));

                Token rp = consume(TokenType::RParen, "expected ')'");
                call->range = SourceRange::merge(
                    call->callee->range,
                    tr(rp)
                );
                {
                    int d = call->callee->depth;
                    for (auto& a : call->args) d = std::max(d, a->depth);
                    call->depth = d + 1;
                    if (call->depth > kMaxExprDepth)
                        raiseError("parser", "expression too deeply nested or too long (max depth " +
                                   std::to_string(kMaxExprDepth) + ")", call->range, src_);
                }

                expr_ = std::move(call);
            }
            else if (match(TokenType::LBracket)) {
                auto idx = std::make_unique<IndexExpr>();
                idx->object = std::move(expr_);
                idx->index = expr();
                Token rb = consume(TokenType::RBracket, "expected ']'");
                idx->range = SourceRange::merge(idx->object->range, tr(rb));
                setDepth(idx.get(), {idx->object.get(), idx->index.get()});
                expr_ = std::move(idx);
            }
            else if (match(TokenType::Dot)) {
                Token id = consume(TokenType::Identifier, "expected identifier after '.'");
                auto idx = std::make_unique<IndexExpr>();
                idx->object = std::move(expr_);
                // convert the identifier into a string literal expression
                auto keyExpr = std::make_unique<StringExpr>(id.text, SourceRange::fromToken(id));
                idx->index = std::move(keyExpr);
                idx->range = SourceRange::merge(idx->object->range, SourceRange::fromToken(id));
                setDepth(idx.get(), {idx->object.get(), idx->index.get()});
                expr_ = std::move(idx);
            }
            else {
                break;
            }
        }
        return expr_;
    }

    ExprPtr primary() {
        Token tok = toks_[i_++];
        SourceRange r = tr(tok);
        switch (tok.type) {
            case TokenType::Number: {
                if (isRadixLiteral(tok.text)) return std::make_unique<IntExpr>(*intLiteral(tok.text), r);
                // no decimal point = an exact int64 literal
                // defaults to float parsing if it doesn't fit
                if (tok.text.find('.') == std::string::npos) {
                    int64_t iv = 0;
                    auto [ptr, ec] = std::from_chars(tok.text.data(), tok.text.data()+tok.text.size(), iv);
                    if (ec == std::errc() && ptr == tok.text.data()+tok.text.size())
                        return std::make_unique<IntExpr>(iv, r);
                }
                double v; auto [ptr,ec] = parse_double(tok.text.data(), tok.text.data()+tok.text.size(), v);
                if (ec != std::errc()) raiseError("parser","invalid number: "+tok.text, r, src_);
                return std::make_unique<NumberExpr>(v, r);
            }
            case TokenType::String:
                return std::make_unique<StringExpr>(tok.text, r);
            case TokenType::Identifier: {
                return std::make_unique<VarExpr>(tok.text, r);
            }
            case TokenType::LParen: {
                auto e = expr(); Token rp = consume(TokenType::RParen, "expected ')'");
                e->range = SourceRange::merge(r, tr(rp)); return e;
            }
            case TokenType::LBracket: {
                auto arr = std::make_unique<ArrayExpr>();
                if (!check(TokenType::RBracket)) {
                    arr->elements.push_back(expr());
                    if (match(TokenType::Ellipsis)) {                         // [a...b]
                        arr->elements.push_back(expr());
                        arr->isRange = true;
                    } else {
                        while (match(TokenType::Comma) && !check(TokenType::RBracket)) {
                            arr->elements.push_back(expr());
                            // [a, b...c]: the second element followed by the dots is a range with a step
                            if (arr->elements.size() == 2 && match(TokenType::Ellipsis)) {
                                arr->elements.push_back(expr());
                                arr->isRange = true;
                                break;
                            }
                        }
                    }
                }
                Token rb = consume(TokenType::RBracket, "expected ']'");
                arr->range = SourceRange::merge(r, tr(rb));
                { int d = 0; for (auto& el : arr->elements) d = std::max(d, el->depth);
                  arr->depth = d + 1;
                  if (arr->depth > kMaxExprDepth)
                      raiseError("parser", "expression too deeply nested or too long (max depth " +
                                 std::to_string(kMaxExprDepth) + ")", arr->range, src_); }
                return arr;
            }
            case TokenType::LCurly: {
                auto map = std::make_unique<MapExpr>();
                if (!check(TokenType::RCurly)) {
                    do {
                        ExprPtr key;
                        if (check(TokenType::Identifier) && checkAt(1, TokenType::Colon)) {
                            Token id = toks_[i_++];
                            key = std::make_unique<StringExpr>(id.text, tr(id));
                            consume(TokenType::Colon, "expected ':'");
                        } else { key = expr(); consume(TokenType::Colon, "expected ':'"); }
                        map->entries.emplace_back(std::move(key), expr());
                    } while (match(TokenType::Comma) && !check(TokenType::RCurly));
                }
                Token rc = consume(TokenType::RCurly, "expected '}'");
                map->range = SourceRange::merge(r, tr(rc));
                { int d = 0; for (auto& kv : map->entries) d = std::max({d, kv.first->depth, kv.second->depth});
                  map->depth = d + 1;
                  if (map->depth > kMaxExprDepth)
                      raiseError("parser", "expression too deeply nested or too long (max depth " +
                                 std::to_string(kMaxExprDepth) + ")", map->range, src_); }
                return map;
            }
            // fn(params) body end
            case TokenType::Fn: {
                consume(TokenType::LParen, "expected '(' after 'fn'");
                std::vector<SourceRange> paramRanges;
                auto params = parseParamList(&paramRanges);
                consume(TokenType::RParen, "expected ')'");
                TypeElem retElem;
                const bool retWritten = check(TokenType::RArrow);
        TypeSet retType = parseReturnAnnotation(&retElem);
                auto lam = std::make_unique<LambdaExpr>();
                lam->params = std::move(params);
                lam->paramRanges = std::move(paramRanges);
                lam->returnType = retType; lam->returnWritten = retWritten;
                lam->returnElem = retElem;
                int savedLoopDepth = loopDepth_; loopDepth_ = 0; // break/continue can't cross a fn boundary
                while (!check(TokenType::End) && !check(TokenType::Eof))
                    stmtInto(lam->ownedBody);
                loopDepth_ = savedLoopDepth;
                Token endTok = consume(TokenType::End, "expected 'end' after fn expression body");
                lam->range  = SourceRange::merge(r, tr(endTok));
                return lam;
            }
            default:
                // primary() advanced past `tok` unconditionally above; if that was the Eof sentinel,
                // put it back so the error-recovery sync() in parse() doesn't read one past the end
                // of the token vector (found by tools/fuzz: a bare `return` at end of input).
                if (tok.type == TokenType::Eof) --i_;
                raiseError("parser", "unexpected token '" + tok.text + "'", r, src_);
        }
    }

    // binary operator levels, low to high: comparison, `|`, `^`, `&`, shifts, `+ -`, `* / %`. the bit operators bind tighter
    // than a comparison (as in Python, not C), so `x & 1 == 0` is `(x & 1) == 0`
    static constexpr int kShiftPrec = 6;
    int infixPrec(TokenType t) const {
        switch (t) {
            case TokenType::EqualEqual:   case TokenType::BangEqual:
            case TokenType::Greater:      case TokenType::Less:
            case TokenType::GreaterEqual: case TokenType::LessEqual: return 2;
            case TokenType::Pipe:                                    return 3;
            case TokenType::Caret:                                   return 4;
            case TokenType::Amp:                                     return 5;
            case TokenType::LShift:                                  return kShiftPrec;
            case TokenType::Plus:         case TokenType::Minus:     return 7;
            case TokenType::Star:         case TokenType::Slash:
            case TokenType::Percent:                                 return 8;

            default: return -1;
        }
    }
};

} // namespace embr

#endif // EMBR_CORE_PARSER_H
