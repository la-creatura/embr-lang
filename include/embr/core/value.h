#ifndef EMBR_CORE_VALUE_H
#define EMBR_CORE_VALUE_H

#include "types.h"
#include "fwd.h"
#include "diagnostics.h"

#include <string>
#include <vector>
#include <unordered_map>
#include <variant>
#include <functional>
#include <memory>
#include <sstream>
#include <cstdint>
#include <type_traits>
#include <algorithm>
#include <mutex>
#include <atomic>
#include <unordered_set>
#include <string_view>

namespace embr {

struct ScriptFn {
    std::string        name;
    std::vector<Param> params;
    TypeSet            returnType = TypeSet::Any();
    SourceRange        definedAt;
    std::string        sourceFile;
    std::shared_ptr<CaptureFrame> captured;
    // type-erased embr::vm::CompiledChunk*. set only when the vm's Compiler compiled this function, null otherwise
    // it's type-erased because core/ can't depend on backends/vm.h (a peer header)
    // callers that need it use std::static_pointer_cast<vm::CompiledChunk>(compiledChunk)
    // see core/invoke.h for the dispatch this exists for
    std::shared_ptr<void> compiledChunk;
};

struct Value;

// how a plugin tells the gc plugin which Values its opaque userdata holds. the gc can't look inside a type-erased
// `owner`, so a handle whose object stores Values (a table's entries, a callback's closure) attaches a TraceFn
// that calls `visit` on each of them. without it, a closure reachable only through such a handle looks
// unreachable, and gc_collect() would clear its captured scope and break a live closure
using TraceVisitor = std::function<void(const Value&)>;
using TraceFn      = std::function<void(const TraceVisitor&)>;

// a pointer's tag ("vec", "types.desc", ...), kept as one pointer to a string that lives for the whole run.
// copying a tag copies the pointer, and two tags from the same plugin compare by address first. two plugins may
// each hold their own entry for the same text, so a mismatch still compares the text, which keeps it correct
class Tag {
public:
    Tag() = default;
    Tag(const std::string& s)      : p_(s.empty() ? nullptr : intern(s)) {}
    Tag(const char* s)             : p_(s && *s ? intern(s) : nullptr) {}
    bool empty() const { return !p_; }
    const void* id() const { return p_; }   // the same for equal tags (they are interned), so it can key a cache
    const std::string& str() const { return p_ ? *p_ : emptyStr(); }
    operator const std::string&() const { return str(); }
    size_t size() const { return str().size(); }
    const char* c_str() const { return str().c_str(); }
    bool operator==(const Tag& o) const { return p_ == o.p_ || str() == o.str(); }
    bool operator!=(const Tag& o) const { return !(*this == o); }
    bool operator==(const std::string& o) const { return str() == o; }
    bool operator==(const char* o) const { return str() == o; }
    bool operator!=(const std::string& o) const { return !(*this == o); }
    bool operator!=(const char* o) const { return !(*this == o); }
    friend std::string operator+(const std::string& a, const Tag& t) { return a + t.str(); }
    friend std::string operator+(const char* a, const Tag& t) { return std::string(a) + t.str(); }
    friend std::string operator+(const Tag& t, const std::string& b) { return t.str() + b; }
    friend std::string operator+(const Tag& t, const char* b) { return t.str() + b; }
private:
    static const std::string& emptyStr() { static const std::string e; return e; }
    static const std::string* intern(const std::string& s) {
        static std::mutex m;
        static std::unordered_set<std::string> table; // never freed, entries never move
        std::lock_guard<std::mutex> lk(m);
        return &*table.insert(s).first;
    }
    const std::string* p_ = nullptr;
};

// what a handle owns, shared by every copy of it. one object so a TypedPtr stays small
struct HandleData {
    void*                          object = nullptr;  // the owned object, or null
    std::shared_ptr<void>          keep;              // set when the object lives somewhere else (see TypedPtr's 3 argument constructor)
    // optional, see TraceFn. it may capture a raw pointer to the object: it is only ever invoked through a
    // TypedPtr that also holds this HandleData, so the object is alive. (A handle without an owner, e.g.
    // ffi callbacks, must not capture a raw pointer; look the object up by key in weakly-held plugin state instead.)
    std::unique_ptr<const TraceFn> tracer;
    // optional: a handle that is a list (a vec) gives its elements, so code that reads arrays (a C plugin through
    // embrp_array_len / embrp_array_at) can read it where it is. `object` is passed back
    const std::vector<Value>* (*arrayView)(void* object) = nullptr;
    virtual ~HandleData() = default;
};
// the object and its HandleData in one allocation (what makeUserdata makes)
template<class T> struct HandleOf : HandleData {
    T value;
    template<class... A> explicit HandleOf(A&&... a) : value(std::forward<A>(a)...) { object = &value; }
};

// a plugin-typed opaque pointer/userdata handle
struct TypedPtr {
    void*                       ptr = nullptr;
    Tag                         type;   // empty = untyped/raw
    std::shared_ptr<HandleData> h;      // non-null only for handles that own something (makeUserdata and friends)

    TypedPtr() = default;
    explicit TypedPtr(void* p) : ptr(p) {}
    TypedPtr(void* p, Tag t) : ptr(p), type(t) {}
    TypedPtr(void* p, Tag t, std::shared_ptr<void> own) : ptr(p), type(t) {
        if (own) { h = std::make_shared<HandleData>(); h->object = own.get(); h->keep = std::move(own); }
    }

    // the owned object as a shared_ptr (empty if the handle owns nothing). it keeps the whole handle data alive
    std::shared_ptr<void> owner() const {
        if (!h) return {};
        return h->keep ? h->keep : std::shared_ptr<void>(h, h->object);
    }
    // the owned object without touching a reference count
    void* ownerPtr() const { return h ? h->object : nullptr; }

    // same address and same tag. the tag matters so two null pointers with different tags (embrtypes' nil and false)
    // stay different. see the header of plugins/embrtypes/embrtypes.cpp
    bool operator==(const TypedPtr& o) const { return ptr == o.ptr && type == o.type; }
    bool operator!=(const TypedPtr& o) const { return !(*this == o); }
};

inline std::string formatNumber(double d) {
    if (d >= -1e15 && d <= 1e15 && d == static_cast<double>(static_cast<long long>(d))) {
        std::ostringstream oss; oss << static_cast<long long>(d); return oss.str();
    }
    std::string s = std::to_string(d);
    if (s.find('.') != std::string::npos) {
        s.erase(s.find_last_not_of('0') + 1);
        if (s.back() == '.') s.pop_back();
    }
    return s;
}

struct ArgSpan;
struct Value {
    using array_type = std::vector<Value>;
    using map_type   = std::unordered_map<std::string, Value>;
    using NativeFn   = std::function<Value(const std::vector<Value>&)>;
    using int_type   = int64_t;
    using ptr_type   = TypedPtr;

