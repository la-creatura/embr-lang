// embrlib.cpp
// the stock library for embr: containers, functional helpers, control flow as functions, file io, module loading
//
// usage
//   import "embrlib"
//   print(len([1, 2, 3]))            # 3
//   print(sort([3, 1, 2]))           # [1, 2, 3]
//   print(join(["a", "b"], "-"))     # a-b
//
// can be used two ways:
//   1. linked directly: #include "embrlib.cpp" and call embr::registerStdlib(interp, runner)
//   2. dynamic plugin:  import "embrlib"  (calls embr_register via dlopen)

#include <embr/embr.h>
#include "../vec/vec.h"
#include <climits>

using namespace embr;

static Param pAny (std::string n)                      { return Param::req(std::move(n), TS::Any); }
// an argument that has to fit what the array given as argument number `of` holds (arr<int> takes ints), see Param::elemOf
static Param pElem(std::string n, int of)              { Param p = Param::req(std::move(n), TS::Any); p.elemOf = of; return p; }
static Param pNum (std::string n)                      { return Param::req(std::move(n), TS::Num); }
static Param pStr (std::string n)                      { return Param::req(std::move(n), TS::Str); }
static Param pArr (std::string n)                      { return Param::req(std::move(n), TS::Arr); }
static Param pMap (std::string n)                      { return Param::req(std::move(n), TS::Map); }
static Param pOpt (std::string n, TypeSet m = TS::Any) { return Param::opt(std::move(n), m);       }
static Param pRest(std::string n, TypeSet m = TS::Any) { return Param::rest(std::move(n), m);      }

// like pFn, but also accepts a typed pointer. for a parameter that is just handed to embr::invoke() (which resolves
// a pointer's __<tag>_call dunder itself, see resolveDunder() in core/registry.h), widening the signature is all it
// needs. invoke() raises its own clear error for a pointer with no matching dunder
static Param pFnOrPtr(std::string n) { return Param::req(std::move(n), TS::Fn|TS::Ptr); }

// resolves v to itself if it's already Callable, or to the Callable behind its __<tag>_call dunder if v is a typed
// pointer that opts into being called. shared by fn_arity/fn_name/fn_sig/is_native so all four treat anything callable
// the same way. sets *viaDunder so fn_arity can drop the dunder's own leading self parameter from the arity
// (a script calling `p(x, y)` never writes that argument)
static Value resolveIntrospectable(Interpreter& interp, const std::string& fn,
                                   const Value& v, bool& viaDunder) {
    viaDunder = false;
    if (v.isCallable()) return v;
    Value dfn;
    if (v.isPointer() && resolveDunder(interp, v, "call", dfn)) {
        viaDunder = true;
        return dfn;
    }
    raiseError(fn, "value is not callable and has no __<tag>_call (got " + v.typeName() +
               (v.isPointer() ? " tag '" + v.asPointer().type + "'" : "") + ")");
}

// stable merge sort driven by a "does a sort strictly before b" predicate. std::sort is undefined behavior (it can
// read out of bounds) when its comparator isn't a strict weak ordering, and every comparator here is script-controlled
// (a user cmp fn, a __<tag>_lt dunder, NaN weights). so anything sorting on script-supplied order goes through this:
// an inconsistent comparator just gives some order, never a crash. stability keeps equal elements in input order on both backends
template <typename T, typename Less>
static void stableSort(std::vector<T>& v, Less less) {
    if (v.size() < 2) return;
    std::vector<T> tmp(v.size());
    for (size_t width = 1; width < v.size(); width *= 2) {
        for (size_t lo = 0; lo < v.size(); lo += 2 * width) {
            size_t mid = std::min(lo + width, v.size());
            size_t hi  = std::min(lo + 2 * width, v.size());
            size_t i = lo, j = mid, k = lo;
            while (i < mid && j < hi)
                tmp[k++] = less(v[j], v[i]) ? std::move(v[j++]) : std::move(v[i++]);
            while (i < mid) tmp[k++] = std::move(v[i++]);
            while (j < hi)  tmp[k++] = std::move(v[j++]);
        }
        v.swap(tmp);
    }
}

// embrlib's file natives reach the real filesystem; a restricted sandbox (Interpreter::pluginPolicy.fileIo)
// turns them into errors instead of silently letting a "pure" plugin read or write anything
static void requireFileIo(const Interpreter& interp, const char* fn) {
    if (!interp.pluginPolicy.fileIo)
        raiseError(fn, "file access is disabled in this sandbox");
}

// script number -> int index without the UB of a raw (int) cast on NaN/Inf/huge
static int idxArg(const Value& v) {
    double d = v.asNumber();
    if (!(d > -2147483648.0)) return INT_MIN;   // also catches NaN
    if (d >= 2147483647.0)    return INT_MAX;
    return (int)d;
}

// type, str and len are called in loops, so they are typed natives (see Interpreter::bindTyped): no std::function and no argument vector
static Value lib_type(Interpreter& interp, const Value& v) {
    if (v.isPointer()) {
        Value dfn;
        if (resolveDunder(interp, v, "type", dfn))
            return invoke(interp, dfn, {v});
    }
    return Value(v.typeName());
}

static Value lib_str(Interpreter& interp, const Value& v) {
    if (v.isPointer()) {
        Value dfn;
        if (resolveDunder(interp, v, "str", dfn))
            return invoke(interp, dfn, {v});
    }
    return Value(v.formatAsString());
}

static Value lib_len(Interpreter& interp, const Value& v) {
    if (v.isString()) return Value((int64_t)v.asString().size());
    if (v.isArray())  return Value((int64_t)v.arraySize());   // a range that has not been built stays unbuilt
    if (v.isMap())    return Value((int64_t)v.asMap().size());
    if (v.isPointer()) {
        Value dfn;
        if (resolveDunder(interp, v, "len", dfn))
            return invoke(interp, dfn, {v});
    }
    raiseError("len", "no __<tag>_len defined and val is not a str/arr/map (got " +
               v.typeName() +
               (v.isPointer() ? " tag '" + v.asPointer().type + "'" : "") + ")");
}

