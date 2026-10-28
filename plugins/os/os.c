/* os.c
 * process and environment plugin for embr: environment variables, script arguments, the working folder, the process id, the
 * platform, exiting, running a program, and raw stdin and stdout. written in C against embr_plugin.h
 *
 * usage
 *   import "os"
 *   print(os_getenv("HOME"))
 *   print(os_getenv("NOPE", "fallback"))
 *   r = os_exec(["echo", "hi"])       # no shell involved
 *   print(r["code"])                  # 0
 *   print(r["stdout"])                # hi\n
 *
 * good to know
 *   - embr has no nil, so "unset" is a default: os_getenv takes one and raises when the variable is unset and none was given.
 *     os_has_env answers 1 or 0
 *   - deliberately not here: chdir (it would change where relative paths and the plugin lookup resolve for every other
 *     plugin) and running a shell string (os_exec takes an argv array, so there is nothing to quote or inject into)
 *   - os_exec works on Windows too (CreateProcess), but there is no signal code there
 *   - stdin and stdout go through the host (embrp_stdin_read, embrp_stdio_write), the same streams print() and input() use,
 *     so the order of output and input stays right
 */
#include <embr_plugin.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <direct.h>
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
#    define environ (*_NSGetEnviron())     /* a macOS dylib can't link against the `environ` symbol directly */
#  else
extern char** environ;
#  endif
#endif

static int oom(EmbrCtx* ctx) { return embrp_fail(ctx, "out of memory"); }

/* ---- a growable byte buffer ---- */

typedef struct { char* p; size_t len, cap; } Buf;

static int buf_add(Buf* b, const char* s, size_t n) {
    if (n == 0) return 0;
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->len + n) cap *= 2;
        char* np = realloc(b->p, cap);
        if (!np) return 1;
        b->p = np; b->cap = cap;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    return 0;
}

/* the name of a value's type as scripts see it, for an error message */
static const char* type_label(uint32_t tag) {
    switch (tag) {
        case EMBR_TAG_INT: return "int";
        case EMBR_TAG_FLOAT: return "number";
        case EMBR_TAG_STRING: return "string";
        case EMBR_TAG_ARRAY: return "array";
        case EMBR_TAG_MAP: return "map";
        case EMBR_TAG_FUNCTION: return "function";
        default: return "pointer";
    }
}

/* ---- environment variables ---- */

static int os_getenv(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    size_t len;
    const char* name = embrp_str(ctx, &args[0], &len);
    const char* v = getenv(name);
    if (v) return embrp_set_str(ctx, out, v, strlen(v));
    if (n >= 2) return embrp_set_copy(ctx, out, &args[1]);
    char msg[300];
    snprintf(msg, sizeof msg, "environment variable '%.200s' is not set", name);
    return embrp_fail(ctx, msg);
}

static int os_has_env(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    size_t len; (void)n;
    return embrp_set_int(ctx, out, getenv(embrp_str(ctx, &args[0], &len)) ? 1 : 0);
}

static int os_setenv(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    size_t nl, vl; (void)n;
    const char* name = embrp_str(ctx, &args[0], &nl);
    const char* value = embrp_str(ctx, &args[1], &vl);
    if (nl == 0 || memchr(name, '=', nl) || memchr(name, 0, nl)) {
        char msg[300];
        snprintf(msg, sizeof msg, "invalid variable name '%.200s'", name);
        return embrp_fail(ctx, msg);
    }
#if defined(_WIN32)
    int bad = _putenv_s(name, value) != 0;
#else
    int bad = setenv(name, value, 1) != 0;
#endif
    if (bad) {
        char msg[300];
        snprintf(msg, sizeof msg, "cannot set '%.200s'", name);
        return embrp_fail(ctx, msg);
    }
    return embrp_set_float(ctx, out, 0.0);
}