    // a function value's data: a native or a script function, shared by every copy of the value
    struct Callable {
        enum class Kind { Native, Script } kind = Kind::Native;
        NativeFn native;
        // a native that takes its arguments as a view (bindSpan) has this too. the VM calls it directly with the values on its stack. `native` does
        // the same for a caller that has a vector. such a native must not call back into script code: the stack could move under it
        std::function<Value(const ArgSpan&)> span;
        ScriptFn           script;  // script.name also holds native display name
        std::vector<Param> sig;
        // what a native always returns, when it is always one of a few types (any when it varies). the static type checker
        // reads it, and the VM refuses a native that returns something else, so a wrong claim fails loudly
        TypeSet            returns = TypeSet::Any();
        // the native gives the same result for the same arguments and changes nothing (no state, files, time or script callbacks),
        // so a `const` may call it while the script is compiled. a plugin says so with setPure (registry.h)
        bool               pure = false;
        // the bare name of an overload group (`abs` for abs__1, abs__2, ...). the VM picks the member and calls it in place of this
        bool               overloadDispatcher = false;
        // a native bound with bindTyped (typed.h): the VM calls `thunk(fn, args)` straight on the values where they sit when `n` arguments of the
        // types in `mask` (one bit per core type, like TypeSet) are there. no std::function, no signature check
        struct Typed {
            Value (*thunk)(void* fn, void* ctx, const Value* args) = nullptr;
            void*   fn = nullptr;
            void*   ctx = nullptr;   // the interpreter it was bound in
            uint8_t n = 0;
            uint8_t mask[4] = {0, 0, 0, 0};
        } typed;
        // when it returns an array or a map: what that holds (`str_split` returns arr<str>), null when it varies
        TypeElem           returnsElem;
        // the result is the argument with this number, with its element type (`push(list, x)` gives back list), -1 when it is not
        int                returnsLike = -1;

        // what the VM needs to check a call of this native quickly, worked out once when it is bound. `quick`: no variadic or in-out
        // parameter and at most 8, so the type of argument i is types[i] and nothing else has to be looked at
        struct SigInfo {
            uint16_t minArgs = 0, maxArgs = 0xFFFF;
            bool     quick = false;
            TypeSet  types[8];
        } sigInfo;
        static Callable fromNative(std::string nm, NativeFn fn, std::vector<Param> sig) {
            Callable c; c.kind = Kind::Native; c.native = std::move(fn);
            c.script.name = std::move(nm); c.sig = std::move(sig);
            size_t minArgs = 0; bool variadic = false, inout = false;
            for (const auto& p : c.sig) {
                if (p.variadic) { variadic = true; break; }
                if (!p.optional) ++minArgs;
                if (p.inout) inout = true;
            }
            c.sigInfo.minArgs = (uint16_t)std::min<size_t>(minArgs, 0xFFFF);
            c.sigInfo.maxArgs = variadic ? 0xFFFF : (uint16_t)std::min<size_t>(c.sig.size(), 0xFFFF);
            c.sigInfo.quick   = !variadic && !inout && c.sig.size() <= 8;
            for (size_t i = 0; i < c.sig.size() && i < 8; ++i) c.sigInfo.types[i] = c.sig[i].type;
            return c;
        }
        static Callable fromScript(ScriptFn fn) {
            Callable c; c.kind = Kind::Script; c.script = std::move(fn);
            return c;
        }
        const std::string& name()     const { return script.name; }
        bool               isNative() const { return kind == Kind::Native; }
        bool               isScript() const { return kind == Kind::Script; }
    };

    // a Value is 16 bytes: an 8 byte payload, the nesting depth and a one byte type. numbers and ints sit in the
    // payload. a string, array, map, function or pointer handle is a pointer to a counted box (StrBox, ArrBox,
    // MapBox, FnBox, PtrBox below), shared by every copy. copying a Value shares the box, and a write goes through
    // strMut()/arrayMut()/mapMut(), which copy the box first when someone else also holds it. so scripts see plain
    // value semantics (`b = a; push(b, x)` leaves a alone) and a copy costs one count, not the whole container.
    // the type is the bit position of its TypeTag (types.h), so tag() is a shift
    union Bits { double d; int64_t i; void* p; };
    Bits     u;

    // how deeply arrays/maps are nested in this value: 0 for anything else, 1 + the deepest element for arrays/maps
    // (an empty array is 1). kept up to date by the array/map constructors and copied along, so it's one pass over the
    // elements, not a recursive walk. exists because copying, destroying, comparing and printing recurse once per
    // level: `a = [a]` in a loop could build a value ~20,000 deep and crash the process. so building anything
    // deeper than kMaxValueDepth is an ordinary error
    static constexpr uint32_t kMaxValueDepth = 1000;
    uint32_t nestDepth = 0;
    uint8_t  tg        = 0;   // 0 number, 1 int, 2 string, 3 array, 4 map, 5 callable, 6 pointer. below 2 owns nothing

    Value()                     : tg(0) { u.d = 0.0; }
    [[gnu::always_inline]] explicit Value(double v)    : tg(0) { u.d = v; }
    [[gnu::always_inline]] explicit Value(int_type v)  : tg(1) { u.i = v; }
    // constrained to an exact bool
    // so a stray raw pointer passed to Value(...) fails to compile via pointer-to-bool conversion instead of silently constructing Value(true)
    // a bool is the int 1 or 0 (embr has no boolean type)
    template<class B, typename = std::enable_if_t<std::is_same_v<B, bool>>>
    [[gnu::always_inline]] explicit Value(B b)         : tg(1) { u.i = b ? 1 : 0; }
    Value(const std::string& v);
    Value(std::string&& v);
    Value(array_type v);
    Value(map_type v);
    explicit Value(ptr_type p);

