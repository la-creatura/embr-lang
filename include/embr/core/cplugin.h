// cplugin.h
// the host side of the C plugin interface (include/embr_plugin.h): reads a C plugin's table and binds its functions.
//
// good to know
//   - a C function never sees an exception. every api call here catches, records the message in the ctx, and returns
//     nonzero. after the C function returns, one error is raised for the script
//   - EmbrValue and Value have the same layout (checked below), so arguments are passed as they sit, no copy
//   - a function with at most 4 fixed parameters is also bound for the VM's typed call (Callable::typed): the VM checks
//     the argument types itself and calls typedThunk, with no std::function and no signature check

#ifndef EMBR_CORE_CPLUGIN_H
#define EMBR_CORE_CPLUGIN_H

#include "../../embr_plugin.h"
#include "registry.h"
#include "typed.h"

#include <algorithm>
#include <iostream>
#if defined(_WIN32)
#  include <fcntl.h>
#  include <io.h>
#endif
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace embr {

static_assert(sizeof(EmbrValue) == sizeof(Value), "EmbrValue must have the layout of Value");
static_assert(offsetof(EmbrValue, nest_depth) == offsetof(Value, nestDepth), "EmbrValue layout");
static_assert(offsetof(EmbrValue, tag) == offsetof(Value, tg), "EmbrValue layout");
static_assert(offsetof(EmbrValue, u) == offsetof(Value, u), "EmbrValue layout");

namespace cplug {

// one call. kept small: it is built on the stack for every call of a C function
struct Ctx : EmbrCtx {
    Interpreter*                        interp = nullptr;
    bool                                failed = false;
    char                                error[512];
    char                                kind[48] = {0};      // the error kind the plugin asked for (fail_kind), empty: the label of the function
    // temp slots: the first few live in the Ctx itself (built only when asked for), the rest in a vector
    alignas(Value) unsigned char        inl[4 * sizeof(Value)];
    int                                 ninl = 0;
    std::vector<std::unique_ptr<Value>>* temps = nullptr;
    std::vector<std::unique_ptr<std::vector<EmbrMapEntry>>>* entries = nullptr;   // the same for embrp_map_entries
    std::vector<std::unique_ptr<std::string>>* strings = nullptr;                 // the same for embrp_to_string

    Ctx() = default;
    Ctx(const Ctx&) = delete;
    ~Ctx() {
        for (int i = 0; i < ninl; ++i) reinterpret_cast<Value*>(inl + i * sizeof(Value))->~Value();
        delete temps; delete entries; delete strings;
    }

    void setError(const char* msg) {
        if (failed) return;
        failed = true;
        std::strncpy(error, msg, sizeof error - 1);
        error[sizeof error - 1] = 0;
    }
    void setError(const char* k, const char* msg) {
        if (failed) return;
        setError(msg);
        std::strncpy(kind, k ? k : "", sizeof kind - 1);
        kind[sizeof kind - 1] = 0;
    }
};

inline const Value& V(const EmbrValue* v) { return *reinterpret_cast<const Value*>(v); }
inline Value&       V(EmbrValue* v)       { return *reinterpret_cast<Value*>(v); }

// runs f, turning an exception into a recorded error. returns 0 or 1
template<class F> int guarded(EmbrCtx* c, F&& f) {
    Ctx* ctx = static_cast<Ctx*>(c);
    try { f(); return 0; }
    catch (const std::exception& e) { ctx->setError(e.what()); }
    catch (...) { ctx->setError("unknown error"); }
    return 1;
}

inline const EmbrApi* api() {
    static const EmbrApi table = [] {
        EmbrApi a{};
        a.abi_version = EMBR_PLUGIN_ABI_VERSION;
        a.struct_size = sizeof(EmbrApi);
        a.str      = [](const EmbrValue* v, size_t* len) -> const char* {
            if (!V(v).isString()) { if (len) *len = 0; return ""; }
            const std::string& s = V(v).asString();
            if (len) *len = s.size();
            return s.c_str();
        };
        a.array_len = [](const EmbrValue* v) -> size_t { auto* arr = V(v).arrayLike(); return arr ? arr->size() : 0; };
        a.array_at  = [](const EmbrValue* v, size_t i) -> const EmbrValue* {
            auto* arr = V(v).arrayLike();
            return arr && i < arr->size() ? reinterpret_cast<const EmbrValue*>(&(*arr)[i]) : nullptr;
        };
        a.map_len = [](const EmbrValue* v) -> size_t { return V(v).isMap() ? V(v).asMap().size() : 0; };
        a.map_get = [](const EmbrValue* v, const char* key) -> const EmbrValue* {
            if (!V(v).isMap() || !key) return nullptr;
            const auto& m = V(v).asMap();
            auto it = m.find(key);
            return it == m.end() ? nullptr : reinterpret_cast<const EmbrValue*>(&it->second);
        };
        a.map_entries = [](EmbrCtx* c, const EmbrValue* v, size_t* count) -> const EmbrMapEntry* {
            Ctx* ctx = static_cast<Ctx*>(c);
            *count = 0;
            if (!V(v).isMap()) return nullptr;
            const auto& m = V(v).asMap();
            std::vector<const Value::map_type::value_type*> nodes;
            nodes.reserve(m.size());
            for (const auto& kv : m) nodes.push_back(&kv);
            std::sort(nodes.begin(), nodes.end(), [](auto* x, auto* y) { return x->first < y->first; });
            auto list = std::make_unique<std::vector<EmbrMapEntry>>();
            list->reserve(nodes.size());
            for (auto* kv : nodes) list->push_back({kv->first.data(), kv->first.size(), reinterpret_cast<const EmbrValue*>(&kv->second)});
            if (!ctx->entries) ctx->entries = new std::vector<std::unique_ptr<std::vector<EmbrMapEntry>>>();
            ctx->entries->push_back(std::move(list));
            *count = nodes.size();
            return ctx->entries->back()->data();
        };
        a.parse_float = [](const char* text, size_t len, double* out) -> int {
            double d;
            auto r = parse_double(text, text + len, d);
            if (r.ec != std::errc() || r.ptr != text + len) return 1;
            *out = d;
            return 0;
        };
        a.ptr_get = [](const EmbrValue* v, const char* tag) -> void* {
            if (!V(v).isPointer() || !tag) return nullptr;
            const TypedPtr& p = V(v).asPointer();
            return p.type == tag ? p.ptr : nullptr;
        };
        a.ptr_tag = [](const EmbrValue* v) -> const char* { return V(v).isPointer() ? V(v).asPointer().type.c_str() : ""; };
        a.to_string = [](EmbrCtx* c, const EmbrValue* v, size_t* len) -> const char* {
            Ctx* ctx = static_cast<Ctx*>(c);
            std::unique_ptr<std::string> s;
            try { s = std::make_unique<std::string>(V(v).formatAsString()); }
            catch (const std::exception& e) { ctx->setError(e.what()); if (len) *len = 0; return ""; }
            if (!ctx->strings) ctx->strings = new std::vector<std::unique_ptr<std::string>>();
            ctx->strings->push_back(std::move(s));
            if (len) *len = ctx->strings->back()->size();
            return ctx->strings->back()->c_str();
        };
        a.truthy  = [](const EmbrValue* v) -> int { return V(v).truthy() ? 1 : 0; };
        a.set_int   = [](EmbrCtx* c, EmbrValue* s, int64_t x) -> int { return guarded(c, [&] { V(s) = Value(x); }); };
        a.set_float = [](EmbrCtx* c, EmbrValue* s, double x) -> int { return guarded(c, [&] { V(s) = Value(x); }); };
        a.set_str   = [](EmbrCtx* c, EmbrValue* s, const char* p, size_t n) -> int {
            return guarded(c, [&] { V(s) = Value(std::string(p ? p : "", p ? n : 0)); });
        };
        a.set_str_alloc = [](EmbrCtx* c, EmbrValue* s, size_t len) -> char* {
            char* where = nullptr;
            guarded(c, [&] {
                if (len > (size_t)kMaxScriptAlloc) raiseError("value", "string too long");
                V(s) = Value(std::string(len, '\0'));
                where = const_cast<char*>(V(s).asString().data());
            });
            return where;
        };
        a.set_copy  = [](EmbrCtx* c, EmbrValue* s, const EmbrValue* f) -> int { return guarded(c, [&] { V(s) = V(f); }); };
        a.set_array = [](EmbrCtx* c, EmbrValue* s, size_t reserve) -> int {
            return guarded(c, [&] { Value::array_type arr; arr.reserve(std::min<size_t>(reserve, 1 << 20)); V(s) = Value(std::move(arr)); });
        };
        a.array_push = [](EmbrCtx* c, EmbrValue* arr, const EmbrValue* item) -> int {
            return guarded(c, [&] { V(arr).arrayAppend(V(item)); });
        };
        a.array_push_str = [](EmbrCtx* c, EmbrValue* arr, const char* p, size_t n) -> int {
            return guarded(c, [&] { V(arr).arrayAppend(Value(std::string(p ? p : "", p ? n : 0))); });
        };
        a.array_push_int   = [](EmbrCtx* c, EmbrValue* arr, int64_t x) -> int { return guarded(c, [&] { V(arr).arrayAppend(Value(x)); }); };
        a.array_push_float = [](EmbrCtx* c, EmbrValue* arr, double x) -> int { return guarded(c, [&] { V(arr).arrayAppend(Value(x)); }); };
        a.set_map = [](EmbrCtx* c, EmbrValue* s) -> int { return guarded(c, [&] { V(s) = Value(Value::map_type{}); }); };
        a.map_set = [](EmbrCtx* c, EmbrValue* m, const char* key, size_t kn, const EmbrValue* item) -> int {
            return guarded(c, [&] { V(m).mapSet(std::string(key ? key : "", key ? kn : 0), V(item)); });
        };
        a.map_set_str = [](EmbrCtx* c, EmbrValue* m, const char* key, size_t kn, const char* p, size_t n) -> int {
            return guarded(c, [&] { V(m).mapSet(std::string(key ? key : "", key ? kn : 0), Value(std::string(p ? p : "", p ? n : 0))); });
        };
        a.set_ptr = [](EmbrCtx* c, EmbrValue* s, void* p, const char* tag, void (*fin)(void*)) -> int {
            return guarded(c, [&] {
                std::shared_ptr<void> own;
                if (fin) own = std::shared_ptr<void>(p, [fin](void* q) { fin(q); });
                V(s) = Value(TypedPtr(p, tag ? tag : "", own));
            });
        };
        a.temp = [](EmbrCtx* c) -> EmbrValue* {
            Ctx* ctx = static_cast<Ctx*>(c);
            if (ctx->ninl < 4) return reinterpret_cast<EmbrValue*>(new (ctx->inl + (ctx->ninl++) * sizeof(Value)) Value());
            if (!ctx->temps) ctx->temps = new std::vector<std::unique_ptr<Value>>();
            ctx->temps->push_back(std::make_unique<Value>());
            return reinterpret_cast<EmbrValue*>(ctx->temps->back().get());
        };
        a.fail = [](EmbrCtx* c, const char* msg) -> int {
            static_cast<Ctx*>(c)->setError(msg ? msg : "error");
            return 1;
        };
        a.root = [](EmbrCtx* c, const EmbrValue* v) -> EmbrRoot* {
            Ctx* ctx = static_cast<Ctx*>(c);
            Value* r = new Value(V(v));
            ctx->interp->cRoots.insert(r);
            return reinterpret_cast<EmbrRoot*>(r);
        };
        a.root_get = [](const EmbrRoot* r) -> const EmbrValue* { return reinterpret_cast<const EmbrValue*>(r); };
        a.script_dir = [](EmbrCtx* c) -> const char* { return static_cast<Ctx*>(c)->interp->scriptDir.c_str(); };
        a.array_data = [](EmbrCtx* c, const EmbrValue* v, size_t* len) -> const EmbrValue* {
            static const EmbrValue none{};
            *len = 0;
            const std::vector<Value>* arr = nullptr;
            try { arr = V(v).arrayLike(); }       // a lazy range is made here, and is refused when it is too long
            catch (const std::exception& e) { static_cast<Ctx*>(c)->setError(e.what()); return nullptr; }
            if (!arr) return nullptr;
            *len = arr->size();
            return arr->empty() ? &none : reinterpret_cast<const EmbrValue*>(arr->data());
        };
        a.array_push_ints = [](EmbrCtx* c, EmbrValue* arr, const int64_t* xs, size_t n) -> int {
            return guarded(c, [&] { for (size_t i = 0; i < n; ++i) V(arr).arrayAppend(Value(xs[i])); });
        };
        a.array_push_floats = [](EmbrCtx* c, EmbrValue* arr, const double* xs, size_t n) -> int {
            return guarded(c, [&] { for (size_t i = 0; i < n; ++i) V(arr).arrayAppend(Value(xs[i])); });
        };
        a.fail_kind = [](EmbrCtx* c, const char* kind, const char* msg) -> int {
            static_cast<Ctx*>(c)->setError(kind, msg ? msg : "error");
            return 1;
        };
        a.script_arg_count = [](EmbrCtx* c) -> size_t { return static_cast<Ctx*>(c)->interp->scriptArgs.size(); };
        a.script_arg = [](EmbrCtx* c, size_t i, size_t* len) -> const char* {
            const auto& args = static_cast<Ctx*>(c)->interp->scriptArgs;
            if (i >= args.size()) { if (len) *len = 0; return nullptr; }
            if (len) *len = args[i].size();
            return args[i].c_str();
        };
        // the streams print() and input() use, so the order stays right. on windows a stream is switched to binary mode
        // on first use, or it would turn \n into \r\n and stop at ^Z
        a.stdio_write = [](EmbrCtx* c, int fd, const char* p, size_t n) -> int {
            Ctx* ctx = static_cast<Ctx*>(c);
#if defined(_WIN32)
            static bool bin[3] = {false, false, false};
            if (fd >= 0 && fd <= 2 && !bin[fd]) { bin[fd] = true; std::cout.flush(); _setmode(fd, _O_BINARY); }
#endif
            if (fd == 2) {
                std::cout.flush();
                std::cerr.write(p, (std::streamsize)n);
                std::cerr.flush();
                if (!std::cerr) { ctx->setError("write failed"); return 1; }
            } else {
                std::cout.write(p, (std::streamsize)n);
                if (!std::cout) { ctx->setError("write failed"); return 1; }
            }
            return 0;
        };
        a.stdio_flush = [](EmbrCtx* c) -> int {
            std::cout.flush();
            if (!std::cout) { static_cast<Ctx*>(c)->setError("flush failed"); return 1; }
            return 0;
        };
        a.stdin_read = [](EmbrCtx*, char* buf, size_t n) -> size_t {
#if defined(_WIN32)
            static bool bin = (_setmode(0, _O_BINARY), true); (void)bin;
#endif
            std::cin.read(buf, (std::streamsize)n);
            size_t got = (size_t)std::cin.gcount();
            if (std::cin.eof()) std::cin.clear(std::ios::eofbit);   // keep eof, but let a later read ask again
            return got;
        };
        a.unroot = [](EmbrCtx* c, EmbrRoot* r) {
            Ctx* ctx = static_cast<Ctx*>(c);
            Value* v = reinterpret_cast<Value*>(r);
            if (ctx->interp->cRoots.erase(v)) delete v;
        };
        return a;
    }();
    return &table;
}

// what a bound C function needs when it is called. `label` is the error context: "[random:rand_bytes]". `n` is the
// number of arguments for the typed call, which only runs with exactly that many
struct Bound {
    EmbrFn         fn;
    void*          userdata;
    Interpreter*   interp;
    std::string    label;
    size_t         n = 0;
    const EmbrApi* table = nullptr;   // api(), looked up once
};

inline Value call(const Bound& b, const Value* args, size_t n) {
    Ctx ctx;
    ctx.api = b.table;
    ctx.userdata = b.userdata;
    ctx.interp = b.interp;
    Value out;
    int rc = b.fn(&ctx, reinterpret_cast<const EmbrValue*>(args), n, reinterpret_cast<EmbrValue*>(&out));
    if (rc != 0 || ctx.failed) {
        if (!ctx.kind[0]) raiseError(b.label, ctx.failed ? ctx.error : "failed");
        // a kind the plugin chose: it has to survive the native call that re-raises with a position (keepContext)
        try { raiseError(ctx.kind, ctx.error); }
        catch (EmbrError& e) { e.keepContext = true; throw; }
    }
    return out;
}

// the VM's typed call (Callable::typed): `fn` is the Bound
inline Value typedThunk(void* fn, void*, const Value* args) {
    const Bound& b = *static_cast<const Bound*>(fn);
    return call(b, args, b.n);
}

} // namespace cplug

// what a table must satisfy before anything of the plugin runs: the interface number, the size of the table, the build id
// of a C++ plugin, and the capabilities the host denies. returns "" or why the plugin is refused
inline std::string checkPluginInfo(const Interpreter& interp, const EmbrPluginInfo* info, const std::string& path, bool isCxx) {
    if (!info) return "'embr_plugin_info' returned nothing in \"" + path + "\"";
    if (info->abi_version != EMBR_PLUGIN_ABI_VERSION)
        return "plugin \"" + path + "\" was built for plugin ABI " + std::to_string(info->abi_version) +
               ", this embr uses ABI " + std::to_string(EMBR_PLUGIN_ABI_VERSION) + ". rebuild it against this embr's embr_plugin.h";
    // fields added to the end of EmbrPluginInfo are read only when the plugin's struct is big enough to have them
    const size_t minSize = offsetof(EmbrPluginInfo, nfuncs) + sizeof(info->nfuncs);
    if (info->struct_size < minSize)
        return "plugin \"" + path + "\" has a too small EmbrPluginInfo (" + std::to_string(info->struct_size) + " bytes)";
    const char* pname = info->name ? info->name : "?";
    if (isCxx) {
        const bool hasBuildId = info->struct_size >= offsetof(EmbrPluginInfo, build_id) + sizeof(info->build_id);
        const bool hasTag = info->struct_size >= offsetof(EmbrPluginInfo, abi_tag) + sizeof(info->abi_tag);
        const char* theirs = hasBuildId && info->build_id ? info->build_id : "(none)";
        const char* theirTag = hasTag && info->abi_tag ? info->abi_tag : "(none)";
        // the C++ ABI decides. a plugin built by another compiler of the same ABI loads, with a note, because it is the exact toolchain
        // that is different and not what the two sides can share
        if (std::string(theirTag) != abiTag())
            return std::string("plugin \"") + pname + "\" (" + path + ") was built with a different compiler, standard library or embr C++ interface "
                   "than this embr, so it cannot be loaded safely.\n  the plugin: " + theirTag + "  (" + theirs + ")\n  this embr: " + abiTag() + "  (" + buildId() +
                   ")\n  rebuild the plugin with a toolchain of the same C++ ABI as the one that built embr";
        if (std::string(theirs) != buildId() && !std::getenv("EMBR_NO_ABI_WARNINGS"))
            std::cerr << "embr: warning: plugin \"" << pname << "\" (" << path << ") was built with a different toolchain than this embr\n  the plugin: "
                      << theirs << "\n  this embr: " << buildId() << "\n  both have the C++ ABI " << abiTag()
                      << ", so it is loaded. if it misbehaves, rebuild it with the toolchain that built embr (EMBR_NO_ABI_WARNINGS=1 hides this)\n";
    }
    const uint64_t denied = interp.pluginPolicy.deniedCaps;
    if (info->capabilities & denied) {
        std::string names;
        for (const auto& n : capNames(info->capabilities & denied)) names += (names.empty() ? "" : ", ") + n;
        return std::string("plugin \"") + pname + "\" is not allowed in this sandbox: it needs " + names;
    }
    return "";
}

// the record of an imported plugin. `info` may be null for a C++ plugin built before there was a description
inline void recordPlugin(Interpreter& interp, const EmbrPluginInfo* info, const std::string& path, bool isCxx) {
    Interpreter::PluginRecord rec;
    rec.path = path;
    rec.kind = isCxx ? Interpreter::PluginRecord::Kind::Cxx : Interpreter::PluginRecord::Kind::C;
    if (info) {
        rec.declared = true;
        rec.name = info->name ? info->name : "";
        rec.version = info->version ? info->version : "";
        rec.description = info->description ? info->description : "";
        rec.abi = info->abi_version;
        rec.caps = info->capabilities;
        if (info->struct_size >= offsetof(EmbrPluginInfo, optional_capabilities) + sizeof(info->optional_capabilities))
            rec.optionalCaps = info->optional_capabilities;
        if (info->struct_size >= offsetof(EmbrPluginInfo, build_id) + sizeof(info->build_id) && info->build_id)
            rec.buildId = info->build_id;
        if (info->struct_size >= offsetof(EmbrPluginInfo, abi_tag) + sizeof(info->abi_tag) && info->abi_tag)
            rec.abiTag = info->abi_tag;
        for (uint32_t i = 0; i < info->nfuncs; ++i)
            if (info->funcs[i].name) rec.fns.push_back({info->funcs[i].name, info->funcs[i].doc ? info->funcs[i].doc : ""});
    } else {
        // no description: the name is the file's
        size_t slash = path.find_last_of("/\\"), dot = path.find_last_of('.');
        size_t from = slash == std::string::npos ? 0 : slash + 1;
        rec.name = path.substr(from, dot == std::string::npos || dot < from ? std::string::npos : dot - from);
    }
    interp.plugins.push_back(std::move(rec));
    interp.installPendingOverloads();   // the plugin's bare group names work in code that was compiled before it was imported
}

// reads the table a C plugin returned and binds its functions. returns "" or why it was refused
inline std::string loadCPlugin(Interpreter& interp, const EmbrPluginInfo* info, const std::string& path) {
    std::string why = checkPluginInfo(interp, info, path, /*isCxx=*/false);
    if (!why.empty()) return why;
    const bool hasInit = info->struct_size >= offsetof(EmbrPluginInfo, init) + sizeof(info->init);
    const char* pname = info->name ? info->name : "?";

    if (hasInit && info->init) {
        cplug::Ctx ctx;
        ctx.api = cplug::api();
        ctx.userdata = nullptr;
        ctx.interp = &interp;
        int rc = info->init(&ctx);
        if (rc != 0 || ctx.failed)
            return std::string("plugin \"") + pname + "\" refused to load: " + (ctx.failed ? ctx.error : "init failed");
    }

    // the bound functions keep a copy of what they need: the table in the plugin is only read here
    const bool useTyped = typed::typedCallsEnabled();
    for (uint32_t i = 0; i < info->nfuncs; ++i) {
        const EmbrFunc& f = info->funcs[i];
        if (!f.name || !f.fn) return std::string("plugin \"") + pname + "\" lists a function with no name or no code";
        std::vector<Param> sig;
        bool fixed = f.nparams <= 4;   // no optional or variadic parameter, for the typed call
        uint8_t masks[4] = {0, 0, 0, 0};
        for (uint32_t k = 0; k < f.nparams; ++k) {
            const EmbrParam& p = f.params[k];
            TypeSet ts((uint8_t)(p.types ? p.types : (uint32_t)EMBR_T_ANY));
            std::string nm = p.name ? p.name : "arg";
            if (p.flags & EMBR_PARAM_VARIADIC) { sig.push_back(Param::rest(nm, ts)); fixed = false; }
            else if (p.flags & EMBR_PARAM_OPTIONAL) { sig.push_back(Param::opt(nm, ts)); fixed = false; }
            else sig.push_back(Param::req(nm, ts));
            if (k < 4) masks[k] = ts.mask;
        }
        auto b = std::make_shared<cplug::Bound>(cplug::Bound{f.fn, f.userdata, &interp, "[" + std::string(pname) + ":" + f.name + "]", f.nparams, cplug::api()});
        interp.bindSpan(f.name, std::move(sig), [b](const ArgSpan& args) -> Value { return cplug::call(*b, args.p, args.n); });
        Value::Callable& c = const_cast<Value::Callable&>(interp.getGlobal(f.name).asCallable());
        if (f.flags & EMBR_FUNC_PURE) c.pure = true;
        if (f.result_types && f.result_types != (uint32_t)EMBR_T_ANY) c.returns = TypeSet((uint8_t)f.result_types);
        if (f.result_elem) { auto a = std::make_shared<TypeAnn>(); a->t = TypeSet((uint8_t)f.result_elem); c.returnsElem = a; }
        if (fixed && useTyped) {
            c.typed.thunk = &cplug::typedThunk;
            c.typed.fn = b.get();
            c.typed.n = (uint8_t)f.nparams;
            for (uint32_t k = 0; k < f.nparams; ++k) c.typed.mask[k] = masks[k];
        }
    }

    bool firstC = true;   // the gc learns about rooted values once, with the first C plugin
    for (const auto& r : interp.plugins) if (r.kind == Interpreter::PluginRecord::Kind::C) firstC = false;
    if (firstC) {
        Interpreter* ip = &interp;
        interp.addGcRootProvider([ip](const TraceVisitor& visit) { for (Value* v : ip->cRoots) visit(*v); });
    }
    recordPlugin(interp, info, path, /*isCxx=*/false);
    return "";
}

} // namespace embr

#endif // EMBR_CORE_CPLUGIN_H
