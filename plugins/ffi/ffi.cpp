// ffi.cpp
// foreign function interface plugin for embr: call C functions from a shared library
//
// usage
//   import "embrtypes"   # for type descriptors: type_i32(), type_ptr(), etc
//   import "ffi"
//   lib = ffi_open("libc.so.6")
//   sym = ffi_sym(lib, "strlen")
//   print(ffi_call(sym, type_i64(), [type_str()], ["hello"]))
//
// good to know
//   - every handle given to a script (library, symbol, prepared call, callback) is a typed pointer/userdata (see
//     TypedPtr in core/value.h), never a map, so a script can't forge one. asPointer(expectedType) raises on a tag mismatch
//   - type descriptors (the `ret`/`arg_types` arguments) are plugins/embrtypes/embrtypes.h's TypeDesc, shared by a
//     plain #include. ffi.so doesn't need embrtypes.so loaded, but a script must import embrtypes to build them
//   - usable in a call: i8..f64, ptr, str (passed as a C string), and structs built from those. the other kinds
//     (arr, map, fn, union, untyped) raise a clear error
//   - `ret` can be `nil` to mean "returns nothing" (void isn't a value shape, so embrtypes has no type_void())
//   - out-parameters are embrtypes' buffer(): a buffer's pointer is its byte data's address, so it works directly
//     as a `ptr` argument, or with ptr_read/ptr_write/buffer_pack/buffer_unpack

#include <embr/embr.h>
#include "../embrtypes/embrtypes.h"
#include <ffi.h>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <algorithm>
#include <exception>
#include <cmath>

using namespace embr;
using TypeDesc = embrtypes::TypeDesc;

static Param pAny(std::string n)                      { return Param::req(std::move(n), TS::Any); }
static Param pStr(std::string n)                      { return Param::req(std::move(n), TS::Str); }
static Param pNum(std::string n)                      { return Param::req(std::move(n), TS::Num); }
static Param pArr(std::string n)                      { return Param::req(std::move(n), TS::Arr); }
static Param pFn (std::string n)                      { return Param::req(std::move(n), TS::Fn);  }
static Param pPtr(std::string n)                      { return Param::req(std::move(n), TS::Ptr); }
static Param pOpt(std::string n, TypeSet m = TS::Any) { return Param::opt(std::move(n), m);       }

// allocated once at registration time, captured by shared_ptr into every bound lambda
// lives as long as global scope
struct FFIState {

    // sole strong owner of an open library. ffi_close() erasing the map
    // entry below drops the only shared_ptr to it, running ~LibHandle (and
    // therefore pluginClose) immediately, real, synchronous close
    // semantics, not "whenever the last reference happens to go away".
    struct LibHandle {
        PluginHandle handle = nullptr;
        std::string  path;
        ~LibHandle() { if (handle) pluginClose(handle); }
    };
    std::unordered_map<const void*, std::shared_ptr<LibHandle>> libs;

    // a symbol resolved from a library. holds only a *weak* reference to the
    // owning LibHandle, it must not keep a closed library's mapping alive,
    // but ffi_call/ffi_invoke need to detect a closed library and raise a
    // catchable error instead of jumping into unmapped memory.
    struct SymHandle {
        void*                    fnptr = nullptr;
        std::string              name;
        std::weak_ptr<LibHandle> lib;
        bool                     raw = false;   // from ffi_fn(): no library to check, the caller vouches for the address
    };

    // a C-callable trampoline made by ffi_callback(). C code may keep the raw executable pointer without any embr Value
    // (after registering it with a callback API), so unlike sym/prepared this is not refcounted: it lives until
    // ffi_callback_free is called
    struct CallbackEntry;
    std::unordered_map<const void*, std::unique_ptr<CallbackEntry>> callbacks;

    // a cif + resolved descriptors cached by ffi_prepare() for repeated
    // ffi_invoke() calls. refcounted (unlike callbacks): nothing outside
    // embr holds a raw pointer to a PreparedCall, so it's safe to free
    // automatically once the last Value referencing it is dropped.
    struct PreparedCall;

    ~FFIState();
};

// void isn't a TypeDesc kind embrtypes.cpp itself exposes (see the file
// header comment), scripts spell "no return value" as `nil` for ret, and
// this plugin maps that to its own single, process-lifetime Void descriptor.
static const TypeDesc g_voidDesc = embrtypes::kindOnly(TypeDesc::Kind::Void);

// a TypeDesc argument plus whatever keeps it alive. requireDesc() returns a raw pointer into the Value's TypedPtr
// payload, fine for one ffi_call/ffi_callback invocation. ffi_prepare/ffi_callback store the descriptor for
// reuse, so they also hold the shared_ptr the Value's TypedPtr held (from asPointer().owner()). nil (void) has no
// owner, g_voidDesc is static
struct DescRef {
    const TypeDesc*                    ptr = &g_voidDesc;
    std::shared_ptr<const TypeDesc>    owner; // null for g_voidDesc, non-null otherwise
};
static DescRef resolveDescArg(const Value& v, const std::string& caller, const std::string& argname) {
    if (embrtypes::isNil(v)) return {};
    const TypeDesc* raw = embrtypes::requireDesc(v, caller, argname);
    return DescRef{raw, std::static_pointer_cast<const TypeDesc>(v.asPointer().owner())};
}

// libffi needs a stable-address ffi_type (and elements[] array) for as long as a cif uses it. for a struct from
// struct_type() this plugin builds and caches one, keyed by the TypeDesc's own stable heap address (embrtypes
// knows nothing about libffi)
struct StructFFI { ffi_type type{}; std::vector<ffi_type*> elements; };
// thread_local, not one shared static: a plain static would be an unsynchronized map written by every thread that
// marshals a struct type (see g_rng in embrmath.cpp). a TypeDesc* key only matters to the thread that made it, so
// each thread building its own entry costs one rebuild per thread, which beats a data race
static thread_local std::unordered_map<const void*, std::unique_ptr<StructFFI>> g_structFFICache;

