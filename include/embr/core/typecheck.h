// typecheck.h
// finds type mistakes in a script before it runs, from the syntax tree alone.
//
// usage
//   auto errors = embr::typecheck::check(ast);       // one EmbrError per mistake, in source order
//   embr::vm::compileSource() runs this and stops at the first one. `embr check` prints them all.
//
// good to know
//   - it only reports a mistake that cannot be right: the type of the value and the type of the variable (or
//     parameter) share no type at all. a value that is "maybe" the wrong type is left to the run-time checks.
//   - a variable's type is the type of its first assignment, so `x = 1` then `x = "a"` is an error. int and float
//     count as one family (num), so `x = 1` then `x = x / 2` is fine. a declared type (`local int x`) is exact.
//   - an untyped parameter of a function that is only ever called by name inside this file takes the types of those
//     calls. a function anything else could call (a top-level one in a module, or one passed around) stays `any`.
//   - names it cannot see (plugins, other files, globals made elsewhere) are `any`.
//   - a tool that wants the type of every expression sets Options::typesOut and reads one ExprType per Expr after the check.
//   - see the wiki, type-system.

#ifndef EMBR_CORE_TYPECHECK_H
#define EMBR_CORE_TYPECHECK_H

#include "ast.h"
#include "types.h"
#include "diagnostics.h"
#include "registry.h"

#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace embr {
namespace typecheck {

// a name a file makes at its top level, as another file that imports it sees it
struct ExportedName {
    TypeSet  type = TypeSet::Any();
    TypeElem el;                       // what the elements are, for an array or a map
    bool     isFn = false;
    TypeSet  ret = TypeSet::Any();     // what a function returns
    TypeElem retEl;
    std::vector<Param> params;         // a function's parameters as written, for an editor's hover
    bool     isLocal = false;          // declared with `local`, so an importer does not get it
};
using ModuleExports = std::unordered_map<std::string, ExportedName>;

// what the checker settled on for one expression, for tools that need the type of every expression (aembr, typed opcodes).
// `sure`: one core type and not soft, the same test as AssignStmt::storeProven
struct ExprType {
    TypeSet  t = TypeSet::Any();
    TypeElem el;                       // what the elements are, for an array or a map
    bool     sure = false;
};
using ExprTypes = std::unordered_map<const Expr*, ExprType>;

struct Options {
    // this file is the whole program: no other file will call its top-level functions. the `embr` command sets it
    // for the script you run, and only when the file imports no script module and starts no worker
    bool closedWorld = false;
    // this file is an imported script module: the importer's variables are not its own, so a plain assignment in one of its functions never means one of them
    bool isModule = false;
    const SourceMap* source = nullptr;   // for the source line in an error
    // the interpreter the file will run in. names it already has are known: a global with a recorded type (made by an
    // earlier file or REPL line), and natives with a signature (plugins imported so far). null: none are
    const Interpreter* interp = nullptr;
    // the names a script module (an `import "x.embr"` with a literal path) makes, or null when it can't be found or read. the checker
    // then knows the types of what the import brings in
    std::function<const ModuleExports*(const std::string&)> importModule;
    // filled in with every name this file makes at its top level
    ModuleExports* exportsOut = nullptr;
    // filled in with the type of every expression. null (the default): nothing is recorded and the tree and the errors are the same
    ExprTypes* typesOut = nullptr;
};

namespace detail {

constexpr uint8_t kBitNumber = 1 << 0, kBitInt = 1 << 1, kBitStr = 1 << 2, kBitArr = 1 << 3,
                  kBitMap = 1 << 4, kBitFn = 1 << 5, kBitPtr = 1 << 6;

// a type set as the words a script writes (num, float, int, str, arr, map, fn, ptr)
inline std::string tn(TypeSet t) {
    if (t.mask == 0xFF) return "any";
    std::string out;
    auto add = [&](const char* n) { if (!out.empty()) out += '|'; out += n; };
    const bool f = t.mask & kBitNumber, i = t.mask & kBitInt;
    if (f && i) add("num"); else if (f) add("float"); else if (i) add("int");
    if (t.mask & kBitStr) add("str");
    if (t.mask & kBitArr) add("arr");
    if (t.mask & kBitMap) add("map");
    if (t.mask & kBitFn)  add("fn");
    if (t.mask & kBitPtr) add("ptr");
    return out.empty() ? "?" : out;
}
inline TypeSet bits(uint8_t m) { return TypeSet(m); }
inline TypeSet joinT(TypeSet a, TypeSet b) { return TypeSet((uint8_t)(a.mask | b.mask)); }
inline bool disjoint(TypeSet a, TypeSet b) { return (a.mask & b.mask) == 0; }
inline bool isAnyT(TypeSet t) { return t.mask == 0xFF; }
// exactly one core type
inline bool singleTag(TypeSet t) { return t.mask != 0 && t.mask != 0xFF && (t.mask & (t.mask - 1)) == 0; }
inline bool subsetOf(TypeSet a, uint8_t m) { return (a.mask & ~m) == 0 && a.mask != 0; }
inline const TypeSet kNum   = TypeSet((uint8_t)(kBitNumber | kBitInt));
inline const TypeSet kFloat = TypeSet(kBitNumber);
inline const TypeSet kInt   = TypeSet(kBitInt);
inline const TypeSet kStr   = TypeSet(kBitStr);
inline const TypeSet kArr   = TypeSet(kBitArr);
inline const TypeSet kMap   = TypeSet(kBitMap);
inline const TypeSet kFn    = TypeSet(kBitFn);
inline const TypeSet kAny   = TypeSet::Any();

// what the checker knows of a value: its type, and for an array or a map what it holds (null when that is not known).
// soft: the type came out of a container's elements. an element type is a promise the run time does not check, so
// nothing the VM skips (a store without a test, a variable's recorded type) is built on a soft type
struct Ty {
    TypeSet  t = kAny;
    TypeElem el;
    bool     soft = false;
    Ty() = default;
    Ty(TypeSet ts) : t(ts) {}
    Ty(TypeSet ts, TypeElem e, bool sf = false) : t(ts), el(std::move(e)), soft(sf) {}
};
inline bool hasContainer(TypeSet t) { return (t.mask & (kBitArr | kBitMap)) != 0; }
inline TypeElem makeElem(TypeSet t, TypeElem el) { auto a = std::make_shared<TypeAnn>(); a->t = t; a->el = std::move(el); return a; }
// what a value of the type annotated `a` carries in its el slot: a function type its signature, a container its element type
inline TypeElem detail(const TypeElem& a) { return a ? (a->isFn ? a : a->el) : nullptr; }
// a function type is not carried by a variable that only got it from its first value: a later assignment may be a function of another arity
inline TypeElem noSig(const TypeElem& a) { return (a && a->isFn) ? nullptr : a; }
// the join of two element types (the element type of a container that holds either)
inline TypeElem joinElem(const TypeElem& a, const TypeElem& b) {
    if (!a || !b) return nullptr;
    return makeElem(joinT(a->t, b->t), (hasContainer(a->t) && hasContainer(b->t)) ? joinElem(a->el, b->el) : (hasContainer(a->t) ? a->el : b->el));
}
inline Ty joinTy(const Ty& a, const Ty& b) {
    Ty r(joinT(a.t, b.t));
    r.soft = a.soft || b.soft;
    if (hasContainer(a.t) && hasContainer(b.t)) r.el = joinElem(a.el, b.el);
    else if (hasContainer(a.t)) r.el = a.el;
    else if (hasContainer(b.t)) r.el = b.el;
    return r;
}
// the type of one element of `c`, any when it is not known
inline Ty elemOf(const Ty& c) {
    if (!c.el) return Ty(kAny);
    return Ty(c.el->t, detail(c.el), true);
}
inline bool singleContainer(TypeSet t) { return t.mask == kBitArr || t.mask == kBitMap; }
// two element types that can never both be right
inline bool elemsDiffer(const TypeElem& a, const TypeElem& b);
// two function types that can never both be right: another number of parameters, or a parameter or result of types with nothing in common
inline bool fnSigsDiffer(const TypeAnn& a, const TypeAnn& b) {
    if (a.params.size() != b.params.size()) return true;
    for (size_t i = 0; i < a.params.size(); ++i) if (elemsDiffer(a.params[i], b.params[i])) return true;
    return elemsDiffer(a.ret, b.ret);      // a result that is not written is not checked
}
inline bool elemsDiffer(const TypeElem& a, const TypeElem& b) {
    if (!a || !b) return false;
    if (disjoint(a->t, b->t)) return true;
    if (a->isFn && b->isFn) return fnSigsDiffer(*a, *b);
    return singleContainer(a->t) && a->t.mask == b->t.mask && elemsDiffer(a->el, b->el);
}
// a value of type `got` can never be used where `want` is wanted
inline bool differ(const Ty& want, const Ty& got) {
    if (disjoint(want.t, got.t)) return true;
    // (a native that takes `fn|ptr` and says what the function takes is checked against a function; a pointer is not)
    if ((want.t.mask & kBitFn) && got.t.mask == kBitFn && want.el && got.el && want.el->isFn && got.el->isFn) return fnSigsDiffer(*want.el, *got.el);
    return singleContainer(want.t) && want.t.mask == got.t.mask && elemsDiffer(want.el, got.el);
}
inline std::string tyName(const TypeSet& t, const TypeElem& el);
inline std::string annName(const TypeElem& a) { return a ? tyName(a->t, detail(a)) : "any"; }
inline std::string tyName(const TypeSet& t, const TypeElem& el) {
    std::string n = tn(t);
    if (el && el->isFn && (t.mask & kBitFn)) {
        if (t.mask != kBitFn) n = "fn";       // (a native that also takes a pointer: say what the function is)
        n += "(";
        for (size_t i = 0; i < el->params.size(); ++i) n += (i ? ", " : "") + annName(el->params[i]);
        n += ")";
        if (el->ret) n += " -> " + annName(el->ret);
        if (t.mask != kBitFn) n += " (or a pointer)";
    } else if (singleContainer(t) && el) {
        n += "<" + annName(el) + ">";
    }
    return n;
}
inline std::string tyName(const Ty& t) { return tyName(t.t, t.el); }

struct Var {
    TypeSet type   = kAny;
    TypeElem el;                // what the elements are, for an array or a map
    bool    soft   = false;     // the type came from a container's elements, see Ty
    bool    fromLocal = false;  // made by a `local` statement
    bool    late   = false;     // found through globalPrev_: a global this file makes further down, which a local of the same name can still shadow
    // the variable's exact type when it is known for sure (declared, or the one type its first value had). a store
    // into the variable carries it so the VM can test the value on the spot. any when not known
    TypeSet hint   = kAny;
    bool    declared = false;   // the type was written in the source
    bool    isConst = false;    // made by `const name = ...`: no assignment may follow
    int     fn     = 0;         // the function that owns it, 0 for the top level
    int     declFn = 0;         // a function declared with `fn name(...)`: its id plus one, so its signature can be read when it is used as a value
};

struct FnInfo {
    const FnStmt*       decl = nullptr;
    int                 id = 0;
    bool                nested = false;      // declared inside another function
    bool                closed = false;      // every call is a direct call by name in this file
    std::vector<Ty>     callArgs;            // what each parameter was called with, joined (this pass)
    std::vector<Ty>     params;              // the type each parameter has inside the body (used this pass)
    std::vector<bool>   explicitParam;
    Ty                  declRet;
    bool                hasDeclRet = false;
    Ty                  inferRet;            // joined from the previous pass
    Ty                  retNow = Ty(TypeSet(0));   // collected during this pass
    bool                called = false;
};

struct Scope {
    std::unordered_map<std::string, Var> vars;
    int  fn = 0;
    bool isFnRoot = false;
};

class Checker {
public:
    Checker(const std::vector<StmtPtr>& ast, const Options& o) : ast_(ast), opt_(o) {}

