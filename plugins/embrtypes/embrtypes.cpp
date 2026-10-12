// embrtypes.cpp
// runtime type descriptors for embr, plus true/false/nil
//
// usage
//   import "embrtypes"
//
// the built-in Param/TypeSet signatures (core/types.h) are a fixed bitmask over the seven core types. they can't say
// "array of ints" or "a pointer with one of these tags". a TypeDesc here is ordinary runtime data a script builds
// and checks values against with type_check/type_assert:
//
//   type_arr(type_int())                    # an array of ints
//   type_map(type_str())                    # a map whose values are all strings
//   type_ptr("ffi.lib", "ffi.sym")          # a pointer tagged either way
//   type_union([type_int(), type_str()])    # an int or a string
//   struct_type([["x", type_i32()], ["y", type_i32()]])  # named, heterogeneous
//
// the TypeDesc representation, matches()/describe() and buffer packing live in embrtypes.h, so plugins/ffi/ffi.cpp
// can #include it and share the same descriptors (a plain compile-time include, no runtime link between the .so files)
//
// a TypeDesc is a typed pointer (tag "types.desc", see TypedPtr in core/value.h), not a map, so a script can't forge one
//
// true, false and nil (embr's core has no bool or nil type, on purpose)
//   - they are tagged pointers. true/false use the tag "bool": false is a null pointer, true is a non-null one
//     (a fixed address, g_truthySentinel in embrtypes.h, not the address of a global, which would differ per .so),
//     so the normal rule "a non-null pointer is truthy" just works
//   - nil is a null pointer tagged "nil". same address as false but a different tag, and TypedPtr::operator==
//     compares tags, so nil != false. nil is falsy like any null pointer
//   - print(true) shows <bool 0x1>, not "true". a script that wants text spells it out
//   - this is a convention, not a link between plugins: plugins/json/json.c builds its own true/false/nil the same
//     way (same tags, same sentinel), so a value from either plugin is recognized by the other and compares equal.
//     keep the two in sync if this representation ever changes. ffi.cpp reuses nil (via embrtypes.h) as
//     "this call returns nothing"

#include "embrtypes.h"
#include <functional>
#include <algorithm>

using namespace embr;
using namespace embrtypes;

static Param pAny (std::string n)                      { return Param::req(std::move(n), TS::Any); }
static Param pNum (std::string n)                      { return Param::req(std::move(n), TS::Num); }
static Param pStr (std::string n)                      { return Param::req(std::move(n), TS::Str); }
static Param pArr (std::string n)                      { return Param::req(std::move(n), TS::Arr); }
static Param pPtr (std::string n)                      { return Param::req(std::move(n), TS::Ptr); }
static Param pOpt (std::string n, TypeSet m = TS::Any) { return Param::opt(std::move(n), m);       }
static Param pRestStr(std::string n)                   { return Param::rest(std::move(n), TS::Str); }

static void checkBufIndex(const std::string& caller, const Buffer& b, int64_t i) {
    if (i < 0 || (size_t)i >= b.bytes.size())
        raiseError(caller, "index " + std::to_string(i) + " out of range for a " +
                   std::to_string(b.bytes.size()) + "-byte buffer");
}

