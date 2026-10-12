#ifndef EMBR_CORE_REGISTRY_H
#define EMBR_CORE_REGISTRY_H

// the plugin-facing runtime with global scope, plugin handles, source map, and module import/export

#include "diagnostics.h"
#include "types.h"
#include "value.h"
#include "typed.h"
#include "ast.h"
#include "platform.h"

#include <chrono>
#include <atomic>
#include <string>
#include <vector>
#include <algorithm>
#include <deque>
#include <map>
#include <set>
#include <unordered_map>
#include <memory>
#include <unordered_set>
#include <iostream>

#include "plugininfo.h"

namespace embr {

// which native plugins a script may `import`, and whether embrlib may touch files
// the default is unrestricted. a restricted policy (--sandbox, --allow-plugins, --no-plugins, or an embedder's own)
// is enforced in one place, importNativePlugin() in module.h, which every backend calls. it is also copied into the
// worker Interpreters that parallel/async create, so a script can't escape by spawning a worker
// this is an import allow-list, not a full sandbox: nothing limits cpu, memory or time, and an allowed plugin can do anything it can do
struct PluginPolicy {
    bool                     restricted = false;   // false: everything allowed
    std::set<std::string>    allowed;              // plugin names (bare, no extension) when restricted
    bool                     fileIo = true;        // embrlib's load_file / write_file / append_file
    uint64_t                 deniedCaps = 0;       // EMBR_CAP_* bits a plugin may not ask for (cplugin.h). fileIo off also denies the filesystem one
    bool                     anyName = false;      // restricted, but every bare name may be imported: the capabilities decide, not the name
    bool                     requireSigned = false; // a package must be signed by a trusted key (core/package.h); an unsigned one is refused
    std::set<std::string>    trustedKeys;          // public keys (hex) trusted besides the user's trust list and the project's, set by an embedder

    // "" if importing `rawPath` is permitted, else a message saying why not
    std::string check(const std::string& rawPath) const {
        if (!restricted) return "";
        // a restricted policy only accepts bare names: a path ("./json", "../plugins/os", "/tmp/x/json")
        // would let a script pick the library file, and an attacker's json.so is a plugin whose name
        // happens to be allowed
        if (rawPath.empty() || rawPath.find_first_of("/\\") != std::string::npos ||
            rawPath.find('.') != std::string::npos)
            return "plugin imports must be bare names (no path, no extension) in a restricted sandbox: \"" + rawPath + "\"";
        if (!anyName && !allowed.count(rawPath)) {
            std::string list;
            for (const auto& n : allowed) list += (list.empty() ? "" : ", ") + n;
            return "plugin \"" + rawPath + "\" is not allowed in this sandbox (allowed: " +
                   (list.empty() ? "none" : list) + ")";
        }
        return "";
    }
};

// `abs__2` is the second member of the overload group `abs`: a name that ends in two underscores and a number with no leading zero.
// the author picks the numbers and a number is part of the public name, so adding `abs__3` never changes a call of `abs__2`.
// a bare `abs(x)` is resolved to one of the members from the argument types (see core/typecheck.h) or, when they are not known
// before the script runs, when it is called (VmRunner, vm.h). `base` and `num` are filled in when it is one.
inline bool overloadName(const std::string& name, std::string* base = nullptr, int* num = nullptr) {
    const size_t at = name.rfind("__");
    if (at == std::string::npos || at == 0 || at + 2 >= name.size()) return false;
    if (name[at - 1] == '_') return false;                       // `a___1`: not clear where the group name ends
    const char first = name[at + 2];
    if (first < '1' || first > '9') return false;
    long n = 0;
    for (size_t i = at + 2; i < name.size(); ++i) {
        if (name[i] < '0' || name[i] > '9') return false;
        n = n * 10 + (name[i] - '0');
        if (n > 1000000) return false;
    }
    if (base) *base = name.substr(0, at);
    if (num) *num = (int)n;
    return true;
}

class Interpreter;
// puts the hooks below on an interpreter. defined in core/invoke.h, called when a runner is made (see the hooks)
inline void installBackend(Interpreter& interp);

class Interpreter {
public:
    using NativeFn = Value::NativeFn;

    // how a plugin calls back into script code without compiling the backend into itself: embr::invoke(), loadEmbrModule() and
    // runSource() (core/invoke.h) call these. a VmRunner puts them on its interpreter when it is made, so by the time a plugin runs
    // they are set. an interpreter a plugin makes itself (a worker thread) copies its parent's
    struct BackendHooks {
        Value (*invoke)(Interpreter&, const Value& callee, const std::vector<Value>& args, const SourceRange& site) = nullptr;
        Value (*loadModule)(Interpreter&, const std::string& rawPath, const SourceRange& site, bool mutateCallerScope, bool forceReload) = nullptr;
        void  (*runSource)(const std::string& src, Interpreter&, const std::string& filename) = nullptr;
    };
    BackendHooks backend;

    Interpreter() {
        auto s0 = std::make_shared<Scope>();
        scopes_.push_back(s0);
        gcRegisterScope(s0);
        varTypes_.emplace_back();
        bindSig("print", {Param::rest("args", TS::Any)},
        [](const std::vector<Value>& args) -> Value {
            for (size_t i = 0; i < args.size(); ++i) {
                if (i) std::cout << ' ';
                std::cout << args[i].formatAsString();
            }
            std::cout << '\n';
            return Value(0.0);
        });
        bindSig("globals", {},
        [this](const std::vector<Value>&) -> Value {
            Value::map_type m;
            for (const auto& [k, v] : this->globals())
                m[k] = v;
            return Value(std::move(m));
        });
    }
    ~Interpreter() {
        // break reference cycles before anything is released. a closure stored in a scope captures that same scope,
        // so dropping scopes_ only drops our references and the cycle would leak at exit. emptying every live scope
        // frees their contents, and lets plugin-owned values (tables, handles) die while their plugin is still loaded
        scopesChanged();
        for (auto& sc : gcAllLiveScopes()) sc->clear();
        for (auto& c : gcAllLiveCells()) { c->v = Value(); c->defined = false; }
        // empty every plugin container that is still alive (a table holding itself is a cycle no scope owns)
        for (auto& c : clearables_) if (auto owner = c.owner.lock()) { (void)owner; c.clear(); }
        clearables_.clear();
        for (auto& hook : teardownHooks_) hook();
        teardownHooks_.clear();        // hooks are std::functions whose code may live in a plugin: drop before dlclose
        for (Value* r : cRoots) delete r;   // a rooted value can hold a handle whose finalizer is plugin code
        cRoots.clear();
        // remove anything holding plugin function pointers
        scopes_.clear();
        // GC root providers are std::functions whose code lives in the plugin that registered them;
        // destroying one after pluginClose() below would run code in an unmapped .so (segfault)
        gcRootProviders_.clear();

        for (auto h : pluginHandles_)
            pluginClose(h);
    }