    std::vector<EmbrError> run() {
        scan();
        // each pass uses what the one before learned about parameters and return types. the types only ever get
        // more exact (a pass starts from `any`), so stopping early is always safe
        for (int pass = 0; pass < 3; ++pass) {
            report_ = false;
            analyzeUnit();
            adopt();
        }
        report_ = true;
        errors_.clear();
        checkOverloadConflicts();
        analyzeUnit();
        collectExports();
        return std::move(errors_);
    }

private:
    const std::vector<StmtPtr>& ast_;
    Options                     opt_;
    std::vector<EmbrError>      errors_;
    bool                        report_ = false;

    std::vector<FnInfo>                   fns_;        // index 0 is unused (the top level)
    std::unordered_map<std::string, int>  fnByName_;   // functions bound exactly once in the unit
    std::unordered_map<const FnStmt*, int> fnOfStmt_;
    std::unordered_map<std::string, int>  bindings_;   // how many times a name is bound anywhere in the unit
    std::unordered_map<std::string, int>  valueUses_;  // uses of a name that are not a direct call
    bool                                  dynamicImport_ = false;
    // overload groups (`abs__1`, `abs__2` are the group `abs`): the members this file declares at its top level, and one declaration for an error
    std::unordered_map<std::string, std::vector<std::string>> scriptGroups_;
    std::unordered_map<std::string, const FnStmt*>            groupDecl_;

    std::vector<Scope> scopes_;
    int                curFn_ = 0;

    // ---- facts about the whole file, found before any typing ----

    void scan() {
        fns_.emplace_back();
        for (auto& s : ast_) scanStmt(s.get(), 0);
        for (size_t i = 1; i < fns_.size(); ++i) {
            FnInfo& f = fns_[i];
            const std::string& nm = f.decl->name;
            if (bindings_[nm] != 1) continue;
            fnByName_[nm] = (int)i;
            const bool visibleOnlyHere = f.nested || (opt_.closedWorld && !dynamicImport_);
            f.closed = visibleOnlyHere && valueUses_[nm] == 0;
        }
    }
    void bind(const std::string& n) { bindings_[n]++; }
    void scanParams(const std::vector<Param>& ps) { for (auto& p : ps) bind(p.name); }

