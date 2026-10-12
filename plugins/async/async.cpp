// async.cpp
// futures/promises over a small shared thread pool. "start some independent work, get a handle back right away,
// collect the result later", without managing threads or channels by hand
//
// not the same as plugins/parallel, which gives you raw workers and channels: one thread per worker_spawn(), and
// you wire the channels and join it yourself (see modules/parallel.embr's worker_map()). use that for N long-lived
// workers streaming a queue of tasks
//
// usage
//   import "async"
//   p1 = async_spawn("worker.embr", "fetch_thing", url1)
//   p2 = async_spawn("worker.embr", "fetch_thing", url2)
//   # ... do other work while those run concurrently ...
//   r1 = async_await(p1)   # {"ok":1,"value":...} or {"ok":0,"error":{...}}
//   r2 = async_await(p2)
//
//   results = async_all([p1, p2, p3])   # array of results, same order
//
// good to know
//   - async_spawn() doesn't start a thread per call. it pushes a task onto a pool of worker threads (started on
//     the first call, size set beforehand with async_pool_size()) and returns a promise at once
//   - the queue holds at most 10000 waiting tasks (async_queue_limit()): a spawner that gets ahead of the workers waits
//   - a task is (module_path, fn_name, arg) plus the spawning script's own scriptDir. the pool thread resolves
//     module_path relative to that scriptDir, like `import` would from that script
//   - each pool thread caches an Interpreter and resolved functions per (scriptDir, module_path) (WorkerModule
//     below). so different task modules never share globals, and a worker that keeps getting tasks for one module
//     loads it only once
//   - what can cross: plain data (numbers/strings/arrays/maps, recursively) or a parallel shared_table handle.
//     NOT a promise, a plain table() handle, an ffi handle or a closure. async_spawn()/async_await() raise a clear
//     error instead of letting an unsafe value through (same rule as parallel.cpp)

#include <embr/embr.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace embr;

static Param pPtr(std::string n)                      { return Param::req(std::move(n), TS::Ptr); }
static Param pAny(std::string n)                      { return Param::req(std::move(n), TS::Any); }
static Param pStr(std::string n)                      { return Param::req(std::move(n), TS::Str); }
static Param pNum(std::string n)                      { return Param::req(std::move(n), TS::Num); }
static Param pArr(std::string n)                      { return Param::req(std::move(n), TS::Arr); }

[[noreturn]] static void throwError(const std::string& fn, const std::string& msg) {
    raiseError("[async:" + fn + "]", msg);
}

// duplicated convention from plugins/parallel/parallel.cpp, keep the two
// in sync if that representation ever changes. see this file's own header
// and parallel.cpp's for why a plain table()/ffi handle/closure isn't safe
// to alias across threads but plain data and a shared_table handle are.
static bool isCrossThreadSafe(const Value& v) {
    if (v.isNumber() || v.isInt() || v.isString()) return true;
    if (v.isArray()) {
        for (auto& e : v.asArray()) if (!isCrossThreadSafe(e)) return false;
        return true;
    }
    if (v.isMap()) {
        for (auto& kv : v.asMap()) if (!isCrossThreadSafe(kv.second)) return false;
        return true;
    }
    if (v.isPointer() && v.asPointer().type == "parallel.shared_table") return true;
    return false;
}

static void requireCrossThreadSafe(const char* fn, const Value& v) {
    if (!isCrossThreadSafe(v))
        throwError(fn, "value is not safe to share across threads (must be plain data -- "
                        "numbers/strings/arrays/maps -- or a parallel.shared_table handle): got " +
                        v.typeName() + (v.isPointer() ? " (tag '" + v.asPointer().type + "')" : ""));
}

// {"ok": 1, "value": result}
static Value okResult(Value result) {
    Value::map_type m;
    m["ok"]    = Value((int64_t)1);
    m["value"] = std::move(result);
    return Value(std::move(m));
}

// {"ok": 0, "error": {message, kind, line}}
static Value errResult(const EmbrError& e) {
    Value::map_type m;
    m["ok"]    = Value((int64_t)0);
    m["error"] = errorToMap(e);
    return Value(std::move(m));
}

// one task's eventual outcome. made by async_spawn(), filled in by the pool thread that runs the task, read by
// async_await()/async_try_await()/async_all(). it outlives the task: kept alive by the promise handle's owner
// and the Task's copy, whichever the caller drops last
struct Promise {
    std::mutex              mu;
    std::condition_variable cv;
    bool                    ready = false;
    Value                   result{0.0}; // set once, under mu, before ready flips true
};