    Interpreter(const Interpreter&)            = delete;
    Interpreter& operator=(const Interpreter&) = delete;

    class ScopeGuard {
        Interpreter& i_;
    public:
        explicit ScopeGuard(Interpreter& i) : i_(i) { i_.push(); }
        ~ScopeGuard()                               { i_.pop();  }
    };

    void push() {
        scopesChanged();
        auto s = std::make_shared<Scope>();
        scopes_.push_back(s);
        gcRegisterScope(s);
        varTypes_.emplace_back();
    }
    void pop()  { scopesChanged(); scopes_.pop_back(); varTypes_.pop_back(); }

    // assign into nearest scope that already holds the name
    // checking it against that scope's recorded type constraint if there is one
    // falling back to the current module's top scope (not necessarily scopes_[0]) for a name never declared
    void set(const std::string& k, Value v, TypeSet createType = TypeSet::Any()) {
        for (size_t idx = scopes_.size(); idx-- > 0; ) {
            auto it = scopes_[idx]->find(k);
            if (it == scopes_[idx]->end()) continue;
            if (!varTypes_[idx].empty()) {
                auto tyIt = varTypes_[idx].find(k);
                if (tyIt != varTypes_[idx].end() && !tyIt->second.isAny() && !tyIt->second.contains(v.tag()))
                    raiseError("runtime", "cannot assign " + v.typeName() + " to '" + k +
                               "' declared as " + tyIt->second.name());
            }
            it->second = std::move(v);
            return;
        }
        // a new name: its type is the type of this first value (or `createType`, when the compiler worked out
        // a wider one), and every later assignment is checked against it
        scopesChanged();
        TypeSet ts = createType.isAny() ? TypeSet(v.tag()) : createType;
        varTypes_[moduleBase_][k] = ts;
        (*scopes_[moduleBase_])[k] = std::move(v);
    }

    // a name assigned for the first time at top level becomes a module global. inside a function the VM makes
    // it local to that call instead (VmRunner::resolveStore). to set a global from inside a function, use the
    // embrmeta plugin

    // ideally should be wrapped in has() where used to throw a more meaningful error
    Value get(const std::string& name) const {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            auto f = (*it)->find(name);
            if (f != (*it)->end()) return f->second;
        }
        raiseError("runtime", "undefined variable: " + name);
    }