static ffi_type* descToFFIType(const TypeDesc& d) {
    using Kind = TypeDesc::Kind;
    if (d.kind == Kind::Void) return &ffi_type_void;
    if (d.kind == Kind::Ptr || d.kind == Kind::Str) return &ffi_type_pointer;

    if (d.kind == Kind::Struct) {
        if (d.structSize == 0)
            raiseError("ffi", "struct '" + embrtypes::describe(d) +
                       "' has a non-fixed-width field and cannot be used in an ffi call");
        auto it = g_structFFICache.find(&d);
        if (it != g_structFFICache.end()) return &it->second->type;

        auto sf = std::make_unique<StructFFI>();
        sf->elements.reserve(d.fields.size() + 1);
        for (auto& f : d.fields) sf->elements.push_back(descToFFIType(*f.type));
        sf->elements.push_back(nullptr); // libffi requires a null-terminated elements array
        sf->type.size      = d.structSize;
        sf->type.alignment = (unsigned short)d.structAlignment;
        sf->type.type      = FFI_TYPE_STRUCT;
        sf->type.elements  = sf->elements.data();

        ffi_type* result = &sf->type;
        g_structFFICache[&d] = std::move(sf);
        return result;
    }

    embrtypes::FixedWidth w;
    if (embrtypes::fixedWidth(d.kind, w)) {
        if (w.isFloat) return w.size == 4 ? &ffi_type_float : &ffi_type_double;
        if (w.isSigned) {
            switch (w.size) {
                case 1: return &ffi_type_sint8;  case 2: return &ffi_type_sint16;
                case 4: return &ffi_type_sint32; case 8: return &ffi_type_sint64;
            }
        } else {
            switch (w.size) {
                case 1: return &ffi_type_uint8;  case 2: return &ffi_type_uint16;
                case 4: return &ffi_type_uint32; case 8: return &ffi_type_uint64;
            }
        }
    }
    raiseError("ffi", "type '" + embrtypes::describe(d) + "' cannot be used in an ffi call "
               "(only i8..f64, ptr, str, nil (as ret, meaning void), and structs built from these are supported)");
}

// a descriptor's byte size for buffer-sizing purposes (argument slots, the
// return buffer), distinct from embrtypes::packedSize() because ptr/str
// (sizeof(void*)) and Void (0) are meaningful sizes here but aren't
// "packable" in embrtypes' own sense.
static size_t descByteSize(const TypeDesc& d) {
    using Kind = TypeDesc::Kind;
    if (d.kind == Kind::Void) return 0;
    if (d.kind == Kind::Ptr || d.kind == Kind::Str) return sizeof(void*);
    if (d.kind == Kind::Struct) return d.structSize;
    embrtypes::FixedWidth w;
    return embrtypes::fixedWidth(d.kind, w) ? w.size : 0;
}

// a script-supplied double -> int64_t without the undefined behaviour of a
// raw (int64_t) cast on NaN/Inf/out-of-range input. raises instead.
static int64_t doubleToInt64(double x, const std::string& context, const std::string& what) {
    // 2^63 is exactly representable as a double; valid range is [-2^63, 2^63)
    if (!(x >= -9223372036854775808.0 && x < 9223372036854775808.0))
        raiseError(context, what + ": number " + std::to_string(x) + " is out of integer range");
    return (int64_t)x;
}

// an error thrown by a script callback while C code (qsort, a signal-style API, ...) is on the stack must not unwind
// through those C frames (undefined behavior, usually an abort). the trampoline catches it here and performCall
// rethrows it after ffi_call returns normally. thread_local because each embr thread runs its own ffi_calls
static thread_local std::exception_ptr g_pendingCallbackError;

static int64_t nfixedArg(const Value& v, const std::string& context) {
    int64_t n = doubleToInt64(v.asNumber(), context, "nfixed");
    if (n < 0) raiseError(context, "nfixed cannot be negative");
    return n;
}

// raw memory -> Value
// used for ffi_call/ffi_invoke return values, ptr_read, and
// unmarshalling callback arguments
//
// libffi promotes integer return values smaller than ffi_arg (register width) into full ffi_arg-sized slot
// must read ffi_arg and then narrow rather than reading only sz bytes
// otherwise get garbage on big-endian platforms and potential reads of uninitialised memory on little-endian ones
static Value unmarshalReturn(const TypeDesc& d, const void* buf) {
    using Kind = TypeDesc::Kind;
    if (d.kind == Kind::Void) return Value(0.0);

    if (d.kind == Kind::Struct) {
        Value::map_type m;
        for (auto& f : d.fields) m[f.name] = unmarshalReturn(*f.type, (const uint8_t*)buf + f.offset);
        return Value(std::move(m));
    }

    if (d.kind == Kind::Str) {
        const char* s; memcpy(&s, buf, sizeof(char*));
        return Value(s ? std::string(s) : std::string(""));
    }

    if (d.kind == Kind::Ptr) {
        void* p; memcpy(&p, buf, sizeof(void*));
        return Value::makePointer(p);
    }

    embrtypes::FixedWidth w;
    if (!embrtypes::fixedWidth(d.kind, w))
        raiseError("ffi", "type '" + embrtypes::describe(d) + "' cannot be used as an ffi return type");

    if (w.isFloat) {
        if (w.size == 4) { float  f; memcpy(&f, buf, 4); return Value((double)f); }
        else              { double f; memcpy(&f, buf, 8); return Value(f); }
    }

    // returned as an exact int64, not double, so large 64-bit C values keep their precision above 2^53. a uint64
    // above INT64_MAX still can't be exact (embr has one signed 64-bit int type)
    if (w.isSigned) {
        switch (w.size) {
            case 1: { ffi_sarg v; memcpy(&v, buf, sizeof(v)); return Value((int64_t)(int8_t)v);  }
            case 2: { ffi_sarg v; memcpy(&v, buf, sizeof(v)); return Value((int64_t)(int16_t)v); }
            case 4: { int32_t  v; memcpy(&v, buf, 4);         return Value((int64_t)v); }
            case 8: { int64_t  v; memcpy(&v, buf, 8);         return Value(v); }
        }
    } else {
        switch (w.size) {
            case 1: { ffi_arg  v; memcpy(&v, buf, sizeof(v)); return Value((int64_t)(uint8_t)v);  }
            case 2: { ffi_arg  v; memcpy(&v, buf, sizeof(v)); return Value((int64_t)(uint16_t)v); }
            case 4: { uint32_t v; memcpy(&v, buf, 4);         return Value((int64_t)v); }
            case 8: { uint64_t v; memcpy(&v, buf, 8);         return Value((int64_t)v); }
        }
    }
    raiseError("ffi", "unmarshal: unsupported width " + std::to_string(w.size));
}

