/* fs.c
 * filesystem plugin for embr: existence and type checks, directory listing, mkdir and remove, and lexical path helpers
 * (join/basename/dirname/extname). written in C against embr_plugin.h
 *
 * usage
 *   import "fs"
 *   fs_mkdir("out")
 *   write_file("out/report.txt", "...")   # embrlib, not this plugin
 *   print(fs_list_dir("out"))
 *   print(fs_join("out", "report.txt"))
 *
 * good to know
 *   - a relative path resolves the way embrlib's write_file() does: against the folder of the running script, or the
 *     process's working folder when there is none (the REPL). there is no "search" like load_file() does, because most
 *     calls here (mkdir, remove) point at a path that may not exist yet
 *   - the answers, and the paths in the error messages, are the ones std::filesystem gave (the C++ version of this plugin
 *     was built on it), compared on generated paths. the path_* functions of the path plugin are the better lexical ones;
 *     these predate it and are kept as they were
 */
#define _POSIX_C_SOURCE 200809L   /* strdup */
#include <embr_plugin.h>

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(_WIN32)
#  include <direct.h>
#  define IS_SEP(c) ((c) == '/' || (c) == '\\')
#  define MKDIR(p) _mkdir(p)
#else
#  define IS_SEP(c) ((c) == '/')
#  define MKDIR(p) mkdir((p), 0777)
#endif

/* ---- paths, the way std::filesystem reads them: an optional drive, an optional root, then names ---- */

typedef struct {
    const char* s;
    size_t n;
    size_t drive;     /* length of "C:" (Windows only), else 0 */
    int    root;      /* there is a root directory */
    size_t rel;       /* where the names start */
} Path;

