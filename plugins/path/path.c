/* path.c
 * pure path-string utilities for embr. nothing here touches the filesystem: every function is a lexical change of its
 * string arguments, so a path need not exist. written in C against embr_plugin.h
 *
 * usage
 *   import "path"
 *   print(path_join("a", "b", "c.txt"))     # a/b/c.txt
 *   print(path_normalize("a/./b/../c"))     # a/c
 *   print(path_basename("/x/y.tar.gz"))     # y.tar.gz
 *   print(path_stem("/x/y.tar.gz"))         # y.tar
 *   print(path_extname("/x/y.tar.gz"))      # .gz
 *   print(path_dirname("/x/y.tar.gz"))      # /x
 *   print(path_is_abs("/x"))                # 1
 *
 * good to know
 *   - the separators are '/' everywhere and also '\' on Windows, and the result always uses '/'. a Windows path may
 *     start with a drive ("C:"), which is kept as it is and is never removed by ".."
 *   - the answers are the ones std::filesystem gave (the C++ version of this plugin was built on it), and were compared
 *     with it on generated paths. it is much faster because it does not build a path object
 *   - fs also exposes fs_join/fs_basename/fs_dirname/fs_extname; those predate this plugin and are left as they are
 *     (plugins have no runtime dependency on each other), prefer path_* in new code
 */
#include <embr_plugin.h>

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  define IS_SEP(c) ((c) == '/' || (c) == '\\')
#else
#  define IS_SEP(c) ((c) == '/')
#endif

