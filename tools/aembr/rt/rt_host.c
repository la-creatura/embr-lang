/* rt_host.c
   the host side of the C plugin interface (include/embr_plugin.h) for compiled code: it loads a C plugin, and calls one of its functions with
   compiled values. a program that calls no plugin does not link this file.

   how a call works: the compiled arguments are copied into EmbrValues (a string is borrowed, an array or a map is copied into a small array or
   map of EmbrValues), the plugin function runs and writes its result into an EmbrValue, and the result is copied into a compiled value.
   everything the plugin makes during the call (strings, arrays, maps, temp slots) belongs to the call and is freed when it ends.

   good to know
     - a plugin that makes a pointer (embrp_set_ptr) fails here: a handle needs a type aembr does not have. rooting a value keeps a copy until
       it is unrooted.
     - the kind of an error from a plugin is the name of the function, as the VM shows it, unless the plugin chose one (embrp_fail_kind) */
#define _GNU_SOURCE
#include "rt.h"
#include <embr_plugin.h>
#include "rt_host.h"

#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#define RT_PLUGIN_EXT ".dylib"
#else
#define RT_PLUGIN_EXT ".so"
#endif

/* ---- the objects behind an EmbrValue of tag ARRAY and MAP */

typedef struct { size_t n, cap; EmbrValue* items; } HostArr;
typedef struct { char* key; size_t klen; EmbrValue val; } HostEntry;
typedef struct { size_t n, cap; HostEntry* e; EmbrMapEntry* view; } HostMap;

enum { OWN_FREE, OWN_STR, OWN_ARR, OWN_MAP };
typedef struct { void* p; int kind; } Owned;

typedef struct HostCtx {
    EmbrCtx     base;
    int         failed;
    char        err[512];
    char        kind[48];
    Owned*      owned;
    size_t      nowned, cap;
    const char* scriptDir;
} HostCtx;

static void own(HostCtx* h, void* p, int kind) {
    if (h->nowned == h->cap) {
        h->cap = h->cap ? h->cap * 2 : 16;
        h->owned = realloc(h->owned, h->cap * sizeof *h->owned);
        if (!h->owned) { fputs("error: out of memory\n", stderr); exit(1); }
    }
    h->owned[h->nowned].p = p; h->owned[h->nowned].kind = kind; ++h->nowned;
}

static void host_free(HostCtx* h) {
    for (size_t i = 0; i < h->nowned; ++i) {
        void* p = h->owned[i].p;
        switch (h->owned[i].kind) {
        case OWN_STR: rt_release_str((EmbrStr*)p); break;
        case OWN_ARR: { HostArr* a = p; free(a->items); free(a); break; }
        case OWN_MAP: {
            HostMap* m = p;
            for (size_t k = 0; k < m->n; ++k) free(m->e[k].key);
            free(m->e); free(m->view); free(m);
            break;
        }
        default: free(p); break;
        }
    }
    free(h->owned);
    h->owned = NULL; h->nowned = h->cap = 0;
}

static void set_error(HostCtx* h, const char* kind, const char* msg) {
    if (h->failed) return;
    h->failed = 1;
    snprintf(h->err, sizeof h->err, "%s", msg ? msg : "error");
    snprintf(h->kind, sizeof h->kind, "%s", kind ? kind : "");
}

static HostCtx* H(EmbrCtx* c) { return (HostCtx*)c; }

/* ---- copying a value made by the plugin (or given to it) */

static void copy_value(HostCtx* h, EmbrValue* dst, const EmbrValue* src);

static HostArr* new_arr(HostCtx* h, size_t reserve) {
    HostArr* a = calloc(1, sizeof *a);
    if (!a) { fputs("error: out of memory\n", stderr); exit(1); }
    if (reserve > (1u << 20)) reserve = 1u << 20;
    if (reserve) { a->items = calloc(reserve, sizeof(EmbrValue)); a->cap = reserve; }
    own(h, a, OWN_ARR);
    return a;
}
static int arr_room(HostArr* a) {
    if (a->n < a->cap) return 1;
    size_t cap = a->cap ? a->cap * 2 : 4;
    EmbrValue* it = realloc(a->items, cap * sizeof(EmbrValue));
    if (!it) return 0;
    a->items = it; a->cap = cap;
    return 1;
}
static HostMap* new_map(HostCtx* h) {
    HostMap* m = calloc(1, sizeof *m);
    if (!m) { fputs("error: out of memory\n", stderr); exit(1); }
    own(h, m, OWN_MAP);
    return m;
}
static HostEntry* map_find(HostMap* m, const char* key, size_t klen) {
    for (size_t i = 0; i < m->n; ++i) if (m->e[i].klen == klen && memcmp(m->e[i].key, key, klen) == 0) return &m->e[i];
    return NULL;
}
static int map_put(HostCtx* h, HostMap* m, const char* key, size_t klen, const EmbrValue* v) {
    HostEntry* e = map_find(m, key, klen);
    if (!e) {
        if (m->n == m->cap) {
            size_t cap = m->cap ? m->cap * 2 : 4;
            HostEntry* ne = realloc(m->e, cap * sizeof *ne);
            if (!ne) return 0;
            m->e = ne; m->cap = cap;
        }
        e = &m->e[m->n++];
        e->key = malloc(klen + 1);
        if (!e->key) return 0;
        memcpy(e->key, key, klen); e->key[klen] = 0; e->klen = klen;
        memset(&e->val, 0, sizeof e->val);
        free(m->view); m->view = NULL;
    }
    copy_value(h, &e->val, v);
    return 1;
}

