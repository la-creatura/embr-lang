// coroutine.cpp
// Lua-style coroutines for embr, built on the VM's own pause/resume (VmRunner::start/resume/suspend, see backends/vm.h)
//
// not the same thing as plugins/async or plugins/parallel:
//   - async/parallel: real threads, each with its own separate Interpreter, talking through channels. truly concurrent
//   - coroutine (this plugin): one thread, cooperative. a coroutine shares its creator's Interpreter (globals,
//     closures, plugin handles). only one thing runs at a time, and control changes hands only at
//     coroutine_resume() and coroutine_yield()
//
// usage
//   import "coroutine"
//   fn counter(start)
//       local n = start
//       while 1
//           local step = coroutine_yield(n)   # returns what the next resume passed
//           n = n + step
//       end
//   end
//   local co = coroutine_create(counter)
//   print(coroutine_resume(co, 10))   # 10, first resume's args become counter(start)'s args
//   print(coroutine_resume(co, 1))    # 11, step=1
//   print(coroutine_resume(co, 5))    # 16, step=5
//   print(coroutine_status(co))       # "suspended"
//
// good to know
//   - VM only. a coroutine body must be a function the VM compiled, so a backend added later would not run
//     coroutines until it can pause and resume the same way
//   - one value in and one value out per resume/yield (not Lua's multiple values), on purpose
//   - coroutine_yield() pauses with VmRunner::suspend() instead of inject()+YIELD. it is simpler and doesn't touch injBuf_
//   - how values travel: coroutine_yield(v) stores v on the Coroutine and returns a placeholder (a native must
//     return something). coroutine_resume() reads v back, and on the next resume swaps the placeholder for the new
//     value (VmRunner::setYieldResult), so `x = coroutine_yield()` gets the resume argument
//   - each Coroutine has its own VmRunner (own frames and stack) on the shared Interpreter, like a nested import.
//     not thread-safe: resume a coroutine only from the thread that made it
//   - dropping a suspended coroutine gives back its gc roots and call-depth count (~Coroutine), so abandoning them in a loop is fine

#include <embr/embr.h>

using namespace embr;

[[noreturn]] static void throwError(const std::string& fn, const std::string& msg) {
    raiseError("[coroutine:" + fn + "]", msg);
}

static Param pPtr(std::string n) { return Param::req(std::move(n), TS::Ptr); }
static Param pFn (std::string n) { return Param::req(std::move(n), TS::Fn); }

struct Coroutine {
    vm::VmRunner runner;
    Value        fn;
    bool         started = false;
    Value        yieldedValue{0.0};

    enum class State { Suspended, Running, Dead };
    State state = State::Suspended;

    Coroutine(Interpreter& interp, Value f) : runner(interp), fn(std::move(f)) {}
    // a coroutine dropped while suspended still has frames registered with the Interpreter
    ~Coroutine() { runner.releaseFrames(); }
};

// which Coroutine each thread is currently running inside, as a stack so a coroutine can resume another one.
// thread_local because async/parallel run scripts on other threads with their own Interpreters, and one
// thread's coroutine must never show up in another thread's coroutine_yield()
static thread_local std::vector<Coroutine*> g_running;

static Coroutine* requireCoroutine(const Value& v) {
    return v.userdata<Coroutine>("coroutine.handle");
}

static const char* stateName(Coroutine::State s) {
    switch (s) {
        case Coroutine::State::Suspended: return "suspended";
        case Coroutine::State::Running:   return "running";
        case Coroutine::State::Dead:      return "dead";
    }
    return "dead";
}

EMBR_PLUGIN {

    // coroutine_create(fn) -> ptr<coroutine.handle>
    // fn must be a script function compiled by the VM, raises otherwise. does not start running fn;
    // its body only begins executing on the first coroutine_resume().
    interp->bindSig("coroutine_create", {pFn("fn")},
    [interp](const std::vector<Value>& args) -> Value {
        const auto& c = args[0].asCallable();
        if (!c.isScript() || !c.script.compiledChunk)
            throwError("coroutine_create",
                       "coroutine body must be a script function compiled by the VM backend");
        return Value::makeUserdata<Coroutine>("coroutine.handle", *interp, args[0]);
    });

    // coroutine_resume(co, value) -> any
    // first call: value is passed as co's argument (only one is forwarded). later calls: value becomes
    // coroutine_yield()'s return value inside the coroutine. this call waits (it's a plain nested call, no
    // thread) until the coroutine yields again or returns, and returns that. raises if co is Dead or already
    // Running (for example resuming it from inside its own body)
    interp->bindSig("coroutine_resume", {pPtr("co"), Param::opt("value", TS::Any)},
    [](const std::vector<Value>& args) -> Value {
        Coroutine* co = requireCoroutine(args[0]);
        Value resumeVal = args.size() > 1 ? args[1] : Value(0.0);

        if (co->state == Coroutine::State::Dead)
            throwError("coroutine_resume", "cannot resume a dead coroutine");
        if (co->state == Coroutine::State::Running)
            throwError("coroutine_resume", "coroutine is already running");

        co->state = Coroutine::State::Running;
        g_running.push_back(co);
        Value out;
        try {
            if (!co->started) {
                co->started = true;
                std::vector<Value> startArgs;
                if (args.size() > 1) startArgs.push_back(resumeVal);
                out = co->runner.start(co->fn, startArgs);
            } else {
                co->runner.setYieldResult(resumeVal);
                out = co->runner.resume();
            }
        } catch (...) {
            g_running.pop_back();
            co->state = Coroutine::State::Dead;
            co->runner.releaseFrames();
            throw;
        }
        g_running.pop_back();

        if (co->runner.state() == vm::VmRunner::State::Suspended) {
            co->state = Coroutine::State::Suspended;
            return co->yieldedValue;
        }
        co->state = Coroutine::State::Dead;
        return out;
    });

    // coroutine_yield(value) -> any
    // suspends the currently running coroutine (the innermost one on this
    // thread's g_running stack), handing value back as this call's
    // coroutine_resume() result. raises if called outside any coroutine.
    interp->bindSig("coroutine_yield", {Param::opt("value", TS::Any)},
    [](const std::vector<Value>& args) -> Value {
        if (g_running.empty())
            throwError("coroutine_yield", "coroutine_yield() called outside a running coroutine");
        Coroutine* co = g_running.back();
        co->yieldedValue = args.empty() ? Value(0.0) : args[0];
        co->runner.suspend();
        return Value(0.0); // placeholder, coroutine_resume() overwrites it via setYieldResult()
    });

    // coroutine_status(co) -> "suspended" | "running" | "dead"
    interp->bindSig("coroutine_status", {pPtr("co")},
    [](const std::vector<Value>& args) -> Value {
        return Value(std::string(stateName(requireCoroutine(args[0])->state)));
    });
}

// what this plugin says about itself. embr reads it before it runs embr_register (see core/plugininfo.h)
EMBR_PLUGIN_INFO("coroutine", "1.0.0", "cooperative coroutines (VM backend only)", EMBR_CAP_NONE, EMBR_CAP_NONE,
    {"coroutine_create", "does not start fn; raises unless fn is VM-compiled"},
    {"coroutine_resume", "first call: value becomes the body's single argument; later calls: value is what the pending coroutine_yield returns. Returns the next yielded value, or the body's return value when it finishes. Raises if co is dead or already running (resuming from inside itself)"},
    {"coroutine_status", "\"suspended\", \"running\" or \"dead\""},
    {"coroutine_yield", "suspends the innermost running coroutine; raises outside any coroutine"});
