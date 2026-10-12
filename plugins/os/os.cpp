// os.cpp
// process/environment plugin for embr: environment variables, script
// arguments, cwd, pid, platform, exiting, and running a subprocess.
//
// usage
//   import "os"
//   print(os_getenv("HOME"))
//   print(os_getenv("NOPE", "fallback"))
//   r = os_exec(["echo", "hi"])       # no shell involved
//   print(r["code"])                  # 0
//   print(r["stdout"])                # hi\n
//
// embr has no nil/bool core type (see embrtypes), so "unset" is expressed the
// way the rest of the stdlib does: os_getenv takes a default and raises if the
// variable is unset and none was given; os_has_env answers 1/0.
//
// deliberately NOT here: chdir (it would silently change where relative
// paths and cwd-fallback plugin lookup resolve for every other plugin) and
// shell-string execution (os_exec takes an argv array, so no quoting or
// injection concerns). os_exec works on Windows too (CreateProcess), but there is no signal code there.

#include <embr/embr.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <iostream>

#if defined(_WIN32)
#  include <process.h>
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <poll.h>
#  include <signal.h>
#  include <spawn.h>
#  include <sys/wait.h>
#  include <unistd.h>
#  if defined(__APPLE__)
#    include <crt_externs.h>
#    define environ (*_NSGetEnviron())     // a macOS dylib can't link against the `environ` symbol directly
#  else
extern char** environ;
#  endif
#endif

using namespace embr;

static Param pStr(std::string n)                      { return Param::req(std::move(n), TS::Str); }
static Param pArr(std::string n)                      { return Param::req(std::move(n), TS::Arr); }
static Param pOpt(std::string n, TypeSet m = TS::Any) { return Param::opt(std::move(n), m);       }

[[noreturn]] static void fail(const std::string& fn, const std::string& msg) {
    raiseError("[os:" + fn + "]", msg);
}

#if !defined(_WIN32)
namespace {
struct Fd {
    int fd = -1;
    Fd() = default;
    explicit Fd(int f) : fd(f) {}
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    ~Fd() { close(); }
    void close() { if (fd >= 0) { ::close(fd); fd = -1; } }
};

// runs argv (no shell), feeding `input` on stdin and collecting stdout/stderr
// concurrently via poll() so a child that fills either pipe can't deadlock us
Value execProcess(const std::vector<std::string>& argvStr, const std::string& input) {
    int in[2], out[2], err[2];
    if (pipe(in) != 0) fail("os_exec", "pipe failed");
    Fd inR(in[0]), inW(in[1]);
    if (pipe(out) != 0) fail("os_exec", "pipe failed");
    Fd outR(out[0]), outW(out[1]);
    if (pipe(err) != 0) fail("os_exec", "pipe failed");
    Fd errR(err[0]), errW(err[1]);

    std::vector<char*> argv;
    for (auto& a : argvStr) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, inR.fd,  0);
    posix_spawn_file_actions_adddup2(&fa, outW.fd, 1);
    posix_spawn_file_actions_adddup2(&fa, errW.fd, 2);
    for (int fd : {inR.fd, inW.fd, outR.fd, outW.fd, errR.fd, errW.fd})
        posix_spawn_file_actions_addclose(&fa, fd);