static void copy_value(HostCtx* h, EmbrValue* dst, const EmbrValue* src) {
    switch (src->tag) {
    case EMBR_TAG_STRING: {
        const EmbrStr* s = src->u.p;
        EmbrStr* c = rt_str_from(s->data, s->len);
        own(h, c, OWN_STR);
        dst->u.p = c; dst->tag = EMBR_TAG_STRING; dst->nest_depth = 0;
        break;
    }
    case EMBR_TAG_ARRAY: {
        const HostArr* a = src->u.p;
        HostArr* c = new_arr(h, a->n);
        for (size_t i = 0; i < a->n; ++i) { copy_value(h, &c->items[i], &a->items[i]); c->n = i + 1; }
        dst->u.p = c; dst->tag = EMBR_TAG_ARRAY; dst->nest_depth = 0;
        break;
    }
    case EMBR_TAG_MAP: {
        const HostMap* m = src->u.p;
        HostMap* c = new_map(h);
        for (size_t i = 0; i < m->n; ++i) map_put(h, c, m->e[i].key, m->e[i].klen, &m->e[i].val);
        dst->u.p = c; dst->tag = EMBR_TAG_MAP; dst->nest_depth = 0;
        break;
    }
    default: *dst = *src; break;
    }
}

/* ---- the table of host functions */

static const char* api_str(const EmbrValue* v, size_t* len) {
    if (v->tag != EMBR_TAG_STRING) { if (len) *len = 0; return ""; }
    const EmbrStr* s = v->u.p;
    if (len) *len = (size_t)s->len;
    return s->data;
}
static size_t api_array_len(const EmbrValue* v) { return v->tag == EMBR_TAG_ARRAY ? ((const HostArr*)v->u.p)->n : 0; }
static const EmbrValue* api_array_at(const EmbrValue* v, size_t i) {
    if (v->tag != EMBR_TAG_ARRAY) return NULL;
    const HostArr* a = v->u.p;
    return i < a->n ? &a->items[i] : NULL;
}
static size_t api_map_len(const EmbrValue* v) { return v->tag == EMBR_TAG_MAP ? ((const HostMap*)v->u.p)->n : 0; }
static const EmbrValue* api_map_get(const EmbrValue* v, const char* key) {
    if (v->tag != EMBR_TAG_MAP || !key) return NULL;
    HostEntry* e = map_find((HostMap*)v->u.p, key, strlen(key));
    return e ? &e->val : NULL;
}
static int entry_cmp(const void* x, const void* y) {
    const EmbrMapEntry* a = x; const EmbrMapEntry* b = y;
    size_t n = a->key_len < b->key_len ? a->key_len : b->key_len;
    int c = memcmp(a->key, b->key, n);
    return c ? c : (a->key_len < b->key_len ? -1 : a->key_len > b->key_len);
}
static const EmbrMapEntry* api_map_entries(EmbrCtx* c, const EmbrValue* v, size_t* count) {
    (void)c;
    *count = 0;
    if (v->tag != EMBR_TAG_MAP) return NULL;
    HostMap* m = v->u.p;
    free(m->view);
    m->view = malloc((m->n ? m->n : 1) * sizeof *m->view);
    if (!m->view) return NULL;
    for (size_t i = 0; i < m->n; ++i) { m->view[i].key = m->e[i].key; m->view[i].key_len = m->e[i].klen; m->view[i].value = &m->e[i].val; }
    qsort(m->view, m->n, sizeof *m->view, entry_cmp);
    *count = m->n;
    return m->view;
}
static int api_parse_float(const char* text, size_t len, double* out) {
    if (len == 0 || len > 400 || isspace((unsigned char)text[0]) || text[0] == '+') return 1;
    char buf[408];
    memcpy(buf, text, len); buf[len] = 0;
    if (strchr(buf, 'x') || strchr(buf, 'X')) return 1;
    char* end;
    errno = 0;
    double d = strtod(buf, &end);
    if (end != buf + len || (errno == ERANGE && (isinf(d) || d == 0.0))) return 1;
    *out = d;
    return 0;
}
typedef struct { void* p; char tag[48]; void (*fin)(void*); } HostPtr;
static void* api_ptr_get(const EmbrValue* v, const char* tag) {
    if (v->tag != EMBR_TAG_POINTER) return NULL;
    const HostPtr* hp = v->u.p;
    return strcmp(hp->tag, tag) == 0 ? hp->p : NULL;
}
static const char* api_ptr_tag(const EmbrValue* v) { return v->tag == EMBR_TAG_POINTER ? ((const HostPtr*)v->u.p)->tag : ""; }