void registerEmbrLib(Interpreter& interp) {
    // input(prompt?: str) -> str
    interp.bindSpan("input", {pOpt("prompt", TS::Str)},
    [](const ArgSpan& args) -> Value {
        if (!args.empty()) std::cout << args[0].asString() << std::flush;
        std::string line;
        std::getline(std::cin, line);
        return Value(line);
    });


    // assert(cond, msg?: str)
    interp.bindSpan("assert", {pAny("cond"), pOpt("msg", TS::Str)},
    [](const ArgSpan& args) -> Value {
        if (!args[0].truthy()) {
            std::string msg = args.size() > 1 ? args[1].asString() : "assertion failed";
            raiseError("assert", msg);
        }
        return Value(0.0);
    });

    // error(msg: str) -> never returns normally
    // raises a script error carrying msg
    // catchable from script with try ... catch e ... end
    // the return type is never actually used
    // it exists so error() type-checks as an ordinary callable expression
    interp.bindSpan("error", {pStr("msg")},
    [](const ArgSpan& args) -> Value {
        raiseError("error", args[0].asString());
        return Value(0.0);
    });



    // type(val) -> str
    //
    // a typed pointer whose tag has a __<tag>_type global gets that called instead of the generic "pointer" name, so a
    // plugin can report a more specific type for its own values (like str()/len()/has() use __<tag>_str/__<tag>_len/
    // __<tag>_has). everything else uses the ordinary typeName(). like str(), type() never has to raise
    interp.bindTyped("type", {"val"}, lib_type);

    // num(val: str|num) -> num
    interp.bindSpan("num", {Param::req("val", TS::Num|TS::Str)},
    [](const ArgSpan& args) -> Value {
        if (args[0].isNumeric()) return args[0];
        const auto& s = args[0].asString();
        double d;
        auto [ptr, ec] = parse_double(s.data(), s.data() + s.size(), d);
        if (ec != std::errc()) raiseError("num", "cannot convert to number: " + s);
        return Value(d);
    });

    // str(val: any) -> str
    //
    // a typed pointer whose tag has a __<tag>_str global gets that called instead of the generic <TAG 0xADDR> form, so a
    // plugin can give its values a meaningful string (a table's contents, say), like Python's __str__. everything else
    // uses formatAsString(). unlike len()/has() below, str() never has to raise
    interp.bindTyped("str", {"val"}, lib_str);


    // immmut container operations

    // len(val: str|arr|map, or a typed pointer with a __<tag>_len) -> num
    interp.bindTyped("len", {"val"}, lib_len);

    // push(arr: arr|vec, val) -> arr. on a vec this appends in place and returns the same vec, so `v = push(v, x)` is O(1)
    interp.bindSpan("push", {Param::req("arr", TS::Arr|TS::Ptr), pElem("val", 0)},
    [](const ArgSpan& args) -> Value {
        if (args[0].isPointer()) {
            embrvec::Vec* v = embrvec::vecOf(args[0]);
            if (!v) raiseError("push", "expected an array or a vec, got ptr with tag '" + args[0].asPointer().type + "'");
            checkAllocSize("push", "vec length", (int64_t)v->items.size() + 1);
            v->items.push_back(args[1]);
            return args[0];
        }
        auto arr = args[0].asArray();
        arr.push_back(args[1]);
        return Value(std::move(arr));
    });

    // pop(arr: arr|vec) -> arr, without the last element. on a vec it removes it in place and returns the same vec
    // (vec_pop gives you the element instead)
    interp.bindSpan("pop", {Param::req("arr", TS::Arr|TS::Ptr)},
    [](const ArgSpan& args) -> Value {
        if (args[0].isPointer()) {
            embrvec::Vec* v = embrvec::vecOf(args[0]);
            if (!v) raiseError("pop", "expected an array or a vec, got ptr with tag '" + args[0].asPointer().type + "'");
            if (v->items.empty()) raiseError("pop", "cannot pop empty array");
            v->items.pop_back();
            return args[0];
        }
        auto arr = args[0].asArray();
        if (arr.empty()) raiseError("pop", "cannot pop empty array");
        arr.pop_back();
        return Value(std::move(arr));
    });

    // the in-place natives below change the variable they are given, so they take it as &name (see "in-out parameters" on the
    // wiki architecture page). push and pop above still return a new array and leave the original alone

    // append(&arr, val) -> the new length
    interp.bindSpan("append", {Param::io("arr", TS::Arr), pElem("val", 0)},
    [](const ArgSpan& args) -> Value {
        Value& a = inoutTarget(args[0], args[1].nestDepth);
        a.arrayAppend(args[1]);
        return Value((int64_t)a.asArray().size());
    });

    // insert(&arr, pos, val) -> the new length. pos can be the length, which appends
    interp.bindSpan("insert", {Param::io("arr", TS::Arr), pNum("pos"), pElem("val", 0)},
    [](const ArgSpan& args) -> Value {
        Value& a = inoutTarget(args[0], args[2].nestDepth);
        int pos = idxArg(args[1]);
        if (pos < 0 || pos > (int)a.asArray().size())
            raiseError("insert", "position " + std::to_string(pos) + " is out of bounds for an array of length " +
                       std::to_string(a.asArray().size()));
        a.arrayInsert((size_t)pos, args[2]);
        return Value((int64_t)a.asArray().size());
    });

    // remove_at(&arr, pos) -> the element that was there
    interp.bindSpan("remove_at", {Param::io("arr", TS::Arr), pNum("pos")},
    [](const ArgSpan& args) -> Value {
        Value& a = inoutTarget(args[0]);
        int pos = idxArg(args[1]);
        if (pos < 0 || pos >= (int)a.asArray().size())
            raiseError("remove_at", "position " + std::to_string(pos) + " is out of bounds for an array of length " +
                       std::to_string(a.asArray().size()));
        return a.arrayRemoveAt((size_t)pos);
    });

    // remove_last(&arr) -> the element that was last
    interp.bindSpan("remove_last", {Param::io("arr", TS::Arr)},
    [](const ArgSpan& args) -> Value {
        Value& a = inoutTarget(args[0]);
        if (a.asArray().empty()) raiseError("remove_last", "cannot remove from an empty array");
        return a.arrayRemoveAt(a.asArray().size() - 1);
    });

    // remove_key(&m, key) -> 1 if the key was there and is gone now, 0 if it was never there
    interp.bindSpan("remove_key", {Param::io("m", TS::Map), pStr("key")},
    [](const ArgSpan& args) -> Value {
        Value& m = inoutTarget(args[0]);
        return Value(m.mapRemove(args[1].asString()));
    });

    // keys(m: map) -> arr (alphabetically sorted, so iteration order is deterministic
    // across runs, the underlying map_type is an unordered_map)
    interp.bindTyped("keys", {"m"},
    +[](const Value::map_type& m) -> Value {
        Value::array_type out;
        for (const auto& k : sortedMapKeys(m)) out.push_back(Value(k));
        return Value(std::move(out));
    });

    // values(m: map) -> arr (ordered to match keys()'s alphabetical order)
    interp.bindTyped("values", {"m"},
    +[](const Value::map_type& m) -> Value {
        Value::array_type out;
        for (const auto& k : sortedMapKeys(m)) out.push_back(m.at(k));
        return Value(std::move(out));
    });

    // has(container: arr|map, key) -> num
    // a typed pointer with a __<tag>_has global is dispatched to as
    // __<tag>_has(container, key) instead of raising.
    interp.bindSpan("has", {Param::req("container", TS::Arr|TS::Map|TS::Ptr), pAny("key")},
    [&interp](const ArgSpan& args) -> Value {
        if (args[0].isMap()) {
            if (!args[1].isString()) raiseError("has", "map key must be string");
            return Value(args[0].asMap().count(args[1].asString()) ? (int64_t)1 : (int64_t)0);
        }
        if (args[0].isPointer()) {
            Value dfn;
            if (resolveDunder(interp, args[0], "has", dfn))
                return invoke(interp, dfn, {args[0], args[1]});
            raiseError("has", "no __<tag>_has defined for tag '" + args[0].asPointer().type + "'");
        }
        // array: check index in range
        int64_t ii = numToInt64(args[1], "has", "index");
        return Value(ii >= 0 && ii < (int64_t)args[0].asArray().size() ? (int64_t)1 : (int64_t)0);
    });

    // slice(container: arr|str, start: num, end?: num) -> arr
    interp.bindSpan("slice", {Param::req("container", TS::Arr|TS::Str|TS::Map), pNum("start"), pOpt("end", TS::Num)},
    [](const ArgSpan& args) -> Value {
        if (args[0].isArray()) {
            const auto& arr = args[0].asArray();
            int start = idxArg(args[1]);
            int end   = args.size() >= 3 ? idxArg(args[2]) : (int)arr.size();
            start = std::max(0,     std::min(start, (int)arr.size()));
            end   = std::max(start, std::min(end,   (int)arr.size()));
            return Value(Value::array_type(arr.begin() + start, arr.begin() + end));
        } else if (args[0].isMap()) {
            // slices by alphabetical key order (see sortedMapKeys), matching keys()/values()
            const auto& m = args[0].asMap();
            std::vector<std::string> keys = sortedMapKeys(m);
            int start = idxArg(args[1]);
            int end   = args.size() >= 3 ? idxArg(args[2]) : (int)keys.size();
            start = std::max(0,     std::min(start, (int)keys.size()));
            end   = std::max(start, std::min(end,   (int)keys.size()));
            Value::map_type out;
            for (int i = start; i < end; ++i) out[keys[i]] = m.at(keys[i]);
            return Value(std::move(out));
        } else {
            const auto& s = args[0].asString();
            int start = idxArg(args[1]);
            int end   = args.size() >= 3 ? idxArg(args[2]) : (int)s.size();
            start = std::max(0,     std::min(start, (int)s.size()));
            end   = std::max(start, std::min(end,   (int)s.size()));
            return Value(s.substr((size_t)start, (size_t)(end - start)));
        }
    });

    // join(arr: arr, sep?: str) -> str
    interp.bindSpan("join", {pArr("arr"), pOpt("sep", TS::Str)},
    [](const ArgSpan& args) -> Value {
        std::string sep = args.size() > 1 ? args[1].asString() : "";
        std::string out;
        const auto& arr = args[0].asArray();
        for (size_t i = 0; i < arr.size(); ++i) {
            if (i) out += sep;
            out += arr[i].asString();
        }
        return Value(out);
    });



    // globals() -> map
    // (the returned map is unordered, so printing it in a stable order is formatAsString's job via sortedMapKeys)
    interp.bindSpan("globals", {},
    [&interp](const ArgSpan&) -> Value {
        Value::map_type m;
        for (const auto& [k, v] : interp.globals())
            m[k] = v;
        return Value(std::move(m));
    });

    // call(fn: fn|ptr, args?: arr) -> any
    // fn may be a typed pointer with a __<tag>_call dunder instead of a real Callable. invoke() resolves that
    // (see resolveDunder() in core/registry.h), so this only has to accept one
    interp.bindSpan("call", {pFnOrPtr("fn"), pOpt("args", TS::Arr)},
    [&interp](const ArgSpan& args) -> Value {
        std::vector<Value> fargs;
        if (args.size() >= 2) fargs = args[1].asArray();
        return invoke(interp, args[0], fargs);
    });

    // fn_name(fn: fn|ptr) -> str display name includes file:line for script fns
    // fn may also be a typed pointer with a __<tag>_call dunder, reports
    // the dunder function's own name (see resolveIntrospectable()).
    interp.bindSpan("fn_name", {pFnOrPtr("fn")},
    [&interp](const ArgSpan& args) -> Value {
        bool viaDunder;
        Value target = resolveIntrospectable(interp, "fn_name", args[0], viaDunder);
        const auto& c = target.asCallable();
        if (c.isScript() && !c.script.sourceFile.empty() && c.script.definedAt.valid()) {
            return Value(c.script.name + "@" + c.script.sourceFile +
                         ":" + std::to_string(c.script.definedAt.startLine));
        }
        return Value(c.name());
    });

    // fn_arity(fn: fn|ptr) -> num declared param count, -1 for native (open arity)
    // for a dunder-callable pointer it reports the __<tag>_call dunder's arity minus 1, because the leading self
    // parameter is added by invoke() and a script calling p(...) never counts it
    interp.bindSpan("fn_arity", {pFnOrPtr("fn")},
    [&interp](const ArgSpan& args) -> Value {
        bool viaDunder;
        Value target = resolveIntrospectable(interp, "fn_arity", args[0], viaDunder);
        const auto& c = target.asCallable();
        long n;
        if (c.isNative() && c.sig.empty()) {
            n = -1;
        } else if (c.isNative()) {
            // count non-variadic required params as the effective arity
            size_t cnt = 0;
            for (const auto& p : c.sig) { if (!p.optional && !p.variadic) ++cnt; }
            n = (long)cnt;
        } else {
            n = (long)c.script.params.size();
        }
        if (viaDunder && n > 0) --n;
        return Value((int64_t)n);
    });

    // fn_sig(fn: fn|ptr) -> str person-readable signature string
    // for a dunder-callable pointer it describes the __<tag>_call dunder itself, including its leading self parameter
    // (unlike fn_arity, this is a literal description of the underlying function)
    interp.bindSpan("fn_sig", {pFnOrPtr("fn")},
    [&interp](const ArgSpan& args) -> Value {
        bool viaDunder;
        Value target = resolveIntrospectable(interp, "fn_sig", args[0], viaDunder);
        const auto& c = target.asCallable();
        std::string s = c.name() + "(";
        const auto& params = c.isNative() ? c.sig : c.script.params;
        for (size_t i = 0; i < params.size(); ++i) {
            if (i) s += ", ";
            const auto& p = params[i];
            if (p.variadic) s += "...";
            s += p.name;
            if (!p.type.isAny()) s += ": " + p.type.name();
            if (p.optional && !p.variadic) s += "?";
        }
        s += ")";
        if (c.isScript() && !c.script.returnType.isAny())
            s += " -> " + c.script.returnType.name();
        return Value(s);
    });

    // is_native(fn: fn|ptr) -> num
    interp.bindSpan("is_native", {pFnOrPtr("fn")},
    [&interp](const ArgSpan& args) -> Value {
        bool viaDunder;
        return Value(resolveIntrospectable(interp, "is_native", args[0], viaDunder).asCallable().isNative() ? (int64_t)1 : (int64_t)0);
    });



    // if_do(cond, then_fn: fn|ptr, else_fn?: fn|ptr) -> any
    // inline if: calls then_fn() or else_fn() based on whether cond is truthy
    // then_fn/else_fn may be a typed pointer with a __<tag>_call dunder, invoke() resolves it
    interp.bindSpan("if_do", {pAny("cond"), pFnOrPtr("then_fn"), pOpt("else_fn", TS::Fn|TS::Ptr)},
    [&interp](const ArgSpan& args) -> Value {
        if (args[0].truthy()) return invoke(interp, args[1], {});
        if (args.size() >= 3) return invoke(interp, args[2], {});
        return Value(0.0);
    });

    // while_do(cond_fn: fn|ptr, body_fn: fn|ptr) -> 0
    // calls cond_fn() before each iteration and body_fn() while truthy
    interp.bindSpan("while_do", {pFnOrPtr("cond_fn"), pFnOrPtr("body_fn")},
    [&interp](const ArgSpan& args) -> Value {
        while (invoke(interp, args[0], {}).truthy()) invoke(interp, args[1], {});
        return Value(0.0);
    });

    // do(fn: fn|ptr) -> any
    interp.bindSpan("do", {pFnOrPtr("fn")},
    [&interp](const ArgSpan& args) -> Value {
        return invoke(interp, args[0], {});
    });



    // for_each_do(container: arr|map|str, fn: fn|ptr) -> arr|map
    interp.bindSpan("for_each_do", {Param::req("container", TS::Arr|TS::Map|TS::Str), pFnOrPtr("fn")},
    [&interp](const ArgSpan& args) -> Value {
        if (TS::Arr.contains(args[0].tag())) {
            Value::array_type out;
            for (const auto& el : args[0].asArray())
                out.push_back(invoke(interp, args[1], {el}));
            return Value(std::move(out));
        } else if (TS::Map.contains(args[0].tag())) {
            Value::map_type out;
            for (const auto& [k, v] : args[0].asMap())
                out[k] = invoke(interp, args[1], {k, v});
            return Value(std::move(out));
        } else {
            Value::array_type out;
            for (const auto& el : args[0].asString())
                out.push_back(invoke(interp, args[1], {Value(std::string(1, el))}));
            return Value(std::move(out));
        }
    });

    // filter(arr: arr, pred: fn|ptr) -> arr
    interp.bindSpan("filter", {pArr("arr"), pFnOrPtr("pred")},
    [&interp](const ArgSpan& args) -> Value {
        Value::array_type out;
        for (const auto& el : args[0].asArray())
            if (invoke(interp, args[1], {el}).truthy()) out.push_back(el);
        return Value(std::move(out));
    });

    // reduce(arr: arr, fn: fn|ptr, init) -> any
    interp.bindSpan("reduce", {pArr("arr"), pFnOrPtr("fn"), pAny("init")},
    [&interp](const ArgSpan& args) -> Value {
        Value acc = args[2];
        for (const auto& el : args[0].asArray())
            acc = invoke(interp, args[1], {acc, el});
        return acc;
    });

    // sort(arr: arr, descending?) -> arr
    // sorts a flat numeric array via std::sort (0 = ascend, 1 = descend)
    // if every element is a typed pointer with one shared tag that has a __<tag>_lt dunder, that dunder is the comparator
    // (sorted ascending, then reversed for descending, so no __<tag>_gt is needed)
    // raises an error if neither applies (mixed types, or no comparator)
    interp.bindSpan("sort", {pArr("arr"), pOpt("descending")},
    [&interp](const ArgSpan& args) -> Value {
        auto arr = args[0].asArray();
        bool desc = args.size() >= 2 && args[1].truthy();

        bool allNumeric = true;
        for (const auto& el : arr) {
            if (!el.isNumeric()) { allNumeric = false; break; }
        }

        if (allNumeric) {
            std::sort(arr.begin(), arr.end(),
                [desc](const Value& a, const Value& b) {
                    return desc ? a.asNumber() > b.asNumber()
                                : a.asNumber() < b.asNumber();
                });
            return Value(std::move(arr));
        }

        bool allTaggedPointers = !arr.empty();
        std::string tag;
        for (const auto& el : arr) {
            if (!el.isPointer() || el.asPointer().type.empty()) {
                allTaggedPointers = false;
                break;
            }
            if (tag.empty()) tag = el.asPointer().type;
            else if (el.asPointer().type != tag) { allTaggedPointers = false; break; }
        }

        Value ltFn;
        if (allTaggedPointers && resolveDunder(interp, arr[0], "lt", ltFn)) {
            stableSort(arr,
                [&interp, &ltFn](const Value& a, const Value& b) {
                    return invoke(interp, ltFn, {a, b}).truthy();
                });
            if (desc) std::reverse(arr.begin(), arr.end());
            return Value(std::move(arr));
        }

        raiseError("sort_",
            "array elements are not all numbers, and not all typed pointers "
            "sharing a tag with a __tag_lt dunder. "
            "use sort_do with a weight function for other arrays");
    });

    // sort_do(arr: arr, weight: fn|ptr, descending?) -> arr
    // sorts an array of any elements by a caller-supplied weight function
    // calls weight(elem) per elem, must return num (0 = ascending, 1 = descending)
    //
    //   users = [{"name":"Alex","followers":123}, ...]
    //   sort_do(users, fn(u) return u["followers"] end, 0)
    interp.bindSpan("sort_do", {pArr("arr"), pFnOrPtr("weight"), pOpt("descending")},
    [&interp](const ArgSpan& args) -> Value {
        const auto& arr = args[0].asArray();
        bool desc = args.size() >= 3 && args[2].truthy();

        // pre-compute weights. one interpreter call per element, not per comparison
        std::vector<std::pair<double, Value>> weighted;
        weighted.reserve(arr.size());

        for (size_t i = 0; i < arr.size(); ++i) {
            Value w = invoke(interp, args[1], {arr[i]});
            if (!w.isNumeric())
                raiseError("sort_do",
                    "weight function returned " + w.typeName() +
                    " for element at index " + std::to_string(i) +
                    " weight must return a number");
            weighted.emplace_back(w.asNumber(), arr[i]);
        }

        // NaN weights would break std::sort's ordering contract; stableSort
        // tolerates them (they just compare as neither less nor greater)
        stableSort(weighted,
            [desc](const std::pair<double, Value>& a,
                   const std::pair<double, Value>& b) {
                return desc ? a.first > b.first
                            : a.first < b.first;
            });

        Value::array_type out;
        out.reserve(weighted.size());
        for (auto& [_, v] : weighted) out.push_back(std::move(v));

        return Value(std::move(out));
    });

    // sort_cmp(arr: arr, cmp: fn|ptr, descending?) -> arr
    // stable sort driven by a comparator: cmp(a, b) returns a number, negative
    // if a sorts before b, positive if after, 0 if equivalent. use it when a
    // single numeric weight (sort_do) can't express the order, e.g. by one key
    // then another. an inconsistent comparator gives an unspecified order but
    // never crashes.
    //
    //   sort_cmp(words, fn(a, b) return len(a) - len(b) end)
    interp.bindSpan("sort_cmp", {pArr("arr"), pFnOrPtr("cmp"), pOpt("descending")},
    [&interp](const ArgSpan& args) -> Value {
        Value::array_type arr = args[0].asArray();
        bool desc = args.size() >= 3 && args[2].truthy();
        stableSort(arr, [&](const Value& a, const Value& b) {
            Value r = invoke(interp, args[1], desc ? std::vector<Value>{b, a}
                                                   : std::vector<Value>{a, b});
            if (!r.isNumeric())
                raiseError("sort_cmp", "comparator returned " + r.typeName() +
                           ", expected a number");
            return r.asNumber() < 0;
        });
        return Value(std::move(arr));
    });

    // zip(a: arr, b: arr, ...more: arr) -> arr of arr
    // pairs up elements by index; stops at the shortest input
    //
    //   zip([1,2,3], ["a","b"])   # [[1,"a"],[2,"b"]]
    interp.bindSpan("zip", {pArr("a"), pArr("b"), pRest("more", TS::Arr)},
    [](const ArgSpan& args) -> Value {
        size_t n = args[0].asArray().size();
        for (const auto& a : args) n = std::min(n, a.asArray().size());
        Value::array_type out;
        out.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            Value::array_type row;
            row.reserve(args.size());
            for (const auto& a : args) row.push_back(a.asArray()[i]);
            out.push_back(Value(std::move(row)));
        }
        return Value(std::move(out));
    });

    // enumerate(arr: arr, start?: num) -> arr of [index, element]
    //
    //   enumerate(["a","b"])      # [[0,"a"],[1,"b"]]
    //   enumerate(["a","b"], 1)   # [[1,"a"],[2,"b"]]
    interp.bindSpan("enumerate", {pArr("arr"), pOpt("start", TS::Num)},
    [](const ArgSpan& args) -> Value {
        int64_t start = 0;
        if (args.size() >= 2) {
            if (!args[1].isInt()) raiseError("enumerate", "start must be an integer");
            start = args[1].asInt();
        }
        Value::array_type out;
        const auto& arr = args[0].asArray();
        out.reserve(arr.size());
        for (size_t i = 0; i < arr.size(); ++i)
            out.push_back(Value(Value::array_type{Value(start + (int64_t)i), arr[i]}));
        return Value(std::move(out));
    });

    // concat(a: arr|str, b: arr|str, ...more) -> arr|str
    // joins arrays into a new array, or strings into a new string; every
    // argument must be the same kind as the first
    //
    //   concat([1,2], [3], [4,5])   # [1,2,3,4,5]
    //   concat("ab", "cd")          # "abcd"
    interp.bindSpan("concat",
        {Param::req("a", TS::Arr|TS::Str), Param::req("b", TS::Arr|TS::Str),
         pRest("more", TS::Arr|TS::Str)},
    [](const ArgSpan& args) -> Value {
        bool str = args[0].isString();
        for (size_t i = 1; i < args.size(); ++i)
            if (args[i].isString() != str)
                raiseError("concat", "cannot mix " + args[0].typeName() + " and " +
                           args[i].typeName() + " (argument " + std::to_string(i) + ")");
        if (str) {
            std::string out;
            for (const auto& a : args) out += a.asString();
            return Value(std::move(out));
        }
        Value::array_type out;
        for (const auto& a : args)
            out.insert(out.end(), a.asArray().begin(), a.asArray().end());
        return Value(std::move(out));
    });

    // items(m: map) -> arr of [key, value]
    // same (alphabetical) key order as keys()/values()
    interp.bindSpan("items", {pMap("m")},
    [](const ArgSpan& args) -> Value {
        const auto& m = args[0].asMap();
        std::vector<const Value::map_type::value_type*> entries;
        entries.reserve(m.size());
        for (const auto& kv : m) entries.push_back(&kv);
        std::sort(entries.begin(), entries.end(),
                  [](auto* a, auto* b) { return a->first < b->first; });
        Value::array_type out;
        out.reserve(entries.size());
        for (auto* kv : entries)
            out.push_back(Value(Value::array_type{Value(kv->first), kv->second}));
        return Value(std::move(out));
    });

    // min(arr: arr) / max(arr: arr) -> num
    // numeric arrays only; raises on an empty array or a non-number element.
    // keeps the element's own int/float type.
    auto extremum = [](const char* fname, bool wantMax) {
        return [fname, wantMax](const ArgSpan& args) -> Value {
            const auto& arr = args[0].asArray();
            if (arr.empty()) raiseError(fname, "empty array has no " + std::string(wantMax ? "max" : "min"));
            const Value* best = nullptr;
            for (size_t i = 0; i < arr.size(); ++i) {
                if (!arr[i].isNumeric())
                    raiseError(fname, "element " + std::to_string(i) + " is " +
                               arr[i].typeName() + ", expected a number");
                if (!best || (wantMax ? arr[i].asNumber() > best->asNumber()
                                      : arr[i].asNumber() < best->asNumber()))
                    best = &arr[i];
            }
            return *best;
        };
    };
    interp.bindSpan("min", {pArr("arr")}, extremum("min", false));
    interp.bindSpan("max", {pArr("arr")}, extremum("max", true));

    // sum(arr: arr) -> num
    // exact int64 sum while every element is an int and nothing overflows;
    // falls back to a double sum otherwise. empty array -> 0.
    interp.bindTyped("sum", {"arr"},
    +[](const Value::array_type& arr) -> Value {
        int64_t  isum = 0;
        double   dsum = 0.0;
        bool     exact = true;
        for (size_t i = 0; i < arr.size(); ++i) {
            if (!arr[i].isNumeric())
                raiseError("sum", "element " + std::to_string(i) + " is " +
                           arr[i].typeName() + ", expected a number");
            dsum += arr[i].asNumber();
            if (exact) {
                if (!arr[i].isInt() || __builtin_add_overflow(isum, arr[i].asInt(), &isum))
                    exact = false;
            }
        }
        return exact ? Value(isum) : Value(dsum);
    });

    // ---- predicate / reshaping helpers ----------------------------------------------------------
    // convention: a helper that takes a script function is named *_do (like for_each_do, sort_do); every
    // callback goes through embr::invoke so both backends and __call pointers work. embr has no nil, so
    // "not found" is an error, a default argument, or -1, never a missing value.

    // strict equality for unique()/index_of(): same core type AND ==, so 5 and "5" (equal under the
    // loose cross-type == fallback) and 5 and 5.0 stay distinct
    auto sameValue = [](const Value& a, const Value& b) { return a.tag() == b.tag() && a == b; };

    // find_do(arr, pred, default?) -> the first element for which pred(el) is truthy; `default` if none
    // match and one was given, otherwise raises
    interp.bindSpan("find_do", {pArr("arr"), pFnOrPtr("pred"), pOpt("default")},
    [&interp](const ArgSpan& args) -> Value {
        for (const auto& el : args[0].asArray())
            if (invoke(interp, args[1], {el}).truthy()) return el;
        if (args.size() >= 3) return args[2];
        raiseError("find_do", "no element matches (pass a default to avoid this error)");
    });

    // find_index_do(arr, pred) -> index of the first match, or -1
    interp.bindSpan("find_index_do", {pArr("arr"), pFnOrPtr("pred")},
    [&interp](const ArgSpan& args) -> Value {
        const auto& a = args[0].asArray();
        for (size_t i = 0; i < a.size(); ++i)
            if (invoke(interp, args[1], {a[i]}).truthy()) return Value((int64_t)i);
        return Value((int64_t)-1);
    });

    // index_of(arr, value) -> index of the first element strictly equal to value, or -1
    interp.bindTyped("index_of", {"arr", "value"},
    +[](const Value::array_type& a, const Value& value) -> int64_t {
        for (size_t i = 0; i < a.size(); ++i)
            if (a[i].tag() == value.tag() && a[i] == value) return (int64_t)i;
        return -1;
    });

    // any_do(arr, pred) / all_do(arr, pred) -> 1/0, short-circuiting (all_do of [] is 1, any_do of [] is 0)
    interp.bindSpan("any_do", {pArr("arr"), pFnOrPtr("pred")},
    [&interp](const ArgSpan& args) -> Value {
        for (const auto& el : args[0].asArray())
            if (invoke(interp, args[1], {el}).truthy()) return Value((int64_t)1);
        return Value((int64_t)0);
    });
    interp.bindSpan("all_do", {pArr("arr"), pFnOrPtr("pred")},
    [&interp](const ArgSpan& args) -> Value {
        for (const auto& el : args[0].asArray())
            if (!invoke(interp, args[1], {el}).truthy()) return Value((int64_t)0);
        return Value((int64_t)1);
    });

    // count_do(arr, pred) -> how many elements satisfy pred
    interp.bindSpan("count_do", {pArr("arr"), pFnOrPtr("pred")},
    [&interp](const ArgSpan& args) -> Value {
        int64_t n = 0;
        for (const auto& el : args[0].asArray())
            if (invoke(interp, args[1], {el}).truthy()) ++n;
        return Value(n);
    });

    // partition(arr, pred) -> [matching, non_matching], each in the original order
    interp.bindSpan("partition", {pArr("arr"), pFnOrPtr("pred")},
    [&interp](const ArgSpan& args) -> Value {
        Value::array_type yes, no;
        for (const auto& el : args[0].asArray())
            (invoke(interp, args[1], {el}).truthy() ? yes : no).push_back(el);
        return Value(Value::array_type{Value(std::move(yes)), Value(std::move(no))});
    });

    // unique(arr) -> first occurrence of each distinct element, original order (strict equality: see
    // sameValue). bucketed by type + printed form first, so typical arrays are O(n), not O(n^2)
    interp.bindSpan("unique", {pArr("arr")},
    [sameValue](const ArgSpan& args) -> Value {
        Value::array_type out;
        std::unordered_map<std::string, std::vector<size_t>> seen;      // bucket key -> indices into out
        for (const auto& el : args[0].asArray()) {
            std::string key = std::to_string((int)el.tag()) + ":" + el.formatAsString();
            auto& bucket = seen[key];
            bool dup = false;
            for (size_t idx : bucket) if (sameValue(out[idx], el)) { dup = true; break; }
            if (dup) continue;
            bucket.push_back(out.size());
            out.push_back(el);
        }
        return Value(std::move(out));
    });

    // flatten(arr, depth?) -> arr with nested arrays spliced in, `depth` levels deep (default 1, max 100)
    interp.bindSpan("flatten", {pArr("arr"), pOpt("depth", TS::Num)},
    [](const ArgSpan& args) -> Value {
        int64_t depth = args.size() >= 2 ? numToInt64(args[1], "flatten", "depth") : 1;
        if (depth < 0 || depth > 100) raiseError("flatten", "depth must be between 0 and 100");
        std::function<void(const Value::array_type&, int64_t, Value::array_type&)> go =
            [&](const Value::array_type& in, int64_t d, Value::array_type& out) {
                for (const auto& el : in) {
                    if (d > 0 && el.isArray()) go(el.asArray(), d - 1, out);
                    else out.push_back(el);
                }
            };
        Value::array_type out;
        go(args[0].asArray(), depth, out);
        return Value(std::move(out));
    });

    // reverse(arr|str) -> a reversed copy (strings are reversed byte-wise; see unicode_chars for code points)
    interp.bindSpan("reverse", {Param::req("v", TS::Arr|TS::Str)},
    [](const ArgSpan& args) -> Value {
        if (args[0].isString()) { std::string s = args[0].asString(); std::reverse(s.begin(), s.end()); return Value(std::move(s)); }
        Value::array_type a = args[0].asArray();
        std::reverse(a.begin(), a.end());
        return Value(std::move(a));
    });

    // chunk(arr, n) -> arrays of up to n elements (the last may be shorter); n must be >= 1
    interp.bindSpan("chunk", {pArr("arr"), pNum("n")},
    [](const ArgSpan& args) -> Value {
        int64_t n = numToInt64(args[1], "chunk", "n");
        if (n < 1) raiseError("chunk", "n must be >= 1");
        const auto& a = args[0].asArray();
        Value::array_type out;
        for (size_t i = 0; i < a.size(); i += (size_t)n)
            out.push_back(Value(Value::array_type(a.begin() + i, a.begin() + std::min(a.size(), i + (size_t)n))));
        return Value(std::move(out));
    });

    // take(arr, n) / drop(arr, n) -> the first n elements / everything after them (n clamped to 0..len)
    interp.bindSpan("take", {pArr("arr"), pNum("n")},
    [](const ArgSpan& args) -> Value {
        const auto& a = args[0].asArray();
        int64_t n = std::max<int64_t>(0, std::min<int64_t>(numToInt64(args[1], "take", "n"), (int64_t)a.size()));
        return Value(Value::array_type(a.begin(), a.begin() + n));
    });
    interp.bindSpan("drop", {pArr("arr"), pNum("n")},
    [](const ArgSpan& args) -> Value {
        const auto& a = args[0].asArray();
        int64_t n = std::max<int64_t>(0, std::min<int64_t>(numToInt64(args[1], "drop", "n"), (int64_t)a.size()));
        return Value(Value::array_type(a.begin() + n, a.end()));
    });

    // group_by(arr, keyfn) -> map of key -> array of the elements with that key, in original order.
    // keyfn must return a string or a number (numbers become their printed form: embr map keys are strings)
    interp.bindSpan("group_by", {pArr("arr"), pFnOrPtr("keyfn")},
    [&interp](const ArgSpan& args) -> Value {
        std::unordered_map<std::string, Value::array_type> tmp;      // built as plain vectors, converted once
        for (const auto& el : args[0].asArray()) {
            Value k = invoke(interp, args[1], {el});
            if (!k.isString() && !k.isNumeric())
                raiseError("group_by", "keyfn must return a string or number, got " + k.typeName());
            tmp[k.isString() ? k.asString() : k.formatAsString()].push_back(el);
        }
        Value::map_type groups;
        for (auto& [key, vec] : tmp) groups.emplace(key, Value(std::move(vec)));
        return Value(std::move(groups));
    });

    // min_do(arr, weight) / max_do(arr, weight) -> the element with the smallest / largest weight(el)
    // (the first one on ties); weight must return a number; raises on an empty array
    auto extremeBy = [&interp](const char* fname, bool wantMax) {
        return [&interp, fname, wantMax](const ArgSpan& args) -> Value {
            const auto& a = args[0].asArray();
            if (a.empty()) raiseError(fname, "empty array");
            size_t best = 0; double bestW = 0;
            for (size_t i = 0; i < a.size(); ++i) {
                Value w = invoke(interp, args[1], {a[i]});
                if (!w.isNumeric())
                    raiseError(fname, "weight returned " + w.typeName() + " for element " + std::to_string(i) + ", expected a number");
                double d = w.asNumber();
                if (i == 0 || (wantMax ? d > bestW : d < bestW)) { best = i; bestW = d; }
            }
            return a[best];
        };
    };
    interp.bindSpan("min_do", {pArr("arr"), pFnOrPtr("weight")}, extremeBy("min_do", false));
    interp.bindSpan("max_do", {pArr("arr"), pFnOrPtr("weight")}, extremeBy("max_do", true));

    // load_file(path: str) -> str
    //
    // reads an arbitrary file and returns its entire contents as a string
    // the path is resolved relative to scriptDir, then cwd
    // intended as a lightweight stopgap until a proper IO plugin exists
    //
    //   contents = load_file("data/words.txt")
    //   contents = load_file("/etc/hostname")
    interp.bindSpan("load_file", {pStr("path")},
    [&interp](const ArgSpan& args) -> Value {
        requireFileIo(interp, "load_file");
        namespace fs = std::filesystem;
 
        fs::path p(args[0].asString());
 
        // resolve relative paths against scriptDir first, then cwd
        fs::path resolved;
        if (p.is_absolute()) {
            resolved = p;
        } else {
            if (!interp.scriptDir.empty()) {
                fs::path cand = (fs::path(interp.scriptDir) / p).lexically_normal();
                std::error_code ec;
                if (fs::exists(cand, ec) && !ec) resolved = cand;
            }
            if (resolved.empty()) {
                fs::path cand = (fs::current_path() / p).lexically_normal();
                std::error_code ec;
                if (fs::exists(cand, ec) && !ec) resolved = cand;
            }
            if (resolved.empty())
                resolved = p;  // let ifstream produce the OS error
        }
 
        std::ifstream f(resolved);
        if (!f)
            raiseError("load_file", "cannot open file: " + resolved.string());
 
        std::string contents((std::istreambuf_iterator<char>(f)), {});
        return Value(std::move(contents));
    });


    // write_file(path: str, contents: str) -> 0
    //
    // writes contents to path, overwriting any existing file
    // path resolution matches load_file.
    //
    //   write_file("out.txt", "hello\n")
    interp.bindSpan("write_file", {pStr("path"), pStr("contents")},
    [&interp](const ArgSpan& args) -> Value {
        requireFileIo(interp, "write_file");
        namespace fs = std::filesystem;

        fs::path p(args[0].asString());
        fs::path resolved = p.is_absolute()
            ? p
            : (!interp.scriptDir.empty() ? fs::path(interp.scriptDir) / p : fs::current_path() / p);

        std::ofstream f(resolved, std::ios::out | std::ios::trunc);
        if (!f)
            raiseError("write_file", "cannot open file for writing: " + resolved.string());

        f << args[1].asString();
        return Value(0.0);
    });

    // append_file(path: str, contents: str) -> 0
    //
    // appends contents to path, creating it if it doesn't exist.
    // path resolution matches write_file.
    //
    //   append_file("log.txt", "line\n")
    interp.bindSpan("append_file", {pStr("path"), pStr("contents")},
    [&interp](const ArgSpan& args) -> Value {
        requireFileIo(interp, "append_file");
        namespace fs = std::filesystem;

        fs::path p(args[0].asString());
        fs::path resolved = p.is_absolute()
            ? p
            : (!interp.scriptDir.empty() ? fs::path(interp.scriptDir) / p : fs::current_path() / p);

        std::ofstream f(resolved, std::ios::out | std::ios::app);
        if (!f)
            raiseError("append_file", "cannot open file for appending: " + resolved.string());

        f << args[1].asString();
        return Value(0.0);
    });

    // range(start: num, end: num, step?: num) -> arr
    interp.bindSpan("range", {pNum("start"), pNum("end"), pOpt("step", TS::Num)},
    [](const ArgSpan& args) -> Value {
        // if every argument was given as an int literal, produce int elements instead of silently downgrading to float
        bool asInt = args[0].isInt() && args[1].isInt() && (args.size() < 3 || args[2].isInt());

        double start = args[0].asNumber();
        double end   = args[1].asNumber();
        double step;
        if (args.size() >= 3) {
            step = args[2].asNumber();
            if (step == 0.0) raiseError("range", "step must not be 0");
        } else {
            step = (end < start) ? -1.0 : 1.0;
        }
        if ((step > 0 && start >= end) || (step < 0 && start <= end))
            return Value(Value::array_type{});
        if ((step > 0) != (end > start))
            raiseError("range", "step direction never reaches end");
        // whole numbers: the elements are not built. a `for` loop, len() and an element read work on the range as it is, and the list is
        // built only if something needs it (and is refused above Value's range limit)
        if (asInt) {
            const int64_t st = args[0].asInt(), en = args[1].asInt(), sp = args.size() >= 3 ? args[2].asInt() : (en < st ? -1 : 1);
            const uint64_t dm = en >= st ? (uint64_t)en - (uint64_t)st : (uint64_t)st - (uint64_t)en;
            const uint64_t sm = sp >= 0 ? (uint64_t)sp : (uint64_t)0 - (uint64_t)sp;
            const uint64_t count = dm / sm + (dm % sm != 0 ? 1 : 0);
            if (count > (uint64_t)INT64_MAX) raiseError("range", "range is too long");
            return Value::makeRange(st, sp, (int64_t)count);
        }
        // element count up front, so a huge span is an error instead of exhausting memory
        // (an array element is a ~48-byte Value, so the limit is far below the byte-oriented kMaxScriptAlloc)
        static constexpr double kMaxRangeElements = 10000000.0;
        double count = std::ceil((end - start) / step);
        if (!(count >= 0) || count > kMaxRangeElements)
            raiseError("range", "range would have " + formatNumber(count) + " elements (limit " +
                       formatNumber(kMaxRangeElements) + "); iterate with a while loop instead");

        Value::array_type out;
        auto emit = [&](double v) { out.push_back(asInt ? Value((int64_t)v) : Value(v)); };
        if (step > 0) for (double v = start; v < end; v += step) emit(v);
        else          for (double v = start; v > end; v += step) emit(v);
        return Value(std::move(out));
    });

    // map_merge(a: map, b: map) -> map
    //
    // shallow-merges two maps; keys in b win on conflict.
    //
    //   map_merge({"a":1}, {"a":2,"b":3})   # {"a":2,"b":3}
    interp.bindSpan("map_merge", {pMap("a"), pMap("b")},
    [](const ArgSpan& args) -> Value {
        Value::map_type out = args[0].asMap();
        for (const auto& [k, v] : args[1].asMap()) out[k] = v;
        return Value(std::move(out));
    });

    // map_pick(m: map, keys: arr) -> map
    //
    // projects m down to the given keys (keys not present in m are skipped).
    //
    //   map_pick({"a":1,"b":2,"c":3}, ["a","c"])   # {"a":1,"c":3}
    interp.bindSpan("map_pick", {pMap("m"), pArr("keys")},
    [](const ArgSpan& args) -> Value {
        const auto& m = args[0].asMap();
        Value::map_type out;
        for (const auto& k : args[1].asArray()) {
            if (!k.isString()) raiseError("map_pick", "keys must be strings");
            auto it = m.find(k.asString());
            if (it != m.end()) out[it->first] = it->second;
        }
        return Value(std::move(out));
    });

    // map_omit(m: map, keys: arr) -> map
    //
    // like map_pick but excludes the given keys instead of selecting them.
    //
    //   map_omit({"a":1,"b":2,"c":3}, ["b"])   # {"a":1,"c":3}
    interp.bindSpan("map_omit", {pMap("m"), pArr("keys")},
    [](const ArgSpan& args) -> Value {
        Value::map_type out = args[0].asMap();
        for (const auto& k : args[1].asArray()) {
            if (!k.isString()) raiseError("map_omit", "keys must be strings");
            out.erase(k.asString());
        }
        return Value(std::move(out));
    });

    // load_module(path: str, reload?: num) -> map
    //
    // loads and runs an embr script module and returns its non-local bindings as a map. unlike `import "foo.embr"` it
    // does NOT touch the caller's scope (see loadEmbrModule in core/invoke.h). later calls with the same resolved path do
    // nothing (returning {}) unless reload is truthy
    //
    //   utils = load_module("utils")        # loads utils.embr
    //   m     = load_module("lib/math")     # loads lib/math.embr relative to caller
    //   a     = load_module("/abs/path")    # absolute
    //   utils = load_module("utils", 1)     # force reload
    //
    // works under either backend
    interp.bindSpan("load_module",
        {pStr("path"), pOpt("reload")},
    [&interp](const ArgSpan& args) -> Value {
        const std::string& rawPath = args[0].asString();
        bool forceReload = args.size() >= 2 && args[1].truthy();
        return loadEmbrModule(interp, rawPath, {}, /*mutateCallerScope=*/false, forceReload);
    });

    // what each of these always returns (the static type checker reads this, and the VM refuses a native that breaks it)
    // left out: the ones whose result depends on a script function or a __<tag>_ operator (str, len on a pointer, has, call, reduce, find_do, min, max ...), which stay any
    interp.setResultTypes({
        {"input", TS::Str},     {"assert", TS::Float},     {"num", TS::Num},
        {"append", TS::Int},    {"insert", TS::Int},       {"remove_key", TS::Int},
        {"keys", TS::Arr},      {"values", TS::Arr},       {"slice", TS::Arr | TS::Str | TS::Map}, {"join", TS::Str}, {"globals", TS::Map},
        {"fn_name", TS::Str},   {"fn_arity", TS::Int},     {"fn_sig", TS::Str},    {"is_native", TS::Int},
        {"for_each_do", TS::Arr | TS::Map}, {"filter", TS::Arr},
        {"sort", TS::Arr},      {"sort_do", TS::Arr},      {"sort_cmp", TS::Arr},
        {"zip", TS::Arr},       {"enumerate", TS::Arr},    {"partition", TS::Arr}, {"unique", TS::Arr}, {"flatten", TS::Arr},
        {"chunk", TS::Arr},     {"take", TS::Arr},         {"drop", TS::Arr},      {"items", TS::Arr},  {"range", TS::Arr},
        {"concat", TS::Str | TS::Arr},                     {"reverse", TS::Str | TS::Arr}, {"group_by", TS::Map},
        {"count_do", TS::Int},  {"sum", TS::Num},          {"find_index_do", TS::Int},     {"index_of", TS::Int},
        {"any_do", TS::Int},  {"all_do", TS::Int},
        {"map_merge", TS::Map}, {"map_pick", TS::Map},     {"map_omit", TS::Map},
        {"load_file", TS::Str}, {"write_file", TS::Float}, {"append_file", TS::Float},
    });

    // the same answer for the same arguments and no side effects, so a const may call them while compiling
    interp.setPure({
        "type", "num", "str", "len", "has", "slice", "join", "min", "max", "sum", "index_of",
    });
    // what an array a native returns holds (arr<str>), and the natives that give back the container they were given, changed
    interp.setResultElems({{"keys", TS::Str}, {"range", TS::Num}});
    interp.setResultLike({{"push", 0}, {"reverse", 0}, {"slice", 0}, {"sort", 0}, {"sort_do", 0}, {"sort_cmp", 0}, {"filter", 0}, {"unique", 0}, {"take", 0}, {"drop", 0}});

} // registerEmbrlib

