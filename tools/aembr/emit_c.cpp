// emit_c.cpp
// writes C11 text for an ir::Module. the text is the same for the same input: no dates, no paths other than the source path given.
//
// good to know
//   - a function that can raise takes the error context first and the caller looks at cx->err after the call (see rt/rt.h)
//   - a function that calls anything checks the stack budget on entry, so deep recursion ends in a clean error

#include "emit.h"
#include "record.h"

#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <map>
#include <sstream>

namespace aembr {

using namespace ir;

namespace {

std::string mangle(const std::string& n) {
    std::string out;
    for (unsigned char c : n) {
        if (std::isalnum(c) || c == '_') out += (char)c;
        else { char b[8]; std::snprintf(b, sizeof b, "_x%02x", c); out += b; }
    }
    return out;
}

const char* cty(Ty t) {
    if (isArr(t)) return "EmbrArr*";
    if (isMap(t)) return "EmbrMap*";
    switch (t) {
    case Ty::Int: return "int64_t"; case Ty::Float: return "double"; case Ty::Void: return "void"; case Ty::Str: return "EmbrStr*";
    case Ty::MapInt: case Ty::MapFloat: case Ty::MapStr: return "EmbrMap*";
    case Ty::Err: return "EmbrErr*";
    case Ty::Any: return "EmbrAny*";
    case Ty::Host: return "const EmbrValue*";
    case Ty::ArrInt: case Ty::ArrFloat: case Ty::ArrStr: return "EmbrArr*";
    default: return "EmbrClosure*";       // a function value
    }
}
// the runtime function names end in the kind of heap value: rt_release_str, rt_release_arr
const char* kindName(Ty t) { return t == Ty::Str ? "str" : t == Ty::Err ? "err" : t == Ty::Any ? "any" : isMap(t) ? "map" : isFn(t) ? "clo" : "arr"; }
// what a cell holds: the number rt_cell_new takes, and the member of the union that has the value
int cellKind(Ty t) { return t == Ty::Int ? 0 : t == Ty::Float ? 1 : t == Ty::Str ? 2 : isArr(t) ? 3 : isMap(t) ? 4 : t == Ty::Err ? 5 : t == Ty::Any ? 7 : 6; }
char cellMember(Ty t) { return "ifsamecy"[cellKind(t)]; }
// the element suffix of the array functions: rt_arr_push_i64, _f64, _str
const char* elemSuffix(Ty t) { return t == Ty::Int ? "i64" : t == Ty::Float ? "f64" : t == Ty::Str ? "str" : t == Ty::Any ? "any" : isArr(t) ? "arr" : "map"; }
std::string zeroOf(Ty t) { return t == Ty::Int ? "0" : t == Ty::Float ? "0.0" : isHeap(t) || t == Ty::Host ? "NULL" : ""; }
std::string initOf(Ty t) { return t == Ty::Str ? "rt_empty_str" : zeroOf(t); }

std::string quoted(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\' || c == '?') { o += '\\'; o += (char)c; }       // (an escaped ? cannot start a trigraph)
        else if (c < 32 || c > 126) { char b[8]; std::snprintf(b, sizeof b, "\\%03o", c); o += b; }
        else o += (char)c;
    }
    return o + "\"";
}

std::string floatLit(double d) {
    if (std::isnan(d)) return "NAN";
    if (std::isinf(d)) return d < 0 ? "(-HUGE_VAL)" : "HUGE_VAL";
    char b[64];
    std::snprintf(b, sizeof b, "%a", d);       // exact, so the same double comes back
    return std::string("(") + b + ")";
}

struct Emitter {
    const Module& m;
    const PluginMeta* meta = nullptr;      // set when the output is a plugin
    std::string record;                    // the build record to carry, "" for none
    std::string err;
    std::ostringstream os;
    const Fn* fn = nullptr;

    explicit Emitter(const Module& mod, const PluginMeta* pm = nullptr) : m(mod), meta(pm) {}

    // the cell of a variable that a closure uses, as a pointer
    std::string cellOf(const Ref& r) const {
        if (r.captured) return "self->cells[" + std::to_string(r.index) + "]";
        return "cell" + std::to_string(r.index);
    }
    std::string var(const Ref& r) const {
        if (r.self) return "self";
        if (r.captured) return "(self->cells[" + std::to_string(r.index) + "]->v." + cellMember(fn->captures[(size_t)r.index].ty) + ")";
        if (r.global) return "g_" + mangle(m.globals[(size_t)r.index].name);
        const Var& v = fn->vars[(size_t)r.index];
        if (v.boxed) return "(cell" + std::to_string(r.index) + "->v." + cellMember(v.ty) + ")";
        if (v.byref) return "(*v_" + mangle(v.name) + ")";
        if (v.temp) return v.slotref ? "(*t" + std::to_string(r.index) + ")" : "t" + std::to_string(r.index);
        // two variables of one function can share a name (a loop variable that is gone after its loop)
        for (int i = 0; i < r.index; ++i)
            if (!fn->vars[(size_t)i].temp && fn->vars[(size_t)i].name == v.name) return "v_" + mangle(v.name) + "_" + std::to_string(r.index);
        return "v_" + mangle(v.name);
    }
    static std::string fname(const Fn& f) { return "embr_fn" + std::to_string(f.id) + "_" + mangle(f.name); }
    static std::string wrapperName(const Fn& f) { return "embr_fnv" + std::to_string(f.id); }

    std::string ex(const ExP& e) const {
        switch (e->kind) {
        case Ex::Kind::ConstInt:   return "INT64_C(" + std::to_string(e->i) + ")";
        case Ex::Kind::ConstFloat: return floatLit(e->f);
        case Ex::Kind::ConstStr:   return "((EmbrStr*)&embr_lit" + std::to_string(e->i) + ")";
        case Ex::Kind::Len:        return e->a->ty == Ty::Str ? "(" + ex(e->a) + ")->len" : isMap(e->a->ty) ? "rt_map_len(" + ex(e->a) + ")" : "rt_arr_len(" + ex(e->a) + ")";
        case Ex::Kind::Load:       return var(e->ref);
        case Ex::Kind::AddrOf:     return "(&" + var(e->ref) + ")";
        case Ex::Kind::ToFloat:    return "((double)" + ex(e->a) + ")";
        case Ex::Kind::Un: {
            const std::string a = ex(e->a);
            switch (e->uop) {
            case UnOp::Neg:    return e->ty == Ty::Int ? "EMBR_NEG(" + a + ")" : "(-" + a + ")";
            case UnOp::Not:    return "((int64_t)(" + a + " == 0))";
            case UnOp::BitNot: return "(~" + a + ")";
            }
            break;
        }
        case Ex::Kind::Bin: {
            const std::string a = ex(e->a), b = ex(e->b);
            const bool i = e->ty == Ty::Int;
            auto cmp = [&](const char* op) { return "((int64_t)(" + a + " " + op + " " + b + "))"; };
            switch (e->bop) {
            case BinOp::Add: return i ? "EMBR_ADD(" + a + ", " + b + ")" : "(" + a + " + " + b + ")";
            case BinOp::Sub: return i ? "EMBR_SUB(" + a + ", " + b + ")" : "(" + a + " - " + b + ")";
            case BinOp::Mul: return i ? "EMBR_MUL(" + a + ", " + b + ")" : "(" + a + " * " + b + ")";
            case BinOp::Lt: return cmp("<");
            case BinOp::Le: return cmp("<=");
            case BinOp::Gt: return cmp(">");
            case BinOp::Ge: return cmp(">=");
            case BinOp::Eq: return cmp("==");
            case BinOp::Ne: return cmp("!=");
            case BinOp::BitAnd: return "(" + a + " & " + b + ")";
            case BinOp::BitOr:  return "(" + a + " | " + b + ")";
            case BinOp::BitXor: return "(" + a + " ^ " + b + ")";
            case BinOp::Shl: return "((int64_t)((uint64_t)" + a + " << " + b + "))";
            case BinOp::Shr: return "(" + a + " >> " + b + ")";
            case BinOp::AndAlso: return "((int64_t)(" + a + " != 0 && " + b + " != 0))";
            case BinOp::OrElse:  return "((int64_t)(" + a + " != 0 || " + b + " != 0))";
            }
            break;
        }
        }
        return "0";
    }

