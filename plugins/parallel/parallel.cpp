// parallel.cpp
// threads for embr, the "isolated" way: every worker thread gets its own private Interpreter, and the only things
// that cross threads are channel messages and shared_table handles, each with its own lock
//
// this does not make an existing Interpreter/VmRunner safe to use from several threads. workers are
// separate (see workerLoop() below), like separate `embr` processes
//
// usage (raw primitives)
//   import "parallel"
//   in_ch  = channel()
//   out_ch = channel()
//   w = worker_spawn("worker.embr", "process", in_ch, out_ch)
//   channel_send(in_ch, 21)
//   channel_close(in_ch)
//   r = channel_recv(out_ch)      # {"ok": 1, "value": 42} (or {"ok":0,"error":{...}})
//   worker_join(w)
//
// usage (higher-level map pattern, see modules/parallel.embr)
//   import "parallel.embr"
//   results = worker_map("worker.embr", "process", [1,2,3,4], 4)
//
// good to know
//   - channel() has no limit. channel(n) holds n values, and channel_send then waits for room (channel_try_send does not wait)
//   - what can go in a channel or a shared_table value: plain data (numbers, strings, arrays/maps of the same) or
//     another shared_table handle. NOT a plain table() handle, an ffi handle or a closure: none are locked
//     (table.cpp's Table isn't, and the VM can write a closure's captured frame in place, see vm.h resolveStore),
//     so sharing one would only move the data race. channel_send()/shared_table_set() raise a clear error instead
//   - worker_spawn loads its module with loadEmbrModule (core/invoke.h), so the path is relative to the spawning
//     script's directory, like `import`/load_module(), not the process's cwd

#include <embr/embr.h>
#include "../table/table.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>

using namespace embr;
using namespace embrtable;

static Param pPtr(std::string n)                      { return Param::req(std::move(n), TS::Ptr); }
static Param pAny(std::string n)                      { return Param::req(std::move(n), TS::Any); }
static Param pStr(std::string n)                      { return Param::req(std::move(n), TS::Str); }
static Param pFn (std::string n)                      { return Param::req(std::move(n), TS::Fn);  }
static Param pOpt(std::string n, TypeSet m = TS::Any) { return Param::opt(std::move(n), m);       }

[[noreturn]] static void throwError(const std::string& fn, const std::string& msg) {
    raiseError("[parallel:" + fn + "]", msg);
}

// true if v is safe to alias across threads without its own lock: plain
// data, recursively, or a shared_table handle (which carries its own lock).
// anything else, a plain table() handle, an ffi handle, a closure, any
// other pointer tag, is refused. see file header for why.
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
                        "numbers/strings/arrays/maps -- or a shared_table handle): got " +
                        v.typeName() + (v.isPointer() ? " (tag '" + v.asPointer().type + "')" : ""));
}

// a thread-safe FIFO queue of Values. capacity == 0 means unbounded.
struct Channel {
    std::mutex              mu;
    std::condition_variable cv;
    std::deque<Value>       queue;
    size_t                  capacity = 0;
    bool                    closed   = false;

    void send(Value v) {
        v.markShared();   // the receiving thread will hold it
        std::unique_lock<std::mutex> lock(mu);
        if (capacity > 0)
            cv.wait(lock, [&] { return queue.size() < capacity || closed; });
        if (closed) throwError("channel_send", "cannot send on a closed channel");
        queue.push_back(std::move(v));
        lock.unlock();
        cv.notify_all();
    }

    // like send() but never waits: false if a bounded channel is full right now
    bool trySend(Value v) {
        v.markShared();
        std::unique_lock<std::mutex> lock(mu);
        if (closed) throwError("channel_try_send", "cannot send on a closed channel");
        if (capacity > 0 && queue.size() >= capacity) return false;
        queue.push_back(std::move(v));
        lock.unlock();
        cv.notify_all();
        return true;
    }

    // blocks until an item is available or the channel is closed and
    // drained (returned as std::nullopt, the latter case)
    std::optional<Value> recv() {
        std::unique_lock<std::mutex> lock(mu);
        cv.wait(lock, [&] { return !queue.empty() || closed; });
        if (queue.empty()) return std::nullopt;
        Value v = std::move(queue.front());
        queue.pop_front();
        lock.unlock();
        cv.notify_all(); // wake any sender blocked on a full bounded channel
        return v;
    }

