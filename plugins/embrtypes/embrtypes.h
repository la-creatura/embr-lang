#ifndef EMBR_PLUGINS_EMBRTYPES_H
#define EMBR_PLUGINS_EMBRTYPES_H

// the type-descriptor core shared by embrtypes.cpp and plugins/ffi/ffi.cpp, so both read the same TypeDesc layout
// (a plain #include, no runtime link: ffi.so doesn't need embrtypes.so loaded, only the same struct layout to read
// the descriptors a script builds with `import "embrtypes"`)
// everything is `inline` because each plugin that includes this compiles its own copy
// why it's designed this way (typed pointers, how true/false/nil work) is in embrtypes.cpp's header.
// this file only has the mechanics, embrtypes.cpp adds the script-facing functions

#include <embr/embr.h>
#include <memory>
#include <cstring>

namespace embrtypes {

using embr::Value;
using embr::TypedPtr;
using embr::raiseError;

// the fixed address used for true. it's a literal, not `&someGlobal`, so every plugin .so that builds a true
// uses the same bits (a global's address differs per loaded .so). that way a true from this plugin and one
// from json.c compare equal. never dereferenced, only compared. any future truthy-only sentinel should do the same
// see embrtypes.cpp's header for why true/false are a "bool" tagged pointer
inline void* const g_truthySentinel = reinterpret_cast<void*>(1);
inline Value makeBool(bool b) { return Value::makePointer(b ? g_truthySentinel : nullptr, "bool"); }
inline bool  isBool(const Value& v) { return v.isPointer() && v.asPointer().type == "bool"; }

// nil: a null pointer like false, but tagged "nil" instead of "bool". TypedPtr::operator== compares tags, so
// nil and false stay distinct. being null also makes nil falsy (see embrtypes.cpp's header)
inline Value makeNil() { return Value::makePointer(nullptr, "nil"); }
inline bool  isNil(const Value& v) { return v.isPointer() && v.asPointer().type == "nil"; }

// a runtime type descriptor. built recursively (Arr/Map/Union/Struct hold shared_ptrs to more descriptors), so
// arr<arr<int>>, union<ptr, str> and nested structs all come from the same few kinds
//
// I8..F64 are fixed-width numbers. type_check treats them like Int/Float (embr's int is always 64-bit), but only
// they carry a byte size, which struct layout, buffer_pack/buffer_unpack and ffi marshaling need
// Void exists only for ffi's "returns nothing" return slot. there is no void Value, so it matches vacuously and
// scripts use `nil` there instead (that's why there is no type_void() binding)
struct TypeDesc {
    enum class Kind {
        Any, Num, Float, Int, Str, Arr, Map, Fn, Ptr, Union,
        I8, I16, I32, I64, U8, U16, U32, U64, F32, F64,
        Struct, Void,
    };
    Kind kind = Kind::Any;

    std::vector<std::string>               ptrTags;   // Kind::Ptr: allowed tags; empty = any tag
    std::shared_ptr<TypeDesc>              elem;      // Kind::Arr: element type; null = any
    std::shared_ptr<TypeDesc>              valueType; // Kind::Map: value type; null = any
    std::vector<std::shared_ptr<TypeDesc>> alts;      // Kind::Union: must match at least one

