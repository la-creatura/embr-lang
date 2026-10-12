// embrmeta.cpp
// metaprogramming for embr: create, update and delete global variables and functions by a name computed at runtime
//
// usage
//   import "embrmeta"
//   meta_set("greet", fn(name) return "hi " + name end)
//   print(greet("world"))          # "hi world"
//   print(meta_has("greet"))       # 1
//   meta_undef("greet")
//   print(meta_has("greet"))       # 0
//   meta_overload("area", fn(r: int) return r * r end)      # makes area__1
//   meta_overload("area", fn(w: str) return len(w) end)      # makes area__2, area(5) and area("ab") now work
//
// why it's a plugin: `name = value` always targets a name written in the source, there is no syntax for "assign to
// the variable this string names". meta_set/meta_undef are that missing piece, on top of Interpreter::defineGlobal/
// undefineGlobal/hasGlobal (core/registry.h). "global" here is the same native scope bind() writes to, so a
// binding made this way acts like a real native function: visible from every module, not filtered by import
//
// meta_overload builds an overload group (`name__N`, see the wiki's type-system page) at runtime and does the numbering for you
//
// main use: modules/ffi.embr parses a C declaration at runtime and installs a callable under the C function's own
// name (like `InitWindow`), which the module's source can't spell as an identifier
//
// raw bytecode injection (VmRunner::inject) is deliberately not exposed here. it works on VM internals with no
// script-level meaning, and global-scope changes don't need it (they work the same on both backends)

#include <embr/embr.h>

using namespace embr;

static Param pStr(std::string n) { return Param::req(std::move(n), TS::Str); }
static Param pAny(std::string n) { return Param::req(std::move(n), TS::Any); }

