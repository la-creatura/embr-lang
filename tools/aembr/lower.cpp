// lower.cpp
// walks the checked tree and writes the intermediate form (ir.h). every construct aembr does not compile is refused here, with the
// line it is written on.
//
// good to know
//   - int, float, str, arrays and maps of those, errors, and function values. a name has one type for the whole function, taken from
//     its first value
//   - the types of operators are worked out here (int op int is int, anything with a float is float, `/` is float). the checker
//     types `+ - *` as "num" because the VM turns an int overflow into a float, so it is only used to cross-check the leaves
//   - in a program, top level names are the locals of main. a top level function cannot see them (the VM would read them live), so that
//     is refused. a closure can use the names of the function around it, they are then kept in a cell that the closure shares
//   - in a plugin, top level names are globals, made once when the plugin is imported and read only to its functions (no function can
//     change one, so a plugin used from several threads shares no state)
//   - an expression gives a pure value (ir::Ex). anything that allocates, calls or can raise becomes a statement writing a temp,
//     and the temps are what hold the counts of intermediate strings and arrays
//   - a function type is read from the source, `fn(int, str) -> int`. a result that is not written means the function returns nothing
//   - one thing is still learned from a use: the element type of an empty `[]` or `{}`. the first use that says one is remembered
//     (`hints`), and the program is lowered again with it. a function that needs what is not known yet is skipped for that round

#include "lower.h"
#include "natives.h"
#include "plugins.h"

#include <map>
#include <set>

namespace aembr {

using namespace embr;

std::string formatRefusal(const std::string& file, const Refusal& r) {
    return (r.file.empty() ? file : r.file) + ":" + std::to_string(r.at.startLine) + ":" + std::to_string(r.at.startCol) +
           ": outside the compiled subset: " + r.msg + "\n";
}

namespace {

// the file of the statements being lowered, when it is a module (a refusal names it)
std::string gRefuseFile;
[[noreturn]] void refuse(const SourceRange& at, const std::string& msg) { throw Refusal{msg, at, gRefuseFile}; }

struct Sig {
    int                 index = 0;
    std::vector<ir::Ty> params;
    ir::Ty              ret = ir::Ty::Void;
    // --infer: the hint key of each parameter (empty for a written one), and of the result when that is not written
    std::vector<std::string> paramKey;
    std::vector<std::string> paramName;      // for the message of a check on an argument
    std::string              retKey;
    bool                     builtin = false;       // from std/*.embr: an int is taken where a float is written, as the natives of the plugins do
    bool                     retHost = false;       // `-> host`: the result is written by C text, see Fn::resultHost
    bool                     variadic = false;      // the last parameter collects the arguments that are left, as an array
    std::vector<ParamDefault> defaults;              // per parameter; kind None for a required one
    std::vector<char>        byref;                  // per parameter: an in-out one, the call passes &name
    size_t                   nreq = 0;               // the parameters before the optional ones
    bool unknownParam() const {
        for (size_t i = 0; i < params.size(); ++i) if (!paramKey[i].empty() && params[i] == ir::Ty::Void) return true;
        return false;
    }
};

// what a call site that takes a function says about it: the types of the arguments it will give, and the result it wants (Void: any, worked out
// from the body). a lambda written right there may leave its parameters unwritten
struct Expect { std::vector<ir::Ty> params; ir::Ty ret = ir::Ty::Void; };

// thrown when the function being lowered needs a type that is not known yet. the function is skipped in this round
struct Retry {};

struct Lowerer {
    const Front& fr;
    ir::Module   m;
    std::map<std::string, Sig> sigs;
    // `const fn`: written out for each combination of argument types it is called with, as the VM copies it into each call
    std::map<std::string, const FnStmt*> templates;
    std::map<std::string, Sig>           instances;      // by name and argument types
    struct Pending { const FnStmt* f; std::string iname; };
    std::vector<Pending>                 pendingInst;
    std::set<std::string>      topNames;       // names the top level assigns, a top level function may not touch them
    std::map<int, std::set<std::string>> topNamesOf;   // the same, for each imported module (by file index): a function of a module only means its own file's names

    // the function being written. a closure is written while the function around it waits in `parents`
    ir::Fn*                    cur = nullptr;
    std::map<std::string, int> locals;         // name -> index in cur->vars
    ir::Ty                     curRet = ir::Ty::Void;
    bool                       retSkipped = false;       // a return was left out because its type was not known yet
    std::string                curRetHintKey;            // the hint that says what the result is, when it is worked out (--infer), to widen it to any
    bool                       curRetInferred = false;   // curRet was worked out, for the message of a return that disagrees
    std::string                curRetKey;          // --infer: the hint the first `return value` sets, in a function whose result is not written
    std::string                pendingRet;         // the last call was to a function whose result type is not known yet
    std::string                deferReason;        // why the function being written was put off, for the message when nothing settles
    std::map<std::string, std::string> deferWhy;
    bool                       inFn = false;
    int                        loopDepth = 0;
    std::string                selfName;       // a function written inside another one can call itself by its name
    int                        selfSig = -1;
    std::map<std::string, int> capIdx;         // name -> index in cur->captures
    int                        curFileIdx = 0;          // the source file the statements being lowered are written in (Module::files)
    bool                       curIsStd = false;        // the function being lowered comes from std/*.embr
    bool                       curResultHost = false;   // the function being lowered has `-> host`
    std::string                curKey;         // names the function in hints

    struct Saved {
        ir::Fn* fn; std::map<std::string, int> locals; ir::Ty ret; bool inFn; int loopDepth;
        std::string selfName; int selfSig; std::map<std::string, int> capIdx; std::string key;
    };
    std::vector<Saved> parents;

    std::map<std::string, ir::Ty>& hints;
    std::vector<ir::FnSig>&        sigTable;      // the same numbers in every round: hints hold function types
    bool&                          changed;
    std::set<std::string>          guessing;      // the arrays and maps that were given a guess (int values) in this round
    std::set<std::string>          deferred;
    std::string                    noValueReason;      // why the last call gave no value, when that is not obvious

    std::set<std::string>          imported;      // the built in plugins the file imports (embrlib, str, embrmath)
    std::vector<PluginDesc>        descs;         // the C plugins it imports, read when the file is compiled
    std::map<std::string, std::pair<int, const PluginFnDesc*>> pluginFns;   // function name -> its plugin and description
    LowerOptions                   opts;
    bool                           plugin;
    std::map<std::string, int>     globalIdx;     // plugin: the top level names, in Module::globals

    Lowerer(const Front& f, std::map<std::string, ir::Ty>& h, std::vector<ir::FnSig>& st, bool& ch, const LowerOptions& o)
        : fr(f), hints(h), sigTable(st), changed(ch), opts(o), plugin(o.plugin) {}
    // a top level name is a global when it is a plugin's, or a function of the program uses it (found while writing the function, then the
    // program is written again with main first, so the globals and their types exist when the functions are written)
    // the library std/*.embr has locals of its own: a name there is never one of the top level names of the file that brings it in
    bool topName(const std::string& n) const {
        if (curIsStd) return false;
        if (curFileIdx > 0) { auto it = topNamesOf.find(curFileIdx); return it != topNamesOf.end() && it->second.count(n) != 0; }
        return topNames.count(n) != 0;
    }
    // index of the file a top level statement comes from: 0 is the program's own
    int fileOf(const Stmt* s) {
        auto it = fr.stmtFile.find(s);
        if (it == fr.stmtFile.end()) return 0;
        for (size_t i = 1; i < m.files.size(); ++i) if (m.files[i] == it->second) return (int)i;
        m.files.push_back(it->second);
        return (int)m.files.size() - 1;
    }
    bool isGlobalName(const std::string& n) const { return plugin || hints.count("g:" + n) != 0; }
    bool anyProgramGlobals() const {
        for (const auto& kv : hints) if (kv.first.rfind("g:", 0) == 0) return true;
        return false;
    }
    // the top level statements are being written (not a function, not a closure)
    bool atTop() const { return parents.empty() && cur == &m.main; }

    void setHint(const std::string& key, ir::Ty t) {
        auto it = hints.find(key);
        if (it != hints.end() && it->second == t) return;
        hints[key] = t;
        changed = true;
    }
    int internSig(const std::vector<ir::Ty>& params, ir::Ty ret) {
        for (size_t i = 0; i < sigTable.size(); ++i)
            if (sigTable[i].params == params && sigTable[i].ret == ret) return (int)i;
        sigTable.push_back({params, ret});
        return (int)sigTable.size() - 1;
    }

    // ---- types
    static const char* tn(ir::Ty t) { return ir::tyName(t); }
    static bool scalarElem(ir::Ty t) { return ir::canBeElem(t); }       // can it be an element of an array or a map (int, float, str, an array, a map)

    static ir::Ty scalarType(TypeSet t) {
        if (t == TS::Int) return ir::Ty::Int;
        if (t == TS::Float) return ir::Ty::Float;
        if (t == TS::Str) return ir::Ty::Str;
        return ir::Ty::Void;
    }
    // a type the checker recorded (the element type of an empty literal), nested containers included. Void when it is not one type
    static ir::Ty annType(TypeSet t, const TypeElem& el) {
        if (t == TS::Arr || t == TS::Map) {
            if (!el) return ir::Ty::Void;
            const ir::Ty e = annType(el->t, el->el);
            if (e == ir::Ty::Void) return ir::Ty::Void;
            return t == TS::Map ? ir::mapOf(e) : ir::arrOf(e);
        }
        return scalarType(t);
    }
    // a type as written in the source (a parameter, a return type, `local`)
    ir::Ty writtenType(TypeSet t, const TypeElem& el, const SourceRange& at, const std::string& what) {
        if (el && el->isHost) {
            if (!opts.plugin) refuse(at, what + " is host: that type is for the functions of a plugin (aembr plugin), a program has no host");
            return ir::Ty::Host;
        }
        if (t == TS::Fn) {
            if (!el || !el->isFn) refuse(at, what + " is a function without its type (write fn(int) -> int, with the types it takes and gives)");
            std::vector<ir::Ty> ps;
            for (const TypeElem& p : el->params) ps.push_back(writtenType(p->t, typecheck::detail::detail(p), at, "a parameter of " + what));
            // a result that is not written is nothing: the VM does not check it, the compiled code has no value to give
            const ir::Ty ret = el->ret ? writtenType(el->ret->t, typecheck::detail::detail(el->ret), at, "the result of " + what) : ir::Ty::Void;
            return ir::fnTy(internSig(ps, ret));
        }
        if (t == TS::Arr || t == TS::Map) {
            const bool isM = t == TS::Map;
            if (!el) refuse(at, what + (isM ? " is a map without a value type (write map<int>, map<float> or map<str>)" : " is an array without an element type (write arr<int>, arr<float> or arr<str>)"));
            if (el->t == TS::Fn) refuse(at, what + " holds functions, which a container of the compiled subset cannot");
            const ir::Ty e = (el->t == TS::Arr || el->t == TS::Map) ? writtenType(el->t, el->el, at, what) : el->t.isAny() ? ir::Ty::Any : scalarType(el->t);
            if (e == ir::Ty::Void) refuse(at, what + " has to hold int, float, str, an array or a map");
            return isM ? ir::mapOf(e) : ir::arrOf(e);
        }
        if (t.isAny()) return ir::Ty::Any;          // `any` written on purpose
        const ir::Ty r = scalarType(t);
        if (r == ir::Ty::Void) refuse(at, what + " has to be written as int, float, str, any, arr<...> or map<...>");
        return r;
    }

    std::string sigText(ir::Ty t) {
        const ir::FnSig& s = sigTable[(size_t)ir::sigOf(t)];
        std::string o = "fn(";
        for (size_t i = 0; i < s.params.size(); ++i) o += std::string(i ? ", " : "") + tn(s.params[i]);
        return o + ")" + (s.ret == ir::Ty::Void ? "" : std::string(" -> ") + tn(s.ret));
    }
    std::string name(ir::Ty t) { return ir::isFn(t) ? sigText(t) : tn(t); }

    // the checker's record has to allow the type aembr worked out (it never has to be exactly equal: num covers both)
    void crossCheck(const Expr* e, ir::Ty t) {
        if (t == ir::Ty::Any) return;
        auto it = fr.types.find(e);
        if (it == fr.types.end()) return;
        const TypeTag tag = t == ir::Ty::Int ? TypeTag::Int : t == ir::Ty::Float ? TypeTag::Number
                          : t == ir::Ty::Str ? TypeTag::String : (ir::isMap(t) || t == ir::Ty::Err) ? TypeTag::Map
                          : ir::isFn(t) ? TypeTag::Callable : TypeTag::Array;
        if (!it->second.t.contains(tag))
            refuse(e->range, std::string("internal check failed: aembr found ") + name(t) + " but the checker found " + it->second.t.name());
    }

    // ---- guesses and hints
    std::string hintKey(const std::string& var) const { return curKey + ":" + var; }
    // the name of the function being written, for a message ("main" at the top level, the name of the nearest named function inside a closure)
    std::string curFnName() const { return cur ? cur->name : std::string("?"); }

    // the container in `v` was given a guess and the use says it holds something else: remember, and skip this function for now.
    // `v` can be a variable of a function around this one, then the guess to change is the one that function made
    void learn(const ir::ExP& v, ir::Ty expected) {
        if (!(ir::isArr(expected) || ir::isMap(expected)) || v->kind != ir::Ex::Kind::Load || v->ref.global || v->ref.self) return;
        size_t k = parents.size();
        ir::Ref r = v->ref;
        while (r.captured) { r = view(k).fn->captures[(size_t)r.index].from; --k; }
        const ir::Var& var = view(k).fn->vars[(size_t)r.index];
        const std::string key = (k == parents.size() ? curKey : parents[k].key) + ":" + var.name;
        if (var.temp || !guessing.count(key)) return;
        setHint(key, expected);
        throw Retry{};
    }
    static bool isEmptyLiteral(const Expr* e) {
        if (e->kind == Expr::Kind::Map) return static_cast<const MapExpr*>(e)->entries.empty();
        return e->kind == Expr::Kind::Array && !static_cast<const ArrayExpr*>(e)->isRange && static_cast<const ArrayExpr*>(e)->elements.empty();
    }
    // the type an empty array gets for the variable it is stored in
    ir::Ty guessFor(const std::string& var, const Expr* lit) {
        auto it = fr.types.find(lit);
        if (it != fr.types.end() && it->second.el && annType(it->second.el->t, it->second.el->el) != ir::Ty::Void) return ir::Ty::Void;   // the checker knows
        auto h = hints.find(hintKey(var));
        if (h != hints.end()) return h->second;
        guessing.insert(hintKey(var));
        return lit->kind == Expr::Kind::Map ? ir::Ty::MapInt : ir::Ty::ArrInt;
    }

    // ---- building
    int newTemp(ir::Ty t) {
        cur->vars.push_back({"", t, true});
        return (int)cur->vars.size() - 1;
    }
    static ir::ExP cInt(int64_t v) { auto e = std::make_shared<ir::Ex>(); e->kind = ir::Ex::Kind::ConstInt; e->ty = ir::Ty::Int; e->i = v; return e; }
    static ir::ExP cFloat(double v) { auto e = std::make_shared<ir::Ex>(); e->kind = ir::Ex::Kind::ConstFloat; e->ty = ir::Ty::Float; e->f = v; return e; }
    // the value of a default written in a parameter list, as the type of the parameter
    ir::ExP defaultEx(const ParamDefault& d, ir::Ty ty) {
        if (d.kind == ParamDefault::Str) return cStr(d.s);
        if (ty == ir::Ty::Float) return cFloat(d.kind == ParamDefault::Int ? (double)d.i : d.f);
        return cInt(d.i);
    }
    ir::ExP cStr(const std::string& v) {
        size_t i = 0;
        for (; i < m.strings.size(); ++i) if (m.strings[i] == v) break;
        if (i == m.strings.size()) m.strings.push_back(v);
        auto e = std::make_shared<ir::Ex>(); e->kind = ir::Ex::Kind::ConstStr; e->ty = ir::Ty::Str; e->i = (int64_t)i; return e;
    }
    static ir::ExP loadRef(const ir::Ref& r, ir::Ty ty) {
        auto e = std::make_shared<ir::Ex>(); e->kind = ir::Ex::Kind::Load; e->ty = ty; e->ref = r; return e;
    }
    ir::ExP load(int idx) { return loadRef({false, idx}, cur->vars[(size_t)idx].ty); }
    static ir::ExP bin(ir::BinOp op, ir::Ty ty, ir::ExP a, ir::ExP b) {
        auto e = std::make_shared<ir::Ex>(); e->kind = ir::Ex::Kind::Bin; e->ty = ty; e->bop = op; e->a = std::move(a); e->b = std::move(b); return e;
    }
    static ir::ExP un(ir::UnOp op, ir::Ty ty, ir::ExP a) {
        auto e = std::make_shared<ir::Ex>(); e->kind = ir::Ex::Kind::Un; e->ty = ty; e->uop = op; e->a = std::move(a); return e;
    }
    static ir::ExP toFloat(ir::ExP a) {
        if (a->ty == ir::Ty::Float) return a;
        auto e = std::make_shared<ir::Ex>(); e->kind = ir::Ex::Kind::ToFloat; e->ty = ir::Ty::Float; e->a = std::move(a); return e;
    }
    static ir::ExP lenOf(ir::ExP a) {
        auto e = std::make_shared<ir::Ex>(); e->kind = ir::Ex::Kind::Len; e->ty = ir::Ty::Int; e->a = std::move(a); return e;
    }
    // ---- values of no fixed type (any). a typed value is put in a box where an any is wanted, and an any is checked and taken out where a type is wanted
    // a call of a runtime function: the result in a new variable (null when it gives nothing)
    ir::ExP rtCall(const char* cname, std::vector<ir::ExP> args, ir::Ty rty, bool raises, int line, std::vector<ir::St>& out) {
        ir::St st; st.kind = ir::St::Kind::Native; st.nop = ir::NativeOp::Rt; st.line = line; st.cname = cname; st.raises = raises; st.args = std::move(args);
        if (rty == ir::Ty::Void) { out.push_back(std::move(st)); return nullptr; }
        const int t = newTemp(rty);
        st.hasDst = true; st.dst = {false, t};
        out.push_back(std::move(st));
        return load(t);
    }
    // a type as the text the runtime reads: i f s a, A<elem> for an array, M<elem> for a map
    static std::string descOf(ir::Ty t) {
        if (t == ir::Ty::Int) return "i";
        if (t == ir::Ty::Float) return "f";
        if (t == ir::Ty::Str) return "s";
        if (t == ir::Ty::Any) return "a";
        return std::string(ir::isArr(t) ? "A" : "M") + descOf(ir::elemOf(t));
    }
    ir::ExP toAny(ir::ExP v, const SourceRange& at, std::vector<ir::St>& out) {
        const int line = at.startLine;
        switch (v->ty) {
        case ir::Ty::Any: return v;
        case ir::Ty::Int: return rtCall("rt_any_of_int", {v}, ir::Ty::Any, false, line, out);
        case ir::Ty::Float: return rtCall("rt_any_of_float", {v}, ir::Ty::Any, false, line, out);
        case ir::Ty::Str: return rtCall("rt_any_of_str", {v}, ir::Ty::Any, false, line, out);
        default: break;
        }
        if (ir::isArr(v->ty)) return rtCall("rt_any_of_arr", {v}, ir::Ty::Any, false, line, out);
        if (ir::isMap(v->ty)) return rtCall("rt_any_of_map", {v}, ir::Ty::Any, false, line, out);
        refuse(at, "a " + name(v->ty) + " cannot be put in an any");
    }
    // an any as a value of the type `want`; it is checked when the program runs
    // `tmpl` is the error when it is not that type: %W is the type wanted and %G the type it was
    ir::ExP fromAny(ir::ExP v, ir::Ty want, const SourceRange& at, std::vector<ir::St>& out, const std::string& tmpl = "expected %W but got %G") {
        const int line = at.startLine;
        if (want == ir::Ty::Int) return rtCall("rt_any_to_i64", {v, cStr(tmpl)}, ir::Ty::Int, true, line, out);
        if (want == ir::Ty::Float) return rtCall("rt_any_to_f64", {v, cStr(tmpl)}, ir::Ty::Float, true, line, out);
        if (want == ir::Ty::Str) return rtCall("rt_any_to_str", {v, cStr(tmpl)}, ir::Ty::Str, true, line, out);
        if (ir::isArr(want) || ir::isMap(want)) {
            const int t = newTemp(want);
            out.push_back(native(ir::isArr(want) ? ir::NativeOp::AnyToArr : ir::NativeOp::AnyToMap, line, {v, cStr(tmpl)}, t));
            out.back().cname = descOf(ir::elemOf(want));
            return load(t);
        }
        refuse(at, "an any cannot be used as a " + name(want));
    }
    // v as the type `want` when either one is an any (a box, or a check); otherwise v as it is
    ir::ExP coerce(ir::ExP v, ir::Ty want, const SourceRange& at, std::vector<ir::St>& out, const std::string& tmpl = "expected %W but got %G") {
        if (!v || v->ty == want) return v;
        if (want == ir::Ty::Any) return toAny(v, at, out);
        if (v->ty == ir::Ty::Any) return fromAny(v, want, at, out, tmpl);
        return v;
    }