    // a write to a pipe whose reader exited must fail with EPIPE, not kill us
    struct sigaction ign{}, old{};
    ign.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &ign, &old);

    std::cout.flush();
    pid_t pid;
    int rc = posix_spawnp(&pid, argv[0], &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) {
        sigaction(SIGPIPE, &old, nullptr);
        fail("os_exec", "cannot run '" + argvStr[0] + "': " + std::strerror(rc));
    }
    inR.close(); outW.close(); errW.close();

    std::string sOut, sErr;
    size_t written = 0;
    if (input.empty()) inW.close();
    // non-blocking so a write bigger than the pipe buffer returns partially
    // instead of blocking while the child waits for us to drain its output
    else fcntl(inW.fd, F_SETFL, fcntl(inW.fd, F_GETFL) | O_NONBLOCK);
    char buf[4096];
    while (outR.fd >= 0 || errR.fd >= 0 || inW.fd >= 0) {
        pollfd pfds[3]; int n = 0; int iO = -1, iE = -1, iI = -1;
        if (outR.fd >= 0) { iO = n; pfds[n++] = {outR.fd, POLLIN,  0}; }
        if (errR.fd >= 0) { iE = n; pfds[n++] = {errR.fd, POLLIN,  0}; }
        if (inW.fd  >= 0) { iI = n; pfds[n++] = {inW.fd,  POLLOUT, 0}; }
        if (poll(pfds, n, -1) < 0) { if (errno == EINTR) continue; break; }
        auto drain = [&](int idx, Fd& f, std::string& dst) {
            if (idx < 0 || !(pfds[idx].revents & (POLLIN | POLLHUP | POLLERR))) return;
            ssize_t r = read(f.fd, buf, sizeof buf);
            if (r > 0) dst.append(buf, (size_t)r); else if (r == 0 || errno != EINTR) f.close();
        };
        drain(iO, outR, sOut);
        drain(iE, errR, sErr);
        if (iI >= 0 && (pfds[iI].revents & (POLLOUT | POLLERR | POLLHUP))) {
            ssize_t w = write(inW.fd, input.data() + written, input.size() - written);
            if (w > 0) written += (size_t)w;
            if ((w < 0 && errno != EINTR && errno != EAGAIN) || written >= input.size()) inW.close();
        }
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    sigaction(SIGPIPE, &old, nullptr);

    // exit status, or -N if the child was killed by signal N
    int64_t code = WIFEXITED(status) ? WEXITSTATUS(status)
                 : WIFSIGNALED(status) ? -(int64_t)WTERMSIG(status) : -1;
    Value::map_type m;
    m["code"]   = Value(code);
    m["stdout"] = Value(std::move(sOut));
    m["stderr"] = Value(std::move(sErr));
    return Value(std::move(m));
}
} // namespace
#endif

#if defined(_WIN32)
namespace {
// quotes one argument the way CommandLineToArgvW / the C runtime will split it back
std::string quoteArg(const std::string& a) {
    if (!a.empty() && a.find_first_of(" \t\n\v\"") == std::string::npos) return a;
    std::string out = "\"";
    size_t slashes = 0;
    for (char c : a) {
        if (c == '\\') { ++slashes; continue; }
        if (c == '"') out.append(slashes * 2 + 1, '\\');
        else out.append(slashes, '\\');
        slashes = 0;
        out += c;
    }
    out.append(slashes * 2, '\\');   // trailing backslashes must not escape the closing quote
    return out + "\"";
}

struct Handle {
    HANDLE h = nullptr;
    Handle() = default;
    explicit Handle(HANDLE x) : h(x) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { close(); }
    void close() { if (h) { CloseHandle(h); h = nullptr; } }
};

// reads a pipe to the end on its own thread, so neither output pipe can fill up and block the child
struct Reader {
    HANDLE pipe;
    std::string data;
    HANDLE thread = nullptr;
    explicit Reader(HANDLE p) : pipe(p) {
        thread = CreateThread(nullptr, 0, [](LPVOID self) -> DWORD {
            auto* r = static_cast<Reader*>(self);
            char buf[4096]; DWORD n;
            while (ReadFile(r->pipe, buf, sizeof buf, &n, nullptr) && n > 0) r->data.append(buf, n);
            return 0;
        }, this, 0, nullptr);
    }
    void join() { if (thread) { WaitForSingleObject(thread, INFINITE); CloseHandle(thread); thread = nullptr; } }
    ~Reader() { join(); }
};

// runs argv (no shell), feeding `input` on stdin and collecting stdout/stderr
Value execProcess(const std::vector<std::string>& argvStr, const std::string& input) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    Handle inR, inW, outR, outW, errR, errW;
    if (!CreatePipe(&inR.h, &inW.h, &sa, 0) || !CreatePipe(&outR.h, &outW.h, &sa, 0) ||
        !CreatePipe(&errR.h, &errW.h, &sa, 0))
        fail("os_exec", "pipe failed");
    // our ends must not be inherited by the child
    SetHandleInformation(inW.h,  HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outR.h, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errR.h, HANDLE_FLAG_INHERIT, 0);

    std::string cmd;
    for (size_t i = 0; i < argvStr.size(); ++i) cmd += (i ? " " : "") + quoteArg(argvStr[i]);