/* a path split the way std::filesystem sees it: an optional drive, an optional root directory, then the rest */
typedef struct {
    const char* s;
    size_t n;
    size_t drive;     /* length of "C:" (Windows only), else 0 */
    int    root;      /* there is a root directory */
    size_t rel;       /* where the names start (after the drive and the root separators) */
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

/* the last name, and where it starts. a path that ends in a separator has no file name (like std::filesystem) */
static size_t path_filename(const Path* p) {
    size_t i = p->n;
    while (i > p->rel && !IS_SEP(p->s[i - 1])) --i;
    return i;   /* == n when the path ends with a separator or is only a root */
}

/* the extension of the name at [b, n): from the last '.', but not for ".", ".." or a name that starts with the dot */
static size_t path_ext_start(const char* s, size_t b, size_t n) {
    size_t len = n - b;
    if (len == 0 || (len == 1 && s[b] == '.') || (len == 2 && s[b] == '.' && s[b + 1] == '.')) return n;
    size_t i = n;
    while (i > b && s[i - 1] != '.') --i;
    if (i == b || i == b + 1) return n;   /* no dot, or the dot is the first character */
    return i - 1;
}

/* ---- output ---- */

static int give(EmbrCtx* ctx, EmbrValue* out, const char* s, size_t n) {
#if defined(_WIN32)
    /* the result uses '/' (generic_string) */
    if (memchr(s, '\\', n)) {
        char* w = embrp_set_str_alloc(ctx, out, n);
        if (!w) return 1;
        for (size_t i = 0; i < n; ++i) w[i] = s[i] == '\\' ? '/' : s[i];
        return 0;
    }
#endif
    return embrp_set_str(ctx, out, s, n);
}

/* ---- functions ---- */

/* the path without its last name, rebuilt from its elements (so repeated separators between names become one, like
   std::filesystem). "a/b" -> "a", "/a" -> "/", "a" -> "", "a/b/" -> "a" (the trailing separator is not a name) */
static int path_dirname(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    (void)n;
    size_t len; const char* s = embrp_str(ctx, &args[0], &len);
    Path p; path_parse(&p, s, len);
    size_t end = len;
    while (end > p.rel && IS_SEP(s[end - 1])) --end;        /* a trailing separator is not a name */
    size_t f = end;                                        /* where the last name starts */
    while (f > p.rel && !IS_SEP(s[f - 1])) --f;
    char stackbuf[512];
    char* buf = f + 2 <= sizeof stackbuf ? stackbuf : (char*)malloc(f + 2);
    if (!buf) return embrp_fail(ctx, "path_dirname: out of memory");
    size_t at = 0;
    memcpy(buf, s, p.drive); at = p.drive;
    if (p.root) buf[at++] = '/';
    size_t i = p.rel;
    int first = 1;
    while (i < f) {                                         /* the names before the last one */
        while (i < f && IS_SEP(s[i])) ++i;
        size_t b = i;
        while (i < f && !IS_SEP(s[i])) ++i;
        if (i == b) break;
        if (!first) buf[at++] = '/';
        memcpy(buf + at, s + b, i - b); at += i - b;
        first = 0;
    }
    int rc = give(ctx, out, buf, at);
    if (buf != stackbuf) free(buf);
    return rc;
}

static int path_basename(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    (void)n;
    size_t len; const char* s = embrp_str(ctx, &args[0], &len);
    Path p; path_parse(&p, s, len);
    size_t end = len;
    if (path_filename(&p) == len && p.rel < len) while (end > p.rel && IS_SEP(s[end - 1])) --end;   /* "a/b/" -> "b" */
    size_t f = end;
    while (f > p.rel && !IS_SEP(s[f - 1])) --f;
    return give(ctx, out, s + f, end - f);
}

static int path_extname(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    (void)n;
    size_t len; const char* s = embrp_str(ctx, &args[0], &len);
    Path p; path_parse(&p, s, len);
    size_t f = path_filename(&p);
    size_t e = path_ext_start(s, f, len);
    return give(ctx, out, s + e, len - e);
}

static int path_stem(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    (void)n;
    size_t len; const char* s = embrp_str(ctx, &args[0], &len);
    Path p; path_parse(&p, s, len);
    size_t f = path_filename(&p);
    size_t e = path_ext_start(s, f, len);
    return give(ctx, out, s + f, e - f);
}

static int path_is_abs(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    (void)n;
    size_t len; const char* s = embrp_str(ctx, &args[0], &len);
    int abs = len > 0 && IS_SEP(s[0]);   /* a leading separator counts as absolute on every platform */
#if defined(_WIN32)
    { Path p; path_parse(&p, s, len); if (p.drive && p.root) abs = 1; }
#else
    (void)abs;
#endif
    return embrp_set_int(ctx, out, abs ? 1 : 0);
}

/* ---- join ---- */

/* writes the names of a path from `from` on, one '/' between them (repeated separators become one, like std::filesystem),
   and a '/' at the end if the path ends with a separator. returns the new length of buf */
static size_t path_put_names(char* buf, size_t at, const char* s, size_t from, size_t n) {
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
    size_t w = path_put_names(buf, head, tmp, p->rel, at);
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
        memmove(buf + keep, s, l);
        return keep + l;
    }
    if (b.drive && !(a.drive == b.drive && memcmp(buf, s, 2) == 0)) { memmove(buf, s, l); return l; }   /* another drive replaces it */
    if (at > a.rel && !IS_SEP(buf[at - 1])) buf[at++] = '/';
    size_t skip = b.drive;                                           /* the same drive again is not repeated */
    memcpy(buf + at, s + skip, l - skip);
    return at + l - skip;
}

static int path_join(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    size_t cap = 0;
    for (size_t i = 0; i < n; ++i) { size_t l; embrp_str(ctx, &args[i], &l); cap += l + 2; }
    char stackbuf[512];
    char* buf = cap <= sizeof stackbuf ? stackbuf : (char*)malloc(cap);
    if (!buf) return embrp_fail(ctx, "path_join: out of memory");
    size_t at = 0;
    int keepRoot = 0;       /* "//" /= "x" stays "//x" (std::filesystem keeps the left root as it was written) */
    for (size_t i = 0; i < n; ++i) {
        size_t l; const char* s = embrp_str(ctx, &args[i], &l);
        if (i == 0) { memcpy(buf, s, l); at = l; }
        else if (l > 0) {
            Path a, b; path_parse(&a, buf, at); path_parse(&b, s, l);
            if (!b.root && a.root && a.rel == at) keepRoot = 1;
            at = path_append(buf, at, s, l);
        }
    }
    { Path c; path_parse(&c, buf, at); at = path_canon_keep(buf, at, &c, keepRoot); }   /* one separator between names, like generic_string() */
    int rc = at ? give(ctx, out, buf, at) : embrp_set_str(ctx, out, "", 0);
    if (buf != stackbuf) free(buf);
    return rc;
}

/* ---- normalize ---- */

typedef struct { size_t b, e; } Seg;