/* the text print() shows for a value */
static void text_of(HostCtx* h, const EmbrValue* v, char** buf, size_t* n, size_t* cap, int quote);
static void put(char** buf, size_t* n, size_t* cap, const char* s, size_t len) {
    if (*n + len + 1 > *cap) { *cap = (*n + len + 1) * 2; *buf = realloc(*buf, *cap); if (!*buf) { fputs("error: out of memory\n", stderr); exit(1); } }
    memcpy(*buf + *n, s, len); *n += len; (*buf)[*n] = 0;
}
static void text_of(HostCtx* h, const EmbrValue* v, char** buf, size_t* n, size_t* cap, int quote) {
    char t[400];
    switch (v->tag) {
    case EMBR_TAG_INT: { int k = snprintf(t, sizeof t, "%lld", (long long)v->u.i); put(buf, n, cap, t, (size_t)k); break; }
    case EMBR_TAG_FLOAT: rt_fmt_f64(t, sizeof t, v->u.d); put(buf, n, cap, t, strlen(t)); break;
    case EMBR_TAG_STRING: { const EmbrStr* s = v->u.p; if (quote) put(buf, n, cap, "\"", 1); put(buf, n, cap, s->data, (size_t)s->len); if (quote) put(buf, n, cap, "\"", 1); break; }
    case EMBR_TAG_ARRAY: {
        const HostArr* a = v->u.p;
        put(buf, n, cap, "[", 1);
        for (size_t i = 0; i < a->n; ++i) { if (i) put(buf, n, cap, ", ", 2); text_of(h, &a->items[i], buf, n, cap, 1); }
        put(buf, n, cap, "]", 1);
        break;
    }
    case EMBR_TAG_MAP: {
        size_t cnt; const EmbrMapEntry* es = api_map_entries(&h->base, v, &cnt);
        put(buf, n, cap, "{", 1);
        for (size_t i = 0; i < cnt; ++i) { if (i) put(buf, n, cap, ", ", 2); put(buf, n, cap, es[i].key, es[i].key_len); put(buf, n, cap, ": ", 2); text_of(h, es[i].value, buf, n, cap, 1); }
        put(buf, n, cap, "}", 1);
        break;
    }
    default: put(buf, n, cap, "<value>", 7); break;
    }
}
static const char* api_to_string(EmbrCtx* c, const EmbrValue* v, size_t* len) {
    HostCtx* h = H(c);
    char* buf = NULL; size_t n = 0, cap = 0;
    put(&buf, &n, &cap, "", 0);
    text_of(h, v, &buf, &n, &cap, 0);
    own(h, buf, OWN_FREE);
    if (len) *len = n;
    return buf;
}
static int api_truthy(const EmbrValue* v) {
    switch (v->tag) {
    case EMBR_TAG_INT: return v->u.i != 0;
    case EMBR_TAG_FLOAT: return v->u.d != 0.0;
    case EMBR_TAG_STRING: return ((const EmbrStr*)v->u.p)->len > 0;
    case EMBR_TAG_ARRAY: return ((const HostArr*)v->u.p)->n > 0;
    case EMBR_TAG_MAP: return ((const HostMap*)v->u.p)->n > 0;
    case EMBR_TAG_POINTER: return ((const HostPtr*)v->u.p)->p != NULL;
    default: return 1;
    }
}
static int api_set_int(EmbrCtx* c, EmbrValue* s, int64_t x) { (void)c; s->u.i = x; s->tag = EMBR_TAG_INT; s->nest_depth = 0; return 0; }
static int api_set_float(EmbrCtx* c, EmbrValue* s, double x) { (void)c; s->u.d = x; s->tag = EMBR_TAG_FLOAT; s->nest_depth = 0; return 0; }
static int api_set_str(EmbrCtx* c, EmbrValue* s, const char* p, size_t n) {
    EmbrStr* str = rt_str_from(p ? p : "", p ? (int64_t)n : 0);
    own(H(c), str, OWN_STR);
    s->u.p = str; s->tag = EMBR_TAG_STRING; s->nest_depth = 0;
    return 0;
}
static char* api_set_str_alloc(EmbrCtx* c, EmbrValue* s, size_t n) {
    if (n > (size_t)(256 * 1024 * 1024)) { set_error(H(c), NULL, "string too long"); return NULL; }
    EmbrStr* str = rt_str_alloc((int64_t)n);
    own(H(c), str, OWN_STR);
    s->u.p = str; s->tag = EMBR_TAG_STRING; s->nest_depth = 0;
    return str->data;
}
static int api_set_copy(EmbrCtx* c, EmbrValue* s, const EmbrValue* f) { copy_value(H(c), s, f); return 0; }
static int api_set_array(EmbrCtx* c, EmbrValue* s, size_t reserve) {
    HostArr* a = new_arr(H(c), reserve);
    s->u.p = a; s->tag = EMBR_TAG_ARRAY; s->nest_depth = 0;
    return 0;
}
static int push_slot(HostCtx* h, EmbrValue* arr, EmbrValue** slot) {
    if (arr->tag != EMBR_TAG_ARRAY) { set_error(h, NULL, "not an array"); return 1; }
    HostArr* a = arr->u.p;
    if (!arr_room(a)) { set_error(h, NULL, "out of memory"); return 1; }
    *slot = &a->items[a->n++];
    memset(*slot, 0, sizeof **slot);
    return 0;
}
static int api_array_push(EmbrCtx* c, EmbrValue* arr, const EmbrValue* item) {
    EmbrValue* slot;
    if (push_slot(H(c), arr, &slot)) return 1;
    copy_value(H(c), slot, item);
    return 0;
}
static int api_array_push_str(EmbrCtx* c, EmbrValue* arr, const char* p, size_t n) {
    EmbrValue* slot;
    if (push_slot(H(c), arr, &slot)) return 1;
    return api_set_str(c, slot, p, n);
}
static int api_array_push_int(EmbrCtx* c, EmbrValue* arr, int64_t x) {
    EmbrValue* slot;
    if (push_slot(H(c), arr, &slot)) return 1;
    return api_set_int(c, slot, x);
}
static int api_array_push_float(EmbrCtx* c, EmbrValue* arr, double x) {
    EmbrValue* slot;
    if (push_slot(H(c), arr, &slot)) return 1;
    return api_set_float(c, slot, x);
}
static int api_set_map(EmbrCtx* c, EmbrValue* s) {
    HostMap* m = new_map(H(c));
    s->u.p = m; s->tag = EMBR_TAG_MAP; s->nest_depth = 0;
    return 0;
}
static int api_map_set(EmbrCtx* c, EmbrValue* m, const char* key, size_t kn, const EmbrValue* item) {
    if (m->tag != EMBR_TAG_MAP) { set_error(H(c), NULL, "not a map"); return 1; }
    return map_put(H(c), m->u.p, key ? key : "", key ? kn : 0, item) ? 0 : (set_error(H(c), NULL, "out of memory"), 1);
}
static int api_map_set_str(EmbrCtx* c, EmbrValue* m, const char* key, size_t kn, const char* p, size_t n) {
    EmbrValue tmp;
    memset(&tmp, 0, sizeof tmp);
    api_set_str(c, &tmp, p, n);
    return api_map_set(c, m, key, kn, &tmp);
}
/* a handle the plugin makes: the call keeps it, and an any made from the result takes it over (with its finalizer) */
static int api_set_ptr(EmbrCtx* c, EmbrValue* s, void* p, const char* tag, void (*fin)(void*)) {
    HostPtr* hp = calloc(1, sizeof *hp);
    if (!hp) { fputs("error: out of memory\n", stderr); exit(1); }
    own(H(c), hp, OWN_FREE);
    hp->p = p; hp->fin = fin;
    snprintf(hp->tag, sizeof hp->tag, "%s", tag ? tag : "");
    memset(s, 0, sizeof *s);
    s->u.p = hp; s->tag = EMBR_TAG_POINTER;
    return 0;
}
static EmbrValue* api_temp(EmbrCtx* c) {
    EmbrValue* t = calloc(1, sizeof *t);
    if (!t) { fputs("error: out of memory\n", stderr); exit(1); }
    own(H(c), t, OWN_FREE);
    return t;
}
static int api_fail(EmbrCtx* c, const char* msg) { set_error(H(c), NULL, msg); return 1; }
static int api_fail_kind(EmbrCtx* c, const char* kind, const char* msg) { set_error(H(c), kind, msg); return 1; }

