// vec.cpp
// a growable array that you change in place. embr's own arrays are values: every read, write or push copies the
// whole array, so building or updating a big one is slow. a vec is a handle, copying it shares the same elements.
//
// usage
//   import "vec"
//   v = vec()
//   vec_push(v, 10)
//   vec_push(v, 20)
//   vec_set(v, 0, 11)
//   print(vec_get(v, 0))      # 11
//   print(vec_len(v))         # 2
//   print(vec_to_array(v))    # [11, 20]
//
// good to know
//   - `b = a` makes b the same vec, not a copy. use vec_copy(a) for a separate one
//   - there is no v[i] syntax, use vec_get and vec_set. every call is O(1) except insert, remove, extend, slice, copy
//   - for loops only walk arrays and maps. use vec_each_do(v, fn) (fn gets the element and its index), or
//     `for x in vec_to_array(v)` when you want a copy to loop over
//   - len(v) and has(v, i) work, and embrlib's push(v, x) and pop(v) change a vec in place. nothing else in embrlib
//     takes a vec: those functions would only copy it, so say so with vec_to_array(v)
//   - json_stringify, csv_stringify and rand_choice read a vec where it is
//   - not synchronized, use a vec from one thread only

#include "vec.h"

using namespace embr;
using namespace embrvec;

static Param pPtr(std::string n) { return Param::req(std::move(n), TS::Ptr); }
static Param pAny(std::string n) { return Param::req(std::move(n), TS::Any); }
static Param pNum(std::string n) { return Param::req(std::move(n), TS::Num); }

static Vec* requireVec(const char* fn, const Value& v) {
    Vec* p = vecOf(v);
    if (!p) raiseError(fn, "expected a vec, got " + v.typeName());
    return p;
}

// index into 0..size-1, or raise
static size_t checkIndex(const char* fn, const Value& idx, size_t size) {
    int64_t i = numToInt64(idx, fn, "index");
    if (i < 0 || i >= (int64_t)size)
        raiseError(fn, "index " + std::to_string(i) + " out of bounds (length " + std::to_string(size) + ")");
    return (size_t)i;
}

static Value makeVec(Interpreter* interp, Value::array_type items) {
    Value h = Value::makeUserdata<Vec>(kTag);
    Vec* v = static_cast<Vec*>(h.asPointer().ptr);
    v->items = std::move(items);
    // empty it at interpreter teardown, so a vec that holds itself (or a closure over it) doesn't leak
    interp->addClearable(h.asPointer().owner(), [w = std::weak_ptr<Vec>(std::static_pointer_cast<Vec>(h.asPointer().owner()))] {
        if (auto p = w.lock()) p->items.clear();
    });
    // gc_collect() must see what the vec holds, or a closure stored in it looks unreachable
    h.withTracer([v](const TraceVisitor& visit) { for (const auto& x : v->items) visit(x); });
    // a C plugin reads a vec like an array (embrp_array_len / embrp_array_at)
    h.withArrayView(+[](void* p) -> const std::vector<Value>* { return &static_cast<Vec*>(p)->items; });
    return h;
}