    // a function with a str or array anywhere (a parameter, a local, a temp or the result) ends in one place, `done`, that lets go of
    // what its variables hold. a raise jumps there too, and the result is then the zero value
    bool heapFn = false;
    static bool anyHeap(const Fn& f) {
        if (isHeap(f.ret)) return true;
        for (const Var& v : f.vars) if (isHeap(v.ty) || v.boxed) return true;
        return false;
    }
    // what a function that raised gives back (nothing reads it)
    std::string zero() const { return zeroOf(fn->ret); }
    // inside a try block a raise goes to the catch code, which is in this same function
    mutable std::vector<std::string> handlers;
    mutable int labels = 0;
    std::string bail() const {
        if (!handlers.empty()) return "goto " + handlers.back() + ";";
        if (heapFn) return "goto done;";
        return "return" + (zero().empty() ? std::string() : " " + zero()) + ";";
    }
    // dst = value, as a copy (the variable takes its own count) or as a move (it takes over the count the value came with)
    std::string assign(const Ref& d, Ty t, const std::string& val, bool owned) const {
        if (!isHeap(t)) return var(d) + " = " + val + ";";
        return std::string(owned ? "rt_own_" : "rt_set_") + kindName(t) + "(&" + var(d) + ", " + val + ");";
    }
    // the file a call is written in, for the trace: NULL is the program's own
    std::string fileArg(int i) const { return i > 0 && (size_t)i < m.files.size() ? quoted(m.files[(size_t)i]) : std::string("NULL"); }
    std::string chk(const std::string& pad) const { return pad + "if (EMBR_UNLIKELY(RT_ERR(cx))) " + bail() + "\n"; }