/* rooting keeps a copy of the value until it is unrooted; the copy lives outside the call */
typedef struct { HostCtx h; EmbrValue v; } Root;
static EmbrRoot* api_root(EmbrCtx* c, const EmbrValue* v) {
    (void)c;
    Root* r = calloc(1, sizeof *r);
    if (!r) return NULL;
    copy_value(&r->h, &r->v, v);
    return (EmbrRoot*)r;
}
static const EmbrValue* api_root_get(const EmbrRoot* r) { return &((const Root*)r)->v; }
static void api_unroot(EmbrCtx* c, EmbrRoot* r) { (void)c; Root* x = (Root*)r; host_free(&x->h); free(x); }
static const char* api_script_dir(EmbrCtx* c) { const char* d = H(c)->scriptDir; return d ? d : ""; }
static const EmbrValue* api_array_data(EmbrCtx* c, const EmbrValue* v, size_t* len) {
    (void)c;
    static const EmbrValue none = {{0}, 0, 0, {0}};
    *len = 0;
    if (v->tag != EMBR_TAG_ARRAY) return NULL;
    const HostArr* a = v->u.p;
    *len = a->n;
    return a->n ? a->items : &none;
}
static int api_array_push_ints(EmbrCtx* c, EmbrValue* arr, const int64_t* xs, size_t n) {
    for (size_t i = 0; i < n; ++i) if (api_array_push_int(c, arr, xs[i])) return 1;
    return 0;
}
static int api_array_push_floats(EmbrCtx* c, EmbrValue* arr, const double* xs, size_t n) {
    for (size_t i = 0; i < n; ++i) if (api_array_push_float(c, arr, xs[i])) return 1;
    return 0;
}