// Value -> raw memory slot
// slot points at at least descByteSize(info) bytes of zeroed storage owned by the caller's argStorage vector, which
// must not reallocate before ffi_call. keepalive collects heap allocations (marshaled strings) that must outlive
// the call, the caller frees them afterwards. context is the calling native's name, for error messages
static void marshalArg(const std::string&     context,
                       const Value&            val,
                       const TypeDesc&         d,
                       void*                   slot,
                       std::vector<void*>&     keepalive,
                       size_t                  argIdx)
{
    using Kind = TypeDesc::Kind;
    const std::string argLabel = "arg[" + std::to_string(argIdx) + "]";
    const std::string tname    = embrtypes::describe(d);

    // struct passed by value: slot is sized to the whole struct (see
    // performCall's per-arg slot sizing) and each field marshals into its
    // own offset within it, recursively, so a struct field can itself be
    // another struct
    if (d.kind == Kind::Struct) {
        if (!val.isMap())
            raiseError(context, argLabel + ": expects struct '" + tname + "', got " + val.typeName());
        const auto& fields = val.asMap();
        for (auto& f : d.fields) {
            auto it = fields.find(f.name);
            if (it == fields.end())
                raiseError(context, argLabel + ": struct '" + tname + "' missing field '" + f.name + "'");
            marshalArg(context, it->second, *f.type, (uint8_t*)slot + f.offset, keepalive, argIdx);
        }
        return;
    }

    // str -> malloc'd null-terminated buffer (freed by performCall after the call)
    if (d.kind == Kind::Str) {
        if (!val.isString())
            raiseError(context, argLabel + ": expects a string, got " + val.typeName());
        const std::string& s = val.asString();
        char* buf = (char*)malloc(s.size() + 1);
        if (!buf) raiseError(context, argLabel + ": malloc failed for string argument");
        memcpy(buf, s.data(), s.size() + 1);
        keepalive.push_back(buf);
        memcpy(slot, &buf, sizeof(void*));
        return;
    }

    if (d.kind == Kind::Ptr) {
        if (!val.isPointer())
            raiseError(context, argLabel + ": expects ptr, got " + val.typeName());
        void* p = val.asPointer().ptr;
        memcpy(slot, &p, sizeof(void*));
        return;
    }

    embrtypes::FixedWidth w;
    if (!embrtypes::fixedWidth(d.kind, w))
        raiseError(context, argLabel + ": type '" + tname + "' cannot be used as an ffi argument");

    if (w.isFloat) {
        if (!val.isNumeric())
            raiseError(context, argLabel + ": expects " + tname + ", got " + val.typeName());
        double x = val.asNumber();
        if (w.size == 4) { float f = (float)x; memcpy(slot, &f, 4); }
        else              {                     memcpy(slot, &x, 8); }
        return;
    }

    // integer. prefer an already-exact int64 (e.g. from an int literal or a
    // prior ffi_call's int-typed return) over a double round-trip so large
    // 64-bit values don't lose precision on the way in.
    const int64_t iv = embrtypes::intForWidth(val, w, tname, context, argLabel);
    switch (w.size) {
        case 1: { uint8_t  v=(uint8_t) iv; memcpy(slot,&v,1); } break;
        case 2: { uint16_t v=(uint16_t)iv; memcpy(slot,&v,2); } break;
        case 4: { uint32_t v=(uint32_t)iv; memcpy(slot,&v,4); } break;
        case 8: { uint64_t v=(uint64_t)iv; memcpy(slot,&v,8); } break;
    }
}

// shared marshal -> ffi_call -> unmarshal sequence used by both the one-shot ffi_call() and the cached ffi_invoke() paths
//
// ffi_call() builds a fresh cif + descriptor array right before calling this
// ffi_invoke() passes in the cif + descriptors cached by ffi_prepare()
static Value performCall(const std::string&                       context,
                         ffi_cif&                                  cif,
                         void*                                      fnptr,
                         const std::vector<const TypeDesc*>&        argInfo,
                         const TypeDesc&                            retInfo,
                         const Value::array_type&                   argVals)
{
    size_t nargs = argInfo.size();
    if (argVals.size() != nargs)
        raiseError(context, "expected " + std::to_string(nargs) +
                   " args, got " + std::to_string(argVals.size()));

    // each arg gets a slot of at least kSlotSize bytes, a struct passed by
    // value can be arbitrarily larger, so slots are sized per-argument and
    // laid out back to back rather than assuming a uniform stride
    static constexpr size_t kSlotSize = 8;  // sizeof largest scalar aligns to pointer
    std::vector<size_t> slotOffset(nargs);
    size_t totalArgBytes = 0;
    for (size_t i = 0; i < nargs; ++i) {
        slotOffset[i] = totalArgBytes;
        totalArgBytes += std::max(kSlotSize, descByteSize(*argInfo[i]));
    }

    // argStorage is allocated up front so it never reallocates
    // argptrs array holds pointers into it and must stay valid until after ffi_call returns
    std::vector<uint8_t> argStorage(totalArgBytes, 0);
    std::vector<void*>   argptrs(nargs);
    // malloc'd string copies; freed on every exit path, including a
    // marshalArg() raise partway through the argument list
    struct Keepalive : std::vector<void*> {
        ~Keepalive() { for (void* p : *this) free(p); }
    } keepalive;

    for (size_t i = 0; i < nargs; ++i) {
        argptrs[i] = argStorage.data() + slotOffset[i];
        marshalArg(context, argVals[i], *argInfo[i], argptrs[i], keepalive, i);
    }

    // the return buffer must be at least sizeof(ffi_arg) bytes because libffi
    // promotes small integer returns into a register-sized slot; 16 bytes
    // covers all scalars including double and pointer, but a struct return
    // can be larger, so size up to fit it too
    std::vector<uint8_t> retbuf(std::max((size_t)16, descByteSize(retInfo)), 0);
    if (!fnptr) raiseError(context, "symbol resolved to a null address");
    g_pendingCallbackError = nullptr;
    ffi_call(&cif, FFI_FN(fnptr), retbuf.data(), nargs > 0 ? argptrs.data() : nullptr);

    // a script callback invoked during the call failed: surface it now
    if (g_pendingCallbackError) {
        std::exception_ptr e = g_pendingCallbackError;
        g_pendingCallbackError = nullptr;
        std::rethrow_exception(e);
    }

    return unmarshalReturn(retInfo, retbuf.data());
}