    void block(const std::vector<St>& b, int depth) {
        const std::string pad((size_t)depth * 4, ' ');
        for (const St& s : b) {
            switch (s.kind) {
            case St::Kind::Set: os << pad << assign(s.dst, s.a->ty, ex(s.a), false) << "\n"; break;
            case St::Kind::Native: native(s, pad); break;
            case St::Kind::InlineC: {
                // the text goes in as written, inside its own braces. #line makes the C compiler's messages point at the embr file
                const std::string base = m.file.substr(m.file.find_last_of('/') == std::string::npos ? 0 : m.file.find_last_of('/') + 1);
                if (s.cfile.empty()) os << pad << "{\n#line " << s.line << " " << quoted(base) << "\n";
                else                 os << pad << "{\n#line 1 " << quoted(s.cfile) << "\n";
                for (const CSeg& g : s.cseg) os << (g.isVar ? var(g.ref) : g.text);
                const std::string so_far = os.str();
                os << "\n" << pad << "}\n#line " << (std::count(so_far.begin(), so_far.end(), '\n') + 3) << " \"embr_program.c\"\n";
                if (fn->canRaise) os << chk(pad);
                break;
            }
            case St::Kind::Raising: {
                // the test is written here, with the jump to the handler, so one branch does the job of the test and the flag check after it
                const std::string A = ex(s.a), B = ex(s.b), L = std::to_string(s.line), D = var(s.dst);
                switch (s.rop) {
                case RaiseOp::DivF:
                    os << pad << "if (EMBR_UNLIKELY(" << B << " == 0.0)) { rt_raise(cx, \"vm\", " << L << ", \"division by zero\"); " << bail() << " }\n";
                    os << pad << D << " = " << A << " / " << B << ";\n";
                    break;
                case RaiseOp::ModI:
                    os << pad << "if (EMBR_UNLIKELY(" << B << " == 0)) { rt_raise(cx, \"vm\", " << L << ", \"modulo by zero\"); " << bail() << " }\n";
                    os << pad << D << " = embr_mod_i64_nz(" << A << ", " << B << ");\n";
                    break;
                case RaiseOp::ModF:
                    os << pad << "if (EMBR_UNLIKELY(" << B << " == 0.0)) { rt_raise(cx, \"vm\", " << L << ", \"modulo by zero\"); " << bail() << " }\n";
                    os << pad << D << " = fmod(" << A << ", " << B << ");\n";
                    break;
                case RaiseOp::Shl: case RaiseOp::Shr:
                    os << pad << "if (EMBR_UNLIKELY((uint64_t)" << B << " > 63u)) { rt_raise(cx, \"vm\", " << L << ", \"shift count %lld is out of range, it has to be 0 to 63\", (long long)" << B << "); " << bail() << " }\n";
                    os << pad << D << " = " << (s.rop == RaiseOp::Shl ? "embr_shl_nc(" : "embr_shr_nc(") << A << ", " << B << ")" << ";\n";
                    break;
                }
                break;
            }
            case St::Kind::Call: {
                const Fn& callee = m.fns[(size_t)s.fn];
                std::string callText = fname(callee) + "(";
                bool first = true;
                if (callee.canRaise) { callText += "cx"; first = false; }
                for (const ExP& a : s.args) { callText += (first ? "" : ", ") + ex(a); first = false; }
                callText += ")";
                os << pad << (s.hasDst ? assign(s.dst, callee.ret, callText, true) : callText + ";") << "\n";
                if (callee.canRaise) {
                    if (callee.builtin) os << pad << "if (EMBR_UNLIKELY(RT_ERR(cx))) { rt_fix_line(cx, " << s.line << "); " << bail() << " }\n";
                    else if (callee.tmpl) os << pad << "if (EMBR_UNLIKELY(RT_ERR(cx))) " << bail() << "\n";      // (a const fn has no frame of its own, the VM shows none)
                    else os << pad << "if (EMBR_UNLIKELY(RT_ERR(cx))) { rt_trace(cx, " << quoted(callee.name) << ", " << s.line << ", " << fileArg(s.file) << "); " << bail() << " }\n";
                }
                break;
            }
            case St::Kind::NewCell:
                if (fn->vars[(size_t)s.dst.index].boxed)
                    os << pad << "rt_own_cell(&" << cellOf(s.dst) << ", rt_cell_new(" << cellKind(varTy(s.dst)) << "));\n";
                break;
            case St::Kind::MakeClosure: {
                const Fn& f = m.fns[(size_t)s.fn];
                os << pad << "{\n" << pad << "    EmbrClosure* nc = rt_clo_new((void*)" << (f.isClosure ? fname(f) : wrapperName(f)) << ", " << quoted(f.name) << ", "
                   << f.captures.size() << ");\n";
                for (size_t i = 0; i < f.captures.size(); ++i)
                    os << pad << "    nc->cells[" << i << "] = rt_retain_cell(" << cellOf(f.captures[i].from) << ");\n";
                os << pad << "    rt_own_clo(&" << var(s.dst) << ", nc);\n" << pad << "}\n";
                break;
            }
            case St::Kind::CallValue: {
                const FnSig& g = m.sigs[(size_t)sigOf(s.a->ty)];
                const std::string c = ex(s.a);
                std::string callText = "((" + fnPtrType(sigOf(s.a->ty)) + ")" + c + "->fn)(cx, " + c;
                for (const ExP& a : s.args) callText += ", " + ex(a);
                callText += ")";
                os << pad << "if (EMBR_UNLIKELY(!" << c << ")) { rt_raise(cx, \"vm\", " << s.line << ", \"calling a function value that has not been made\"); " << bail() << " }\n";
                os << pad << "{\n" << pad << "    const char* fnm = " << c << "->name;\n";
                os << pad << "    " << (s.hasDst ? assign(s.dst, g.ret, callText, true) : callText + ";") << "\n";
                os << pad << "    if (EMBR_UNLIKELY(RT_ERR(cx))) { rt_trace(cx, fnm, " << s.line << ", " << fileArg(s.file) << "); " << bail() << " }\n" << pad << "}\n";
                break;
            }
            case St::Kind::If:
                os << pad << "if (" << ex(s.a) << ") {\n";
                block(s.then, depth + 1);
                if (!s.els.empty()) { os << pad << "} else {\n"; block(s.els, depth + 1); }
                os << pad << "}\n";
                break;
            case St::Kind::Try: {
                const int id = labels++;
                const std::string lc = "catch_" + std::to_string(id), le = "try_end_" + std::to_string(id);
                handlers.push_back(lc);
                block(s.body, depth);
                handlers.pop_back();
                os << pad << "goto " << le << ";\n";
                os << pad << lc << ": __attribute__((unused));\n";
                os << pad << "rt_own_err(&" << var(s.dst) << ", rt_err_take(cx));\n";
                block(s.els, depth);
                os << pad << le << ": ;\n";
                break;
            }
            case St::Kind::Loop: os << pad << "for (;;) {\n"; block(s.body, depth + 1); os << pad << "}\n"; break;
            case St::Kind::Break: os << pad << "break;\n"; break;
            case St::Kind::Continue: os << pad << "continue;\n"; break;
            case St::Kind::Return:
                if (heapFn) {
                    if (s.a) {
                        os << pad << "r = " << ex(s.a) << ";\n";
                        if (isHeap(s.a->ty)) os << pad << "rt_retain_" << kindName(s.a->ty) << "(r);\n";
                    }
                    os << pad << "goto done;\n";
                } else {
                    if (s.a) os << pad << "return " << ex(s.a) << ";\n"; else os << pad << "return;\n";
                }
                break;
            case St::Kind::Print:
                if (s.args.empty() && (isArr(s.a->ty) || isMap(s.a->ty) || s.a->ty == Ty::Err || s.a->ty == Ty::Any)) {
                    os << pad << putFn(s.a->ty) << "(" << ex(s.a) << ");\n" << pad << "rt_put_end();\n";
                } else if (s.args.empty()) {
                    os << pad << (s.a->ty == Ty::Int ? "rt_print_i64(" : s.a->ty == Ty::Float ? "rt_print_f64(" : "rt_print_str(") << ex(s.a) << ");\n";
                } else {
                    for (size_t i = 0; i < s.args.size(); ++i) {
                        if (i) os << pad << "rt_put_sep();\n";
                        const Ty t = s.args[i]->ty;
                        os << pad << putFn(t) << "(" << ex(s.args[i]) << ");\n";
                    }
                    os << pad << "rt_put_end();\n";
                }
                break;
            }
        }
    }

    // the runtime function that prints a value of this type without a newline
    static const char* putFn(Ty t) {
        return t == Ty::Int ? "rt_put_i64" : t == Ty::Float ? "rt_put_f64" : t == Ty::Str ? "rt_put_str" : t == Ty::Any ? "rt_put_any" : isArr(t) ? "rt_put_arr" : isMap(t) ? "rt_put_map" : "rt_put_err";
    }