    // the value bound to name in the innermost scope that has it, or null. one lookup, no copy
    const Value* find(const std::string& name) const {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            auto f = (*it)->find(name);
            if (f != (*it)->end()) return &f->second;
        }
        return nullptr;
    }

    // bumped whenever a scope is pushed, popped, or gains or loses a name, which is exactly when a pointer
    // from findForCache() could stop being the right one. the vm keeps such pointers between instructions and
    // checks this number before using one. the number is unique across interpreters
    uint64_t scopeEpoch() const { return scopeEpoch_; }
    void scopesChanged() {
        static std::atomic<uint64_t> next{1};
        scopeEpoch_ = next.fetch_add(1, std::memory_order_relaxed);
    }

    // like findMut(), and also says whether the scope holding the name has a type constraint on it
    // (then assigning has to go through set(), which checks it)
    Value* findForCache(const std::string& name, bool& typed, TypeSet* type = nullptr) {
        for (size_t idx = scopes_.size(); idx-- > 0; ) {
            auto it = scopes_[idx]->find(name);
            if (it == scopes_[idx]->end()) continue;
            typed = false;
            if (!varTypes_[idx].empty()) {
                auto ty = varTypes_[idx].find(name);
                typed = ty != varTypes_[idx].end() && !ty->second.isAny();
                if (typed && type) *type = ty->second;
            }
            return &it->second;
        }
        return nullptr;
    }

    // the type recorded for the innermost variable of this name (declared, or taken from its first value), or null if
    // it has none. the static type checker reads it so a name made by an earlier file or REPL line is known
    // what an earlier file or REPL line found the elements of an array or map global to be (`xs = [1, 2]` is arr<int>), kept by the static checker
    const TypeElem* staticElem(const std::string& name) const {
        auto it = staticElems_.find(name);
        return it == staticElems_.end() ? nullptr : &it->second;
    }
    void setStaticElem(const std::string& name, TypeElem el) { if (el) staticElems_[name] = std::move(el); else staticElems_.erase(name); }
    const TypeSet* recordedType(const std::string& name) const {
        for (size_t idx = scopes_.size(); idx-- > 0; ) {
            if (!scopes_[idx]->count(name)) continue;
            auto it = varTypes_[idx].find(name);
            return it == varTypes_[idx].end() || it->second.isAny() ? nullptr : &it->second;
        }
        return nullptr;
    }

    Value* findMut(const std::string& name) {
        return const_cast<Value*>(find(name));
    }

    bool has(const std::string& name) const {
        for (auto& sc : scopes_) if (sc->count(name)) return true;
        return false;
    }

    // records a type constraint checked on every later assignment to k within this scope's lifetime
    // used by local <type> name and local auto name declarations
    void define(const std::string& k, Value v, TypeSet ts = TypeSet::Any()) {
        scopesChanged();
        (*scopes_.back())[k] = std::move(v);
        if (!ts.isAny()) varTypes_.back()[k] = ts;
        else              varTypes_.back().erase(k);
    }

    // resolveDunder()'s cache, one entry per (tag, op). mutable: a lookup is a const operation
    struct DunderCacheEntry { std::string op; std::string name; };
    DunderCacheEntry& dunderEntry(const void* tagId, const std::string& op) const {
        auto& list = dunderCache_[tagId];
        for (auto& e : list) if (e.op == op) return e;
        list.emplace_back(); list.back().op = op;
        return list.back();
    }

    // the members of the overload group `base` that are visible now (every scope), by number: {number, name}
    std::vector<std::pair<int, std::string>> overloadGroup(const std::string& base) const {
        std::vector<std::pair<int, std::string>> out;
        std::unordered_set<std::string> seen;
        for (size_t i = scopes_.size(); i-- > 0; )
            for (const auto& kv : *scopes_[i]) {
                std::string b; int n = 0;
                if (!overloadName(kv.first, &b, &n) || b != base || !seen.insert(kv.first).second) continue;
                if (kv.second.isCallable()) out.emplace_back(n, kv.first);
            }
        std::sort(out.begin(), out.end());
        return out;
    }
    // the same, kept until a name is added or a scope comes or goes (scopeEpoch), for the dispatcher that runs on every call
    const std::vector<std::pair<int, std::string>>& overloadGroupCached(const std::string& base) const {
        auto& e = overloadCache_[base];
        if (e.first != scopeEpoch_ || e.first == 0) { e.second = overloadGroup(base); e.first = scopeEpoch_; }
        return e.second;
    }
    // every name that has at least one member visible now
    std::vector<std::string> overloadBases() const {
        std::set<std::string> bases;
        for (const auto& sp : scopes_)
            for (const auto& kv : *sp) {
                std::string b;
                if (overloadName(kv.first, &b) && kv.second.isCallable()) bases.insert(b);
            }
        return std::vector<std::string>(bases.begin(), bases.end());
    }
    // the number the next member of the group `base` gets: one above every number the group has had, so a number is never reused even
    // after a member was removed. claimOverload() records it
    int nextOverload(const std::string& base) const {
        int top = 0;
        auto hw = overloadHighWater_.find(base);
        if (hw != overloadHighWater_.end()) top = hw->second;
        for (const auto& m : overloadGroup(base)) top = std::max(top, m.first);
        return top + 1;
    }
    void claimOverload(const std::string& base, int num) { int& hw = overloadHighWater_[base]; hw = std::max(hw, num); }
    // makes the bare name `base` the runtime dispatcher of its group. the VM sets the function that does it when the first script is
    // compiled, so a plugin can ask without knowing about the VM
    std::function<void(const std::string&)> overloadInstaller;
    void installOverload(const std::string& base) {
        if (!overloadInstaller) raiseError("overload", "no script has been compiled in this interpreter, so there is no dispatcher to install");
        overloadInstaller(base);
    }
    // a plugin that was loaded after the last compile (an import inside a module, which is compiled before its own imports run) may
    // have brought a group no script was compiled against: give its bare name a dispatcher now, as compileSource does
    void installPendingOverloads() {
        if (!overloadInstaller) return;
        for (const std::string& base : overloadBases())
            if (!isOverloadDispatcher(base) && !hasGlobal(base)) overloadInstaller(base);
    }
    // the bare names that run the runtime dispatcher (see VmRunner::installOverloads), not a function of that name
    bool isOverloadDispatcher(const std::string& name) const { return overloadDispatchers_.count(name) > 0; }
    void markOverloadDispatcher(const std::string& name) { overloadDispatchers_.insert(name); }

    // a native that takes a number twice is a plugin bug: two plugins (or two calls) both claimed `abs__2`
    void refuseSecondOverload(const std::string& name) const {
        if (!overloadName(name)) return;
        auto it = scopes_.front()->find(name);
        if (it != scopes_.front()->end() && it->second.isCallable() && it->second.asCallable().isNative())
            raiseError("overload", "'" + name + "' is already bound as an overload by another plugin, numbers are never reused");
    }

    // bind nativefn with no sigcheck
    void bind(const std::string& name, NativeFn fn) {
        refuseSecondOverload(name);
        scopesChanged();
        (*scopes_.front())[name] = Value::makeNative(name, std::move(fn), {});
    }

    // bindSig() for a native that reads its arguments through an ArgSpan, which is the values on the VM's stack with no copy and no
    // vector. it must be a leaf: no calling back into script code (invoke, sort_do, ...), because that can move the stack the view points
    // into. a caller that has a vector (embr::invoke) still works, it goes through `native`
    void bindSpan(const std::string& name, std::vector<Param> sig, std::function<Value(const ArgSpan&)> fn) {
        refuseSecondOverload(name);
        scopesChanged();
        auto sp = fn;
        Value v = Value::makeNative(name, [sp](const std::vector<Value>& a) -> Value { return sp(ArgSpan{a.data(), a.size()}); }, std::move(sig));
        const_cast<Value::Callable&>(v.asCallable()).span = std::move(fn);
        (*scopes_.front())[name] = std::move(v);
    }

    // binds a C++ function whose parameters are plain types: double (a number), int64_t (an int), const std::string& (a string) or const Value& (anything).
    // the parameter list, the checks and the result type come from the signature, so there is nothing to keep in step. `names` are the parameter names
    // in error messages. the function must be a leaf, like bindSpan's
    template <class R, class... A>
    void bindTyped(const std::string& name, std::initializer_list<const char*> names, R (*fn)(A...)) {
        std::vector<Param> sig;
        auto it = names.begin();
        size_t n = 0;
        auto add = [&](TypeSet t) {
            sig.push_back(Param::req(it != names.end() ? std::string(*it++) : "arg" + std::to_string(n + 1), t));
            ++n;
        };
        (add(typed::Arg<typed::Plain<A>>::type()), ...);
        bindSpan(name, std::move(sig), [fn](const ArgSpan& a) -> Value { return typed::call(fn, a, std::index_sequence_for<A...>{}); });
        Value::Callable& c = const_cast<Value::Callable&>(scopes_.front()->find(name)->second.asCallable());
        c.returns = typed::Ret<typed::Plain<R>>::type();
        if constexpr (sizeof...(A) <= 4) {   // more than four arguments go the ordinary way
            if (!typed::typedCallsEnabled()) return;
            c.typed.thunk = &typed::thunk<R, A...>;
            c.typed.fn = reinterpret_cast<void*>(fn);
            c.typed.n = (uint8_t)sizeof...(A);
            size_t k = 0;
            ((c.typed.mask[k++] = typed::Arg<typed::Plain<A>>::type().mask), ...);
        }
    }

    // the same for a function that needs the interpreter: it is the first parameter of the C++ function and not one of the script's
    template <class R, class... A>
    void bindTyped(const std::string& name, std::initializer_list<const char*> names, R (*fn)(Interpreter&, A...)) {
        std::vector<Param> sig;
        auto it = names.begin();
        size_t n = 0;
        auto add = [&](TypeSet t) {
            sig.push_back(Param::req(it != names.end() ? std::string(*it++) : "arg" + std::to_string(n + 1), t));
            ++n;
        };
        (add(typed::Arg<typed::Plain<A>>::type()), ...);
        bindSpan(name, std::move(sig), [this, fn](const ArgSpan& a) -> Value { return typed::callI(fn, *this, a, std::index_sequence_for<A...>{}); });
        Value::Callable& c = const_cast<Value::Callable&>(scopes_.front()->find(name)->second.asCallable());
        c.returns = typed::Ret<typed::Plain<R>>::type();
        if constexpr (sizeof...(A) <= 4) {
            if (!typed::typedCallsEnabled()) return;
            c.typed.thunk = &typed::thunkI<R, A...>;
            c.typed.fn = reinterpret_cast<void*>(fn);
            c.typed.ctx = this;
            c.typed.n = (uint8_t)sizeof...(A);
            size_t k = 0;
            ((c.typed.mask[k++] = typed::Arg<typed::Plain<A>>::type().mask), ...);
        }
    }

    // says what natives already bound always return, one entry per name (see Value::Callable::returns). a name that is not a
    // native here is ignored, so a plugin can list its functions without caring which ones were bound
    void setResultTypes(std::initializer_list<std::pair<const char*, TypeSet>> list) {
        for (const auto& [name, type] : list) {
            auto it = scopes_.front()->find(name);
            if (it == scopes_.front()->end() || !it->second.isCallable()) continue;
            Value::Callable& c = const_cast<Value::Callable&>(it->second.asCallable());
            if (c.isNative()) c.returns = type;
        }
    }

    // marks natives as pure (see Value::Callable::pure): only for a native whose result depends on nothing but its arguments
    void setPure(std::initializer_list<const char*> names) {
        for (const char* name : names) {
            auto it = scopes_.front()->find(name);
            if (it == scopes_.front()->end() || !it->second.isCallable()) continue;
            Value::Callable& c = const_cast<Value::Callable&>(it->second.asCallable());
            if (c.isNative()) c.pure = true;
        }
    }

    // the same for what a native returning an array or a map holds: {"str_split", TS::Str} says it returns arr<str> (or map<str>)
    void setResultElems(std::initializer_list<std::pair<const char*, TypeSet>> list) {
        for (const auto& [name, type] : list) {
            auto it = scopes_.front()->find(name);
            if (it == scopes_.front()->end() || !it->second.isCallable()) continue;
            Value::Callable& c = const_cast<Value::Callable&>(it->second.asCallable());
            if (c.isNative()) { auto a = std::make_shared<TypeAnn>(); a->t = type; c.returnsElem = a; }
        }
    }
    // says which argument a native hands back, changed (`push(list, x)` returns list with x added): the checker gives the call the type of that argument
    void setResultLike(std::initializer_list<std::pair<const char*, int>> list) {
        for (const auto& [name, arg] : list) {
            auto it = scopes_.front()->find(name);
            if (it == scopes_.front()->end() || !it->second.isCallable()) continue;
            Value::Callable& c = const_cast<Value::Callable&>(it->second.asCallable());
            if (c.isNative()) c.returnsLike = arg;
        }
    }

    // bind nativefn with sig
    void bindSig(const std::string& name, std::vector<Param> sig, NativeFn fn) {
        refuseSecondOverload(name);
        scopesChanged();
        (*scopes_.front())[name] = Value::makeNative(name, std::move(fn), std::move(sig));
    }

    // defines name in the native/global scope (scopes_[0]), the same place bind()/bindSig() write to, but for any
    // Value. unlike define(), which writes wherever the caller is running, this is visible from every module
    // exists for metaprogramming (plugins/embrmeta): a script installs a binding under a name it only knows at
    // runtime, like ffi_cdef naming a function after a C declaration. plain `name = value` can't do that
    void defineGlobal(const std::string& name, Value v) {
        scopesChanged();
        (*scopes_.front())[name] = std::move(v);
    }

    // removes name from the native/global scope only (mirrors
    // defineGlobal's scope, not set()/get()'s full scope-chain search).
    // returns true if something was actually erased.
    bool undefineGlobal(const std::string& name) {
        scopesChanged();
        return scopes_.front()->erase(name) > 0;
    }

    // the names bound in the native/global scope (where plugins bind), not the current module's scope
    std::vector<std::string> nativeNames() const {
        std::vector<std::string> out;
        out.reserve(scopes_.front()->size());
        for (const auto& kv : *scopes_.front()) out.push_back(kv.first);
        return out;
    }
    bool hasGlobal(const std::string& name) const {
        return scopes_.front()->count(name) > 0;
    }

    // reads a binding from the native/global scope specifically, unlike
    // get() (which walks the whole scope chain and returns whatever shadows
    // it first), this always answers "what did defineGlobal() last set this
    // to", even if some inner/module scope happens to define the same name
    const Value& getGlobal(const std::string& name) const {
        return scopes_.front()->at(name);
    }

    // register scriptfn into innermost scope
    void defineScriptFn(ScriptFn fn) {
        std::string nm = fn.name;
        scopesChanged();
        (*scopes_.back())[nm] = Value::makeScript(std::move(fn));
    }

    void setSource(const std::string& src,
                   const std::string& filename = "<input>") {
        srcMap_.load(src); currentFile_ = filename;
    }
    void setSource(SourceMap m, const std::string& filename = "<input>") {
        srcMap_ = std::move(m); currentFile_ = filename;
    }
    const SourceMap&     sourceMap() const { return srcMap_; }
    const std::string& currentFile() const { return currentFile_; }

    // exposes C++ variable as getter/setter pair
    // reference must outlive Interpreter
    template<typename T>
    void bindVar(const std::string& name, T& ref) {
        // name() -> value
        bind(name, [&ref](const std::vector<Value>&) -> Value {
            return toValue(ref);
        });
        // set_name(val)
        bindSig("set_" + name, {Param::req("val", typeConstraintFor<T>())},
                [&ref](const std::vector<Value>& args) -> Value {
            fromValue(args[0], ref);
            return Value(0.0);
        });
    }

    // returns the current module's global scope (the assignment floor)
    // for the top-level script this is scopes_[0] (the native layer)
    // for an imported module it is the module's own export scope
    const Scope& globals() const { return *scopes_[moduleBase_]; }
    // plugin fns, built-ins always scopes_[0]
    const Scope& nativeGlobals() const { return *scopes_[0]; }

    const std::vector<std::shared_ptr<Scope>>& allScopes() const { return scopes_; }

    // --- support for the opt-in gc plugin (plugins/gc/gc.cpp) ----------
    //
    // closures share live Scope objects, and reference counting alone can't free a cycle (a closure stored in a
    // variable inside its own captured scope). the gc plugin finds such cycles by mark-and-sweep over every scope:
    // whatever isn't reachable from the active call stack gets its contents cleared
    //
    // gcRegisterScope() tracks (weakly) every Scope a backend creates. call it only where a new Scope is made,
    // not when an existing one is pushed back on a stack
    void gcRegisterScope(const std::shared_ptr<Scope>& s) { gcScopes_.push_back(s); }

    // a plugin function only ever gets an Interpreter&, never the VmRunner, so each VmRunner registers itself here
    // once, and the gc asks it what its running frames hold (their module scopes, locals and variable cells). nothing is
    // pushed or popped per call. a runner takes itself out again when it is destroyed
    struct GcFrameSource {
        virtual void gcCollectRoots(std::vector<std::shared_ptr<Scope>>& scopes,
                                    std::vector<const std::vector<Value>*>& slots,
                                    std::vector<const std::vector<std::shared_ptr<Cell>>*>& cells) const = 0;
    protected:
        ~GcFrameSource() = default;
    };
    // the list is shared so a runner that outlives the Interpreter (a static one, see vm.h) can still take itself out without
    // touching a dead Interpreter: it keeps a weak handle to the list
    using GcFrameSourceList = std::vector<const GcFrameSource*>;
    std::weak_ptr<GcFrameSourceList> gcAddFrameSource(const GcFrameSource* src) {
        gcFrameSources_->push_back(src);
        return gcFrameSources_;
    }
    static void gcRemoveFrameSource(const std::weak_ptr<GcFrameSourceList>& handle, const GcFrameSource* src) {
        if (auto list = handle.lock())
            for (size_t i = list->size(); i-- > 0; )
                if ((*list)[i] == src) { list->erase(list->begin() + i); break; }
    }

    // makes a variable cell and remembers it (weakly), so the gc can clear a cell that is only held by a cycle and the
    // destructor can empty every cell. the list is pruned as it grows
    std::shared_ptr<Cell> newCell() {
        if (gcCells_.size() >= gcCellsPruneAt_) {
            size_t w = 0;
            for (auto& c : gcCells_) if (!c.expired()) gcCells_[w++] = std::move(c);
            gcCells_.resize(w);
            gcCellsPruneAt_ = std::max<size_t>(4096, gcCells_.size() * 2);
        }
        auto c = std::make_shared<Cell>();
        gcCells_.push_back(c);
        return c;
    }
    // every cell still alive, like gcAllLiveScopes()
    std::vector<std::shared_ptr<Cell>> gcAllLiveCells() {
        std::vector<std::shared_ptr<Cell>> live;
        live.reserve(gcCells_.size());
        size_t w = 0;
        for (size_t r = 0; r < gcCells_.size(); ++r)
            if (auto sp = gcCells_[r].lock()) { live.push_back(sp); gcCells_[w++] = std::move(gcCells_[r]); }
        gcCells_.resize(w);
        return live;
    }
    // scopes reachable right now from an active call stack on either
    // backend, the root set a mark phase starts from.
    std::vector<std::shared_ptr<Scope>> gcRoots() const {
        std::vector<std::shared_ptr<Scope>> roots = scopes_;
        std::vector<const std::vector<Value>*> slots;
        std::vector<const std::vector<std::shared_ptr<Cell>>*> cells;
        for (const GcFrameSource* src : *gcFrameSources_) src->gcCollectRoots(roots, slots, cells);
        return roots;
    }
    // the locals of every running VM frame as raw Values, and the variable cells they hold. a closure in one of them
    // reaches scopes the mark phase must not miss (gc.cpp walks each Value with markValue())
    std::vector<const std::vector<Value>*> gcValueRoots() const {
        std::vector<std::shared_ptr<Scope>> scopes;
        std::vector<const std::vector<Value>*> slots;
        std::vector<const std::vector<std::shared_ptr<Cell>>*> cells;
        for (const GcFrameSource* src : *gcFrameSources_) src->gcCollectRoots(scopes, slots, cells);
        return slots;
    }
    std::vector<const std::vector<std::shared_ptr<Cell>>*> gcCellRoots() const {
        std::vector<std::shared_ptr<Scope>> scopes;
        std::vector<const std::vector<Value>*> slots;
        std::vector<const std::vector<std::shared_ptr<Cell>>*> cells;
        for (const GcFrameSource* src : *gcFrameSources_) src->gcCollectRoots(scopes, slots, cells);
        return cells;
    }

    // a plugin container that can hold Values (a table) registers here so it gets emptied when the Interpreter is
    // destroyed. a container that holds itself (`table_set(t, "self", t)`) is a cycle no scope owns and would leak
    // `owner` is held weakly, and `clear` runs before the plugin is unloaded
    // teardown only: gc_collect() doesn't use this, because a value that lives only on the VM operand stack
    // mid-expression isn't a known root and could be emptied while still in use
    void addClearable(std::weak_ptr<void> owner, std::function<void()> clear) {
        if (clearables_.size() >= clearablePruneAt_) {      // drop entries whose owner is already gone
            size_t w = 0;
            for (auto& c : clearables_) if (!c.owner.expired()) clearables_[w++] = std::move(c);
            clearables_.resize(w);
            clearablePruneAt_ = std::max<size_t>(4096, clearables_.size() * 2);
        }
        clearables_.push_back({std::move(owner), std::move(clear)});
    }

    // run once when the Interpreter is destroyed, before its scopes are released and before any plugin
    // is unloaded. for things that hold reference cycles the scopes can't reach (the VM's compiled chunks
    // share a sibling list that contains themselves). a hook must not rely on the Interpreter's scopes.
    void addTeardownHook(std::function<void()> h) { teardownHooks_.push_back(std::move(h)); }

    // Values a plugin keeps in its own state that must stay alive even if no script variable refers to a handle
    // for them. example: ffi callbacks. C code holds the function pointer until ffi_callback_free(), so the script
    // closure behind it is a root even after the script drops its handle. gc_collect() calls every provider and
    // marks what it visits. (values only reachable through a handle should use TypedPtr::tracer instead, see core/value.h)
    // a provider must not keep plugin state alive on its own: capture a weak_ptr
    using GcRootProvider = std::function<void(const TraceVisitor&)>;
    void addGcRootProvider(GcRootProvider p) { gcRootProviders_.push_back(std::move(p)); }
    const std::vector<GcRootProvider>& gcRootProviders() const { return gcRootProviders_; }

    // every native plugin imported into this interpreter: what it said about itself (embr_plugin.h, plugininfo.h), and for a
    // plugin written in C the table its functions came from. a plugin with no description (built before there was one) has
    // `declared` false. cRoots holds the values C code kept alive with embrp_root, the gc reads them
    struct PluginRecord {
        struct Fn { std::string name, doc; };
        std::string name, version, description, path, buildId, abiTag;
        enum class Kind { C, Cxx } kind = Kind::Cxx;
        bool        declared = false;
        uint32_t    abi = 0;
        uint64_t    caps = 0, optionalCaps = 0;
        std::vector<Fn> fns;               // what the plugin documents (a C plugin: everything it binds)
        std::vector<std::string> bound;    // what a C++ plugin bound while it loaded (names that were not there before)
    };
    std::vector<PluginRecord> plugins;
    std::set<Value*>           cRoots;

    // every package (core/package.h) imported into this interpreter, by name: where it was unpacked, and the sha256 of each entry
    // so package_path() can check a file before it hands out its path
    struct PackageRecord {
        std::string name, version, file, dir, id;
        uint64_t    caps = 0;
        std::map<std::string, std::string> sha;     // label -> hex sha256
        std::set<std::string>              checked; // labels whose file was hashed and found right
    };
    std::map<std::string, PackageRecord> packages;

    // every scope still alive anywhere (on a stack, captured by a live
    // closure, or orphaned in an unreachable cycle), prunes dead weak_ptrs
    // from the registry as a side effect, so the registry doesn't grow
    // unboundedly across a long-lived interpreter's lifetime.
    std::vector<std::shared_ptr<Scope>> gcAllLiveScopes() {
        std::vector<std::shared_ptr<Scope>> live;
        live.reserve(gcScopes_.size());
        size_t w = 0;
        for (size_t r = 0; r < gcScopes_.size(); ++r) {
            if (auto sp = gcScopes_[r].lock()) {
                live.push_back(sp);
                gcScopes_[w++] = std::move(gcScopes_[r]);
            }
        }
        gcScopes_.resize(w);
        return live;
    }

    std::string               scriptDir;
    // how long a script may run, for embedders and `embr --max-steps= / --timeout=`. a step is one turn of a loop or
    // one call of a script function, which is enough to stop any script that never ends. 0 means no limit. the
    // limit ends the run with an error that try / catch cannot catch. a native that blocks (sleep, http) is not
    // interrupted. worker interpreters (async, parallel) get a copy
    struct Budget {
        uint64_t maxSteps = 0;
        double   seconds = 0;
        std::chrono::steady_clock::time_point deadline{};
        bool on() const { return maxSteps != 0 || seconds > 0; }
    };
    Budget                    budget;
    uint64_t                  stepsUsed = 0;
    void setBudget(uint64_t maxSteps, double seconds) {
        budget.maxSteps = maxSteps;
        budget.seconds = seconds;
        if (seconds > 0)
            budget.deadline = std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(seconds));
        stepsUsed = 0;
        budgetOn = budget.on();
    }
    void setBudget(const Budget& b) { budget = b; stepsUsed = 0; budgetOn = b.on(); }   // keeps the other one's deadline
    bool budgetOn = false;   // the VM tests this one flag on every loop turn and call
    // where compiled modules are kept as .vmbr files (see backends/vm_bytecode.h). empty: never write one
    std::string               cacheDir;
    // the files of the plugins this interpreter has registered, so importing one twice (a script and a module, or the
    // checker reading signatures before the script runs) registers it once
    std::unordered_set<std::string> loadedPlugins_;
    PluginPolicy              pluginPolicy;       // see PluginPolicy
    // arguments after a bare `--` on the embr command line (embr script.embr -- a b c);
    // read by the os plugin's os_args()
    std::vector<std::string>  scriptArgs;
    std::vector<PluginHandle> pluginHandles_;

    // push a new module scope
    // sets moduleBase_ to the new scope
    size_t pushModuleScope() {
        scopesChanged();
        auto s = std::make_shared<Scope>();
        scopes_.push_back(s);
        gcRegisterScope(s);
        varTypes_.emplace_back();
        size_t idx = scopes_.size() - 1;
        prevModuleBases_.push_back(moduleBase_);
        moduleBase_ = idx;
        moduleLocalSets_.emplace_back();
        return idx;
    }

    struct ModuleScopeResult {
        std::unordered_map<std::string, Value> scope;
        std::set<std::string>                  locals;  // names declared with 'local'
    };

    // pop the current module scope and restore moduleBase_ to the parent
    // returns a copy of the scope's contents (and its local-name set) so the caller can decide what to export
    // it is a copy, not a move, because an exported closure may still hold the real scope object and must keep seeing it
    ModuleScopeResult popModuleScope() {
        ModuleScopeResult res;
        scopesChanged();
        res.scope  = *scopes_.back();
        res.locals = std::move(moduleLocalSets_.back());
        scopes_.pop_back();
        varTypes_.pop_back();
        moduleBase_ = prevModuleBases_.back();
        prevModuleBases_.pop_back();
        moduleLocalSets_.pop_back();
        return res;
    }

    // mark a name as module-local at the current module top scope
    // called by exec(LocalStmt) when exactly at the module floor
    void markModuleLocal(const std::string& name) {
        if (!moduleLocalSets_.empty())
            moduleLocalSets_.back().insert(name);
    }

    // true when executing statements directly in the module top scope
    bool atModuleTopScope() const {
        return scopes_.size() - 1 == moduleBase_;
    }

    // true while running inside an imported module (a top-level script never moves moduleBase_ off 0)
    // vm.h's snapshotLocals() uses it to decide whether a closure made inside a module also needs the module's
    // top scope in its capture
    bool insideImportedModule() const {
        return moduleBase_ != 0;
    }

    // a shared reference to the current module's top scope (or the script's, at moduleBase_ == 0). vm.h's
    // snapshotLocals() adds it to a closure's CaptureFrame, since the VM can't reach scopes_ directly
    std::shared_ptr<Scope> currentModuleScope() const {
        return scopes_[moduleBase_];
    }

    // canonical set of already-loaded module paths (deduplication)
    std::set<std::string> loadedModules_;

    // guards one callable invocation against unbounded native C++ stack recursion from a deeply/infinitely recursive script
    // the backend constructs one of these per call so a script that recurses too deep raises a clean, catchable EmbrError
    // shared on Interpreter cuz a call chain can hop between backends and the depth must be tracked across that
    struct CallDepthGuard {
        Interpreter& i;
        explicit CallDepthGuard(Interpreter& interp, const SourceRange& site = {}) : i(interp) {
            i.enterCall(site);
        }
        CallDepthGuard(const CallDepthGuard&) = delete;
        ~CallDepthGuard() { i.exitCall(); }
    };

    // non-RAII form of the same guard for vm whose call frame outlives the C++ function that pushed it
    // pair every enterCall() with exactly one later exitCall() when that frame is actually popped (see vm.h's pushFrame/doReturn)
    [[gnu::always_inline]] void enterCall(const SourceRange& site = {}) {
        if (++callDepth_ > maxCallDepth_) [[unlikely]] callTooDeep(site);
    }
    [[noreturn, gnu::noinline]] void callTooDeep(const SourceRange& site) {
        --callDepth_;
        raiseError("runtime", "stack overflow: max call depth (" +
                   std::to_string(maxCallDepth_) + ") exceeded", site, sourceMap());
    }
    void exitCall() { --callDepth_; }

    void   setMaxCallDepth(size_t n) { maxCallDepth_ = n; }
    size_t maxCallDepth() const      { return maxCallDepth_; }