    void scanBlock(const std::vector<StmtPtr>& b, int depth) { for (auto& s : b) scanStmt(s.get(), depth); }
    void scanStmt(const Stmt* s, int depth) {
        switch (s->kind) {
            case Stmt::Kind::Assign: {
                auto* a = static_cast<const AssignStmt*>(s);
                if (a->target->kind == Expr::Kind::Var) bind(static_cast<const VarExpr*>(a->target.get())->name);
                else scanExpr(a->target.get(), false);
                scanExpr(a->value.get(), false);
                break;
            }
            case Stmt::Kind::MultiAssign: {
                auto* m = static_cast<const MultiAssignStmt*>(s);
                for (auto& n : m->names) bind(n);
                scanExpr(m->value.get(), false);
                break;
            }
            case Stmt::Kind::Local: {
                auto* l = static_cast<const LocalStmt*>(s);
                for (auto& t : l->targets) bind(t.name);
                scanExpr(l->value.get(), false);
                break;
            }
            case Stmt::Kind::If: {
                auto* i = static_cast<const IfStmt*>(s);
                scanExpr(i->cond.get(), false); scanBlock(i->thenBlock, depth); scanBlock(i->elseBlock, depth);
                break;
            }
            case Stmt::Kind::Fn: {
                auto* f = static_cast<const FnStmt*>(s);
                bind(f->name);
                FnInfo info; info.decl = f; info.id = (int)fns_.size(); info.nested = depth > 0;
                info.explicitParam.assign(f->params.size(), false);
                info.params.assign(f->params.size(), Ty(kAny));
                info.callArgs.assign(f->params.size(), Ty(TypeSet(0)));
                for (size_t i = 0; i < f->params.size(); ++i) {
                    info.explicitParam[i] = f->params[i].explicitType;
                    if (info.explicitParam[i]) info.params[i] = Ty(f->params[i].type, f->params[i].elem);
                }
                info.hasDeclRet = !f->returnType.isAny();
                info.declRet = Ty(f->returnType, f->returnElem);
                { std::string base; if (depth == 0 && overloadName(f->name, &base)) { scriptGroups_[base].push_back(f->name); groupDecl_.emplace(base, f); } }
                fnOfStmt_[f] = info.id;
                fns_.push_back(std::move(info));
                scanParams(f->params);
                scanBlock(f->body, depth + 1);
                break;
            }
            case Stmt::Kind::Return: {
                auto* r = static_cast<const ReturnStmt*>(s);
                if (r->value) scanExpr(r->value.get(), false);
                break;
            }
            case Stmt::Kind::While: {
                auto* w = static_cast<const WhileStmt*>(s);
                scanExpr(w->cond.get(), false); scanBlock(w->body, depth);
                break;
            }
            case Stmt::Kind::For: {
                auto* f = static_cast<const ForStmt*>(s);
                bind(f->varName); if (!f->keyName.empty()) bind(f->keyName);
                scanExpr(f->iterable.get(), false); scanBlock(f->body, depth);
                break;
            }
            case Stmt::Kind::Import: {
                auto* im = static_cast<const ImportStmt*>(s);
                // a plugin name can't call back into this file, a script module might
                bool plugin = im->pathExpr && im->pathExpr->kind == Expr::Kind::String &&
                              static_cast<const StringExpr*>(im->pathExpr.get())->v.find(".embr") == std::string::npos &&
                              static_cast<const StringExpr*>(im->pathExpr.get())->v.find(".vmbr") == std::string::npos;
                if (!plugin) dynamicImport_ = true;
                if (im->pathExpr) scanExpr(im->pathExpr.get(), false);
                break;
            }
            case Stmt::Kind::Expr:
                scanExpr(static_cast<const ExprStmt*>(s)->expr.get(), false);
                break;
            case Stmt::Kind::Try: {
                auto* t = static_cast<const TryStmt*>(s);
                scanBlock(t->tryBlock, depth); bind(t->catchVar); scanBlock(t->catchBlock, depth);
                break;
            }
            case Stmt::Kind::Break: case Stmt::Kind::Continue: break;
        }
    }
    void scanExpr(const Expr* e, bool isCallee) {
        if (!e) return;
        switch (e->kind) {
            case Expr::Kind::Var: {
                const std::string& n = static_cast<const VarExpr*>(e)->name;
                if (!isCallee) valueUses_[n]++;
                if (n == "load_module" || n == "worker_spawn" || n == "async_spawn") dynamicImport_ = true;
                break;
            }
            case Expr::Kind::Unary:  scanExpr(static_cast<const UnaryExpr*>(e)->right.get(), false); break;
            case Expr::Kind::Binary: { auto* b = static_cast<const BinaryExpr*>(e); scanExpr(b->left.get(), false); scanExpr(b->right.get(), false); break; }
            case Expr::Kind::And: case Expr::Kind::Or: { auto* b = static_cast<const LogicExpr*>(e); scanExpr(b->left.get(), false); scanExpr(b->right.get(), false); break; }
            case Expr::Kind::Call: {
                auto* c = static_cast<const CallExpr*>(e);
                scanExpr(c->callee.get(), true);
                for (auto& a : c->args) scanExpr(a.get(), false);
                break;
            }
            case Expr::Kind::Array: for (auto& x : static_cast<const ArrayExpr*>(e)->elements) scanExpr(x.get(), false); break;
            case Expr::Kind::Map: for (auto& kv : static_cast<const MapExpr*>(e)->entries) { scanExpr(kv.first.get(), false); scanExpr(kv.second.get(), false); } break;
            case Expr::Kind::Index: { auto* i = static_cast<const IndexExpr*>(e); scanExpr(i->object.get(), false); scanExpr(i->index.get(), false); break; }
            case Expr::Kind::Lambda: {
                auto* l = static_cast<const LambdaExpr*>(e);
                scanParams(l->params);
                scanBlock(l->ownedBody, 1);
                break;
            }
            case Expr::Kind::Ref: scanExpr(static_cast<const RefExpr*>(e)->target.get(), false); break;
            default: break;
        }
    }

    // what the file made at its top level, for another file that imports it and for the interpreter's memory of its globals
    void collectExports() {
        if (!opt_.exportsOut) return;
        for (auto& [n, v] : scopes_.front().vars) {
            if (v.fn != 0 || v.late) continue;
            if (n == "__module__") continue;   // describes the module, an importer never gets it (core/modinfo.h)
            ExportedName e;
            e.type = v.type; e.el = v.el; e.isLocal = v.fromLocal;
            auto fi = fnByName_.find(n);
            if (fi != fnByName_.end()) {
                const FnInfo& f = fns_[(size_t)fi->second];
                e.isFn = true; e.ret = f.inferRet.t; e.retEl = f.inferRet.el;
                if (f.decl) e.params = f.decl->params;
            }
            (*opt_.exportsOut)[n] = std::move(e);
        }
    }

    // after a pass: parameters of closed functions take what they were called with, and every function's return type
    // is what its body returned. both are upper bounds, so the next pass can only get more exact
    void adopt() {
        globalPrev_.clear();
        for (auto& [n, v] : scopes_.front().vars) if (v.fn == 0) { Var g = v; g.late = true; globalPrev_[n] = g; }
        for (size_t i = 1; i < fns_.size(); ++i) {
            FnInfo& f = fns_[i];
            for (size_t p = 0; p < f.params.size(); ++p) {
                if (f.explicitParam[p]) continue;
                f.params[p] = (f.closed && f.called && f.callArgs[p].t.mask != 0) ? f.callArgs[p] : Ty(kAny);
                f.callArgs[p] = Ty(TypeSet(0));
            }
            f.inferRet = f.hasDeclRet ? f.declRet : (f.retNow.t.mask != 0 ? f.retNow : Ty(kAny));
            f.retNow = Ty(TypeSet(0));
            f.called = false;
        }
    }

    // ---- typing ----

    void error(const SourceRange& r, const std::string& msg) {
        if (!report_) return;
        try { raiseError("type", msg, r, opt_.source ? *opt_.source : emptyMap_); }
        catch (const EmbrError& e) { errors_.push_back(e); }
    }
    SourceMap emptyMap_;

    void analyzeUnit() {
        scopes_.clear(); scopes_.push_back(Scope{{}, 0, true}); curFn_ = 0;
        for (auto& s : ast_) stmt(s.get());
    }

    Var* lookup(const std::string& n) {
        for (size_t i = scopes_.size(); i-- > 0; ) {
            auto it = scopes_[i].vars.find(n);
            if (it != scopes_[i].vars.end()) return &it->second;
        }
        // a global an earlier file or REPL line made, with the type it was given
        if (opt_.interp && !opt_.isModule) {
            if (const TypeSet* rec = opt_.interp->recordedType(n)) {
                Var v; v.type = *rec; v.hint = *rec; v.declared = true; v.fn = 0;
                if (const TypeElem* e = opt_.interp->staticElem(n)) v.el = *e;
                return &(scopes_.front().vars[n] = v);
            }
        }
        // a global this file makes further down: what an earlier pass found out about it, so a function can use a global that is assigned after it
        auto git = globalPrev_.find(n);
        if (git != globalPrev_.end()) return &git->second;
        return nullptr;
    }
    // names the top level made, as the last pass found them (see lookup)
    std::unordered_map<std::string, Var> globalPrev_;
    Scope& fnRoot() {
        for (size_t i = scopes_.size(); i-- > 0; ) if (scopes_[i].isFnRoot) return scopes_[i];
        return scopes_.front();
    }

