// ir.cpp
// the parts of the intermediate form that are not a type: which functions can raise, and a text form for `--emit-ir`.

#include "ir.h"

#include <algorithm>
#include <functional>
#include <cctype>
#include <cstring>
#include <sstream>

namespace aembr::ir {

namespace {

void calls(const std::vector<St>& body, std::vector<int>& out, bool& raises, bool builtin) {
    for (const St& s : body) {
        if (s.kind == St::Kind::Call) out.push_back(s.fn);
        if (s.kind == St::Kind::Raising || (s.kind == St::Kind::Native && (nativeRaises(s.nop) || s.raises))) raises = true;
        if (s.kind == St::Kind::Try) raises = true;    // the catch code reads the context
        if (s.kind == St::Kind::InlineC) {
            // C text can raise. in the library the compiler brings in (std/*.embr) the text that never names the context cannot
            bool names = !builtin;
            for (const CSeg& g : s.cseg) if (!g.isVar && g.text.find("cx") != std::string::npos) names = true;
            if (names) raises = true;
        }
        if (s.kind == St::Kind::CallValue) raises = true;   // it can be any function, and it can fail or recurse
        calls(s.then, out, raises, builtin);
        calls(s.els, out, raises, builtin);
        calls(s.body, out, raises, builtin);
    }
}

}  // namespace

void markRaisers(Module& m) {
    const size_t n = m.fns.size();
    std::vector<std::vector<int>> callees(n);
    std::vector<bool> raises(n, false);
    for (size_t i = 0; i < n; ++i) {
        bool r = false;
        calls(m.fns[i].body, callees[i], r, m.fns[i].builtin);
        raises[i] = r;
    }
    // a function that can reach itself recurses, so it can overflow the stack
    for (size_t i = 0; i < n; ++i) {
        std::vector<bool> seen(n, false);
        std::vector<int> todo(callees[i].begin(), callees[i].end());
        while (!todo.empty()) {
            int f = todo.back(); todo.pop_back();
            if (f == (int)i) { m.fns[i].recursive = true; if (!m.fatalOverflow) raises[i] = true; break; }
            if (seen[(size_t)f]) continue;
            seen[(size_t)f] = true;
            for (int g : callees[(size_t)f]) todo.push_back(g);
        }
    }
    // and one that calls a function that can raise
    for (bool changed = true; changed;) {
        changed = false;
        for (size_t i = 0; i < n; ++i)
            if (!raises[i]) for (int f : callees[i]) if (raises[(size_t)f]) { raises[i] = true; changed = true; break; }
    }
    for (size_t i = 0; i < n; ++i) m.fns[i].canRaise = raises[i] || m.fns[i].isClosure;   // a closure is always called through a value, with the context
    m.main.canRaise = true;
}

namespace {

// C text names a variable with $name. it changes the variable when its address is taken (&$x), it is assigned ($x = ..., $x += ..., $x++) or stepped.
// anything else (a read, $x->len, f($x)) leaves it as it is. (a write through the pointer, $x->data[0] = 1, changes a shared object whatever this says)
bool cWrites(const std::vector<CSeg>& seg, size_t k) {
    auto before = [&]() { std::string t = k > 0 && !seg[k - 1].isVar ? seg[k - 1].text : std::string(); while (!t.empty() && isspace((unsigned char)t.back())) t.pop_back(); return t; };
    auto after = [&]() { std::string t = k + 1 < seg.size() && !seg[k + 1].isVar ? seg[k + 1].text : std::string(); size_t i = 0; while (i < t.size() && isspace((unsigned char)t[i])) ++i; return t.substr(i); };
    const std::string b = before(), a = after();
    if (b.size() >= 2 && (b.compare(b.size() - 2, 2, "++") == 0 || b.compare(b.size() - 2, 2, "--") == 0)) return true;
    if (!b.empty() && b.back() == '&') {
        // a single & after a name, a number or a closing bracket is the operator and reads the variable. anywhere else it takes the address
        std::string head = b.substr(0, b.size() - 1);
        while (!head.empty() && isspace((unsigned char)head.back())) head.pop_back();
        if (head.empty()) return true;
        const char c = head.back();
        return !(isalnum((unsigned char)c) || c == '_' || c == ')' || c == ']' || c == '&');
    }
    if (a.compare(0, 2, "++") == 0 || a.compare(0, 2, "--") == 0) return true;
    size_t i = 0;
    if (a.compare(0, 2, "<<") == 0 || a.compare(0, 2, ">>") == 0) i = 2; else if (!a.empty() && strchr("+-*/%|&^", a[0])) i = 1;
    return i < a.size() && a[i] == '=' && (i + 1 >= a.size() || a[i + 1] != '=');
}

bool localRef(const Ref& r) { return !r.global && !r.captured && !r.self; }

// the parameters of `f` that its body can change: assigned, changed in place, given as &name, named in C text, or used to reach into a container that is then changed
void markWritten(const std::vector<St>& body, std::vector<char>& w) {
    auto mark = [&](const Ref& r) { if (localRef(r) && (size_t)r.index < w.size()) w[(size_t)r.index] = 1; };
    std::function<void(const ExP&)> scan = [&](const ExP& e) {
        if (!e) return;
        if (e->kind == Ex::Kind::AddrOf) mark(e->ref);
        scan(e->a); scan(e->b);
    };
    for (const St& s : body) {
        if (s.hasDst || s.kind == St::Kind::Try) mark(s.dst);
        for (size_t k = 0; k < s.cseg.size(); ++k)
            if (s.cseg[k].isVar && cWrites(s.cseg, k)) mark(s.cseg[k].ref);
        scan(s.a); scan(s.b);
        for (const ExP& a : s.args) scan(a);
        if (s.kind == St::Kind::Native && !s.args.empty() && s.args[0] && s.args[0]->kind == Ex::Kind::Load &&
            (s.inplace || s.nop == NativeOp::ChildArr || s.nop == NativeOp::ChildMap || s.nop == NativeOp::ChildAny))
            mark(s.args[0]->ref);
        markWritten(s.then, w); markWritten(s.els, w); markWritten(s.body, w);
    }
}

// every call of a function: is each argument something that stays alive and unchanged while the callee runs?
void checkSites(const std::vector<St>& body, const std::vector<Var>& vars, std::vector<std::vector<char>>& unsafe) {
    for (const St& s : body) {
        if (s.kind == St::Kind::Call && s.fn >= 0) {
            for (size_t i = 0; i < s.args.size(); ++i) {
                const ExP& a = s.args[i];
                bool bad = false;
                if (a && a->kind == Ex::Kind::Load) {
                    if (!localRef(a->ref)) bad = true;
                    else {
                        const Var& v = vars[(size_t)a->ref.index];
                        bad = v.boxed || v.byref || v.borrowed || v.slotref;
                        for (const ExP& o : s.args) if (o && o->kind == Ex::Kind::AddrOf && o->ref.index == a->ref.index && localRef(o->ref)) bad = true;
                    }
                }
                if (bad) unsafe[(size_t)s.fn][i] = 1;
            }
        }
        checkSites(s.then, vars, unsafe); checkSites(s.els, vars, unsafe); checkSites(s.body, vars, unsafe);
    }
}

}  // namespace

void markBorrowedParams(Module& m) {
    const size_t n = m.fns.size();
    std::vector<std::vector<char>> unsafe(n);
    for (size_t i = 0; i < n; ++i) unsafe[i].assign((size_t)std::max(m.fns[i].nparams, 0) + 8, 0);
    for (size_t i = 0; i < n; ++i) checkSites(m.fns[i].body, m.fns[i].vars, unsafe);
    checkSites(m.main.body, m.main.vars, unsafe);
    for (size_t i = 0; i < n; ++i) {
        Fn& f = m.fns[i];
        if (f.isClosure || f.asValue) continue;            // called through a value: the callers are not known
        std::vector<char> written((size_t)f.nparams, 0);
        markWritten(f.body, written);
        for (int p = 0; p < f.nparams; ++p) {
            Var& v = f.vars[(size_t)p];
            if (isHeap(v.ty) && !v.boxed && !v.byref && !written[(size_t)p] && !unsafe[i][(size_t)p]) v.borrowedParam = true;
        }
    }
}

namespace {

const char* uopName(UnOp o) { return o == UnOp::Neg ? "neg" : o == UnOp::Not ? "not" : "bitnot"; }
const char* bopName(BinOp o) {
    switch (o) {
    case BinOp::Add: return "add"; case BinOp::Sub: return "sub"; case BinOp::Mul: return "mul";
    case BinOp::Lt: return "lt"; case BinOp::Le: return "le"; case BinOp::Gt: return "gt"; case BinOp::Ge: return "ge";
    case BinOp::Eq: return "eq"; case BinOp::Ne: return "ne";
    case BinOp::BitAnd: return "and"; case BinOp::BitOr: return "or"; case BinOp::BitXor: return "xor"; case BinOp::Shl: return "shl"; case BinOp::Shr: return "shr";
    case BinOp::AndAlso: return "andalso"; case BinOp::OrElse: return "orelse";
    }
    return "?";
}
const char* nopName(NativeOp o) {
    switch (o) {
    case NativeOp::StrConcat: return "str_concat"; case NativeOp::StrAppend: return "str_append"; case NativeOp::StrAppendSlice: return "str_append_slice"; case NativeOp::StrEq: return "str_eq";
    case NativeOp::StrIndex: return "str_index"; case NativeOp::StrOfInt: return "str_of_int"; case NativeOp::StrOfFloat: return "str_of_float";
    case NativeOp::Join: return "join"; case NativeOp::ArrNew: return "arr_new"; case NativeOp::ArrPush: return "arr_push";
    case NativeOp::ArrGet: return "arr_get"; case NativeOp::ArrSet: return "arr_set";
    case NativeOp::ChildAny: return "child_any"; case NativeOp::AnySet: return "any_set"; case NativeOp::AnyPush: return "any_push"; case NativeOp::AnyToArr: return "any_to_arr"; case NativeOp::AnyToMap: return "any_to_map";
    case NativeOp::SortCmp: return "sort_cmp"; case NativeOp::Own: return "own"; case NativeOp::ChildArr: return "child_arr"; case NativeOp::ChildMap: return "child_map";
    case NativeOp::MapNew: return "map_new"; case NativeOp::MapGet: return "map_get"; case NativeOp::MapSet: return "map_set";
    case NativeOp::Rt: return "rt"; case NativeOp::Plugin: return "plugin";
    case NativeOp::Raise: return "raise"; case NativeOp::ErrMessage: return "err_message"; case NativeOp::ErrKind: return "err_kind"; case NativeOp::ErrLine: return "err_line";
    case NativeOp::MapHas: return "map_has"; case NativeOp::MapKeys: return "map_keys"; case NativeOp::MapValues: return "map_values";
    }
    return "?";
}
const char* ropName(RaiseOp o) {
    switch (o) { case RaiseOp::DivF: return "div_f"; case RaiseOp::ModI: return "mod_i"; case RaiseOp::ModF: return "mod_f";
                 case RaiseOp::Shl: return "shl"; case RaiseOp::Shr: return "shr"; }
    return "?";
}

struct Printer {
    const Module& m;
    const Fn*     fn = nullptr;
    std::ostringstream os;