    [[gnu::always_inline]] Value(const Value& o) noexcept : u(o.u), nestDepth(o.nestDepth), tg(o.tg) { if (tg >= 2) retain(); }
    [[gnu::always_inline]] Value(Value&& o) noexcept : u(o.u), nestDepth(o.nestDepth), tg(o.tg) { o.tg = 0; o.u.d = 0.0; o.nestDepth = 0; }
    [[gnu::always_inline]] ~Value() { if (tg >= 2) release(); }
    [[gnu::always_inline]] Value& operator=(const Value& o) noexcept { if (this != &o) { Value tmp(o); swap(tmp); } return *this; }
    // a value that holds nothing on the heap is overwritten in place. otherwise through a temporary, so
    // `v = std::move(v.something inside v)` works: the old contents die last
    [[gnu::always_inline]] Value& operator=(Value&& o) noexcept {
        if (tg < 2) { u = o.u; nestDepth = o.nestDepth; tg = o.tg; o.tg = 0; o.u.d = 0.0; o.nestDepth = 0; return *this; }
        Value tmp(std::move(o)); swap(tmp); return *this;
    }
    // written out by hand: std::swap on the union was a call every time
    [[gnu::always_inline]] void swap(Value& o) noexcept {
        const auto tu = u; u = o.u; o.u = tu;
        const auto tn = nestDepth; nestDepth = o.nestDepth; o.nestDepth = tn;
        const auto tt = tg; tg = o.tg; o.tg = tt;
    }

    static Value makeNative(std::string name, NativeFn fn,
                            std::vector<Param> sig = {});
    static Value makeScript(ScriptFn fn);

    void computeNestDepth();

    // in-place edits of an array, map or string. the backends and the in-out natives use these instead of
    // copy, change, store back, and they keep nestDepth right. callers do their own range checks so the error
    // can name the script line. these only back-stop it
    void arrayAppend(Value v) {
        auto& a = arrayMut();
        checkElementDepth(v);
        uint32_t nw = v.nestDepth;
        a.push_back(std::move(v));
        noteReplaced(0, nw);
    }
    void arrayInsert(size_t i, Value v) {
        auto& a = arrayMut();
        if (i > a.size()) raiseError("value", "array insert position " + std::to_string(i) + " out of bounds");
        checkElementDepth(v);
        uint32_t nw = v.nestDepth;
        a.insert(a.begin() + (std::ptrdiff_t)i, std::move(v));
        noteReplaced(0, nw);
    }
    void arraySet(size_t i, Value v) {
        auto& a = arrayMut();
        if (i >= a.size()) raiseError("value", "array index " + std::to_string(i) + " out of bounds");
        checkElementDepth(v);
        uint32_t old = a[i].nestDepth, nw = v.nestDepth;
        a[i] = std::move(v);
        noteReplaced(old, nw);
    }
    Value arrayRemoveAt(size_t i) {
        auto& a = arrayMut();
        if (i >= a.size()) raiseError("value", "array index " + std::to_string(i) + " out of bounds");
        Value out = std::move(a[i]);
        a.erase(a.begin() + (std::ptrdiff_t)i);
        noteRemoved(out.nestDepth);
        return out;
    }
    void mapSet(const std::string& k, Value v) {
        auto& m = mapMut();
        checkElementDepth(v);
        uint32_t nw = v.nestDepth;
        // one hash for both cases: the key is only copied when it is new
        auto [it, isNew] = m.try_emplace(k);
        if (isNew) {
            it->second = std::move(v);
            noteReplaced(0, nw);
        } else {
            uint32_t old = it->second.nestDepth;
            it->second = std::move(v);
            noteReplaced(old, nw);
        }
    }
    // false if there was no such key. `removed` (if given) gets the old value
    bool mapRemove(const std::string& k, Value* removed = nullptr) {
        auto& m = mapMut();
        auto it = m.find(k);
        if (it == m.end()) return false;
        Value old = std::move(it->second);
        m.erase(it);
        noteRemoved(old.nestDepth);
        if (removed) *removed = std::move(old);
        return true;
    }
    // replaces s.size() characters from position i, like std::string::replace
    void stringSetAt(size_t i, const std::string& s) {
        if (!isString()) raiseError("value", "expected string, got " + typeName());
        if (i >= asString().size()) raiseError("value", "string index " + std::to_string(i) + " out of bounds");
        strMut().replace(i, s.size(), s);
    }

    // raises if `elem`, put into a container that has `levelsAbove` more containers above it, would go past kMaxValueDepth
    static void checkElementDepth(const Value& elem, size_t levelsAbove = 0) {
        if ((size_t)elem.nestDepth + 1 + levelsAbove > kMaxValueDepth)
            raiseError("value", "value nested too deeply (max depth " + std::to_string(kMaxValueDepth) + ")");
    }
    // nestDepth bookkeeping after one element changed from oldDepth to newDepth. cheap unless the deepest element
    // was replaced by a shallower one
    void noteReplaced(uint32_t oldDepth, uint32_t newDepth) {
        if (newDepth + 1 > nestDepth) nestDepth = newDepth + 1;
        else if (oldDepth + 1 == nestDepth && newDepth < oldDepth) computeNestDepth();
    }
    void noteRemoved(uint32_t removedDepth) {
        if (removedDepth > 0 && removedDepth + 1 == nestDepth) computeNestDepth();
    }

    // constructs a typed pointer Value
    // type is a plugin-namespaced tag
    // leave it empty for an untyped raw pointer
    // named factory rather than an implicit Value(void*) constructor
    static Value makePointer(void* p, Tag type = Tag()) {
        return Value(ptr_type(p, type));
    }

    // constructs an owned, refcounted userdata handle
    // heap-allocates a T via make_shared, tags it type, and keeps it alive for as long as any copy of the returned Value exists
    // auto-freed when the last one is dropped
    template<class T, class... Args>
    static Value makeUserdata(Tag type, Args&&... args) {
        auto obj = std::make_shared<HandleOf<T>>(std::forward<Args>(args)...);
        void* raw = &obj->value;
        TypedPtr tp(raw, type);
        tp.h = std::move(obj);
        return Value(std::move(tp));
    }

    // attaches a TraceFn to this (pointer) Value's handle, call right after creating it, before any
    // copies are made, so every copy shares it. returns *this for chaining.
    Value& withTracer(TraceFn f);
    // the same for arrayView (see HandleData)
    Value& withArrayView(const std::vector<Value>* (*view)(void* object));
    // the elements of an array, or of a handle that gave an arrayView. null for anything else
    const std::vector<Value>* arrayLike() const;