    void block(const std::vector<StmtPtr>& b) {
        scopes_.push_back(Scope{{}, curFn_, false});
        for (auto& s : b) stmt(s.get());
        scopes_.pop_back();
    }

    static bool alwaysReturns(const std::vector<StmtPtr>& b) {
        if (b.empty()) return false;
        const Stmt* last = b.back().get();
        if (last->kind == Stmt::Kind::Return) return true;
        if (last->kind == Stmt::Kind::If) {
            auto* i = static_cast<const IfStmt*>(last);
            return !i->elseBlock.empty() && alwaysReturns(i->thenBlock) && alwaysReturns(i->elseBlock);
        }
        return false;
    }

    // the function whose body is being typed, for return statements
    int retFn_ = 0;

    void function(const std::vector<Param>& params, const std::vector<Ty>* paramTypes, const std::vector<StmtPtr>& body,
                  int fnId, TypeSet declRet, bool hasDeclRet) {
        int savedFn = curFn_, savedRet = retFn_;
        curFn_ = fnId; retFn_ = fnId;
        scopes_.push_back(Scope{{}, fnId, true});
        for (size_t i = 0; i < params.size(); ++i) {
            Var v;
            v.fn = fnId;
            v.declared = params[i].explicitType;
            Ty pt = (paramTypes && i < paramTypes->size()) ? (*paramTypes)[i] : Ty(params[i].type, params[i].elem);
            v.type = pt.t; v.el = pt.el; v.soft = pt.soft;
            if (v.declared) v.hint = params[i].type;
            // a rest parameter holds an array of what the written type says each argument is
            if (params[i].variadic) { v.type = kArr; v.hint = kAny; v.el = params[i].explicitType ? makeElem(params[i].type, params[i].elem) : nullptr; v.soft = false; }
            scopes_.back().vars[params[i].name] = v;
        }
        for (auto& s : body) stmt(s.get());
        // falling off the end returns 0
        if (fnId > 0 && !alwaysReturns(body)) fns_[(size_t)fnId].retNow = joinTy(fns_[(size_t)fnId].retNow, Ty(kFloat));
        scopes_.pop_back();
        (void)declRet; (void)hasDeclRet;
        curFn_ = savedFn; retFn_ = savedRet;
    }

    void stmt(const Stmt* s) {
        switch (s->kind) {
            case Stmt::Kind::Assign: assign(static_cast<const AssignStmt*>(s)); break;
            case Stmt::Kind::MultiAssign: {
                auto* m = static_cast<const MultiAssignStmt*>(s);
                Ty vt = exprTy(m->value.get());
                // unpacking an array gives its elements
                const Ty each = vt.t.mask == kBitArr ? elemOf(vt) : Ty(kAny);
                for (auto& n : m->names) {
                    if (Var* old = lookup(n); old && !old->late) continue;
                    Var v; v.fn = curFn_; v.type = each.t; v.el = each.el; v.soft = each.soft;
                    fnRoot().vars[n] = v;
                }
                break;
            }
            case Stmt::Kind::Local: local(static_cast<const LocalStmt*>(s)); break;
            case Stmt::Kind::If: {
                auto* i = static_cast<const IfStmt*>(s);
                expr(i->cond.get());
                block(i->thenBlock);
                block(i->elseBlock);
                break;
            }
            case Stmt::Kind::Fn: {
                auto* f = static_cast<const FnStmt*>(s);
                int id = fnOfStmt_[f];
                FnInfo& info = fns_[(size_t)id];
                Var fv; fv.type = kFn; fv.declared = true; fv.fn = curFn_; fv.declFn = id + 1;
                scopes_.back().vars[f->name] = fv;
                function(f->params, &info.params, f->body, id, info.declRet.t, info.hasDeclRet);
                break;
            }
            case Stmt::Kind::Return: {
                auto* r = static_cast<const ReturnStmt*>(s);
                const bool wantRet = retFn_ > 0 && fns_[(size_t)retFn_].hasDeclRet && fns_[(size_t)retFn_].declRet.el;
                const size_t errsBefore = errors_.size();
                Ty t = r->value ? exprTy(r->value.get(), wantRet ? &fns_[(size_t)retFn_].declRet : nullptr) : Ty(kFloat);
                const bool valueBad = errors_.size() != errsBefore;
                if (retFn_ > 0) {
                    FnInfo& f = fns_[(size_t)retFn_];
                    f.retNow = joinTy(f.retNow, t);
                    if (f.hasDeclRet && !valueBad && differ(f.declRet, t))
                        error(r->value ? r->value->range : s->range, "'" + f.decl->name + "' declared return type " + tyName(f.declRet) + " but returns " + tyName(t));
                }
                break;
            }
            case Stmt::Kind::While: {
                auto* w = static_cast<const WhileStmt*>(s);
                expr(w->cond.get());
                block(w->body);
                break;
            }
            case Stmt::Kind::For: {
                auto* f = static_cast<const ForStmt*>(s);
                Ty it = exprTy(f->iterable.get());
                scopes_.push_back(Scope{{}, curFn_, false});
                // an array gives its elements, a string its characters, a map its keys (and with two names, the values too)
                auto bindVar = [&](const std::string& name, const Ty& ty) {
                    Var v; v.fn = curFn_; v.type = ty.t; v.el = ty.el; v.soft = ty.soft;
                    scopes_.back().vars[name] = v;
                };
                const bool two = !f->keyName.empty();
                Ty first = kAny, second = kAny;
                if (subsetOf(it.t, kBitStr)) first = Ty(kStr);
                else if (it.t.mask == kBitArr) first = elemOf(it);
                else if (it.t.mask == kBitMap) { first = Ty(kStr); second = elemOf(it); }
                if (two) { if (it.t.mask == kBitMap) { bindVar(f->keyName, first); bindVar(f->varName, second); } else { bindVar(f->keyName, Ty(kAny)); bindVar(f->varName, Ty(kAny)); } }
                else bindVar(f->varName, first);
                for (auto& st : f->body) stmt(st.get());
                scopes_.pop_back();
                break;
            }
            case Stmt::Kind::Import: {
                auto* im = static_cast<const ImportStmt*>(s);
                if (im->pathExpr) expr(im->pathExpr.get());
                importNames(im);
                break;
            }
            case Stmt::Kind::Expr: expr(static_cast<const ExprStmt*>(s)->expr.get()); break;
            case Stmt::Kind::Try: {
                auto* t = static_cast<const TryStmt*>(s);
                block(t->tryBlock);
                scopes_.push_back(Scope{{}, curFn_, false});
                Var e; e.fn = curFn_; e.type = kMap;
                scopes_.back().vars[t->catchVar] = e;
                for (auto& st : t->catchBlock) stmt(st.get());
                scopes_.pop_back();
                break;
            }
            case Stmt::Kind::Break: case Stmt::Kind::Continue: break;
        }
    }

    // `import "x.embr"` makes the names x makes at its top level, with the types x gave them
    std::unordered_map<std::string, std::pair<TypeSet, TypeElem>> importedFn_;   // an imported function, by name: what it returns
    void importNames(const ImportStmt* im) {
        if (!opt_.importModule || !im->pathExpr || im->pathExpr->kind != Expr::Kind::String) return;
        const std::string& path = static_cast<const StringExpr*>(im->pathExpr.get())->v;
        if (path.find(".embr") == std::string::npos) return;
        const ModuleExports* ex = opt_.importModule(path);
        if (!ex) return;
        for (const auto& [n, e] : *ex) {
            if (e.isLocal) continue;
            Var v; v.fn = 0; v.type = e.isFn ? kFn : e.type; v.el = e.el;
            scopes_.front().vars[n] = v;
            if (e.isFn) importedFn_[n] = {e.ret, e.retEl};
        }
    }