    STARTUPINFOA si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = inR.h; si.hStdOutput = outW.h; si.hStdError = errW.h;
    PROCESS_INFORMATION pi{};
    std::cout.flush();
    // lpApplicationName is null so the program is looked up on PATH (".exe" is added if missing)
    if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi))
        fail("os_exec", "cannot run '" + argvStr[0] + "': error " + std::to_string(GetLastError()));
    Handle proc(pi.hProcess), thr(pi.hThread);
    inR.close(); outW.close(); errW.close();

    Reader rOut(outR.h), rErr(errR.h);
    // writing happens here; the child may exit without reading, which just makes WriteFile fail
    size_t written = 0;
    while (written < input.size()) {
        DWORD w = 0;
        if (!WriteFile(inW.h, input.data() + written, (DWORD)std::min<size_t>(input.size() - written, 1 << 16), &w, nullptr) || w == 0)
            break;
        written += w;
    }
    inW.close();
    rOut.join(); rErr.join();
    WaitForSingleObject(proc.h, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(proc.h, &code);

    Value::map_type m;
    m["code"]   = Value((int64_t)(int32_t)code);
    m["stdout"] = Value(std::move(rOut.data));
    m["stderr"] = Value(std::move(rErr.data));
    return Value(std::move(m));
}
} // namespace
#endif

EMBR_PLUGIN {

    // os_getenv(name: str, default?: str) -> str
    // raises if the variable is unset and no default was given
    interp->bindSpan("os_getenv", {pStr("name"), pOpt("default", TS::Str)},
    [](const ArgSpan& args) -> Value {
        const char* v = std::getenv(args[0].asString().c_str());
        if (v) return Value(std::string(v));
        if (args.size() >= 2) return args[1];
        fail("os_getenv", "environment variable '" + args[0].asString() + "' is not set");
    });

    // os_has_env(name: str) -> num   (1/0)
    interp->bindSpan("os_has_env", {pStr("name")},
    [](const ArgSpan& args) -> Value {
        return Value(std::getenv(args[0].asString().c_str()) ? (int64_t)1 : (int64_t)0);
    });

    // os_setenv(name: str, value: str) -> 0
    // affects this process and any subprocess started afterwards (os_exec)
    interp->bindSpan("os_setenv", {pStr("name"), pStr("value")},
    [](const ArgSpan& args) -> Value {
        const std::string& n = args[0].asString();
        if (n.empty() || n.find('=') != std::string::npos || n.find('\0') != std::string::npos)
            fail("os_setenv", "invalid variable name '" + n + "'");
#if defined(_WIN32)
        if (_putenv_s(n.c_str(), args[1].asString().c_str()) != 0)
#else
        if (setenv(n.c_str(), args[1].asString().c_str(), 1) != 0)
#endif
            fail("os_setenv", "cannot set '" + n + "'");
        return Value(0.0);
    });

    // os_unsetenv(name: str) -> 0   (no error if it wasn't set)
    interp->bindSpan("os_unsetenv", {pStr("name")},
    [](const ArgSpan& args) -> Value {
        const std::string& n = args[0].asString();
        if (n.empty() || n.find('=') != std::string::npos)
            fail("os_unsetenv", "invalid variable name '" + n + "'");
#if defined(_WIN32)
        _putenv_s(n.c_str(), "");
#else
        unsetenv(n.c_str());
#endif
        return Value(0.0);
    });

    // os_environ() -> map of every environment variable
    interp->bindSpan("os_environ", {},
    [](const ArgSpan&) -> Value {
        Value::map_type m;
#if !defined(_WIN32)
        for (char** e = environ; e && *e; ++e) {
            std::string kv(*e);
            size_t eq = kv.find('=');
            if (eq == std::string::npos || eq == 0) continue;
            m[kv.substr(0, eq)] = Value(kv.substr(eq + 1));
        }
#else
        if (char* block = GetEnvironmentStringsA()) {
            for (const char* e = block; *e; e += std::strlen(e) + 1) {
                std::string kv(e);
                size_t eq = kv.find('=', 1);   // entries like "=C:=C:\\" start with '='
                if (eq == std::string::npos) continue;
                m[kv.substr(0, eq)] = Value(kv.substr(eq + 1));
            }
            FreeEnvironmentStringsA(block);
        }
#endif
        return Value(std::move(m));
    });

    // os_args() -> arr of str
    // the arguments after a bare `--` on the embr command line:
    //   embr script.embr -- a b   ->   ["a", "b"]
    interp->bindSpan("os_args", {},
    [interp](const ArgSpan&) -> Value {
        Value::array_type out;
        for (const auto& a : interp->scriptArgs) out.push_back(Value(a));
        return Value(std::move(out));
    });

    // os_cwd() -> str
    interp->bindSpan("os_cwd", {},
    [](const ArgSpan&) -> Value {
        std::error_code ec;
        auto p = std::filesystem::current_path(ec);
        if (ec) fail("os_cwd", ec.message());
        return Value(p.string());
    });

    // os_pid() -> num
    interp->bindSpan("os_pid", {},
    [](const ArgSpan&) -> Value {
#if defined(_WIN32)
        return Value((int64_t)_getpid());
#else
        return Value((int64_t)getpid());
#endif
    });

    // os_platform() -> "linux" | "macos" | "windows" | "unknown"
    interp->bindSpan("os_platform", {},
    [](const ArgSpan&) -> Value {
#if defined(_WIN32)
        return Value(std::string("windows"));
#elif defined(__APPLE__)
        return Value(std::string("macos"));
#elif defined(__linux__)
        return Value(std::string("linux"));
#else
        return Value(std::string("unknown"));
#endif
    });

    // os_exit(code?: num) -> never returns
    // flushes stdout then ends the whole process with the given status
    // (default 0; the OS keeps only the low 8 bits). not catchable by try/catch.
    interp->bindSpan("os_exit", {pOpt("code", TS::Num)},
    [](const ArgSpan& args) -> Value {
        int code = 0;
        if (!args.empty()) {
            double d = args[0].asNumber();
            if (!(d >= -2147483648.0 && d <= 2147483647.0)) fail("os_exit", "exit code out of range");
            code = (int)d;
        }
        std::cout.flush();
        std::exit(code);
    });

    // os_exec(argv: arr of str, input?: str) -> map {code, stdout, stderr}
    // runs a program directly (PATH lookup, NO shell), optionally feeding
    // `input` to its stdin, and waits for it. code is the exit status, or
    // -N if the process was killed by signal N. a program that can't be
    // started raises; one that runs and fails just reports a nonzero code.
    interp->bindSpan("os_exec", {pArr("argv"), pOpt("input", TS::Str)},
    [](const ArgSpan& args) -> Value {
        std::vector<std::string> argv;
        for (size_t i = 0; i < args[0].asArray().size(); ++i) {
            const Value& a = args[0].asArray()[i];
            if (!a.isString())
                fail("os_exec", "argv[" + std::to_string(i) + "] must be a string, got " + a.typeName());
            argv.push_back(a.asString());
        }
        if (argv.empty()) fail("os_exec", "argv must not be empty");
        return execProcess(argv, args.size() >= 2 ? args[1].asString() : std::string());
    });

    // what each always returns (see Value::Callable::returns)
    interp->setResultTypes({
        {"os_has_env", TS::Int}, {"os_setenv", TS::Float}, {"os_unsetenv", TS::Float}, {"os_environ", TS::Map},
        {"os_args", TS::Arr}, {"os_cwd", TS::Str}, {"os_pid", TS::Int}, {"os_platform", TS::Str},
    });
    interp->setResultElems({{"os_args", TS::Str}});
}