// Value -> raw memory at an arbitrary address, per a type descriptor. used by ptr_write and for struct fields
// (a field can be another struct). a str descriptor is treated like a plain ptr here: the caller passes an
// already-allocated pointer (from embrtypes' buffer()/ptr_offset), because there is no per-call keepalive to
// hang a fresh malloc'd copy on, unlike marshalArg
static void writeValueToMemory(const std::string& context, void* p,
                               const TypeDesc& d, const Value& v)
{
    using Kind = TypeDesc::Kind;
    if (d.kind == Kind::Struct) {
        if (!v.isMap())
            raiseError(context, "value must be a map for struct type '" + embrtypes::describe(d) + "'");
        const auto& fields = v.asMap();
        for (auto& f : d.fields) {
            auto it = fields.find(f.name);
            if (it == fields.end())
                raiseError(context, "struct '" + embrtypes::describe(d) + "' missing field '" + f.name + "'");
            writeValueToMemory(context, (uint8_t*)p + f.offset, *f.type, it->second);
        }
        return;
    }
    if (d.kind == Kind::Ptr || d.kind == Kind::Str) {
        if (!v.isPointer())
            raiseError(context, "value must be a pointer for ptr type");
        void* q = v.asPointer().ptr;
        memcpy(p, &q, sizeof(void*));
        return;
    }
    embrtypes::FixedWidth w;
    if (!embrtypes::fixedWidth(d.kind, w))
        raiseError(context, "type '" + embrtypes::describe(d) + "' cannot be written to memory");
    if (w.isFloat) {
        if (!v.isNumeric())
            raiseError(context, "value must be a number for float type");
        double x = v.asNumber();
        if (w.size == 4) { float f = (float)x; memcpy(p, &f, 4); }
        else              {                     memcpy(p, &x, 8); }
    } else {
        const int64_t iv = embrtypes::intForWidth(v, w, embrtypes::describe(d), context, "value");
        switch (w.size) {
            case 1: { uint8_t  x=(uint8_t) iv; memcpy(p,&x,1); } break;
            case 2: { uint16_t x=(uint16_t)iv; memcpy(p,&x,2); } break;
            case 4: { uint32_t x=(uint32_t)iv; memcpy(p,&x,4); } break;
            case 8: { uint64_t x=(uint64_t)iv; memcpy(p,&x,8); } break;
        }
    }
}

// placed after marshalArg/unmarshalReturn so the trampoline below can call them
// stored as unique_ptr<CallbackEntry> so its address is stable across map rehashes
// the trampoline receives a raw pointer to it as userdata
struct FFIState::CallbackEntry {
    ffi_cif                cif;
    ffi_closure*           closure  = nullptr;  // writable page
    void*                  fnptr    = nullptr;  // executable page
    std::vector<ffi_type*> argFFITypes;
    std::vector<DescRef>   argDescs;            // resolved once at ffi_callback() time
    DescRef                retDesc;
    Value                  embrFn;              // keeps the callable alive
    Interpreter*           interp   = nullptr;

    ~CallbackEntry() { if (closure) ffi_closure_free(closure); }
};

// a symbol + cif prepared once by ffi_prepare(), invoked repeatedly by ffi_invoke() without re-resolving
// descriptors or calling ffi_prep_cif again
// argFFITypes must stay alive and at a stable address for as long as cif does
// ffi_prep_cif stores a pointer into it, which is guaranteed here since both live inside the same heap-allocated PreparedCall
struct FFIState::PreparedCall {
    ffi_cif                cif{};
    void*                  fnptr = nullptr;
    std::vector<ffi_type*> argFFITypes;
    std::vector<DescRef>   argDescs;
    ffi_type*              retFFIType = nullptr;
    DescRef                retDesc;
    std::string            symName;   // for error messages
    std::weak_ptr<FFIState::LibHandle> lib;  // validated live at ffi_invoke() time
    bool                   raw = false;       // made from ffi_fn(): nothing to validate
};

// out of line: libs (shared_ptr<LibHandle>) and callbacks (unique_ptr<CallbackEntry>)
// both clean up entirely on their own via their own destructors once
// CallbackEntry is a complete type, so this has nothing left to do by hand
FFIState::~FFIState() = default;

static void trampolineBody(FFIState::CallbackEntry* entry, void* retbuf, void** argptrs)
{

    std::vector<Value> embrArgs;
    embrArgs.reserve(entry->argDescs.size());
    for (size_t i = 0; i < entry->argDescs.size(); ++i)
        embrArgs.push_back(unmarshalReturn(*entry->argDescs[i].ptr, argptrs[i]));

    Value result = invoke(*entry->interp, entry->embrFn, embrArgs);

    // marshal the return value into retbuf.
    const TypeDesc& retInfo = *entry->retDesc.ptr;
    size_t rsz = descByteSize(retInfo);
    if (rsz == 0) return;   // void

    if (retInfo.kind == TypeDesc::Kind::Ptr || retInfo.kind == TypeDesc::Kind::Str) {
        void* p = result.isPointer() ? result.asPointer().ptr : nullptr;
        memcpy(retbuf, &p, sizeof(void*));
        return;
    }
    embrtypes::FixedWidth w;
    if (embrtypes::fixedWidth(retInfo.kind, w) && w.isFloat) {
        if (w.size == 4) { float f = (float)result.asNumber(); memcpy(retbuf, &f, 4); }
        else              { double f = result.asNumber();       memcpy(retbuf, &f, 8); }
        return;
    }
    // integer. write into an ffi_arg slot. libffi reads the right width.
    int64_t iv = embrtypes::fixedWidth(retInfo.kind, w)
        ? embrtypes::intForWidth(result, w, embrtypes::describe(retInfo), "ffi_callback", "return value")
        : doubleToInt64(result.asNumber(), "ffi_callback", "return value");
    memcpy(retbuf, &iv, sizeof(iv));
}