    void assign(const AssignStmt* a) {
        if (a->target->kind != Expr::Kind::Var) {
            exprTy(a->target.get());
            // `xs[i] = v` into an array or map whose elements are known
            Ty o(kAny), wantEl(kAny);
            bool known = false;
            if (a->target->kind == Expr::Kind::Index) {
                o = exprTy(static_cast<const IndexExpr*>(a->target.get())->object.get());
                known = singleContainer(o.t) && o.el && !isProtoKey(static_cast<const IndexExpr*>(a->target.get())->index.get());
                if (known) wantEl = Ty(o.el->t, detail(o.el));
            }
            Ty t = exprTy(a->value.get(), known && wantEl.el ? &wantEl : nullptr);
            if (known && differ(wantEl, t))
                error(a->value->range, "cannot put " + tyName(t) + " into " + tyName(o) + ", whose elements are " + tyName(o.el->t, detail(o.el)));
            return;
        }
        const std::string& name = static_cast<const VarExpr*>(a->target.get())->name;
        Var* v = lookup(name);
        Ty wantV = (v && !v->late && singleContainer(v->type) && v->el) ? Ty(v->type, v->el) : Ty(kAny);
        const size_t errsBefore = errors_.size();
        Ty t = exprTy(a->value.get(), wantV.el ? &wantV : nullptr);
        const bool valueBad = errors_.size() != errsBefore;   // the literal already said what is wrong with it
        v = lookup(name);
        if (!v || v->late) {          // the first assignment makes the variable, and fixes its type
            Var nv; nv.fn = curFn_; nv.type = t.t; nv.el = noSig(t.el); nv.soft = t.soft; nv.isConst = a->isConst;
            const bool sure = singleTag(t.t) && !t.soft;
            nv.hint = sure ? t.t : kAny;
            a->storeHint = nv.hint; a->storeProven = sure;
            fnRoot().vars[name] = nv;
            return;
        }
        if (v->isConst) { error(a->target->range, "cannot assign to '" + name + "', which is a const"); return; }
        // a write from another function (a closure writing a captured variable, or a global) is not tested by the VM here, and its store
        // carries no type: the variable it names at run time may not be the one the checker found. a top-level variable made before this
        // function is still a mistake to give the wrong type
        if (v->fn != curFn_) {
            if (v->fn == 0 && !v->soft && !isAnyT(v->type) && differ(Ty(v->type, v->el), t))
                error(a->value->range, "cannot assign " + tyName(t) + " to '" + name + "', which holds " + tyName(v->type, v->el));
            return;
        }
        a->storeHint = v->hint;
        a->storeProven = !isAnyT(v->hint) && !t.soft && t.t.mask != 0 && (t.t.mask & ~v->hint.mask) == 0;
        if (!valueBad && !isAnyT(v->type) && differ(Ty(v->type, v->el), t))
            error(a->value->range, "cannot assign " + tyName(t) + " to '" + name + "'" +
                  (v->declared ? " declared as " : ", which holds ") + tyName(v->type, v->el));
    }

    void local(const LocalStmt* l) {
        const bool single = l->targets.size() == 1 && !l->bracketed;
        Ty wantT = single ? Ty(l->targets[0].type, l->targets[0].elem) : Ty(kAny);
        const size_t errsBefore = errors_.size();
        Ty t = exprTy(l->value.get(), single && l->targets[0].explicitType && wantT.el ? &wantT : nullptr);
        const bool valueBad = errors_.size() != errsBefore;
        for (auto& tg : l->targets) {
            Var v; v.fn = curFn_;
            v.declared = tg.explicitType;
            if (v.declared) {
                v.type = tg.type;
                v.el = tg.elem;
                v.hint = tg.type;
                if (single && !valueBad && differ(Ty(tg.type, tg.elem), t))
                    error(l->value->range, "cannot assign " + tyName(t) + " to '" + tg.name + "' declared as " + tyName(tg.type, tg.elem));
            } else {
                const Ty each = (!single && t.t.mask == kBitArr) ? elemOf(t) : Ty(kAny);   // unpacking an array gives its elements
                v.type = single ? t.t : each.t;
                v.el = noSig(single ? t.el : each.el);
                v.soft = single ? t.soft : each.soft;
                v.hint = (single && singleTag(t.t) && !t.soft) ? t.t : kAny;
                if (single) l->inferredHint = v.hint;
            }
            v.fromLocal = true;
            scopes_.back().vars[tg.name] = v;
        }
    }

    // the type an expression can have. reports the operations that can never work
    TypeSet expr(const Expr* e) { return exprTy(e).t; }
    // want: what the context asks for, when it is an array or a map with a known element type. a literal then checks each of its elements
    Ty exprTy(const Expr* e, const Ty* want = nullptr) {
        Ty r = exprTyImpl(e, want);
        // only the last, reporting pass: its types are the settled ones
        if (report_ && opt_.typesOut) (*opt_.typesOut)[e] = ExprType{r.t, r.el, singleTag(r.t) && !r.soft};
        return r;
    }
    Ty exprTyImpl(const Expr* e, const Ty* want) {
        switch (e->kind) {
            case Expr::Kind::Number: return Ty(kFloat);
            case Expr::Kind::Int:    return Ty(kInt);
            case Expr::Kind::String: return Ty(kStr);
            case Expr::Kind::Var: {
                const std::string& n = static_cast<const VarExpr*>(e)->name;
                if (Var* v = lookup(n)) {
                    if (v->declFn && v->type.mask == kBitFn && !v->el) { const FnStmt* d = fns_[(size_t)v->declFn - 1].decl; return fnSigTy(d->params, d->returnType, d->returnElem); }
                    return Ty(v->type, v->el, v->soft);
                }
                return Ty(kAny);
            }
            case Expr::Kind::Array: {
                auto* a = static_cast<const ArrayExpr*>(e);
                static const char* const what[] = {"start", "second number", "end"};
                Ty acc(TypeSet(0));
                const bool checkEls = want && !a->isRange && want->t.mask == kBitArr && want->el;
                const Ty wantEl = checkEls ? Ty(want->el->t, detail(want->el)) : Ty(kAny);
                for (size_t i = 0; i < a->elements.size(); ++i) {
                    Ty et = exprTy(a->elements[i].get(), checkEls ? &wantEl : nullptr);
                    if (checkEls && differ(wantEl, et))
                        error(a->elements[i]->range, "cannot put " + tyName(et) + " into " + tyName(want->t, want->el) + ", whose elements are " + tyName(want->el->t, detail(want->el)));
                    acc = joinTy(acc, et);
                    // the numbers of a range have to be numbers
                    if (a->isRange && notNumeric(et.t))
                        error(a->elements[i]->range, std::string("the ") + what[a->elements.size() == 3 ? i : (i == 0 ? 0 : 2)] + " of a range has to be a number, not " + tn(et.t));
                }
                if (a->isRange) return Ty(kArr, makeElem(kInt, nullptr));
                // an empty literal says nothing about what the array will hold
                return a->elements.empty() ? Ty(kArr) : Ty(kArr, makeElem(acc.t, acc.el));
            }
            case Expr::Kind::Map: {
                Ty acc(TypeSet(0));
                const auto& entries = static_cast<const MapExpr*>(e)->entries;
                const bool checkEls = want && want->t.mask == kBitMap && want->el;
                const Ty wantEl = checkEls ? Ty(want->el->t, detail(want->el)) : Ty(kAny);
                for (auto& kv : entries) {
                    expr(kv.first.get());
                    // `__proto__` is the map's prototype, not one of its values
                    const bool proto = isProtoKey(kv.first.get());
                    Ty vt = exprTy(kv.second.get(), checkEls && !proto ? &wantEl : nullptr);
                    if (proto) continue;
                    if (checkEls && differ(wantEl, vt))
                        error(kv.second->range, "cannot put " + tyName(vt) + " into " + tyName(want->t, want->el) + ", whose elements are " + tyName(want->el->t, detail(want->el)));
                    acc = joinTy(acc, vt);
                }
                return entries.empty() ? Ty(kMap) : Ty(kMap, makeElem(acc.t, acc.el));
            }
            case Expr::Kind::Lambda: {
                auto* l = static_cast<const LambdaExpr*>(e);
                function(l->params, nullptr, l->ownedBody, 0, l->returnType, false);
                return fnSigTy(l->params, l->returnType, l->returnElem);
            }
            case Expr::Kind::Ref: return exprTy(static_cast<const RefExpr*>(e)->target.get());   // `&a` is a, handed over to be changed
            case Expr::Kind::And: case Expr::Kind::Or: {
                auto* b = static_cast<const LogicExpr*>(e);
                return joinTy(exprTy(b->left.get()), exprTy(b->right.get()));
            }
            case Expr::Kind::Unary: return Ty(unary(static_cast<const UnaryExpr*>(e)));
            case Expr::Kind::Binary: return binary(static_cast<const BinaryExpr*>(e));
            case Expr::Kind::Index: return index(static_cast<const IndexExpr*>(e));
            case Expr::Kind::Call: return call(static_cast<const CallExpr*>(e));
        }
        return Ty(kAny);
    }