private:
    // each layer is shared_ptr-managed so a closure's CaptureFrame can hold
    // a reference to the same live object rather than a copy of its
    // contents, see core/value.h's CaptureFrame doc comment.
    std::vector<std::shared_ptr<Scope>> scopes_;
    // parallel to scopes_
    // per-scope type constraints recorded by typed/auto local declarations, consulted by set()
    std::vector<std::unordered_map<std::string, TypeSet>> varTypes_;
    uint64_t                                              scopeEpoch_ = 0;
    mutable std::unordered_map<std::string, std::pair<uint64_t, std::vector<std::pair<int, std::string>>>> overloadCache_;
    std::unordered_map<std::string, TypeElem>             staticElems_;

    // gc plugin support, see gcRegisterScope() and GcFrameSource above
    std::vector<std::weak_ptr<Scope>>                              gcScopes_;
    std::unordered_set<std::string>                                overloadDispatchers_;
    std::unordered_map<std::string, int>                           overloadHighWater_;
    mutable std::unordered_map<const void*, std::deque<DunderCacheEntry>> dunderCache_;   // a deque: an entry keeps its address as the list grows
    std::shared_ptr<GcFrameSourceList>                             gcFrameSources_ = std::make_shared<GcFrameSourceList>();
    std::vector<std::weak_ptr<Cell>>                 gcCells_;
    size_t                                           gcCellsPruneAt_ = 4096;
    std::vector<GcRootProvider>                      gcRootProviders_;
    std::vector<std::function<void()>>               teardownHooks_;
    struct Clearable { std::weak_ptr<void> owner; std::function<void()> clear; };
    std::vector<Clearable>                           clearables_;
    size_t                                           clearablePruneAt_ = 4096;

    size_t                             moduleBase_ = 0;
    std::vector<size_t>                prevModuleBases_;
    std::vector<std::set<std::string>> moduleLocalSets_;
    size_t                             callDepth_    = 0;
    size_t                             maxCallDepth_ = 1000;

    SourceMap   srcMap_;
    std::string currentFile_ = "<input>";

    // c++ type to Value
    static Value toValue(double v)             { return Value(v); }
    static Value toValue(float v)              { return Value((double)v); }
    static Value toValue(int v)                { return Value((double)v); }
    static Value toValue(int64_t v)            { return Value(v); }
    static Value toValue(bool v)               { return Value(v); }
    static Value toValue(const std::string& v) { return Value(v); }
    static Value toValue(void* p)              { return Value::makePointer(p); }

    // Value to c++ variable
    static void fromValue(const Value& v, double& out)      { out = v.asNumber(); }
    static void fromValue(const Value& v, float& out)       { out = (float)v.asNumber(); }
    static void fromValue(const Value& v, int& out)         { out = (int)v.asNumber(); }
    static void fromValue(const Value& v, int64_t& out)     { out = v.asInt(); }
    static void fromValue(const Value& v, bool& out)        { out = v.truthy(); }
    static void fromValue(const Value& v, std::string& out) { out = v.asString(); }
    static void fromValue(const Value& v, void*& out)       { out = v.asPointer().ptr; }

    // return typeset for T
    template<typename T> static constexpr TypeSet typeConstraintFor() { return TypeSet::Any(); }
};