/* the words after the program name are the script arguments, and the standard streams are the C ones */
static size_t api_script_arg_count(EmbrCtx* c) { (void)c; return rt_argc > 1 ? (size_t)(rt_argc - 1) : 0; }
static const char* api_script_arg(EmbrCtx* c, size_t i, size_t* len) {
    (void)c;
    if (!rt_argv || i + 1 >= (size_t)rt_argc) { if (len) *len = 0; return NULL; }
    if (len) *len = strlen(rt_argv[i + 1]);
    return rt_argv[i + 1];
}
static int api_stdio_write(EmbrCtx* c, int fd, const char* p, size_t n) {
    FILE* f = fd == 2 ? stderr : stdout;
    if (fd == 2) fflush(stdout);
    if (n && fwrite(p, 1, n, f) != n) return api_fail(c, "write failed");
    if (fd == 2) fflush(stderr);
    return 0;
}
static int api_stdio_flush(EmbrCtx* c) { return fflush(stdout) ? api_fail(c, "flush failed") : 0; }
static size_t api_stdin_read(EmbrCtx* c, char* buf, size_t n) { (void)c; return fread(buf, 1, n, stdin); }

static const EmbrApi* host_api(void) {
    static EmbrApi t;
    if (!t.struct_size) {
        t.abi_version = EMBR_PLUGIN_ABI_VERSION; t.struct_size = sizeof t;
        t.str = api_str; t.array_len = api_array_len; t.array_at = api_array_at; t.map_len = api_map_len; t.map_get = api_map_get;
        t.map_entries = api_map_entries; t.parse_float = api_parse_float; t.ptr_get = api_ptr_get; t.ptr_tag = api_ptr_tag;
        t.to_string = api_to_string; t.truthy = api_truthy; t.set_int = api_set_int; t.set_float = api_set_float; t.set_str = api_set_str;
        t.set_str_alloc = api_set_str_alloc; t.set_copy = api_set_copy; t.set_array = api_set_array; t.array_push = api_array_push;
        t.array_push_str = api_array_push_str; t.array_push_int = api_array_push_int; t.array_push_float = api_array_push_float;
        t.set_map = api_set_map; t.map_set = api_map_set; t.map_set_str = api_map_set_str; t.set_ptr = api_set_ptr; t.temp = api_temp;
        t.fail = api_fail; t.root = api_root; t.root_get = api_root_get; t.unroot = api_unroot; t.script_dir = api_script_dir;
        t.array_data = api_array_data; t.array_push_ints = api_array_push_ints; t.array_push_floats = api_array_push_floats; t.fail_kind = api_fail_kind;
        t.script_arg_count = api_script_arg_count; t.script_arg = api_script_arg; t.stdio_write = api_stdio_write; t.stdio_flush = api_stdio_flush; t.stdin_read = api_stdin_read;
    }
    return &t;
}