/* collapses ".", ".." and repeated separators. the result is built in `buf` (at least n + 2 bytes). returns its length */
static size_t path_norm(const Path* p, char* buf, Seg* segs) {
    size_t top = 0;
    const char* s = p->s;
    size_t i = p->rel;
    while (i < p->n) {
        while (i < p->n && IS_SEP(s[i])) ++i;
        size_t b = i;
        while (i < p->n && !IS_SEP(s[i])) ++i;
        if (i == b) break;
        size_t l = i - b;
        if (l == 1 && s[b] == '.') continue;
        if (l == 2 && s[b] == '.' && s[b + 1] == '.') {
            if (top > 0 && !(segs[top - 1].e - segs[top - 1].b == 2 && s[segs[top - 1].b] == '.' && s[segs[top - 1].b + 1] == '.')) { --top; continue; }
            if (p->root) continue;     /* ".." at the root goes nowhere */
        }
        segs[top].b = b; segs[top].e = i; ++top;
    }
    size_t at = 0;
    memcpy(buf, s, p->drive); at = p->drive;
    if (p->root) buf[at++] = '/';
    for (size_t k = 0; k < top; ++k) {
        if (k > 0) buf[at++] = '/';
        memcpy(buf + at, s + segs[k].b, segs[k].e - segs[k].b);
        at += segs[k].e - segs[k].b;
    }
    if (top == 0 && !p->root) { if (p->drive == 0) buf[at++] = '.'; }
    return at;
}

static int path_normalize(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    (void)n;
    size_t len; const char* s = embrp_str(ctx, &args[0], &len);
    if (len == 0) return embrp_set_str(ctx, out, ".", 1);
    Path p; path_parse(&p, s, len);
    char stackbuf[512];
    Seg segstack[64];
    size_t maxSegs = len / 2 + 2;
    char* buf = len + 4 <= sizeof stackbuf ? stackbuf : (char*)malloc(len + 4);
    Seg* segs = maxSegs <= 64 ? segstack : (Seg*)malloc(maxSegs * sizeof(Seg));
    if (!buf || !segs) { if (buf != stackbuf) free(buf); if (segs != segstack) free(segs); return embrp_fail(ctx, "path_normalize: out of memory"); }
    size_t at = path_norm(&p, buf, segs);
    int rc = give(ctx, out, buf, at);
    if (buf != stackbuf) free(buf);
    if (segs != segstack) free(segs);
    return rc;
}

/* ---- relative ---- */

/* the elements std::filesystem iterates: a drive, the root directory, each name, and an empty one after a trailing separator */
typedef struct { const char* s; size_t l; } Elem;

static size_t path_elems(const Path* p, Elem* el) {
    size_t k = 0;
    if (p->drive) { el[k].s = p->s; el[k].l = p->drive; ++k; }
    if (p->root) { el[k].s = "/"; el[k].l = 1; ++k; }
    size_t i = p->rel;
    while (i < p->n) {
        size_t b = i;
        while (i < p->n && !IS_SEP(p->s[i])) ++i;
        el[k].s = p->s + b; el[k].l = i - b; ++k;
        if (i < p->n) {
            while (i < p->n && IS_SEP(p->s[i])) ++i;
            if (i == p->n) { el[k].s = ""; el[k].l = 0; ++k; }   /* ends with a separator */
        }
    }
    return k;
}

static int elem_eq(const Elem* a, const Elem* b) {
    if (a->l != b->l) return 0;
    if (a->l == 1 && a->s[0] != b->s[0]) return IS_SEP(a->s[0]) && IS_SEP(b->s[0]);
    return memcmp(a->s, b->s, a->l) == 0;
}