EMBR_PLUGIN {

    // meta_set(name: str, value: any) -> 0
    // creates or overwrites a global binding named `name`. works the same for a variable or a callable
    // (a Value is a Value), so there is no separate "define a function" call
    interp->bindSpan("meta_set", {pStr("name"), pAny("value")},
    [interp](const ArgSpan& args) -> Value {
        interp->defineGlobal(args[0].asString(), args[1]);
        return Value(0.0);
    });

    // meta_get(name: str) -> any
    // reads a global binding by name: the one meta_set() or a plugin's bind() installed, even if a module scope
    // shadows that name right now (a plain variable reference would see the shadow). raises if it isn't defined,
    // so a typo fails loudly like a literal identifier would
    interp->bindSpan("meta_get", {pStr("name")},
    [interp](const ArgSpan& args) -> Value {
        const std::string& name = args[0].asString();
        if (!interp->hasGlobal(name))
            raiseError("meta_get", "undefined global: " + name);
        return interp->getGlobal(name);
    });

    // meta_has(name: str) -> num
    interp->bindSpan("meta_has", {pStr("name")},
    [interp](const ArgSpan& args) -> Value {
        return Value(interp->hasGlobal(args[0].asString()) ? (int64_t)1 : (int64_t)0);
    });

    // meta_undef(name: str) -> num
    // removes a global binding; returns 1 if something was actually
    // removed, 0 if it wasn't defined (a no-op, not an error, matches
    // fs_remove()'s "removing something already gone is fine" convention)
    interp->bindSpan("meta_undef", {pStr("name")},
    [interp](const ArgSpan& args) -> Value {
        return Value(interp->undefineGlobal(args[0].asString()) ? (int64_t)1 : (int64_t)0);
    });

    // meta_overload(base: str, fn: fn) -> str
    // adds `fn` to the overload group `base` under the next number: the first call makes `base__1`, the next `base__2`, and so on, never
    // reusing a number (not even one whose function was removed with meta_undef). returns the name it made. `base(x)` then picks a member
    // by the argument types, so give each function typed parameters (`fn(x: int)`) or the dispatcher can't tell them apart.
    // `base` can't be a plain function already, and can't end in a number itself
    interp->bindSpan("meta_overload", {pStr("base"), Param::req("fn", TS::Fn)},
    [interp](const ArgSpan& args) -> Value {
        const std::string& base = args[0].asString();
        std::string b;
        if (base.empty() || overloadName(base, &b))
            raiseError("meta_overload", "'" + base + "' is not a group name: it is empty or already ends in __<number>");
        if (interp->hasGlobal(base) && !interp->isOverloadDispatcher(base))
            raiseError("meta_overload", "'" + base + "' is already a plain name, so it can't also be an overload group");
        const int num = interp->nextOverload(base);
        const std::string name = base + "__" + std::to_string(num);
        interp->claimOverload(base, num);
        interp->defineGlobal(name, args[1]);
        interp->installOverload(base);
        return Value(name);
    });

    // meta_overloads(base: str) -> arr
    // the names of the members of the group `base`, in number order (an empty array if there are none)
    interp->bindSpan("meta_overloads", {pStr("base")},
    [interp](const ArgSpan& args) -> Value {
        Value::array_type out;
        for (const auto& m : interp->overloadGroup(args[0].asString())) out.push_back(Value(m.second));
        return Value(std::move(out));
    });

    // meta_plugins() -> arr of map
    // one summary per native plugin imported into this interpreter, in the order they were imported: name, version,
    // description, kind ("c" or "cxx"), abi, build_id, path, capabilities and optional_capabilities (arrays of names)
    interp->bindSpan("meta_plugins", {},
    [interp](const ArgSpan&) -> Value {
        Value::array_type out;
        for (const auto& r : interp->plugins) out.push_back(pluginValue(*interp, r, /*withFunctions=*/false));
        return Value(std::move(out));
    });

    // meta_plugin(name: str) -> map
    // the summary of one imported plugin, plus functions: an array of maps with name, signature, doc and group (the overload
    // group a numbered function belongs to, else ""). raises if the plugin has not been imported here
    interp->bindSpan("meta_plugin", {pStr("name")},
    [interp](const ArgSpan& args) -> Value {
        const std::string& name = args[0].asString();
        for (const auto& r : interp->plugins)
            if (r.name == name) return pluginValue(*interp, r, /*withFunctions=*/true);
        raiseError("meta_plugin", "plugin '" + name + "' has not been imported (import \"" + name + "\" first)");
    });

    // meta_modules() -> arr of map
    // one summary per script module imported into this interpreter (sorted by path): name, version, description, path,
    // declared (has a __module__), dynamic_imports, capabilities (declared and inferred from its imports),
    // declared_capabilities, inferred_capabilities, problems and imports. the module is read again from its file, not run
    interp->bindSpan("meta_modules", {},
    [interp](const ArgSpan&) -> Value {
        Value::array_type out;
        std::vector<std::string> paths(interp->loadedModules_.begin(), interp->loadedModules_.end());
        std::sort(paths.begin(), paths.end());
        for (const auto& p : paths) {
            std::set<std::string> visiting;
            ModuleInfo m = analyzeModuleFile(p, interp->pluginPolicy, visiting);
            if (m.parsed) out.push_back(moduleValue(m, /*withFunctions=*/false));
        }
        return Value(std::move(out));
    });

    // meta_module(name: str) -> map
    // the same for one imported module, found by its name (the `name` in its __module__, else its file name) or its path,
    // plus functions: an array of maps with name, signature and doc (the comment above the fn). raises if it was not imported
    interp->bindSpan("meta_module", {pStr("name")},
    [interp](const ArgSpan& args) -> Value {
        const std::string& want = args[0].asString();
        std::vector<std::string> paths(interp->loadedModules_.begin(), interp->loadedModules_.end());
        std::sort(paths.begin(), paths.end());
        for (const auto& p : paths) {
            std::set<std::string> visiting;
            ModuleInfo m = analyzeModuleFile(p, interp->pluginPolicy, visiting);
            if (m.parsed && (m.name == want || m.path == want || std::filesystem::path(p).stem() == want))
                return moduleValue(m, /*withFunctions=*/true);
        }
        raiseError("meta_module", "module '" + want + "' has not been imported (import \"" + want + ".embr\" first, or read the file with meta_module_file)");
    });

    // meta_module_file(path: str) -> map
    // the same for any script module file, imported or not, found by path. nothing in the file is run, and the plugins it
    // imports are looked for only in the places the host controls (never opened from next to the file), so it is safe on a
    // file you just downloaded. a plugin file is not offered: opening one runs its start-up code
    interp->bindSpan("meta_module_file", {pStr("path")},
    [](const ArgSpan& args) -> Value {
        const std::string& path = args[0].asString();
        PluginPolicy policy;
        policy.restricted = true;
        std::set<std::string> visiting;
        ModuleInfo m = analyzeModuleFile(path, policy, visiting);
        if (!m.parsed && m.problems.empty()) raiseError("meta_module_file", "'" + path + "' is not a script module that parses");
        if (m.noMetadata) raiseError("meta_module_file", "'" + path + "' is a compiled module written without a description");
        if (!m.parsed) raiseError("meta_module_file", "cannot read '" + path + "'");
        return moduleValue(m, /*withFunctions=*/true);
    });

    // what each always returns (see Value::Callable::returns)
    interp->setResultTypes({
        {"meta_modules", TS::Arr}, {"meta_module", TS::Map}, {"meta_module_file", TS::Map},
        {"meta_set", TS::Float}, {"meta_has", TS::Int}, {"meta_undef", TS::Int}, {"meta_overload", TS::Str}, {"meta_overloads", TS::Arr},
        {"meta_plugins", TS::Arr}, {"meta_plugin", TS::Map},
    });
}

// what this plugin says about itself. embr reads it before it runs embr_register (see core/plugininfo.h)
EMBR_PLUGIN_INFO("embrmeta", "1.0.0", "programmatic global create/update/delete", EMBR_CAP_NONE, EMBR_CAP_NONE,
    {"meta_get", "reads a global by name, ignoring any local-scope shadow. raises \"meta_get: undefined global: <name>\" if undefined"},
    {"meta_has", "1 if a global with this name exists, else 0"},
    {"meta_overload", "adds fn to the overload group base under the next free number (base__1, base__2, ...) and returns the name it made. give each function typed parameters, that is how the group tells them apart"},
    {"meta_plugin", "the description of one imported plugin: name, version, description, kind, abi, build_id, path, capabilities, optional_capabilities and functions (name, signature, doc, group). raises if it was not imported"},
    {"meta_plugins", "one summary map per native plugin imported so far, in import order"},
    {"meta_module", "the description of one imported script module, read from its file without running it: name, version, description, path, capabilities (declared and inferred), imports, problems and functions (name, signature, doc). raises if it was not imported"},
    {"meta_module_file", "the same for any script module file (.embr or compiled .vmbr) by path, imported or not. nothing is run and its plugins are looked up only in trusted places, so it is safe on a downloaded file"},
    {"meta_modules", "one summary map per script module imported so far, sorted by path, with its capabilities and problems"},
    {"meta_overloads", "the names of the members of the group, in number order"},
    {"meta_set", "creates or overwrites a global binding, works the same for a plain value or a callable"},
    {"meta_undef", "1 if something was removed, 0 if there was nothing to remove (not an error)"});