    TypeTag tag() const { return static_cast<TypeTag>(1 << tg); }

    bool isNumber()   const { return tg == 0; }
    bool isInt()      const { return tg == 1; }
    bool isNumeric()  const { return tg < 2;  }
    bool isString()   const { return tg == 2; }
    bool isArray()    const { return tg == 3; }
    bool isMap()      const { return tg == 4; }
    bool isCallable() const { return tg == 5; }
    bool isPointer()  const { return tg == 6; }

    TypeSet typeSet() const { return TypeSet(tag()); }

    // ideally these would only produce errors when something went very, very terribly wrong

    // the right-type case is inline, the error is a call nobody pays for. an out-of-line asInt() was a measurable part of a tight loop
    [[gnu::always_inline]] double asNumber() const {
        if (tg == 0) return u.d;
        if (tg == 1) return (double)u.i;
        wrongType("number");
    }

    [[gnu::always_inline]] int_type asInt() const {
        if (tg == 1) return u.i;
        if (tg == 0) return (int_type)u.d;
        wrongType("int");
    }

    [[noreturn, gnu::noinline]] void wrongType(const char* want) const {
        raiseError("value", std::string("expected ") + want + ", got " + typeName());
    }
    const std::string& asString()   const;
    const array_type&  asArray()    const;
    // ranges (`[0...10]`): the elements are not built until something needs the list. these four don't build it
    static Value       makeRange(int64_t start, int64_t step, int64_t count);
    bool               isLazyRange() const;
    bool               rangeParts(int64_t& start, int64_t& step, int64_t& count) const;   // true while it is still a range
    size_t             arraySize() const;
    Value              arrayGet(size_t i) const;
    const map_type&    asMap()      const;
    array_type&        arrayMut();
    map_type&          mapMut();
    std::string&       strMut();
    [[gnu::always_inline]] const Callable& asCallable() const;
    const ptr_type&    asPointer()  const;

    // validating overload
    // also checks the pointer's plugin-assigned type tag
    const ptr_type& asPointer(const std::string& expectedType) const { return asPointer(expectedType.c_str()); }
    const ptr_type& asPointer(const char* expectedType) const {
        const ptr_type& p = asPointer();
        if (*expectedType && !(p.type == expectedType))
            raiseError("value", "expected pointer of type '" + std::string(expectedType) +
                       "', got '" + (p.type.empty() ? std::string("<untyped>") : p.type.str()) + "'");
        return p;
    }

    // convenience accessor for a userdata payload's underlying object
    // does not check dynamic C++ type, only the plugin-assigned tag string
    template<class T>
    T* userdata(const std::string& expectedType = "") const {
        return static_cast<T*>(asPointer(expectedType).ptr);
    }

    std::string typeName() const { return TypeSet(tag()).name(); }

    std::string  formatAsString()   const {
        switch (tag()) {
            case TypeTag::Number:   return formatNumber(this->asNumber());
            case TypeTag::Int:      return std::to_string(u.i);
            case TypeTag::String:   return asString();
            case TypeTag::Callable: return "<fn " + asCallable().name() + ">";
            case TypeTag::Array: {
                std::string s = "["; const auto& a = asArray();
                for (size_t i = 0; i < a.size(); ++i) { if (i) s += ", "; s += valueRepr(a[i]); }
                    return s + "]";
            }
            case TypeTag::Map: {
                // asMap() is an unordered_map (unspecified/unstable hash order),
                // so sort keys here for reproducible output across runs
                const auto& mp = asMap();
                std::vector<std::string> ks; ks.reserve(mp.size());
                for (const auto& [k, val] : mp) ks.push_back(k);
                std::sort(ks.begin(), ks.end());
                std::string s = "{"; bool first = true;
                for (const auto& k : ks) {
                    if (!first) s += ", ";
                        first = false;
                    s += k + ": " + valueRepr(mp.at(k));
                }
                return s + "}";
            }
            case TypeTag::Pointer: {
                // a tagged pointer prints as "<TAG 0xADDR>", the tag leads (nil prints "<nil 0x0>"). an untyped pointer
                // (empty tag) prints as "<ptr 0xADDR>"
                const auto& p = asPointer();
                std::ostringstream oss;
                oss << "<" << (p.type.empty() ? std::string("ptr") : p.type.str())
                    << " 0x" << std::hex << reinterpret_cast<uintptr_t>(p.ptr) << ">";
                return oss.str();
            }
        }
        return "<unknown>";
    }

    bool operator==(const Value& o) const {
        if (typeSet() == o.typeSet()) {
            switch (tag()) {
                case TypeTag::Number: return asNumber() == o.asNumber();
                case TypeTag::Int:    return asInt()    == o.asInt();
                case TypeTag::String: return asString() == o.asString();
                case TypeTag::Array: {
                    const auto& arrA = asArray();
                    const auto& arrB = o.asArray();
                    if (arrA.size() != arrB.size()) return false;
                    for (size_t i = 0; i < arrA.size(); ++i)
                        if (!(arrA[i] == arrB[i])) return false;
                    return true;
                }
                case TypeTag::Map: {
                    const auto& mapA = asMap();
                    const auto& mapB = o.asMap();
                    if (mapA.size() != mapB.size()) return false;
                    for (const auto& [key, valA] : mapA) {
                        auto it = mapB.find(key);
                        if (it == mapB.end()) return false;
                        if (!(valA == it->second)) return false;
                    }
                    return true;
                }
                // identity: two callables are equal iff they are the same function object (every copy of a
                // closure value shares one Callable, so `f == f` and `g = f; g == f` are true, while two
                // separately created closures, even with identical source, are different)
                case TypeTag::Callable: return u.p == o.u.p;
                case TypeTag::Pointer:  return asPointer() == o.asPointer();
            }
        }
        // cross-subtype numeric equality (5 == 5.0) falls out of the general string-formatting fallback below,
        // same as any other mismatched-tag comparison
        return formatAsString() == o.formatAsString();
    }

    bool operator!=(const Value& o) const { return !(*this == o); }