// the actual C function pointer libffi installs
//
// must be a plain extern C function
// userdata is the heap-stable raw pointer to CallbackEntry
static void ffiClosureTrampoline(ffi_cif*  /*cif*/,
                                 void*     retbuf,
                                 void**    argptrs,
                                 void*     userdata)
{
    auto* entry = static_cast<FFIState::CallbackEntry*>(userdata);

    // never let an exception cross the C frames above us; see
    // g_pendingCallbackError. retbuf is zeroed so C sees a harmless 0/NULL.
    try {
        trampolineBody(entry, retbuf, argptrs);
    } catch (...) {
        if (!g_pendingCallbackError) g_pendingCallbackError = std::current_exception();
        size_t rsz = std::max(descByteSize(*entry->retDesc.ptr), sizeof(ffi_arg));
        memset(retbuf, 0, rsz);
    }
}

EMBR_PLUGIN {

    auto ffi = std::make_shared<FFIState>();

    // the closure behind every live ffi_callback is a gc root: C code keeps the function pointer until
    // ffi_callback_free(), even if the script drops its handle, so gc_collect() must not clear the closure's
    // captured scope (the next call from C would fail with "undefined variable")
    // weak_ptr so this provider, stored in the Interpreter, can't keep the plugin state alive
    interp->addGcRootProvider([weak = std::weak_ptr<FFIState>(ffi)](const TraceVisitor& visit) {
        if (auto st = weak.lock())
            for (auto& kv : st->callbacks) visit(kv.second->embrFn);
    });

    // ffi_open(path: str) -> ptr<ffi.lib>
    //
    // opens a shared library via pluginOpen()
    // LoadLibraryA on windows dlopen(RTLD_NOW|RTLD_LOCAL) on POSIX
    // RTLD_NOW resolves all symbols immediately so missing-symbol errors surface at load time
    interp->bindSig("ffi_open", {pStr("path")},
    [ffi](const std::vector<Value>& args) -> Value {
        const std::string& path = args[0].asString();

        PluginHandle handle = pluginOpen(path.c_str());
        if (!handle)
            raiseError("ffi_open", "cannot open '" + path + "': " + pluginError());

        auto lib = std::make_shared<FFIState::LibHandle>();
        lib->handle = handle;
        lib->path   = path;
        const void* key = lib.get();
        ffi->libs[key] = lib;

        return Value::makePointer(const_cast<void*>(key), "ffi.lib");
    });

    // ffi_close(lib: ptr<ffi.lib>)
    // closes a library explicitly. sym/prepared handles made from it notice (their weak reference to the LibHandle
    // expires) and raise a clean error instead of using a dangling function pointer. libraries not closed
    // explicitly are closed in ~FFIState
    interp->bindSig("ffi_close", {pPtr("lib")},
    [ffi](const std::vector<Value>& args) -> Value {
        const void* key = args[0].asPointer("ffi.lib").ptr;
        if (!ffi->libs.erase(key))
            raiseError("ffi_close", "library is not open (already closed?)");
        return Value(0.0);
    });

    // ffi_sym(lib: ptr<ffi.lib>, name: str) -> ptr<ffi.sym>
    //
    // looks up a symbol via pluginSym()
    // GetProcAddress on windows dlsym on POSIX
    interp->bindSig("ffi_sym", {pPtr("lib"), pStr("name")},
    [ffi](const std::vector<Value>& args) -> Value {
        const void* key = args[0].asPointer("ffi.lib").ptr;
        auto it = ffi->libs.find(key);
        if (it == ffi->libs.end())
            raiseError("ffi_sym", "library has already been closed");
        auto& lib = it->second;

        const std::string& name = args[1].asString();
        void* sym = nullptr;

#if defined(__linux__) || defined(__APPLE__) || defined(__unix__)
        dlerror();  // clear any previous error
        sym = pluginSym(lib->handle, name.c_str());
        const char* err = dlerror();
        if (err)
            raiseError("ffi_sym", "symbol '" + name + "' not found in '" +
                       lib->path + "': " + err);
#else
        sym = pluginSym(lib->handle, name.c_str());
        if (!sym)
            raiseError("ffi_sym", "symbol '" + name + "' not found in '" +
                       lib->path + "': " + pluginError());
#endif

        if (!sym)
            raiseError("ffi_sym", "symbol '" + name + "' resolved to a null address in '" + lib->path + "'");

        return Value::makeUserdata<FFIState::SymHandle>("ffi.sym",
            FFIState::SymHandle{sym, name, lib});
    });

    // ffi_fn(address: ptr, name?: str) -> ptr<ffi.sym>
    // wraps a function address that did not come from ffi_sym (the result of glXGetProcAddress or raylib's rlGetProcAddress,
    // or a function pointer read from a struct). nothing checks it: a bad address crashes the process, and the address must
    // stay valid for as long as the symbol is used
    interp->bindSig("ffi_fn", {pPtr("address"), pOpt("name", TS::Str)},
    [](const std::vector<Value>& args) -> Value {
        void* fn = args[0].asPointer().ptr;
        if (!fn) raiseError("ffi_fn", "the address is null");
        std::string name = args.size() > 1 ? args[1].asString() : "function pointer";
        FFIState::SymHandle h{fn, name, {}};
        h.raw = true;
        return Value::makeUserdata<FFIState::SymHandle>("ffi.sym", std::move(h));
    });

    // ffi_call(sym: ptr<ffi.sym>, ret: desc|nil, arg_types: arr of desc, args: arr) -> any
    //
    // marshals args, builds a libffi CIF, calls the function, and unmarshals the return value
    // this rebuilds the cif and re-resolves every type descriptor on every call. that is fine for one-off calls, wasteful in a loop
    // for repeated calls to the same symbol use ffi_prepare()/ffi_invoke() instead, which does this work once
    // pass nil for ret when the function returns nothing
    // a buffer (from embrtypes' buffer()) may appear in args wherever a ptr type descriptor is given, to use it as an out-parameter
    interp->bindSig("ffi_call",
        {pPtr("sym"), pAny("ret"), pArr("arg_types"), pArr("args")},
    [ffi](const std::vector<Value>& args) -> Value {
        auto* sym = args[0].userdata<FFIState::SymHandle>("ffi.sym");
        if (!sym->raw && sym->lib.expired())
            raiseError("ffi_call", "the symbol's library has been closed");

        const auto& argTypeVals = args[2].asArray();
        const auto& argVals     = args[3].asArray();

        if (argTypeVals.size() != argVals.size())
            raiseError("ffi_call",
                "arg_types has " + std::to_string(argTypeVals.size()) +
                " entries but args has " + std::to_string(argVals.size()));

        size_t nargs = argTypeVals.size();

        // resolve descriptors + build the libffi type array
        std::vector<const TypeDesc*> argInfo(nargs);
        std::vector<ffi_type*>       ffiArgTypes(nargs);
        for (size_t i = 0; i < nargs; ++i) {
            argInfo[i]     = embrtypes::requireDesc(argTypeVals[i], "ffi_call",
                                                    "arg_types[" + std::to_string(i) + "]");
            ffiArgTypes[i] = descToFFIType(*argInfo[i]);
        }
        const TypeDesc* retInfo = resolveDescArg(args[1], "ffi_call", "ret").ptr;
        ffi_type* ffiRet = descToFFIType(*retInfo);

        // prepare the call interface
        // this plus the descriptor resolution above is exactly the per-call cost ffi_prepare/ffi_invoke avoid
        ffi_cif cif;
        ffi_status status = ffi_prep_cif(
            &cif,
            FFI_DEFAULT_ABI,
            (unsigned int)nargs,
            ffiRet,
            nargs > 0 ? ffiArgTypes.data() : nullptr
        );
        if (status != FFI_OK)
            raiseError("ffi_call", "ffi_prep_cif failed (status=" +
                       std::to_string((int)status) + ")");

        return performCall("ffi_call", cif, sym->fnptr, argInfo, *retInfo, argVals);
    });

    // ffi_prepare(sym: ptr<ffi.sym>, ret: desc|nil, arg_types: arr of desc) -> ptr<ffi.prepared>
    //
    // resolves the return + argument type descriptors and calls ffi_prep_cif once, caching the resulting cif behind a refcounted handle
    // pass that handle to ffi_invoke() to run the call without repeating any of this setup
    // use this instead of ffi_call() whenever the same symbol is called repeatedly, because ffi_call() redoes the
    // descriptor resolution and ffi_prep_cif on every call
    interp->bindSig("ffi_prepare",
        {pPtr("sym"), pAny("ret"), pArr("arg_types")},
    [ffi](const std::vector<Value>& args) -> Value {
        auto* sym = args[0].userdata<FFIState::SymHandle>("ffi.sym");
        if (!sym->raw && sym->lib.expired())
            raiseError("ffi_prepare", "the symbol's library has been closed");

        auto prep = std::make_shared<FFIState::PreparedCall>();
        prep->fnptr   = sym->fnptr;
        prep->symName = sym->name;
        prep->lib     = sym->lib;
        prep->raw     = sym->raw;

        const auto& argTypeVals = args[2].asArray();
        size_t nargs = argTypeVals.size();

        prep->argDescs.resize(nargs);
        prep->argFFITypes.resize(nargs);
        for (size_t i = 0; i < nargs; ++i) {
            prep->argDescs[i]     = resolveDescArg(argTypeVals[i], "ffi_prepare",
                                                   "arg_types[" + std::to_string(i) + "]");
            prep->argFFITypes[i] = descToFFIType(*prep->argDescs[i].ptr);
        }
        prep->retDesc    = resolveDescArg(args[1], "ffi_prepare", "ret");
        prep->retFFIType = descToFFIType(*prep->retDesc.ptr);

        ffi_status status = ffi_prep_cif(
            &prep->cif,
            FFI_DEFAULT_ABI,
            (unsigned int)nargs,
            prep->retFFIType,
            nargs > 0 ? prep->argFFITypes.data() : nullptr
        );
        if (status != FFI_OK)
            raiseError("ffi_prepare", "ffi_prep_cif failed (status=" +
                       std::to_string((int)status) + ")");

        return Value(TypedPtr(prep.get(), "ffi.prepared", prep));
    });

    // ffi_invoke(prepared: ptr<ffi.prepared>, args: arr) -> any
    //
    // runs a call built by ffi_prepare()
    // only marshals args and calls ffi_call() itself. no cif prep, no descriptor hash lookups
    // this is the fast path for repeated calls to the same symbol
    interp->bindSig("ffi_invoke", {pPtr("prepared"), pArr("args")},
    [](const std::vector<Value>& args) -> Value {
        auto* prep = args[0].userdata<FFIState::PreparedCall>("ffi.prepared");
        if (!prep->raw && prep->lib.expired())
            raiseError("ffi_invoke", "prepared call '" + prep->symName +
                       "': the symbol's library has been closed");

        std::vector<const TypeDesc*> argInfo(prep->argDescs.size());
        for (size_t i = 0; i < argInfo.size(); ++i) argInfo[i] = prep->argDescs[i].ptr;

        return performCall("ffi_invoke", prep->cif, prep->fnptr,
                           argInfo, *prep->retDesc.ptr, args[1].asArray());
    });

    // ffi_callback(fn: fn, ret: desc|nil, arg_types: arr of desc) -> ptr<ffi.callback>
    //
    // synthesises a real C function pointer that when called by C code invokes fn with arguments unmarshalled from C
    //
    // libffi closures use a two-pointer pattern for w^x page protection
    //
    // ffi_closure_alloc     returns a writable address for setup
    // ffi_prep_closure_loc  fills in an executable address for passing to C
    //
    // the closure entry is owned by FFIState::callbacks as a unique_ptr, keyed
    // by the executable address itself, the same address the returned
    // Value wraps and that C code will call through
    // release with ffi_callback_free when the C library no longer holds the pointer
    interp->bindSig("ffi_callback",
        {pFn("fn"), pAny("ret"), pArr("arg_types")},
    [ffi, interp](const std::vector<Value>& args) -> Value {
        const auto& argTypeVals = args[2].asArray();
        size_t nargs = argTypeVals.size();

        auto entry = std::make_unique<FFIState::CallbackEntry>();
        entry->embrFn  = args[0];
        entry->retDesc = resolveDescArg(args[1], "ffi_callback", "ret");
        entry->interp  = interp;
        entry->argDescs.resize(nargs);
        entry->argFFITypes.resize(nargs);

        for (size_t i = 0; i < nargs; ++i) {
            entry->argDescs[i]     = resolveDescArg(argTypeVals[i], "ffi_callback",
                                                    "arg_types[" + std::to_string(i) + "]");
            entry->argFFITypes[i] = descToFFIType(*entry->argDescs[i].ptr);
        }

        ffi_type* retType = descToFFIType(*entry->retDesc.ptr);

        // allocate writable page. execPtr is the matching executable address
        void* execPtr = nullptr;
        entry->closure = static_cast<ffi_closure*>(
            ffi_closure_alloc(sizeof(ffi_closure), &execPtr)
        );
        if (!entry->closure)
            raiseError("ffi_callback", "ffi_closure_alloc failed");

        ffi_status s = ffi_prep_cif(
            &entry->cif,
            FFI_DEFAULT_ABI,
            (unsigned int)nargs,
            retType,
            nargs > 0 ? entry->argFFITypes.data() : nullptr
        );
        if (s != FFI_OK) {
            ffi_closure_free(entry->closure);
            entry->closure = nullptr;
            raiseError("ffi_callback",
                "ffi_prep_cif failed for callback (status=" + std::to_string((int)s) + ")");
        }

        // entry->get() is the stable userdata pointer the trampoline receives.
        s = ffi_prep_closure_loc(
            entry->closure,
            &entry->cif,
            ffiClosureTrampoline,
            entry.get(),
            execPtr
        );
        if (s != FFI_OK) {
            ffi_closure_free(entry->closure);
            entry->closure = nullptr;
            raiseError("ffi_callback",
                "ffi_prep_closure_loc failed (status=" + std::to_string((int)s) + ")");
        }

        entry->fnptr = execPtr;

        // key by the executable address itself: that's what the returned
        // Value wraps (it must, so it can flow straight into a ptr-typed
        // ffi_call argument) and so what ffi_callback_free must look up by
        const void* key = execPtr;
        ffi->callbacks[key] = std::move(entry);

        return Value::makePointer(execPtr, "ffi.callback");
    });

    // ffi_callback_free(cb: ptr<ffi.callback>)
    //
    // releases the executable page and the embr function reference
    // must be called once C code no longer holds the function pointer
    interp->bindSig("ffi_callback_free", {pPtr("cb")},
    [ffi](const std::vector<Value>& args) -> Value {
        const void* key = args[0].asPointer("ffi.callback").ptr;
        if (!ffi->callbacks.erase(key))
            raiseError("ffi_callback_free", "callback has already been freed");
        return Value(0.0);
    });

    // ptr_null() -> ptr
    //
    // returns a null pointer value
    // there is no literal syntax for pointers in embr so this is the canonical way to produce one
    interp->bind("ptr_null",
    [](const std::vector<Value>&) -> Value {
        return Value::makePointer(nullptr);
    });

    // ptr_offset(p, n: num) -> ptr
    //
    // returns a pointer displaced by n bytes
    // used to access struct fields by known offset or to step through arrays of C structs
    interp->bindSig("ptr_offset", {pPtr("p"), pOpt("n")},
    [](const std::vector<Value>& args) -> Value {
        ptrdiff_t off = args.size() > 1 ? (ptrdiff_t)doubleToInt64(args[1].asNumber(), "ptr_offset", "n") : 0;
        uint8_t* base = static_cast<uint8_t*>(args[0].asPointer().ptr);
        return Value::makePointer(base + off);
    });

    // ptr_read(p, desc) -> any
    // reads a value from raw memory at p, interpreted per desc (an embrtypes descriptor). use ptr_offset to walk
    // struct fields, or pass a struct_type() desc directly
    interp->bindSig("ptr_read", {pPtr("p"), pAny("desc")},
    [](const std::vector<Value>& args) -> Value {
        const TypeDesc* d = embrtypes::requireDesc(args[1], "ptr_read", "desc");
        void* p = args[0].asPointer().ptr;
        if (!p) raiseError("ptr_read", "cannot read from null pointer");
        return unmarshalReturn(*d, p);
    });

    // ptr_write(p, desc, val: any)
    // writes val into raw memory at p per desc. for a struct_type(), val must be a map keyed by field name, and
    // every field is written to its computed offset, nested structs included
    interp->bindSig("ptr_write", {pPtr("p"), pAny("desc"), pAny("val")},
    [](const std::vector<Value>& args) -> Value {
        const TypeDesc* d = embrtypes::requireDesc(args[1], "ptr_write", "desc");
        void* p = args[0].asPointer().ptr;
        if (!p) raiseError("ptr_write", "cannot write to null pointer");
        writeValueToMemory("ptr_write", p, *d, args[2]);
        return Value(0.0);
    });

    // ffi_call_var(sym: ptr<ffi.sym>, ret: desc|nil, arg_types: arr of desc, args: arr, nfixed: num) -> any
    // like ffi_call, for variadic C functions (printf-style). nfixed is how many of arg_types/args are the declared
    // fixed parameters (1 for printf's format string), the rest are the variadic tail. it uses ffi_prep_cif_var, which
    // libffi needs for ABI-correct variadic calls (x86-64 SysV must set %al to the number of vector registers used)
    interp->bindSig("ffi_call_var",
        {pPtr("sym"), pAny("ret"), pArr("arg_types"), pArr("args"), pNum("nfixed")},
    [](const std::vector<Value>& args) -> Value {
        auto* sym = args[0].userdata<FFIState::SymHandle>("ffi.sym");
        if (!sym->raw && sym->lib.expired())
            raiseError("ffi_call_var", "the symbol's library has been closed");

        const auto& argTypeVals = args[2].asArray();
        const auto& argVals     = args[3].asArray();
        if (argTypeVals.size() != argVals.size())
            raiseError("ffi_call_var",
                "arg_types has " + std::to_string(argTypeVals.size()) +
                " entries but args has " + std::to_string(argVals.size()));

        size_t nargs  = argTypeVals.size();
        size_t nfixed = (size_t)nfixedArg(args[4], "ffi_call_var");
        if (nfixed > nargs)
            raiseError("ffi_call_var", "nfixed (" + std::to_string(nfixed) +
                       ") cannot exceed the total argument count (" + std::to_string(nargs) + ")");

        std::vector<const TypeDesc*> argInfo(nargs);
        std::vector<ffi_type*>       ffiArgTypes(nargs);
        for (size_t i = 0; i < nargs; ++i) {
            argInfo[i]     = embrtypes::requireDesc(argTypeVals[i], "ffi_call_var",
                                                    "arg_types[" + std::to_string(i) + "]");
            ffiArgTypes[i] = descToFFIType(*argInfo[i]);
        }
        const TypeDesc* retInfo = resolveDescArg(args[1], "ffi_call_var", "ret").ptr;
        ffi_type* ffiRet = descToFFIType(*retInfo);

        ffi_cif cif;
        ffi_status status = ffi_prep_cif_var(
            &cif, FFI_DEFAULT_ABI, (unsigned int)nfixed, (unsigned int)nargs, ffiRet,
            nargs > 0 ? ffiArgTypes.data() : nullptr
        );
        if (status != FFI_OK)
            raiseError("ffi_call_var", "ffi_prep_cif_var failed (status=" +
                       std::to_string((int)status) + ")");

        return performCall("ffi_call_var", cif, sym->fnptr, argInfo, *retInfo, argVals);
    });

    // ffi_prepare_var(sym: ptr<ffi.sym>, ret: desc|nil, arg_types: arr of desc, nfixed: num) -> ptr<ffi.prepared>
    // like ffi_prepare for variadic functions (see ffi_call_var for nfixed). libffi bakes the total argument count
    // into the cif, so a prepared variadic call only works with ffi_invoke() calls of exactly that many arguments.
    // a different count needs its own ffi_prepare_var/ffi_call_var
    interp->bindSig("ffi_prepare_var",
        {pPtr("sym"), pAny("ret"), pArr("arg_types"), pNum("nfixed")},
    [](const std::vector<Value>& args) -> Value {
        auto* sym = args[0].userdata<FFIState::SymHandle>("ffi.sym");
        if (!sym->raw && sym->lib.expired())
            raiseError("ffi_prepare_var", "the symbol's library has been closed");

        const auto& argTypeVals = args[2].asArray();
        size_t nargs  = argTypeVals.size();
        size_t nfixed = (size_t)nfixedArg(args[3], "ffi_prepare_var");
        if (nfixed > nargs)
            raiseError("ffi_prepare_var", "nfixed (" + std::to_string(nfixed) +
                       ") cannot exceed the total argument count (" + std::to_string(nargs) + ")");

        auto prep = std::make_shared<FFIState::PreparedCall>();
        prep->fnptr   = sym->fnptr;
        prep->symName = sym->name;
        prep->lib     = sym->lib;
        prep->raw     = sym->raw;

        prep->argDescs.resize(nargs);
        prep->argFFITypes.resize(nargs);
        for (size_t i = 0; i < nargs; ++i) {
            prep->argDescs[i]     = resolveDescArg(argTypeVals[i], "ffi_prepare_var",
                                                   "arg_types[" + std::to_string(i) + "]");
            prep->argFFITypes[i] = descToFFIType(*prep->argDescs[i].ptr);
        }
        prep->retDesc    = resolveDescArg(args[1], "ffi_prepare_var", "ret");
        prep->retFFIType = descToFFIType(*prep->retDesc.ptr);

        ffi_status status = ffi_prep_cif_var(
            &prep->cif, FFI_DEFAULT_ABI, (unsigned int)nfixed, (unsigned int)nargs,
            prep->retFFIType, nargs > 0 ? prep->argFFITypes.data() : nullptr
        );
        if (status != FFI_OK)
            raiseError("ffi_prepare_var", "ffi_prep_cif_var failed (status=" +
                       std::to_string((int)status) + ")");

        return Value(TypedPtr(prep.get(), "ffi.prepared", prep));
    });
}