    void native(const St& s, const std::string& pad) {
        auto a = [&](size_t i) { return ex(s.args[i]); };
        const std::string line = std::to_string(s.line);
        auto own = [&](Ty t, const std::string& val) { os << pad << assign(s.dst, t, val, true) << "\n"; };
        switch (s.nop) {
        case NativeOp::StrConcat: own(Ty::Str, "rt_str_concat(" + a(0) + ", " + a(1) + ")"); break;
        case NativeOp::StrAppendSlice: os << pad << var(s.dst) << " = rt_str_append_slice(" << var(s.dst) << ", " << a(1) << ", " << a(2) << ", " << a(3) << ");\n"; break;
        case NativeOp::StrAppend: os << pad << var(s.dst) << " = rt_str_append(" << var(s.dst) << ", " << a(1) << ");\n"; break;
        case NativeOp::StrEq:     os << pad << var(s.dst) << " = rt_str_eq(" << a(0) << ", " << a(1) << ");\n"; break;
        case NativeOp::StrIndex:  own(Ty::Str, "rt_str_index(cx, " + a(0) + ", " + a(1) + ", " + line + ")"); os << chk(pad); break;
        case NativeOp::StrOfInt:  own(Ty::Str, "rt_str_of_i64(" + a(0) + ")"); break;
        case NativeOp::StrOfFloat: own(Ty::Str, "rt_str_of_f64(" + a(0) + ")"); break;
        case NativeOp::Join:      own(Ty::Str, "rt_join(" + a(0) + ", " + a(1) + ")"); break;
        case NativeOp::ArrNew: {
            const Ty at = varTy(s.dst);
            own(at, std::string("rt_arr_new(") + std::to_string(elemKind(at)) + ", 0)");
            break;
        }
        case NativeOp::ArrPush: {
            const std::string f = std::string("rt_arr_push_") + elemSuffix(s.args[1]->ty);
            if (s.inplace) os << pad << var(s.dst) << " = " << f << "(" << var(s.dst) << ", " << a(1) << ");\n";
            else own(varTy(s.dst), f + "(rt_retain_arr(" + a(0) + "), " + a(1) + ")");
            break;
        }
        case NativeOp::ArrGet: {
            const Ty et = varTy(s.dst);
            if (!isHeap(et)) {
                // the bounds test and the jump to the handler are one branch. a is null for an array that was never made
                os << pad << "RT_STAT(arr_get);\n";
                os << pad << "if (EMBR_UNLIKELY(!" << a(0) << " || (uint64_t)" << a(1) << " >= (uint64_t)" << a(0) << "->len)) { rt_arr_oob(cx, " << a(1) << ", " << line << "); " << bail() << " }\n";
                if (et == Ty::Int) os << pad << var(s.dst) << " = " << a(0) << "->data[" << a(1) << "];\n";
                else os << pad << "memcpy(&" << var(s.dst) << ", &" << a(0) << "->data[" << a(1) << "], 8);\n";
                break;
            }
            own(et, std::string("rt_arr_get_") + elemSuffix(et) + "(cx, " + a(0) + ", " + a(1) + ", " + line + ")");
            os << chk(pad);
            break;
        }
        case NativeOp::MapNew: {
            const Ty mt = varTy(s.dst);
            own(mt, std::string("rt_map_new(") + std::to_string(elemKind(mt)) + ")");
            break;
        }
        case NativeOp::MapGet: {
            const Ty et = varTy(s.dst);
            const std::string call = std::string("rt_map_get_") + elemSuffix(et) + "(cx, " + a(0) + ", " + a(1) + ", " + line + ")";
            if (isHeap(et)) own(et, call); else os << pad << var(s.dst) << " = " << call << ";\n";
            os << chk(pad);
            break;
        }
        case NativeOp::MapSet:
            os << pad << var(s.dst) << " = rt_map_unique(" << var(s.dst) << ");\n";
            os << pad << "rt_map_set_" << elemSuffix(s.args[2]->ty) << "(" << var(s.dst) << ", " << a(1) << ", " << a(2) << ");\n";
            break;
        case NativeOp::MapHas:    os << pad << var(s.dst) << " = rt_map_has(" << a(0) << ", " << a(1) << ");\n"; break;
        case NativeOp::MapKeys:   own(Ty::ArrStr, "rt_map_keys(" + a(0) + ")"); break;
        case NativeOp::MapValues: own(varTy(s.dst), "rt_map_values(" + a(0) + ")"); break;
        case NativeOp::Rt: {
            std::string call = s.cname + "(";
            bool first = true;
            if (s.raises) { call += "cx"; first = false; }
            for (const ExP& x : s.args) { call += (first ? "" : ", ") + ex(x); first = false; }
            if (s.raises) call += ", " + line;
            call += ")";
            // a native that changes an array or a map held by a variable first makes sure that variable's value is nobody else's
            if (s.inplace) {
                const Ref& r = s.args[0]->ref;
                os << pad << var(r) << " = rt_" << (isMap(varTy(r)) ? "map" : "arr") << "_unique(" << var(r) << ");\n";
            }
            if (!s.hasDst) os << pad << call << ";\n";
            else if (isHeap(varTy(s.dst))) os << pad << assign(s.dst, varTy(s.dst), call, true) << "\n";
            else os << pad << var(s.dst) << " = " << call << ";\n";
            if (s.raises) os << chk(pad);
            break;
        }
        case NativeOp::Plugin: {
            // the arguments go into a small array of kinds, the runtime copies them into the plugin's values and the result back
            const int n = (int)s.args.size();
            os << pad << "{\n" << pad << "    RtPArg pa[" << (n ? n : 1) << "] = {";
            if (!n) os << "{0, 0, 0.0, NULL}";
            for (int i = 0; i < n; ++i) {
                const Ty t = s.args[(size_t)i]->ty;
                const std::string v = ex(s.args[(size_t)i]);
                os << (i ? ", " : "");
                if (t == Ty::Int) os << "{0, " << v << ", 0.0, NULL}";
                else if (t == Ty::Float) os << "{1, 0, " << v << ", NULL}";
                else os << "{" << (t == Ty::Str ? 2 : isArr(t) ? 3 : isMap(t) ? 4 : 5) << ", 0, 0.0, (void*)" << v << "}";
            }
            os << "};\n";
            const Ty rt_ = s.hasDst ? varTy(s.dst) : Ty::Void;
            const int want = rt_ == Ty::Int ? 0 : rt_ == Ty::Float ? 1 : rt_ == Ty::Str ? 2 : rt_ == Ty::ArrInt ? 3 : rt_ == Ty::ArrFloat ? 4 : rt_ == Ty::ArrStr ? 5 : rt_ == Ty::Any ? 7 : 6;
            os << pad << "    RtPRes pr;\n";
            os << pad << "    rt_plugin_call(cx, " << pluginFnVar(s.fn, s.cname) << ", " << quoted(s.cname) << ", " << quoted(m.scriptDir) << ", pa, " << n << ", " << want << ", &pr, " << line << ");\n";
            os << pad << "    if (EMBR_UNLIKELY(RT_ERR(cx))) " << bail() << "\n";
            if (want == 0) os << pad << "    " << var(s.dst) << " = pr.i;\n";
            else if (want == 1) os << pad << "    " << var(s.dst) << " = pr.f;\n";
            else if (want < 6 || want == 7) os << pad << "    rt_own_" << kindName(rt_) << "(&" << var(s.dst) << ", (" << cty(rt_) << ")pr.p);\n";
            os << pad << "}\n";
            break;
        }
        case NativeOp::Raise:
            os << pad << "rt_raise_str(cx, " << line << ", " << a(0) << ");\n" << pad << bail() << "\n";
            break;
        case NativeOp::ErrMessage: own(Ty::Str, "rt_retain_str((" + a(0) + ")->msg)"); break;
        case NativeOp::ErrKind:    own(Ty::Str, "rt_retain_str((" + a(0) + ")->kind)"); break;
        case NativeOp::ErrLine:    os << pad << var(s.dst) << " = (" << a(0) << ")->line;\n"; break;
        case NativeOp::SortCmp: {
            own(varTy(s.dst), "rt_sort_cmp(cx, " + a(0) + ", (void*)" + a(1) + ", " + cmpWrapperName(s) + ", " + a(2) + ")");
            os << chk(pad);
            break;
        }
        case NativeOp::ChildAny: {
            // the address of a slot of a container, so the any in it can be changed where it is
            const std::string t = "t" + std::to_string(s.dst.index);
            if (s.args[0]->ty == Ty::Any) os << pad << t << " = rt_any_slot(cx, &" << var(s.args[0]->ref) << ", " << a(1) << ", " << line << ");\n";
            else os << pad << t << " = " << (isMap(s.args[0]->ty) ? "rt_map_slot_any" : "rt_arr_slot_any") << "(cx, " << a(0) << ", " << a(1) << ", " << line << ");\n";
            os << chk(pad);
            break;
        }
        case NativeOp::AnySet:
            os << pad << "rt_any_set_index(cx, &" << var(s.dst) << ", " << a(1) << ", " << a(2) << ", " << line << ");\n";
            os << chk(pad);
            break;
        case NativeOp::AnyPush:
            os << pad << "rt_any_push(cx, &" << var(s.dst) << ", " << a(1) << ", " << line << ");\n";
            os << chk(pad);
            break;
        case NativeOp::AnyToArr: case NativeOp::AnyToMap:
            own(varTy(s.dst), std::string(s.nop == NativeOp::AnyToArr ? "rt_any_to_arr" : "rt_any_to_map") + "(cx, " + a(0) + ", " + quoted(s.cname) + ", " + a(1) + ", " + line + ")");
            os << chk(pad);
            break;
        case NativeOp::Own:
            os << pad << var(s.dst) << " = rt_" << (isMap(varTy(s.dst)) ? "map" : "arr") << "_unique(" << var(s.dst) << ");\n";
            break;
        case NativeOp::ChildArr: case NativeOp::ChildMap: {
            // a pointer into the container, which was made unique before: no count is taken, and the child is made unique in its slot
            const Ty ct = varTy(s.dst);
            os << pad << var(s.dst) << " = rt_" << (isMap(s.args[0]->ty) ? "map" : "arr") << "_child_" << (s.nop == NativeOp::ChildArr ? "arr" : "map")
               << "(cx, " << a(0) << ", " << a(1) << ", " << elemKind(ct) << ", " << line << ");\n";
            os << chk(pad);
            break;
        }
        case NativeOp::ArrSet:
            os << pad << var(s.dst) << " = rt_arr_unique(" << var(s.dst) << ");\n";
            if (s.args[2]->ty == Ty::Int || s.args[2]->ty == Ty::Float) {
                const std::string d = var(s.dst);
                os << pad << "RT_STAT(arr_set);\n";
                os << pad << "if (EMBR_UNLIKELY(!" << d << " || (uint64_t)" << a(1) << " >= (uint64_t)" << d << "->len)) { rt_arr_oob(cx, " << a(1) << ", " << line << "); " << bail() << " }\n";
                if (s.args[2]->ty == Ty::Int) os << pad << d << "->data[" << a(1) << "] = " << a(2) << ";\n";
                else os << pad << "{ double sv_ = " << a(2) << "; memcpy(&" << d << "->data[" << a(1) << "], &sv_, 8); }\n";
                break;
            }
            os << pad << "rt_arr_set_" << elemSuffix(s.args[2]->ty) << "(cx, " << var(s.dst) << ", " << a(1) << ", " << a(2) << ", " << line << ");\n";
            os << chk(pad);
            break;
        }
    }
    // ---- C plugins the file calls: one variable for the plugin and one for each of its functions that is used
    std::vector<std::pair<int, std::string>> pluginFnsUsed;
    void collectPluginCalls(const std::vector<St>& b) {
        for (const St& s : b) {
            if (s.kind == St::Kind::Native && s.nop == NativeOp::Plugin) {
                std::pair<int, std::string> k{s.fn, s.cname};
                bool seen = false;
                for (auto& x : pluginFnsUsed) seen = seen || x == k;
                if (!seen) pluginFnsUsed.push_back(k);
            }
            collectPluginCalls(s.then); collectPluginCalls(s.els); collectPluginCalls(s.body);
        }
    }
    std::string pluginFnVar(int plugin, const std::string& fnName) const {
        for (size_t i = 0; i < pluginFnsUsed.size(); ++i)
            if (pluginFnsUsed[i].first == plugin && pluginFnsUsed[i].second == fnName) return "embr_plf" + std::to_string(i);
        return "NULL";
    }
    // at the start of the top level code: open each plugin and look up the functions that are called
    void pluginLoader() {
        for (size_t p = 0; p < m.plugins.size(); ++p)
            os << "    embr_pl" << p << " = rt_plugin_open(cx, " << quoted(m.plugins[p].path) << ", " << quoted(m.plugins[p].name) << ", " << quoted(m.scriptDir)
               << ");\n    if (EMBR_UNLIKELY(RT_ERR(cx))) " << bail() << "\n";
        for (size_t i = 0; i < pluginFnsUsed.size(); ++i)
            os << "    embr_plf" << i << " = rt_plugin_fn(cx, embr_pl" << pluginFnsUsed[i].first << ", " << quoted(pluginFnsUsed[i].second)
               << ");\n    if (EMBR_UNLIKELY(RT_ERR(cx))) " << bail() << "\n";
    }