static void path_parse(Path* p, const char* s, size_t n) {
    p->s = s; p->n = n; p->drive = 0; p->root = 0;
#if defined(_WIN32)
    if (n >= 2 && s[1] == ':' && ((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z'))) p->drive = 2;
#endif
    size_t i = p->drive;
    while (i < n && IS_SEP(s[i])) { p->root = 1; ++i; }
    p->rel = i;
}

/* where the last name starts. == n when the path ends with a separator or is only a root (no file name, like std::filesystem) */
static size_t path_filename(const Path* p) {
    size_t i = p->n;
    while (i > p->rel && !IS_SEP(p->s[i - 1])) --i;
    return i;
}

static size_t path_ext_start(const char* s, size_t b, size_t n) {
    size_t len = n - b;
    if (len == 0 || (len == 1 && s[b] == '.') || (len == 2 && s[b] == '.' && s[b + 1] == '.')) return n;
    size_t i = n;
    while (i > b && s[i - 1] != '.') --i;
    if (i == b || i == b + 1) return n;
    return i - 1;
}

/* writes the names of a path from `from` on with one '/' between them, and a '/' at the end if it ends with a separator */
static size_t put_names(char* buf, size_t at, const char* s, size_t from, size_t n) {
    size_t i = from;
    int first = 1;
    while (i < n) {
        while (i < n && IS_SEP(s[i])) ++i;
        size_t b = i;
        while (i < n && !IS_SEP(s[i])) ++i;
        if (i == b) break;
        if (!first) buf[at++] = '/';
        memcpy(buf + at, s + b, i - b); at += i - b;
        first = 0;
    }
    if (!first && n > from && IS_SEP(s[n - 1])) buf[at++] = '/';
    return at;
}

/* the same path with one separator between names, written over itself (it never gets longer) */
static size_t path_canon_keep(char* buf, size_t at, const Path* p, int keepRoot) {
    size_t head = p->drive + (p->root ? (keepRoot ? p->rel - p->drive : 1) : 0);
    if (p->root && !keepRoot) buf[p->drive] = '/';
    char small[256];
    char* tmp = at <= sizeof small ? small : (char*)malloc(at);
    if (!tmp) return at;
    memcpy(tmp, buf, at);
    size_t w = put_names(buf, head, tmp, p->rel, at);
    if (tmp != small) free(tmp);
    return w;
}

/* buf[0..at) /= s[0..l), like std::filesystem::path::operator/=: the right side is added as it is (a separator in front
   when the left side ends in a name), an absolute right side replaces everything. buf must have room for at + l + 2 bytes */
static size_t path_append(char* buf, size_t at, const char* s, size_t l) {
    Path b; path_parse(&b, s, l);
    Path a; path_parse(&a, buf, at);
    if (b.root) {
        size_t keep = (b.drive == 0 && a.drive) ? a.drive : 0;      /* "C:" stays when the right side has no drive of its own */
        memmove(buf + keep, s, l);                                  /* the drive, if kept, is already in place */
        return keep + l;
    }
    if (b.drive && !(a.drive == b.drive && memcmp(buf, s, 2) == 0)) { memmove(buf, s, l); return l; }   /* another drive replaces it */
    if (at > a.rel && !IS_SEP(buf[at - 1])) buf[at++] = '/';
    size_t skip = b.drive;                                           /* the same drive again is not repeated */
    memcpy(buf + at, s + skip, l - skip);
    return at + l - skip;
}

static int is_absolute(const char* s, size_t l) {
    Path p; path_parse(&p, s, l);
#if defined(_WIN32)
    return p.drive && p.root;
#else
    return p.root;
#endif
}

/* ---- where a relative path points ---- */

/* the full path of `raw`: as it is when absolute, else the script's folder (or the working folder) joined with it. malloc'd, NULL after an error */
static char* resolve(EmbrCtx* ctx, const char* raw, size_t rl, size_t* outlen) {
    if (is_absolute(raw, rl)) {
        char* r = (char*)malloc(rl + 1);
        if (!r) { embrp_fail(ctx, "out of memory"); return NULL; }
        memcpy(r, raw, rl); r[rl] = 0;
        *outlen = rl;
        return r;
    }
    const char* dir = embrp_script_dir(ctx);
    char cwd[4096];
    if (!dir || !*dir) {
        if (!getcwd(cwd, sizeof cwd)) { embrp_fail(ctx, "cannot read the working folder"); return NULL; }
        dir = cwd;
    }
    size_t dl = strlen(dir);
    char* r = (char*)malloc(dl + rl + 4);
    if (!r) { embrp_fail(ctx, "out of memory"); return NULL; }
    memcpy(r, dir, dl);
    size_t at = path_append(r, dl, raw, rl);
    r[at] = 0;
    *outlen = at;
    return r;
}

static int failp(EmbrCtx* ctx, const char* what, const char* path, const char* why) {
    size_t n = strlen(what) + strlen(path) + strlen(why) + 16;
    char* m = (char*)malloc(n);
    if (!m) return embrp_fail(ctx, "out of memory");
    snprintf(m, n, "%s'%s': %s", what, path, why);
    int rc = embrp_fail(ctx, m);
    free(m);
    return rc;
}

/* ---- file checks ---- */

static int fs_exists(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    (void)n;
    size_t rl, pl; const char* raw = embrp_str(ctx, &args[0], &rl);
    char* p = resolve(ctx, raw, rl, &pl);
    if (!p) return 1;
    struct stat st;
    int ok = stat(p, &st) == 0;
    free(p);
    return embrp_set_int(ctx, out, ok ? 1 : 0);
}

static int fs_is_dir(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    (void)n;
    size_t rl, pl; const char* raw = embrp_str(ctx, &args[0], &rl);
    char* p = resolve(ctx, raw, rl, &pl);
    if (!p) return 1;
    struct stat st;
    int ok = stat(p, &st) == 0 && S_ISDIR(st.st_mode);
    free(p);
    return embrp_set_int(ctx, out, ok ? 1 : 0);
}

static int name_cmp(const void* a, const void* b) {
    const char* x = *(const char* const*)a;
    const char* y = *(const char* const*)b;
    return strcmp(x, y);   /* byte order, like comparing std::strings */
}

static int fs_list_dir(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    (void)n;
    size_t rl, pl; const char* raw = embrp_str(ctx, &args[0], &rl);
    char* p = resolve(ctx, raw, rl, &pl);
    if (!p) return 1;
    struct stat st;
    if (stat(p, &st) != 0 || !S_ISDIR(st.st_mode)) {
        size_t m = pl + 32;
        char* msg = (char*)malloc(m);
        int rc = msg ? (snprintf(msg, m, "not a directory: %s", p), embrp_fail(ctx, msg)) : embrp_fail(ctx, "out of memory");
        free(msg); free(p);
        return rc;
    }
    DIR* d = opendir(p);
    if (!d) { int rc = failp(ctx, "cannot list ", p, strerror(errno)); free(p); return rc; }
    size_t cap = 64, cnt = 0;
    char** names = (char**)malloc(cap * sizeof(char*));
    int rc = 0;
    struct dirent* e;
    while (names && (e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.' && (e->d_name[1] == 0 || (e->d_name[1] == '.' && e->d_name[2] == 0))) continue;
        if (cnt == cap) {
            char** bigger = (char**)realloc(names, cap * 2 * sizeof(char*));
            if (!bigger) { rc = embrp_fail(ctx, "out of memory"); break; }
            names = bigger; cap *= 2;
        }
        names[cnt] = strdup(e->d_name);
        if (!names[cnt]) { rc = embrp_fail(ctx, "out of memory"); break; }
        ++cnt;
    }
    closedir(d);
    if (!names) rc = embrp_fail(ctx, "out of memory");
    if (!rc) {
        qsort(names, cnt, sizeof(char*), name_cmp);
        rc = embrp_set_array(ctx, out, cnt);
        for (size_t i = 0; !rc && i < cnt; ++i) rc = embrp_array_push_str(ctx, out, names[i], strlen(names[i]));
    }
    for (size_t i = 0; i < cnt; ++i) free(names[i]);
    free(names); free(p);
    return rc;
}

/* ---- changes ---- */

static int is_dir_path(const char* p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }

/* creates one folder. 1 created, 0 it was already a folder, -1 error (errno set) */
static int make_one(const char* p) {
    if (MKDIR(p) == 0) return 1;
    int err = errno;
    if (err == EEXIST && is_dir_path(p)) return 0;
    errno = err;
    return -1;
}

/* like std::filesystem::create_directories. 1 created, 0 it already was a folder, -1 error (errno set) */
static int make_all(const char* p, size_t n) {
    struct stat st;
    if (stat(p, &st) == 0) {
        if (S_ISDIR(st.st_mode)) return 0;
        errno = ENOTDIR;
        return -1;
    }
    Path pp; path_parse(&pp, p, n);
    /* the parent first: everything before the last name (a trailing separator is not a name) */
    size_t end = n;
    while (end > pp.rel && IS_SEP(p[end - 1])) --end;
    size_t f = end;
    while (f > pp.rel && !IS_SEP(p[f - 1])) --f;
    size_t pe = f;
    while (pe > pp.rel && IS_SEP(p[pe - 1])) --pe;
    if (pe > pp.rel) {                      /* there is a parent to make */
        char* parent = (char*)malloc(pe + 1);
        if (!parent) { errno = ENOMEM; return -1; }
        memcpy(parent, p, pe); parent[pe] = 0;
        int r = make_all(parent, pe);
        free(parent);
        if (r < 0) return -1;
    }
    return make_one(p);
}

static int fs_mkdir(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    size_t rl, pl; const char* raw = embrp_str(ctx, &args[0], &rl);
    int recursive = n >= 2 && embrp_truthy(ctx, &args[1]);
    char* p = resolve(ctx, raw, rl, &pl);
    if (!p) return 1;
    int r = recursive ? make_all(p, pl) : make_one(p);
    if (r < 0) { int rc = failp(ctx, "cannot create ", p, strerror(errno)); free(p); return rc; }
    free(p);
    return embrp_set_int(ctx, out, r == 1 ? 1 : 0);
}

static int fs_remove(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    (void)n;
    size_t rl, pl; const char* raw = embrp_str(ctx, &args[0], &rl);
    char* p = resolve(ctx, raw, rl, &pl);
    if (!p) return 1;
    int ok;
#if defined(_WIN32)
    ok = (is_dir_path(p) ? rmdir(p) : unlink(p)) == 0;
#else
    ok = remove(p) == 0;
#endif
    if (!ok && errno != ENOENT) { int rc = failp(ctx, "cannot remove ", p, strerror(errno)); free(p); return rc; }
    free(p);
    return embrp_set_int(ctx, out, ok ? 1 : 0);
}

/* ---- lexical helpers ---- */

static int give(EmbrCtx* ctx, EmbrValue* out, const char* s, size_t n) {
#if defined(_WIN32)
    if (memchr(s, '\\', n)) {       /* the result uses '/' */
        char* w = embrp_set_str_alloc(ctx, out, n);
        if (!w) return 1;
        for (size_t i = 0; i < n; ++i) w[i] = s[i] == '\\' ? '/' : s[i];
        return 0;
    }
#endif
    return embrp_set_str(ctx, out, s, n);
}

static int fs_join(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    if (n == 0) return embrp_fail(ctx, "expects at least one argument");
    size_t cap = 0;
    for (size_t i = 0; i < n; ++i) { size_t l; embrp_str(ctx, &args[i], &l); cap += l + 2; }
    char stackbuf[512];
    char* buf = cap <= sizeof stackbuf ? stackbuf : (char*)malloc(cap);
    if (!buf) return embrp_fail(ctx, "out of memory");
    size_t at = 0;
    int keepRoot = 0;
    for (size_t i = 0; i < n; ++i) {
        size_t l; const char* s = embrp_str(ctx, &args[i], &l);
        if (i == 0) { memcpy(buf, s, l); at = l; }
        else {
            Path a, b; path_parse(&a, buf, at); path_parse(&b, s, l);
            if (!b.root && l > 0 && a.root && a.rel == at) keepRoot = 1;   /* "//" /= "x" stays "//x" */
            at = path_append(buf, at, s, l);
        }
    }
    { Path c; path_parse(&c, buf, at); at = path_canon_keep(buf, at, &c, keepRoot); }   /* the answer has one separator between names, like generic_string() */
    int rc = at ? give(ctx, out, buf, at) : embrp_set_str(ctx, out, "", 0);
    if (buf != stackbuf) free(buf);
    return rc;
}

static int fs_basename(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    (void)n;
    size_t len; const char* s = embrp_str(ctx, &args[0], &len);
    Path p; path_parse(&p, s, len);
    size_t f = path_filename(&p);
    return give(ctx, out, s + f, len - f);
}

static int fs_extname(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    (void)n;
    size_t len; const char* s = embrp_str(ctx, &args[0], &len);
    Path p; path_parse(&p, s, len);
    size_t f = path_filename(&p);
    size_t e = path_ext_start(s, f, len);
    return give(ctx, out, s + e, len - e);
}

/* the path without its last element, rebuilt from the elements: "a/b" -> "a", "a/b/" -> "a/b", "/a" -> "/", "a" -> "" */
static int fs_dirname(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    (void)n;
    size_t len; const char* s = embrp_str(ctx, &args[0], &len);
    Path p; path_parse(&p, s, len);
    if (p.rel >= len) {                                        /* only a root, or nothing: it is its own parent */
        char root[4]; size_t rn = p.drive;
        memcpy(root, s, p.drive); if (p.root) root[rn++] = '/';
        return give(ctx, out, root, rn);
    }
    size_t f = path_filename(&p);                              /* == len when it ends with a separator */
    char stackbuf[512];
    char* buf = len + 2 <= sizeof stackbuf ? stackbuf : (char*)malloc(len + 2);
    if (!buf) return embrp_fail(ctx, "out of memory");
    size_t at = p.drive;
    memcpy(buf, s, p.drive);
    if (p.root) buf[at++] = '/';
    at = put_names(buf, at, s, p.rel, f);                     /* the names before the last one (all of them when it ends with a separator) */
    while (at > p.drive + (p.root ? 1u : 0u) && buf[at - 1] == '/') --at;   /* no separator at the end, except a root */
    int rc = give(ctx, out, buf, at);
    if (buf != stackbuf) free(buf);
    return rc;
}

/* ---- the table ---- */

static const EmbrParam p_path[]  = { { "path", EMBR_T_STRING, 0 } };
static const EmbrParam p_mkdir[] = { { "path", EMBR_T_STRING, 0 }, { "recursive", EMBR_T_NUM, EMBR_PARAM_OPTIONAL } };
static const EmbrParam p_join[]  = { { "parts", EMBR_T_STRING, EMBR_PARAM_OPTIONAL | EMBR_PARAM_VARIADIC } };

static const EmbrFunc funcs[] = {
    { "fs_basename", "pure path-string ops, no filesystem access; results use / on every platform",
      fs_basename, NULL, p_path, 1, EMBR_T_STRING, 0, 0 },
    { "fs_dirname", "pure path-string ops, no filesystem access; results use / on every platform",
      fs_dirname, NULL, p_path, 1, EMBR_T_STRING, 0, 0 },
    { "fs_exists", "1/0, OS errors count as \"doesn't exist\"",
      fs_exists, NULL, p_path, 1, EMBR_T_INT, 0, 0 },
    { "fs_extname", "pure path-string ops, no filesystem access; results use / on every platform",
      fs_extname, NULL, p_path, 1, EMBR_T_STRING, 0, 0 },
    { "fs_is_dir", "1 if path is a directory, else 0",
      fs_is_dir, NULL, p_path, 1, EMBR_T_INT, 0, 0 },
    { "fs_join", "variadic; always joins with /, also on Windows; raises with zero args",
      fs_join, NULL, p_join, 1, EMBR_T_STRING, 0, 0 },
    { "fs_list_dir", "entry names (not full paths), alphabetically sorted; raises if not a directory",
      fs_list_dir, NULL, p_path, 1, EMBR_T_ARRAY, 0, EMBR_T_STRING },
    { "fs_mkdir", "non-recursive = create_directory; recursive = create_directories (mkdir -p); returns 1 if newly created",
      fs_mkdir, NULL, p_mkdir, 2, EMBR_T_INT, 0, 0 },
    { "fs_remove", "a single file or an empty directory only. it's deliberately not recursive or forced, so it never silently deletes a directory's contents, and it raises on a non-empty directory",
      fs_remove, NULL, p_path, 1, EMBR_T_INT, 0, 0 },
};

static const EmbrPluginInfo info = {
    EMBR_PLUGIN_ABI_VERSION, sizeof(EmbrPluginInfo), "fs", "1.0.0",
    "files and directories: exists, list, make, remove, and lexical path helpers", EMBR_CAP_FILESYSTEM, funcs, sizeof funcs / sizeof funcs[0], NULL, 0, NULL,
    NULL
};

EMBR_PLUGIN_EXPORT const EmbrPluginInfo* embr_plugin_info(void) { return &info; }