    // 0.0, "", [], {} are falsy
    // functions truthy
    bool truthy() const {
        switch (tag()) {
            case TypeTag::Number:   return  u.d != 0.0;
            case TypeTag::Int:      return  u.i != 0;
            case TypeTag::String:   return !asString().empty();
            case TypeTag::Array:    return !asArray().empty();
            case TypeTag::Map:      return !asMap().empty();
            case TypeTag::Callable: return true;
            case TypeTag::Pointer:  return asPointer().ptr != nullptr;
        }
        return false;
    }

private:
    void retain() const noexcept;
    void release() noexcept;
public:
    // marks this value and everything inside it as possibly seen by several threads, so its counts become atomic.
    // call it on a value before another thread can get at it. walks arrays and maps all the way down every time:
    // a box that is already marked can have been given new, unmarked elements since (see the wiki, architecture)
    void markShared() const;
};
static_assert(sizeof(Value) == 16, "Value is meant to stay 16 bytes");

// the counted boxes behind a string, array, map, function or pointer Value. a count is atomic only once the box is marked
// shared (see RcBox), because a Value can be copied on one thread while another thread holds a copy (parallel / async
// hand values across)
// a native's arguments as a view of the values where they already are (on the VM's value stack), so a call copies nothing. it has the
// parts of std::vector a native uses to read them. valid only during the call. see Interpreter::bindSpan
struct ArgSpan {
    const Value* p = nullptr;
    size_t       n = 0;
    size_t       size()  const { return n; }
    bool         empty() const { return n == 0; }
    const Value& operator[](size_t i) const { return p[i]; }
    const Value& front() const { return p[0]; }
    const Value& back()  const { return p[n - 1]; }
    const Value* begin() const { return p; }
    const Value* end()   const { return p + n; }
};

// `shared` says another thread may hold a copy of this box. until then the count is changed with plain loads and stores,
// which are much cheaper than the locked add an atomic count needs. markShared() (below) sets it on everything a value
// holds, and must be called before a value is handed to another thread (channels, async, shared tables). once set it
// stays set. see the wiki, architecture
struct RcBox {
    std::atomic<uint32_t> rc{1};
    std::atomic<bool>     shared{false};
    void retainBox() noexcept {
        if (__builtin_expect(shared.load(std::memory_order_relaxed), 0)) rc.fetch_add(1, std::memory_order_relaxed);
        else rc.store(rc.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    }
    // true when this was the last count
    bool releaseBox() noexcept {
        if (__builtin_expect(shared.load(std::memory_order_relaxed), 0)) return rc.fetch_sub(1, std::memory_order_acq_rel) == 1;
        uint32_t n = rc.load(std::memory_order_relaxed) - 1;
        rc.store(n, std::memory_order_relaxed);
        return n == 0;
    }
};
struct StrBox : RcBox { std::string s;        explicit StrBox(std::string v) : s(std::move(v)) {} };
// the most elements a range turns into an array (the same limit natives have)
inline constexpr int64_t kMaxRangeElements = 256LL * 1024 * 1024;

// an array. `[0...10]` makes one that has not built its elements: it only knows where they start, the step and how many there are, and builds
// the real list the first time something asks for it (asArray, arrayMut). a `for` loop, len, and an element read do not ask, so a range
// that is only looped over or indexed never takes memory. state: 0 = `v` holds the elements, 1 = still a range, 2 = being filled in by a thread
struct ArrBox : RcBox {
    mutable Value::array_type v;
    mutable std::atomic<uint8_t> state{0};
    int64_t start = 0, step = 1, count = 0;
    explicit ArrBox(Value::array_type x) : v(std::move(x)) {}
    ArrBox(int64_t s, int64_t st, int64_t c) : start(s), step(st), count(c) { state.store(1, std::memory_order_relaxed); }
    ArrBox(const ArrBox&) = delete;
    void fill() const;   // out of line, below Value
};
struct MapBox : RcBox { Value::map_type m;    explicit MapBox(Value::map_type x) : m(std::move(x)) {} };
struct FnBox  : RcBox { Value::Callable c;    explicit FnBox(Value::Callable x) : c(std::move(x)) {} };
struct PtrBox : RcBox { TypedPtr tp;          explicit PtrBox(TypedPtr x) : tp(std::move(x)) {} };

inline void Value::retain() const noexcept {
    static_cast<RcBox*>(u.p)->retainBox();
}
inline void Value::release() noexcept {
    RcBox* b = static_cast<RcBox*>(u.p);
    if (!b->releaseBox()) return;
    switch (tg) {
        case 2: delete static_cast<StrBox*>(b); break;
        case 3: delete static_cast<ArrBox*>(b); break;
        case 4: delete static_cast<MapBox*>(b); break;
        case 5: delete static_cast<FnBox*>(b);  break;
        case 6: delete static_cast<PtrBox*>(b); break;
    }
}

inline void Value::markShared() const {
    if (tg < 2) return;
    static_cast<RcBox*>(u.p)->shared.store(true, std::memory_order_relaxed);
    if (tg == 3) {
        auto* b = static_cast<ArrBox*>(u.p);
        if (b->state.load(std::memory_order_acquire) == 0) for (const Value& e : b->v) e.markShared();   // an unbuilt range holds numbers only
    } else if (tg == 4) {
        for (const auto& kv : static_cast<MapBox*>(u.p)->m) kv.second.markShared();
    }
}

inline Value::Value(const std::string& v) : tg(2) { u.p = new StrBox(v); }
inline Value::Value(std::string&& v)      : tg(2) { u.p = new StrBox(std::move(v)); }
// computeNestDepth() raises when the value is too deep. a constructor that throws never runs the destructor, so the
// box made just before it has to be given back here
inline Value::Value(array_type v)         : tg(3) { u.p = new ArrBox(std::move(v)); try { computeNestDepth(); } catch (...) { release(); throw; } }
inline Value::Value(map_type v)           : tg(4) { u.p = new MapBox(std::move(v)); try { computeNestDepth(); } catch (...) { release(); throw; } }
inline Value::Value(ptr_type p)           : tg(6) { u.p = new PtrBox(std::move(p)); }

inline Value Value::makeNative(std::string name, NativeFn fn, std::vector<Param> sig) {
    Value v; v.tg = 5;
    v.u.p = new FnBox(Callable::fromNative(std::move(name), std::move(fn), std::move(sig)));
    return v;
}
inline Value Value::makeScript(ScriptFn fn) {
    Value v; v.tg = 5;
    v.u.p = new FnBox(Callable::fromScript(std::move(fn)));
    return v;
}

inline void Value::computeNestDepth() {
    uint32_t deepest = 0;
    if (tg == 3) {
        for (const auto& el : static_cast<ArrBox*>(u.p)->v) if (el.nestDepth > deepest) deepest = el.nestDepth;
    } else if (tg == 4) {
        for (const auto& kv : static_cast<MapBox*>(u.p)->m) if (kv.second.nestDepth > deepest) deepest = kv.second.nestDepth;
    }
    nestDepth = deepest + 1;
    if (nestDepth > kMaxValueDepth)
        raiseError("value", "value nested too deeply (max depth " + std::to_string(kMaxValueDepth) + ")");
}

inline Value& Value::withTracer(TraceFn f) {
    if (!isPointer()) raiseError("value", "withTracer: expected pointer, got " + typeName());
    auto& hd = static_cast<PtrBox*>(u.p)->tp.h;
    if (!hd) raiseError("value", "withTracer: the handle owns nothing");
    hd->tracer = std::make_unique<const TraceFn>(std::move(f));
    return *this;
}

inline Value& Value::withArrayView(const std::vector<Value>* (*view)(void* object)) {
    if (!isPointer()) raiseError("value", "withArrayView: expected pointer, got " + typeName());
    auto& hd = static_cast<PtrBox*>(u.p)->tp.h;
    if (!hd) raiseError("value", "withArrayView: the handle owns nothing");
    hd->arrayView = view;
    return *this;
}

inline const std::string&      Value::asString()   const { if (tg != 2) raiseError("value","expected string, got "  +typeName()); return static_cast<StrBox*>(u.p)->s; }
inline void ArrBox::fill() const {
    uint8_t expected = 1;
    while (state.load(std::memory_order_acquire) != 0) {
        if (state.compare_exchange_weak(expected, 2, std::memory_order_acquire)) {
            if (count > kMaxRangeElements) { state.store(1, std::memory_order_release); raiseError("value", "a range of " + std::to_string(count) + " elements is too long to turn into an array (the most is " + std::to_string(kMaxRangeElements) + ")"); }
            v.reserve((size_t)count);
            for (int64_t i = 0; i < count; ++i) v.push_back(Value((int64_t)((uint64_t)start + (uint64_t)i * (uint64_t)step)));
            state.store(0, std::memory_order_release);
            return;
        }
        if (expected == 0) return;
        expected = 1;
    }
}
inline const Value::array_type& Value::asArray()   const {
    if (tg != 3) raiseError("value","expected array, got " + typeName());
    auto* b = static_cast<ArrBox*>(u.p);
    if (b->state.load(std::memory_order_acquire) != 0) [[unlikely]] b->fill();
    return b->v;
}
inline const std::vector<Value>* Value::arrayLike() const {
    if (tg == 3) return &asArray();
    if (tg == 6) {
        const TypedPtr& p = static_cast<PtrBox*>(u.p)->tp;
        if (p.h && p.h->arrayView) return p.h->arrayView(p.ptr);
    }
    return nullptr;
}
inline Value Value::makeRange(int64_t start, int64_t step, int64_t count) {
    Value r; r.tg = 3; r.u.p = new ArrBox(start, step, count); r.nestDepth = 1;
    return r;
}
inline bool Value::isLazyRange() const { return tg == 3 && static_cast<ArrBox*>(u.p)->state.load(std::memory_order_acquire) == 1; }
inline bool Value::rangeParts(int64_t& start, int64_t& step, int64_t& count) const {
    if (tg != 3) return false;
    auto* b = static_cast<ArrBox*>(u.p);
    if (b->state.load(std::memory_order_acquire) != 1) return false;
    start = b->start; step = b->step; count = b->count;
    return true;
}
inline size_t Value::arraySize() const {
    if (tg != 3) raiseError("value","expected array, got " + typeName());
    auto* b = static_cast<ArrBox*>(u.p);
    return b->state.load(std::memory_order_acquire) == 1 ? (size_t)b->count : b->v.size();
}
inline Value Value::arrayGet(size_t i) const {   // the caller has checked i < arraySize()
    auto* b = static_cast<ArrBox*>(u.p);
    if (b->state.load(std::memory_order_acquire) == 1) return Value((int64_t)((uint64_t)b->start + (uint64_t)i * (uint64_t)b->step));
    return b->v[i];
}
inline const Value::map_type&   Value::asMap()     const { if (tg != 4) raiseError("value","expected map, got "     +typeName()); return static_cast<MapBox*>(u.p)->m; }
inline const Value::Callable&   Value::asCallable() const { if (tg != 5) [[unlikely]] wrongType("function"); return static_cast<FnBox*>(u.p)->c; }
inline const TypedPtr&          Value::asPointer() const { if (tg != 6) raiseError("value","expected pointer, got " +typeName()); return static_cast<PtrBox*>(u.p)->tp; }

// the data behind a string, array or map Value, copied into a box of its own first if another Value shares it
inline std::string& Value::strMut() {
    if (tg != 2) raiseError("value","expected string, got " + typeName());
    auto* b = static_cast<StrBox*>(u.p);
    if (b->rc.load(std::memory_order_acquire) > 1) { auto* nb = new StrBox(b->s); release(); u.p = nb; b = nb; }
    return b->s;
}
inline Value::array_type& Value::arrayMut() {
    if (tg != 3) raiseError("value","expected array, got " + typeName());
    auto* b = static_cast<ArrBox*>(u.p);
    if (b->state.load(std::memory_order_acquire) != 0) b->fill();
    if (b->rc.load(std::memory_order_acquire) > 1) { auto* nb = new ArrBox(b->v); release(); u.p = nb; b = nb; }
    return b->v;
}
inline Value::map_type& Value::mapMut() {
    if (tg != 4) raiseError("value","expected map, got " + typeName());
    auto* b = static_cast<MapBox*>(u.p);
    if (b->rc.load(std::memory_order_acquire) > 1) { auto* nb = new MapBox(b->m); release(); u.p = nb; b = nb; }
    return b->m;
}

// int64 arithmetic for `+ - * %` and unary `-` on two Ints, shared by both backends so they can't drift
// plain signed overflow is undefined behavior, and INT64_MIN % -1 raises SIGFPE and kills the process
// so: a result that doesn't fit in int64 widens to a double (like an integer literal that overflows),
// and INT64_MIN % -1 is 0
inline Value intAdd(int64_t a, int64_t b) {
    int64_t r; if (__builtin_add_overflow(a, b, &r)) return Value((double)a + (double)b); return Value(r);
}
inline Value intSub(int64_t a, int64_t b) {
    int64_t r; if (__builtin_sub_overflow(a, b, &r)) return Value((double)a - (double)b); return Value(r);
}
inline Value intMul(int64_t a, int64_t b) {
    int64_t r; if (__builtin_mul_overflow(a, b, &r)) return Value((double)a * (double)b); return Value(r);
}
inline Value intMod(int64_t a, int64_t b) {   // b != 0, checked by the caller
    return Value(b == -1 ? (int64_t)0 : a % b);
}
// the bit operators & | ^ << >> on two ints (64 bit two's complement). false when the shift count is outside 0..63. << wraps (it is
// done on the unsigned bits), >> keeps the sign
// op is '&', '|', '^', 'l' for << or 'r' for >>
inline bool bitOpInts(char op, int64_t a, int64_t b, int64_t& out) {
    if (op == '&') { out = a & b; return true; }
    if (op == '|') { out = a | b; return true; }
    if (op == '^') { out = a ^ b; return true; }
    if (b < 0 || b > 63) return false;
    if (op == 'l') { out = (int64_t)((uint64_t)a << b); return true; }
    if (op == 'r') { out = a >> b; return true; }
    return false;
}
inline Value intNeg(int64_t a) {
    return a == INT64_MIN ? Value(-(double)a) : Value(-a);
}

// checked conversion of a script number to int64_t for natives. a raw (int64_t)d on NaN/Inf/out-of-range
// is undefined behaviour (and often a huge allocation or a wrapped-around size_t), so every native that
// turns a script number into a count, size, index or code point should go through this: it raises a
// catchable error instead. truncates toward zero, like the plain casts it replaces.
// the names are only used for the error, so a literal does not build a std::string on every call
inline int64_t numToInt64(const Value& v, const char* fn, const char* what) {
    if (v.isInt()) return v.asInt();
    double d = v.asNumber();
    if (!(d >= -9223372036854775808.0 && d < 9223372036854775808.0))
        raiseError(fn, std::string(what) + " must be a finite number in integer range, got " + v.formatAsString());
    return (int64_t)d;
}
inline int64_t numToInt64(const Value& v, const std::string& fn, const std::string& what) { return numToInt64(v, fn.c_str(), what.c_str()); }
inline int64_t numToInt64(const Value& v, const std::string& fn, const char* what) { return numToInt64(v, fn.c_str(), what); }

// the largest single string / buffer / array a native will build on a script's behalf. without a cap a
// one-line script (`str_repeat("ab", 4000000000000)`, `buffer(1e18)`, `range(0, 1e11)`) makes the process
// exhaust memory or die from an uncatchable std::bad_alloc / std::length_error.
constexpr int64_t kMaxScriptAlloc = 256LL * 1024 * 1024;
inline void checkAllocSize(const char* fn, const char* what, int64_t n) {
    if (n < 0 || n > kMaxScriptAlloc)
        raiseError(fn, std::string(what) + " (" + std::to_string(n) + ") exceeds the limit of " +
                   std::to_string(kMaxScriptAlloc) + " elements/bytes");
}
inline void checkAllocSize(const std::string& fn, const std::string& what, int64_t n) { checkAllocSize(fn.c_str(), what.c_str(), n); }
inline void checkAllocSize(const std::string& fn, const char* what, int64_t n) { checkAllocSize(fn.c_str(), what, n); }

inline std::string valueRepr(const Value& v) {
    if (v.isString()) return '"' + v.asString() + '"';
    return v.formatAsString();
}

// references for in-out parameters
//
// `append(&list, x)` hands the native a reference to the caller's variable instead of a copy of its value. a reference
// is never a script value: the call site makes it just before the call and drops it right after, and the backends
// refuse to let one come back out of a native. two stages:
//   kRefPathTag  what `&a[i]["k"]` evaluates to: the variable's name and the already worked-out keys. it points at
//                nothing yet, so evaluating later arguments can't leave it dangling
//   kRefTag      made by the backend once every argument is in: .ptr is the Value to change in place
// a native with an in-out parameter gets a kRefTag Value and calls inoutTarget() on it. the native must keep the
// target's type and must not run script code (which could resize the container the pointer lives in)
inline constexpr const char* kRefPathTag = "embr.refpath";
inline constexpr const char* kRefTag     = "embr.ref";

struct RefPath {
    int                nameIdx = 0;   // string-table index (VM) of the variable
    int                slot    = -1;  // VM local slot, -1 for a global
    std::string        name;          // the same name, for error messages
    std::vector<Value> keys;          // one per [..] after the name
};

// the containers between the variable and the target, outermost first, and what each one's child nestDepth was
// before the call. settleRefs() uses it to fix their nestDepth afterwards
struct RefChain {
    struct Level { Value* container; uint32_t childDepth; };
    std::vector<Level> above;
};

inline bool isRefPath(const Value& v) { return v.isPointer() && v.asPointer().type == kRefPathTag; }
inline bool isRef(const Value& v)     { return v.isPointer() && v.asPointer().type == kRefTag; }
inline RefPath& refPathOf(const Value& v) { return *static_cast<RefPath*>(v.asPointer().ptr); }
inline Value&   refTarget(const Value& v) { return *static_cast<Value*>(v.asPointer().ptr); }

inline Value makeRefPath(RefPath p) {
    auto sp = std::make_shared<RefPath>(std::move(p));
    return Value(TypedPtr(sp.get(), kRefPathTag, sp));
}

// follows path.keys down from `root` (the variable). returns the target, or null with `err` set. arrays are indexed in
// range, maps by a key they hold themselves (not through __proto__): a reference must name something that exists
inline Value* resolveRefPath(Value* root, const RefPath& path, std::shared_ptr<RefChain>& chain, std::string& err) {
    Value* cur = root;
    for (const Value& key : path.keys) {
        if (!chain) chain = std::make_shared<RefChain>();
        chain->above.push_back({cur, 0});
        if (cur->isArray()) {
            if (!key.isNumeric()) { err = "array index must be a number"; return nullptr; }
            int64_t i = (int64_t)key.asNumber();
            auto& a = cur->arrayMut();
            if (i < 0 || i >= (int64_t)a.size()) { err = "array index " + std::to_string(i) + " out of bounds"; return nullptr; }
            cur = &a[(size_t)i];
        } else if (cur->isMap()) {
            if (!key.isString()) { err = "map key must be string"; return nullptr; }
            auto& m = cur->mapMut();
            auto it = m.find(key.asString());
            if (it == m.end()) { err = "key not found: " + key.asString(); return nullptr; }
            cur = &it->second;
        } else {
            err = "cannot take a reference into " + cur->typeName();
            return nullptr;
        }
        chain->above.back().childDepth = cur->nestDepth;
    }
    return cur;
}

inline Value makeRef(Value* target, std::shared_ptr<RefChain> chain) {
    return Value(TypedPtr(target, kRefTag, std::move(chain)));
}

// for natives: the Value an in-out parameter points at. `incoming` is the nestDepth of anything the native is about
// to put into it, so a reference deep inside a structure can't push the whole thing past the depth limit
inline Value& inoutTarget(const Value& ref, uint32_t incoming = 0) {
    if (!isRef(ref)) raiseError("value", "internal error: in-out parameter without a reference");
    const TypedPtr& p = ref.asPointer();
    size_t above = p.h ? static_cast<RefChain*>(p.ownerPtr())->above.size() : 0;
    if ((size_t)incoming + 1 + above > Value::kMaxValueDepth)
        raiseError("value", "value nested too deeply (max depth " + std::to_string(Value::kMaxValueDepth) + ")");
    return *static_cast<Value*>(p.ptr);
}

// brings the nestDepth of the containers in `chain` up to date once the value under them (now `childDepth` deep) has changed
inline void settleChain(const RefChain& chain, uint32_t childDepth) {
    for (size_t i = chain.above.size(); i-- > 0; ) {
        Value* c = chain.above[i].container;
        c->noteReplaced(chain.above[i].childDepth, childDepth);
        childDepth = c->nestDepth;
    }
}

// after a call: the containers above each reference's target get their nestDepth brought up to date
inline void settleRefs(const std::vector<Value>& args) {
    for (const Value& a : args) {
        if (!isRef(a)) continue;
        const TypedPtr& p = a.asPointer();
        if (p.h) settleChain(*static_cast<RefChain*>(p.ownerPtr()), static_cast<Value*>(p.ptr)->nestDepth);
    }
}

// Value::map_type is unordered_map (hash order, unspecified/unstable across runs);
// callers that need reproducible iteration (formatAsString, keys()/values()/globals())
// go through this instead of iterating the map directly
inline std::vector<std::string> sortedMapKeys(const Value::map_type& m) {
    std::vector<std::string> keys;
    keys.reserve(m.size());
    for (const auto& [k, v] : m) keys.push_back(k);
    std::sort(keys.begin(), keys.end());
    return keys;
}

// a plain lexical scope layer: a name -> Value map. always held through a shared_ptr, never by value, so a
// closure that captures a layer shares the same live object (see CaptureFrame below)
using Scope = std::unordered_map<std::string, Value>;

// one captured variable. a local that an inner function uses lives in a cell, not in the frame's slot, so the frame and every
// closure that captured it share the same variable. a cell is made when the variable is defined, so a loop variable gets a
// fresh cell per iteration. `defined` is false for a cell made early (by a closure that was created before the variable's
// definition ran), which is what lets two nested functions call each other
struct Cell {
    Value v;
    bool  defined = false;
};

// a closure's captured environment
//   upvals  the cells of the enclosing functions' variables it uses, in the order the compiler numbered them. a captured name
//           is an index here, so reading it never looks anything up by name. the cells are shared, never copied, so sibling
//           closures and the enclosing function see each other's changes
//   layers  only a module's own top scope, when the function was written in an imported module. module-level names live in
//           that scope, not in cells. the scope is shared, so an exported closure keeps its module's names alive
// more on the wiki, scoping-and-closures
struct CaptureFrame {
    std::shared_ptr<std::vector<std::shared_ptr<Cell>>> upvals;   // null when the function captured no variables
    std::vector<std::shared_ptr<Scope>> layers;
    // the same scopes as a gc root entry, built once when the capture is made so a call doesn't build one
    std::shared_ptr<std::vector<std::shared_ptr<Scope>>> gcRoots;
    // some layer is a module's own scope, so a name the compiler found in no enclosing function can still live in
    // a captured layer, and a lookup can't go straight to the globals
    bool hasModule = false;

