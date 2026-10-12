#ifndef EMBR_CORE_TYPES_H
#define EMBR_CORE_TYPES_H

// the embr value-type tag system
// the TypeTag/TypeSet bitmask and the Param struct used to describe native + script function signatures

#include <cstdint>
#include <string>
#include <memory>

namespace embr {

enum class TypeTag : uint8_t {
    Number   = 1 << 0,  // f64 float
    Int      = 1 << 1,  // i64 exact integer
    String   = 1 << 2,
    Array    = 1 << 3,
    Map      = 1 << 4,
    Callable = 1 << 5,
    Pointer  = 1 << 6,
};

struct TypeSet {
    uint8_t mask;

    constexpr TypeSet()                    : mask(0)              {}
    constexpr explicit TypeSet(uint8_t m)  : mask(m)              {}
    constexpr TypeSet(TypeTag t)           : mask((uint8_t)t)     {}

    static constexpr TypeSet Any()         { return TypeSet(0xFF); }
    static constexpr TypeSet None()        { return TypeSet(0x00); }
    static constexpr TypeSet of(TypeTag t) { return TypeSet(t);    }

    constexpr TypeSet operator|(TypeSet  o) const { return TypeSet(uint8_t(mask | o.mask)); }
    constexpr TypeSet operator|(TypeTag  t) const { return TypeSet(uint8_t(mask | (uint8_t)t)); }
    constexpr bool contains(TypeTag t)      const { return mask & (uint8_t)t; }
    constexpr bool isAny()                  const { return mask == 0xFF; }
    constexpr bool isNone()                 const { return mask == 0; }
    constexpr bool operator==(TypeSet o)    const { return mask == o.mask; }
    constexpr bool operator!=(TypeSet o)    const { return mask != o.mask; }

    std::string name() const {
        if (isAny()) return "any";
        struct Row { TypeTag tag; const char* nm; };
        static constexpr Row kTypes[] = {
            {TypeTag::Number,   "number"},
            {TypeTag::Int,      "int"},
            {TypeTag::String,   "string"},
            {TypeTag::Array,    "array"},
            {TypeTag::Map,      "map"},
            {TypeTag::Callable, "function"},
            {TypeTag::Pointer,  "pointer"}
        };
        std::string s;
        for (auto& r : kTypes)
            if (contains(r.tag)) { if (!s.empty()) s += '|'; s += r.nm; }
        return s.empty() ? "?" : s;
    }

    // num is a supertype (Number|Int)
    // float/double and int specify the exact type
    static TypeSet fromName(const std::string& n) {
        if (n == "num" || n == "number")                  return TypeSet(TypeTag::Number) | TypeSet(TypeTag::Int);
        if (n == "float" || n == "double")                return TypeSet(TypeTag::Number);
        if (n == "int" || n == "integer")                 return TypeSet(TypeTag::Int);
        if (n == "str" || n == "string")                  return TypeSet(TypeTag::String);
        if (n == "arr" || n == "array")                   return TypeSet(TypeTag::Array);
        if (n == "map")                                   return TypeSet(TypeTag::Map);
        if (n == "fn"  || n == "func" || n == "function") return TypeSet(TypeTag::Callable);
        if (n == "ptr" || n == "pointer")                 return TypeSet(TypeTag::Pointer);
        if (n == "any")                                   return TypeSet::Any();
        return TypeSet::None();
    }
};

namespace TS {
    inline constexpr TypeSet Float = TypeSet(TypeTag::Number);
    inline constexpr TypeSet Int   = TypeSet(TypeTag::Int);
    inline constexpr TypeSet Num   = TypeSet(TypeTag::Number) | TypeSet(TypeTag::Int); // any numeric (float or int)
    inline constexpr TypeSet Str   = TypeSet(TypeTag::String);
    inline constexpr TypeSet Arr   = TypeSet(TypeTag::Array);
    inline constexpr TypeSet Map   = TypeSet(TypeTag::Map);
    inline constexpr TypeSet Fn    = TypeSet(TypeTag::Callable);
    inline constexpr TypeSet Ptr   = TypeSet(TypeTag::Pointer);
    inline constexpr TypeSet Any   = TypeSet(0xFF);
}

// the type of what a container holds, when it is written or worked out (`arr<int>`, `map<str>`, `arr<arr<float>>`). `el` is null when it is not known.
// only the static checker (typecheck.h) looks at it: at run time an array is an array
struct TypeAnn;
using TypeElem = std::shared_ptr<const TypeAnn>;
struct TypeAnn { TypeSet t = TypeSet::Any(); TypeElem el; };

struct Param {
    std::string name;
    TypeSet     type     = TS::Any;
    bool        optional = false;
    bool        variadic = false;
    // a native parameter the function changes where the caller keeps it. the call site must pass `&name`
    // (see "in-out parameters" on the wiki architecture page). only natives can have one
    bool        inout    = false;
    // the type was written in the source (`a: any` counts, an omitted type does not). the static checker uses it
    bool        explicitType = false;
    // the type of the elements, for an `arr<int>` parameter. for a variadic one the written type is the type of each argument, so this stays null
    TypeElem    elem = nullptr;
    // a native: this argument has to fit what the array (or map) passed as argument number `elemOf` holds. `push(&list, x)` is req/io list, then x with elemOf 0
    int         elemOf = -1;

    static Param req (std::string nm, TypeSet t = TypeSet::Any()) { return {std::move(nm), t, false, false}; }
    static Param opt (std::string nm, TypeSet t = TypeSet::Any()) { return {std::move(nm), t, true,  false}; }
    static Param rest(std::string nm, TypeSet t = TypeSet::Any()) { return {std::move(nm), t, true,  true }; }
    static Param io  (std::string nm, TypeSet t = TypeSet::Any()) { return {std::move(nm), t, false, false, true}; }
};

} // namespace embr

#endif // EMBR_CORE_TYPES_H