    static bool isProtoKey(const Expr* k) { return k && k->kind == Expr::Kind::String && static_cast<const StringExpr*>(k)->v == "__proto__"; }
    // something that can't take part in arithmetic: it has no number in it and isn't a pointer (a pointer can have a
    // user-defined operator)
    static bool notNumeric(TypeSet t) { return !isAnyT(t) && (t.mask & (kBitNumber | kBitInt | kBitPtr)) == 0; }

    // what a bit operator can't take: no int in it and not a pointer (a pointer can have a user-defined operator). a float is
    // refused, a `num` might hold an int and is checked when the script runs
    static bool notWhole(TypeSet t) { return !isAnyT(t) && (t.mask & (kBitInt | kBitPtr)) == 0; }

    TypeSet unary(const UnaryExpr* u) {
        TypeSet t = expr(u->right.get());
        if (u->op == "-") {
            if (notNumeric(t)) { error(u->range, "cannot negate " + tn(t)); return kNum; }
            return subsetOf(t, kBitNumber | kBitInt) ? t : kAny;
        }
        if (u->op == "~") {
            if (notWhole(t)) { error(u->range, "cannot use " + tn(t) + " with '~', it needs an int"); return kInt; }
            return (t.mask & kBitPtr) ? kAny : kInt;
        }
        return kInt;   // `!x` is the int 0 or 1
    }

    Ty binary(const BinaryExpr* b) {
        Ty lt = exprTy(b->left.get()), rt = exprTy(b->right.get());
        TypeSet l = lt.t, r = rt.t;
        const std::string& op = b->op;
        if (op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">=") return Ty(kInt);
        if (op == "-" || op == "*" || op == "/" || op == "%") {
            if (notNumeric(l)) { error(b->range, "cannot use " + tn(l) + " with '" + op + "'"); return Ty(kNum); }
            if (notNumeric(r)) { error(b->range, "cannot use " + tn(r) + " with '" + op + "'"); return Ty(kNum); }
            if (!subsetOf(l, kBitNumber | kBitInt) || !subsetOf(r, kBitNumber | kBitInt)) return Ty(kAny);
            if (op == "/") return Ty(kFloat);
            if (op == "%") return Ty((subsetOf(l, kBitInt) && subsetOf(r, kBitInt)) ? kInt : kNum);
            // an int result that does not fit becomes a float, so int op int is a number
            return Ty(kNum);
        }
        if (op == "&" || op == "|" || op == "^" || op == "<<" || op == ">>") {
            if (notWhole(l)) { error(b->range, "cannot use " + tn(l) + " with '" + op + "', it needs ints"); return Ty(kInt); }
            if (notWhole(r)) { error(b->range, "cannot use " + tn(r) + " with '" + op + "', it needs ints"); return Ty(kInt); }
            return Ty(((l.mask | r.mask) & kBitPtr) ? kAny : kInt);
        }
        if (op == "+") {
            if (subsetOf(l, kBitStr) || subsetOf(r, kBitStr)) return Ty(kStr);
            if (subsetOf(l, kBitNumber | kBitInt) && subsetOf(r, kBitNumber | kBitInt)) return Ty(kNum);
            if (subsetOf(l, kBitArr) && subsetOf(r, kBitArr)) return Ty(kArr, joinElem(lt.el, rt.el));
            return Ty(kAny);
        }
        return Ty(kAny);
    }

    Ty index(const IndexExpr* i) {
        Ty ot = exprTy(i->object.get());
        TypeSet o = ot.t, k = expr(i->index.get());
        if (!isAnyT(o) && (o.mask & (kBitStr | kBitArr | kBitMap | kBitPtr)) == 0) {
            error(i->range, "cannot index " + tn(o));
            return Ty(kAny);
        }
        if (subsetOf(o, kBitArr | kBitStr) && !isAnyT(k) && (k.mask & (kBitNumber | kBitInt | kBitPtr)) == 0)
            error(i->index->range, "an index into " + tn(o) + " has to be a number, not " + tn(k));
        if (subsetOf(o, kBitMap) && !isAnyT(k) && (k.mask & (kBitStr | kBitPtr)) == 0)
            error(i->index->range, "a map key has to be a string, not " + tn(k));
        if (subsetOf(o, kBitStr)) return Ty(kStr);
        // a key a map does not have is looked up in its `__proto__`, so what `m[k]` gives is not always one of the map's own values
        if (o == kArr) return elemOf(ot);
        return Ty(kAny);
    }

    // the function type a lambda or a declared function has, from the types written in its parameters and result. a function with an optional
    // or rest parameter takes more than one number of arguments, so it gets no signature
    Ty fnSigTy(const std::vector<Param>& ps, TypeSet ret, const TypeElem& retEl) {
        auto a = std::make_shared<TypeAnn>();
        a->t = kFn; a->isFn = true;
        for (const Param& p : ps) {
            if (p.optional || p.variadic) return Ty(kFn);
            a->params.push_back(p.explicitType ? makeAnn(p.type, p.elem) : makeAnn(kAny, nullptr));
        }
        if (!ret.isAny() || retEl) a->ret = makeAnn(ret, retEl);
        return Ty(kFn, a);
    }
    static std::string count(size_t n) { return std::to_string(n) + (n == 1 ? " argument" : " arguments"); }
    // a call of something that holds a function with a written type: the arguments have to fit it, and the result is what it says
    Ty callTyped(const CallExpr* c, const std::string& what, const TypeAnn& sig, const std::vector<Ty>& argTys, const std::vector<bool>& argBad) {
        if (c->args.size() != sig.params.size()) {
            error(c->range, what + " is a " + tyName(kFn, std::make_shared<TypeAnn>(sig)) + ", which takes " + count(sig.params.size()) + ", not " + std::to_string(c->args.size()));
        } else {
            for (size_t i = 0; i < c->args.size(); ++i) {
                const TypeElem& pt = sig.params[i];
                if (!(i < argBad.size() && argBad[i]) && differ(Ty(pt->t, detail(pt)), argTys[i]))
                    error(c->args[i]->range, "argument " + std::to_string(i + 1) + " to " + what + ": expected " + annName(pt) + " but got " + tyName(argTys[i]));
            }
        }
        return sig.ret ? Ty(sig.ret->t, detail(sig.ret)) : Ty(kAny);
    }

