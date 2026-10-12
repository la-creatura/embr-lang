#ifndef EMBR_CORE_INVOKE_H
#define EMBR_CORE_INVOKE_H

// backend-agnostic callable invocation and script running
//
// a plugin that needs to call back into script code should call embr::invoke(), not build a specific
// backend's runner. then the same plugin binary works with whatever backend the host has
// see EMBR_BACKENDS in the top-level CMakeLists.txt
//
// the choice is made per callable, not per active backend:
//   - a native function is just called
//   - a script function carries the compiled form its backend made (ScriptFn::compiledChunk), and that
//     backend runs it. a new runner is made for that one call and thrown away
//
// to add a backend (a JIT, say): put it in backends/<name>.h behind EMBR_WITH_<NAME>, list it in EMBR_BACKENDS,
// and add a case to each of the three places below marked "backend switch"

#include "value.h"
#include "registry.h"
#include "module.h"

#if !defined(EMBR_WITH_VM)
#error "embr: no execution backend compiled in. define EMBR_WITH_VM (or the flag of another backend)"
#endif

namespace embr {

// the backends this build has, default first. the CLI's --backend and the test harness pick from these
inline const std::vector<std::string>& backendNames() {
    static const std::vector<std::string> names = {
#ifdef EMBR_WITH_VM
        "vm",
#endif
    };
    return names;
}

// the implementations. only a host that makes a runner ever refers to these (through installBackend() below), so a plugin that calls
// invoke() / loadEmbrModule() / runSource() doesn't compile the backend into itself
inline Value invokeOnBackend(Interpreter& interp, const Value& callee,
                             const std::vector<Value>& args, const SourceRange& site) {
    // resolve a __<tag>_call dunder (resolveDunder() in core/registry.h) BEFORE picking a backend. the backend
    // choice depends on the function that is really going to run, and a pointer with a call dunder isn't a
    // ScriptFn itself
    Value target = callee;
    std::vector<Value> targetArgs = args;
    if (!target.isCallable()) {
        Value dfn;
        if (resolveDunder(interp, target, "call", dfn)) {
            targetArgs.insert(targetArgs.begin(), target);
            target = dfn;
        }
        // else: target is still not callable, the backend below raises its own "not callable" error
    }

    // backend switch 1 of 3: which backend runs this callable
    vm::VmRunner r(interp);
    return r.invoke(target, targetArgs, site);
}

inline void runSourceOnBackend(const std::string& src, Interpreter& interp, const std::string& filename) {
    // backend switch 2 of 3: the default backend for whole scripts
    vm::runSource(src, interp, filename);
}

// backend-agnostic .embr module loading, used by embrlib's load_module()
//
// unlike invoke() there is no compiled callable to inspect, so nothing says which backend should run the
// module. the `import` statement doesn't have this problem (each backend runs imports with itself, see
// execImportEmbrModule in vm.h), but load_module() is a plain native function that any backend may have called.
// so it uses the default backend
//
// mutateCallerScope=false (load_module()) returns the module's exports as a map instead of merging them
// into the caller's scope, see exportModuleScope() in module.h
inline Value loadEmbrModuleOnBackend(Interpreter& interp, const std::string& rawPath,
                                     const SourceRange& site, bool mutateCallerScope,
                                     bool forceReload)
{
    namespace fs = std::filesystem;

    const bool compiledFile = fs::path(rawPath).extension() == ".vmbr";   // a compiled module, see vm_bytecode.h
    auto cands = fileCandidates(rawPath, interp.scriptDir, compiledFile ? ".vmbr" : ".embr", "modules");
    fs::path resolved = findExistingCandidate(cands);
    if (resolved.empty())
        raiseError("load_module", "cannot find module \"" + rawPath + "\"\n  tried:" +
                   describeCandidates(cands), site, interp.sourceMap());

    const std::string canonStr = canonicalOrSelf(resolved);

    if (forceReload) interp.loadedModules_.erase(canonStr);
    if (interp.loadedModules_.count(canonStr))
        return mutateCallerScope ? Value(0.0) : Value(Value::map_type{});
    interp.loadedModules_.insert(canonStr);

    // a script module is read as text and compiled (or taken from the cache folder, Interpreter::cacheDir)
    // a compiled one is read as bytes
    auto src = compiledFile ? std::optional<std::string>(std::string()) : tryReadFile(resolved);
    auto bytes = compiledFile ? vm::readWholeFile(resolved.string()) : std::optional<std::string>(std::string());
    if (!src || !bytes) raiseError("load_module", "cannot open module file: " + resolved.string(),
                                   site, interp.sourceMap());

    // a wrong `__module__`, or a module that needs a capability the host denies, is refused before any of it runs
    {
        std::string why = compiledFile ? checkCompiledBeforeRun(interp, resolved, *bytes) : checkModuleBeforeRun(interp, resolved, *src);
        if (!why.empty()) {
            interp.loadedModules_.erase(canonStr);
            raiseError("load_module", why, site, interp.sourceMap());
        }
    }

    std::string savedDir  = interp.scriptDir;
    SourceMap   savedMap  = interp.sourceMap();
    std::string savedFile = interp.currentFile();
    interp.scriptDir = resolved.parent_path().string();

    interp.pushModuleScope();
    try {
        // backend switch 3 of 3: the default backend for modules loaded by load_module()
        vm::CompiledProgram compiled;
        if (compiledFile) {
            compiled = vm::deserializeProgram(*bytes, resolved.string(), nullptr);
            vm::registerChunkTeardown(interp, compiled);   // free the chunks' sibling cycle at teardown
            interp.setSource(SourceMap(), resolved.string());
        } else {
            compiled = vm::compileSourcePreferCached(*src, interp, resolved.string(), interp.cacheDir);
        }
        vm::VmRunner runner(interp);
        runner.run(compiled);
    } catch (...) {
        interp.popModuleScope();
        interp.scriptDir = savedDir;
        interp.setSource(savedMap, savedFile);
        throw;
    }

    auto res = interp.popModuleScope();
    interp.scriptDir = savedDir;
    interp.setSource(savedMap, savedFile);

    return exportModuleScope(interp, std::move(res), mutateCallerScope);
}

// backend switch: a new backend sets its own functions here
inline void installBackend(Interpreter& interp) {
    interp.backend.invoke     = invokeOnBackend;
    interp.backend.loadModule = loadEmbrModuleOnBackend;
    interp.backend.runSource  = runSourceOnBackend;
}

// what a plugin calls
inline Value invoke(Interpreter& interp, const Value& callee,
                    const std::vector<Value>& args, const SourceRange& site = {}) {
    if (!interp.backend.invoke) raiseError("runtime", "no script has run in this interpreter yet, so there is no execution backend to call into", site);
    return interp.backend.invoke(interp, callee, args, site);
}

// compile and run src on the default backend. an uncaught EmbrError stops the rest of src and is printed once
// here. embedders that want a specific backend call its own runSource (embr::vm::runSource)
inline void runSource(const std::string& src, Interpreter& interp,
                      const std::string& filename = "<input>") {
    if (!interp.backend.runSource) installBackend(interp);   // an embedder starting from nothing: this is what a runner would have done
    interp.backend.runSource(src, interp, filename);
}

inline Value loadEmbrModule(Interpreter& interp, const std::string& rawPath,
                            const SourceRange& site, bool mutateCallerScope,
                            bool forceReload = false)
{
    if (!interp.backend.loadModule) raiseError("load_module", "no script has run in this interpreter yet, so there is no execution backend to load a module with", site);
    return interp.backend.loadModule(interp, rawPath, site, mutateCallerScope, forceReload);
}

} // namespace embr

#endif // EMBR_CORE_INVOKE_H