    Ty varTy(const Ref& r) const {
        if (r.self) return fnTy(fn->sig);
        if (r.captured) return fn->captures[(size_t)r.index].ty;
        return r.global ? m.globals[(size_t)r.index].ty : fn->vars[(size_t)r.index].ty;
    }

    // the C type of the pointer a function value holds, for a given signature
    // sort_cmp calls the comparator from C, so each one gets a small function that turns two slots of the array into the arguments of the closure
    std::map<std::string, int> cmpIds;
    std::vector<std::string>   cmpTexts;
    std::string cmpWrapperText(const St& s, const std::string& name) const {
        const int sig = sigOf(s.args[1]->ty);
        const FnSig& g = m.sigs[(size_t)sig];
        const Ty et = elemOf(s.args[0]->ty);
        auto dec = [&](const char* v) -> std::string {
            if (et == Ty::Int) return v;
            if (et == Ty::Float) return std::string("({ double d_; memcpy(&d_, &") + v + ", 8); d_; })";
            return std::string("(") + cty(et) + ")(intptr_t)" + v;
        };
        std::string t = "static int " + name + "(RtCtx* cx, void* clo, int64_t a, int64_t b) {\n    EmbrClosure* c = (EmbrClosure*)clo;\n    ";
        t += std::string(cty(g.ret)) + " r = ((" + fnPtrType(sig) + ")c->fn)(cx, c, " + dec("a") + ", " + dec("b") + ");\n";
        t += g.ret == Ty::Float ? "    return r < 0.0 ? -1 : r > 0.0;\n}\n" : "    return r < 0 ? -1 : r > 0;\n}\n";
        return t;
    }
    std::string cmpWrapperName(const St& s) {
        const std::string key = cmpWrapperText(s, "@");
        auto it = cmpIds.find(key);
        if (it == cmpIds.end()) { it = cmpIds.emplace(key, (int)cmpTexts.size()).first; cmpTexts.push_back(key); }
        return "embr_cmp" + std::to_string(it->second);
    }
    void collectCmp(const std::vector<St>& b) {
        for (const St& s : b) {
            if (s.kind == St::Kind::Native && s.nop == NativeOp::SortCmp) cmpWrapperName(s);
            collectCmp(s.then); collectCmp(s.els); collectCmp(s.body);
        }
    }

    std::string fnPtrType(int sig) const {
        const FnSig& g = m.sigs[(size_t)sig];
        std::string t = std::string(cty(g.ret)) + " (*)(RtCtx*, EmbrClosure*";
        for (Ty p : g.params) t += std::string(", ") + cty(p);
        return t + ")";
    }