    Ty call(const CallExpr* c) {
        std::vector<bool> argBad;
        std::vector<TypeSet> args;
        std::vector<Ty> argTys;
        // what the call names, so a literal argument can be checked against the parameter it goes to
        const FnInfo* wantFn = nullptr; const Value::Callable* wantNative = nullptr;
        if (c->callee->kind == Expr::Kind::Var) {
            const std::string& cn = static_cast<const VarExpr*>(c->callee.get())->name;
            auto wf = fnByName_.find(cn);
            if (wf != fnByName_.end()) wantFn = &fns_[(size_t)wf->second];
            else if (!lookup(cn) && opt_.interp && bindings_[cn] == 0) {
                const Value* fv = opt_.interp->find(cn);
                if (fv && fv->isCallable() && fv->asCallable().isNative() && !fv->asCallable().sig.empty()) wantNative = &fv->asCallable();
            }
        }
        for (size_t i = 0; i < c->args.size(); ++i) {
            Ty want(kAny);
            if (wantFn) {
                const auto& ps = wantFn->decl->params;
                if (i < ps.size() && !ps[i].variadic && wantFn->explicitParam[i] && ps[i].elem) want = Ty(ps[i].type, ps[i].elem);
            } else if (wantNative) {
                const auto& sig = wantNative->sig;
                const Param* p = (i < sig.size() && !sig[i].variadic) ? &sig[i] : nullptr;
                if (p && p->elem) want = Ty(p->type, p->elem);
                else if (p && p->elemOf >= 0 && (size_t)p->elemOf < i && singleContainer(argTys[(size_t)p->elemOf].t) && argTys[(size_t)p->elemOf].el)
                    want = Ty(argTys[(size_t)p->elemOf].el->t, argTys[(size_t)p->elemOf].el->el);
            }
            const size_t before = errors_.size();
            argTys.push_back(exprTy(c->args[i].get(), want.el ? &want : nullptr));
            args.push_back(argTys.back().t);
            argBad.resize(argTys.size()); argBad[i] = errors_.size() != before;   // already said what is wrong with it
        }
        if (c->callee->kind != Expr::Kind::Var) {
            Ty cty = exprTy(c->callee.get());
            TypeSet ct = cty.t;
            if (notCallable(ct)) error(c->range, "cannot call " + tn(ct));
            if (ct.mask == kBitFn && cty.el && cty.el->isFn) return callTyped(c, "this function", *cty.el, argTys, argBad);
            return Ty(kAny);
        }
        const std::string& name = static_cast<const VarExpr*>(c->callee.get())->name;
        auto fi = fnByName_.find(name);
        Var* v = lookup(name);
        // `abs(x)` where `abs` is an overload group and nothing in this file is called `abs`
        if (fi == fnByName_.end() && !v && bindings_[name] == 0 && hasOverloadGroup(name))
            return Ty(overloadCall(c, name, args));
        if (fi == fnByName_.end()) {
            if (v && notCallable(v->type)) error(c->range, "cannot call '" + name + "', which holds " + tn(v->type));
            if (v && v->type.mask == kBitFn && v->el && v->el->isFn) return callTyped(c, "'" + name + "'", *v->el, argTys, argBad);
            if (v) { auto im = importedFn_.find(name); if (im != importedFn_.end()) return Ty(im->second.first, im->second.second); }
            // a native this interpreter already has, with a signature, and a name the file never binds itself
            if (!v && opt_.interp && bindings_[name] == 0) {
                const Value* fv = opt_.interp->find(name);
                if (fv && fv->isCallable() && fv->asCallable().isNative()) {
                    const Value::Callable& nc = fv->asCallable();
                    if (!nc.sig.empty()) nativeCall(c, name, nc.sig, argTys, argBad);
                    // the argument it hands back, or what it always returns, any when it varies
                    if (nc.returnsLike >= 0 && (size_t)nc.returnsLike < argTys.size()) {
                        Ty r = argTys[(size_t)nc.returnsLike];
                        if (singleContainer(r.t) || subsetOf(r.t, kBitStr)) { r.soft = false; return r; }   // an array, a map or a string comes back as it is
                    }
                    return Ty(nc.returns, nc.returnsElem);
                }
            }
            return Ty(kAny);
        }
        FnInfo& f = fns_[(size_t)fi->second];
        const auto& ps = f.decl->params;
        const bool variadic = !ps.empty() && ps.back().variadic;
        const size_t fixed = variadic ? ps.size() - 1 : ps.size();
        size_t required = 0;                                  // a parameter with a default value may be left out (aembr)
        for (size_t i = 0; i < fixed; ++i) if (!ps[i].optional) required = i + 1;
        if (args.size() < required)
            error(c->range, "'" + name + "' expects " + (variadic || required < fixed ? "at least " : "") + std::to_string(required) +
                  " args, got " + std::to_string(args.size()));
        f.called = true;
        for (size_t i = 0; i < fixed && i < args.size(); ++i) {
            f.callArgs[i] = joinTy(f.callArgs[i], argTys[i]);
            if (f.explicitParam[i] && !(i < argBad.size() && argBad[i]) && differ(Ty(ps[i].type, ps[i].elem), argTys[i]))
                error(c->args[i]->range, "argument '" + ps[i].name + "' to '" + name + "': expected " + tyName(ps[i].type, ps[i].elem) +
                      " but got " + tyName(argTys[i]));
        }
        // a variadic parameter takes whatever is left, so it can't be narrowed
        for (size_t i = fixed; i < ps.size(); ++i) f.callArgs[i] = joinTy(f.callArgs[i], Ty(kAny));
        return f.inferRet;
    }
    // the same rules the VM applies to a native call (checkNativeSig in vm.h), for what can be told from the code
    void nativeCall(const CallExpr* c, const std::string& name, const std::vector<Param>& sig, const std::vector<Ty>& args, const std::vector<bool>& argBad) {
        size_t minArgs = 0;
        bool   hasVariadic = false;
        for (const auto& p : sig) {
            if (p.variadic) { hasVariadic = true; break; }
            if (!p.optional) ++minArgs;
        }
        const size_t maxArgs = hasVariadic ? SIZE_MAX : sig.size();
        if (args.size() < minArgs)
            error(c->range, "too few arguments to '" + name + "': expected " + std::to_string(minArgs) + " but got " + std::to_string(args.size()));
        if (args.size() > maxArgs)
            error(c->range, "too many arguments to '" + name + "': expected at most " + std::to_string(maxArgs) + " but got " + std::to_string(args.size()));
        for (size_t i = 0; i < args.size(); ++i) {
            const Param* p = (i < sig.size() && !sig[i].variadic) ? &sig[i] : nullptr;
            if (!p) for (auto it = sig.rbegin(); it != sig.rend(); ++it) if (it->variadic) { p = &*it; break; }
            const bool ref = c->args[i]->kind == Expr::Kind::Ref;
            if (p && p->inout != ref) {
                error(c->args[i]->range, ref ? "argument '" + p->name + "' to '" + name + "' is not an in-out parameter, so it can't take '&'"
                                             : "argument '" + p->name + "' to '" + name + "' is changed in place, so pass it as &name");
                continue;
            }
            if (!p || ref) continue;
            // an argument that has to fit what another one holds: `push(list, x)` with list an arr<int> takes an int
            if (p->elemOf >= 0 && (size_t)p->elemOf < args.size()) {
                const Ty& holder = args[(size_t)p->elemOf];
                if (singleContainer(holder.t) && holder.el && differ(Ty(holder.el->t, detail(holder.el)), args[i]))
                    error(c->args[i]->range, "argument '" + p->name + "' to '" + name + "': the " + tyName(holder) + " holds " + tyName(holder.el->t, detail(holder.el)) +
                          ", not " + tyName(args[i]));
                continue;
            }
            if (p->type.isAny() || (i < argBad.size() && argBad[i])) continue;
            if (differ(Ty(p->type, p->elem), args[i]))
                error(c->args[i]->range, "argument '" + p->name + "' to '" + name + "': expected " + tyName(p->type, p->elem) + " but got " + tyName(args[i]));
        }
    }