static int os_unsetenv(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    size_t nl; (void)n;
    const char* name = embrp_str(ctx, &args[0], &nl);
    if (nl == 0 || memchr(name, '=', nl)) {
        char msg[300];
        snprintf(msg, sizeof msg, "invalid variable name '%.200s'", name);
        return embrp_fail(ctx, msg);
    }
#if defined(_WIN32)
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
    return embrp_set_float(ctx, out, 0.0);
}

static int os_environ(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    (void)args; (void)n;
    if (embrp_set_map(ctx, out)) return 1;
#if !defined(_WIN32)
    for (char** e = environ; e && *e; ++e) {
        const char* eq = strchr(*e, '=');
        if (!eq || eq == *e) continue;
        if (embrp_map_set_str(ctx, out, *e, (size_t)(eq - *e), eq + 1, strlen(eq + 1))) return 1;
    }
#else
    char* block = GetEnvironmentStringsA();
    if (block) {
        for (const char* e = block; *e; e += strlen(e) + 1) {
            const char* eq = strchr(e + 1, '=');   /* entries like "=C:=C:\" start with '=' */
            if (!eq) continue;
            if (embrp_map_set_str(ctx, out, e, (size_t)(eq - e), eq + 1, strlen(eq + 1))) { FreeEnvironmentStringsA(block); return 1; }
        }
        FreeEnvironmentStringsA(block);
    }
#endif
    return 0;
}

/* ---- the script's arguments, working folder, process id, platform, exit ---- */

static int need_stdio(EmbrCtx* ctx, const char* fn) {
    if (embrp_has_stdio(ctx)) return 0;
    char msg[120];
    snprintf(msg, sizeof msg, "%s needs a newer embr (this one has no host streams or script arguments)", fn);
    return embrp_fail(ctx, msg);
}

static int os_args(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    (void)args; (void)n;
    if (need_stdio(ctx, "os_args")) return 1;
    size_t count = embrp_script_arg_count(ctx);
    if (embrp_set_array(ctx, out, count)) return 1;
    for (size_t i = 0; i < count; ++i) {
        size_t len;
        const char* a = embrp_script_arg(ctx, i, &len);
        if (embrp_array_push_str(ctx, out, a ? a : "", a ? len : 0)) return 1;
    }
    return 0;
}

static int os_cwd(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    (void)args; (void)n;
    size_t cap = 256;
    for (;;) {
        char* buf = malloc(cap);
        if (!buf) return oom(ctx);
#if defined(_WIN32)
        char* r = _getcwd(buf, (int)cap);
#else
        char* r = getcwd(buf, cap);
#endif
        if (r) { int rc = embrp_set_str(ctx, out, buf, strlen(buf)); free(buf); return rc; }
        int e = errno;
        free(buf);
        if (e == ERANGE && cap < ((size_t)1 << 20)) { cap *= 4; continue; }
        return embrp_fail(ctx, strerror(e));
    }
}

static int os_pid(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    (void)args; (void)n;
#if defined(_WIN32)
    return embrp_set_int(ctx, out, (int64_t)_getpid());
#else
    return embrp_set_int(ctx, out, (int64_t)getpid());
#endif
}

static int os_platform(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    (void)args; (void)n;
#if defined(_WIN32)
    return embrp_set_str(ctx, out, "windows", 7);
#elif defined(__APPLE__)
    return embrp_set_str(ctx, out, "macos", 5);
#elif defined(__linux__)
    return embrp_set_str(ctx, out, "linux", 5);
#else
    return embrp_set_str(ctx, out, "unknown", 7);
#endif
}

static int os_exit(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    (void)out;
    int code = 0;
    if (n >= 1) {
        double d = embrp_to_float(ctx, &args[0]);
        if (!(d >= -2147483648.0 && d <= 2147483647.0)) return embrp_fail(ctx, "exit code out of range");
        code = (int)d;
    }
    if (embrp_has_stdio(ctx)) embrp_stdio_flush(ctx);
    fflush(stdout);
    exit(code);
}

/* ---- os_exec ---- */

typedef struct { char** v; size_t n; } Argv;