EMBR_PLUGIN {
    // true / false: boolean literals as plain global values (see the file
    // header comment for why these are a "bool" tagged pointer, not a new
    // core type). truthy() already treats them correctly for free.
    interp->define("true",  makeBool(true));
    interp->define("false", makeBool(false));

    // make_true() / make_false() -> fn
    // some embrlib functions (while_do) need the condition to be a callable, re-evaluated each time, not a plain value.
    // these return a zero-arg native that always returns true/false. while_do(make_true(), body) loops until body
    // breaks out some other way, while_do(make_false(), body) never runs
    interp->bind("make_true",
    [](const std::vector<Value>&) -> Value {
        return Value::makeNative("true", [](const std::vector<Value>&) -> Value { return makeBool(true); });
    });
    interp->bind("make_false",
    [](const std::vector<Value>&) -> Value {
        return Value::makeNative("false", [](const std::vector<Value>&) -> Value { return makeBool(false); });
    });

    // nil: a value distinct from every other value, including false, and
    // falsy (see the file header comment).
    interp->define("nil", makeNil());

    // is_nil(v) -> int (0 or 1)
    interp->bindSig("is_nil", {pAny("v")},
    [](const std::vector<Value>& args) -> Value { return Value(isNil(args[0]) ? (int64_t)1 : (int64_t)0); });

    interp->bind("type_any",   [](const std::vector<Value>&) -> Value { return wrapDesc(kindOnly(TypeDesc::Kind::Any));   });
    interp->bind("type_num",   [](const std::vector<Value>&) -> Value { return wrapDesc(kindOnly(TypeDesc::Kind::Num));   });
    interp->bind("type_float", [](const std::vector<Value>&) -> Value { return wrapDesc(kindOnly(TypeDesc::Kind::Float)); });
    interp->bind("type_int",   [](const std::vector<Value>&) -> Value { return wrapDesc(kindOnly(TypeDesc::Kind::Int));   });
    interp->bind("type_str",   [](const std::vector<Value>&) -> Value { return wrapDesc(kindOnly(TypeDesc::Kind::Str));   });
    interp->bind("type_fn",    [](const std::vector<Value>&) -> Value { return wrapDesc(kindOnly(TypeDesc::Kind::Fn));    });

    // type_bool() -> desc
    //
    // shorthand for type_ptr("bool"); matches true/false from either this
    // plugin or json.c's own true/false (same tag convention, see above).
    interp->bind("type_bool",
    [](const std::vector<Value>&) -> Value {
        TypeDesc d; d.kind = TypeDesc::Kind::Ptr; d.ptrTags.push_back("bool");
        return wrapDesc(std::move(d));
    });

    // type_nil() -> desc
    //
    // shorthand for type_ptr("nil"); matches nil from either this plugin or
    // json.c's own null (same tag convention, see the file header comment).
    interp->bind("type_nil",
    [](const std::vector<Value>&) -> Value {
        TypeDesc d; d.kind = TypeDesc::Kind::Ptr; d.ptrTags.push_back("nil");
        return wrapDesc(std::move(d));
    });

    // type_buffer() -> desc
    //
    // shorthand for type_ptr("types.buffer"); matches any buffer.
    interp->bind("type_buffer",
    [](const std::vector<Value>&) -> Value {
        TypeDesc d; d.kind = TypeDesc::Kind::Ptr; d.ptrTags.push_back("types.buffer");
        return wrapDesc(std::move(d));
    });

    // type_i8/i16/i32/i64/u8/u16/u32/u64/f32/f64() -> desc
    // fixed-width numeric descriptors. type_check treats them like type_int()/type_float() (embr numbers have no
    // narrower width). only these carry a byte size, which struct_type() layout, buffer_pack/buffer_unpack and
    // ffi marshaling need
    interp->bind("type_i8",  [](const std::vector<Value>&) -> Value { return wrapDesc(kindOnly(TypeDesc::Kind::I8));  });
    interp->bind("type_i16", [](const std::vector<Value>&) -> Value { return wrapDesc(kindOnly(TypeDesc::Kind::I16)); });
    interp->bind("type_i32", [](const std::vector<Value>&) -> Value { return wrapDesc(kindOnly(TypeDesc::Kind::I32)); });
    interp->bind("type_i64", [](const std::vector<Value>&) -> Value { return wrapDesc(kindOnly(TypeDesc::Kind::I64)); });
    interp->bind("type_u8",  [](const std::vector<Value>&) -> Value { return wrapDesc(kindOnly(TypeDesc::Kind::U8));  });
    interp->bind("type_u16", [](const std::vector<Value>&) -> Value { return wrapDesc(kindOnly(TypeDesc::Kind::U16)); });
    interp->bind("type_u32", [](const std::vector<Value>&) -> Value { return wrapDesc(kindOnly(TypeDesc::Kind::U32)); });
    interp->bind("type_u64", [](const std::vector<Value>&) -> Value { return wrapDesc(kindOnly(TypeDesc::Kind::U64)); });
    interp->bind("type_f32", [](const std::vector<Value>&) -> Value { return wrapDesc(kindOnly(TypeDesc::Kind::F32)); });
    interp->bind("type_f64", [](const std::vector<Value>&) -> Value { return wrapDesc(kindOnly(TypeDesc::Kind::F64)); });

    // struct_type(fields: arr of [name: str, type: desc]) -> desc
    //
    // a named-field shape. unlike type_map (one type for every value), each field has its own. works with
    // type_check/type_assert on any map, for example to check a parsed JSON object has the fields you expect
    //
    // if every field is a fixed-width number (i8..f64), a ptr or another packable struct_type, offsets are computed
    // with natural C alignment (computeStructLayout in embrtypes.h). such a struct works with buffer_pack/
    // buffer_unpack and as an arg/return type for ffi_call/ffi_prepare/ffi_callback (ffi.cpp builds and caches
    // its libffi type on first use). a ptr field is pointer-sized and holds any pointer. with any other field (str, arr, map, fn, union, untyped) it is still a
    // fine type_check shape, but buffer_pack/buffer_unpack/ffi raise a clear error
    interp->bindSig("struct_type", {pArr("fields")},
    [](const std::vector<Value>& args) -> Value {
        const auto& fieldVals = args[0].asArray();
        if (fieldVals.empty())
            raiseError("struct_type", "a struct needs at least one field");

        TypeDesc d; d.kind = TypeDesc::Kind::Struct;
        d.fields.reserve(fieldVals.size());
        for (size_t i = 0; i < fieldVals.size(); ++i) {
            const std::string label = "fields[" + std::to_string(i) + "]";
            if (!fieldVals[i].isArray() || fieldVals[i].asArray().size() != 2)
                raiseError("struct_type", label + " must be [name: str, type: <type descriptor>]");
            const auto& pair = fieldVals[i].asArray();
            if (!pair[0].isString())
                raiseError("struct_type", label + ": field name must be a string");
            d.fields.push_back({pair[0].asString(),
                std::make_shared<TypeDesc>(*requireDesc(pair[1], "struct_type", label + "[1]")), 0});
        }

        computeStructLayout(d);
        return wrapDesc(std::move(d));
    });

    // buffer(size: num) -> buf
    //
    // a zeroed, fixed-size, mutable byte buffer. see embrtypes.h's Buffer
    // struct doc comment for the ownership model.
    interp->bindSig("buffer", {pNum("size")},
    [](const std::vector<Value>& args) -> Value {
        int64_t n = numToInt64(args[0], "buffer", "size");
        if (n < 0) raiseError("buffer", "size must not be negative");
        checkAllocSize("buffer", "size", n);
        return makeBuffer(std::vector<uint8_t>((size_t)n, 0));
    });

    // buffer_from_str(s: str) -> buf
    interp->bindSig("buffer_from_str", {pStr("s")},
    [](const std::vector<Value>& args) -> Value {
        const std::string& s = args[0].asString();
        return makeBuffer(std::vector<uint8_t>(s.begin(), s.end()));
    });

    // buffer_to_str(buf) -> str
    interp->bindSig("buffer_to_str", {pPtr("buf")},
    [](const std::vector<Value>& args) -> Value {
        auto* b = requireBuffer(args[0], "buffer_to_str", "buf");
        return Value(std::string(b->bytes.begin(), b->bytes.end()));
    });

    // buffer_len(buf) -> num
    interp->bindSig("buffer_len", {pPtr("buf")},
    [](const std::vector<Value>& args) -> Value {
        return Value((int64_t)requireBuffer(args[0], "buffer_len", "buf")->bytes.size());
    });

    // buffer_get(buf, i: num) -> num  (a single byte, 0-255)
    interp->bindSig("buffer_get", {pPtr("buf"), pNum("i")},
    [](const std::vector<Value>& args) -> Value {
        auto* b = requireBuffer(args[0], "buffer_get", "buf");
        int64_t i = numToInt64(args[1], "buffer", "index");
        checkBufIndex("buffer_get", *b, i);
        return Value((int64_t)b->bytes[(size_t)i]);
    });

    // buffer_set(buf, i: num, v: num)  (writes a single byte, v mod 256)
    interp->bindSig("buffer_set", {pPtr("buf"), pNum("i"), pNum("v")},
    [](const std::vector<Value>& args) -> Value {
        auto* b = requireBuffer(args[0], "buffer_set", "buf");
        int64_t i = numToInt64(args[1], "buffer", "index");
        checkBufIndex("buffer_set", *b, i);
        b->bytes[(size_t)i] = (uint8_t)numToInt64(args[2], "buffer_set", "value");
        return Value(0.0);
    });

    // buffer_fill(buf, v: num) -> buf  (fills the whole buffer with v mod 256)
    interp->bindSig("buffer_fill", {pPtr("buf"), pNum("v")},
    [](const std::vector<Value>& args) -> Value {
        auto* b = requireBuffer(args[0], "buffer_fill", "buf");
        std::fill(b->bytes.begin(), b->bytes.end(), (uint8_t)numToInt64(args[1], "buffer_fill", "value"));
        return args[0];
    });

    // buffer_slice(buf, start: num, end?: num) -> buf  (a new, independent copy)
    interp->bindSig("buffer_slice", {pPtr("buf"), pNum("start"), pOpt("end", TS::Num)},
    [](const std::vector<Value>& args) -> Value {
        auto* b = requireBuffer(args[0], "buffer_slice", "buf");
        int64_t n = (int64_t)b->bytes.size();
        int64_t start = std::max((int64_t)0, std::min(numToInt64(args[1], "buffer_slice", "start"), n));
        int64_t end   = args.size() > 2 ? numToInt64(args[2], "buffer_slice", "end") : n;
        end = std::max(start, std::min(end, n));
        return makeBuffer(std::vector<uint8_t>(b->bytes.begin() + start, b->bytes.begin() + end));
    });

    // buffer_concat(a: buf, b: buf) -> buf  (a new buffer, a's bytes then b's)
    interp->bindSig("buffer_concat", {pPtr("a"), pPtr("b")},
    [](const std::vector<Value>& args) -> Value {
        auto* a = requireBuffer(args[0], "buffer_concat", "a");
        auto* bb = requireBuffer(args[1], "buffer_concat", "b");
        std::vector<uint8_t> out;
        out.reserve(a->bytes.size() + bb->bytes.size());
        out.insert(out.end(), a->bytes.begin(), a->bytes.end());
        out.insert(out.end(), bb->bytes.begin(), bb->bytes.end());
        return makeBuffer(std::move(out));
    });

    // buffer_pack(desc, values) -> buf
    // serializes values (a struct_type's map, or one number for a bare fixed-width descriptor) into a new buffer
    // laid out per desc. raises if desc isn't packable (see struct_type()) or values doesn't match
    interp->bindSig("buffer_pack", {pAny("desc"), pAny("values")},
    [](const std::vector<Value>& args) -> Value {
        const TypeDesc& d = *requireDesc(args[0], "buffer_pack", "desc");
        size_t sz = packedSize(d, "buffer_pack");
        std::vector<uint8_t> bytes(sz, 0);
        packInto(bytes.data(), d, args[1], "buffer_pack");
        return makeBuffer(std::move(bytes));
    });

    // buffer_unpack(desc, buf) -> map | num
    //
    // the inverse of buffer_pack: reads buf per desc's layout. raises if
    // desc isn't packable or buf is smaller than desc's packed size.
    interp->bindSig("buffer_unpack", {pAny("desc"), pPtr("buf")},
    [](const std::vector<Value>& args) -> Value {
        const TypeDesc& d = *requireDesc(args[0], "buffer_unpack", "desc");
        size_t sz = packedSize(d, "buffer_unpack");
        auto* b = requireBuffer(args[1], "buffer_unpack", "buf");
        if (b->bytes.size() < sz)
            raiseError("buffer_unpack", "buffer is too small (" + std::to_string(b->bytes.size()) +
                       " bytes, need " + std::to_string(sz) + ")");
        return unpackFrom(b->bytes.data(), d, "buffer_unpack");
    });

    // type_ptr(...tags: str) -> desc
    // no tags: any pointer. one tag: only a pointer with exactly that tag (like "ffi.lib"). several tags: a pointer
    // with any of them (no need for type_union)
    interp->bindSig("type_ptr", {pRestStr("tags")},
    [](const std::vector<Value>& args) -> Value {
        TypeDesc d; d.kind = TypeDesc::Kind::Ptr;
        for (auto& t : args) d.ptrTags.push_back(t.asString());
        return wrapDesc(std::move(d));
    });

    // type_arr(elem?: desc) -> desc
    //
    // with no elem, matches any array. with elem, matches only an array
    // whose elements all match elem (recursively, so arr(arr(int)) works).
    interp->bindSig("type_arr", {pOpt("elem", TS::Ptr)},
    [](const std::vector<Value>& args) -> Value {
        TypeDesc d; d.kind = TypeDesc::Kind::Arr;
        if (!args.empty())
            d.elem = std::make_shared<TypeDesc>(*requireDesc(args[0], "type_arr", "elem"));
        return wrapDesc(std::move(d));
    });

    // type_map(value?: desc) -> desc
    //
    // like type_arr but for a map's values (embr map keys are always
    // strings, so there's no separate key-type parameter).
    interp->bindSig("type_map", {pOpt("value", TS::Ptr)},
    [](const std::vector<Value>& args) -> Value {
        TypeDesc d; d.kind = TypeDesc::Kind::Map;
        if (!args.empty())
            d.valueType = std::make_shared<TypeDesc>(*requireDesc(args[0], "type_map", "value"));
        return wrapDesc(std::move(d));
    });

    // type_union(alts: arr of desc) -> desc
    // matches a value that matches at least one of alts. for "one of several pointer tags" type_ptr is more direct,
    // this is for mixing kinds of descriptor (a str or an int)
    interp->bindSig("type_union", {pArr("alts")},
    [](const std::vector<Value>& args) -> Value {
        const auto& alts = args[0].asArray();
        if (alts.size() < 2)
            raiseError("type_union", "needs at least 2 alternatives");
        TypeDesc d; d.kind = TypeDesc::Kind::Union;
        for (size_t i = 0; i < alts.size(); ++i)
            d.alts.push_back(std::make_shared<TypeDesc>(
                *requireDesc(alts[i], "type_union", "alts[" + std::to_string(i) + "]")));
        return wrapDesc(std::move(d));
    });

    // type_check(desc, value) -> int (0 or 1)
    interp->bindSig("type_check", {pAny("desc"), pAny("value")},
    [](const std::vector<Value>& args) -> Value {
        const TypeDesc& d = *requireDesc(args[0], "type_check", "desc");
        return Value(matches(d, args[1]) ? (int64_t)1 : (int64_t)0);
    });

    // type_assert(desc, value, msg?: str) -> value
    //
    // returns value unchanged if it matches desc, otherwise raises a
    // catchable error (see try/catch) describing the mismatch
    interp->bindSig("type_assert", {pAny("desc"), pAny("value"), pOpt("msg", TS::Str)},
    [](const std::vector<Value>& args) -> Value {
        const TypeDesc& d = *requireDesc(args[0], "type_assert", "desc");
        if (!matches(d, args[1])) {
            std::string msg = args.size() > 2 ? args[2].asString()
                : "expected " + describe(d) + ", got " + args[1].typeName();
            raiseError("type_assert", msg);
        }
        return args[1];
    });

    // type_name(desc) -> str
    interp->bindSig("type_name", {pAny("desc")},
    [](const std::vector<Value>& args) -> Value {
        return Value(describe(*requireDesc(args[0], "type_name", "desc")));
    });

    // type_of(value) -> desc
    // a best-effort shallow descriptor of value's dynamic type. an array's element type (or map's value type) is
    // inferred only if every element has the same type (recursively), otherwise it is an untyped type_arr()/type_map()
    // a pointer's tag is kept if it has one
    interp->bindSig("type_of", {pAny("value")},
    [](const std::vector<Value>& args) -> Value {
        std::function<TypeDesc(const Value&)> infer = [&](const Value& v) -> TypeDesc {
            if (v.isNumber())   return kindOnly(TypeDesc::Kind::Float);
            if (v.isInt())      return kindOnly(TypeDesc::Kind::Int);
            if (v.isString())   return kindOnly(TypeDesc::Kind::Str);
            if (v.isCallable()) return kindOnly(TypeDesc::Kind::Fn);
            if (v.isPointer()) {
                TypeDesc d; d.kind = TypeDesc::Kind::Ptr;
                if (!v.asPointer().type.empty()) d.ptrTags.push_back(v.asPointer().type);
                return d;
            }
            if (v.isArray()) {
                const auto& arr = v.asArray();
                TypeDesc d; d.kind = TypeDesc::Kind::Arr;
                if (!arr.empty()) {
                    auto first = std::make_shared<TypeDesc>(infer(arr[0]));
                    bool uniform = true;
                    for (size_t i = 1; i < arr.size() && uniform; ++i)
                        uniform = describe(infer(arr[i])) == describe(*first);
                    if (uniform) d.elem = first;
                }
                return d;
            }
            if (v.isMap()) {
                const auto& m = v.asMap();
                TypeDesc d; d.kind = TypeDesc::Kind::Map;
                if (!m.empty()) {
                    auto it = m.begin();
                    auto first = std::make_shared<TypeDesc>(infer(it->second));
                    bool uniform = true;
                    for (++it; it != m.end() && uniform; ++it)
                        uniform = describe(infer(it->second)) == describe(*first);
                    if (uniform) d.valueType = first;
                }
                return d;
            }
            return kindOnly(TypeDesc::Kind::Any);
        };
        return wrapDesc(infer(args[0]));
    });

    // what each always returns (see Value::Callable::returns)
    interp->setResultTypes({
        {"type_check", TS::Int}, {"type_name", TS::Str}, {"buffer", TS::Ptr}, {"buffer_from_str", TS::Ptr}, {"buffer_to_str", TS::Str},
        {"buffer_len", TS::Int}, {"buffer_get", TS::Int}, {"buffer_set", TS::Float}, {"buffer_slice", TS::Ptr}, {"buffer_concat", TS::Ptr},
        {"buffer_pack", TS::Ptr},
    });
}