EMBR_PLUGIN {
    registerEmbrLib(*interp);
}

// what this plugin says about itself. embr reads it before it runs embr_register (see core/plugininfo.h)
EMBR_PLUGIN_INFO("embrlib", "1.0.0", "the batteries standard library: arrays, maps, sorting, higher-order helpers, files and modules", EMBR_CAP_NONE, EMBR_CAP_FILESYSTEM,
    {"all_do", "1/0, short-circuiting; any_do([]) is 0, all_do([]) is 1"},
    {"any_do", "1/0, short-circuiting; any_do([]) is 0, all_do([]) is 1"},
    {"append", "in place: adds val to the end of the variable and returns the new length. call it as append(&list, x)"},
    {"append_file", "appends contents to the end of the file (the file is created if it is not there)"},
    {"assert", "raises [\"assert\", msg or \"assertion failed\"] if falsy"},
    {"call", "invoke(interp, fn, args or [])"},
    {"chunk", "n >= 1; the last chunk may be shorter"},
    {"concat", "all arguments must be the same kind"},
    {"count_do", "original order kept"},
    {"do", "invoke(interp, fn, {})"},
    {"drop", "first n / all but the first n; n clamped to 0..len"},
    {"enumerate", "[[start, el0], [start+1, el1], ...]"},
    {"error", "always raises [\"error\", msg]"},
    {"filter", "the elements of the array for which pred(element) is truthy, in order"},
    {"find_do", "first element with truthy pred(el); default if none and one was given, else raises (no nil, so \"not found\" is never a missing value)"},
    {"find_index_do", "index of the first match, or -1; index_of uses *strict* equality (same core type and ==: 5, 5.0, \"5\" are all different)"},
    {"flatten", "splices nested arrays depth levels deep (default 1, 0..100)"},
    {"fn_arity", "native w/o sig: -1 (open arity)"},
    {"fn_name", "script: name@file:line; native: c.name()"},
    {"fn_sig", "human-readable signature incl. types"},
    {"for_each_do", "array: fn(el); map: fn(k,v) rebuilt into map; string: fn(char) → array"},
    {"group_by", "key -> array of elements, original order; keyfn must return a string or number (numbers become their printed form - map keys are strings)"},
    {"has", "map: string key + .count(); array: in-range int index; pointer: __<tag>_has(container, key)"},
    {"if_do", "calls then_fn when cond is truthy, else else_fn if there is one, and gives back what it returned; an if that can be passed around"},
    {"index_of", "index of the first match, or -1; index_of uses *strict* equality (same core type and ==: 5, 5.0, \"5\" are all different)"},
    {"input", "prints prompt if given, reads one stdin line"},
    {"insert", "in place: pos 0 to the length (the length appends), raises outside that"},
    {"is_native", "1 if fn is a native function (made in C or C++), 0 if it is a script function"},
    {"items", "[[key, value], ...] in keys() order"},
    {"join", "joins asString() of each element"},
    {"keys", "alphabetically sorted (deterministic despite unordered_map backing)"},
    {"len", "length of string/array/map; __<tag>_len(val) for a pointer, else raises"},
    {"load_file", "resolves against scriptDir then cwd"},
    {"load_module", "loads via loadEmbrModule(..., mutateCallerScope=false, ...); does not touch caller's scope; a no-op (returns {}) on an already-loaded path unless reload is truthy"},
    {"map_merge", "shallow merge, b wins on conflict"},
    {"map_omit", "inverse of map_pick"},
    {"map_pick", "project to given (string) keys"},
    {"max", "numeric arrays only; raises on empty; keeps int/float type"},
    {"max_do", "element with the smallest/largest weight(el) (first on ties); raises on [] or a non-number weight"},
    {"min", "numeric arrays only; raises on empty; keeps int/float type"},
    {"min_do", "element with the smallest/largest weight(el) (first on ties); raises on [] or a non-number weight"},
    {"num", "parses a string; raises on failure"},
    {"partition", "original order kept"},
    {"pop", "new array minus last element; raises on empty"},
    {"push", "returns a new array (not in place)"},
    {"range", "int elements iff all-int args, and then it is a lazy range like [a...b] (see syntax) with the end left out; raises on step==0 or a step that never reaches end"},
    {"reduce", "folds the array from the left: acc = fn(acc, element) for each element, starting with init; gives the final acc"},
    {"remove_at", "in place: removes and returns the element at pos"},
    {"remove_key", "in place: 1 if the key was there and is gone, 0 if it was never there"},
    {"remove_last", "in place: removes and returns the last element, raises on empty"},
    {"reverse", "copy; strings reverse byte-wise"},
    {"slice", "clamps bounds; map slices by alphabetical key order"},
    {"sort", "numeric only; raises with a hint to use sort_do otherwise"},
    {"sort_cmp", "stable merge sort; cmp(a, b) returns a number (negative = a first). Inconsistent comparators give an unspecified order but never crash; sort's __lt path and sort_do are stable for the same reason"},
    {"sort_do", "weight(el) computed once per element; raises if non-numeric"},
    {"str", "__<tag>_str(val) if defined for a pointer, else val.formatAsString()"},
    {"sum", "exact int64 while all ints and no overflow, else float; sum([]) is 0"},
    {"take", "first n / all but the first n; n clamped to 0..len"},
    {"type", "val.typeName() (not dunder-aware: this is a core-type question, not a plugin one)"},
    {"unique", "first occurrence of each distinct element, stable, strict equality, O(n) for typical arrays (bucketed by type + printed form)"},
    {"values", "same key order as keys()"},
    {"while_do", "re-evaluates cond_fn() each iteration"},
    {"write_file", "truncate+write"},
    {"zip", "array of index-aligned arrays, truncated to the shortest"});