    static bool hasCallValue(const std::vector<St>& b) {
        for (const St& s : b)
            if (s.kind == St::Kind::CallValue || hasCallValue(s.then) || hasCallValue(s.els) || hasCallValue(s.body)) return true;
        return false;
    }
    static bool hasCall(const std::vector<St>& b) {
        for (const St& s : b)
            if (s.kind == St::Kind::Call || s.kind == St::Kind::CallValue || hasCall(s.then) || hasCall(s.els) || hasCall(s.body)) return true;
        return false;
    }

    std::string head(const Fn& f, bool isMain) const {
        std::string h = std::string(f.tmpl ? "static inline " : f.builtin ? "static __attribute__((unused)) " : "static ") + std::string(isMain ? "void" : cty(f.ret)) + " " + (isMain ? "embr_main" : fname(f)) + "(";
        bool first = true;
        if (f.canRaise) { h += "RtCtx* cx __attribute__((unused))"; first = false; }
        if (f.isClosure) { h += ", EmbrClosure* self __attribute__((unused))"; }
        for (int i = 0; i < f.nparams; ++i) {
            h += std::string(first ? "" : ", ") + cty(f.vars[(size_t)i].ty) + (f.vars[(size_t)i].byref ? "*" : "") + " v_" + mangle(f.vars[(size_t)i].name);   // a parameter is first, so it is never renamed
            first = false;
        }
        if (first) h += "void";
        return h + ")";
    }

    void function(const Fn& f, bool isMain) {
        fn = &f;
        heapFn = anyHeap(f);
        os << head(f, isMain) << " {\n    RT_STAT(calls);\n";
        if (heapFn && !isMain && f.ret != Ty::Void) os << "    " << cty(f.ret) << " r = " << zeroOf(f.ret) << ";\n";
        for (size_t i = 0; i < f.vars.size(); ++i) {
            const Var& v = f.vars[i];
            if (v.boxed) {      // a closure uses it: it lives in a cell, and a parameter is copied into one
                os << "    EmbrCell* cell" << i << " __attribute__((unused)) = rt_cell_new(" << cellKind(v.ty) << ");\n";
                continue;
            }
            if (i >= (size_t)f.nparams && v.slotref)
                os << "    EmbrAny** t" << i << " __attribute__((unused)) = NULL;\n";
            else if (i >= (size_t)f.nparams)
                os << "    " << cty(v.ty) << " " << var({false, (int)i}) << " __attribute__((unused)) = " << initOf(v.ty) << ";\n";
        }
        // the function holds its own count of every parameter, so a write to one cannot show in the caller
        for (int i = 0; i < f.nparams; ++i) {
            const Var& v = f.vars[(size_t)i];
            const std::string raw = "v_" + mangle(v.name);
            if (v.byref || v.borrowedParam) continue;
            if (v.boxed) {
                os << "    cell" << i << "->v." << cellMember(v.ty) << " = " << (isHeap(v.ty) ? "rt_retain_" + std::string(kindName(v.ty)) + "(" + raw + ")" : raw) << ";\n";
            } else if (isHeap(v.ty)) {
                os << "    RT_STAT(param_retain);\n    rt_retain_" << kindName(v.ty) << "(" << raw << ");\n";
            }
        }
        if (m.fatalOverflow) {
            // only a function that can call itself (or calls through a value, which might) can run the stack out
            if (!isMain && (f.recursive || hasCallValue(f.body)))
                os << "    if (EMBR_UNLIKELY(rt_stack_low_g())) rt_stack_overflow(" << f.line << ");\n";
        } else if (f.canRaise && !isMain && hasCall(f.body))
            os << "    if (EMBR_UNLIKELY(embr_stack_low(cx))) { rt_raise(cx, \"vm\", " << f.line << ", \"stack overflow: the stack budget is used up\"); " << bail() << " }\n";
        if (isMain && !m.plugins.empty()) pluginLoader();
        block(f.body, 1);
        if (heapFn) {
            os << "    done: __attribute__((unused));\n";
            for (size_t i = 0; i < f.vars.size(); ++i) {
                if (f.vars[i].boxed) os << "    rt_release_cell(cell" << i << ");\n";
                else if (isHeap(f.vars[i].ty) && !f.vars[i].borrowed && !f.vars[i].byref && !f.vars[i].borrowedParam) os << "    rt_release_" << kindName(f.vars[i].ty) << "(" << var({false, (int)i}) << ");\n";
            }
            if (!isMain && f.ret != Ty::Void) os << "    return r;\n";
        }
        os << "}\n\n";
    }

    // ---- a plugin
    static const char* tBit(Ty t) { return t == Ty::Any || t == Ty::Host ?  "EMBR_T_ANY" : t == Ty::Int ? "EMBR_T_INT" : t == Ty::Float ? "EMBR_T_FLOAT" : t == Ty::Str ? "EMBR_T_STRING" : isArr(t) ? "EMBR_T_ARRAY" : "EMBR_T_MAP"; }
    static bool nested(Ty t) { return (isArr(t) || isMap(t)) && (isArr(elemOf(t)) || isMap(elemOf(t))); }
    static std::string what(Ty t) { return isFn(t) ? "a function value, which cannot cross the plugin boundary" : tyName(t); }
    static bool boundaryType(Ty t) { return t == Ty::Host || t == Ty::Int || t == Ty::Float || t == Ty::Str || t == Ty::Any || isArr(t) || isMap(t); }
    // a type as the text the runtime reads for a cast: i f s a, A<elem>, M<elem>
    static std::string descOf(Ty t) {
        if (t == Ty::Int) return "i";
        if (t == Ty::Float) return "f";
        if (t == Ty::Str) return "s";
        if (t == Ty::Any) return "a";
        return std::string(isArr(t) ? "A" : "M") + descOf(elemOf(t));
    }

