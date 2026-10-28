// ir.h
// the small typed intermediate form between the checked tree and the C text.
//
// usage
//   ir::Module m = lower(...);         // lower.cpp
//   std::string c = emitC(m);          // emit_c.cpp
//   std::string t = printIr(m);        // ir.cpp, what `aembr build --emit-ir` shows
//
// good to know
//   - it is structured (if, loop, break), not a graph of blocks, because the target is C
//   - an expression (Ex) is pure: no calls and nothing that can raise. whatever has an effect or can raise is its own statement
//     (St) writing into a variable, so the order of effects is the order of the statements
//   - int, float, str and arrays of those. a variable of a heap type owns one count of its value (released when it is
//     replaced and when the function ends), a function takes its own count of a parameter, and a return gives one to the caller

#ifndef AEMBR_IR_H
#define AEMBR_IR_H

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace aembr::ir {

// from Fn on, a type is a function value and the number says which signature (Module::sigs): Ty(Fn + 3) is the fourth signature.
// from Nest on, a type is an array or a map that holds a container (arr<arr<int>>): the number says which one in the table of nested types
enum class Ty : int { Int, Float, Void, Str, ArrInt, ArrFloat, ArrStr, MapInt, MapFloat, MapStr, Err, Any, Host, Fn = 100, Nest = 1000000 };

// the nested container types, made when they are first asked for. one table for the whole process: a compile is one thread
struct NestInfo { bool isMap; Ty elem; };
inline std::vector<NestInfo>& nestTable() { static std::vector<NestInfo> t; return t; }
inline bool isNest(Ty t) { return (int)t >= (int)Ty::Nest; }
inline Ty nestOf(bool isMap, Ty elem) {
    auto& t = nestTable();
    for (size_t i = 0; i < t.size(); ++i) if (t[i].isMap == isMap && t[i].elem == elem) return Ty((int)Ty::Nest + (int)i);
    t.push_back({isMap, elem});
    return Ty((int)Ty::Nest + (int)t.size() - 1);
}

inline const char* tyName(Ty t) {
    switch (t) {
    case Ty::Int: return "int"; case Ty::Float: return "float"; case Ty::Void: return "nothing"; case Ty::Str: return "str";
    case Ty::ArrInt: return "arr<int>"; case Ty::ArrFloat: return "arr<float>"; case Ty::ArrStr: return "arr<str>";
    case Ty::Err: return "error";
    case Ty::Any: return "any";
    case Ty::Host: return "host";
    case Ty::Fn: default: {
        if (isNest(t)) {
            static thread_local std::string buf[8];
            static thread_local int turn = 0;
            const NestInfo& n = nestTable()[(size_t)((int)t - (int)Ty::Nest)];
            std::string& b = buf[turn++ & 7];
            b = std::string(n.isMap ? "map<" : "arr<") + tyName(n.elem) + ">";
            return b.c_str();
        }
        return "fn";
    }
    case Ty::MapInt: return "map<int>"; case Ty::MapFloat: return "map<float>"; case Ty::MapStr: return "map<str>";
    }
    return "?";
}
// a value with a reference count: the variable that holds it owns one count
inline bool isArr(Ty t) { return t == Ty::ArrInt || t == Ty::ArrFloat || t == Ty::ArrStr || (isNest(t) && !nestTable()[(size_t)((int)t - (int)Ty::Nest)].isMap); }
inline bool isMap(Ty t) { return t == Ty::MapInt || t == Ty::MapFloat || t == Ty::MapStr || (isNest(t) && nestTable()[(size_t)((int)t - (int)Ty::Nest)].isMap); }
inline bool isFn(Ty t) { return (int)t >= (int)Ty::Fn && (int)t < (int)Ty::Nest; }
inline Ty fnTy(int sig) { return Ty((int)Ty::Fn + sig); }
inline int sigOf(Ty t) { return (int)t - (int)Ty::Fn; }
inline bool isHeap(Ty t) { return t == Ty::Str || t == Ty::Err || t == Ty::Any || isArr(t) || isMap(t) || isFn(t); }
// what an array or a map holds
inline Ty elemOf(Ty t) {
    if (isNest(t)) return nestTable()[(size_t)((int)t - (int)Ty::Nest)].elem;
    return t == Ty::ArrInt || t == Ty::MapInt ? Ty::Int : t == Ty::ArrFloat || t == Ty::MapFloat ? Ty::Float : Ty::Str;
}
// an array or map holds int, float, str, or another array or map. (what else it could hold is the caller's to refuse)
inline Ty arrOf(Ty e) { return e == Ty::Int ? Ty::ArrInt : e == Ty::Float ? Ty::ArrFloat : e == Ty::Str ? Ty::ArrStr : nestOf(false, e); }
inline Ty mapOf(Ty e) { return e == Ty::Int ? Ty::MapInt : e == Ty::Float ? Ty::MapFloat : e == Ty::Str ? Ty::MapStr : nestOf(true, e); }
// what a container with elements of this type can hold (the type of an element)
inline bool canBeElem(Ty e) { return e == Ty::Int || e == Ty::Float || e == Ty::Str || e == Ty::Any || isArr(e) || isMap(e); }
// the kind number the runtime keeps in an array or a map: 0 int, 1 float, 2 str, 3 arr, 4 map
inline int elemKind(Ty container) { const Ty e = elemOf(container); return e == Ty::Int ? 0 : e == Ty::Float ? 1 : e == Ty::Str ? 2 : isArr(e) ? 3 : isMap(e) ? 4 : 5; }

struct Var {
    std::string name;
    Ty          ty = Ty::Int;
    bool        temp = false;     // made by the compiler, not named in the source
    bool        boxed = false;    // a closure uses it, so it lives in a cell that the function and the closures share
    bool        slotref = false;  // holds the address of a slot that holds an any (inside an array, a map or a box), and stands for what is in that slot; borrowed too
    bool        borrowedParam = false; // a parameter that is only read, whose callers all keep the value alive during the call: it takes no count of its own (markBorrowedParams)
    bool        byref = false;    // an in-out parameter: holds the address of the caller's variable, and stands for what is in it. owns no count
    bool        borrowed = false; // holds a pointer into a container (a nested one being changed in place): it owns no count, so nothing is released
};

// what a function value takes and gives
struct FnSig { std::vector<Ty> params; Ty ret = Ty::Void; };

struct Ex;
using ExP = std::shared_ptr<const Ex>;

// where a variable lives
// `captured`: a variable of an enclosing function, found through the closure (index into Fn::captures). `self`: the closure itself
struct Ref { bool global = false; int index = 0; bool captured = false; bool self = false; };

enum class UnOp { Neg, Not, BitNot };
// Shl and Shr are only made for a count written as a number from 0 to 63, which cannot fail; any other count is a Raising op
enum class BinOp { Add, Sub, Mul, Lt, Le, Gt, Ge, Eq, Ne, BitAnd, BitOr, BitXor, AndAlso, OrElse, Shl, Shr };

struct Ex {
    enum class Kind { ConstInt, ConstFloat, ConstStr, Load, Un, Bin, ToFloat, Len, AddrOf } kind = Kind::ConstInt;
    Ty          ty = Ty::Int;     // what it gives
    int64_t     i = 0;            // ConstInt, or the index into Module::strings for ConstStr
    double      f = 0;
    Ref         ref;              // Load, AddrOf (the address of the variable, for an in-out argument)
    UnOp        uop = UnOp::Neg;
    BinOp       bop = BinOp::Add;
    ExP         a, b;             // operands. for a comparison both have the same type (the lowering put a ToFloat where one was int)
};

// the operations that can raise: they read two pure operands and write a variable
enum class RaiseOp { DivF, ModI, ModF, Shl, Shr };

// operations on strings and arrays (runtime calls). operands are borrowed, a result is a new reference that the destination takes
// over. `inplace`: the first operand is the destination variable itself and the call consumes it (so a string or array that nobody
// else holds grows without a copy)
enum class NativeOp {
    StrConcat,    // a + b
    StrAppend,    // dst = dst + a            (inplace)
    StrAppendSlice, // dst = dst + slice(a0, a1, a2)   (inplace), the slice is not made as a string of its own
    StrEq,        // a == b, an int
    StrIndex,     // a[i]           can raise
    StrOfInt, StrOfFloat,
    Join,         // join(array of str, sep)
    ArrNew,       // empty array of the destination's type
    ArrPush,      // push(a, x)
    ArrGet,       // a[i]           can raise
    ArrSet,       // dst[i] = x     can raise, inplace (makes dst unique first)
    ChildAny,     // dst (a slotref) = the slot of a[i] or a["k"] of an array or a map of anys, or of the any a0 holding one (made unique, and turned into a container of anys). can raise
    AnySet,       // dst[a1] = a2, dst an any holding an array or a map (it is made unique first). can raise, inplace
    AnyPush,      // push(dst, a1), dst an any holding an array. can raise, inplace
    AnyToArr,     // dst = the array in the any a0 as an array of the type written in `cname` (i f s a, A<elem>, M<elem>). can raise
    AnyToMap,     // the same for a map
    SortCmp,      // dst = sort_cmp(a, f, descending): a new array, ordered by calling f(x, y) (a number, negative: x first). can raise
    Own,          // dst = a value nobody else holds (the start of changing a nested container in place), inplace
    ChildArr,     // dst = the unique array at a[i] or a["k"] (a borrowed pointer into the container a, which is already unique). can raise
    ChildMap,     // the same for a map
    MapNew,       // empty map of the destination's type
    MapGet,       // m["k"]         can raise (key not found)
    MapSet,       // dst["k"] = x   inplace
    MapHas,       // has(m, "k"), an int
    MapKeys,      // keys(m), sorted
    MapValues,    // values(m), in the order of the sorted keys
    Raise,        // error(text)    can raise (always does)
    ErrMessage, ErrKind, ErrLine,   // e["message"], e["kind"], e["line"]
    Plugin,       // a function of a C plugin the file imports: `fn` is the plugin (Module::plugins), `cname` the function
    Rt,           // a function of the runtime library (rt_nat.c or libm) named in `cname`: the natives of embrlib, str and embrmath
};
inline bool nativeRaises(NativeOp o) {
    return o == NativeOp::StrIndex || o == NativeOp::ArrGet || o == NativeOp::ArrSet || o == NativeOp::MapGet || o == NativeOp::Raise ||
           o == NativeOp::ChildArr || o == NativeOp::ChildMap || o == NativeOp::SortCmp ||
           o == NativeOp::ChildAny || o == NativeOp::AnySet || o == NativeOp::AnyPush || o == NativeOp::AnyToArr || o == NativeOp::AnyToMap;
}

// a piece of an inline C block: text as written, or the variable `$name` stood for
struct CSeg { std::string text; bool isVar = false; Ref ref; };

struct St {
    enum class Kind { Set, Call, CallValue, MakeClosure, NewCell, Raising, Native, If, Loop, Try, Break, Continue, Return, Print, InlineC } kind = Kind::Set;
    int         line = 0;
    // Set: dst = a        Raising: dst = op(a, b)        Call: dst = fn(args)        Print: print(a)        Return: return a (a may be null)
    // CallValue: dst = a(args)        MakeClosure: dst = a closure of function `fn` that holds the cells of its captures
    // NewCell: the variable dst gets a new cell (a new variable, for a closure made in a loop)
    // If: if (a) then else els        Loop: for (;;) body        Try: body, and `els` is the catch block with the error in dst
    bool        hasDst = false;
    Ref         dst;
    ExP         a, b;
    RaiseOp     rop = RaiseOp::DivF;
    NativeOp    nop = NativeOp::StrConcat;
    bool        inplace = false;
    std::string cname;            // Native Rt: the C function. with `inplace`, args[0] is the array or map variable it changes
    bool        raises = false;   // Native Rt: takes the context and the line, and can raise
    int         fn = -1;          // Call: index into Module::fns
    int         file = 0;         // Call, CallValue: which source file the call is written in (index into Module::files, 0 is the program's own)
    std::vector<ExP> args;
    std::vector<CSeg> cseg;       // InlineC: the C text, in pieces
    std::string cfile;            // InlineC: the file the text was read from by `const c import` (its line 1 is the first line of the text), else empty
    std::vector<St> then, els, body;
};

// a variable of an enclosing function that a closure uses. `from` is where the function that makes the closure finds its cell
struct Capture { std::string name; Ty ty = Ty::Int; Ref from; };

struct Fn {
    int              id = 0;      // the index in Module::fns, in the C name
    std::string      name;
    bool             isClosure = false;       // written inside another function (or `fn(...)` as a value): takes the closure first
    std::vector<ExP> defaults;                // per parameter: the value an optional one has when the caller leaves it out, else null
    bool             variadic = false;        // the last parameter collects the arguments that are left (for a plugin function, the host passes them one by one)
    bool             internal = false;        // written in an imported script module: not part of a plugin's interface
    bool             builtin = false;         // written in the library std/*.embr: no frame in a stack trace, and not an error when nothing calls it
    bool             resultHost = false;      // `-> host`: the result is written into the host's slot by C text ($out); the function gives no value
    bool             tmpl = false;            // one version of a `const fn`: written as inline, and it has no frame in a stack trace, as in the VM
    bool             asValue = false;         // a named function used as a value: the C code gets a wrapper that takes a closure
    int              sig = -1;                // the function's own signature, for a closure or a function used as a value
    std::vector<Capture> captures;
    int              line = 0;
    std::vector<Var> vars;        // the parameters first, then the locals and temporaries
    int              nparams = 0;
    Ty               ret = Ty::Void;
    std::vector<St>  body;
    bool             recursive = false;  // can reach itself
    bool             canRaise = false;   // takes the error context and checks it (found by markRaisers)
};

// a C plugin the file imports and calls (its description was read when the file was compiled)
struct PluginUse { std::string name, path; };

// `c file """ ... """`: text for the whole C file. `file` is the file `const c file import` read it from (its line 1 is the first line of the text), else empty
struct CFile { std::string text, file; int line = 0; };

struct Module {
    std::string      file;        // the source path as given, shown in a stack trace
    std::vector<std::string> files;   // the script modules the program imports (index 1 on; index 0 stands for `file`), for the stack trace
    std::vector<Var> globals;
    std::vector<std::string> strings;   // the string literals
    std::vector<PluginUse>   plugins;
    std::vector<CFile>       fileC;       // `c file` text: C for the whole file, put before the functions
    std::string      scriptDir;   // the folder of the source file (absolute), what a plugin gets from embrp_script_dir
    std::vector<FnSig> sigs;
    bool             fatalOverflow = false;   // a stack overflow ends the program on the spot (no error to catch, no trace), so a function that raises only that does not check anything
    int              nTop = 0;    // the first nTop functions are the file's own top level functions, what comes after are closures
    std::deque<Fn>   fns;         // a deque: a closure is added while the function around it is still being written
    Fn               main;        // the top level statements
};

// sets Fn::canRaise: a function can raise when it has a raising operation, calls one that can, or can reach itself (a stack
// overflow). main always can
void markRaisers(Module& m);

// sets Var::borrowedParam on the heap parameters that need no count of their own
void markBorrowedParams(Module& m);

std::string printIr(const Module& m);

}  // namespace aembr::ir

#endif