EMBR_PLUGIN {

    // vec() -> ptr<vec.handle>
    interp->bindSpan("vec", {}, [interp](const ArgSpan&) -> Value {
        return makeVec(interp, {});
    });

    // vec_from(arr) -> vec, a copy of the array's elements
    interp->bindSpan("vec_from", {Param::req("arr", TS::Arr)},
    [interp](const ArgSpan& args) -> Value {
        return makeVec(interp, args[0].asArray());
    });

    interp->bindSpan("vec_len", {pPtr("v")},
    [](const ArgSpan& args) -> Value {
        return Value((int64_t)requireVec("vec_len", args[0])->items.size());
    });

    interp->bindSpan("vec_get", {pPtr("v"), pNum("i")},
    [](const ArgSpan& args) -> Value {
        Vec* v = requireVec("vec_get", args[0]);
        return v->items[checkIndex("vec_get", args[1], v->items.size())];
    });

    interp->bindSpan("vec_set", {pPtr("v"), pNum("i"), pAny("x")},
    [](const ArgSpan& args) -> Value {
        Vec* v = requireVec("vec_set", args[0]);
        v->items[checkIndex("vec_set", args[1], v->items.size())] = args[2];
        return Value(0.0);
    });

    interp->bindSpan("vec_push", {pPtr("v"), pAny("x")},
    [](const ArgSpan& args) -> Value {
        Vec* v = requireVec("vec_push", args[0]);
        checkAllocSize("vec_push", "vec length", (int64_t)v->items.size() + 1);
        v->items.push_back(args[1]);
        return Value(0.0);
    });

    // vec_pop(v) -> the removed last element. raises on an empty vec
    interp->bindSpan("vec_pop", {pPtr("v")},
    [](const ArgSpan& args) -> Value {
        Vec* v = requireVec("vec_pop", args[0]);
        if (v->items.empty()) raiseError("vec_pop", "vec is empty");
        Value last = std::move(v->items.back());
        v->items.pop_back();
        return last;
    });

    // vec_insert(v, i, x): i may be vec_len(v), which appends
    interp->bindSpan("vec_insert", {pPtr("v"), pNum("i"), pAny("x")},
    [](const ArgSpan& args) -> Value {
        Vec* v = requireVec("vec_insert", args[0]);
        int64_t i = numToInt64(args[1], "vec_insert", "index");
        if (i < 0 || i > (int64_t)v->items.size())
            raiseError("vec_insert", "index " + std::to_string(i) + " out of bounds (length " +
                       std::to_string(v->items.size()) + ")");
        checkAllocSize("vec_insert", "vec length", (int64_t)v->items.size() + 1);
        v->items.insert(v->items.begin() + i, args[2]);
        return Value(0.0);
    });

    // vec_remove(v, i) -> the removed element
    interp->bindSpan("vec_remove", {pPtr("v"), pNum("i")},
    [](const ArgSpan& args) -> Value {
        Vec* v = requireVec("vec_remove", args[0]);
        size_t i = checkIndex("vec_remove", args[1], v->items.size());
        Value out = std::move(v->items[i]);
        v->items.erase(v->items.begin() + i);
        return out;
    });

    interp->bindSpan("vec_swap", {pPtr("v"), pNum("i"), pNum("j")},
    [](const ArgSpan& args) -> Value {
        Vec* v = requireVec("vec_swap", args[0]);
        size_t i = checkIndex("vec_swap", args[1], v->items.size());
        size_t j = checkIndex("vec_swap", args[2], v->items.size());
        std::swap(v->items[i], v->items[j]);
        return Value(0.0);
    });

    interp->bindSpan("vec_reverse", {pPtr("v")},
    [](const ArgSpan& args) -> Value {
        Vec* v = requireVec("vec_reverse", args[0]);
        std::reverse(v->items.begin(), v->items.end());
        return Value(0.0);
    });

    interp->bindSpan("vec_clear", {pPtr("v")},
    [](const ArgSpan& args) -> Value {
        requireVec("vec_clear", args[0])->items.clear();
        return Value(0.0);
    });

    // vec_extend(v, other): other is an array or another vec (or v itself) and is appended in order
    interp->bindSpan("vec_extend", {pPtr("v"), Param::req("other", TS::Arr | TS::Ptr)},
    [](const ArgSpan& args) -> Value {
        Vec* v = requireVec("vec_extend", args[0]);
        Value::array_type extra;   // copied first, so extending a vec with itself is fine
        if (args[1].isArray()) extra = args[1].asArray();
        else extra = requireVec("vec_extend", args[1])->items;
        checkAllocSize("vec_extend", "vec length", (int64_t)(v->items.size() + extra.size()));
        v->items.insert(v->items.end(), extra.begin(), extra.end());
        return Value(0.0);
    });

    // vec_to_array(v) -> arr, a copy of the elements
    interp->bindSpan("vec_to_array", {pPtr("v")},
    [](const ArgSpan& args) -> Value {
        return Value(Value::array_type(requireVec("vec_to_array", args[0])->items));
    });

    // vec_slice(v, start, end?) -> arr of v[start..end), both ends clamped to the vec
    interp->bindSpan("vec_slice", {pPtr("v"), pNum("start"), Param::opt("end", TS::Num)},
    [](const ArgSpan& args) -> Value {
        Vec* v = requireVec("vec_slice", args[0]);
        int64_t n = (int64_t)v->items.size();
        int64_t s = std::max<int64_t>(0, std::min(numToInt64(args[1], "vec_slice", "start"), n));
        int64_t e = args.size() >= 3 ? std::max<int64_t>(s, std::min(numToInt64(args[2], "vec_slice", "end"), n)) : n;
        return Value(Value::array_type(v->items.begin() + s, v->items.begin() + e));
    });

    // vec_copy(v) -> a new vec with the same elements (the elements themselves are values, so they are copied)
    interp->bindSpan("vec_copy", {pPtr("v")},
    [interp](const ArgSpan& args) -> Value {
        return makeVec(interp, requireVec("vec_copy", args[0])->items);
    });

    // vec_each_do(v, fn): calls fn(x, i) for each element, in order. stops early and returns fn's result as soon as
    // it is truthy, otherwise returns 0. it walks the vec where it is (no copy), checking the length every step, so
    // fn may push (the new elements are visited too), set, or remove without anything breaking
    interp->bindSpan("vec_each_do", {pPtr("v"), Param::req("fn", TS::Fn | TS::Ptr)},
    [interp](const ArgSpan& args) -> Value {
        Vec* v = requireVec("vec_each_do", args[0]);
        for (size_t i = 0; i < v->items.size(); ++i) {
            Value x = v->items[i];   // a copy: fn may grow the vec and move its storage
            Value r = invoke(*interp, args[1], {x, Value((int64_t)i)});
            if (r.truthy()) return r;
        }
        return Value(0.0);
    });

    // vec_map_do(v, fn): replaces each element x with fn(x, i), in place. same live walk as vec_each_do
    interp->bindSpan("vec_map_do", {pPtr("v"), Param::req("fn", TS::Fn | TS::Ptr)},
    [interp](const ArgSpan& args) -> Value {
        Vec* v = requireVec("vec_map_do", args[0]);
        for (size_t i = 0; i < v->items.size(); ++i) {
            Value x = v->items[i];
            Value r = invoke(*interp, args[1], {x, Value((int64_t)i)});
            if (i < v->items.size()) v->items[i] = std::move(r);   // fn may have removed it
        }
        return Value(0.0);
    });

    // vec_assign(v, other): replace everything in v with the elements of other (an array or another vec).
    // the in-place way to sort or filter a vec: vec_assign(v, sort(v))
    interp->bindSpan("vec_assign", {pPtr("v"), Param::req("other", TS::Arr | TS::Ptr)},
    [](const ArgSpan& args) -> Value {
        Vec* v = requireVec("vec_assign", args[0]);
        Value::array_type items = args[1].isArray() ? args[1].asArray() : requireVec("vec_assign", args[1])->items;
        v->items = std::move(items);
        return Value(0.0);
    });

    // len(v) and has(v, i) from embrlib find these by name (the dunder convention in core/registry.h), so the
    // usual calls work on a vec: len(v), has(v, 2)
    interp->bindSpan("__vec_handle_len", {pPtr("v")},
    [](const ArgSpan& args) -> Value {
        return Value((int64_t)requireVec("len", args[0])->items.size());
    });
    interp->bindSpan("__vec_handle_has", {pPtr("v"), pAny("i")},
    [](const ArgSpan& args) -> Value {
        Vec* v = requireVec("has", args[0]);
        int64_t i = numToInt64(args[1], "has", "index");
        return Value(i >= 0 && i < (int64_t)v->items.size() ? (int64_t)1 : (int64_t)0);
    });

    // v[i], v[i] = x and `for x in v` go through these (the same convention). setindex returns the vec, which is what the assignment writes
    // back to the variable. iter returns the elements as an array to loop over
    interp->bindSpan("__vec_handle_index", {pPtr("v"), pNum("i")},
    [](const ArgSpan& args) -> Value {
        Vec* v = requireVec("vec index", args[0]);
        return v->items[checkIndex("vec index", args[1], v->items.size())];
    });
    interp->bindSpan("__vec_handle_setindex", {pPtr("v"), pNum("i"), pAny("x")},
    [](const ArgSpan& args) -> Value {
        Vec* v = requireVec("vec index", args[0]);
        v->items[checkIndex("vec index", args[1], v->items.size())] = args[2];
        return args[0];
    });
    interp->bindSpan("__vec_handle_iter", {pPtr("v")},
    [](const ArgSpan& args) -> Value {
        return Value(Value::array_type(requireVec("for", args[0])->items));
    });

    // is_vec(x) -> 1 if x is a vec handle, else 0
    interp->bindSpan("is_vec", {pAny("x")},
    [](const ArgSpan& args) -> Value {
        return Value(vecOf(args[0]) ? (int64_t)1 : (int64_t)0);
    });

    // what each always returns (see Value::Callable::returns)
    interp->setResultTypes({
        {"vec", TS::Ptr}, {"vec_from", TS::Ptr}, {"vec_copy", TS::Ptr}, {"vec_len", TS::Int}, {"__vec_handle_len", TS::Int},
        {"vec_set", TS::Float}, {"vec_push", TS::Float}, {"vec_insert", TS::Float}, {"vec_swap", TS::Float},
        {"vec_reverse", TS::Float}, {"vec_clear", TS::Float}, {"vec_extend", TS::Float}, {"vec_assign", TS::Float},
        {"vec_map_do", TS::Float}, {"vec_to_array", TS::Arr}, {"vec_slice", TS::Arr},
        {"__vec_handle_has", TS::Int}, {"is_vec", TS::Int}, {"__vec_handle_index", TS::Any}, {"__vec_handle_setindex", TS::Ptr}, {"__vec_handle_iter", TS::Arr},
    });
}