    // the value `x` to be stored as an element of the container `c` (an array or a map). an any goes into a container of anys in a box, comes out of one as
    // a checked value, and a guess that held something else is changed to hold anys
    ir::ExP elementFor(ir::ExP x, const ir::ExP& c, const SourceRange& at, std::vector<ir::St>& out) {
        const ir::Ty want = ir::elemOf(c->ty);
        if (x->ty == ir::Ty::Any && want != ir::Ty::Any) learn(c, ir::isArr(c->ty) ? ir::arrOf(ir::Ty::Any) : ir::mapOf(ir::Ty::Any));
        return coerce(x, want, at, out);
    }

    // an int that says yes or no for a number: 1 when it is not zero
    ir::ExP truth(ir::ExP a, const SourceRange& at, std::vector<ir::St>* out = nullptr) {
        if (a->ty == ir::Ty::Any && out) return rtCall("rt_any_truthy", {a}, ir::Ty::Int, false, at.startLine, *out);
        if (a->ty == ir::Ty::Float) return bin(ir::BinOp::Ne, ir::Ty::Int, std::move(a), cFloat(0.0));
        if (a->ty != ir::Ty::Int) refuse(at, "a " + name(a->ty) + " cannot be used as a yes or no here");
        return a;
    }
    static ir::St set(const ir::Ref& dst, ir::ExP v, int line) {
        ir::St s; s.kind = ir::St::Kind::Set; s.line = line; s.hasDst = true; s.dst = dst; s.a = std::move(v); return s;
    }
    static ir::St set(int idx, ir::ExP v, int line) { return set(ir::Ref{false, idx}, std::move(v), line); }
    static ir::St native(ir::NativeOp op, int line, std::vector<ir::ExP> args, const ir::Ref& dst, bool inplace = false) {
        ir::St s; s.kind = ir::St::Kind::Native; s.nop = op; s.line = line; s.args = std::move(args); s.inplace = inplace;
        s.hasDst = true; s.dst = dst;
        return s;
    }
    static ir::St native(ir::NativeOp op, int line, std::vector<ir::ExP> args, int dst = -1, bool inplace = false) {
        if (dst >= 0) return native(op, line, std::move(args), ir::Ref{false, dst}, inplace);
        ir::St s; s.kind = ir::St::Kind::Native; s.nop = op; s.line = line; s.args = std::move(args); s.inplace = inplace;
        return s;
    }

    // ---- names. a variable is a local, the function itself (a function written inside another one), or a variable of a function
    // around this one, which the closure then captures
    struct VarInfo { bool found = false; ir::Ref ref; ir::Ty ty = ir::Ty::Void; };

    struct View { ir::Fn* fn; std::map<std::string, int>* locals; std::string* selfName; int* selfSig; std::map<std::string, int>* capIdx; };
    View view(size_t k) {
        if (k == parents.size()) return {cur, &locals, &selfName, &selfSig, &capIdx};
        Saved& p = parents[k];
        return {p.fn, &p.locals, &p.selfName, &p.selfSig, &p.capIdx};
    }
    VarInfo lookupIn(size_t k, const std::string& n, const SourceRange& at) {
        View v = view(k);
        auto it = v.locals->find(n);
        if (it != v.locals->end()) return {true, ir::Ref{false, it->second}, v.fn->vars[(size_t)it->second].ty};
        if (!v.selfName->empty() && *v.selfName == n) { ir::Ref r; r.self = true; return {true, r, ir::fnTy(*v.selfSig)}; }
        auto c = v.capIdx->find(n);
        if (c != v.capIdx->end()) { ir::Ref r; r.captured = true; r.index = c->second; return {true, r, v.fn->captures[(size_t)c->second].ty}; }
        if (k == 0) return {};
        VarInfo up = lookupIn(k - 1, n, at);
        if (!up.found) return up;
        if (up.ref.self) refuse(at, "a function written inside another one cannot call the outer one by name yet");
        if (up.ty == ir::Ty::Host) refuse(at, "a function written inside another one cannot use the host value '" + n + "'");
        if (!up.ref.captured && view(k - 1).fn->vars[(size_t)up.ref.index].byref)
            refuse(at, "a function written inside another one cannot use the in-out parameter '" + n + "'");
        if (!up.ref.captured) view(k - 1).fn->vars[(size_t)up.ref.index].boxed = true;
        v.fn->captures.push_back({n, up.ty, up.ref});
        ir::Ref r; r.captured = true; r.index = (int)v.fn->captures.size() - 1;
        (*v.capIdx)[n] = r.index;
        return {true, r, up.ty};
    }
    VarInfo lookup(const std::string& n, const SourceRange& at) {
        VarInfo v = lookupIn(parents.size(), n, at);
        if (!v.found && isGlobalName(n)) {
            auto g = globalIdx.find(n);
            if (g != globalIdx.end()) { ir::Ref r; r.global = true; r.index = g->second; return {true, r, m.globals[(size_t)g->second].ty}; }
        }
        return v;
    }

    // ---- expressions. statements an expression needs (calls, operations that can raise) go to `out` in order
    // `want`: the type the context expects, used by an empty array to learn its elements
    ir::ExP value(const Expr* e, std::vector<ir::St>& out, ir::Ty want = ir::Ty::Void) {
        noValueReason.clear();
        ir::ExP r = expr(e, out, want);
        if (!r && !pendingRet.empty()) {
            deferReason = "the result of '" + pendingRet + "' is used, but no 'return value' of it gave a type";
            throw Retry{};
        }
        if (!r) refuse(e->range, noValueReason.empty() ? "this call gives no value" : noValueReason);
        crossCheck(e, r->ty);
        if (r->ty == ir::Ty::Host && e != hostArgOk) refuse(e->range, "a host value can only be given to a host parameter or named in C text");
        return r;
    }
    const Expr* hostArgOk = nullptr;      // the argument being read for a host parameter

    // `a, b = arr`, `local a, b = arr`, `local [a, b] = arr`: each name takes one element, from the start. more elements than names are fine,
    // fewer is an error when it runs. every name has the type of the elements
    void unpack(const std::vector<std::string>& names, const std::vector<SourceRange>& ranges, const std::vector<DestructureTarget>* decl,
                const Expr* valueExpr, bool local, int line, std::vector<ir::St>& out) {
        ir::ExP v = value(valueExpr, out);
        if (ir::isMap(v->ty)) refuse(valueExpr->range, "unpacking a map is not compiled: the VM visits the keys in an order nobody defines (use keys(m) and values(m))");
        if (!ir::isArr(v->ty)) refuse(valueExpr->range, "cannot unpack a " + name(v->ty) + " into " + std::to_string(names.size()) + " variables");
        const ir::Ty ety = ir::elemOf(v->ty);
        const int arr = newTemp(v->ty);
        out.push_back(set(arr, v, line));
        ir::St chk; chk.kind = ir::St::Kind::Native; chk.nop = ir::NativeOp::Rt; chk.line = line; chk.cname = "rt_unpack_check"; chk.raises = true;
        chk.args = {load(arr), cInt((int64_t)names.size())};
        out.push_back(std::move(chk));
        for (size_t i = 0; i < names.size(); ++i) {
            if (decl && (*decl)[i].explicitType) {
                const ir::Ty declared = writtenType((*decl)[i].type, (*decl)[i].elem, (*decl)[i].range, "the type of '" + names[i] + "'");
                if (declared != ety) refuse(ranges[i], "'" + names[i] + "' is declared " + name(declared) + " but the array holds " + name(ety));
            }
            const int x = newTemp(ety);
            out.push_back(native(ir::NativeOp::ArrGet, line, {load(arr), cInt((int64_t)i)}, x));
            const ir::Ref dst = variable(names[i], ety, ranges[i], local);
            store(dst, load(x), line, out, local);
        }
    }