    // the arguments from `p` on, collected into the array of the variadic parameter
    static std::string restIn(Ty t, int p) {
        const char* kind = elemOf(t) == Ty::Int ? "i64" : elemOf(t) == Ty::Float ? "f64" : "str";
        return std::string("rtp_in_rest_") + kind + "(&cx, h, args, n, " + std::to_string(p) + ")";
    }
    // the value of the host's argument `i`, as a compiled one
    std::string argIn(Ty t, int i) const {
        const std::string a = "&args[" + std::to_string(i) + "]";
        if (t == Ty::Host) return a;       // the host's own value, not read into anything
        if (t == Ty::Any) return "rtp_in_any(&cx, h, " + a + ")";
        if (nested(t)) return std::string(isArr(t) ? "rtp_in_nested_arr" : "rtp_in_nested_map") + "(&cx, h, " + a + ", " + quoted(descOf(elemOf(t))) + ")";
        if ((isArr(t) || isMap(t)) && elemOf(t) == Ty::Any) return std::string(isArr(t) ? "rtp_in_nested_arr" : "rtp_in_nested_map") + "(&cx, h, " + a + ", \"a\")";
        switch (t) {
        case Ty::Int:      return "embrp_to_int(h, " + a + ")";
        case Ty::Float:    return "embrp_to_float(h, " + a + ")";
        case Ty::Str:      return "rtp_in_str(h, " + a + ")";
        case Ty::ArrInt:   return "rtp_in_arr_i64(&cx, h, " + a + ")";
        case Ty::ArrFloat: return "rtp_in_arr_f64(&cx, h, " + a + ")";
        case Ty::ArrStr:   return "rtp_in_arr_str(&cx, h, " + a + ")";
        case Ty::MapInt:   return "rtp_in_map(&cx, h, " + a + ", 0)";
        case Ty::MapFloat: return "rtp_in_map(&cx, h, " + a + ", 1)";
        default:           return "rtp_in_map(&cx, h, " + a + ", 2)";
        }
    }
    std::string resultOut(Ty t) const {
        if (t == Ty::Any) return "rtp_out_any(h, out, r)";
        if (nested(t) || ((isArr(t) || isMap(t)) && elemOf(t) == Ty::Any)) return std::string(isArr(t) ? "rtp_out_nested_arr" : "rtp_out_nested_map") + "(h, out, r)";
        switch (t) {
        case Ty::Int:      return "embrp_set_int(h, out, r)";
        case Ty::Float:    return "embrp_set_float(h, out, r)";
        case Ty::Str:      return "rtp_out_str(h, out, r)";
        case Ty::ArrInt:   return "rtp_out_arr_i64(h, out, r)";
        case Ty::ArrFloat: return "rtp_out_arr_f64(h, out, r)";
        case Ty::ArrStr:   return "rtp_out_arr_str(h, out, r)";
        default:           return "rtp_out_map(h, out, r)";
        }
    }

    bool pluginTail() {
        std::vector<const Fn*> ex;
        for (int i = 0; i < m.nTop; ++i) {
            const Fn& f = m.fns[(size_t)i];
            if (f.name.compare(0, 2, "__") == 0 || f.builtin || f.internal) continue;      // (the library the compiler brings in is not part of the plugin's interface)
            for (int p = 0; p < f.nparams; ++p) {
                if (f.vars[(size_t)p].byref) {
                    err = "'" + f.name + "' cannot be exported: parameter '" + f.vars[(size_t)p].name + "' is in-out (start the name with __ to keep the function inside the plugin)";
                    return false;
                }
                if (!boundaryType(f.vars[(size_t)p].ty)) {
                    err = "'" + f.name + "' cannot be exported: parameter '" + f.vars[(size_t)p].name + "' is " + what(f.vars[(size_t)p].ty) +
                          " (start the name with __ to keep the function inside the plugin)";
                    return false;
                }
            }
            if (f.ret != Ty::Void && !boundaryType(f.ret)) {
                err = "'" + f.name + "' cannot be exported: it returns " + what(f.ret) + " (start the name with __ to keep the function inside the plugin)";
                return false;
            }
            ex.push_back(&f);
        }
        const std::string src = quoted(m.file);
        os << "/* ---- the plugin interface */\n\n";
        for (const Fn* f : ex) {
            fn = f;
            const std::string id = std::to_string(f->id);
            os << "static int embrp_" << id << "(EmbrCtx* h, const EmbrValue* args, size_t n, EmbrValue* out) {\n";
            os << "    (void)n; (void)args; (void)out;\n    RtCtx cx;\n    rt_ctx_init(&cx, " << src << ");\n";
            for (int p = 0; p < f->nparams; ++p) {
                const Ty t = f->vars[(size_t)p].ty;
                os << "    " << cty(t) << " a" << p << " = " << (isHeap(t) ? "NULL" : zeroOf(t)) << ";\n";
            }
            if (f->ret != Ty::Void) os << "    " << cty(f->ret) << " r = " << zeroOf(f->ret) << ";\n";
            for (int p = 0; p < f->nparams; ++p) {
                const Ty t = f->vars[(size_t)p].ty;
                std::string in = f->variadic && p == f->nparams - 1 ? restIn(t, p) : argIn(t, p);
                if ((size_t)p < f->defaults.size() && f->defaults[(size_t)p]) in = "n > " + std::to_string(p) + " ? " + in + " : " + this->ex(f->defaults[(size_t)p]);
                os << "    " << (isHeap(t) ? "if (!cx.err) " : "") << "a" << p << " = " << in << ";\n";
            }
            bool usesHost = f->resultHost;
            for (int p = 0; p < f->nparams; ++p) usesHost = usesHost || f->vars[(size_t)p].ty == Ty::Host;
            if (usesHost) os << "    rt_host_ctx = h;\n    rt_host_out = out;\n";
            std::string call = fname(*f) + "(";
            bool first = true;
            if (f->canRaise) { call += "&cx"; first = false; }
            for (int p = 0; p < f->nparams; ++p) { call += std::string(first ? "" : ", ") + "a" + std::to_string(p); first = false; }
            call += ")";
            os << "    if (!cx.err) " << (f->ret != Ty::Void ? "r = " : "") << call << ";\n";
            for (int p = 0; p < f->nparams; ++p) {
                const Ty t = f->vars[(size_t)p].ty;
                if (isHeap(t)) os << "    rt_release_" << kindName(t) << "(a" << p << ");\n";
            }
            const bool heapRet = isHeap(f->ret);
            os << "    if (cx.err) {\n";
            if (heapRet) os << "        rt_release_" << kindName(f->ret) << "(r);\n";
            os << "        return rtp_finish(h, &cx);\n    }\n";
            if (f->ret == Ty::Void) os << "    return 0;\n";
            else {
                os << "    const int rc = " << resultOut(f->ret) << ";\n";
                if (heapRet) os << "    rt_release_" << kindName(f->ret) << "(r);\n";
                os << "    return rc;\n";
            }
            os << "}\n\n";
            if (f->nparams) {
                os << "static const EmbrParam embrp_params_" << id << "[] = {\n";
                for (int p = 0; p < f->nparams; ++p)
                    os << "    { " << quoted(f->vars[(size_t)p].name) << ", " << tBit(f->variadic && p == f->nparams - 1 ? elemOf(f->vars[(size_t)p].ty) : f->vars[(size_t)p].ty)
                       << ", " << (f->variadic && p == f->nparams - 1 ? "EMBR_PARAM_VARIADIC" : (size_t)p < f->defaults.size() && f->defaults[(size_t)p] ? "EMBR_PARAM_OPTIONAL" : "0") << " },\n";
                os << "};\n\n";
            }
        }
        if (ex.empty()) { err = "the file has no function to export (a plugin function is a top level fn whose name does not start with __)"; return false; }
        os << "static const EmbrFunc embrp_funcs[] = {\n";
        for (const Fn* f : ex) {
            const std::string id = std::to_string(f->id);
            auto d = meta->docs.find(f->name);
            const std::string doc = d != meta->docs.end() ? d->second : f->name;
            const char* result = f->resultHost ? "EMBR_T_ANY" : f->ret == Ty::Void ? "EMBR_T_FLOAT" : tBit(f->ret);
            const char* elem = isArr(f->ret) ? tBit(elemOf(f->ret)) : "0";
            os << "    { " << quoted(f->name) << ", " << quoted(doc) << ", embrp_" << id << ", NULL, " << (f->nparams ? "embrp_params_" + id : std::string("NULL"))
               << ", " << f->nparams << ", " << result << ", 0, " << elem << " },\n";
        }
        os << "};\n\n";
        // what the top level statements made is complete now: constants are marked so that nothing writes to them
        const bool hasInit = !m.main.body.empty() || !m.globals.empty() || !m.plugins.empty();
        if (hasInit) {
            fn = &m.main;
            os << "static int embrp_init(EmbrCtx* h) {\n    RtCtx cx;\n    rt_ctx_init(&cx, " << src << ");\n";
            for (const Var& g : m.globals) if (g.ty == Ty::Str) os << "    g_" << mangle(g.name) << " = rt_empty_str;\n";
            os << "    embr_main(&cx);\n";
            for (const Var& g : m.globals)
                if (isHeap(g.ty)) os << "    rt_immortal_" << kindName(g.ty) << "(g_" << mangle(g.name) << ");\n";
            os << "    return rtp_finish(h, &cx);\n}\n\n";
        }
        os << "static const EmbrPluginInfo embrp_info = {\n    EMBR_PLUGIN_ABI_VERSION, sizeof(EmbrPluginInfo), " << quoted(meta->name) << ", "
           << quoted(meta->version) << ", " << quoted(meta->description) << ",\n    " << meta->caps << "ull, embrp_funcs, " << ex.size() << ", "
           << (hasInit ? "embrp_init" : "NULL") << ", 0, NULL, NULL\n};\n\n";
        os << "EMBR_PLUGIN_EXPORT const EmbrPluginInfo* embr_plugin_info(void) { return &embrp_info; }\n";
        return true;
    }