// what this plugin says about itself. embr reads it before it runs embr_register (see core/plugininfo.h)
EMBR_PLUGIN_INFO("vec", "1.0.0", "growable arrays you change in place", EMBR_CAP_NONE, EMBR_CAP_NONE,
    {"__vec_handle_has", "runs for has(v, i); 1 if i is a valid index"},
    {"__vec_handle_index", "runs for v[i]; the same as vec_get"},
    {"__vec_handle_iter", "runs for `for x in v`; gives the elements"},
    {"__vec_handle_len", "runs for len(v)"},
    {"__vec_handle_setindex", "runs for v[i] = x; the same as vec_set"},
    {"is_vec", "1 if v is a vec handle, else 0"},
    {"vec", "new empty vec"},
    {"vec_assign", "replaces all elements. this is the in-place sort: vec_assign(v, sort(v))"},
    {"vec_clear", "removes every element"},
    {"vec_copy", "a separate vec with the same elements"},
    {"vec_each_do", "calls fn(x, i) for each element. stops and returns fn's result as soon as it is truthy, else returns 0. it walks the vec where it is (no copy) and checks the length at every step, so fn may push (new elements get visited), set or remove"},
    {"vec_extend", "other is an array or a vec, and may be v itself"},
    {"vec_from", "new vec holding a copy of the array's elements"},
    {"vec_get", "raise index N out of bounds (length L) outside 0..len-1"},
    {"vec_insert", "O(n). i may be vec_len(v) for insert"},
    {"vec_len", "the number of elements (len(v) gives the same)"},
    {"vec_map_do", "replaces each element x with fn(x, i) in place, same live walk"},
    {"vec_pop", "vec_pop returns the removed element and raises on an empty vec"},
    {"vec_push", "vec_pop returns the removed element and raises on an empty vec"},
    {"vec_remove", "O(n). i may be vec_len(v) for insert"},
    {"vec_reverse", "reverses the order of the elements in place"},
    {"vec_set", "raise index N out of bounds (length L) outside 0..len-1"},
    {"vec_slice", "copies. vec_slice clamps both ends"},
    {"vec_swap", "swaps the elements at positions i and j"},
    {"vec_to_array", "copies. vec_slice clamps both ends"});