// typeConstraintFor<T> specialisations
// double/float/int/bool all accept either numeric subtype (Number or Int)
// asNumber()/asInt() widen on their own, so bindVar()'s auto-marshaling shouldn't reject a literal that
// defaulted to the wrong numeric tag
template<> inline constexpr TypeSet Interpreter::typeConstraintFor<double>()      { return TS::Num; }
template<> inline constexpr TypeSet Interpreter::typeConstraintFor<float>()       { return TS::Num; }
template<> inline constexpr TypeSet Interpreter::typeConstraintFor<int>()         { return TS::Num; }
template<> inline constexpr TypeSet Interpreter::typeConstraintFor<int64_t>()     { return TS::Num; }
template<> inline constexpr TypeSet Interpreter::typeConstraintFor<bool>()        { return TS::Num; }
template<> inline constexpr TypeSet Interpreter::typeConstraintFor<std::string>() { return TypeSet(TypeTag::String); }
template<> inline constexpr TypeSet Interpreter::typeConstraintFor<void*>()       { return TypeSet(TypeTag::Pointer);}

// "dunder" dispatch: a plugin-tagged pointer can opt into being called, used with + - * / % > < >= <=, or act as a
// map's prototype (see lookupMapChain() below). one convention, three uses: look up a specially named global function
//
// the name is __<tag>_<op>. to make it always a valid identifier, every '.' in the tag becomes '_' and every real
// '_' is doubled to '__' ("table.handle" -> __table_handle_call). so a script can define a dunder for its own tag
// with a plain `fn __mytag_call(...) ... end`. a plugin can register a dotted one straight from C++ (bind() takes any string)
//
// doubling '_' keeps it collision-free: just replacing '.' with '_' would give "a.b_c" and "a_b.c" the same name.
// the encoding only has to be injective (never decoded back)
inline std::string dunderName(const std::string& tag, const std::string& op) {
    std::string s;
    s.reserve(tag.size());
    for (char c : tag) {
        if      (c == '.') s += '_';
        else if (c == '_') s += "__";
        else                s += c;
    }
    return "__" + s + "_" + op;
}