static int path_relative(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    (void)n;
    size_t pl, bl; const char* ps = embrp_str(ctx, &args[0], &pl); const char* bs = embrp_str(ctx, &args[1], &bl);
    Path p, b; path_parse(&p, ps, pl); path_parse(&b, bs, bl);
    if (p.drive != b.drive || (p.drive && memcmp(ps, bs, p.drive) != 0) || p.root != b.root) return embrp_set_str(ctx, out, "", 0);
    if (!p.root && !p.drive && b.root) return embrp_set_str(ctx, out, "", 0);
    Elem* pe = (Elem*)malloc((pl + 3) * sizeof(Elem));
    Elem* be = (Elem*)malloc((bl + 3) * sizeof(Elem));
    char* buf = (char*)malloc(pl + 3 * bl + 8);
    if (!pe || !be || !buf) { free(pe); free(be); free(buf); return embrp_fail(ctx, "path_relative: out of memory"); }
    size_t np = path_elems(&p, pe), nb = path_elems(&b, be);
    size_t i = 0;
    while (i < np && i < nb && elem_eq(&pe[i], &be[i])) ++i;
    int rc;
    if (i == np && i == nb) { rc = embrp_set_str(ctx, out, ".", 1); goto done; }
    long cnt = 0;
    for (size_t k = i; k < nb; ++k) {
        if (be[k].l == 2 && be[k].s[0] == '.' && be[k].s[1] == '.') --cnt;
        else if (be[k].l != 0 && !(be[k].l == 1 && be[k].s[0] == '.')) ++cnt;
    }
    if (cnt < 0) { rc = embrp_set_str(ctx, out, "", 0); goto done; }
    if (cnt == 0 && (i == np || pe[i].l == 0)) { rc = embrp_set_str(ctx, out, ".", 1); goto done; }
    {
        size_t at = 0;
        for (long k = 0; k < cnt; ++k) { if (at) buf[at++] = '/'; buf[at++] = '.'; buf[at++] = '.'; }
        for (size_t k = i; k < np; ++k) {
            if (at && buf[at - 1] != '/') buf[at++] = '/';
            memcpy(buf + at, pe[k].s, pe[k].l); at += pe[k].l;
        }
        rc = give(ctx, out, buf, at);
    }
done:
    free(pe); free(be); free(buf);
    return rc;
}

/* ---- the table ---- */

static const EmbrParam p_one[]  = { { "p", EMBR_T_STRING, 0 } };
static const EmbrParam p_rel[]  = { { "p", EMBR_T_STRING, 0 }, { "base", EMBR_T_STRING, 0 } };
static const EmbrParam p_join[] = { { "a", EMBR_T_STRING, 0 }, { "more", EMBR_T_STRING, EMBR_PARAM_VARIADIC } };

/* every function gives the same answer for the same arguments and changes nothing, so a const may call them while compiling */
static const EmbrFunc funcs[] = {
    { "path_basename", "trailing separators ignored (\"a/b/\" → \"a\" / \"b\")",
      path_basename, NULL, p_one, 1, EMBR_T_STRING, EMBR_FUNC_PURE, 0 },
    { "path_dirname", "trailing separators ignored (\"a/b/\" → \"a\" / \"b\")",
      path_dirname, NULL, p_one, 1, EMBR_T_STRING, EMBR_FUNC_PURE, 0 },
    { "path_extname", "last extension only (y.tar.gz → .gz / y.tar); dotfiles have no extension",
      path_extname, NULL, p_one, 1, EMBR_T_STRING, EMBR_FUNC_PURE, 0 },
    { "path_is_abs", "1/0; a leading / counts as absolute on Windows too",
      path_is_abs, NULL, p_one, 1, EMBR_T_INT, EMBR_FUNC_PURE, 0 },
    { "path_join", "a later absolute component throws away the earlier ones (like python), empty components are skipped",
      path_join, NULL, p_join, 2, EMBR_T_STRING, EMBR_FUNC_PURE, 0 },
    { "path_normalize", "collapses ., .., repeated separators; \"\" → \".\"; no symlink resolution",
      path_normalize, NULL, p_one, 1, EMBR_T_STRING, EMBR_FUNC_PURE, 0 },
    { "path_relative", "lexical; \"\" if no relative path exists",
      path_relative, NULL, p_rel, 2, EMBR_T_STRING, 0, 0 },
    { "path_stem", "last extension only (y.tar.gz → .gz / y.tar); dotfiles have no extension",
      path_stem, NULL, p_one, 1, EMBR_T_STRING, EMBR_FUNC_PURE, 0 },
};

static const EmbrPluginInfo info = {
    EMBR_PLUGIN_ABI_VERSION, sizeof(EmbrPluginInfo), "path", "1.0.0",
    "pure path-string utilities", EMBR_CAP_NONE, funcs, sizeof funcs / sizeof funcs[0], NULL, 0, NULL,
    NULL
};

EMBR_PLUGIN_EXPORT const EmbrPluginInfo* embr_plugin_info(void) { return &info; }