    std::string run() {
        os << "/* written by aembr from " << m.file << " */\n#include \"rt.h\"\n";
        if (meta || !m.plugins.empty()) os << "#include <embr_plugin.h>\n";
        if (!m.plugins.empty()) os << "#include \"rt_host.h\"\n";
        if (meta) os << "#include \"rt_plugin.h\"\n";
        os << "\n";
        // `c file` text goes in as written, after the runtime's headers and before everything the compiler writes, with #line so the C compiler's
        // messages name the embr file (or the file `const c file import` read it from) and its own lines
        {
            const std::string base = m.file.substr(m.file.find_last_of('/') == std::string::npos ? 0 : m.file.find_last_of('/') + 1);
            for (const CFile& f : m.fileC) {
                if (f.file.empty()) os << "#line " << f.line << " " << quoted(base) << "\n";
                else                os << "#line 1 " << quoted(f.file) << "\n";
                os << f.text << "\n";
                const std::string so_far = os.str();
                os << "#line " << (std::count(so_far.begin(), so_far.end(), '\n') + 2) << " \"embr_program.c\"\n\n";
            }
        }
        // what made this file, as text inside it: `aembr info` finds it by scanning, without running or opening anything
        if (!record.empty()) os << "__attribute__((used)) static const char aembr_record[] = " << quoted(recordBegin() + record + recordEnd()) << ";\n\n";
        for (size_t i = 0; i < m.strings.size(); ++i) {
            const size_t n = m.strings[i].size();
            os << "static struct { int64_t rc, len, cap; char d[" << n + 1 << "]; } embr_lit" << i << " = { EMBR_IMMORTAL, " << n << ", " << n
               << ", " << quoted(m.strings[i]) << " };\n";
        }
        if (!m.strings.empty()) os << "\n";
        for (const Fn& f : m.fns) collectCmp(f.body);
        collectCmp(m.main.body);
        for (const Fn& f : m.fns) collectPluginCalls(f.body);
        collectPluginCalls(m.main.body);
        for (size_t p = 0; p < m.plugins.size(); ++p) os << "static const EmbrPluginInfo* embr_pl" << p << ";\n";
        for (size_t i = 0; i < pluginFnsUsed.size(); ++i) os << "static EmbrFn embr_plf" << i << ";\n";
        if (!m.plugins.empty()) os << "\n";
        // a plugin's top level names, and the names of a program that its functions use (the other names of a program are locals of main)
        for (const Var& g : m.globals) os << "static " << cty(g.ty) << " g_" << mangle(g.name) << " = " << zeroOf(g.ty) << ";\n";
        if (!m.globals.empty()) os << "\n";
        for (size_t i = 0; i < cmpTexts.size(); ++i) {
            std::string t = cmpTexts[i];
            t.replace(t.find('@'), 1, "embr_cmp" + std::to_string(i));
            os << t << "\n";
        }
        for (const Fn& f : m.fns) os << head(f, false) << ";\n";
        if (!m.fns.empty()) os << "\n";
        // a function of the file used as a value is called through a closure, so it gets a wrapper that takes the closure and drops it
        for (const Fn& f : m.fns) {
            if (!f.asValue) continue;
            std::string h = "static " + std::string(cty(f.ret)) + " " + wrapperName(f) + "(RtCtx* cx __attribute__((unused)), EmbrClosure* self __attribute__((unused))";
            std::string call = fname(f) + "(" + (f.canRaise ? "cx" : "");
            for (int i = 0; i < f.nparams; ++i) {
                h += std::string(", ") + cty(f.vars[(size_t)i].ty) + " a" + std::to_string(i);
                call += std::string(f.canRaise || i ? ", " : "") + "a" + std::to_string(i);
            }
            os << h << ") { " << (f.ret == Ty::Void ? "" : "return ") << call << "); }\n";
        }
        for (const Fn& f : m.fns) function(f, false);
        function(m.main, true);
        if (meta) {
            if (!pluginTail()) return "";
        } else {
            os << "int main(int argc, char** argv) {\n    RtCtx cx;\n    rt_set_args(argc, argv);\n    rt_ctx_init(&cx, " << quoted(m.file) << ");\n";
            for (const Var& g : m.globals) if (g.ty == Ty::Str) os << "    g_" << mangle(g.name) << " = rt_empty_str;\n";
            os << "    embr_main(&cx);\n";
            for (const Var& g : m.globals) if (isHeap(g.ty)) os << "    rt_release_" << kindName(g.ty) << "(g_" << mangle(g.name) << ");\n";
            os << "    return rt_finish(&cx);\n}\n";
        }
        return os.str();
    }
};

}  // namespace

std::string emitC(const Module& m, const std::string& record) {
    Emitter e(m);
    e.record = record;
    return e.run();
}

bool emitPluginC(const Module& m, const PluginMeta& meta, std::string& out, std::string& err, const std::string& record) {
    Emitter e(m, &meta);
    e.record = record;
    out = e.run();
    if (!e.err.empty()) { err = e.err; return false; }
    return true;
}

}  // namespace aembr
