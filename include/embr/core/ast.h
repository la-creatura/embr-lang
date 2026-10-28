#ifndef EMBR_CORE_AST_H
#define EMBR_CORE_AST_H

#include "diagnostics.h"
#include "types.h"
#include "fwd.h"

#include <cstdint>
#include <vector>
#include <memory>
#include <utility>
#include <string>
#include <unordered_set>

namespace embr {

struct Expr {
    enum class Kind { Number, Int, String, Var, Unary, Binary, Call, Array, Map, Index, Lambda, And, Or, Ref };
    Kind        kind;
    SourceRange range;
    // height of the expression tree rooted here (a leaf is 1). set by the parser on every compound
    // node and capped (Parser::kMaxExprDepth) so the recursive evaluators, the VM compiler, and the
    // nodes' own destructors can't be driven into a native stack overflow by a very long operator
    // chain or deeply nested input
    int         depth = 1;
    explicit Expr(Kind k) : kind(k) {}
    virtual ~Expr() = default;
};

struct NumberExpr : Expr { double      v; NumberExpr(double      v,SourceRange r):Expr(Kind::Number),v(v)           {range=r;} };
// a numeric literal with no decimal point
struct IntExpr    : Expr { int64_t     v; IntExpr(int64_t        v,SourceRange r):Expr(Kind::Int),   v(v)           {range=r;} };
struct StringExpr : Expr { std::string v; StringExpr(std::string v,SourceRange r):Expr(Kind::String),v(std::move(v)){range=r;} };
struct VarExpr    : Expr { std::string name; VarExpr(std::string n,SourceRange r):Expr(Kind::Var),name(std::move(n)){range=r;} };

struct UnaryExpr  : Expr { std::string op;      ExprPtr right;            UnaryExpr():Expr(Kind::Unary) {} };
struct BinaryExpr : Expr { std::string op;      ExprPtr left,right;      BinaryExpr():Expr(Kind::Binary){} };
struct LogicExpr  : Expr { ExprPtr left, right;                           LogicExpr(Kind k) : Expr(k)   {} };

struct CallExpr   : Expr {
    ExprPtr callee;      std::vector<ExprPtr> args;
    // set by the type checker when `callee` names an overload group (`abs`) and the argument types pick one member (`abs__2`):
    // the compiler then calls that function directly. empty: call what the name says (for a group, its runtime dispatcher)
    mutable std::string resolved;
    CallExpr():Expr(Kind::Call)  {}
};
struct ArrayExpr  : Expr {
    std::vector<ExprPtr> elements;
    // `[a...b]` and `[a, b...c]`: a range, not a list. `elements` are then the two or three numbers written (start, [second], end) and the
    // array is made lazily, see Value::makeRange. the end is included, and the step is second - start (1 when there is no second)
    bool isRange = false;
    ArrayExpr():Expr(Kind::Array) {}
};
struct MapExpr    : Expr { std::vector<std::pair<ExprPtr,ExprPtr>> entries; MapExpr():Expr(Kind::Map)   {} };
struct IndexExpr  : Expr {
    ExprPtr object,index;
    IndexExpr():Expr(Kind::Index) {}
};
// `&target` in a call argument: a variable, or an element of one (`&a`, `&a[i]`, `&a[i]["k"]`). only the parser
// makes one, and only as a direct call argument. the callee must be a native with an in-out parameter there
struct RefExpr    : Expr { ExprPtr target;                                RefExpr():Expr(Kind::Ref) {} };

// body heap-owned unlike FnStmt owned by program vector
struct LambdaExpr : Expr {
    std::vector<Param>          params;
    TypeSet                     returnType = TypeSet::Any();
    bool                        returnWritten = false;   // `-> type` was written (so `-> any` can be told from no return type)
    TypeElem                    returnElem;
    const std::vector<StmtPtr>* body;
    std::vector<StmtPtr>        ownedBody;
    std::vector<SourceRange>    paramRanges;   // where each parameter's name is written, for editor tools
    LambdaExpr() : Expr(Kind::Lambda) {}
};

struct Stmt {
    enum class Kind { Assign, MultiAssign, Local, If, Fn, Return, While, For, Break, Continue, Import, Expr, Try };
    Kind kind; SourceRange range;
    explicit Stmt(Kind k) : kind(k) {}
    virtual ~Stmt() = default;
};

// what the type checker worked out about an assignment to a plain variable, for the compiler (see typecheck.h):
//   storeHint    the variable's exact type when it is known (a declared type, or the one type its first value had),
//                otherwise any. the store tests the value against it
//   storeProven  the value's type is inside storeHint, so the store needs no test
struct AssignStmt : Stmt {
    ExprPtr target,value;
    mutable TypeSet storeHint   = TypeSet::Any();
    mutable bool    storeProven = false;
    // `const name = expr`: the value is worked out when the script is compiled, and the name cannot be assigned again
    bool            isConst = false;
    AssignStmt():Stmt(Kind::Assign){}
};

// a, b, c = expr
// see LocalStmt for the declaring local equivalent, which additionally supports types
struct MultiAssignStmt : Stmt {
    std::vector<std::string> names;
    std::vector<SourceRange> nameRanges;   // where each name is written, for editor tools
    ExprPtr value;
    MultiAssignStmt():Stmt(Kind::MultiAssign){}
};

// one binding in a local declaration
// a name, plus an optional type constraint 
struct DestructureTarget {
    std::string name;
    TypeSet     type = TypeSet::Any();
    bool        explicitType = false;   // a type was written, even if it is `any`
    TypeElem    elem;                   // `arr<int>`: what the elements are
    SourceRange range;                  // where the name is written, for editor tools
};

// local [type] name = expr
// local [type] name1, [type] name2, ... = expr
// local [type] [name1, name2, ...] = expr
// targets.size()==1 && !bracketed for no unpacking
// everything else destructures value via the array/map unpacking rules (see UNPACK in backends/vm.h)
struct LocalStmt : Stmt {
    std::vector<DestructureTarget> targets;
    bool    bracketed = false;
    ExprPtr value;
    // for a single target without a written type: its exact type when the checker knows it, otherwise any
    mutable TypeSet inferredHint = TypeSet::Any();
    LocalStmt():Stmt(Kind::Local){}
};
struct IfStmt     : Stmt { ExprPtr cond; std::vector<StmtPtr> thenBlock,elseBlock; IfStmt():Stmt(Kind::If){} };
struct FnStmt     : Stmt {
    std::string        name;
    std::vector<Param> params;
    TypeSet            returnType = TypeSet::Any();
    bool               returnWritten = false;   // `-> type` was written (so `-> any` can be told from no return type)
    TypeElem           returnElem;
    std::vector<StmtPtr> body;
    // written as `name(params) = expr`. the compiler may write the expression into the calls of it, see the wiki's syntax page
    bool               isExprForm = false;
    // `const fn`: the compiler copies the body into each call of it that names it, see the wiki's syntax page
    bool               isTemplate = false;
    // where the name and each parameter's name are written, for editor tools (embr lsp)
    SourceRange              nameRange;
    std::vector<SourceRange> paramRanges;
    FnStmt():Stmt(Kind::Fn){}
};
struct ReturnStmt : Stmt { ExprPtr value; ReturnStmt():Stmt(Kind::Return){} };
struct WhileStmt  : Stmt { ExprPtr cond; std::vector<StmtPtr> body; WhileStmt():Stmt(Kind::While){} };
// for varName in iterable ... end
// for keyName, varName in iterable ... end
struct ForStmt : Stmt {
    std::string keyName;  // empty unless the two-variable map form was used
    std::string varName;
    ExprPtr iterable;
    std::vector<StmtPtr> body;
    SourceRange keyRange, varRange;   // where the names are written, for editor tools
    ForStmt():Stmt(Kind::For){}
};
struct BreakStmt    : Stmt { BreakStmt():Stmt(Kind::Break){} };
struct ContinueStmt : Stmt { ContinueStmt():Stmt(Kind::Continue){} };
// `c """ ... """`: C text for aembr to put in the compiled program as it is. `expr` is the StringExpr with the text. the VM refuses it
struct ExprStmt   : Stmt { ExprPtr expr; bool inlineC = false; bool cFileScope = false; std::string cFile; ExprStmt():Stmt(Kind::Expr){} };   // cFile: the file `const c import` read the text from, for the compiler's messages
// try ... catch e ... end
// catchVar is bound to the raised error's message for the duration of catchBlock only then goes out of scope
struct TryStmt : Stmt {
    std::vector<StmtPtr> tryBlock;
    std::string          catchVar;
    std::vector<StmtPtr> catchBlock;
    SourceRange          catchRange;   // where catchVar is written, for editor tools
    TryStmt():Stmt(Kind::Try){}
};
// import <expr>
// the path is an expression, not only a string literal, so a script can build it (`import "plugins/" + name`),
// like load_module() already allows. each backend evaluates it at runtime and raises if it isn't a string
struct ImportStmt : Stmt { ExprPtr pathExpr; ImportStmt():Stmt(Kind::Import){} };

// true when the expression holds a function value or a `&` argument
inline bool hasLambdaOrRef(const Expr* e) {
    switch (e->kind) {
    case Expr::Kind::Lambda: case Expr::Kind::Ref: return true;
    case Expr::Kind::Unary:  return hasLambdaOrRef(static_cast<const UnaryExpr*>(e)->right.get());
    case Expr::Kind::Binary: { auto* b = static_cast<const BinaryExpr*>(e); return hasLambdaOrRef(b->left.get()) || hasLambdaOrRef(b->right.get()); }
    case Expr::Kind::And: case Expr::Kind::Or: { auto* l = static_cast<const LogicExpr*>(e); return hasLambdaOrRef(l->left.get()) || hasLambdaOrRef(l->right.get()); }
    case Expr::Kind::Index:  { auto* i = static_cast<const IndexExpr*>(e); return hasLambdaOrRef(i->object.get()) || hasLambdaOrRef(i->index.get()); }
    case Expr::Kind::Call: {
        auto* c = static_cast<const CallExpr*>(e);
        if (hasLambdaOrRef(c->callee.get())) return true;
        for (auto& a : c->args) if (hasLambdaOrRef(a.get())) return true;
        return false;
    }
    case Expr::Kind::Array: for (auto& x : static_cast<const ArrayExpr*>(e)->elements) if (hasLambdaOrRef(x.get())) return true; return false;
    case Expr::Kind::Map:   for (auto& kv : static_cast<const MapExpr*>(e)->entries) if (hasLambdaOrRef(kv.first.get()) || hasLambdaOrRef(kv.second.get())) return true; return false;
    default: return false;
    }
}

// how much script code an expression can run while it is worked out
enum class Purity { CallFree, PureCalls, Impure };

// builtins that only look at their arguments: no callbacks, no writes to variables. calling one doesn't count as
// running script code, as long as the name still means the builtin (STASH_VAR checks that when it runs)
inline bool isPureBuiltin(const std::string& n) {
    static const std::unordered_set<std::string> names = {
        "str", "num", "len", "type", "has", "keys", "values", "slice", "join", "min", "max", "sum", "index_of"};
    return names.count(n) > 0;
}

// `callees` (if given) collects the names of the pure builtins the expression calls
// (a variable holding a pointer with a user-defined `+` or `len` is the one way around this, and is not worth a copy per read)
inline Purity purityOf(const Expr* e, std::vector<std::string>* callees = nullptr) {
    auto worse = [](Purity a, Purity b) { return a > b ? a : b; };
    switch (e->kind) {
    case Expr::Kind::Number: case Expr::Kind::Int: case Expr::Kind::String: case Expr::Kind::Var:
        return Purity::CallFree;
    case Expr::Kind::Unary:
        return purityOf(static_cast<const UnaryExpr*>(e)->right.get(), callees);
    case Expr::Kind::Binary: {
        auto* b = static_cast<const BinaryExpr*>(e);
        return worse(purityOf(b->left.get(), callees), purityOf(b->right.get(), callees));
    }
    case Expr::Kind::And: case Expr::Kind::Or: {
        auto* l = static_cast<const LogicExpr*>(e);
        return worse(purityOf(l->left.get(), callees), purityOf(l->right.get(), callees));
    }
    case Expr::Kind::Index: {
        auto* i = static_cast<const IndexExpr*>(e);
        return worse(purityOf(i->object.get(), callees), purityOf(i->index.get(), callees));
    }
    case Expr::Kind::Array: {
        Purity p = Purity::CallFree;
        for (auto& el : static_cast<const ArrayExpr*>(e)->elements) p = worse(p, purityOf(el.get(), callees));
        return p;
    }
    case Expr::Kind::Map: {
        Purity p = Purity::CallFree;
        for (auto& kv : static_cast<const MapExpr*>(e)->entries)
            p = worse(p, worse(purityOf(kv.first.get(), callees), purityOf(kv.second.get(), callees)));
        return p;
    }
    case Expr::Kind::Call: {
        auto* c = static_cast<const CallExpr*>(e);
        if (c->callee->kind != Expr::Kind::Var) return Purity::Impure;
        const std::string& nm = static_cast<const VarExpr*>(c->callee.get())->name;
        if (!isPureBuiltin(nm)) return Purity::Impure;
        for (auto& a : c->args) if (purityOf(a.get(), callees) == Purity::Impure) return Purity::Impure;
        if (callees) callees->push_back(nm);
        return Purity::PureCalls;
    }
    default: return Purity::Impure;   // Lambda
    }
}


} // namespace embr

#endif // EMBR_CORE_AST_H