// what this plugin says about itself. embr reads it before it runs embr_register (see core/plugininfo.h)
EMBR_PLUGIN_INFO("embrtypes", "1.0.0", "runtime type descriptors, bool/nil, and raw buffers", EMBR_CAP_NONE, EMBR_CAP_NONE,
    {"buffer", "zero-arg constructors"},
    {"buffer_concat", "a new buffer holding a followed by b"},
    {"buffer_fill", "sets every byte of the buffer to v (mod 256) and gives the buffer back"},
    {"buffer_from_str", "a new buffer with the bytes of the string"},
    {"buffer_get", "single byte 0–255 (v mod 256 on write); raises out of range"},
    {"buffer_len", "the number of bytes in the buffer"},
    {"buffer_pack", "raises if desc isn't packable, or buf too small. an integer that doesn't fit its field (300 for u8, -1 for u16, 1.5 for i32) raises and names the field, it is never cut down to its low bytes"},
    {"buffer_set", "single byte 0–255 (v mod 256 on write); raises out of range"},
    {"buffer_slice", "new independent copy"},
    {"buffer_to_str", "the bytes of the buffer as a string"},
    {"buffer_unpack", "raises if desc isn't packable, or buf too small. an integer that doesn't fit its field (300 for u8, -1 for u16, 1.5 for i32) raises and names the field, it is never cut down to its low bytes"},
    {"is_nil", "1 if v is the nil value, else 0"},
    {"make_false", "zero-arg natives, for while_do-style re-evaluated conditions"},
    {"make_true", "zero-arg natives, for while_do-style re-evaluated conditions"},
    {"struct_type", "computes natural-alignment layout; raises on an empty field list or malformed field shape"},
    {"type_any", "the descriptor that accepts any value"},
    {"type_arr", "homogeneous container descriptors"},
    {"type_assert", "raises (catchable) on mismatch; passes value through unchanged on match"},
    {"type_bool", "the descriptor for a true or false value"},
    {"type_buffer", "the descriptor for a buffer"},
    {"type_check", "1 if the value fits the descriptor, else 0"},
    {"type_f32", "the descriptor for a 32-bit float"},
    {"type_f64", "the descriptor for a 64-bit float"},
    {"type_float", "the descriptor for a float"},
    {"type_fn", "the descriptor for a function"},
    {"type_i16", "the descriptor for a signed 16-bit integer"},
    {"type_i32", "the descriptor for a signed 32-bit integer"},
    {"type_i64", "the descriptor for a signed 64-bit integer"},
    {"type_i8", "the descriptor for a signed 8-bit integer"},
    {"type_int", "the descriptor for an int"},
    {"type_map", "homogeneous container descriptors"},
    {"type_name", "human-readable (describe())"},
    {"type_nil", "the descriptor for the nil value"},
    {"type_num", "the descriptor for an int or a float"},
    {"type_of", "best-effort shallow inference"},
    {"type_ptr", "0 tags = any pointer; 1 = exact match; 2+ = union"},
    {"type_str", "the descriptor for a string"},
    {"type_u16", "the descriptor for an unsigned 16-bit integer"},
    {"type_u32", "the descriptor for an unsigned 32-bit integer"},
    {"type_u64", "the descriptor for an unsigned 64-bit integer"},
    {"type_u8", "the descriptor for an unsigned 8-bit integer"},
    {"type_union", "raises if fewer than 2 alternatives"});