    std::string var(const Ref& r) const {
        if (r.self) return "self";
        if (r.captured) return "^" + fn->captures[(size_t)r.index].name;
        if (r.global) return "@" + m.globals[(size_t)r.index].name;
        const Var& v = fn->vars[(size_t)r.index];
        return (v.temp ? "%t" : "%") + (v.temp ? std::to_string(r.index) : v.name);
    }
    std::string ex(const ExP& e) const {
        if (!e) return "_";
        switch (e->kind) {
        case Ex::Kind::ConstInt:   return std::to_string(e->i);
        case Ex::Kind::ConstFloat: { std::ostringstream t; t.precision(17); t << e->f; std::string s = t.str(); if (s.find_first_of(".en") == std::string::npos) s += ".0"; return s; }
        case Ex::Kind::ConstStr: { std::string t = "\""; for (char c : m.strings[(size_t)e->i]) { if (c == '\n') t += "\\n"; else if (c == '"') t += "\\\""; else t += c; } return t + "\""; }
        case Ex::Kind::Len:        return "len(" + ex(e->a) + ")";
        case Ex::Kind::Load:       return var(e->ref);
        case Ex::Kind::AddrOf:     return "&" + var(e->ref);
        case Ex::Kind::ToFloat:    return "float(" + ex(e->a) + ")";
        case Ex::Kind::Un:         return std::string(uopName(e->uop)) + "(" + ex(e->a) + ")";
        case Ex::Kind::Bin:        return std::string(bopName(e->bop)) + "(" + ex(e->a) + ", " + ex(e->b) + ")";
        }
        return "?";
    }
    void block(const std::vector<St>& b, int depth) {
        const std::string pad((size_t)depth * 2, ' ');
        for (const St& s : b) {
            switch (s.kind) {
            case St::Kind::Set:      os << pad << var(s.dst) << " = " << ex(s.a) << "\n"; break;
            case St::Kind::Raising:  os << pad << var(s.dst) << " = " << ropName(s.rop) << "(" << ex(s.a) << ", " << ex(s.b) << ")   # can raise, line " << s.line << "\n"; break;
            case St::Kind::NewCell: os << pad << "new_cell " << var(s.dst) << "\n"; break;
            case St::Kind::MakeClosure: {
                const Fn& f = m.fns[(size_t)s.fn];
                os << pad << var(s.dst) << " = closure " << f.name << "#" << f.id << "(";
                for (size_t i = 0; i < f.captures.size(); ++i) os << (i ? ", " : "") << f.captures[i].name;
                os << ")\n";
                break;
            }
            case St::Kind::CallValue: {
                os << pad;
                if (s.hasDst) os << var(s.dst) << " = ";
                os << ex(s.a) << "(";
                for (size_t i = 0; i < s.args.size(); ++i) os << (i ? ", " : "") << ex(s.args[i]);
                os << ")   # through a function value, line " << s.line << "\n";
                break;
            }
            case St::Kind::Native: {
                os << pad;
                if (s.hasDst) os << var(s.dst) << " = ";
                os << (s.nop == NativeOp::Rt ? s.cname : s.nop == NativeOp::Plugin ? m.plugins[(size_t)s.fn].name + "." + s.cname : std::string(nopName(s.nop))) << "(";
                for (size_t i = 0; i < s.args.size(); ++i) os << (i ? ", " : "") << ex(s.args[i]);
                os << ")" << (s.inplace ? "   # in place" : "") << ((nativeRaises(s.nop) || s.raises) ? "   # can raise, line " + std::to_string(s.line) : "") << "\n";
                break;
            }
            case St::Kind::Call: {
                os << pad;
                if (s.hasDst) os << var(s.dst) << " = ";
                os << m.fns[(size_t)s.fn].name << "(";
                for (size_t i = 0; i < s.args.size(); ++i) os << (i ? ", " : "") << ex(s.args[i]);
                os << ")" << (m.fns[(size_t)s.fn].canRaise ? "   # checks the error flag" : "") << "   # line " << s.line << "\n";
                break;
            }
            case St::Kind::If:
                os << pad << "if " << ex(s.a) << "\n";
                block(s.then, depth + 1);
                if (!s.els.empty()) { os << pad << "else\n"; block(s.els, depth + 1); }
                os << pad << "end\n";
                break;
            case St::Kind::Try:
                os << pad << "try\n"; block(s.body, depth + 1);
                os << pad << "catch " << var(s.dst) << "\n"; block(s.els, depth + 1); os << pad << "end\n";
                break;
            case St::Kind::Loop:     os << pad << "loop\n"; block(s.body, depth + 1); os << pad << "end\n"; break;
            case St::Kind::Break:    os << pad << "break\n"; break;
            case St::Kind::Continue: os << pad << "continue\n"; break;
            case St::Kind::Return:   os << pad << "return" << (s.a ? " " + ex(s.a) : "") << "\n"; break;
            case St::Kind::InlineC:  os << pad << "c-block (" << s.cseg.size() << " pieces)\n"; break;
            case St::Kind::Print:    os << pad << "print"; if (s.args.empty()) os << " " << ex(s.a); else for (const ExP& a : s.args) os << " " << ex(a); os << "\n"; break;
            }
        }
    }
    void function(const Fn& f) {
        fn = &f;
        os << "fn " << f.name << "#" << f.id << (f.isClosure ? " closure" : "") << (f.asValue ? " as_value" : "") << "(";
        for (int i = 0; i < f.nparams; ++i) os << (i ? ", " : "") << f.vars[(size_t)i].name << ": " << tyName(f.vars[(size_t)i].ty);
        os << ") -> " << tyName(f.ret) << (f.canRaise ? "   # can raise" : "") << "\n";
        for (size_t i = (size_t)f.nparams; i < f.vars.size(); ++i)
            os << "  var " << (f.vars[i].temp ? "%t" + std::to_string(i) : f.vars[i].name) << ": " << tyName(f.vars[i].ty) << "\n";
        block(f.body, 1);
        os << "end\n\n";
    }
};

}  // namespace

std::string printIr(const Module& m) {
    Printer p{m, nullptr, {}};
    for (const Var& g : m.globals) p.os << "global " << g.name << ": " << tyName(g.ty) << "\n";
    if (!m.globals.empty()) p.os << "\n";
    for (const Fn& f : m.fns) p.function(f);
    p.function(m.main);
    return p.os.str();
}

}  // namespace aembr::ir