/* ---- loading a plugin */

/* when no path is known (a plugin that was moved to another machine), or the file is gone: the places the VM looks, in its order */
static void* open_by_name(const char* name, const char* scriptDir) {
    static char dirs[16][8300];
    int nd = 0;
    char exe[4096];
#if defined(__APPLE__)
    uint32_t sz = sizeof exe;
    ssize_t n = _NSGetExecutablePath(exe, &sz) == 0 ? (ssize_t)strlen(exe) : -1;
#else
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
#endif
    if (scriptDir && *scriptDir) snprintf(dirs[nd++], sizeof dirs[0], "%s", scriptDir);
    if (n > 0) {
        exe[n] = 0;
        char* slash = strrchr(exe, '/');
        if (slash) {
            *slash = 0;
            snprintf(dirs[nd++], sizeof dirs[0], "%s", exe);
            snprintf(dirs[nd++], sizeof dirs[0], "%s/../lib/embr", exe);
        }
    }
    const char* env = getenv("EMBR_PATH");
    if (env) {
        char buf[4096];
        snprintf(buf, sizeof buf, "%s", env);
        for (char* tok = strtok(buf, ":"); tok && nd < 12; tok = strtok(NULL, ":")) snprintf(dirs[nd++], sizeof dirs[0], "%s", tok);
    }
    snprintf(dirs[nd++], sizeof dirs[0], ".");
    for (int i = 0; i < nd; ++i) {
        const char* sub[2] = {"%s/%s" RT_PLUGIN_EXT, "%s/plugins/%s" RT_PLUGIN_EXT};
        for (int k = 0; k < 2; ++k) {
            char* p = NULL;
            if (asprintf(&p, sub[k], dirs[i], name) < 0) continue;
            void* l = access(p, R_OK) == 0 ? dlopen(p, RTLD_NOW | RTLD_LOCAL) : NULL;
            free(p);
            if (l) return l;
        }
    }
    return NULL;
}

const EmbrPluginInfo* rt_plugin_open(RtCtx* cx, const char* path, const char* name, const char* scriptDir) {
    void* lib = (path && *path) ? dlopen(path, RTLD_NOW | RTLD_LOCAL) : NULL;
    if (!lib) lib = open_by_name(name, scriptDir);
    if (!lib) { rt_raise(cx, "vm", 0, "cannot load plugin \"%s\"%s%s", name, path && *path ? ": " : " (it is looked for like the VM does, EMBR_PATH included)", path && *path ? dlerror() : ""); return NULL; }
    EmbrPluginInfoFn fn;
    *(void**)(&fn) = dlsym(lib, "embr_plugin_info");      /* (a function pointer from dlsym, the way POSIX says to write it) */
    const EmbrPluginInfo* info = fn ? fn() : NULL;
    if (!info) { rt_raise(cx, "vm", 0, "\"%s\" is not a C plugin (it has no embr_plugin_info)", name); return NULL; }
    if (info->abi_version != EMBR_PLUGIN_ABI_VERSION) {
        rt_raise(cx, "vm", 0, "plugin \"%s\" was built for plugin interface %u, this program uses %u: rebuild the plugin", name, info->abi_version, EMBR_PLUGIN_ABI_VERSION);
        return NULL;
    }
    if (info->init) {
        HostCtx h;
        memset(&h, 0, sizeof h);
        h.base.api = host_api(); h.scriptDir = scriptDir;
        int rc = info->init(&h.base);
        if (rc != 0 || h.failed) rt_raise(cx, "vm", 0, "plugin \"%s\" refused to load: %s", name, h.failed ? h.err : "init failed");
        host_free(&h);
    }
    return info;
}

EmbrFn rt_plugin_fn(RtCtx* cx, const EmbrPluginInfo* info, const char* name) {
    if (!info) return NULL;
    for (uint32_t i = 0; i < info->nfuncs; ++i) if (strcmp(info->funcs[i].name, name) == 0 && info->funcs[i].fn) return info->funcs[i].fn;
    rt_raise(cx, "vm", 0, "plugin \"%s\" has no function %s", info->name, name);
    return NULL;
}

/* ---- calling a function */