// what this plugin says about itself. embr reads it before it runs embr_register (see core/plugininfo.h)
EMBR_PLUGIN_INFO("ffi", "1.0.0", "libffi-based foreign function interface", EMBR_CAP_NATIVE, EMBR_CAP_NONE,
    {"ffi_call", "one-shot: rebuilds the cif on every call, fine for a single call, wasteful in a loop"},
    {"ffi_call_var", "variadic version. uses ffi_prep_cif_var, which the ABI needs (x86-64 SysV, for one, counts vector registers in %al)"},
    {"ffi_callback", "makes a real executable C function pointer that calls back into fn. not refcounted: it lives until ffi_callback_free"},
    {"ffi_callback_free", "raises if already freed"},
    {"ffi_close", "closes right away (not deferred to the gc), raises if already closed"},
    {"ffi_invoke", "fast path for a prepared call"},
    {"ffi_fn", "wraps a function address that did not come from ffi_sym (a GetProcAddress result, a function pointer in a struct). nothing checks the address"},
    {"ffi_open", "dlopen, raises with the platform's own dlerror text"},
    {"ffi_prepare", "resolves the types and runs ffi_prep_cif once, then caches it"},
    {"ffi_prepare_var", "cached variadic prepare. only valid for calls with exactly this total argument count, because libffi bakes the total count into the cif"},
    {"ffi_sym", "holds only a weak reference to the lib handle, so it raises cleanly (no undefined behavior) if the library was closed first"},
    {"ptr_null", "canonical null pointer (no pointer literal syntax in embr)"},
    {"ptr_offset", "pointer displaced by n bytes"},
    {"ptr_read", "raise on a null pointer; struct descriptors expect/produce a field-name-keyed map"},
    {"ptr_write", "raise on a null pointer; struct descriptors expect/produce a field-name-keyed map"});