    // ---- overload groups --------------------------------------------------------------------------------------------------
    bool hasOverloadGroup(const std::string& base) const {
        if (scriptGroups_.count(base)) return true;
        return opt_.interp && !opt_.interp->overloadGroup(base).empty();
    }
    // a plain `abs` in the same file as the group `abs` (its members here, or natives the interpreter already has)
    void checkOverloadConflicts() {
        if (opt_.interp)
            for (auto& [name, count] : bindings_)
                if (count > 0 && !scriptGroups_.count(name) && !opt_.interp->overloadGroup(name).empty())
                    error({}, "'" + name + "' is a plain name in this file, but it is also the overload group of " + opt_.interp->overloadGroup(name).front().second);
        for (auto& [base, members] : scriptGroups_) {
            if (bindings_[base] == 0) continue;
            const FnStmt* d = groupDecl_[base];
            error(d->range, "'" + base + "' is both a plain name in this file and the overload group of " + members.front() +
                  (members.size() > 1 ? " and " + std::to_string(members.size() - 1) + " more" : std::string()));
        }
    }
    struct Cand {
        std::string          name;
        std::vector<TypeSet> params;       // the fixed parameters
        size_t               minArgs = 0;
        bool                 variadic = false;
        TypeSet              ret = kAny;
        int                  fn = 0;       // index in fns_ for a function of this file, 0 for a native
    };
    std::vector<Cand> overloadCandidates(const std::string& base) {
        std::vector<Cand> out;
        std::unordered_set<std::string> seen;
        auto sit = scriptGroups_.find(base);
        if (sit != scriptGroups_.end())
            for (const std::string& nm : sit->second) {
                auto it = fnByName_.find(nm);
                if (it == fnByName_.end() || !seen.insert(nm).second) continue;
                const FnInfo& f = fns_[(size_t)it->second];
                Cand c; c.name = nm; c.fn = f.id;
                const auto& ps = f.decl->params;
                c.variadic = !ps.empty() && ps.back().variadic;
                const size_t fixed = c.variadic ? ps.size() - 1 : ps.size();
                for (size_t i = 0; i < fixed; ++i) c.params.push_back(f.explicitParam[i] ? ps[i].type : kAny);
                c.minArgs = fixed;
                c.ret = f.hasDeclRet ? f.declRet.t : f.inferRet.t;
                out.push_back(std::move(c));
            }
        if (opt_.interp)
            for (const auto& [num, nm] : opt_.interp->overloadGroup(base)) {
                (void)num;
                if (!seen.insert(nm).second) continue;
                const Value* fv = opt_.interp->find(nm);
                // a native with no signature says nothing about the types it takes, so it can't take part (it is still a plain function)
                if (!fv || !fv->isCallable() || !fv->asCallable().isNative() || fv->asCallable().sig.empty()) continue;
                Cand c; c.name = nm;
                for (const Param& p : fv->asCallable().sig) {
                    if (p.variadic) { c.variadic = true; continue; }
                    c.params.push_back(p.type);
                    if (!p.optional) c.minArgs = c.params.size();
                }
                c.ret = fv->asCallable().returns;
                out.push_back(std::move(c));
            }
        return out;
    }
    static std::string candText(const Cand& c) {
        std::string t = c.name + "(";
        for (size_t i = 0; i < c.params.size(); ++i) t += (i ? ", " : "") + tn(c.params[i]);
        if (c.variadic) t += c.params.empty() ? "..." : ", ...";
        return t + ")";
    }
    static bool exactType(TypeSet t) { return singleTag(t) || (t.mask == kNum.mask); }

    // the call `base(args)`. when every argument has one definite type and one member is the closest fit, the call is resolved to
    // it (CallExpr::resolved) and typed as that function. when no member could take the arguments it is an error. when the types
    // are not known well enough, the runtime dispatcher chooses, and the result is whatever any possible member returns
    TypeSet overloadCall(const CallExpr* c, const std::string& base, const std::vector<TypeSet>& args) {
        c->resolved.clear();
        std::vector<Cand> cands = overloadCandidates(base);
        bool hasRef = false;
        for (auto& a : c->args) if (a->kind == Expr::Kind::Ref) hasRef = true;
        auto paramAt = [](const Cand& cd, size_t i) { return i < cd.params.size() ? cd.params[i] : kAny; };
        std::vector<const Cand*> possible;
        for (const Cand& cd : cands) {
            if (args.size() < cd.minArgs || (!cd.variadic && args.size() > cd.params.size())) continue;
            bool ok = true;
            for (size_t i = 0; i < args.size() && ok; ++i) if (disjoint(args[i], paramAt(cd, i))) ok = false;
            if (ok) possible.push_back(&cd);
        }
        if (possible.empty() && !hasRef && !cands.empty()) {
            std::string have, list;
            for (size_t i = 0; i < args.size(); ++i) have += (i ? ", " : "") + tn(args[i]);
            for (size_t i = 0; i < cands.size(); ++i) list += (i ? ", " : "") + candText(cands[i]);
            error(c->range, "no overload of '" + base + "' takes (" + have + "); candidates: " + list);
            return kAny;
        }
        bool exact = !hasRef;
        for (const TypeSet& a : args) if (!exactType(a)) exact = false;
        if (exact) {
            const Cand* best = nullptr; int bestScore = 1 << 30; bool tie = false;
            std::string tied;
            for (const Cand* cd : possible) {
                bool sure = true;
                int score = 0;
                for (size_t i = 0; i < args.size(); ++i) {
                    const TypeSet p = paramAt(*cd, i);
                    if ((args[i].mask & ~p.mask) != 0) sure = false;
                    score += __builtin_popcount(p.mask);
                }
                if (!sure) continue;
                if (score < bestScore) { best = cd; bestScore = score; tie = false; tied.clear(); }
                else if (score == bestScore) { tie = true; tied += ", " + candText(*cd); }
            }
            if (best && tie) {
                std::string have;
                for (size_t i = 0; i < args.size(); ++i) have += (i ? ", " : "") + tn(args[i]);
                error(c->range, "the call of '" + base + "' with (" + have + ") fits more than one overload equally well: " + candText(*best) + tied);
                return kAny;
            }
            if (best) {
                c->resolved = best->name;
                if (best->fn) {
                    FnInfo& f = fns_[(size_t)best->fn];
                    f.called = true;
                    for (size_t i = 0; i < args.size() && i < f.callArgs.size(); ++i) f.callArgs[i] = joinTy(f.callArgs[i], Ty(args[i]));
                }
                return best->ret;
            }
        }
        // the runtime dispatcher may call any of these with anything, so what their parameters were called with can't narrow them
        TypeSet r(0);
        for (const Cand* cd : possible) {
            r = joinT(r, cd->ret);
            if (cd->fn) {
                FnInfo& f = fns_[(size_t)cd->fn];
                f.called = true;
                for (auto& ca : f.callArgs) ca = joinTy(ca, Ty(kAny));
            }
        }
        return r.mask == 0 ? kAny : r;
    }
    static bool notCallable(TypeSet t) { return !isAnyT(t) && (t.mask & (kBitFn | kBitPtr)) == 0; }
};

} // namespace detail

inline std::vector<EmbrError> check(const std::vector<StmtPtr>& ast, const Options& opt = Options{}) {
    return detail::Checker(ast, opt).run();
}

} // namespace typecheck
} // namespace embr

#endif // EMBR_CORE_TYPECHECK_H