    // non-blocking. {true, value} if something was waiting, {false, _} otherwise.
    std::pair<bool, Value> tryRecv() {
        std::unique_lock<std::mutex> lock(mu);
        if (queue.empty()) return {false, Value(0.0)};
        Value v = std::move(queue.front());
        queue.pop_front();
        lock.unlock();
        cv.notify_all();
        return {true, std::move(v)};
    }

    void close() {
        { std::lock_guard<std::mutex> lock(mu); closed = true; }
        cv.notify_all();
    }

    size_t size()     { std::lock_guard<std::mutex> lock(mu); return queue.size(); }
    bool   isClosed()  { std::lock_guard<std::mutex> lock(mu); return closed; }
};

static Channel* requireChannel(const Value& v) {
    return v.userdata<Channel>("parallel.channel");
}

// a Lua-style table (see table.h/table.cpp) with its own lock, meant to be shared on purpose across
// Interpreters/threads, unlike table.cpp's Table. it's a recursive_mutex so shared_table_update()'s callback can
// re-enter the same table from the same thread without deadlocking. it does NOT prevent a deadlock between two
// different shared_tables locked in opposite orders: callers combining several need a consistent order
struct SharedTable {
    std::recursive_mutex mu;
    TableData             data;
};

static SharedTable* requireSharedTable(const Value& v) {
    return v.userdata<SharedTable>("parallel.shared_table");
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

// the body of every worker thread: builds a fresh private Interpreter, loads the module into it (like `import`,
// so the module's own `import`s bring in the plugins it needs), resolves fnName, then pulls tasks off inCh,
// calls fnName(task, ...extraArgs) and pushes the wrapped result to outCh until inCh is closed and drained
static void workerLoop(std::string scriptDir, std::string modulePath, std::string fnName,
                       std::shared_ptr<Channel> inCh, std::shared_ptr<Channel> outCh,
                       std::vector<Value> extraArgs, PluginPolicy policy, Interpreter::Budget budget, Interpreter::BackendHooks backend, std::string cacheDir,
                       std::map<std::string, Interpreter::PackageRecord> packages)
{
    Interpreter workerInterp;
    workerInterp.scriptDir = scriptDir;
    workerInterp.backend = backend;   // the same way into script code as the spawning interpreter has
    workerInterp.setBudget(budget);   // the spawning script's step/time limit applies to the worker too
    workerInterp.cacheDir = std::move(cacheDir);   // the worker's modules use the same compiled-file cache
    workerInterp.packages = std::move(packages);   // so package_path() works in a worker for a package the parent imported
    // the spawning script's sandbox policy applies to the worker too: a fresh Interpreter defaults to
    // unrestricted, so without this a sandboxed script could `import "os"` from inside a worker
    workerInterp.pluginPolicy = std::move(policy);

    Value fn;
    try {
        loadEmbrModule(workerInterp, modulePath, {}, /*mutateCallerScope=*/true);
        if (!workerInterp.hasGlobal(fnName))
            raiseError("worker_spawn", "module \"" + modulePath +
                       "\" has no exported function \"" + fnName + "\"");
        fn = workerInterp.getGlobal(fnName);
    } catch (const EmbrError& e) {
        outCh->send(errResult(e));
        return;
    }

    while (true) {
        auto task = inCh->recv();
        if (!task) break; // inCh closed and drained, this worker is done

        std::vector<Value> args;
        args.reserve(1 + extraArgs.size());
        args.push_back(std::move(*task));
        for (auto& a : extraArgs) args.push_back(a);

        try {
            outCh->send(okResult(embr::invoke(workerInterp, fn, args)));
        } catch (const EmbrError& e) {
            outCh->send(errResult(e));
        }
    }
}

struct Worker {
    std::thread th;
    ~Worker() { if (th.joinable()) th.join(); }
};

EMBR_PLUGIN {

    // channel(capacity?: num) -> ptr<parallel.channel>
    // capacity omitted or 0 means unbounded; otherwise channel_send()
    // blocks once that many unreceived items are queued.
    interp->bindSig("channel", {pOpt("capacity", TS::Num)},
    [](const std::vector<Value>& args) -> Value {
        auto ch = std::make_shared<Channel>();
        if (!args.empty()) ch->capacity = (size_t)std::max(0.0, args[0].asNumber());
        return Value(TypedPtr(ch.get(), "parallel.channel", ch));
    });

    // channel_send(ch, value) -> 0
    interp->bindSig("channel_send", {pPtr("ch"), pAny("value")},
    [](const std::vector<Value>& args) -> Value {
        requireCrossThreadSafe("channel_send", args[1]);
        requireChannel(args[0])->send(args[1]);
        return Value(0.0);
    });

    // channel_try_send(ch, value) -> 1 or 0
    // for a channel with a capacity: 0 (and the value is not sent) when it is full right now, so a producer can
    // slow down or drop work instead of waiting. an unbounded channel always takes it
    interp->bindSig("channel_try_send", {pPtr("ch"), pAny("value")},
    [](const std::vector<Value>& args) -> Value {
        requireCrossThreadSafe("channel_try_send", args[1]);
        return Value(requireChannel(args[0])->trySend(args[1]) ? (int64_t)1 : (int64_t)0);
    });

    // channel_recv(ch) -> any
    // blocks until a value is available; raises "channel closed" once the
    // channel is closed and empty rather than returning a sentinel, use
    // channel_try_recv() for a non-blocking check instead.
    interp->bindSig("channel_recv", {pPtr("ch")},
    [](const std::vector<Value>& args) -> Value {
        auto v = requireChannel(args[0])->recv();
        if (!v) throwError("channel_recv", "channel closed");
        return *v;
    });

    // channel_try_recv(ch) -> [found: num, value: any]
    // non-blocking. found is 0 (with value 0) if nothing was queued right now.
    interp->bindSig("channel_try_recv", {pPtr("ch")},
    [](const std::vector<Value>& args) -> Value {
        auto [found, v] = requireChannel(args[0])->tryRecv();
        Value::array_type out;
        out.push_back(Value(found ? (int64_t)1 : (int64_t)0));
        out.push_back(std::move(v));
        return Value(std::move(out));
    });

    // channel_close(ch) -> 0
    // wakes any blocked channel_recv (which then raises once drained) and
    // any blocked channel_send on a full bounded channel (which raises
    // immediately, since sending on a closed channel is never valid).
    interp->bindSig("channel_close", {pPtr("ch")},
    [](const std::vector<Value>& args) -> Value {
        requireChannel(args[0])->close();
        return Value(0.0);
    });

    interp->bindSig("channel_len", {pPtr("ch")},
    [](const std::vector<Value>& args) -> Value {
        return Value((int64_t)requireChannel(args[0])->size());
    });

    interp->bindSig("channel_closed", {pPtr("ch")},
    [](const std::vector<Value>& args) -> Value {
        return Value(requireChannel(args[0])->isClosed() ? (int64_t)1 : (int64_t)0);
    });

    // worker_spawn(module_path, fn_name, in_channel, out_channel, extra_args?: arr) -> ptr<parallel.worker>
    //
    // starts a thread with its own private Interpreter (module_path is loaded into it like `import`, so its own
    // `import`s bring in the plugins it needs). the thread repeatedly pulls a task off in_channel, calls
    // fn_name(task, ...extra_args), and pushes {"ok":1,"value":result} (or {"ok":0,"error":{message,kind,line}} if
    // the call raised) onto out_channel, until in_channel is closed and drained, then it exits
    //
    // extra_args is evaluated once at spawn and passed to every call, e.g. a shared_table for workers to add into
    //
    // the handle's destructor joins the thread, which BLOCKS until it exits. close in_channel (and drain out_channel)
    // before a worker handle goes out of scope, or dropping it can hang. worker_join() does this explicitly
    interp->bindSig("worker_spawn",
        {pStr("module_path"), pStr("fn_name"), pPtr("in_channel"), pPtr("out_channel"), pOpt("extra_args", TS::Arr)},
    [interp](const std::vector<Value>& args) -> Value {
        auto inCh  = std::static_pointer_cast<Channel>(args[2].asPointer("parallel.channel").owner());
        auto outCh = std::static_pointer_cast<Channel>(args[3].asPointer("parallel.channel").owner());
        std::vector<Value> extraArgs = args.size() >= 5 ? args[4].asArray() : std::vector<Value>{};
        for (const Value& a : extraArgs) a.markShared();   // the worker thread will hold them

        auto w = std::make_shared<Worker>();
        w->th = std::thread(workerLoop, interp->scriptDir, args[0].asString(), args[1].asString(),
                            inCh, outCh, std::move(extraArgs), interp->pluginPolicy, interp->budget, interp->backend, interp->cacheDir, interp->packages);
        return Value(TypedPtr(w.get(), "parallel.worker", w));
    });

    // worker_join(w) -> 0
    // blocks until the worker's thread has exited (i.e. until its
    // in_channel has been closed and drained). safe to call more than once.
    interp->bindSig("worker_join", {pPtr("w")},
    [](const std::vector<Value>& args) -> Value {
        auto* w = args[0].userdata<Worker>("parallel.worker");
        if (w->th.joinable()) w->th.join();
        return Value(0.0);
    });

    // shared_table() -> ptr<parallel.shared_table>
    // like table() (see table.cpp) but locked: the one container this plugin treats as safe to share across workers.
    // same key rules as table.cpp (table.h): int/float normalize, strings and numbers never collide, pointers and
    // functions compare by identity, arrays/maps can't be keys
    interp->bind("shared_table", [](const std::vector<Value>&) -> Value {
        return Value::makeUserdata<SharedTable>("parallel.shared_table");
    });

    interp->bindSig("shared_table_set", {pPtr("t"), pAny("key"), pAny("value")},
    [](const std::vector<Value>& args) -> Value {
        requireCrossThreadSafe("shared_table_set", args[2]);
        auto* t = requireSharedTable(args[0]);
        TableKey k = makeKey("shared_table_set", args[1]);
        args[1].markShared(); args[2].markShared();   // other threads read the table
        std::lock_guard<std::recursive_mutex> lock(t->mu);
        t->data[k] = {args[1], args[2]};
        return Value(0.0);
    });

    interp->bindSig("shared_table_get", {pPtr("t"), pAny("key")},
    [](const std::vector<Value>& args) -> Value {
        auto* t = requireSharedTable(args[0]);
        TableKey k = makeKey("shared_table_get", args[1]);
        std::lock_guard<std::recursive_mutex> lock(t->mu);
        auto it = t->data.find(k);
        if (it == t->data.end())
            throwError("shared_table_get", "key not found: " + valueRepr(args[1]));
        return it->second.second;
    });

    interp->bindSig("shared_table_has", {pPtr("t"), pAny("key")},
    [](const std::vector<Value>& args) -> Value {
        auto* t = requireSharedTable(args[0]);
        TableKey k = makeKey("shared_table_has", args[1]);
        std::lock_guard<std::recursive_mutex> lock(t->mu);
        return Value(t->data.count(k) ? (int64_t)1 : (int64_t)0);
    });

    interp->bindSig("shared_table_delete", {pPtr("t"), pAny("key")},
    [](const std::vector<Value>& args) -> Value {
        auto* t = requireSharedTable(args[0]);
        TableKey k = makeKey("shared_table_delete", args[1]);
        std::lock_guard<std::recursive_mutex> lock(t->mu);
        return Value(t->data.erase(k) > 0 ? (int64_t)1 : (int64_t)0);
    });

    interp->bindSig("shared_table_len", {pPtr("t")},
    [](const std::vector<Value>& args) -> Value {
        auto* t = requireSharedTable(args[0]);
        std::lock_guard<std::recursive_mutex> lock(t->mu);
        return Value((int64_t)t->data.size());
    });

    interp->bindSig("shared_table_clear", {pPtr("t")},
    [](const std::vector<Value>& args) -> Value {
        auto* t = requireSharedTable(args[0]);
        std::lock_guard<std::recursive_mutex> lock(t->mu);
        t->data.clear();
        return Value(0.0);
    });

    interp->bindSig("shared_table_keys", {pPtr("t")},
    [](const std::vector<Value>& args) -> Value {
        auto* t = requireSharedTable(args[0]);
        std::lock_guard<std::recursive_mutex> lock(t->mu);
        Value::array_type out;
        out.reserve(t->data.size());
        for (auto& [k, kv] : t->data) out.push_back(kv.first);
        return Value(std::move(out));
    });

    interp->bindSig("shared_table_values", {pPtr("t")},
    [](const std::vector<Value>& args) -> Value {
        auto* t = requireSharedTable(args[0]);
        std::lock_guard<std::recursive_mutex> lock(t->mu);
        Value::array_type out;
        out.reserve(t->data.size());
        for (auto& [k, kv] : t->data) out.push_back(kv.second);
        return Value(std::move(out));
    });

    interp->bindSig("shared_table_items", {pPtr("t")},
    [](const std::vector<Value>& args) -> Value {
        auto* t = requireSharedTable(args[0]);
        std::lock_guard<std::recursive_mutex> lock(t->mu);
        Value::array_type out;
        out.reserve(t->data.size());
        for (auto& [k, kv] : t->data) {
            Value::array_type pair;
            pair.push_back(kv.first);
            pair.push_back(kv.second);
            out.push_back(Value(std::move(pair)));
        }
        return Value(std::move(out));
    });

    // shared_table_update(t, key, default, fn) -> any
    // atomic read-modify-write: cur = t[key] if present, else `default`; stores and returns fn(cur). the read, call
    // and write all happen under one lock, so this is the safe way to do a counter or accumulator (a separate
    // shared_table_get + shared_table_set would race). fn runs while holding t's lock: it may re-enter the same
    // table (the lock is recursive), but must not touch a different shared_table another thread might hold while
    // waiting for this one, which could deadlock
    interp->bindSig("shared_table_update", {pPtr("t"), pAny("key"), pAny("default"), pFn("fn")},
    [interp](const std::vector<Value>& args) -> Value {
        auto* t = requireSharedTable(args[0]);
        TableKey k = makeKey("shared_table_update", args[1]);
        std::lock_guard<std::recursive_mutex> lock(t->mu);
        auto it = t->data.find(k);
        Value cur = (it != t->data.end()) ? it->second.second : args[2];
        Value updated = embr::invoke(*interp, args[3], {cur});
        requireCrossThreadSafe("shared_table_update", updated);
        args[1].markShared(); updated.markShared();
        t->data[k] = {args[1], updated};
        return updated;
    });

    // what each always returns (see Value::Callable::returns)
    interp->setResultTypes({
        {"channel", TS::Ptr}, {"channel_send", TS::Float}, {"channel_try_send", TS::Int}, {"channel_try_recv", TS::Arr}, {"channel_close", TS::Float},
        {"channel_len", TS::Int}, {"channel_closed", TS::Int}, {"worker_spawn", TS::Ptr}, {"worker_join", TS::Float}, {"shared_table", TS::Ptr},
        {"shared_table_set", TS::Float}, {"shared_table_has", TS::Int}, {"shared_table_delete", TS::Int}, {"shared_table_len", TS::Int},
        {"shared_table_clear", TS::Float}, {"shared_table_keys", TS::Arr}, {"shared_table_values", TS::Arr}, {"shared_table_items", TS::Arr},
    });
}

// what this plugin says about itself. embr reads it before it runs embr_register (see core/plugininfo.h)
EMBR_PLUGIN_INFO("parallel", "1.0.0", "isolated-interpreter-per-thread concurrency", EMBR_CAP_THREADS, EMBR_CAP_NONE,
    {"channel", "0/omitted = unbounded; otherwise channel_send blocks once that many items are queued"},
    {"channel_close", "wakes any blocked recv/send"},
    {"channel_closed", "1 if the channel has been closed, else 0"},
    {"channel_len", "how many values are waiting in the channel"},
    {"channel_recv", "blocks; raises \"channel closed\" once closed and drained"},
    {"channel_send", "sends value to the channel, waiting for room if the channel is full. raises if the value is not safe to share across threads (plain data or a shared_table handle)"},
    {"channel_try_recv", "non-blocking"},
    {"channel_try_send", "non-blocking. gives 0 (and sends nothing) when a channel made with a capacity is full right now. channel() has no limit; channel(n) holds n values and channel_send waits for room, so use a capacity when a fast producer feeds a slow worker"},
    {"shared_table", "like table() but locked (std::recursive_mutex): the one container this plugin treats as safe to share across workers on purpose"},
    {"shared_table_clear", "removes every entry"},
    {"shared_table_delete", "removes key if it is there; 1 if something was removed, else 0"},
    {"shared_table_get", "the value stored under key. raises if there is none"},
    {"shared_table_has", "1 if key is in the table, else 0"},
    {"shared_table_items", "an array of [key, value] pairs"},
    {"shared_table_keys", "an array of the keys"},
    {"shared_table_len", "the number of entries"},
    {"shared_table_set", "same semantics as table's equivalents, lock-guarded"},
    {"shared_table_update", "atomic read-modify-write: cur = t[key] if present else default, then stores and returns fn(cur), all under one lock. the safe way to build a counter or accumulator (a get + set pair from script would race)"},
    {"shared_table_values", "an array of the values"},
    {"worker_join", "blocks until the thread has exited; safe to call more than once"},
    {"worker_spawn", "starts a thread with its own private Interpreter; loads module_path exactly like import (resolved relative to the *spawning* script's directory); loops recv → fn_name(task, ...extra_args) → send(wrapped result) until in_channel closes"});