    // `c """ ... """`: the text goes into the program as it is. `$name` is the C name of an embr variable of this function, `$$` is a $
    void inlineC(const ExprStmt* x, std::vector<ir::St>& out) {
        if ((opts.denyCaps & EMBR_CAP_NATIVE) && !curIsStd && !fr.stdStmts.count(x)) refuse(x->range, "this C text is native code, which --deny-caps refuses");
        if (x->cFileScope) {
            // the text of the whole file: only at the top level, where it is put before the functions. nothing in it is replaced
            if (!atTop()) refuse(x->range, "c file text is for the whole C file, so it has to be at the top level, outside functions and blocks");
            ir::CFile f; f.text = static_cast<const StringExpr*>(x->expr.get())->v; f.file = x->cFile; f.line = x->range.startLine;
            m.fileC.push_back(std::move(f));
            return;
        }
        ir::St st; st.kind = ir::St::Kind::InlineC; st.line = x->range.startLine; st.cfile = x->cFile;
        const std::string& t = static_cast<const StringExpr*>(x->expr.get())->v;
        std::string lit;
        auto isId = [](char c) { return c == '_' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
        for (size_t i = 0; i < t.size(); ++i) {
            if (t[i] != '$') { lit += t[i]; continue; }
            if (i + 1 < t.size() && t[i + 1] == '$') { lit += '$'; ++i; continue; }
            size_t j = i + 1;
            while (j < t.size() && isId(t[j])) ++j;
            if (j == i + 1) { lit += '$'; continue; }
            const std::string name = t.substr(i + 1, j - i - 1);
            VarInfo v = lookup(name, x->range);
            if (!v.found && (name == "ctx" || name == "out")) {
                // the host the plugin function was called by: ctx is its handle (for the embrp_ calls), out the slot of the result of a `-> host` function
                if (!opts.plugin) refuse(x->range, "'$" + name + "' in C text is the host of a plugin function: a program has no host");
                if (name == "out" && !curResultHost) refuse(x->range, "'$out' is the result slot of a function whose result is written `-> host`");
                lit += name == "ctx" ? "rt_host_ctx" : "rt_host_out";
                i = j - 1;
                continue;
            }
            if (!v.found) refuse(x->range, "'$" + name + "' in C text is not a variable of this function (use $$ for a dollar sign)");
            if (v.ref.self) refuse(x->range, "'$" + name + "' is the function itself, C text cannot use it");
            if (!lit.empty()) { st.cseg.push_back({lit, false, {}}); lit.clear(); }
            st.cseg.push_back({"", true, v.ref});
            i = j - 1;
        }
        if (!lit.empty()) st.cseg.push_back({lit, false, {}});
        out.push_back(std::move(st));
    }

    ir::ExP raising(ir::RaiseOp op, ir::Ty ty, ir::ExP a, ir::ExP b, int line, std::vector<ir::St>& out) {
        const int t = newTemp(ty);
        ir::St s; s.kind = ir::St::Kind::Raising; s.line = line; s.hasDst = true; s.dst = {false, t}; s.rop = op; s.a = std::move(a); s.b = std::move(b);
        out.push_back(std::move(s));
        return load(t);
    }
    // a native that gives a value: it goes into a new temp
    ir::ExP nativeValue(ir::NativeOp op, ir::Ty ty, int line, std::vector<ir::ExP> args, std::vector<ir::St>& out) {
        const int t = newTemp(ty);
        out.push_back(native(op, line, std::move(args), t));
        return load(t);
    }

    // a function of a program reads or assigns a top level name: it has to be a global. the first time this is seen the name is remembered and the
    // program is written again. if it is remembered already and still not found, the name is not a plain variable of main (a loop variable, say)
    [[noreturn]] void useGlobal(const std::string& n, const SourceRange& at, const char* verb) {
        if (hints.count("g:" + n))
            refuse(at, std::string("the function ") + verb + " the top level name '" + n + "', which is not a variable main keeps for good (a loop variable or an error name, say). pass it as an argument");
        setHint("g:" + n, ir::Ty::Int);
        deferReason = "the function " + std::string(verb) + " the top level name '" + n + "'";
        throw Retry{};
    }

    ir::ExP expr(const Expr* e, std::vector<ir::St>& out, ir::Ty want) {
        const int line = e->range.startLine;
        switch (e->kind) {
        case Expr::Kind::Int:    return cInt(static_cast<const IntExpr*>(e)->v);
        case Expr::Kind::Number: return cFloat(static_cast<const NumberExpr*>(e)->v);
        case Expr::Kind::String: return cStr(static_cast<const StringExpr*>(e)->v);
        case Expr::Kind::Var: {
            const std::string& n = static_cast<const VarExpr*>(e)->name;
            VarInfo v = lookup(n, e->range);
            if (v.found) return loadRef(v.ref, v.ty);
            if (inFn && !plugin && topName(n)) useGlobal(n, e->range, "uses");
            if (templates.count(n)) refuse(e->range, "'" + n + "' is a const fn, which is written out for each call that names it, so it has no one type to be used as a value");
            if (sigs.count(n)) return functionAsValue(n, e->range, out);
            if (imported.count("embrmath") && n == "PI") return cFloat(3.14159265358979323846);
            if (imported.count("embrmath") && n == "E") return cFloat(2.71828182845904523536);
            refuse(e->range, "'" + n + "' is not a name aembr knows");
        }
        case Expr::Kind::Unary: {
            auto* u = static_cast<const UnaryExpr*>(e);
            ir::ExP a = value(u->right.get(), out);
            if (u->op == "-") {
                if (a->ty == ir::Ty::Any) return rtCall("rt_any_neg", {a}, ir::Ty::Any, true, line, out);
                if (a->ty != ir::Ty::Int && a->ty != ir::Ty::Float) refuse(e->range, "- does not work on a " + name(a->ty));
                return un(ir::UnOp::Neg, a->ty, a);
            }
            if (u->op == "!") return un(ir::UnOp::Not, ir::Ty::Int, truth(a, e->range, &out));
            if (u->op == "~") {
                if (a->ty != ir::Ty::Int) refuse(e->range, "~ works on int only");
                return un(ir::UnOp::BitNot, ir::Ty::Int, a);
            }
            refuse(e->range, "the operator " + u->op);
        }
        case Expr::Kind::And: case Expr::Kind::Or: {
            auto* l = static_cast<const LogicExpr*>(e);
            const bool isAnd = e->kind == Expr::Kind::And;
            ir::ExP av = value(l->left.get(), out);
            std::vector<ir::St> rhs;
            ir::ExP bv = value(l->right.get(), rhs);
            if (av->ty == ir::Ty::Any || bv->ty == ir::Ty::Any) {
                // values of no fixed type: and gives the left one unless it is true, or gives the right one, as the VM does
                const int res = newTemp(ir::Ty::Any);
                out.push_back(set(res, toAny(av, l->left->range, out), line));
                ir::ExP yes = rtCall("rt_any_truthy", {load(res)}, ir::Ty::Int, false, line, out);
                ir::St iff; iff.kind = ir::St::Kind::If; iff.line = line;
                iff.a = isAnd ? yes : un(ir::UnOp::Not, ir::Ty::Int, yes);
                iff.then = std::move(rhs);
                iff.then.push_back(set(res, toAny(bv, l->right->range, iff.then), line));
                out.push_back(std::move(iff));
                return load(res);
            }
            ir::ExP a = truth(av, l->left->range, &out);
            ir::ExP b = truth(bv, l->right->range, &rhs);
            if (rhs.empty()) return bin(isAnd ? ir::BinOp::AndAlso : ir::BinOp::OrElse, ir::Ty::Int, a, b);
            // the right side does work (a call, a division), so it runs only when the left side did not decide
            const int t = newTemp(ir::Ty::Int);
            out.push_back(set(t, a, line));
            ir::St iff; iff.kind = ir::St::Kind::If; iff.line = line;
            iff.a = isAnd ? load(t) : un(ir::UnOp::Not, ir::Ty::Int, load(t));
            iff.then = std::move(rhs);
            iff.then.push_back(set(t, b, line));
            out.push_back(std::move(iff));
            return load(t);
        }
        case Expr::Kind::Binary: return binary(static_cast<const BinaryExpr*>(e), out);
        case Expr::Kind::Call:   return call(static_cast<const CallExpr*>(e), out);
        case Expr::Kind::Array:  return arrayLiteral(static_cast<const ArrayExpr*>(e), out, want);
        case Expr::Kind::Index:  return index(static_cast<const IndexExpr*>(e), out);
        case Expr::Kind::Map:    return mapLiteral(static_cast<const MapExpr*>(e), out, want);
        case Expr::Kind::Lambda: return lambda(static_cast<const LambdaExpr*>(e), out);
        case Expr::Kind::Ref:    refuse(e->range, "& arguments are not compiled yet");
        }
        refuse(e->range, "this expression");
    }

    ir::ExP arrayLiteral(const ArrayExpr* a, std::vector<ir::St>& out, ir::Ty want) {
        const int line = a->range.startLine;
        if (a->isRange) {
            // [start ... end] and [start, second ... end] as an array of ints. a for loop over one does not make the array
            std::vector<ir::ExP> parts;
            for (const ExprPtr& el : a->elements) {
                parts.push_back(value(el.get(), out));
                if (parts.back()->ty != ir::Ty::Int) refuse(el->range, "a range of floats is not compiled: the numbers of a range have to be ints");
            }
            const bool has2 = parts.size() == 3;
            const int t = newTemp(ir::Ty::ArrInt);
            ir::St st; st.kind = ir::St::Kind::Native; st.nop = ir::NativeOp::Rt; st.line = line; st.cname = "rt_range"; st.raises = true;
            st.args = {parts[0], has2 ? parts[1] : parts[0], cInt(has2 ? 1 : 0), parts.back()};
            st.hasDst = true; st.dst = {false, t};
            out.push_back(std::move(st));
            return load(t);
        }
        std::vector<ir::ExP> items;
        // an empty literal among the elements takes the type of the others
        for (const ExprPtr& el : a->elements)
            items.push_back(value(el.get(), out, ir::isArr(want) ? ir::elemOf(want) : !items.empty() ? items[0]->ty : ir::Ty::Void));
        ir::Ty ety = ir::Ty::Void;
        if (!items.empty()) {
            ety = items[0]->ty;
            bool mixed = false;
            for (size_t i = 0; i < items.size(); ++i) mixed = mixed || items[i]->ty != ety;
            if (ir::isArr(want) && ir::elemOf(want) == ir::Ty::Any) mixed = true, ety = ir::Ty::Any;
            if (mixed) {
                // elements of different types (or an array that is to hold anys): every element goes in as an any
                ety = ir::Ty::Any;
                for (size_t i = 0; i < items.size(); ++i) items[i] = toAny(items[i], a->elements[i]->range, out);
            }
        } else if (ir::isArr(want)) {
            ety = ir::elemOf(want);
        } else {
            auto it = fr.types.find(a);
            if (it != fr.types.end() && it->second.el) ety = annType(it->second.el->t, it->second.el->el);
            if (ety == ir::Ty::Void) refuse(a->range, "the element type of this empty array is not known (write local arr<int> name = [], with int, float, str, arr<...> or map<...>)");
        }
        if (!scalarElem(ety)) refuse(a->range, "an array of " + name(ety) + " is not compiled");
        const int t = newTemp(ir::arrOf(ety));
        out.push_back(native(ir::NativeOp::ArrNew, line, {}, t));
        for (const ir::ExP& x : items) out.push_back(native(ir::NativeOp::ArrPush, line, {load(t), x}, t, true));
        return load(t);
    }

    ir::ExP mapLiteral(const MapExpr* mp, std::vector<ir::St>& out, ir::Ty want) {
        const int line = mp->range.startLine;
        std::vector<ir::ExP> keys, vals;
        for (auto& kv : mp->entries) {
            keys.push_back(value(kv.first.get(), out));
            if (keys.back()->ty != ir::Ty::Str) refuse(kv.first->range, "a map key has to be a str");
            vals.push_back(value(kv.second.get(), out, ir::isMap(want) ? ir::elemOf(want) : !vals.empty() ? vals[0]->ty : ir::Ty::Void));
        }
        ir::Ty ety = ir::Ty::Void;
        if (!vals.empty()) {
            ety = vals[0]->ty;
            bool mixed = false;
            for (size_t i = 0; i < vals.size(); ++i) mixed = mixed || vals[i]->ty != ety;
            if (ir::isMap(want) && ir::elemOf(want) == ir::Ty::Any) mixed = true;
            if (mixed) {
                ety = ir::Ty::Any;
                for (size_t i = 0; i < vals.size(); ++i) vals[i] = toAny(vals[i], mp->entries[i].second->range, out);
            }
        } else if (ir::isMap(want)) {
            ety = ir::elemOf(want);
        } else {
            auto it = fr.types.find(mp);
            if (it != fr.types.end() && it->second.el) ety = annType(it->second.el->t, it->second.el->el);
            if (ety == ir::Ty::Void) refuse(mp->range, "the value type of this empty map is not known (write local map<int> name = {}, with int, float, str, arr<...> or map<...>)");
        }
        if (!scalarElem(ety)) refuse(mp->range, "a map of " + name(ety) + " is not compiled");
        const int t = newTemp(ir::mapOf(ety));
        out.push_back(native(ir::NativeOp::MapNew, line, {}, t));
        for (size_t i = 0; i < keys.size(); ++i) out.push_back(native(ir::NativeOp::MapSet, line, {load(t), keys[i], vals[i]}, t, true));
        return load(t);
    }

    ir::ExP index(const IndexExpr* x, std::vector<ir::St>& out) {
        const int line = x->range.startLine;
        ir::ExP o = value(x->object.get(), out);
        if (o->ty == ir::Ty::Err) {
            // the error object has three fields, named in the source
            if (x->index->kind != Expr::Kind::String) refuse(x->index->range, "an error is read as e[\"message\"], e[\"kind\"] or e[\"line\"]");
            const std::string& key = static_cast<const StringExpr*>(x->index.get())->v;
            if (key == "message") return nativeValue(ir::NativeOp::ErrMessage, ir::Ty::Str, line, {o}, out);
            if (key == "kind") return nativeValue(ir::NativeOp::ErrKind, ir::Ty::Str, line, {o}, out);
            if (key == "line") return nativeValue(ir::NativeOp::ErrLine, ir::Ty::Float, line, {o}, out);
            if (key == "trace") return rtCall("rt_err_trace", {o}, ir::arrOf(ir::mapOf(ir::Ty::Any)), false, line, out);
            refuse(x->index->range, "e[\"" + key + "\"] is not compiled (the error has message, kind, line and trace)");
        }
        ir::ExP i = value(x->index.get(), out);
        if (o->ty == ir::Ty::Any) return rtCall("rt_any_index", {o, toAny(i, x->index->range, out)}, ir::Ty::Any, true, line, out);
        if (i->ty == ir::Ty::Any && (ir::isMap(o->ty) || ir::isArr(o->ty) || o->ty == ir::Ty::Str))
            i = fromAny(i, ir::isMap(o->ty) ? ir::Ty::Str : ir::Ty::Int, x->index->range, out);
        if (ir::isMap(o->ty)) {
            if (i->ty != ir::Ty::Str) refuse(x->index->range, "a map key has to be a str, this is " + name(i->ty));
            return nativeValue(ir::NativeOp::MapGet, ir::elemOf(o->ty), line, {o, i}, out);
        }
        if (i->ty != ir::Ty::Int) refuse(x->index->range, "an index has to be an int, this is " + name(i->ty));
        if (o->ty == ir::Ty::Str) return nativeValue(ir::NativeOp::StrIndex, ir::Ty::Str, line, {o, i}, out);
        if (ir::isArr(o->ty)) return nativeValue(ir::NativeOp::ArrGet, ir::elemOf(o->ty), line, {o, i}, out);
        refuse(x->range, "indexing a " + name(o->ty));
    }

    ir::ExP binary(const BinaryExpr* b, std::vector<ir::St>& out) {
        const int line = b->range.startLine;
        ir::ExP l = value(b->left.get(), out);
        ir::ExP r = value(b->right.get(), out);
        const std::string& op = b->op;
        if (l->ty == ir::Ty::Any || r->ty == ir::Ty::Any) {
            // an operator on a value of no fixed type is worked out when the program runs, as the VM does
            const char* fn = op == "+" ? "rt_any_add" : op == "-" ? "rt_any_sub" : op == "*" ? "rt_any_mul" : op == "/" ? "rt_any_div" : op == "%" ? "rt_any_mod" : nullptr;
            const char* cmp = op == "<" ? "rt_any_lt" : op == "<=" ? "rt_any_le" : op == ">" ? "rt_any_gt" : op == ">=" ? "rt_any_ge" : nullptr;
            if (!fn && !cmp && op != "==" && op != "!=") refuse(b->range, "the operator " + op + " does not work on a value of no fixed type");
            ir::ExP la = toAny(l, b->left->range, out), ra = toAny(r, b->right->range, out);
            if (fn) return rtCall(fn, {la, ra}, ir::Ty::Any, true, line, out);
            if (cmp) return rtCall(cmp, {la, ra}, ir::Ty::Int, true, line, out);
            ir::ExP eq = rtCall("rt_any_eq", {la, ra}, ir::Ty::Int, false, line, out);
            return op == "==" ? eq : un(ir::UnOp::Not, ir::Ty::Int, eq);
        }
        if (ir::isHeap(l->ty) || ir::isHeap(r->ty)) {
            if (l->ty != ir::Ty::Str || r->ty != ir::Ty::Str)
                refuse(b->range, "the operator " + op + " does not work on " + name(l->ty) + " and " + name(r->ty));
            if (op == "+") return nativeValue(ir::NativeOp::StrConcat, ir::Ty::Str, line, {l, r}, out);
            if (op == "==") return nativeValue(ir::NativeOp::StrEq, ir::Ty::Int, line, {l, r}, out);
            if (op == "!=") return un(ir::UnOp::Not, ir::Ty::Int, nativeValue(ir::NativeOp::StrEq, ir::Ty::Int, line, {l, r}, out));
            refuse(b->range, "the operator " + op + " on strings");
        }
        const bool ints = l->ty == ir::Ty::Int && r->ty == ir::Ty::Int;
        auto arith = [&](ir::BinOp o) {
            if (ints) return bin(o, ir::Ty::Int, l, r);
            return bin(o, ir::Ty::Float, toFloat(l), toFloat(r));
        };
        auto compare = [&](ir::BinOp o) {
            if (ints) return bin(o, ir::Ty::Int, l, r);
            return bin(o, ir::Ty::Int, toFloat(l), toFloat(r));
        };
        auto intsOnly = [&] { if (!ints) refuse(b->range, "the operator " + op + " works on int only"); };
        if (op == "+") return arith(ir::BinOp::Add);
        if (op == "-") return arith(ir::BinOp::Sub);
        if (op == "*") return arith(ir::BinOp::Mul);
        if (op == "/") return raising(ir::RaiseOp::DivF, ir::Ty::Float, toFloat(l), toFloat(r), line, out);
        if (op == "%") {
            if (ints) return raising(ir::RaiseOp::ModI, ir::Ty::Int, l, r, line, out);
            return raising(ir::RaiseOp::ModF, ir::Ty::Float, toFloat(l), toFloat(r), line, out);
        }
        if (op == "<")  return compare(ir::BinOp::Lt);
        if (op == "<=") return compare(ir::BinOp::Le);
        if (op == ">")  return compare(ir::BinOp::Gt);
        if (op == ">=") return compare(ir::BinOp::Ge);
        if (op == "==") return compare(ir::BinOp::Eq);
        if (op == "!=") return compare(ir::BinOp::Ne);
        if (op == "&") { intsOnly(); return bin(ir::BinOp::BitAnd, ir::Ty::Int, l, r); }
        if (op == "|") { intsOnly(); return bin(ir::BinOp::BitOr, ir::Ty::Int, l, r); }
        if (op == "^") { intsOnly(); return bin(ir::BinOp::BitXor, ir::Ty::Int, l, r); }
        if ((op == "<<" || op == ">>") && ints && r->kind == ir::Ex::Kind::ConstInt && r->i >= 0 && r->i < 64)
            return bin(op == "<<" ? ir::BinOp::Shl : ir::BinOp::Shr, ir::Ty::Int, l, r);      // a count that is there and fine: nothing can go wrong
        if (op == "<<") { intsOnly(); return raising(ir::RaiseOp::Shl, ir::Ty::Int, l, r, line, out); }
        if (op == ">>") { intsOnly(); return raising(ir::RaiseOp::Shr, ir::Ty::Int, l, r, line, out); }
        refuse(b->range, "the operator " + op);
    }

    // an array variable the way a call or an assignment names it: a local or a captured one
    // `a[i]` and `m["k"][j]` can be named too, when what they hold is a container: the container is made unique one level at a time and the result is a
    // variable that points into it (it owns no count), so the caller changes the nested container in place
    bool containerVar(const Expr* e, std::vector<ir::St>& out, ir::Ref& ref, ir::Ty& ty) {
        if (e->kind == Expr::Kind::Index) {
            auto* ix = static_cast<const IndexExpr*>(e);
            ir::Ref pref; ir::Ty pty;
            if (!containerVar(ix->object.get(), out, pref, pty) || !(ir::isArr(pty) || ir::isMap(pty) || pty == ir::Ty::Any)) return false;
            if (pty == ir::Ty::Any || ir::elemOf(pty) == ir::Ty::Any) {
                // an any inside a container (or inside an any): the address of its slot, so a change is made where it lives
                if (pty != ir::Ty::Any && ix->object->kind == Expr::Kind::Var) out.push_back(native(ir::NativeOp::Own, e->range.startLine, {}, pref, true));
                ir::ExP k = value(ix->index.get(), out);
                if (pty == ir::Ty::Any) k = toAny(k, ix->index->range, out);
                else {
                    const ir::Ty want = ir::isMap(pty) ? ir::Ty::Str : ir::Ty::Int;
                    k = coerce(k, want, ix->index->range, out);
                    if (k->ty != want) refuse(ix->index->range, ir::isMap(pty) ? "a map key has to be a str" : "an index has to be an int");
                }
                const int t = newTemp(ir::Ty::Any);
                cur->vars[(size_t)t].borrowed = true;
                cur->vars[(size_t)t].slotref = true;
                out.push_back(native(ir::NativeOp::ChildAny, e->range.startLine, {loadRef(pref, pty), k}, t));
                ref = ir::Ref{false, t}; ty = ir::Ty::Any;
                return true;
            }
            const ir::Ty cty = ir::elemOf(pty);
            if (!(ir::isArr(cty) || ir::isMap(cty))) return false;
            if (ix->object->kind == Expr::Kind::Var) out.push_back(native(ir::NativeOp::Own, e->range.startLine, {}, pref, true));    // the root first
            ir::ExP k = value(ix->index.get(), out);
            if (ir::isMap(pty) ? k->ty != ir::Ty::Str : k->ty != ir::Ty::Int)
                refuse(ix->index->range, ir::isMap(pty) ? "a map key has to be a str" : "an index has to be an int");
            const int t = newTemp(cty);
            cur->vars[(size_t)t].borrowed = true;
            out.push_back(native(ir::isArr(cty) ? ir::NativeOp::ChildArr : ir::NativeOp::ChildMap, e->range.startLine, {loadRef(pref, pty), k}, t));
            ref = ir::Ref{false, t}; ty = cty;
            return true;
        }
        if (e->kind != Expr::Kind::Var) return false;
        VarInfo v = lookup(static_cast<const VarExpr*>(e)->name, e->range);
        if (!v.found && inFn && !plugin && topName(static_cast<const VarExpr*>(e)->name)) useGlobal(static_cast<const VarExpr*>(e)->name, e->range, "uses");
        if (!v.found || v.ref.self) return false;
        if (v.ref.global && plugin && !atTop()) refuse(e->range, "'" + static_cast<const VarExpr*>(e)->name + "' is a top level name of a plugin, which its functions only read");
        ref = v.ref; ty = v.ty;
        return true;
    }

    // a native is there only after the file imports its plugin, as in the VM
    void requireImport(const std::string& plug, const std::string& fname, const SourceRange& at) {
        if (!imported.count(plug)) refuse(at, "'" + fname + "' comes from the plugin " + plug + ", which this file does not import (add import \"" + plug + "\")");
    }
    static bool isEmbrlibBuiltin(const std::string& n) {
        return n == "len" || n == "str" || n == "push" || n == "append" || n == "keys" || n == "values" || n == "error" || n == "join" || n == "type" || n == "concat";
    }

    // the natives in the table (natives.h): evaluate the arguments, find the overload that takes their types, and call the C function
    ir::ExP tableNative(const std::string& fname, const CallExpr* c, std::vector<ir::St>& out, bool& handled) {
        handled = false;
        const char* plug = nativePlugin(fname);
        if (!plug) return nullptr;
        handled = true;
        requireImport(plug, fname, c->range);
        const int line = c->range.startLine;
        const bool refFirst = !c->args.empty() && c->args[0]->kind == Expr::Kind::Ref;
        std::vector<ir::ExP> args;
        for (size_t i = 0; i < c->args.size(); ++i) {
            if (i == 0 && refFirst) {
                const Expr* target = static_cast<const RefExpr*>(c->args[0].get())->target.get();
                ir::Ref ref; ir::Ty ty;
                if (!containerVar(target, out, ref, ty) || !(ir::isArr(ty) || ir::isMap(ty))) refuse(target->range, fname + " needs an array or a map variable after the &");
                args.push_back(loadRef(ref, ty));
            } else {
                // a literal handed along with a first container (map_merge, concat) takes its type from it, so an empty one can be written
                args.push_back(value(c->args[i].get(), out, i > 0 && !args.empty() && (ir::isArr(args[0]->ty) || ir::isMap(args[0]->ty)) ? args[0]->ty : ir::Ty::Void));
            }
        }
        // an argument that is an any: has() asks the value itself, and for the others the types the native takes are tried one by one, with a check
        // when the program runs that the value is of the type it was taken as
        {
            std::vector<size_t> anys;
            for (size_t i = 0; i < args.size(); ++i) if (args[i]->ty == ir::Ty::Any) anys.push_back(i);
            if (!anys.empty() && fname == "has" && args.size() == 2)
                return rtCall("rt_any_has", {args[0], toAny(args[1], c->args[1]->range, out)}, ir::Ty::Int, true, line, out);
            if (!anys.empty() && anys.size() <= 3 && !refFirst) {
                static const ir::Ty tries[] = {ir::Ty::Int, ir::Ty::Float, ir::Ty::Str, ir::Ty::ArrInt, ir::Ty::ArrFloat, ir::Ty::ArrStr,
                                               ir::Ty::MapInt, ir::Ty::MapFloat, ir::Ty::MapStr};
                const size_t nt = sizeof tries / sizeof tries[0];
                size_t total = 1;
                for (size_t k = 0; k < anys.size(); ++k) total *= nt;
                for (size_t combo = 0; combo < total; ++combo) {
                    std::vector<ir::Ty> trial;
                    for (const ir::ExP& a : args) trial.push_back(a->ty);
                    size_t rest = combo;
                    for (size_t k = 0; k < anys.size(); ++k) { trial[anys[k]] = tries[rest % nt]; rest /= nt; }
                    if (!findNative(fname, trial)) continue;
                    for (size_t k = 0; k < anys.size(); ++k) args[anys[k]] = fromAny(args[anys[k]], trial[anys[k]], c->args[anys[k]]->range, out);
                    break;
                }
            }
        }
        std::vector<ir::Ty> types;
        for (const ir::ExP& a : args) types.push_back(a->ty);
        const NativeSpec* spec = findNative(fname, types);
        if (!spec) {
            std::string have;
            for (size_t i = 0; i < types.size(); ++i) have += std::string(i ? ", " : "") + name(types[i]);
            refuse(c->range, fname + " is not compiled for (" + have + ")");
        }
        if (spec->mutates != refFirst)
            refuse(c->range, spec->mutates ? fname + " changes its array or map, so it takes it as &name" : fname + " does not change its first argument, pass it without the &");
        for (size_t i = 0; i < args.size(); ++i)
            if (spec->params[i] == 'n') args[i] = toFloat(args[i]);
        const ir::Ty rty = nativeResult(*spec, types);
        ir::St st; st.kind = ir::St::Kind::Native; st.nop = ir::NativeOp::Rt; st.line = line; st.args = std::move(args);
        st.cname = spec->cname; st.raises = spec->raises; st.inplace = spec->mutates;
        // an array that comes out empty still needs its kind (what its elements are), which only the type says
        if (fname == "flatten" || fname == "map_merge" || fname == "map_pick" || fname == "map_omit") st.args.push_back(cInt(ir::elemKind(rty)));
        else if (fname == "chunk" || fname == "zip" || fname == "enumerate") {
            st.args.push_back(cInt(ir::elemKind(ir::elemOf(rty))));
        }
        if (rty == ir::Ty::Void) { out.push_back(std::move(st)); return nullptr; }
        const int t = newTemp(rty);
        st.hasDst = true; st.dst = {false, t};
        out.push_back(std::move(st));
        return load(t);
    }

    // a function handed to filter, reduce and the like: a lambda written there takes its parameter types from the call, anything else is a value
    // that has to have the signature the call needs
    ir::ExP functionArg(const Expr* e, const Expect& want, std::vector<ir::St>& out, const std::string& who) {
        ir::ExP f;
        if (e->kind == Expr::Kind::Lambda) f = lambda(static_cast<const LambdaExpr*>(e), out, nullptr, false, nullptr, &want);
        else f = value(e, out);
        if (!ir::isFn(f->ty)) refuse(e->range, who + " needs a function here, this is " + name(f->ty));
        const ir::FnSig& sg = sigTable[(size_t)ir::sigOf(f->ty)];
        if (sg.params != want.params) refuse(e->range, who + " calls its function with " + std::to_string(want.params.size()) + " argument(s) of the types it has, this function takes " + sigText(f->ty));
        return f;
    }
    // call a function value with these arguments; the result, or null when the function gives nothing
    ir::ExP callFn(ir::ExP f, std::vector<ir::ExP> args, int line, std::vector<ir::St>& out) {
        const ir::Ty rty = sigTable[(size_t)ir::sigOf(f->ty)].ret;
        ir::St s; s.kind = ir::St::Kind::CallValue; s.line = line; s.file = curFileIdx; s.a = std::move(f); s.args = std::move(args);
        if (rty == ir::Ty::Void) { out.push_back(std::move(s)); return nullptr; }
        const int t = newTemp(rty);
        s.hasDst = true; s.dst = {false, t};
        out.push_back(std::move(s));
        return load(t);
    }
    // for each element of an array (as it was when the loop started): `body` writes into the loop the statements for one element
    template <class F>
    void eachElement(ir::ExP arr, int line, std::vector<ir::St>& out, F body) {
        const int snap = newTemp(arr->ty), idx = newTemp(ir::Ty::Int), x = newTemp(ir::elemOf(arr->ty));
        out.push_back(set(snap, arr, line));
        out.push_back(set(idx, cInt(0), line));
        ir::St loop; loop.kind = ir::St::Kind::Loop; loop.line = line;
        ir::St iff; iff.kind = ir::St::Kind::If; iff.line = line; iff.a = bin(ir::BinOp::Ge, ir::Ty::Int, load(idx), lenOf(load(snap)));
        ir::St b; b.kind = ir::St::Kind::Break; b.line = line;
        iff.then.push_back(std::move(b));
        loop.body.push_back(std::move(iff));
        const int here = newTemp(ir::Ty::Int);
        loop.body.push_back(native(ir::NativeOp::ArrGet, line, {load(snap), load(idx)}, x));
        loop.body.push_back(set(here, load(idx), line));
        loop.body.push_back(set(idx, bin(ir::BinOp::Add, ir::Ty::Int, load(idx), cInt(1)), line));
        body(loop.body, load(x), load(here));
        out.push_back(std::move(loop));
    }
    // filter, for_each_do (a map over an array), reduce, any_do, all_do, count_do, find_do, find_index_do: they call a function for each element
    bool higherOrder(const std::string& fname, const CallExpr* c, std::vector<ir::St>& out, ir::ExP& result) {
        static const std::set<std::string> names = {"filter", "for_each_do", "reduce", "any_do", "all_do", "count_do", "find_do", "find_index_do", "sort_cmp", "sort_do", "partition", "min_do", "max_do", "group_by"};
        if (!names.count(fname)) return false;
        const int line = c->range.startLine;
        const bool isReduce = fname == "reduce", isFind = fname == "find_do", isSort = fname == "sort_cmp" || fname == "sort_do";
        const size_t lo = isReduce ? 3 : 2, hi = (isFind || isSort) ? 3 : lo;
        if (c->args.size() < lo || c->args.size() > hi) refuse(c->range, fname + " takes " + (lo == hi ? std::to_string(lo) : std::to_string(lo) + " or " + std::to_string(hi)) + " arguments here");
        ir::ExP arr = value(c->args[0].get(), out);
        if (!ir::isArr(arr->ty)) refuse(c->args[0]->range, fname + " is only compiled for arrays, this is " + name(arr->ty));
        const ir::Ty ety = ir::elemOf(arr->ty);
        auto yes = [&](ir::ExP r, const Expr* at, std::vector<ir::St>& where) { return truth(std::move(r), at->range, &where); };
        if (isReduce) {
            ir::ExP init = value(c->args[2].get(), out);
            if (!scalarElem(init->ty)) refuse(c->args[2]->range, "reduce starts from a " + name(init->ty) + ", which is not compiled");
            ir::ExP f = functionArg(c->args[1].get(), Expect{{init->ty, ety}, init->ty}, out, "reduce");
            if (sigTable[(size_t)ir::sigOf(f->ty)].ret != init->ty) refuse(c->args[1]->range, "reduce: the function gives " + name(sigTable[(size_t)ir::sigOf(f->ty)].ret) + ", the start value is " + name(init->ty));
            const int acc = newTemp(init->ty);
            out.push_back(set(acc, init, line));
            eachElement(arr, line, out, [&](std::vector<ir::St>& body, ir::ExP x, ir::ExP) {
                ir::ExP r = callFn(f, {load(acc), x}, line, body);
                body.push_back(set(acc, r, line));
            });
            result = load(acc);
            return true;
        }
        if (isSort) {
            ir::ExP desc = c->args.size() == 3 ? truth(value(c->args[2].get(), out), c->args[2]->range, &out) : cInt(0);
            const int res = newTemp(arr->ty);
            if (fname == "sort_cmp") {
                ir::ExP f = functionArg(c->args[1].get(), Expect{{ety, ety}, ir::Ty::Void}, out, "sort_cmp");
                const ir::Ty r = sigTable[(size_t)ir::sigOf(f->ty)].ret;
                if (r != ir::Ty::Int && r != ir::Ty::Float) refuse(c->args[1]->range, "sort_cmp: the function gives " + name(r) + ", it has to give a number (below 0 when the first goes first)");
                out.push_back(native(ir::NativeOp::SortCmp, line, {arr, f, desc}, res));
                result = load(res);
                return true;
            }
            // sort_do: the weight of each element is worked out once, then the elements are ordered by it
            ir::ExP f = functionArg(c->args[1].get(), Expect{{ety}, ir::Ty::Void}, out, "sort_do");
            const ir::Ty w = sigTable[(size_t)ir::sigOf(f->ty)].ret;
            if (w != ir::Ty::Int && w != ir::Ty::Float) refuse(c->args[1]->range, "sort_do: the weight is " + name(w) + ", it has to be a number");
            const int ws = newTemp(ir::arrOf(w));
            out.push_back(native(ir::NativeOp::ArrNew, line, {}, ws));
            eachElement(arr, line, out, [&](std::vector<ir::St>& body, ir::ExP x, ir::ExP) {
                body.push_back(native(ir::NativeOp::ArrPush, line, {load(ws), callFn(f, {x}, line, body)}, ws, true));
            });
            ir::St st; st.kind = ir::St::Kind::Native; st.nop = ir::NativeOp::Rt; st.line = line;
            st.cname = w == ir::Ty::Int ? "rt_sort_by_i64" : "rt_sort_by_f64"; st.args = {arr, load(ws), desc};
            st.hasDst = true; st.dst = {false, res};
            out.push_back(std::move(st));
            result = load(res);
            return true;
        }
        if (fname == "for_each_do") {
            ir::ExP f = functionArg(c->args[1].get(), Expect{{ety}, ir::Ty::Void}, out, fname);
            const ir::Ty rty = sigTable[(size_t)ir::sigOf(f->ty)].ret;
            if (rty != ir::Ty::Void && !scalarElem(rty)) refuse(c->args[1]->range, "for_each_do: the function gives " + name(rty) + ", which cannot be an element");
            const int res = newTemp(rty == ir::Ty::Void ? ir::Ty::ArrFloat : ir::arrOf(rty));
            out.push_back(native(ir::NativeOp::ArrNew, line, {}, res));
            eachElement(arr, line, out, [&](std::vector<ir::St>& body, ir::ExP x, ir::ExP) {
                ir::ExP r = callFn(f, {x}, line, body);
                // a function that gives nothing: the VM collects 0.0 for each
                body.push_back(native(ir::NativeOp::ArrPush, line, {load(res), r ? r : cFloat(0.0)}, res, true));
            });
            result = load(res);
            return true;
        }
        if (fname == "min_do" || fname == "max_do") {
            // the element with the smallest (or largest) weight; the first one when several are equal
            ir::ExP f = functionArg(c->args[1].get(), Expect{{ety}, ir::Ty::Void}, out, fname);
            const ir::Ty w = sigTable[(size_t)ir::sigOf(f->ty)].ret;
            if (w != ir::Ty::Int && w != ir::Ty::Float) refuse(c->args[1]->range, fname + ": the weight is " + name(w) + ", it has to be a number");
            const bool isMin = fname == "min_do";
            ir::St empty; empty.kind = ir::St::Kind::If; empty.line = line; empty.a = bin(ir::BinOp::Eq, ir::Ty::Int, lenOf(arr), cInt(0));
            { ir::St r; r.kind = ir::St::Kind::Native; r.nop = ir::NativeOp::Rt; r.line = line; r.cname = isMin ? "rt_min_do_empty" : "rt_max_do_empty"; r.raises = true; empty.then.push_back(std::move(r)); }
            out.push_back(std::move(empty));
            const int have = newTemp(ir::Ty::Int), best = newTemp(ety), bestw = newTemp(w);
            out.push_back(set(have, cInt(0), line));
            eachElement(arr, line, out, [&](std::vector<ir::St>& body, ir::ExP x, ir::ExP) {
                ir::ExP r = callFn(f, {x}, line, body);
                const int cw = newTemp(w);
                body.push_back(set(cw, r, line));
                ir::St iff; iff.kind = ir::St::Kind::If; iff.line = line;
                iff.a = bin(ir::BinOp::OrElse, ir::Ty::Int, un(ir::UnOp::Not, ir::Ty::Int, load(have)),
                            bin(isMin ? ir::BinOp::Lt : ir::BinOp::Gt, ir::Ty::Int, load(cw), load(bestw)));
                iff.then.push_back(set(have, cInt(1), line));
                iff.then.push_back(set(best, x, line));
                iff.then.push_back(set(bestw, load(cw), line));
                body.push_back(std::move(iff));
            });
            result = load(best);
            return true;
        }
        if (fname == "group_by") {
            // a map from the key (text) to the array of the elements that have it, in their order
            ir::ExP f = functionArg(c->args[1].get(), Expect{{ety}, ir::Ty::Void}, out, fname);
            const ir::Ty kt = sigTable[(size_t)ir::sigOf(f->ty)].ret;
            if (kt != ir::Ty::Int && kt != ir::Ty::Str) refuse(c->args[1]->range, "group_by: the key is " + name(kt) + ", it has to be an int or a str");
            const int groups = newTemp(ir::mapOf(arr->ty));
            out.push_back(native(ir::NativeOp::MapNew, line, {}, groups));
            eachElement(arr, line, out, [&](std::vector<ir::St>& body, ir::ExP x, ir::ExP) {
                ir::ExP k = callFn(f, {x}, line, body);
                if (kt == ir::Ty::Int) k = nativeValue(ir::NativeOp::StrOfInt, ir::Ty::Str, line, {k}, body);
                const int ks = newTemp(ir::Ty::Str), has = newTemp(ir::Ty::Int);
                body.push_back(set(ks, k, line));
                body.push_back(native(ir::NativeOp::MapHas, line, {load(groups), load(ks)}, has));
                ir::St mk; mk.kind = ir::St::Kind::If; mk.line = line; mk.a = un(ir::UnOp::Not, ir::Ty::Int, load(has));
                const int fresh = newTemp(arr->ty);
                mk.then.push_back(native(ir::NativeOp::ArrNew, line, {}, fresh));
                mk.then.push_back(native(ir::NativeOp::MapSet, line, {load(groups), load(ks), load(fresh)}, groups, true));
                body.push_back(std::move(mk));
                body.push_back(native(ir::NativeOp::Own, line, {}, ir::Ref{false, groups}, true));
                const int g = newTemp(arr->ty);
                cur->vars[(size_t)g].borrowed = true;
                body.push_back(native(ir::NativeOp::ChildArr, line, {load(groups), load(ks)}, g));
                body.push_back(native(ir::NativeOp::ArrPush, line, {load(g), x}, g, true));
            });
            result = load(groups);
            return true;
        }
        ir::ExP f = functionArg(c->args[1].get(), Expect{{ety}, ir::Ty::Int}, out, fname);
        const ir::Ty fr_ = sigTable[(size_t)ir::sigOf(f->ty)].ret;
        if (fr_ != ir::Ty::Int && fr_ != ir::Ty::Float) refuse(c->args[1]->range, fname + ": the function gives " + name(fr_) + ", it has to give a number");
        if (fname == "partition") {
            // [the elements the function says yes to, the others]
            const int yesArr = newTemp(arr->ty), noArr = newTemp(arr->ty);
            out.push_back(native(ir::NativeOp::ArrNew, line, {}, yesArr));
            out.push_back(native(ir::NativeOp::ArrNew, line, {}, noArr));
            eachElement(arr, line, out, [&](std::vector<ir::St>& body, ir::ExP x, ir::ExP) {
                ir::ExP r = callFn(f, {x}, line, body);
                ir::St iff; iff.kind = ir::St::Kind::If; iff.line = line; iff.a = yes(r, c->args[1].get(), body);
                iff.then.push_back(native(ir::NativeOp::ArrPush, line, {load(yesArr), x}, yesArr, true));
                iff.els.push_back(native(ir::NativeOp::ArrPush, line, {load(noArr), x}, noArr, true));
                body.push_back(std::move(iff));
            });
            const int outer = newTemp(ir::arrOf(arr->ty));
            out.push_back(native(ir::NativeOp::ArrNew, line, {}, outer));
            out.push_back(native(ir::NativeOp::ArrPush, line, {load(outer), load(yesArr)}, outer, true));
            out.push_back(native(ir::NativeOp::ArrPush, line, {load(outer), load(noArr)}, outer, true));
            result = load(outer);
            return true;
        }
        if (fname == "filter") {
            const int res = newTemp(arr->ty);
            out.push_back(native(ir::NativeOp::ArrNew, line, {}, res));
            eachElement(arr, line, out, [&](std::vector<ir::St>& body, ir::ExP x, ir::ExP) {
                ir::ExP r = callFn(f, {x}, line, body);
                ir::St iff; iff.kind = ir::St::Kind::If; iff.line = line; iff.a = yes(r, c->args[1].get(), body);
                iff.then.push_back(native(ir::NativeOp::ArrPush, line, {load(res), x}, res, true));
                body.push_back(std::move(iff));
            });
            result = load(res);
            return true;
        }
        // any_do, all_do, count_do, find_index_do, find_do: a counter, a flag or an index, and a break when the answer is known
        const int acc = newTemp(ir::Ty::Int);
        const bool wantsIndex = fname == "find_index_do";
        const bool stops = fname == "any_do" || fname == "all_do" || fname == "find_index_do" || isFind;
        out.push_back(set(acc, cInt(fname == "all_do" ? 1 : wantsIndex ? -1 : 0), line));
        int found = -1;
        ir::ExP deflt;
        if (isFind) {
            found = newTemp(ir::Ty::Int);
            out.push_back(set(found, cInt(0), line));
        }
        const int hit = isFind ? newTemp(ety) : -1;
        eachElement(arr, line, out, [&](std::vector<ir::St>& body, ir::ExP x, ir::ExP idx) {
            ir::ExP r = callFn(f, {x}, line, body);
            ir::ExP ok = yes(r, c->args[1].get(), body);
            if (fname == "all_do") ok = un(ir::UnOp::Not, ir::Ty::Int, ok);
            ir::St iff; iff.kind = ir::St::Kind::If; iff.line = line; iff.a = ok;
            if (fname == "count_do") iff.then.push_back(set(acc, bin(ir::BinOp::Add, ir::Ty::Int, load(acc), cInt(1)), line));
            else if (fname == "any_do") iff.then.push_back(set(acc, cInt(1), line));
            else if (fname == "all_do") iff.then.push_back(set(acc, cInt(0), line));
            else if (wantsIndex) iff.then.push_back(set(acc, idx, line));
            else { iff.then.push_back(set(found, cInt(1), line)); iff.then.push_back(set(hit, x, line)); }
            if (stops) { ir::St b; b.kind = ir::St::Kind::Break; b.line = line; iff.then.push_back(std::move(b)); }
            body.push_back(std::move(iff));
        });
        if (isFind) {
            // no match: the default if one was given, else an error
            ir::ExP d = c->args.size() == 3 ? value(c->args[2].get(), out, ety) : nullptr;
            if (d && d->ty != ety) refuse(c->args[2]->range, "find_do: the default is " + name(d->ty) + ", the array holds " + name(ety));
            ir::St miss; miss.kind = ir::St::Kind::If; miss.line = line; miss.a = un(ir::UnOp::Not, ir::Ty::Int, load(found));
            if (d) miss.then.push_back(set(hit, d, line));
            else {
                ir::St r; r.kind = ir::St::Kind::Native; r.nop = ir::NativeOp::Rt; r.line = line; r.cname = "rt_find_missing"; r.raises = true;
                miss.then.push_back(std::move(r));
            }
            out.push_back(std::move(miss));
            result = load(hit);
            return true;
        }
        result = load(acc);
        return true;
    }

    // the built in functions (embrlib). a function of your own with the same name wins
    ir::ExP builtin(const std::string& fname, const CallExpr* c, std::vector<ir::St>& out, bool& handled) {
        handled = true;
        const int line = c->range.startLine;
        auto argc = [&](size_t lo, size_t hi) {
            if (c->args.size() < lo || c->args.size() > hi)
                refuse(c->range, fname + " takes " + (lo == hi ? std::to_string(lo) : std::to_string(lo) + " or " + std::to_string(hi)) + " arguments here");
        };
        if (fname == "print") {
            if (c->args.empty()) refuse(c->range, "print takes at least one argument here");
            ir::St s; s.kind = ir::St::Kind::Print; s.line = line;
            std::vector<ir::ExP> parts;
            for (const ExprPtr& a : c->args) {
                ir::ExP v = value(a.get(), out);
                if (ir::isFn(v->ty)) refuse(a->range, "printing a function is not compiled");
                parts.push_back(std::move(v));
            }
            if (parts.size() == 1) s.a = parts[0]; else s.args = std::move(parts);
            out.push_back(std::move(s));
            return nullptr;
        }
        if (fname == "len") {
            argc(1, 1);
            ir::ExP a = value(c->args[0].get(), out);
            if (a->ty == ir::Ty::Any) return rtCall("rt_any_len", {a}, ir::Ty::Int, true, line, out);
            if (a->ty != ir::Ty::Str && !ir::isArr(a->ty) && !ir::isMap(a->ty)) refuse(c->args[0]->range, "len of a " + name(a->ty) + " is not compiled");
            // read now, not when the result is used: a later append in the same expression would change it
            const int t = newTemp(ir::Ty::Int);
            out.push_back(set(t, lenOf(a), line));
            return load(t);
        }
        if (fname == "str") {
            argc(1, 1);
            ir::ExP a = value(c->args[0].get(), out);
            if (a->ty == ir::Ty::Str) return a;
            if (a->ty == ir::Ty::Any || ir::isArr(a->ty) || ir::isMap(a->ty)) return rtCall("rt_any_text", {toAny(a, c->args[0]->range, out)}, ir::Ty::Str, false, line, out);
            if (a->ty == ir::Ty::Int) return nativeValue(ir::NativeOp::StrOfInt, ir::Ty::Str, line, {a}, out);
            if (a->ty == ir::Ty::Float) return nativeValue(ir::NativeOp::StrOfFloat, ir::Ty::Str, line, {a}, out);
            refuse(c->args[0]->range, "str of a " + name(a->ty) + " is not compiled yet");
        }
        if (!sigs.count(fname) && !lookup(fname, c->range).found && (fname == "filter" || fname == "for_each_do" || fname == "reduce" || fname == "any_do" || fname == "all_do" ||
            fname == "count_do" || fname == "find_do" || fname == "find_index_do" || fname == "sort_cmp" || fname == "sort_do" || fname == "partition" || fname == "min_do" || fname == "max_do" || fname == "group_by")) {
            requireImport("embrlib", fname, c->range);
            ir::ExP r;
            higherOrder(fname, c, out, r);
            return r;
        }
        if (fname == "push") {
            argc(2, 2);
            ir::ExP a = value(c->args[0].get(), out);
            if (a->ty == ir::Ty::Any) {
                // push gives a new array: the copy shares the box until the push makes its own
                const int t = newTemp(ir::Ty::Any);
                out.push_back(set(t, a, line));
                ir::ExP v = value(c->args[1].get(), out);
                out.push_back(native(ir::NativeOp::AnyPush, line, {load(t), toAny(v, c->args[1]->range, out)}, t, true));
                return load(t);
            }
            if (!ir::isArr(a->ty)) refuse(c->args[0]->range, "push needs an array, this is " + name(a->ty));
            ir::ExP x = value(c->args[1].get(), out, ir::elemOf(a->ty));
            x = elementFor(x, a, c->args[1]->range, out);
            if (x->ty != ir::elemOf(a->ty)) { if (scalarElem(x->ty)) learn(a, ir::arrOf(x->ty)); refuse(c->args[1]->range, "this is " + name(x->ty) + ", the array holds " + name(ir::elemOf(a->ty))); }
            return nativeValue(ir::NativeOp::ArrPush, a->ty, line, {a, x}, out);
        }
        if (fname == "append") {
            argc(2, 2);
            if (c->args[0]->kind != Expr::Kind::Ref) refuse(c->args[0]->range, "append takes the array as &name");
            const Expr* target = static_cast<const RefExpr*>(c->args[0].get())->target.get();
            ir::Ref ref; ir::Ty at;
            if (!containerVar(target, out, ref, at) || !(ir::isArr(at) || at == ir::Ty::Any)) refuse(target->range, "append needs an array variable");
            if (at == ir::Ty::Any) {
                ir::ExP v = value(c->args[1].get(), out);
                out.push_back(native(ir::NativeOp::AnyPush, line, {loadRef(ref, at), toAny(v, c->args[1]->range, out)}, ref, true));
                return rtCall("rt_any_len", {loadRef(ref, at)}, ir::Ty::Int, true, line, out);
            }
            ir::ExP x = value(c->args[1].get(), out, ir::elemOf(at));
            x = elementFor(x, loadRef(ref, at), c->args[1]->range, out);
            if (x->ty != ir::elemOf(at)) { if (scalarElem(x->ty)) learn(loadRef(ref, at), ir::arrOf(x->ty)); refuse(c->args[1]->range, "this is " + name(x->ty) + ", the array holds " + name(ir::elemOf(at))); }
            out.push_back(native(ir::NativeOp::ArrPush, line, {loadRef(ref, at), x}, ref, true));
            const int t = newTemp(ir::Ty::Int);
            out.push_back(set(t, lenOf(loadRef(ref, at)), line));
            return load(t);
        }
        if (fname == "keys" || fname == "values") {
            argc(1, 1);
            ir::ExP mp = value(c->args[0].get(), out);
            if (mp->ty == ir::Ty::Any) return rtCall(fname == "keys" ? "rt_any_keys" : "rt_any_values", {mp}, fname == "keys" ? ir::Ty::ArrStr : ir::arrOf(ir::Ty::Any), true, line, out);
            if (!ir::isMap(mp->ty)) refuse(c->args[0]->range, fname + " is only compiled for maps, this is " + name(mp->ty));
            if (fname == "keys") return nativeValue(ir::NativeOp::MapKeys, ir::Ty::ArrStr, line, {mp}, out);
            return nativeValue(ir::NativeOp::MapValues, ir::arrOf(ir::elemOf(mp->ty)), line, {mp}, out);
        }
        if (fname == "type") {
            // the type of every value is known when the program is compiled, so this is a constant
            argc(1, 1);
            ir::ExP v = value(c->args[0].get(), out);
            if (v->ty == ir::Ty::Any) return rtCall("rt_any_type_name", {v}, ir::Ty::Str, false, line, out);
            const char* tn_ = v->ty == ir::Ty::Int ? "int" : v->ty == ir::Ty::Float ? "number" : v->ty == ir::Ty::Str ? "string" : ir::isArr(v->ty) ? "array"
                            : (ir::isMap(v->ty) || v->ty == ir::Ty::Err) ? "map" : "function";
            return cStr(tn_);
        }
        if (fname == "concat") {
            if (c->args.size() < 2) refuse(c->range, "concat takes at least 2 arguments");
            ir::ExP acc = value(c->args[0].get(), out);
            for (size_t i = 1; i < c->args.size(); ++i) {
                ir::ExP next = value(c->args[i].get(), out);
                if (acc->ty == ir::Ty::Str && next->ty == ir::Ty::Str) acc = nativeValue(ir::NativeOp::StrConcat, ir::Ty::Str, line, {acc, next}, out);
                else if (ir::isArr(acc->ty) && acc->ty == next->ty) {
                    const int t = newTemp(acc->ty);
                    ir::St st; st.kind = ir::St::Kind::Native; st.nop = ir::NativeOp::Rt; st.line = line; st.args = {acc, next}; st.cname = "rt_concat_arr";
                    st.hasDst = true; st.dst = {false, t};
                    out.push_back(std::move(st));
                    acc = load(t);
                } else refuse(c->args[i]->range, "cannot mix " + name(acc->ty) + " and " + name(next->ty) + " (argument " + std::to_string(i) + ")");
            }
            return acc;
        }
        if (fname == "error") {
            argc(1, 1);
            ir::ExP msg = value(c->args[0].get(), out);
            if (msg->ty != ir::Ty::Str) refuse(c->args[0]->range, "error takes a str, this is " + name(msg->ty));
            out.push_back(native(ir::NativeOp::Raise, line, {msg}));
            return nullptr;
        }
        if (fname == "join") {
            argc(1, 2);
            ir::ExP a = value(c->args[0].get(), out);
            if (a->ty != ir::Ty::ArrStr) refuse(c->args[0]->range, "join needs an arr<str>, this is " + name(a->ty));
            ir::ExP sep = c->args.size() > 1 ? value(c->args[1].get(), out) : cStr("");
            if (sep->ty != ir::Ty::Str) refuse(c->args[1]->range, "the separator has to be a str");
            return nativeValue(ir::NativeOp::Join, ir::Ty::Str, line, {a, sep}, out);
        }
        handled = false;
        return nullptr;
    }

    // a call of a function of a C plugin the file imports. the plugin's table says what it takes and gives, which is checked here
    ir::ExP pluginCall(const std::string& fname, const CallExpr* c, std::vector<ir::St>& out, bool& handled) {
        handled = false;
        auto it = pluginFns.find(fname);
        if (it == pluginFns.end()) return nullptr;
        handled = true;
        const PluginFnDesc& f = *it->second.second;
        const int line = c->range.startLine;
        size_t required = 0;
        bool variadic = false;
        for (const PluginParam& p : f.params) { if (p.flags & EMBR_PARAM_VARIADIC) variadic = true; else if (!(p.flags & EMBR_PARAM_OPTIONAL)) ++required; }
        if (c->args.size() < required || (!variadic && c->args.size() > f.params.size()))
            refuse(c->range, fname + " takes " + std::to_string(required) + (required == 1 ? " argument" : " arguments") + (variadic ? " or more" : required != f.params.size() ? " to " + std::to_string(f.params.size()) : "") + ", this call has " + std::to_string(c->args.size()));
        ir::St st; st.kind = ir::St::Kind::Native; st.nop = ir::NativeOp::Plugin; st.line = line; st.fn = it->second.first; st.cname = fname; st.raises = true;
        for (size_t i = 0; i < c->args.size(); ++i) {
            ir::ExP a = value(c->args[i].get(), out);
            const PluginParam& p = f.params[std::min(i, f.params.size() - 1)];
            // an any may be anything: the plugin checks it when it runs
            uint32_t bit = a->ty == ir::Ty::Any ? p.types : a->ty == ir::Ty::Int ? EMBR_T_INT : a->ty == ir::Ty::Float ? EMBR_T_FLOAT : a->ty == ir::Ty::Str ? EMBR_T_STRING
                         : ir::isArr(a->ty) ? EMBR_T_ARRAY : ir::isMap(a->ty) ? EMBR_T_MAP : 0;
            if (!bit) refuse(c->args[i]->range, "a " + name(a->ty) + " cannot be given to a plugin function");
            if (!(p.types & bit))
                refuse(c->args[i]->range, "argument " + std::to_string(i + 1) + " ('" + p.name + "') of " + fname + " is " + name(a->ty) + ", the plugin says it takes " + typeBits(p.types));
            st.args.push_back(std::move(a));
        }
        // what comes back: the plugin says it always returns one type, or it varies (and the result cannot be used)
        ir::Ty rty = ir::Ty::Void;
        const uint32_t r = f.resultTypes;
        if (r == EMBR_T_INT) rty = ir::Ty::Int;
        else if (r == EMBR_T_FLOAT) rty = ir::Ty::Float;
        else if (r == EMBR_T_STRING) rty = ir::Ty::Str;
        else if (r == EMBR_T_ARRAY && f.resultElem == EMBR_T_INT) rty = ir::Ty::ArrInt;
        else if (r == EMBR_T_ARRAY && f.resultElem == EMBR_T_FLOAT) rty = ir::Ty::ArrFloat;
        else if (r == EMBR_T_ARRAY && f.resultElem == EMBR_T_STRING) rty = ir::Ty::ArrStr;
        // a result of a type the plugin does not fix (or an array of what it does not say) is an any
        if (rty == ir::Ty::Void && (r & (EMBR_T_INT | EMBR_T_FLOAT | EMBR_T_STRING | EMBR_T_ARRAY | EMBR_T_MAP))) rty = ir::Ty::Any;
        if (rty == ir::Ty::Void) {
            noValueReason = fname + " gives a value of a type the plugin does not fix (" + typeBits(r) + "), which compiled code cannot use; a call that drops the result is fine";
            out.push_back(std::move(st));
            return nullptr;
        }
        const int t = newTemp(rty);
        st.hasDst = true; st.dst = {false, t};
        out.push_back(std::move(st));
        return load(t);
    }
    static std::string typeBits(uint32_t b) {
        if (b == EMBR_T_ANY) return "any type";
        std::string o;
        auto add = [&](uint32_t bit, const char* n) { if (b & bit) o += (o.empty() ? "" : " or ") + std::string(n); };
        add(EMBR_T_INT, "an int"); add(EMBR_T_FLOAT, "a float"); add(EMBR_T_STRING, "a str"); add(EMBR_T_ARRAY, "an array"); add(EMBR_T_MAP, "a map");
        add(EMBR_T_FUNCTION, "a function"); add(EMBR_T_POINTER, "a pointer");
        return o.empty() ? "nothing" : o;
    }

    // a call through a function value: the type says what it takes and gives
    ir::ExP callValue(const CallExpr* c, ir::ExP callee, std::vector<ir::St>& out) {
        const ir::FnSig sg = sigTable[(size_t)ir::sigOf(callee->ty)];
        if (c->args.size() != sg.params.size())
            refuse(c->range, "this function value takes " + std::to_string(sg.params.size()) + " arguments, this call has " + std::to_string(c->args.size()));
        ir::St s; s.kind = ir::St::Kind::CallValue; s.line = c->range.startLine; s.file = curFileIdx; s.a = std::move(callee);
        for (size_t i = 0; i < c->args.size(); ++i) {
            ir::ExP a = value(c->args[i].get(), out, sg.params[i]);
            if (a->ty != sg.params[i]) a = coerce(a, sg.params[i], c->args[i]->range, out);
            if (a->ty != sg.params[i]) learn(a, sg.params[i]);
            if (a->ty != sg.params[i])
                refuse(c->args[i]->range, "argument " + std::to_string(i + 1) + " is " + name(a->ty) + ", the function takes " + name(sg.params[i]));
            s.args.push_back(std::move(a));
        }
        if (sg.ret == ir::Ty::Void) { out.push_back(std::move(s)); return nullptr; }
        const int t = newTemp(sg.ret);
        s.hasDst = true; s.dst = {false, t};
        out.push_back(std::move(s));
        return load(t);
    }

    // a call of a `const fn`: the arguments say the types, and the version of the function for those types is written (later, see run). one version per
    // combination of types, called like any function. the C compiler is told to inline it
    ir::ExP templateCall(const std::string& fname, const FnStmt* f, const CallExpr* c, std::vector<ir::St>& out) {
        const int line = c->range.startLine;
        if (c->args.size() != f->params.size())
            refuse(c->range, fname + " takes " + std::to_string(f->params.size()) + " arguments, this call has " + std::to_string(c->args.size()));
        std::vector<ir::ExP> args;
        std::string iname = fname + "<";
        for (size_t i = 0; i < c->args.size(); ++i) {
            if (f->params[i].elem && f->params[i].elem->isHost) hostArgOk = c->args[i].get();
            ir::ExP a = value(c->args[i].get(), out);
            hostArgOk = nullptr;
            if (a->ty == ir::Ty::Void || a->ty == ir::Ty::Err) refuse(c->args[i]->range, "a " + name(a->ty) + " cannot be given to " + fname);
            if (f->params[i].explicitType) {
                const ir::Ty declared = writtenType(f->params[i].type, f->params[i].elem, c->args[i]->range, "the type of '" + f->params[i].name + "'");
                if (declared != a->ty) refuse(c->args[i]->range, "argument " + std::to_string(i + 1) + " of " + fname + " is " + name(a->ty) + ", the parameter is " + name(declared));
            }
            iname += (i ? "," : "") + std::string(tn(a->ty));
            args.push_back(std::move(a));
        }
        iname += curResultHost ? ">!host" : ">";
        auto it = instances.find(iname);
        if (it == instances.end()) {
            if (instances.size() >= 200) refuse(c->range, fname + " is called with too many different types (a version is written for each)");
            m.fns.emplace_back();
            ir::Fn& fn = m.fns.back();
            fn.id = (int)m.fns.size() - 1;
            Sig sg;
            sg.index = fn.id;
            for (const ir::ExP& a : args) { sg.params.push_back(a->ty); sg.paramKey.push_back(""); }
            sg.retKey = "r:" + iname;
            auto h = hints.find(sg.retKey);
            sg.ret = h == hints.end() ? ir::Ty::Void : h->second;
            sg.retHost = curResultHost;          // written out in a function that writes its result into the host, it may use $out too
            if (sg.retHost) sg.ret = ir::Ty::Void;
            it = instances.emplace(iname, std::move(sg)).first;
            pendingInst.push_back({f, iname});
        }
        const Sig& sg = it->second;
        if (sg.retHost && !curResultHost) refuse(c->range, "'" + fname + "' writes its result into the host, so it can only be called from a function that has `-> host` itself");
        ir::St s; s.kind = ir::St::Kind::Call; s.line = line; s.file = curFileIdx; s.fn = sg.index; s.args = std::move(args);
        if (sg.ret == ir::Ty::Void) {
            if (!sg.retKey.empty()) pendingRet = fname;      // a return in the body may still give it a type
            out.push_back(std::move(s));
            return nullptr;
        }
        const int t = newTemp(sg.ret);
        s.hasDst = true; s.dst = {false, t};
        out.push_back(std::move(s));
        return load(t);
    }

    ir::ExP call(const CallExpr* c, std::vector<ir::St>& out) {
        const int line = c->range.startLine;
        if (c->callee->kind != Expr::Kind::Var) {
            ir::ExP f = value(c->callee.get(), out);
            if (!ir::isFn(f->ty)) refuse(c->callee->range, "calling a " + name(f->ty));
            return callValue(c, f, out);
        }
        const std::string& fname = static_cast<const VarExpr*>(c->callee.get())->name;
        VarInfo v = lookup(fname, c->callee->range);
        if (v.found) {
            if (!ir::isFn(v.ty)) refuse(c->callee->range, "'" + fname + "' holds " + name(v.ty) + ", it cannot be called");
            return callValue(c, loadRef(v.ref, v.ty), out);
        }
        pendingRet.clear();
        auto tp = templates.find(fname);
        if (tp != templates.end()) return templateCall(fname, tp->second, c, out);
        auto it = sigs.find(fname);
        if (it == sigs.end()) {
            bool handled = false;
            if (isEmbrlibBuiltin(fname)) requireImport("embrlib", fname, c->range);
            ir::ExP r = builtin(fname, c, out, handled);
            if (handled) return r;
            r = tableNative(fname, c, out, handled);
            if (handled) return r;
            r = pluginCall(fname, c, out, handled);
            if (handled) return r;
            refuse(c->range, "the call to '" + fname + "' is not compiled (your own typed functions, print, and the natives of embrlib, str and embrmath that do not take a function are)");
        }
        const Sig& sg = it->second;
        if (sg.retHost && !curResultHost) refuse(c->range, "'" + fname + "' writes its result into the host, so it can only be called from a function that has `-> host` itself");
        const size_t fixed = sg.params.size() - (sg.variadic ? 1 : 0);
        const bool hasOpt = sg.nreq < fixed;
        if (sg.variadic ? c->args.size() < fixed : (c->args.size() < sg.nreq || c->args.size() > sg.params.size()))
            refuse(c->range, fname + " takes " + (sg.variadic || hasOpt ? (c->args.size() < sg.nreq ? "at least " : "at most ") : "") +
                   std::to_string(c->args.size() < sg.nreq || sg.variadic ? sg.nreq : fixed) + " arguments, this call has " + std::to_string(c->args.size()));
        ir::St s; s.kind = ir::St::Kind::Call; s.line = line; s.file = curFileIdx; s.fn = sg.index;
        bool learned = false;
        for (size_t i = 0; i < fixed; ++i) {
            if (i >= c->args.size()) {
                // left out: the default written in the parameter list
                s.args.push_back(defaultEx(sg.defaults[i], sg.params[i]));
                continue;
            }
            if (sg.byref[i]) {
                if (c->args[i]->kind != Expr::Kind::Ref) refuse(c->args[i]->range, "argument " + std::to_string(i + 1) + " of " + fname + " is in-out, write it as &name");
                const Expr* target = static_cast<const RefExpr*>(c->args[i].get())->target.get();
                if (target->kind != Expr::Kind::Var) refuse(target->range, "an in-out argument has to be a variable (&name)");
                const std::string& vn = static_cast<const VarExpr*>(target)->name;
                VarInfo vi = lookup(vn, target->range);
                if (!vi.found) refuse(target->range, "'" + vn + "' is not a variable that is defined here");
                if (vi.ty != sg.params[i]) refuse(target->range, "'" + vn + "' is " + name(vi.ty) + ", the in-out parameter '" + sg.paramName[i] + "' of " + fname + " is " + name(sg.params[i]));
                auto e = std::make_shared<ir::Ex>(); e->kind = ir::Ex::Kind::AddrOf; e->ty = vi.ty; e->ref = vi.ref;
                s.args.push_back(e);
                continue;
            }
            if (c->args[i]->kind == Expr::Kind::Ref) refuse(c->args[i]->range, "argument " + std::to_string(i + 1) + " of " + fname + " is not in-out, write it without the &");
            if (sg.params[i] == ir::Ty::Host) hostArgOk = c->args[i].get();
            ir::ExP a = value(c->args[i].get(), out, sg.params[i]);
            hostArgOk = nullptr;
            if (sg.builtin && sg.params[i] == ir::Ty::Float && a->ty == ir::Ty::Int) a = toFloat(a);
            if (sg.params[i] == ir::Ty::Void && !sg.paramKey[i].empty()) {
                // --infer: the first call tells the type of a parameter that is not written
                if (a->ty == ir::Ty::Void || a->ty == ir::Ty::Err) refuse(c->args[i]->range, "argument " + std::to_string(i + 1) + " of " + fname + " has no type a parameter can take");
                setHint(sg.paramKey[i], a->ty);
                learned = true;
                continue;
            }
            // a parameter that was worked out from another call and this call gives another type: the parameter is an any
            if (a->ty != sg.params[i] && !sg.paramKey[i].empty() && sg.params[i] != ir::Ty::Void && sg.params[i] != ir::Ty::Any && ir::canBeElem(a->ty)) {
                setHint(sg.paramKey[i], ir::Ty::Any);
                learned = true;
                continue;
            }
            if (a->ty != sg.params[i]) a = coerce(a, sg.params[i], c->args[i]->range, out, "argument '" + (i < sg.paramName.size() ? sg.paramName[i] : std::string("?")) + "' to '" + fname + "': expected %W but got %G");
            if (a->ty != sg.params[i]) learn(a, sg.params[i]);
            if (a->ty != sg.params[i])
                refuse(c->args[i]->range, "argument " + std::to_string(i + 1) + " of " + fname + " is " + name(a->ty) + ", the parameter is " + name(sg.params[i]) +
                       (sg.paramKey[i].empty() ? "" : " (worked out from another call, write the type to change it)"));
            s.args.push_back(std::move(a));
        }
        if (learned) throw Retry{};
        if (sg.variadic) {
            // the arguments that are left are put in an array
            const ir::Ty at = sg.params.back(), each = ir::elemOf(at);
            const int t = newTemp(at);
            out.push_back(native(ir::NativeOp::ArrNew, line, {}, t));
            for (size_t i = fixed; i < c->args.size(); ++i) {
                ir::ExP a = value(c->args[i].get(), out, each);
                if (a->ty != each) refuse(c->args[i]->range, "argument " + std::to_string(i + 1) + " of " + fname + " is " + name(a->ty) + ", the variadic parameter collects " + name(each));
                out.push_back(native(ir::NativeOp::ArrPush, line, {load(t), a}, t, true));
            }
            s.args.push_back(load(t));
        }
        if (sg.ret == ir::Ty::Void) {
            if (!sg.retKey.empty()) pendingRet = fname;   // a return in this round may still give it a type
            out.push_back(std::move(s));
            return nullptr;
        }
        const int t = newTemp(sg.ret);
        s.hasDst = true; s.dst = {false, t};
        out.push_back(std::move(s));
        return load(t);
    }

    // ---- function values
    // a function of the file used as a value: a closure that holds nothing
    ir::ExP functionAsValue(const std::string& fname, const SourceRange& at, std::vector<ir::St>& out) {
        const Sig& sg = sigs[fname];
        for (size_t i = 0; i < sg.params.size(); ++i) {
            if (sg.params[i] == ir::Ty::Host || sg.retHost) refuse(at, "'" + fname + "' has a host parameter or result, so it cannot be used as a value");
            if (sg.byref[i] || sg.defaults[i].kind != ParamDefault::None) refuse(at, "'" + fname + "' has an optional or in-out parameter, so it cannot be used as a value");
        }
        bool inferred = !sg.retKey.empty();
        for (const std::string& k : sg.paramKey) inferred = inferred || !k.empty();
        if (inferred) refuse(at, "'" + fname + "' has types aembr works out (--infer), so it cannot be used as a value: write its parameter and result types");
        ir::Fn& f = m.fns[(size_t)sg.index];
        f.asValue = true;
        f.sig = internSig(sg.params, sg.ret);
        const int t = newTemp(ir::fnTy(f.sig));
        ir::St s; s.kind = ir::St::Kind::MakeClosure; s.line = at.startLine; s.hasDst = true; s.dst = {false, t}; s.fn = sg.index;
        out.push_back(std::move(s));
        return load(t);
    }

    // write the body of a closure (a `fn(...)` value, or a function written inside another one) and make it
    ir::ExP closure(const std::string& fname, const std::vector<Param>& params, const std::vector<SourceRange>& paramRanges,
                    TypeSet retType, const TypeElem& retElem, bool retWritten, const std::vector<StmtPtr>& body, const SourceRange& at,
                    const std::string& selfNameIn, std::vector<ir::St>& out,
                    const std::string* assignedTo = nullptr, bool declare = false, ir::Ref* assigned = nullptr, const Expect* expect = nullptr) {
        for (const Param& p : params) {
            if (p.inout || (p.optional && !p.variadic)) refuse(at, "a function value cannot have optional or in-out parameters (" + p.name + ")");
            if (p.elem && p.elem->isHost) refuse(at, "a function value cannot have a host parameter (" + p.name + ")");
        }
        const bool savedResultHost = curResultHost;
        curResultHost = false;
        std::vector<ir::Ty> ptys;
        for (size_t i = 0; i < params.size(); ++i) {
            const SourceRange pr = i < paramRanges.size() ? paramRanges[i] : at;
            if (params[i].optional || params[i].variadic) refuse(pr, "optional and variadic parameters are not compiled yet");
            if (!params[i].explicitType) {
                if (!expect || expect->params.size() != params.size())
                    refuse(pr, "parameter '" + params[i].name + "' needs a written type (int, float, str or arr<...>)");
                ptys.push_back(expect->params[i]);      // a function handed to filter, reduce and the like: the call says what it gets
                continue;
            }
            ptys.push_back(writtenType(params[i].type, params[i].elem, pr, "the type of '" + params[i].name + "'"));
        }
        const std::string key = fname + "@" + std::to_string(at.startLine) + ":" + std::to_string(at.startCol);
        // --infer: a result that is not written is worked out from the first `return value`, as for a top level function
        const bool unwritten = !retWritten;
        const std::string retHintKey = unwritten && (opts.infer || expect) ? "lr:" + key : "";
        ir::Ty ret = unwritten ? ir::Ty::Void : writtenType(retType, retElem, at, "the return type");
        if (unwritten && expect && expect->ret != ir::Ty::Void) ret = expect->ret;
        if (!retHintKey.empty()) { auto h = hints.find(retHintKey); if (h != hints.end()) ret = h->second; }
        const int sig = internSig(ptys, ret);
        // `f = fn() ... f() ... end`: the closure can use the variable it is stored in, so that exists before the body is written
        if (assignedTo) *assigned = variable(*assignedTo, ir::fnTy(sig), at, declare);

        m.fns.emplace_back();
        ir::Fn& fn = m.fns.back();
        fn.id = (int)m.fns.size() - 1;
        fn.name = fname;
        fn.line = at.startLine;
        fn.ret = ret;
        fn.isClosure = true;
        fn.sig = sig;
        const int fnIndex = fn.id;

        std::string savedRetKey = std::move(curRetKey);
        const bool savedRetInferred = curRetInferred;
        std::string savedRetHintKey = std::move(curRetHintKey);
        curRetHintKey = retHintKey;
        curRetKey = ret == ir::Ty::Void ? retHintKey : std::string();
        curRetInferred = !retHintKey.empty() && ret != ir::Ty::Void;
        parents.push_back({cur, std::move(locals), curRet, inFn, loopDepth, std::move(selfName), selfSig, std::move(capIdx), curKey});
        cur = &fn; locals = {}; curRet = ret; inFn = true; loopDepth = 0; selfName = selfNameIn; selfSig = sig; capIdx = {}; curKey = key;
        for (size_t i = 0; i < params.size(); ++i) {
            if (locals.count(params[i].name)) refuse(at, "two parameters are called '" + params[i].name + "'");
            fn.vars.push_back({params[i].name, ptys[i], false});
            locals[params[i].name] = (int)i;
        }
        fn.nparams = (int)params.size();
        block(body, fn.body);
        if (ret != ir::Ty::Void && !alwaysReturns(body)) refuse(at, "this function can reach its end without a return");
        curResultHost = savedResultHost;
        Saved s = std::move(parents.back());
        parents.pop_back();
        curRetKey = std::move(savedRetKey); curRetInferred = savedRetInferred; curRetHintKey = std::move(savedRetHintKey);
        cur = s.fn; locals = std::move(s.locals); curRet = s.ret; inFn = s.inFn; loopDepth = s.loopDepth;
        selfName = std::move(s.selfName); selfSig = s.selfSig; capIdx = std::move(s.capIdx); curKey = s.key;

        const int t = newTemp(ir::fnTy(sig));
        ir::St mk; mk.kind = ir::St::Kind::MakeClosure; mk.line = at.startLine; mk.hasDst = true; mk.dst = {false, t}; mk.fn = fnIndex;
        out.push_back(std::move(mk));
        return load(t);
    }

    ir::ExP lambda(const LambdaExpr* l, std::vector<ir::St>& out, const std::string* assignedTo = nullptr, bool declare = false, ir::Ref* assigned = nullptr,
                   const Expect* expect = nullptr) {
        return closure("<lambda>", l->params, l->paramRanges, l->returnType, l->returnElem, l->returnWritten, l->ownedBody, l->range, "", out, assignedTo, declare, assigned, expect);
    }

    // ---- statements
    // the variable `name` is assigned a value of type `ty`. a name that is not there yet becomes a local
    ir::Ref variable(const std::string& vname, ir::Ty ty, const SourceRange& at, bool declare) {
        if (ty == ir::Ty::Host) refuse(at, "a host value is only good during the call: it cannot be kept in the variable '" + vname + "' (pass it on to a host parameter, or use it in C text)");
        if (declare) {
            if (locals.count(vname) || (isGlobalName(vname) && atTop() && globalIdx.count(vname))) refuse(at, "'" + vname + "' is declared twice");
        } else {
            VarInfo v = lookup(vname, at);
            if (v.found) {
                if (v.ref.self) refuse(at, "'" + vname + "' is the function itself, it cannot be assigned");
                if (v.ref.global && plugin && !atTop()) refuse(at, "'" + vname + "' is a top level name of a plugin, which its functions only read");
                if (v.ty != ty) refuse(at, "'" + vname + "' holds " + name(v.ty) + " here, this gives " + name(ty) + " (a name keeps the type of its first value)");
                return v.ref;
            }
        }
        if (inFn && topName(vname)) {
            if (plugin) refuse(at, "'" + vname + "' is a top level name of a plugin, which its functions only read");
            useGlobal(vname, at, "assigns");
        }
        if (sigs.count(vname)) refuse(at, "'" + vname + "' is already a function");
        if (isGlobalName(vname) && atTop()) {
            if (ir::isFn(ty) || ty == ir::Ty::Err) refuse(at, std::string(plugin ? "a plugin" : "a function reads this name, so it is a global, which") + " cannot keep " + name(ty) + " in the top level name '" + vname + "'" + (plugin ? " (state that persists belongs behind a handle)" : ""));
            m.globals.push_back({vname, ty, false});
            globalIdx[vname] = (int)m.globals.size() - 1;
            ir::Ref r; r.global = true; r.index = (int)m.globals.size() - 1;
            return r;
        }
        cur->vars.push_back({vname, ty, false});
        const int idx = (int)cur->vars.size() - 1;
        locals[vname] = idx;
        return {false, idx};
    }

    // a variable that lives for a block only: the loop variable of a for, the error of a catch
    int loopVar(const std::string& vname, ir::Ty ty, const SourceRange& at, const char* what = "loop variable") {
        if (locals.count(vname)) refuse(at, std::string("the ") + what + " '" + vname + "' is already a name in this function");
        if (inFn && topName(vname)) refuse(at, std::string("the ") + what + " '" + vname + "' is a top level name");
        cur->vars.push_back({vname, ty, false});
        locals[vname] = (int)cur->vars.size() - 1;
        return locals[vname];
    }

    static ir::St newCell(const ir::Ref& r, int line) {      // (the emitter does nothing for a variable no closure uses, and a global is never in a cell)
        ir::St s; s.kind = ir::St::Kind::NewCell; s.line = line; s.hasDst = true; s.dst = r; return s;
    }

    // `name = value`. when the value was just made into a temp by the last statement, that statement writes the variable
    // itself: otherwise the temp and the variable would both hold the array and the first write to it would copy it.
    // `fresh`: the variable is made new here (a `local`), which matters when a closure captured the one from the last pass
    void store(const ir::Ref& dst, ir::ExP v, int line, std::vector<ir::St>& out, bool fresh = false) {
        if (ir::isHeap(v->ty) && v->kind == ir::Ex::Kind::Load && !v->ref.global && !v->ref.captured && !v->ref.self &&
            cur->vars[(size_t)v->ref.index].temp && !out.empty()) {
            ir::St& last = out.back();
            if ((last.kind == ir::St::Kind::Native || last.kind == ir::St::Kind::Call || last.kind == ir::St::Kind::CallValue ||
                 last.kind == ir::St::Kind::MakeClosure) &&
                last.hasDst && !last.inplace && !last.dst.global && !last.dst.captured && last.dst.index == v->ref.index) {
                if (fresh && !dst.global) { ir::St c = newCell(dst, line); out.insert(out.end() - 1, std::move(c)); }
                out.back().dst = dst;
                return;
            }
        }
        if (fresh && !dst.global) out.push_back(newCell(dst, line));
        out.push_back(set(dst, std::move(v), line));
    }

    const VarExpr* asVar(const Expr* e) { return e->kind == Expr::Kind::Var ? static_cast<const VarExpr*>(e) : nullptr; }

    void block(const std::vector<StmtPtr>& stmts, std::vector<ir::St>& out) {
        for (const StmtPtr& s : stmts) stmt(s.get(), out);
    }

    // `a = push(a, x)` and `s = s + x` grow what the variable holds, in place when nobody else holds it
    bool inplaceUpdate(const AssignStmt* a, const VarExpr* target, std::vector<ir::St>& out) {
        ir::Ref ref; ir::Ty vt;
        if (!containerVar(target, out, ref, vt)) return false;
        const int line = a->range.startLine;
        if (a->value->kind == Expr::Kind::Call && (ir::isArr(vt) || vt == ir::Ty::Any)) {
            auto* c = static_cast<const CallExpr*>(a->value.get());
            const VarExpr* callee = asVar(c->callee.get());
            if (callee && callee->name == "push" && !sigs.count("push") && !lookup("push", c->range).found && c->args.size() == 2) {
                const VarExpr* first = asVar(c->args[0].get());
                if (first && first->name == target->name && vt == ir::Ty::Any) {
                    ir::ExP v = value(c->args[1].get(), out);
                    out.push_back(native(ir::NativeOp::AnyPush, line, {loadRef(ref, vt), toAny(v, c->args[1]->range, out)}, ref, true));
                    return true;
                }
                if (first && first->name == target->name) {
                    ir::ExP x = value(c->args[1].get(), out, ir::elemOf(vt));
                    x = elementFor(x, loadRef(ref, vt), c->args[1]->range, out);
                    if (x->ty != ir::elemOf(vt)) { if (scalarElem(x->ty)) learn(loadRef(ref, vt), ir::arrOf(x->ty)); refuse(c->args[1]->range, "this is " + name(x->ty) + ", the array holds " + name(ir::elemOf(vt))); }
                    out.push_back(native(ir::NativeOp::ArrPush, line, {loadRef(ref, vt), x}, ref, true));
                    return true;
                }
            }
        }
        if (a->value->kind == Expr::Kind::Binary && vt == ir::Ty::Str) {
            auto* b = static_cast<const BinaryExpr*>(a->value.get());
            const VarExpr* first = asVar(b->left.get());
            if (b->op == "+" && first && first->name == target->name) {
                ir::ExP x = value(b->right.get(), out);
                if (x->ty != ir::Ty::Str) refuse(b->right->range, "a str + " + name(x->ty));
                // s = s + slice(t, a, b): the slice was just made into a temporary nobody else uses, so append from t without making it
                if (!out.empty() && x->kind == ir::Ex::Kind::Load && !x->ref.global && !x->ref.captured && !x->ref.self) {
                    const ir::St& last = out.back();
                    if (last.kind == ir::St::Kind::Native && last.nop == ir::NativeOp::Rt && last.hasDst && !last.dst.global && !last.dst.captured &&
                        last.dst.index == x->ref.index && (last.cname == "rt_slice_str" || last.cname == "rt_slice_str_from")) {
                        std::vector<ir::ExP> sa = last.args;
                        if (last.cname == "rt_slice_str_from") sa.push_back(cInt(2147483647));
                        out.pop_back();
                        sa.insert(sa.begin(), loadRef(ref, vt));
                        out.push_back(native(ir::NativeOp::StrAppendSlice, line, std::move(sa), ref, true));
                        return true;
                    }
                }
                out.push_back(native(ir::NativeOp::StrAppend, line, {loadRef(ref, vt), x}, ref, true));
                return true;
            }
        }
        return false;
    }

    void forLoop(const ForStmt* f, std::vector<ir::St>& out) {
        const int line = f->range.startLine;
        if (!f->keyName.empty()) refuse(f->range, "the two variable for is not compiled yet");
        ir::St loop; loop.kind = ir::St::Kind::Loop; loop.line = line;
        auto brk = [&](ir::ExP cond) {
            ir::St iff; iff.kind = ir::St::Kind::If; iff.line = line; iff.a = std::move(cond);
            ir::St b; b.kind = ir::St::Kind::Break; b.line = line;
            iff.then.push_back(std::move(b));
            loop.body.push_back(std::move(iff));
        };
        const Expr* it = f->iterable.get();
        if (it->kind == Expr::Kind::Array && static_cast<const ArrayExpr*>(it)->isRange) {
            auto* r = static_cast<const ArrayExpr*>(it);
            if (r->elements.size() == 3) {
                // [start, second ... end]: the step is second - start. the loop counts how many numbers there are, then walks them
                ir::ExP lo = value(r->elements[0].get(), out), sec = value(r->elements[1].get(), out), hi = value(r->elements[2].get(), out);
                if (lo->ty != ir::Ty::Int || sec->ty != ir::Ty::Int || hi->ty != ir::Ty::Int) refuse(it->range, "a range of floats is not compiled: the numbers of a range have to be ints");
                const int start = newTemp(ir::Ty::Int), step = newTemp(ir::Ty::Int), count = newTemp(ir::Ty::Int), idx = newTemp(ir::Ty::Int);
                out.push_back(set(start, lo, line));
                { ir::St st; st.kind = ir::St::Kind::Native; st.nop = ir::NativeOp::Rt; st.line = line; st.cname = "rt_range_step"; st.raises = true;
                  st.args = {load(start), sec}; st.hasDst = true; st.dst = {false, step}; out.push_back(std::move(st)); }
                { ir::St st; st.kind = ir::St::Kind::Native; st.nop = ir::NativeOp::Rt; st.line = line; st.cname = "rt_range_count";
                  st.args = {load(start), load(step), hi}; st.hasDst = true; st.dst = {false, count}; out.push_back(std::move(st)); }
                out.push_back(set(idx, cInt(0), line));
                const int v = loopVar(f->varName, ir::Ty::Int, f->varRange);
                brk(bin(ir::BinOp::Ge, ir::Ty::Int, load(idx), load(count)));
                loop.body.push_back(newCell({false, v}, line));
                loop.body.push_back(set(v, bin(ir::BinOp::Add, ir::Ty::Int, load(start), bin(ir::BinOp::Mul, ir::Ty::Int, load(idx), load(step))), line));
                loop.body.push_back(set(idx, bin(ir::BinOp::Add, ir::Ty::Int, load(idx), cInt(1)), line));
                ++loopDepth;
                block(f->body, loop.body);
                --loopDepth;
                out.push_back(std::move(loop));
                locals.erase(f->varName);
                return;
            }
            ir::ExP lo = value(r->elements[0].get(), out), hi = value(r->elements[1].get(), out);
            if (lo->ty != ir::Ty::Int || hi->ty != ir::Ty::Int) refuse(it->range, "a range of floats is not compiled: the numbers of a range have to be ints");
            const int cur_ = newTemp(ir::Ty::Int), end = newTemp(ir::Ty::Int), done = newTemp(ir::Ty::Int);
            out.push_back(set(cur_, lo, line));
            out.push_back(set(end, hi, line));
            out.push_back(set(done, cInt(0), line));
            const int v = loopVar(f->varName, ir::Ty::Int, f->varRange);
            brk(bin(ir::BinOp::OrElse, ir::Ty::Int, load(done), bin(ir::BinOp::Gt, ir::Ty::Int, load(cur_), load(end))));
            loop.body.push_back(newCell({false, v}, line));
            loop.body.push_back(set(v, load(cur_), line));
            // the last value must not be stepped past: end can be the largest int
            ir::St last; last.kind = ir::St::Kind::If; last.line = line; last.a = bin(ir::BinOp::Eq, ir::Ty::Int, load(cur_), load(end));
            last.then.push_back(set(done, cInt(1), line));
            last.els.push_back(set(cur_, bin(ir::BinOp::Add, ir::Ty::Int, load(cur_), cInt(1)), line));
            loop.body.push_back(std::move(last));
        } else {
            ir::ExP a = value(it, out);
            if (ir::isMap(a->ty)) refuse(it->range, "a for loop over a map is not compiled: the VM visits the keys in an order nobody defines (use for k in keys(m))");
            if (a->ty == ir::Ty::Any || a->ty == ir::Ty::Str) {
                // an any (an array or a str when it runs) or a str: one pass for each element or character, as it was when the loop started
                const bool isAny = a->ty == ir::Ty::Any;
                const int snap = newTemp(a->ty), idx = newTemp(ir::Ty::Int), n = newTemp(ir::Ty::Int);
                out.push_back(set(snap, a, line));
                if (isAny) out.push_back(set(n, rtCall("rt_any_iter_len", {load(snap)}, ir::Ty::Int, true, line, out), line));
                else out.push_back(set(n, lenOf(load(snap)), line));
                out.push_back(set(idx, cInt(0), line));
                const int v = loopVar(f->varName, a->ty, f->varRange);
                brk(bin(ir::BinOp::Ge, ir::Ty::Int, load(idx), load(n)));
                loop.body.push_back(newCell({false, v}, line));
                if (isAny) { ir::ExP el = rtCall("rt_any_at", {load(snap), load(idx)}, ir::Ty::Any, true, line, loop.body); loop.body.push_back(set(v, el, line)); }
                else loop.body.push_back(native(ir::NativeOp::StrIndex, line, {load(snap), load(idx)}, v));
                loop.body.push_back(set(idx, bin(ir::BinOp::Add, ir::Ty::Int, load(idx), cInt(1)), line));
            } else {
            if (!ir::isArr(a->ty)) refuse(it->range, "a for loop over a " + name(a->ty) + " is not compiled yet");
            // the loop goes over the array as it was when it started
            const int snap = newTemp(a->ty), idx = newTemp(ir::Ty::Int);
            out.push_back(set(snap, a, line));
            out.push_back(set(idx, cInt(0), line));
            const int v = loopVar(f->varName, ir::elemOf(a->ty), f->varRange);
            brk(bin(ir::BinOp::Ge, ir::Ty::Int, load(idx), lenOf(load(snap))));
            loop.body.push_back(newCell({false, v}, line));
            loop.body.push_back(native(ir::NativeOp::ArrGet, line, {load(snap), load(idx)}, v));
            loop.body.push_back(set(idx, bin(ir::BinOp::Add, ir::Ty::Int, load(idx), cInt(1)), line));
            }
        }
        ++loopDepth;
        block(f->body, loop.body);
        --loopDepth;
        out.push_back(std::move(loop));
        locals.erase(f->varName);       // the loop variable is not there after the loop, as in the VM
    }

    void stmt(const Stmt* s, std::vector<ir::St>& out) {
        const int line = s->range.startLine;
        switch (s->kind) {
        case Stmt::Kind::Assign: {
            auto* a = static_cast<const AssignStmt*>(s);
            // a const is an ordinary variable here: the type checker has already refused a second assignment
            if (a->target->kind == Expr::Kind::Index) {
                auto* ix = static_cast<const IndexExpr*>(a->target.get());
                ir::Ref ref; ir::Ty at;
                if (!containerVar(ix->object.get(), out, ref, at)) refuse(ix->object->range, "assigning to an element needs a variable that holds an array or a map");
                if (at == ir::Ty::Any) {
                    ir::ExP k = value(ix->index.get(), out);
                    ir::ExP v = value(a->value.get(), out);
                    out.push_back(native(ir::NativeOp::AnySet, line, {loadRef(ref, at), toAny(k, ix->index->range, out), toAny(v, a->value->range, out)}, ref, true));
                    return;
                }
                if (ir::isMap(at)) {
                    ir::ExP k = value(ix->index.get(), out);
                    if (k->ty == ir::Ty::Any) k = fromAny(k, ir::Ty::Str, ix->index->range, out);
                    if (k->ty != ir::Ty::Str) refuse(ix->index->range, "a map key has to be a str");
                    ir::ExP x = value(a->value.get(), out, ir::elemOf(at));
                    x = elementFor(x, loadRef(ref, at), a->value->range, out);
                    if (x->ty != ir::elemOf(at)) { if (scalarElem(x->ty)) learn(loadRef(ref, at), ir::mapOf(x->ty)); refuse(a->value->range, "this is " + name(x->ty) + ", the map holds " + name(ir::elemOf(at))); }
                    out.push_back(native(ir::NativeOp::MapSet, line, {loadRef(ref, at), k, x}, ref, true));
                    return;
                }
                if (!ir::isArr(at)) refuse(ix->object->range, "assigning to an element of a " + name(at) + " is not compiled");
                ir::ExP i = value(ix->index.get(), out);
                if (i->ty == ir::Ty::Any) i = fromAny(i, ir::Ty::Int, ix->index->range, out);
                if (i->ty != ir::Ty::Int) refuse(ix->index->range, "an index has to be an int");
                ir::ExP x = value(a->value.get(), out, ir::elemOf(at));
                x = elementFor(x, loadRef(ref, at), a->value->range, out);
                if (x->ty != ir::elemOf(at)) { if (scalarElem(x->ty)) learn(loadRef(ref, at), ir::arrOf(x->ty)); refuse(a->value->range, "this is " + name(x->ty) + ", the array holds " + name(ir::elemOf(at))); }
                out.push_back(native(ir::NativeOp::ArrSet, line, {loadRef(ref, at), i, x}, ref, true));
                return;
            }
            if (a->target->kind != Expr::Kind::Var) refuse(a->target->range, "this kind of assignment is not compiled yet");
            const VarExpr* tv = static_cast<const VarExpr*>(a->target.get());
            if (inplaceUpdate(a, tv, out)) return;
            if (a->value->kind == Expr::Kind::Lambda) {
                ir::Ref dst;
                ir::ExP c = lambda(static_cast<const LambdaExpr*>(a->value.get()), out, &tv->name, false, &dst);
                store(dst, c, line, out);
                return;
            }
            ir::Ty want = ir::Ty::Void;
            VarInfo ex = lookup(tv->name, a->target->range);
            if (ex.found && !ex.ref.self) want = ex.ty;
            else if (!ex.found && isEmptyLiteral(a->value.get())) want = guessFor(tv->name, a->value.get());
            ir::ExP v = value(a->value.get(), out, want);
            if (ex.found && !ex.ref.self && ex.ty != v->ty) v = coerce(v, ex.ty, a->value->range, out, "cannot assign %G to '" + tv->name + "' declared as %W");
            if (ex.found && !ex.ref.self && ex.ty != v->ty) learn(loadRef(ex.ref, ex.ty), v->ty);
            const ir::Ref dst = variable(tv->name, v->ty, a->target->range, false);
            store(dst, v, line, out);
            return;
        }
        case Stmt::Kind::Local: {
            auto* l = static_cast<const LocalStmt*>(s);
            if (l->targets.size() != 1 || l->bracketed) {
                std::vector<std::string> names; std::vector<SourceRange> ranges;
                for (const auto& t : l->targets) { names.push_back(t.name); ranges.push_back(t.range); }
                unpack(names, ranges, &l->targets, l->value.get(), true, line, out);
                return;
            }
            const auto& t = l->targets[0];
            ir::Ty declared = ir::Ty::Void;
            if (t.explicitType) declared = writtenType(t.type, t.elem, t.range, "the type of '" + t.name + "'");
            if (!t.explicitType && l->value->kind == Expr::Kind::Lambda) {
                ir::Ref dst;
                ir::ExP c = lambda(static_cast<const LambdaExpr*>(l->value.get()), out, &t.name, true, &dst);
                store(dst, c, line, out, true);
                return;
            }
            if (!t.explicitType && isEmptyLiteral(l->value.get())) declared = guessFor(t.name, l->value.get());
            ir::ExP v = value(l->value.get(), out, declared);
            if (t.explicitType) v = coerce(v, declared, l->value->range, out, "cannot assign %G to '" + t.name + "' declared as %W");
            if (t.explicitType && declared != v->ty)
                refuse(l->value->range, "'" + t.name + "' is declared " + name(declared) + " but the value is " + name(v->ty));
            const ir::Ref dst = variable(t.name, v->ty, t.range, true);
            store(dst, v, line, out, true);
            return;
        }
        case Stmt::Kind::If: {
            auto* i = static_cast<const IfStmt*>(s);
            ir::ExP c = truth(value(i->cond.get(), out), i->cond->range, &out);
            ir::St st; st.kind = ir::St::Kind::If; st.line = line; st.a = c;
            block(i->thenBlock, st.then);
            block(i->elseBlock, st.els);
            out.push_back(std::move(st));
            return;
        }
        case Stmt::Kind::While: {
            auto* w = static_cast<const WhileStmt*>(s);
            ir::St loop; loop.kind = ir::St::Kind::Loop; loop.line = line;
            // the test runs at the top of every pass, so what it needs goes inside the loop
            ir::ExP c = truth(value(w->cond.get(), loop.body), w->cond->range, &loop.body);
            ir::St exit; exit.kind = ir::St::Kind::If; exit.line = line; exit.a = un(ir::UnOp::Not, ir::Ty::Int, c);
            ir::St brk; brk.kind = ir::St::Kind::Break; brk.line = line;
            exit.then.push_back(std::move(brk));
            loop.body.push_back(std::move(exit));
            ++loopDepth;
            block(w->body, loop.body);
            --loopDepth;
            out.push_back(std::move(loop));
            return;
        }
        case Stmt::Kind::For: forLoop(static_cast<const ForStmt*>(s), out); return;
        case Stmt::Kind::Break: case Stmt::Kind::Continue: {
            if (!loopDepth) refuse(s->range, s->kind == Stmt::Kind::Break ? "break outside a loop" : "continue outside a loop");
            ir::St st; st.kind = s->kind == Stmt::Kind::Break ? ir::St::Kind::Break : ir::St::Kind::Continue; st.line = line;
            out.push_back(std::move(st));
            return;
        }
        case Stmt::Kind::Return: {
            auto* r = static_cast<const ReturnStmt*>(s);
            if (!inFn) refuse(s->range, "return outside a function");
            ir::St st; st.kind = ir::St::Kind::Return; st.line = line;
            if (r->value) {
                if (curRet == ir::Ty::Void && !curRetKey.empty()) {
                    // --infer: the first return that gives a value tells the result type
                    ir::ExP v0;
                    try {
                        v0 = value(r->value.get(), out);
                    } catch (const Retry&) {
                        // `return f(...)` of a function whose result is not known yet (itself, say): another return may tell it
                        retSkipped = true;
                        return;
                    }
                    if (v0->ty == ir::Ty::Void || v0->ty == ir::Ty::Err) refuse(r->value->range, "a function cannot return " + name(v0->ty));
                    setHint(curRetKey, v0->ty);
                    throw Retry{};
                }
                if (curRet == ir::Ty::Void) refuse(s->range, "this function has no return type, so it cannot return a value (write -> int, or -> fn(int) -> int for a function)");
                ir::ExP v = value(r->value.get(), out, curRet);
                // a result that was worked out and a return that gives another type: the result is an any
                if (v->ty != curRet && curRetInferred && curRet != ir::Ty::Any && !curRetHintKey.empty() && ir::canBeElem(v->ty)) {
                    setHint(curRetHintKey, ir::Ty::Any);
                    throw Retry{};
                }
                if (v->ty != curRet) v = coerce(v, curRet, r->value->range, out, "'" + curFnName() + "' declared return type %W but returned %G");
                if (v->ty != curRet) learn(v, curRet);
                if (v->ty != curRet) refuse(r->value->range, "returns " + name(v->ty) + ", the function says " + name(curRet) + (curRetInferred ? " (worked out from another return, write the type to change it)" : ""));
                st.a = v;
            } else if (curRet != ir::Ty::Void) {
                refuse(s->range, "this function returns a value, a bare return is not allowed");
            }
            out.push_back(std::move(st));
            return;
        }
        case Stmt::Kind::Expr: {
            auto* x = static_cast<const ExprStmt*>(s);
            if (x->inlineC) { inlineC(x, out); return; }
            if (x->expr->kind != Expr::Kind::Call) refuse(s->range, "an expression that is not a call has no effect");
            expr(x->expr.get(), out, ir::Ty::Void);
            return;
        }
        case Stmt::Kind::Import: {
            auto* im = static_cast<const ImportStmt*>(s);
            // embrlib is built in, nothing to load
            if (im->pathExpr->kind == Expr::Kind::String) {
                const std::string& p = static_cast<const StringExpr*>(im->pathExpr.get())->v;
                if (p == "embrlib" || p == "str" || p == "embrmath") return;      // built in, nothing to load
            }
            // a C plugin was read in signatures(); anything else cannot be called from compiled code
            if (im->pathExpr->kind == Expr::Kind::String) {
                const std::string& p = static_cast<const StringExpr*>(im->pathExpr.get())->v;
                for (const PluginDesc& d : descs) if (d.name == p) return;
            }
            refuse(s->range, "import is not compiled: only embrlib, str, embrmath and C plugins can be imported");
        }
        case Stmt::Kind::Fn: {
            // a function written inside another one: a closure held by a local of the same name, which it can call itself by
            if (!inFn) refuse(s->range, "a function inside a block is not compiled yet");
            auto* f = static_cast<const FnStmt*>(s);
            ir::ExP c = closure(f->name, f->params, f->paramRanges, f->returnType, f->returnElem, f->returnWritten, f->body, f->range, f->name, out);
            const ir::Ref dst = variable(f->name, c->ty, f->nameRange, true);
            store(dst, c, line, out, true);
            return;
        }
        case Stmt::Kind::MultiAssign: {
            auto* m_ = static_cast<const MultiAssignStmt*>(s);
            unpack(m_->names, m_->nameRanges, nullptr, m_->value.get(), false, line, out);
            return;
        }
        case Stmt::Kind::Try: {
            auto* t = static_cast<const TryStmt*>(s);
            ir::St st; st.kind = ir::St::Kind::Try; st.line = line;
            block(t->tryBlock, st.body);
            // e exists only inside the catch block
            const int e = loopVar(t->catchVar, ir::Ty::Err, t->catchRange, "error variable");
            st.hasDst = true; st.dst = {false, e};
            block(t->catchBlock, st.els);
            locals.erase(t->catchVar);
            out.push_back(std::move(st));
            return;
        }
        }
    }

    // a function with a return type has to end in a return on every path
    static bool alwaysReturns(const std::vector<StmtPtr>& b) {
        if (b.empty()) return false;
        const Stmt* s = b.back().get();
        if (s->kind == Stmt::Kind::Return) return true;
        if (s->kind == Stmt::Kind::If) {
            auto* i = static_cast<const IfStmt*>(s);
            return alwaysReturns(i->thenBlock) && alwaysReturns(i->elseBlock);
        }
        if (s->kind == Stmt::Kind::Try) {
            auto* t = static_cast<const TryStmt*>(s);
            return alwaysReturns(t->tryBlock) && alwaysReturns(t->catchBlock);
        }
        return false;
    }

    // ---- the program
    // a C plugin the file imports: its description is read now, so its functions can be checked and typed
    void loadPlugin(const std::string& pname, const SourceRange& at) {
        PluginDesc d;
        std::string err;
        std::string dir;
        if (!fr.path.empty()) { std::error_code ec; auto p = std::filesystem::absolute(fr.path, ec); dir = p.parent_path().string(); }
        if (!readPlugin(pname, dir, d, err)) refuse(at, err);
        if (d.caps & opts.denyCaps) {
            std::string names;
            for (const std::string& n : embr::capNames(d.caps & opts.denyCaps)) names += (names.empty() ? "" : ", ") + n;
            refuse(at, "the plugin \"" + pname + "\" needs " + names + ", which --deny-caps refuses");
        }
        descs.push_back(std::move(d));
        m.plugins.push_back({descs.back().name, descs.back().path});
    }

    void signatures() {
        for (const StmtPtr& s : fr.ast) {
            if (s->kind == Stmt::Kind::Import) {
                auto* im = static_cast<const ImportStmt*>(s.get());
                if (im->pathExpr->kind != Expr::Kind::String) continue;
                const std::string& pname = static_cast<const StringExpr*>(im->pathExpr.get())->v;
                if (!imported.insert(pname).second || pname == "embrlib" || pname == "str" || pname == "embrmath") continue;
                loadPlugin(pname, s->range);
            }
        }
        for (const StmtPtr& s : fr.ast) {
            if (s->kind == Stmt::Kind::Fn) {
                auto* f = static_cast<const FnStmt*>(s.get());
                { const int fi = fileOf(s.get()); gRefuseFile = fi > 0 ? m.files[(size_t)fi] : std::string(); }
                if (sigs.count(f->name) || templates.count(f->name)) refuse(f->nameRange, "'" + f->name + "' is defined twice");
                if (f->isTemplate) {
                    for (const Param& p : f->params)
                        if (p.inout || (p.optional && !p.variadic)) refuse(f->nameRange, "a const fn cannot have optional or in-out parameters yet");
                    templates[f->name] = f; continue;
                }
                Sig sg;
                sg.builtin = fr.stdStmts.count(s.get()) > 0;
                sg.index = (int)sigs.size();
                for (size_t i = 0; i < f->params.size(); ++i) {
                    const Param& p = f->params[i];
                    const SourceRange at = i < f->paramRanges.size() ? f->paramRanges[i] : f->range;
                    sg.paramName.push_back(p.name);
                    sg.defaults.push_back(p.def);
                    sg.byref.push_back(p.inout ? 1 : 0);
                    if (p.optional && !p.variadic) {
                        if (!p.explicitType) refuse(at, "the optional parameter '" + p.name + "' needs a written type");
                        const ir::Ty dt = writtenType(p.type, p.elem, at, "the type of '" + p.name + "'");
                        const bool ok = (p.def.kind == ParamDefault::Int && (dt == ir::Ty::Int || dt == ir::Ty::Float)) ||
                                        (p.def.kind == ParamDefault::Float && dt == ir::Ty::Float) || (p.def.kind == ParamDefault::Str && dt == ir::Ty::Str);
                        if (!ok) refuse(at, "the default value of '" + p.name + "' does not fit its type (" + name(dt) + ")");
                    }
                    if (p.inout && p.elem && p.elem->isHost) refuse(at, "a host parameter cannot also be in-out (" + p.name + ")");
                    if (p.inout && !p.explicitType) refuse(at, "the in-out parameter '" + p.name + "' needs a written type");
                    if (p.variadic) {
                        // fn f(a, ...rest: int): rest is an arr<int> inside, and the call gives the arguments that are left
                        if (i + 1 != f->params.size()) refuse(at, "a variadic parameter has to be the last one");
                        if (!p.explicitType) refuse(at, "the variadic parameter '" + p.name + "' needs the type of each argument written (...rest: int)");
                        const ir::Ty each = scalarType(p.type);
                        if (each == ir::Ty::Void) refuse(at, "the variadic parameter '" + p.name + "' can collect ints, floats or strs");
                        sg.params.push_back(ir::arrOf(each));
                        sg.paramKey.push_back("");
                        sg.variadic = true;
                        continue;
                    }
                    if (!p.explicitType) {
                        if (!opts.infer) refuse(at, "parameter '" + p.name + "' needs a written type (int, float, str, arr<...>, map<...> or fn(...) -> ...), or build with --infer to let aembr work it out");
                        // the type comes from the first call that is written, see call()
                        const std::string key = "p:" + f->name + ":" + std::to_string(i);
                        auto h = hints.find(key);
                        sg.params.push_back(h == hints.end() ? ir::Ty::Void : h->second);
                        sg.paramKey.push_back(key);
                        continue;
                    }
                    // the library writes `num` for a native that takes an int or a float: a float here, an int argument is widened
                    sg.params.push_back(sg.builtin && p.type == TS::Num ? ir::Ty::Float : writtenType(p.type, p.elem, at, "the type of '" + p.name + "'"));
                    sg.paramKey.push_back("");
                }
                if (!f->returnWritten) {
                    if (opts.infer) {
                        sg.retKey = "r:" + f->name;
                        auto h = hints.find(sg.retKey);
                        sg.ret = h == hints.end() ? ir::Ty::Void : h->second;
                    } else {
                        sg.ret = ir::Ty::Void;
                    }
                } else if (f->returnElem && f->returnElem->isHost) {
                    if (!opts.plugin) refuse(f->nameRange, "the result of '" + f->name + "' is host: that type is for the functions of a plugin (aembr plugin), a program has no host");
                    sg.retHost = true;
                    sg.ret = ir::Ty::Void;
                } else {
                    sg.ret = writtenType(f->returnType, f->returnElem, f->nameRange, "the return type of '" + f->name + "'");
                }
                sg.nreq = 0;
                for (size_t i = 0; i < f->params.size(); ++i)
                    if (!(f->params[i].optional && !f->params[i].variadic) && !f->params[i].variadic) sg.nreq = i + 1;
                sigs[f->name] = sg;
            } else if (s->kind == Stmt::Kind::Assign) {
                auto* a = static_cast<const AssignStmt*>(s.get());
                if (a->target->kind == Expr::Kind::Var) { const std::string& tn_ = static_cast<const VarExpr*>(a->target.get())->name; topNames.insert(tn_); topNamesOf[fileOf(s.get())].insert(tn_); }
            } else if (s->kind == Stmt::Kind::Local) {
                for (auto& t : static_cast<const LocalStmt*>(s.get())->targets) { topNames.insert(t.name); topNamesOf[fileOf(s.get())].insert(t.name); }
            } else if (s->kind == Stmt::Kind::For) {
                { const std::string& vn_ = static_cast<const ForStmt*>(s.get())->varName; topNames.insert(vn_); topNamesOf[fileOf(s.get())].insert(vn_); }
            }
        }
        m.fns.resize(sigs.size());
        for (auto& kv : sigs) m.fns[(size_t)kv.second.index].id = kv.second.index;
        for (size_t pi = 0; pi < descs.size(); ++pi)
            for (const PluginFnDesc& f : descs[pi].funcs) pluginFns.emplace(f.name, std::make_pair((int)pi, &f));
        std::error_code ec;
        m.scriptDir = fr.path.empty() ? "" : std::filesystem::absolute(fr.path, ec).parent_path().string();
    }

    void function(const FnStmt* f) {
        if (f->isTemplate) return;                 // written for each call, see templateCall
        functionBody(f, sigs[f->name], f->name);
    }
    // the body of a function. `key` names it for the hints and the messages: its name, or for a version of a const fn the name and the argument types
    void functionBody(const FnStmt* f, const Sig& sg, const std::string& key) {
        ir::Fn& fn = m.fns[(size_t)sg.index];
        fn.name = f->name;
        fn.tmpl = f->isTemplate;
        fn.variadic = sg.variadic;
        fn.resultHost = sg.retHost;
        fn.builtin = fr.stdStmts.count(f) > 0;
        fn.internal = fr.stmtFile.count(f) > 0;
        curFileIdx = fileOf(f);
        gRefuseFile = curFileIdx > 0 ? m.files[(size_t)curFileIdx] : std::string();
        curIsStd = fn.builtin;
        curResultHost = sg.retHost;
        for (size_t i = 0; i < sg.defaults.size(); ++i) fn.defaults.push_back(sg.defaults[i].kind == ParamDefault::None ? nullptr : defaultEx(sg.defaults[i], sg.params[i]));
        fn.line = f->range.startLine;
        fn.ret = sg.ret;
        parents.clear();
        cur = &fn; inFn = true; curRet = sg.ret; loopDepth = 0; selfName.clear(); selfSig = -1; capIdx.clear();
        curKey = key;
        retSkipped = false;
        curRetKey = sg.ret == ir::Ty::Void ? sg.retKey : "";
        curRetInferred = !sg.retKey.empty() && sg.ret != ir::Ty::Void;
        curRetHintKey = sg.retKey;
        pendingRet.clear();
        locals.clear();
        if (sg.unknownParam()) {
            // nothing has been called yet that tells the types of the parameters; a later round has them, or nothing does
            deferred.insert(key);
            deferWhy[key] = "'" + key + "' is never called, so the types of its parameters cannot be worked out";
            return;
        }
        try {
            for (size_t i = 0; i < f->params.size(); ++i) {
                if (locals.count(f->params[i].name)) refuse(f->range, "two parameters are called '" + f->params[i].name + "'");
                fn.vars.push_back({f->params[i].name, sg.params[i], false});
                if (f->params[i].inout) { fn.vars.back().byref = true; fn.vars.back().borrowed = true; }
                locals[f->params[i].name] = (int)i;
            }
            fn.nparams = (int)f->params.size();
            block(f->body, fn.body);
            if (retSkipped) throw Retry{};      // no other return gave the result a type; deferReason says why
            if (sg.ret != ir::Ty::Void && !alwaysReturns(f->body))
                refuse(f->nameRange, "'" + key + "' can reach its end without a return");
        } catch (const Retry&) {
            deferred.insert(key);
            if (!deferReason.empty()) deferWhy[key] = deferReason;
        }
        deferReason.clear();
    }

    // false: something is not settled yet and the program has to be lowered again
    void topLevel() {
        curIsStd = false;
        curFileIdx = 0;
        gRefuseFile.clear();
        m.main.name = "main";
        parents.clear();
        cur = &m.main; inFn = false; curRet = ir::Ty::Void; loopDepth = 0; selfName.clear(); selfSig = -1; capIdx.clear();
        curKey = "main";
        curRetKey.clear();
        curRetInferred = false;
        pendingRet.clear();
        locals.clear();
        try {
            for (const StmtPtr& s : fr.ast) {
                if (s->kind == Stmt::Kind::Fn || isModuleMeta(s.get())) continue;
                curFileIdx = fileOf(s.get());
                gRefuseFile = curFileIdx > 0 ? m.files[(size_t)curFileIdx] : std::string();
                stmt(s.get(), m.main.body);
            }
        } catch (const Retry&) {
            deferred.insert("main");
            if (!deferReason.empty()) deferWhy["main"] = deferReason;
        }
        deferReason.clear();
    }
    // `__module__ = {...}` says what the file is (name, version, capabilities, see core/modinfo.h). it is read from the source, it is not code
    static bool isModuleMeta(const Stmt* s) {
        if (s->kind != Stmt::Kind::Assign) return false;
        auto* a = static_cast<const AssignStmt*>(s);
        return a->target->kind == Expr::Kind::Var && static_cast<const VarExpr*>(a->target.get())->name == embr::kModuleMetaName;
    }
    bool run() {
        m.file = fr.path;
        m.files.assign(1, std::string());
        signatures();
        m.nTop = (int)sigs.size();
        // a plugin's functions read its top level names, so those have to exist (and have their types) first
        const bool mainFirst = plugin || anyProgramGlobals();
        if (mainFirst) topLevel();
        for (const StmtPtr& s : fr.ast)
            if (s->kind == Stmt::Kind::Fn) function(static_cast<const FnStmt*>(s.get()));
        if (!mainFirst) topLevel();
        // the versions of the const fns that the calls asked for (writing one can ask for more)
        for (size_t i = 0; i < pendingInst.size(); ++i) {
            const Pending p = pendingInst[i];
            functionBody(p.f, instances.at(p.iname), p.iname);
        }
        if (!deferred.empty()) {
            if (changed) return false;
            std::string names;
            for (const std::string& n : deferred) names += (names.empty() ? "" : ", ") + n;
            for (const std::string& n : deferred) {
                auto w = deferWhy.find(n);
                if (w != deferWhy.end()) refuse(SourceRange(1, 1, 1, 1), w->second + ". write the types it needs");
            }
            refuse(SourceRange(1, 1, 1, 1), "the types in " + names + " could not be settled (an empty array or map whose element type is never told)");
        }
        m.fatalOverflow = opts.fatalOverflow;
        ir::markRaisers(m);
        ir::markBorrowedParams(m);
        return true;
    }
};

}  // namespace

ir::Module lower(const Front& front, const LowerOptions& opts) {
    std::map<std::string, ir::Ty> hints;
    std::vector<ir::FnSig> sigTable;
    bool changed = false;
    for (int round = 0; round < 30; ++round) {
        changed = false;
        Lowerer l(front, hints, sigTable, changed, opts);
        if (l.run()) {
            l.m.sigs = sigTable;
            return std::move(l.m);
        }
    }
    throw Refusal{"the types of the program could not be settled", SourceRange(1, 1, 1, 1), std::string()};
}

}  // namespace aembr