// resolves __<tag>_<op> as a global callable for v (a typed pointer with a
// non-empty tag). returns false (outFn untouched) if v isn't a tagged
// pointer, no such global exists, or it exists but isn't callable, callers
// treat false as "no override, fall back to the built-in behavior."
inline bool resolveDunder(const Interpreter& interp, const Value& v, const std::string& op, Value& outFn) {
    if (!v.isPointer()) return false;
    const Tag& tag = v.asPointer().type;
    if (tag.empty()) return false;
    // the name `__tag_op` is built once per (tag, op) and kept. the global is still looked up each time, so redefining it takes effect
    Interpreter::DunderCacheEntry& e = interp.dunderEntry(tag.id(), op);
    if (e.name.empty()) e.name = dunderName(tag.str(), op);
    if (!interp.hasGlobal(e.name)) return false;
    Value fn = interp.getGlobal(e.name);
    if (!fn.isCallable()) return false;
    outFn = std::move(fn);
    return true;
}

// maps a binary operator to the dunder op name resolveDunder() uses, or nullptr if it can't be overridden
// shared by every backend (vm.h Op::ADD..Op::GTE)
// "==" and "!=" are left out on purpose: they always work structurally through Value::operator==, and letting
// a plugin override equality could break things that rely on it (map dedup, shared_table keys, ...)
inline const char* arithDunderOp(const std::string& op) {
    if (op == "+")  return "add";
    if (op == "-")  return "sub";
    if (op == "*")  return "mul";
    if (op == "/")  return "div";
    if (op == "%")  return "mod";
    if (op == "&")  return "band";
    if (op == "|")  return "bor";
    if (op == "^")  return "bxor";
    if (op == "<<") return "shl";
    if (op == ">>") return "shr";
    if (op == ">")  return "gt";
    if (op == "<")  return "lt";
    if (op == ">=") return "gte";
    if (op == "<=") return "lte";
    return nullptr;
}

// map member lookup with prototype fallback: if key isn't in the map and the map has a "__proto__" key, the
// lookup continues there instead of failing
//   - a map prototype: same lookup again, recursively (multi-level chains work), depth-capped so a cycle fails cleanly
//   - a typed pointer prototype: looks up the global __<tag>_<key> (same dunder convention, with the key as the "op"),
//     so a map whose __proto__ is a "shape" pointer falls back to __shape_area for m["area"]
// returns false (out untouched) on a total miss
// only m[key] reads use this. writes always go to the map's own slots, and `for k in m` only sees its own keys
inline bool lookupMapChain(const Interpreter& interp, const Value& map, const std::string& key,
                           Value& out, int depth = 0) {
    if (depth > 32) return false;
    const auto& m = map.asMap();
    auto it = m.find(key);
    if (it != m.end()) { out = it->second; return true; }
    auto protoIt = m.find("__proto__");
    if (protoIt == m.end()) return false;
    const Value& proto = protoIt->second;
    if (proto.isMap())     return lookupMapChain(interp, proto, key, out, depth + 1);
    if (proto.isPointer()) return resolveDunder(interp, proto, key, out);
    return false;
}

} // namespace embr

#endif // EMBR_CORE_REGISTRY_H