static void argv_free(Argv* a) { for (size_t i = 0; i < a->n; ++i) free(a->v[i]); free(a->v); a->v = NULL; a->n = 0; }

/* the program and its arguments from an array of strings. 0, or nonzero after the error was recorded */
static int argv_read(EmbrCtx* ctx, const EmbrValue* arr, Argv* a) {
    size_t count = embrp_array_len(ctx, arr);
    a->v = calloc(count + 1, sizeof(char*));
    a->n = 0;
    if (!a->v) return oom(ctx);
    for (size_t i = 0; i < count; ++i) {
        const EmbrValue* item = embrp_array_at(ctx, arr, i);
        if (embrp_type(ctx, item) != EMBR_TAG_STRING) {
            char msg[100];
            snprintf(msg, sizeof msg, "argv[%zu] must be a string, got %s", i, type_label(embrp_type(ctx, item)));
            argv_free(a);
            return embrp_fail(ctx, msg);
        }
        size_t len;
        const char* s = embrp_str(ctx, item, &len);
        a->v[i] = malloc(len + 1);
        if (!a->v[i]) { argv_free(a); return oom(ctx); }
        memcpy(a->v[i], s, len);
        a->v[i][len] = 0;
        a->n = i + 1;
    }
    if (count == 0) { argv_free(a); return embrp_fail(ctx, "argv must not be empty"); }
    return 0;
}

/* {code, stdout, stderr} */
static int exec_result(EmbrCtx* ctx, EmbrValue* out, int64_t code, const Buf* so, const Buf* se) {
    if (embrp_set_map(ctx, out)) return 1;
    EmbrValue* t = embrp_temp(ctx);
    if (embrp_set_int(ctx, t, code) || embrp_map_set(ctx, out, "code", 4, t)) return 1;
    if (embrp_map_set_str(ctx, out, "stdout", 6, so->p ? so->p : "", so->len)) return 1;
    return embrp_map_set_str(ctx, out, "stderr", 6, se->p ? se->p : "", se->len);
}

#if !defined(_WIN32)

static void fd_close(int* fd) { if (*fd >= 0) { close(*fd); *fd = -1; } }

/* runs argv (no shell), feeding `input` on stdin and collecting stdout and stderr together with poll(), so a child that
   fills either pipe can't deadlock us */