// what this plugin says about itself. embr reads it before it runs embr_register (see core/plugininfo.h)
EMBR_PLUGIN_INFO("os", "1.0.0", "environment, arguments, process", EMBR_CAP_PROCESS | EMBR_CAP_ENV, EMBR_CAP_NONE,
    {"os_args", "arguments after a bare --: embr script.embr -- a b → [\"a\", \"b\"] (stored in Interpreter::scriptArgs; embr pkg keeps its own arg handling; parallel worker interpreters see [])"},
    {"os_cwd", "os_cwd returns / separators on Windows too. platform is \"linux\", \"macos\", \"windows\" or \"unknown\""},
    {"os_environ", "snapshot of all variables"},
    {"os_exec", "runs a program directly (PATH lookup, no shell), feeds input to stdin, returns {code, stdout, stderr}; code is -N if killed by signal N (POSIX only, Windows reports the plain exit code); a program that can't start raises, one that exits nonzero does not. stdout/stderr/stdin are pumped concurrently (no deadlock on large data). on Windows it uses CreateProcess, so argv[0] must be a real program (cmd, findstr), not a shell builtin or sh"},
    {"os_exit", "flushes stdout then ends the process; not catchable by try/catch"},
    {"os_getenv", "raises if unset and no default (there is no nil core type, see the type system)"},
    {"os_has_env", "1 if the environment variable is set, else 0"},
    {"os_pid", "os_cwd returns / separators on Windows too. platform is \"linux\", \"macos\", \"windows\" or \"unknown\""},
    {"os_platform", "os_cwd returns / separators on Windows too. platform is \"linux\", \"macos\", \"windows\" or \"unknown\""},
    {"os_setenv", "affects this process and later os_exec children; invalid names (empty, containing =) raise"},
    {"os_unsetenv", "affects this process and later os_exec children; invalid names (empty, containing =) raise"});