    struct Field { std::string name; std::shared_ptr<TypeDesc> type; size_t offset = 0; };
    std::vector<Field> fields;          // Kind::Struct, in declaration order
    size_t             structSize = 0;      // Kind::Struct: packed byte size, 0 if not packable
    size_t             structAlignment = 1; // Kind::Struct: widest member's size, for nesting/ffi_type
};

// byte width of a fixed-width numeric kind, or false for anything else
// (including Struct, callers needing a struct's size read structSize
// directly, since it's precomputed once at struct_type() time).
struct FixedWidth { size_t size; bool isSigned; bool isFloat; };
inline bool fixedWidth(TypeDesc::Kind k, FixedWidth& out) {
    switch (k) {
        case TypeDesc::Kind::I8:  out = {1, true,  false}; return true;
        case TypeDesc::Kind::I16: out = {2, true,  false}; return true;
        case TypeDesc::Kind::I32: out = {4, true,  false}; return true;
        case TypeDesc::Kind::I64: out = {8, true,  false}; return true;
        case TypeDesc::Kind::U8:  out = {1, false, false}; return true;
        case TypeDesc::Kind::U16: out = {2, false, false}; return true;
        case TypeDesc::Kind::U32: out = {4, false, false}; return true;
        case TypeDesc::Kind::U64: out = {8, false, false}; return true;
        case TypeDesc::Kind::F32: out = {4, false, true};  return true;
        case TypeDesc::Kind::F64: out = {8, false, true};  return true;
        default: return false;
    }
}

// the integer `v` stands for, when it fits a fixed-width integer type exactly. anything that would lose bytes on the way
// into memory raises: a fraction, a negative value for an unsigned type, a value outside the type's range. buffer_pack,
// ptr_write and every ffi integer (arguments and callback results) go through here. `what` names the value in the message
inline int64_t intForWidth(const Value& v, const FixedWidth& w, const std::string& tname, const std::string& ctx,
                           const std::string& what) {
    if (!v.isNumeric()) raiseError(ctx, what + ": expects " + tname + ", got " + v.typeName());
    int64_t iv;
    if (v.isInt()) {
        iv = v.asInt();
    } else {
        const double x = v.asNumber();
        if (!(x >= -9223372036854775808.0 && x < 9223372036854775808.0))   // 2^63 is exact as a double
            raiseError(ctx, what + ": number " + std::to_string(x) + " is out of integer range");
        iv = (int64_t)x;
        if ((double)iv != x)
            raiseError(ctx, what + ": type " + tname + " is integer but got non-integer value " + std::to_string(x));
    }
    if (!w.isSigned && iv < 0) raiseError(ctx, what + ": type " + tname + " is unsigned but got negative value");
    if (w.size < 8) {
        const int bits = (int)w.size * 8;
        const int64_t lo = w.isSigned ? -(int64_t(1) << (bits - 1)) : 0;
        const int64_t hi = w.isSigned ? (int64_t(1) << (bits - 1)) - 1 : (int64_t(1) << bits) - 1;
        if (iv < lo || iv > hi)
            raiseError(ctx, what + ": " + std::to_string(iv) + " does not fit in type " + tname +
                       " (" + std::to_string(lo) + " to " + std::to_string(hi) + ")");
    }
    return iv;
}

inline Value wrapDesc(TypeDesc d) {
    auto p = std::make_shared<TypeDesc>(std::move(d));
    return Value(TypedPtr(p.get(), "types.desc", p));
}

// aggregate-init with only `kind` set trips -Wmissing-field-initializers
// (the rest have default member initializers, but GCC still wants them
// listed), this sidesteps it for the common "just a kind" case.
inline TypeDesc kindOnly(TypeDesc::Kind k) {
    TypeDesc d;
    d.kind = k;
    return d;
}

// returns a pointer, not a reference: GCC's -Wdangling-reference gives a false positive on references taken
// straight from this return value, which is harmless. the TypeDesc is the descriptor Value's own long-lived payload
inline const TypeDesc* requireDesc(const Value& v, const std::string& caller, const std::string& argname) {
    if (!v.isPointer() || v.asPointer().type != "types.desc")
        raiseError(caller, "'" + argname + "' must be a type descriptor "
                   "(type_num(), type_arr(...), type_ptr(...), ...) got " + v.typeName());
    return v.userdata<TypeDesc>("types.desc");
}

inline bool matches(const TypeDesc& d, const Value& v) {
    switch (d.kind) {
        case TypeDesc::Kind::Any:   return true;
        case TypeDesc::Kind::Void:  return true; // no such thing as a void Value; vacuous
        case TypeDesc::Kind::Num:   return v.isNumeric();
        case TypeDesc::Kind::Float: return v.isNumber();
        case TypeDesc::Kind::Int:   return v.isInt();
        case TypeDesc::Kind::Str:   return v.isString();
        case TypeDesc::Kind::Fn:    return v.isCallable();

        case TypeDesc::Kind::Ptr: {
            if (!v.isPointer()) return false;
            if (d.ptrTags.empty()) return true;
            const std::string& tag = v.asPointer().type;
            for (auto& t : d.ptrTags) if (t == tag) return true;
            return false;
        }
        case TypeDesc::Kind::Arr: {
            if (!v.isArray()) return false;
            if (!d.elem) return true;
            for (auto& item : v.asArray())
                if (!matches(*d.elem, item)) return false;
            return true;
        }
        case TypeDesc::Kind::Map: {
            if (!v.isMap()) return false;
            if (!d.valueType) return true;
            for (auto& [k, val] : v.asMap())
                if (!matches(*d.valueType, val)) return false;
            return true;
        }
        case TypeDesc::Kind::Union:
            for (auto& alt : d.alts)
                if (matches(*alt, v)) return true;
            return false;

        // fixed-width numeric kinds are shape-equivalent to Int/Float for a
        // script Value (embr's int is always 64-bit), the width only
        // matters for struct_type()'s layout math, buffer packing, and ffi
        case TypeDesc::Kind::I8:  case TypeDesc::Kind::I16:
        case TypeDesc::Kind::I32: case TypeDesc::Kind::I64:
        case TypeDesc::Kind::U8:  case TypeDesc::Kind::U16:
        case TypeDesc::Kind::U32: case TypeDesc::Kind::U64:
            return v.isInt();
        case TypeDesc::Kind::F32: case TypeDesc::Kind::F64:
            return v.isNumber();

        case TypeDesc::Kind::Struct: {
            if (!v.isMap()) return false;
            const auto& m = v.asMap();
            for (auto& f : d.fields) {
                auto it = m.find(f.name);
                if (it == m.end() || !matches(*f.type, it->second)) return false;
            }
            return true;
        }
    }
    return false;
}

// human-readable rendering, also used (compared as strings) by type_of() to
// decide whether every element/value it saw infers to "the same" type
inline std::string describe(const TypeDesc& d) {
    switch (d.kind) {
        case TypeDesc::Kind::Any:   return "any";
        case TypeDesc::Kind::Void:  return "void";
        case TypeDesc::Kind::Num:   return "num";
        case TypeDesc::Kind::Float: return "float";
        case TypeDesc::Kind::Int:   return "int";
        case TypeDesc::Kind::Str:   return "str";
        case TypeDesc::Kind::Fn:    return "fn";
        case TypeDesc::Kind::Ptr: {
            if (d.ptrTags.empty()) return "ptr";
            std::string s = "ptr<";
            for (size_t i = 0; i < d.ptrTags.size(); ++i) { if (i) s += "|"; s += d.ptrTags[i]; }
            return s + ">";
        }
        case TypeDesc::Kind::Arr:
            return d.elem ? "arr<" + describe(*d.elem) + ">" : "arr";
        case TypeDesc::Kind::Map:
            return d.valueType ? "map<" + describe(*d.valueType) + ">" : "map";
        case TypeDesc::Kind::Union: {
            std::string s = "union<";
            for (size_t i = 0; i < d.alts.size(); ++i) { if (i) s += "|"; s += describe(*d.alts[i]); }
            return s + ">";
        }
        case TypeDesc::Kind::I8:  return "i8";
        case TypeDesc::Kind::I16: return "i16";
        case TypeDesc::Kind::I32: return "i32";
        case TypeDesc::Kind::I64: return "i64";
        case TypeDesc::Kind::U8:  return "u8";
        case TypeDesc::Kind::U16: return "u16";
        case TypeDesc::Kind::U32: return "u32";
        case TypeDesc::Kind::U64: return "u64";
        case TypeDesc::Kind::F32: return "f32";
        case TypeDesc::Kind::F64: return "f64";
        case TypeDesc::Kind::Struct: {
            std::string s = "struct<";
            for (size_t i = 0; i < d.fields.size(); ++i) {
                if (i) s += ",";
                s += d.fields[i].name + ":" + describe(*d.fields[i].type);
            }
            return s + ">";
        }
    }
    return "?";
}

// size and alignment of a field that has a fixed layout: a fixed-width number, a pointer (any `ptr` field, pointer-sized)
// or a packable struct (its own alignment, not its size). false for anything else
inline bool fieldLayout(const TypeDesc& t, size_t& size, size_t& align) {
    FixedWidth w;
    if (fixedWidth(t.kind, w)) { size = align = w.size; return true; }
    if (t.kind == TypeDesc::Kind::Ptr) { size = align = sizeof(void*); return true; }
    if (t.kind == TypeDesc::Kind::Struct && t.structSize > 0) { size = t.structSize; align = t.structAlignment; return true; }
    return false;
}

// fills in a Struct descriptor's fields and layout. offsets use natural alignment (each field at the next offset
// divisible by its alignment, the struct padded to its widest member). if any field has no fixed layout (see
// fieldLayout), structSize stays 0 ("not packable") instead of raising, so struct_type() still works for plain
// shape checks. shared by struct_type() and any plugin (ffi.cpp) that builds one in C++
inline void computeStructLayout(TypeDesc& d) {
    size_t offset = 0, maxAlign = 1;
    bool packable = true;
    for (auto& f : d.fields) {
        size_t fsize, align;
        if (!fieldLayout(*f.type, fsize, align)) { packable = false; break; }
        offset = (offset + align - 1) / align * align;
        f.offset = offset;
        offset += fsize;
        if (align > maxAlign) maxAlign = align;
    }
    if (packable) {
        d.structSize      = (offset + maxAlign - 1) / maxAlign * maxAlign;
        d.structAlignment = maxAlign;
    }
}

// the byte size a descriptor packs into, or a raised error if it can't
// (only fixed-width numeric kinds and structs built entirely from packable
// fields qualify, see struct_type()'s doc comment in embrtypes.cpp)
inline size_t packedSize(const TypeDesc& d, const char* ctx) {
    FixedWidth w;
    if (fixedWidth(d.kind, w)) return w.size;
    if (d.kind == TypeDesc::Kind::Ptr) return sizeof(void*);
    if (d.kind == TypeDesc::Kind::Struct && d.structSize > 0) return d.structSize;
    raiseError(ctx, "type '" + describe(d) + "' has no fixed binary size "
               "(only i8..f64 and ptr fields, and structs built entirely from them, can be packed)");
}

// Value -> raw memory, per a packable descriptor (fixed-width numeric or a
// packable struct). recurses into nested struct fields.
inline void packInto(uint8_t* dst, const TypeDesc& d, const Value& v, const char* ctx, const std::string& what = "value") {
    if (d.kind == TypeDesc::Kind::Struct) {
        if (!v.isMap())
            raiseError(ctx, "expected a map for struct '" + describe(d) + "', got " + v.typeName());
        const auto& m = v.asMap();
        for (auto& f : d.fields) {
            auto it = m.find(f.name);
            if (it == m.end())
                raiseError(ctx, "struct '" + describe(d) + "' missing field '" + f.name + "'");
            packInto(dst + f.offset, *f.type, it->second, ctx, "field '" + f.name + "'");
        }
        return;
    }
    if (d.kind == TypeDesc::Kind::Ptr) {
        if (!v.isPointer()) raiseError(ctx, "expected a ptr for " + what + ", got " + v.typeName());
        void* p = v.asPointer().ptr;
        memcpy(dst, &p, sizeof(void*));
        return;
    }
    FixedWidth w;
    if (!fixedWidth(d.kind, w))
        raiseError(ctx, "type '" + describe(d) + "' cannot be packed");
    if (w.isFloat) {
        if (!v.isNumeric()) raiseError(ctx, "expected a number for " + describe(d) + ", got " + v.typeName());
        double x = v.asNumber();
        if (w.size == 4) { float f = (float)x; memcpy(dst, &f, 4); }
        else              {                     memcpy(dst, &x, 8); }
        return;
    }
    int64_t iv = intForWidth(v, w, describe(d), ctx, what);
    switch (w.size) {
        case 1: { uint8_t  x = (uint8_t) iv; memcpy(dst, &x, 1); } break;
        case 2: { uint16_t x = (uint16_t)iv; memcpy(dst, &x, 2); } break;
        case 4: { uint32_t x = (uint32_t)iv; memcpy(dst, &x, 4); } break;
        case 8: { uint64_t x = (uint64_t)iv; memcpy(dst, &x, 8); } break;
    }
}

// raw memory -> Value, the inverse of packInto
inline Value unpackFrom(const uint8_t* src, const TypeDesc& d, const char* ctx) {
    if (d.kind == TypeDesc::Kind::Struct) {
        Value::map_type m;
        for (auto& f : d.fields) m[f.name] = unpackFrom(src + f.offset, *f.type, ctx);
        return Value(std::move(m));
    }
    if (d.kind == TypeDesc::Kind::Ptr) {
        void* p; memcpy(&p, src, sizeof(void*));
        return Value::makePointer(p);
    }
    FixedWidth w;
    if (!fixedWidth(d.kind, w))
        raiseError(ctx, "type '" + describe(d) + "' cannot be unpacked");
    if (w.isFloat) {
        if (w.size == 4) { float  f; memcpy(&f, src, 4); return Value((double)f); }
        else              { double f; memcpy(&f, src, 8); return Value(f); }
    }
    if (w.isSigned) {
        switch (w.size) {
            case 1: { int8_t  x; memcpy(&x, src, 1); return Value((int64_t)x); }
            case 2: { int16_t x; memcpy(&x, src, 2); return Value((int64_t)x); }
            case 4: { int32_t x; memcpy(&x, src, 4); return Value((int64_t)x); }
            case 8: { int64_t x; memcpy(&x, src, 8); return Value(x); }
        }
    } else {
        switch (w.size) {
            case 1: { uint8_t  x; memcpy(&x, src, 1); return Value((int64_t)x); }
            case 2: { uint16_t x; memcpy(&x, src, 2); return Value((int64_t)x); }
            case 4: { uint32_t x; memcpy(&x, src, 4); return Value((int64_t)x); }
            case 8: { uint64_t x; memcpy(&x, src, 8); return Value((int64_t)x); }
        }
    }
    raiseError(ctx, "unreachable: unsupported width " + std::to_string(w.size));
}

// a fixed-size, mutable byte buffer. purely an embr value, no connection to ffi. useful for binary data
// (protocols, file formats, struct pack/unpack) and as ffi's out-parameter / scratch memory
// reference counted: freed when the last Value using it goes away
struct Buffer { std::vector<uint8_t> bytes; };

// the Value's pointer is buf.data() itself, not the Buffer struct's address, so it works directly as a raw
// memory address with ptr_read/ptr_write/ffi_call. so requireBuffer() can't use the usual .userdata<T>()
// (which assumes .ptr is the T*) and finds the Buffer through the TypedPtr's owner instead
// never resize an existing Buffer's `bytes`: that can reallocate and invalidate every .ptr already handed out
inline Value makeBuffer(std::vector<uint8_t> bytes) {
    auto p = std::make_shared<Buffer>(Buffer{std::move(bytes)});
    void* data = p->bytes.data();
    return Value(TypedPtr(data, "types.buffer", p));
}
inline Buffer* requireBuffer(const Value& v, const std::string& caller, const std::string& argname) {
    if (!v.isPointer() || v.asPointer().type != "types.buffer")
        raiseError(caller, "'" + argname + "' must be a buffer (from buffer()/buffer_from_str()), got " + v.typeName());
    return static_cast<Buffer*>(v.asPointer().ownerPtr());
}

} // namespace embrtypes

#endif // EMBR_PLUGINS_EMBRTYPES_H