/* a compiled value of this kind (the slot of an array or a map, or what an any holds) to an EmbrValue, all the way down */
static void slot_in(HostCtx* h, EmbrValue* v, int64_t kind, int64_t slot);
static void any_in(HostCtx* h, EmbrValue* v, const EmbrAny* x) {
    if (!x) { memset(v, 0, sizeof *v); v->tag = EMBR_TAG_INT; return; }
    if (x->tag == 6) {          /* a handle goes back to a plugin as it came, without its finalizer */
        HostPtr* hp = calloc(1, sizeof *hp);
        if (!hp) { fputs("error: out of memory\n", stderr); exit(1); }
        own(h, hp, OWN_FREE);
        hp->p = x->v.h->p; snprintf(hp->tag, sizeof hp->tag, "%s", x->v.h->tag);
        memset(v, 0, sizeof *v);
        v->u.p = hp; v->tag = EMBR_TAG_POINTER;
        return;
    }
    switch (x->tag) {
    case 0: slot_in(h, v, 0, x->v.i); break;
    case 1: { int64_t b; memcpy(&b, &x->v.f, 8); slot_in(h, v, 1, b); break; }
    default: slot_in(h, v, x->tag, (int64_t)(intptr_t)x->v.s); break;
    }
}
static void slot_in(HostCtx* h, EmbrValue* v, int64_t kind, int64_t slot) {
    memset(v, 0, sizeof *v);
    switch (kind) {
    case 0: v->u.i = slot; v->tag = EMBR_TAG_INT; break;
    case 1: memcpy(&v->u.d, &slot, 8); v->tag = EMBR_TAG_FLOAT; break;
    case 2: v->u.p = (void*)(intptr_t)slot; v->tag = EMBR_TAG_STRING; break;
    case 3: {
        const EmbrArr* src = (const EmbrArr*)(intptr_t)slot;
        const int64_t n = rt_arr_len(src);
        HostArr* arr = new_arr(h, (size_t)n);
        for (int64_t i = 0; i < n; ++i) slot_in(h, &arr->items[i], src->kind, src->data[i]);
        arr->n = (size_t)n;
        v->u.p = arr; v->tag = EMBR_TAG_ARRAY;
        break;
    }
    case 4: {
        const EmbrMap* src = (const EmbrMap*)(intptr_t)slot;
        HostMap* m = new_map(h);
        for (int64_t i = 0; src && i < src->cap; ++i) {
            if (!src->keys[i]) continue;
            EmbrValue e;
            slot_in(h, &e, src->kind, src->vals[i]);
            map_put(h, m, src->keys[i]->data, (size_t)src->keys[i]->len, &e);
        }
        v->u.p = m; v->tag = EMBR_TAG_MAP;
        break;
    }
    default: any_in(h, v, (const EmbrAny*)(intptr_t)slot); break;
    }
}

/* an EmbrValue made by a plugin to an any, all the way down. false (after raising) for a function or a pointer */
static int value_to_any(RtCtx* cx, const EmbrValue* e, EmbrAny** out, const char* fname, int line) {
    switch (e->tag) {
    case EMBR_TAG_INT: *out = rt_any_of_int(e->u.i); return 1;
    case EMBR_TAG_FLOAT: *out = rt_any_of_float(e->u.d); return 1;
    case EMBR_TAG_STRING: {
        EmbrStr* s = rt_str_from(((const EmbrStr*)e->u.p)->data, ((const EmbrStr*)e->u.p)->len);
        *out = rt_any_of_str(s);
        rt_release_str(s);
        return 1;
    }
    case EMBR_TAG_ARRAY: {
        const HostArr* a = e->u.p;
        EmbrArr* r = rt_arr_new(5, (int64_t)a->n);
        for (size_t i = 0; i < a->n; ++i) {
            EmbrAny* x;
            if (!value_to_any(cx, &a->items[i], &x, fname, line)) { rt_release_arr(r); return 0; }
            r->data[r->len++] = (int64_t)(intptr_t)x;
        }
        *out = rt_any_of_arr(r);
        rt_release_arr(r);
        return 1;
    }
    case EMBR_TAG_MAP: {
        const HostMap* m = e->u.p;
        EmbrMap* r = rt_map_new(5);
        for (size_t i = 0; i < m->n; ++i) {
            EmbrAny* x;
            if (!value_to_any(cx, &m->e[i].val, &x, fname, line)) { rt_release_map(r); return 0; }
            EmbrStr* k = rt_str_from(m->e[i].key, (int64_t)m->e[i].klen);
            rt_map_put_slot(r, k, (int64_t)(intptr_t)x);
            rt_release_str(k);
        }
        *out = rt_any_of_map(r);
        rt_release_map(r);
        return 1;
    }
    case EMBR_TAG_POINTER: {
        HostPtr* hp = e->u.p;
        *out = rt_any_of_ptr(hp->p, hp->tag, hp->fin);
        hp->fin = NULL;                  /* the any owns the handle now */
        return 1;
    }
    default:
        rt_raise(cx, fname, line, "the function gave a function, which compiled code cannot hold");
        return 0;
    }
}