static Promise* requirePromise(const Value& v) {
    return v.userdata<Promise>("async.promise");
}

struct Task {
    std::string scriptDir;  // captured from the spawning script at async_spawn() time
    std::string modulePath;
    std::string fnName;
    Value arg;
    std::shared_ptr<Promise> promise;
    PluginPolicy policy;    // the spawning script's sandbox policy, applied to the worker's Interpreter
    Interpreter::BackendHooks backend;   // the spawning interpreter's way into script code, for the worker's own interpreter
    Interpreter::Budget budget;   // the spawning script's step/time limit, so a worker can't run past it
    std::string cacheDir;   // the spawning script's compiled-file cache folder, also used by the worker
    std::map<std::string, Interpreter::PackageRecord> packages;   // packages the spawning script imported, so package_path() works in the worker
};

// one module's worth of a pool thread's state: its own private Interpreter (scriptDir set once, from the first
// task naming this module) and its own fnName -> Value cache. never shared with another (scriptDir, modulePath)
// pair or another thread (see the file header)
struct WorkerModule {
    Interpreter interp;
    std::unordered_map<std::string, Value> fns;
};

// true on the pool's own threads. a worker's Interpreter can import async too, but only the host Interpreters
// (the ones on other threads) decide when the pool shuts down
static thread_local bool tlsInWorker = false;

// the shared pool: a fixed set of worker threads pulling tasks off one
// shared queue. started lazily on the first async_spawn(); see
// async_pool_size() to pick the thread count before that happens.
struct Pool {
    std::mutex              mu;
    std::condition_variable cv;
    std::condition_variable space;     // a task was taken off the queue, so a spawner waiting on a full queue can go on
    std::deque<Task>        tasks;
    size_t                  maxQueued = 10000;   // async_queue_limit(). 0 = no limit
    std::vector<std::thread> threads;
    std::atomic<bool>       stopping{false};
    std::atomic<bool>       started{false};
    size_t                  size = std::max(1u, std::thread::hardware_concurrency());

    void ensureStarted() {
        bool expected = false;
        if (!started.compare_exchange_strong(expected, true)) return;
        threads.reserve(size);
        for (size_t i = 0; i < size; i++)
            threads.emplace_back([this] { workerLoop(); });
    }

    void workerLoop() {
        tlsInWorker = true;
        // (scriptDir + "\x1f" + modulePath) -> that pair's own WorkerModule.
        // the separator is never valid in either half (scriptDir/modulePath
        // are real filesystem paths), so this can't collide across pairs.
        std::unordered_map<std::string, std::unique_ptr<WorkerModule>> modules;

        while (true) {
            Task task;
            {
                std::unique_lock<std::mutex> lock(mu);
                cv.wait(lock, [this] { return stopping.load() || !tasks.empty(); });
                if (tasks.empty()) {
                    if (stopping.load()) return;
                    continue;
                }
                task = std::move(tasks.front());
                tasks.pop_front();
            }
            space.notify_one();

            Value out;
            try {
                // the worker Interpreter for a (scriptDir, module) pair is cached and reused, so the policy
                // is part of the key: a task spawned under a stricter sandbox must never be handed an
                // Interpreter that was created with (or loaded plugins under) a looser one
                std::string policyKey = task.policy.restricted ? "R" : "U";
                for (const auto& n : task.policy.allowed) policyKey += "," + n;
                policyKey += task.policy.fileIo ? ";io" : ";noio";
                std::string key = task.scriptDir + "\x1f" + task.modulePath + "\x1f" + policyKey;
                auto mit = modules.find(key);
                if (mit == modules.end()) {
                    auto wm = std::make_unique<WorkerModule>();
                    wm->interp.scriptDir = task.scriptDir;
                    wm->interp.backend = task.backend;
                    wm->interp.pluginPolicy = task.policy;
                    wm->interp.cacheDir = task.cacheDir;
                    wm->interp.packages = task.packages;
                    mit = modules.emplace(std::move(key), std::move(wm)).first;
                }
                WorkerModule& wm = *mit->second;
                wm.interp.setBudget(task.budget);

                auto it = wm.fns.find(task.fnName);
                Value fn;
                if (it != wm.fns.end()) {
                    fn = it->second;
                } else {
                    loadEmbrModule(wm.interp, task.modulePath, {}, /*mutateCallerScope=*/true);
                    if (!wm.interp.hasGlobal(task.fnName))
                        raiseError("async_spawn", "module \"" + task.modulePath +
                                   "\" has no exported function \"" + task.fnName + "\"");
                    fn = wm.interp.getGlobal(task.fnName);
                    wm.fns[task.fnName] = fn;
                }
                out = okResult(embr::invoke(wm.interp, fn, {task.arg}));
            } catch (const EmbrError& e) {
                out = errResult(e);
            }

            out.markShared();   // the thread that awaits it will hold it
            {
                std::lock_guard<std::mutex> lock(task.promise->mu);
                task.promise->result = std::move(out);
                task.promise->ready  = true;
            }
            task.promise->cv.notify_all();
        }
    }