static int exec_process(EmbrCtx* ctx, char** argv, const char* input, size_t input_len, EmbrValue* out) {
    int in[2] = {-1, -1}, op[2] = {-1, -1}, er[2] = {-1, -1};
    int rc = 0;
    Buf so = {0, 0, 0}, se = {0, 0, 0};
    if (pipe(in) != 0 || pipe(op) != 0 || pipe(er) != 0) {
        fd_close(&in[0]); fd_close(&in[1]); fd_close(&op[0]); fd_close(&op[1]); fd_close(&er[0]); fd_close(&er[1]);
        return embrp_fail(ctx, "pipe failed");
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, in[0], 0);
    posix_spawn_file_actions_adddup2(&fa, op[1], 1);
    posix_spawn_file_actions_adddup2(&fa, er[1], 2);
    int all[6] = {in[0], in[1], op[0], op[1], er[0], er[1]};
    for (int k = 0; k < 6; ++k) posix_spawn_file_actions_addclose(&fa, all[k]);

    /* a write to a pipe whose reader exited must fail with EPIPE, not kill us */
    struct sigaction ign, old;
    memset(&ign, 0, sizeof ign);
    ign.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &ign, &old);

    if (embrp_has_stdio(ctx)) embrp_stdio_flush(ctx);
    fflush(stdout);
    pid_t pid;
    int sp = posix_spawnp(&pid, argv[0], &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (sp != 0) {
        sigaction(SIGPIPE, &old, NULL);
        fd_close(&in[0]); fd_close(&in[1]); fd_close(&op[0]); fd_close(&op[1]); fd_close(&er[0]); fd_close(&er[1]);
        char msg[400];
        snprintf(msg, sizeof msg, "cannot run '%.200s': %s", argv[0], strerror(sp));
        return embrp_fail(ctx, msg);
    }
    fd_close(&in[0]); fd_close(&op[1]); fd_close(&er[1]);

    size_t written = 0;
    if (input_len == 0) fd_close(&in[1]);
    /* non-blocking, so a write bigger than the pipe buffer returns part of it instead of blocking while the child waits
       for us to drain its output */
    else fcntl(in[1], F_SETFL, fcntl(in[1], F_GETFL) | O_NONBLOCK);
    char buf[4096];
    while (op[0] >= 0 || er[0] >= 0 || in[1] >= 0) {
        struct pollfd pfds[3];
        int np = 0, iO = -1, iE = -1, iI = -1;
        if (op[0] >= 0) { iO = np; pfds[np].fd = op[0]; pfds[np].events = POLLIN; pfds[np].revents = 0; ++np; }
        if (er[0] >= 0) { iE = np; pfds[np].fd = er[0]; pfds[np].events = POLLIN; pfds[np].revents = 0; ++np; }
        if (in[1] >= 0) { iI = np; pfds[np].fd = in[1]; pfds[np].events = POLLOUT; pfds[np].revents = 0; ++np; }
        if (poll(pfds, (nfds_t)np, -1) < 0) { if (errno == EINTR) continue; break; }
        for (int k = 0; k < 2; ++k) {
            int idx = k == 0 ? iO : iE;
            int* fd = k == 0 ? &op[0] : &er[0];
            Buf* dst = k == 0 ? &so : &se;
            if (idx < 0 || !(pfds[idx].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            ssize_t r = read(*fd, buf, sizeof buf);
            if (r > 0) { if (buf_add(dst, buf, (size_t)r)) rc = 1; }
            else if (r == 0 || errno != EINTR) fd_close(fd);
        }
        if (iI >= 0 && (pfds[iI].revents & (POLLOUT | POLLERR | POLLHUP))) {
            ssize_t w = write(in[1], input + written, input_len - written);
            if (w > 0) written += (size_t)w;
            if ((w < 0 && errno != EINTR && errno != EAGAIN) || written >= input_len) fd_close(&in[1]);
        }
        if (rc) break;
    }
    fd_close(&in[1]); fd_close(&op[0]); fd_close(&er[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    sigaction(SIGPIPE, &old, NULL);
    if (rc) { free(so.p); free(se.p); return oom(ctx); }

    /* the exit status, or -N if the child was killed by signal N */
    int64_t code = WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? -(int64_t)WTERMSIG(status) : -1;
    rc = exec_result(ctx, out, code, &so, &se);
    free(so.p); free(se.p);
    return rc;
}

#else   /* _WIN32 */

/* quotes one argument the way CommandLineToArgvW and the C runtime will split it back */
static int quote_arg(Buf* o, const char* a) {
    size_t n = strlen(a);
    if (n && !strpbrk(a, " \t\n\v\"")) return buf_add(o, a, n);
    if (buf_add(o, "\"", 1)) return 1;
    size_t slashes = 0;
    for (size_t i = 0; i < n; ++i) {
        char c = a[i];
        if (c == '\\') { ++slashes; continue; }
        for (size_t k = 0; k < (c == '"' ? slashes * 2 + 1 : slashes); ++k) if (buf_add(o, "\\", 1)) return 1;
        slashes = 0;
        if (buf_add(o, &c, 1)) return 1;
    }
    for (size_t k = 0; k < slashes * 2; ++k) if (buf_add(o, "\\", 1)) return 1;   /* trailing backslashes must not escape the closing quote */
    return buf_add(o, "\"", 1);
}

/* reads a pipe to the end on its own thread, so neither output pipe can fill up and block the child */
typedef struct { HANDLE pipe; Buf data; HANDLE thread; int failed; } Reader;

static DWORD WINAPI reader_main(LPVOID self) {
    Reader* r = (Reader*)self;
    char buf[4096];
    DWORD got;
    while (ReadFile(r->pipe, buf, sizeof buf, &got, NULL) && got > 0)
        if (buf_add(&r->data, buf, got)) { r->failed = 1; break; }
    return 0;
}
static void reader_start(Reader* r, HANDLE pipe) { r->pipe = pipe; r->thread = CreateThread(NULL, 0, reader_main, r, 0, NULL); }
static void reader_join(Reader* r) { if (r->thread) { WaitForSingleObject(r->thread, INFINITE); CloseHandle(r->thread); r->thread = NULL; } }
static void h_close(HANDLE* h) { if (*h) { CloseHandle(*h); *h = NULL; } }

static int exec_process(EmbrCtx* ctx, char** argv, const char* input, size_t input_len, EmbrValue* out) {
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof sa; sa.lpSecurityDescriptor = NULL; sa.bInheritHandle = TRUE;
    HANDLE inR = NULL, inW = NULL, outR = NULL, outW = NULL, errR = NULL, errW = NULL;
    if (!CreatePipe(&inR, &inW, &sa, 0) || !CreatePipe(&outR, &outW, &sa, 0) || !CreatePipe(&errR, &errW, &sa, 0)) {
        h_close(&inR); h_close(&inW); h_close(&outR); h_close(&outW); h_close(&errR); h_close(&errW);
        return embrp_fail(ctx, "pipe failed");
    }
    /* our ends must not be inherited by the child */
    SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errR, HANDLE_FLAG_INHERIT, 0);

    Buf cmd = {0, 0, 0};
    for (size_t i = 0; argv[i]; ++i) {
        if ((i && buf_add(&cmd, " ", 1)) || quote_arg(&cmd, argv[i])) { free(cmd.p); return oom(ctx); }
    }
    if (buf_add(&cmd, "", 1)) { free(cmd.p); return oom(ctx); }   /* the terminating 0 */

    STARTUPINFOA si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = inR; si.hStdOutput = outW; si.hStdError = errW;
    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof pi);
    if (embrp_has_stdio(ctx)) embrp_stdio_flush(ctx);
    fflush(stdout);
    /* lpApplicationName is NULL so the program is looked up on PATH (".exe" is added if missing) */
    if (!CreateProcessA(NULL, cmd.p, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        char msg[400];
        snprintf(msg, sizeof msg, "cannot run '%.200s': error %lu", argv[0], (unsigned long)GetLastError());
        free(cmd.p);
        h_close(&inR); h_close(&inW); h_close(&outR); h_close(&outW); h_close(&errR); h_close(&errW);
        return embrp_fail(ctx, msg);
    }
    free(cmd.p);
    HANDLE proc = pi.hProcess, thr = pi.hThread;
    h_close(&inR); h_close(&outW); h_close(&errW);

    Reader rOut, rErr;
    memset(&rOut, 0, sizeof rOut); memset(&rErr, 0, sizeof rErr);
    reader_start(&rOut, outR);
    reader_start(&rErr, errR);
    /* writing happens here. the child may exit without reading, which just makes WriteFile fail */
    size_t written = 0;
    while (written < input_len) {
        DWORD w = 0;
        size_t left = input_len - written;
        if (!WriteFile(inW, input + written, (DWORD)(left < 65536 ? left : 65536), &w, NULL) || w == 0) break;
        written += w;
    }
    h_close(&inW);
    reader_join(&rOut); reader_join(&rErr);
    WaitForSingleObject(proc, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(proc, &code);
    CloseHandle(proc); CloseHandle(thr);
    h_close(&outR); h_close(&errR);

    int rc = rOut.failed || rErr.failed ? oom(ctx) : exec_result(ctx, out, (int64_t)(int32_t)code, &rOut.data, &rErr.data);
    free(rOut.data.p); free(rErr.data.p);
    return rc;
}

#endif

static int os_exec(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    Argv a;
    if (argv_read(ctx, &args[0], &a)) return 1;
    size_t il = 0;
    const char* input = n >= 2 ? embrp_str(ctx, &args[1], &il) : "";
    int rc = exec_process(ctx, a.v, input, il, out);
    argv_free(&a);
    return rc;
}

/* ---- standard input and output ---- */

static int os_stdin_read(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    (void)n;
    if (need_stdio(ctx, "os_stdin_read")) return 1;
    int64_t want = embrp_to_int(ctx, &args[0]);
    if (want < 0 || want > (1 << 28)) return embrp_fail(ctx, "n out of range");
    char* buf = malloc((size_t)want + 1);
    if (!buf) return oom(ctx);
    size_t got = embrp_stdin_read(ctx, buf, (size_t)want);
    int rc = embrp_set_str(ctx, out, buf, got);
    free(buf);
    return rc;
}

static int os_stdin_read_all(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    (void)args; (void)n;
    if (need_stdio(ctx, "os_stdin_read_all")) return 1;
    Buf b = {0, 0, 0};
    char chunk[65536];
    for (;;) {
        size_t got = embrp_stdin_read(ctx, chunk, sizeof chunk);
        if (got && buf_add(&b, chunk, got)) { free(b.p); return oom(ctx); }
        if (got < sizeof chunk) break;
    }
    int rc = embrp_set_str(ctx, out, b.p ? b.p : "", b.len);
    free(b.p);
    return rc;
}

static int write_to(EmbrCtx* ctx, const char* fn, int fd, const EmbrValue* arg, EmbrValue* out) {
    if (need_stdio(ctx, fn)) return 1;
    size_t len;
    const char* d = embrp_str(ctx, arg, &len);
    if (embrp_stdio_write(ctx, fd, d, len)) return 1;
    return embrp_set_int(ctx, out, (int64_t)len);
}
static int os_stdout_write(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) { (void)n; return write_to(ctx, "os_stdout_write", 1, &args[0], out); }
static int os_stderr_write(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) { (void)n; return write_to(ctx, "os_stderr_write", 2, &args[0], out); }

static int os_stdout_flush(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    (void)args; (void)n;
    if (need_stdio(ctx, "os_stdout_flush")) return 1;
    if (embrp_stdio_flush(ctx)) return 1;
    return embrp_set_int(ctx, out, 0);
}

/* ---- the table ---- */

static const EmbrParam p_name[]    = { { "name", EMBR_T_STRING, 0 } };
static const EmbrParam p_getenv[]  = { { "name", EMBR_T_STRING, 0 }, { "default", EMBR_T_STRING, EMBR_PARAM_OPTIONAL } };
static const EmbrParam p_setenv[]  = { { "name", EMBR_T_STRING, 0 }, { "value", EMBR_T_STRING, 0 } };
static int os_script_dir(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    (void)args; (void)n;
    const char* d = embrp_script_dir(ctx);
    return embrp_set_str(ctx, out, d ? d : "", d ? strlen(d) : 0);
}

static const EmbrParam p_exit[]    = { { "code", EMBR_T_NUM, EMBR_PARAM_OPTIONAL } };
static const EmbrParam p_exec[]    = { { "argv", EMBR_T_ARRAY, 0 }, { "input", EMBR_T_STRING, EMBR_PARAM_OPTIONAL } };
static const EmbrParam p_n[]       = { { "n", EMBR_T_INT, 0 } };
static const EmbrParam p_data[]    = { { "data", EMBR_T_STRING, 0 } };

static const EmbrFunc funcs[] = {
    { "os_args", "os_args() -> arr of str. the arguments after a bare --: embr script.embr -- a b gives [\"a\", \"b\"] (embr pkg keeps its own argument handling; parallel worker interpreters see [])",
      os_args, NULL, NULL, 0, EMBR_T_ARRAY, 0, EMBR_T_STRING },
    { "os_cwd", "os_cwd() -> str. the working folder",
      os_cwd, NULL, NULL, 0, EMBR_T_STRING, 0, 0 },
    { "os_script_dir", "os_script_dir() -> str. the absolute folder of the script (or module) that is running, \"\" when there is none (the REPL). call it at the top level: inside a function that runs later it is the folder of whichever script is running then",
      os_script_dir, NULL, NULL, 0, EMBR_T_STRING, 0, 0 },
    { "os_environ", "os_environ() -> map. a snapshot of all variables",
      os_environ, NULL, NULL, 0, EMBR_T_MAP, 0, 0 },
    { "os_exec", "os_exec(argv: arr, input?: str) -> map. runs a program directly (PATH lookup, no shell), feeds input to stdin, returns {code, stdout, stderr}; code is -N if killed by signal N (POSIX only, Windows reports the plain exit code); a program that can't start raises, one that exits nonzero does not. stdout, stderr and stdin are pumped concurrently (no deadlock on large data). on Windows it uses CreateProcess, so argv[0] must be a real program (cmd, findstr), not a shell builtin or sh",
      os_exec, NULL, p_exec, 2, EMBR_T_ANY, 0, 0 },
    { "os_exit", "os_exit(code?: num). flushes stdout then ends the process; not catchable by try/catch",
      os_exit, NULL, p_exit, 1, EMBR_T_ANY, 0, 0 },
    { "os_getenv", "os_getenv(name: str, default?: str) -> str. raises if unset and no default (there is no nil core type, see the type system)",
      os_getenv, NULL, p_getenv, 2, EMBR_T_ANY, 0, 0 },
    { "os_has_env", "os_has_env(name: str) -> int. 1 if the environment variable is set, else 0",
      os_has_env, NULL, p_name, 1, EMBR_T_INT, 0, 0 },
    { "os_pid", "os_pid() -> int. the process id",
      os_pid, NULL, NULL, 0, EMBR_T_INT, 0, 0 },
    { "os_platform", "os_platform() -> str. \"linux\", \"macos\", \"windows\" or \"unknown\"",
      os_platform, NULL, NULL, 0, EMBR_T_STRING, 0, 0 },
    { "os_setenv", "os_setenv(name: str, value: str) -> num. affects this process and later os_exec children; invalid names (empty, containing =) raise",
      os_setenv, NULL, p_setenv, 2, EMBR_T_FLOAT, 0, 0 },
    { "os_stderr_write", "os_stderr_write(data: str) -> int. raw bytes to stderr, returns the byte count; flushes stdout first so the order holds",
      os_stderr_write, NULL, p_data, 1, EMBR_T_INT, 0, 0 },
    { "os_stdin_read", "os_stdin_read(n: int) -> str. up to n raw bytes from stdin; waits for n, so a shorter answer means end of input and \"\" means nothing left",
      os_stdin_read, NULL, p_n, 1, EMBR_T_STRING, 0, 0 },
    { "os_stdin_read_all", "os_stdin_read_all() -> str. everything left on stdin",
      os_stdin_read_all, NULL, NULL, 0, EMBR_T_STRING, 0, 0 },
    { "os_stdout_flush", "os_stdout_flush() -> int. flushes stdout",
      os_stdout_flush, NULL, NULL, 0, EMBR_T_INT, 0, 0 },
    { "os_stdout_write", "os_stdout_write(data: str) -> int. raw bytes to stdout, no newline added, returns the byte count; shares the stream with print()",
      os_stdout_write, NULL, p_data, 1, EMBR_T_INT, 0, 0 },
    { "os_unsetenv", "os_unsetenv(name: str) -> num. affects this process and later os_exec children; invalid names (empty, containing =) raise",
      os_unsetenv, NULL, p_name, 1, EMBR_T_FLOAT, 0, 0 },
};

static const EmbrPluginInfo info = {
    EMBR_PLUGIN_ABI_VERSION, sizeof(EmbrPluginInfo), "os", "1.2.0", "environment, arguments, process",
    EMBR_CAP_PROCESS | EMBR_CAP_ENV, funcs, sizeof funcs / sizeof funcs[0], NULL, 0, NULL, NULL
};

EMBR_PLUGIN_EXPORT const EmbrPluginInfo* embr_plugin_info(void) { return &info; }