/* the compiled argument to an EmbrValue */
static void arg_in(HostCtx* h, EmbrValue* v, const RtPArg* a) {
    memset(v, 0, sizeof *v);
    switch (a->kind) {
    case 0: v->u.i = a->i; v->tag = EMBR_TAG_INT; break;
    case 1: v->u.d = a->f; v->tag = EMBR_TAG_FLOAT; break;
    case 2: v->u.p = a->p; v->tag = EMBR_TAG_STRING; break;
    case 5: any_in(h, v, a->p); break;
    default: slot_in(h, v, a->kind, (int64_t)(intptr_t)a->p); break;      /* 3 an array, 4 a map, with whatever they hold */
    }
}

static const char* tag_name(const EmbrValue* v) {
    static const char* const names[] = { "number", "int", "string", "array", "map", "function", "pointer" };
    return v->tag < 7 ? names[v->tag] : "?";
}

/* the result the plugin wrote to a compiled value of the kind the caller expects. false after raising */
static int result_out(RtCtx* cx, HostCtx* h, const char* fname, const EmbrValue* out, int want, RtPRes* res, int line) {
    (void)h;
    switch (want) {
    case 6: return 1;                                                   /* the result is not used */
    case 0:
        if (out->tag != EMBR_TAG_INT) break;
        res->i = out->u.i; return 1;
    case 1:
        if (out->tag != EMBR_TAG_FLOAT) break;
        res->f = out->u.d; return 1;
    case 2:
        if (out->tag != EMBR_TAG_STRING) break;
        res->p = rt_str_from(((const EmbrStr*)out->u.p)->data, ((const EmbrStr*)out->u.p)->len); return 1;
    case 7: {
        EmbrAny* x = NULL;
        if (!value_to_any(cx, out, &x, fname, line)) return 0;
        res->p = x;
        return 1;
    }
    case 3: case 4: case 5: {
        if (out->tag != EMBR_TAG_ARRAY) break;
        const HostArr* a = out->u.p;
        const int tagWant = want == 3 ? EMBR_TAG_INT : want == 4 ? EMBR_TAG_FLOAT : EMBR_TAG_STRING;
        EmbrArr* r = rt_arr_new(want - 3, (int64_t)a->n);
        for (size_t i = 0; i < a->n; ++i) {
            const EmbrValue* e = &a->items[i];
            if (e->tag != tagWant) {
                rt_raise(cx, fname, line, "element %zu of the result is %s, the call expects %s", i, tag_name(e), want == 3 ? "int" : want == 4 ? "float" : "str");
                rt_release_arr(r);
                return 0;
            }
            if (want == 3) r->data[r->len++] = e->u.i;
            else if (want == 4) memcpy(&r->data[r->len++], &e->u.d, 8);
            else r->data[r->len++] = (int64_t)(intptr_t)rt_str_from(((const EmbrStr*)e->u.p)->data, ((const EmbrStr*)e->u.p)->len);
        }
        res->p = r;
        return 1;
    }
    default: break;
    }
    rt_raise(cx, fname, line, "the function gave %s, the call expects something else (the type comes from the plugin's description)", tag_name(out));
    return 0;
}

void rt_plugin_call(RtCtx* cx, EmbrFn fn, const char* fname, const char* scriptDir, const RtPArg* args, int n, int want, RtPRes* res, int line) {
    HostCtx h;
    memset(&h, 0, sizeof h);
    h.base.api = host_api();
    h.scriptDir = scriptDir;
    EmbrValue argv[8];
    EmbrValue* av = n <= 8 ? argv : calloc((size_t)n, sizeof *av);
    for (int i = 0; i < n; ++i) arg_in(&h, &av[i], &args[i]);
    EmbrValue out;
    memset(&out, 0, sizeof out);            /* the float 0, as the host starts it */
    memset(res, 0, sizeof *res);
    const int rc = fn(&h.base, av, (size_t)n, &out);
    if (rc != 0 || h.failed) {
        char kind[64];
        snprintf(kind, sizeof kind, "%s", h.kind[0] ? h.kind : fname);
        rt_raise_dyn_kind(cx, kind, line, h.failed ? h.err : "failed");
    } else {
        result_out(cx, &h, fname, &out, want, res, line);
    }
    if (av != argv) free(av);
    host_free(&h);
}