    // innermost-first lookup across every captured layer. returns a
    // pointer into the live shared layer (not a copy), so *find(name) = v changes the object every other
    // reference to that layer sees. that is the whole point. null if name isn't in any captured layer
    Value* find(const std::string& name) const {
        for (auto it = layers.rbegin(); it != layers.rend(); ++it) {
            auto f = (*it)->find(name);
            if (f != (*it)->end()) return &f->second;
        }
        return nullptr;
    }
};

// what `try ... catch e ... end` binds e to, on both backends: a map, so a script can tell an assertion failure
// from a type error without matching message text. e["message"] gives the plain text
//   message: the raw string given to raiseError()/error()
//   kind:    raiseError()'s first argument ("assert", "error", "runtime", "value", "parser", "vm", or a plugin's
//            own tag like "ffi_call")
//   line:    the source line, or 0 if unknown (embr has no nil type, see the header of
//            plugins/embrtypes/embrtypes.cpp, so 0 means "no line", same as SourceRange)
inline Value errorToMap(const EmbrError& e) {
    Value::map_type m;
    m["message"] = Value(e.message);
    m["kind"]    = Value(e.context);
    m["line"]    = Value((double)(e.hasLocation ? e.range.startLine : 0));
    // call stack, innermost first: [{"fn": name, "file": caller's file, "line": call-site line}, ...];
    // line 0 means "called from native code" (see TraceFrame). empty for an error raised at top level
    Value::array_type trace;
    for (const auto& f : e.trace) {
        Value::map_type fm;
        fm["fn"]   = Value(f.fn);
        fm["file"] = Value(f.file);
        fm["line"] = Value((int64_t)f.line);
        trace.push_back(Value(std::move(fm)));
    }
    m["trace"] = Value(std::move(trace));
    return Value(std::move(m));
}

} // namespace embr

#endif // EMBR_CORE_VALUE_H