    // stops the workers and waits for them. a later async_spawn() starts the pool again
    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(mu);
            stopping = true;
        }
        cv.notify_all();
        space.notify_all();
        for (auto& t : threads) if (t.joinable()) t.join();
        threads.clear();
        stopping = false;
        started = false;
    }

    ~Pool() { shutdown(); }
};

// how many host Interpreters have imported this plugin and are still alive
static std::atomic<int>& liveHosts() {
    static std::atomic<int> n{0};
    return n;
}

static Pool& pool() {
    static Pool p;
    return p;
}

EMBR_PLUGIN {

    // stop the pool when the last host Interpreter goes away, before the plugin is unloaded. unloading runs the
    // pool's destructor while the dynamic loader is locked, and the workers need that lock to unload their own
    // plugins, which deadlocks on macOS
    if (!tlsInWorker) {
        ++liveHosts();
        interp->addTeardownHook([] { if (--liveHosts() == 0) pool().shutdown(); });
    }

    // async_pool_size(n) -> 0
    // sets the worker-thread count for the shared pool. call it before the first async_spawn() in the process,
    // it raises once the pool is running (the count is fixed then). default: std::thread::hardware_concurrency(), or 1 if unknown
    interp->bindSig("async_pool_size", {pNum("n")},
    [](const std::vector<Value>& args) -> Value {
        int64_t n = numToInt64(args[0], "async_pool_size", "n");
        if (n < 1) throwError("async_pool_size", "n must be >= 1");
        if (n > 1024) throwError("async_pool_size", "n must be <= 1024 threads");
        if (pool().started.load())
            throwError("async_pool_size", "the pool has already started (first async_spawn() call) -- "
                                           "call async_pool_size() before spawning anything");
        pool().size = (size_t)n;
        return Value(0.0);
    });

    // async_spawn(module_path, fn_name, arg) -> ptr<async.promise>
    //
    // submits a task to the shared pool (starting it on first use) and returns a promise right away, it doesn't
    // block. module_path is loaded like `import` would, relative to this call's script directory, and cached per
    // (scriptDir, module_path) on whichever pool thread handles it
    // that thread calls fn_name(arg) in the module's own private Interpreter and stores the wrapped outcome.
    // get it with async_await()/async_try_await(), or async_all() for several
    interp->bindSig("async_spawn", {pStr("module_path"), pStr("fn_name"), pAny("arg")},
    [interp](const std::vector<Value>& args) -> Value {
        requireCrossThreadSafe("async_spawn", args[2]);
        args[2].markShared();   // a pool thread will hold it
        pool().ensureStarted();

        auto promise = std::make_shared<Promise>();
        {
            std::unique_lock<std::mutex> lock(pool().mu);
            // a full queue slows the spawner down instead of growing without limit. a pool thread must not wait here
            // (every thread could end up waiting for room that only a thread can make), so for it a full queue is an error
            if (pool().maxQueued > 0 && pool().tasks.size() >= pool().maxQueued) {
                if (tlsInWorker)
                    throwError("async_spawn", "the task queue is full (" + std::to_string(pool().maxQueued) + " waiting)");
                pool().space.wait(lock, [] { return pool().stopping.load() || pool().tasks.size() < pool().maxQueued; });
            }
            pool().tasks.push_back(Task{interp->scriptDir, args[0].asString(), args[1].asString(), args[2], promise, interp->pluginPolicy, interp->backend, interp->budget, interp->cacheDir, interp->packages});
        }
        pool().cv.notify_one();

        return Value(TypedPtr(promise.get(), "async.promise", promise));
    });

    // async_queue_limit(n?) -> int
    // how many tasks may wait for a free pool thread. async_spawn() waits when the queue is full (on a pool thread
    // it raises instead). with n it sets the limit (0 = no limit, default 10000), always returns the limit
    interp->bindSig("async_queue_limit", {Param::opt("n", TS::Num)},
    [](const std::vector<Value>& args) -> Value {
        if (!args.empty()) {
            int64_t n = numToInt64(args[0], "async_queue_limit", "n");
            if (n < 0) throwError("async_queue_limit", "n must be >= 0");
            { std::lock_guard<std::mutex> lock(pool().mu); pool().maxQueued = (size_t)n; }
            pool().space.notify_all();
        }
        return Value((int64_t)pool().maxQueued);
    });

    // async_await(p) -> {"ok":1,"value":...} or {"ok":0,"error":{message,kind,line}}
    // blocks until p's task has been picked up and finished. safe to call
    // more than once on the same promise (returns the same stored result).
    interp->bindSig("async_await", {pPtr("p")},
    [](const std::vector<Value>& args) -> Value {
        auto* p = requirePromise(args[0]);
        std::unique_lock<std::mutex> lock(p->mu);
        p->cv.wait(lock, [&] { return p->ready; });
        return p->result;
    });

    // async_try_await(p) -> {"ready":0} if not finished yet, otherwise
    // {"ready":1,"ok":...,"value"/"error":...}, the same fields
    // async_await() returns, plus "ready". non-blocking.
    interp->bindSig("async_try_await", {pPtr("p")},
    [](const std::vector<Value>& args) -> Value {
        auto* p = requirePromise(args[0]);
        std::lock_guard<std::mutex> lock(p->mu);
        Value::map_type m;
        if (!p->ready) {
            m["ready"] = Value((int64_t)0);
            return Value(std::move(m));
        }
        m["ready"] = Value((int64_t)1);
        for (auto& kv : p->result.asMap()) m[kv.first] = kv.second;
        return Value(std::move(m));
    });

    // async_all(promises: arr) -> arr
    // blocks until every promise has finished, and returns their results ({"ok":...,...}) in the input order
    // it doesn't stop at the first error: every task is already running and can't be cancelled, so this waits for
    // and reports all of them (like Promise.allSettled, not Promise.all)
    interp->bindSig("async_all", {pArr("promises")},
    [](const std::vector<Value>& args) -> Value {
        Value::array_type out;
        out.reserve(args[0].asArray().size());
        for (auto& v : args[0].asArray()) {
            auto* p = requirePromise(v);
            std::unique_lock<std::mutex> lock(p->mu);
            p->cv.wait(lock, [&] { return p->ready; });
            out.push_back(p->result);
        }
        return Value(std::move(out));
    });

    // what each always returns (see Value::Callable::returns)
    interp->setResultTypes({
        {"async_pool_size", TS::Float}, {"async_spawn", TS::Ptr}, {"async_queue_limit", TS::Int}, {"async_await", TS::Map},
        {"async_try_await", TS::Map}, {"async_all", TS::Arr},
    });
}

// what this plugin says about itself. embr reads it before it runs embr_register (see core/plugininfo.h)
EMBR_PLUGIN_INFO("async", "1.0.0", "future/promise layer over a shared thread pool", EMBR_CAP_THREADS, EMBR_CAP_NONE,
    {"async_all", "waits for all, results in input order. does not stop at the first failure (every task is already running and can't be cancelled), so it behaves like Promise.allSettled, not fail-fast"},
    {"async_await", "blocks until resolved; safe to call more than once"},
    {"async_pool_size", "must be called before the pool has started (i.e. before the first async_spawn); raises otherwise"},
    {"async_queue_limit", "how many tasks may wait for a free pool thread, default 10000, 0 for no limit. async_spawn waits when the queue is full, so a loop that spawns faster than the workers finish slows down instead of using more and more memory. on a pool thread (a task that spawns tasks) it raises instead of waiting, so the pool cannot jam itself. with n it sets the limit; it always returns the limit"},
    {"async_spawn", "returns a promise handle immediately, does not block; arg must be cross-thread-safe (same rule as parallel)"},
    {"async_try_await", "non-blocking poll"});
