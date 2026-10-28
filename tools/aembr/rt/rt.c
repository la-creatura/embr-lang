/* rt.c
   the runtime of aembr output: the error context, the uncaught error report, and printing numbers the way the VM does. see rt.h. */
#include "rt.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uintptr_t rt_stack_limit_g;
static RtCtx* main_cx;
void rt_stack_overflow(int line) {
    rt_raise(main_cx, "vm", line, "stack overflow: the stack budget is used up");
    exit(rt_finish(main_cx));
}

int rt_argc;
char** rt_argv;
void rt_set_args(int argc, char** argv) { rt_argc = argc; rt_argv = argv; }

void rt_ctx_init(RtCtx* cx, const char* file) {
    memset(cx, 0, sizeof *cx);
    cx->file = file;
    static size_t kb;      /* read once: this runs on every call from the host */
    if (!kb) {
        kb = 1024;
        const char* env = getenv("EMBR_STACK_KB");
        if (env && *env) { long v = strtol(env, NULL, 10); if (v > 0) kb = (size_t)v; }
    }
    cx->limit = (uintptr_t)__builtin_frame_address(0) - kb * 1024;
    rt_stack_limit_g = cx->limit;
    main_cx = cx;
}

void rt_raise_bytes(RtCtx* cx, const char* kind, int line, const char* data, int64_t len) {
    if (cx->err) return;
    cx->err = 1;
    cx->kind = kind;
    cx->line = line;
    cx->msg = malloc((size_t)len + 1);
    if (!cx->msg) { fflush(stdout); fputs("error: out of memory\n", stderr); exit(1); }
    memcpy(cx->msg, data, (size_t)len);
    cx->msg[len] = 0;
    cx->msglen = len;
}

void rt_raise(RtCtx* cx, const char* kind, int line, const char* fmt, ...) {
    if (cx->err) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    if ((size_t)n >= sizeof buf) n = (int)sizeof buf - 1;
    rt_raise_bytes(cx, kind, line, buf, n);
}

void rt_raise_dyn_kind(RtCtx* cx, const char* kind, int line, const char* msg) {
    if (cx->err) return;
    snprintf(cx->kindbuf, sizeof cx->kindbuf, "%s", kind);
    rt_raise_bytes(cx, cx->kindbuf, line, msg, (int64_t)strlen(msg));
}

void rt_raise_str(RtCtx* cx, int line, const EmbrStr* msg) { rt_raise_bytes(cx, "error", line, msg->data, msg->len); }

void rt_trace(RtCtx* cx, const char* fn, int line, const char* file) {
    if (cx->ntrace < EMBR_MAX_TRACE) { cx->trace[cx->ntrace].fn = fn; cx->trace[cx->ntrace].line = line; cx->trace[cx->ntrace].file = file; ++cx->ntrace; }
    else ++cx->dropped;
}

int rt_finish(RtCtx* cx) {
    fflush(stdout);
    if (!cx->err) return 0;
    fputs("error: ", stderr);
    fwrite(cx->msg, 1, (size_t)cx->msglen, stderr);
    fprintf(stderr, " at line %d\n", cx->line);
    if (cx->ntrace || cx->dropped) {
        fprintf(stderr, "stack trace (most recent call first):\n");
        for (int i = 0; i < cx->ntrace; ++i) fprintf(stderr, "  in %s() called at line %d of %s\n", cx->trace[i].fn, cx->trace[i].line, cx->trace[i].file ? cx->trace[i].file : cx->file);
        if (cx->dropped) fprintf(stderr, "  (%d more frames)\n", cx->dropped);
    }
    free(cx->msg);
    cx->msg = NULL;
    return 1;
}

void rt_print_i64(int64_t v) { printf("%lld\n", (long long)v); }

/* the same text as the VM (formatNumber in include/embr/core/value.h): a float that is a whole number below 1e15 prints like an int,
   otherwise six digits with the trailing zeros cut. the range test comes first so a NaN never reaches the cast */
void rt_fmt_f64(char* buf, size_t n, double d);
static void fmt_f64(char* buf, size_t n, double d) { rt_fmt_f64(buf, n, d); }
void rt_fmt_f64(char* buf, size_t n, double d) {
    if (d >= -1e15 && d <= 1e15 && d == (double)(long long)d) {
        snprintf(buf, n, "%lld", (long long)d);
    } else {
        snprintf(buf, n, "%f", d);
        char* dot = strchr(buf, '.');
        if (dot) {
            char* end = buf + strlen(buf);
            while (end > dot + 1 && end[-1] == '0') --end;
            if (end == dot + 1) --end;
            *end = 0;
        }
    }
}

void rt_print_f64(double d) {
    char buf[400];
    fmt_f64(buf, sizeof buf, d);
    printf("%s\n", buf);
}

/* ---- strings */

#ifdef AEMBR_STATS
uint64_t rt_stat_v[RT_NSTAT];
static const char* const rt_stat_names[RT_NSTAT] = {
#define X(n) #n,
RT_STAT_LIST(X)
#undef X
};
/* what the program did, once, when it ends (only if AEMBR_STATS is set in the environment) */
__attribute__((destructor)) static void rt_stat_report(void) {
    if (!getenv("AEMBR_STATS")) return;
    fprintf(stderr, "[aembr stats]\n");
    for (int i = 0; i < RT_NSTAT; ++i) if (rt_stat_v[i]) fprintf(stderr, "  %-20s %llu\n", rt_stat_names[i], (unsigned long long)rt_stat_v[i]);
}
#endif

static void* xalloc(size_t n) {
    RT_STAT_ADD(alloc_bytes, n);
    void* p = malloc(n);
    if (!p) { fflush(stdout); fputs("error: out of memory\n", stderr); exit(1); }
    return p;
}

static struct { int64_t rc, len, cap; char d[1]; } empty_s = { EMBR_IMMORTAL, 0, 0, { 0 } };
EmbrStr* const rt_empty_str = (EmbrStr*)&empty_s;

static EmbrStr* str_alloc(int64_t len, int64_t cap) {
    RT_STAT(alloc_str);
    EmbrStr* s = xalloc(sizeof(EmbrStr) + (size_t)cap + 1);
    s->rc = 1; s->len = len; s->cap = cap; s->data[len] = 0;
    return s;
}

EmbrStr* rt_str_concat(const EmbrStr* a, const EmbrStr* b) {
    RT_STAT(str_concat); RT_STAT_ADD(str_concat_bytes, a->len + b->len);
    EmbrStr* s = str_alloc(a->len + b->len, a->len + b->len);
    memcpy(s->data, a->data, (size_t)a->len);
    memcpy(s->data + a->len, b->data, (size_t)b->len);
    s->data[s->len] = 0;
    return s;
}

/* a nobody else holds (count 1) grows in place, so a loop of `s = s + x` is not quadratic. appends len bytes of src, from off */
EmbrStr* rt_str_append_bytes(EmbrStr* a, const EmbrStr* src, int64_t off, int64_t len) {
    const int64_t n = a->len + len;
    if (a->rc == 1) {
        RT_STAT(str_append_inplace);
        const int self = (const EmbrStr*)a == src;     /* s = s + s: src moves with a */
        if (n > a->cap) {
            int64_t cap = a->cap * 2 > n ? a->cap * 2 : n;
            if (cap < 32) cap = 32;      /* a string that is appended to once is appended to again */
            EmbrStr* g = realloc(a, sizeof(EmbrStr) + (size_t)cap + 1);
            if (!g) { fflush(stdout); fputs("error: out of memory\n", stderr); exit(1); }
            a = g; a->cap = cap;
            if (self) src = a;
        }
        memmove(a->data + a->len, src->data + off, (size_t)len);
        a->len = n; a->data[n] = 0;
        return a;
    }
    RT_STAT(str_append_copy);
    RT_STAT(str_concat); RT_STAT_ADD(str_concat_bytes, a->len + len);
    EmbrStr* r = str_alloc(n, n < 32 ? 32 : n + n / 2);   /* room to grow: this is the start of a `s = s + x` chain */
    memcpy(r->data, a->data, (size_t)a->len);
    memcpy(r->data + a->len, src->data + off, (size_t)len);
    r->data[n] = 0;
    rt_release_str(a);
    return r;
}
EmbrStr* rt_str_append(EmbrStr* a, const EmbrStr* b) { return rt_str_append_bytes(a, b, 0, b->len); }

int64_t rt_str_eq(const EmbrStr* a, const EmbrStr* b) {
    return a->len == b->len && memcmp(a->data, b->data, (size_t)a->len) == 0;
}

EmbrStr* rt_str_index(RtCtx* cx, const EmbrStr* s, int64_t i, int line) {
    if (EMBR_UNLIKELY(i < 0 || i >= s->len)) { rt_raise(cx, "vm", line, "string index %lld out of bounds", (long long)i); return NULL; }
    EmbrStr* r = str_alloc(1, 1);
    r->data[0] = s->data[i];
    return r;
}

EmbrStr* rt_str_of_i64(int64_t v) {
    char buf[32];
    int n = snprintf(buf, sizeof buf, "%lld", (long long)v);
    EmbrStr* s = str_alloc(n, n);
    memcpy(s->data, buf, (size_t)n);
    return s;
}

EmbrStr* rt_str_of_f64(double v) {
    char buf[400];
    fmt_f64(buf, sizeof buf, v);
    size_t n = strlen(buf);
    EmbrStr* s = str_alloc((int64_t)n, (int64_t)n);
    memcpy(s->data, buf, n);
    return s;
}

void rt_print_str(const EmbrStr* s) {
    fwrite(s->data, 1, (size_t)s->len, stdout);
    fputc('\n', stdout);
}

/* ---- arrays */

EmbrArr* rt_arr_new(int64_t kind, int64_t cap) {
    RT_STAT(alloc_arr);
    EmbrArr* a = xalloc(sizeof *a);
    a->rc = 1; a->len = 0; a->cap = cap > 0 ? cap : 0; a->kind = kind;
    a->data = a->cap ? (RT_STAT(alloc_arrdata), xalloc((size_t)a->cap * 8)) : NULL;
    return a;
}

void rt_free_arr(EmbrArr* a) {
    RT_STAT(free_arr);
    if (a->kind >= 2) for (int64_t i = 0; i < a->len; ++i) rt_elem_release(a->kind, a->data[i]);
    free(a->data);
    free(a);
}

EmbrArr* rt_arr_unique_slow(EmbrArr* a) {
    if (!a || a->rc == 1) return a;
    RT_STAT(arr_cow); RT_STAT_ADD(arr_cow_elems, a->len);
    EmbrArr* c = rt_arr_new(a->kind, a->len);
    if (a->len) memcpy(c->data, a->data, (size_t)a->len * 8);      /* (an empty array has no data to copy) */
    c->len = a->len;
    if (a->kind >= 2) for (int64_t i = 0; i < c->len; ++i) rt_elem_retain(a->kind, c->data[i]);
    rt_release_arr(a);
    return c;
}

static EmbrArr* arr_room(EmbrArr* a, int64_t kind) {
    RT_STAT(arr_push);
    a = a ? rt_arr_unique(a) : rt_arr_new(kind, 4);
    if (a->len == a->cap) {
        RT_STAT(arr_grow);
        int64_t cap = a->cap ? a->cap * 2 : 8;
        int64_t* d = realloc(a->data, (size_t)cap * 8);
        if (!d) { fflush(stdout); fputs("error: out of memory\n", stderr); exit(1); }
        a->data = d; a->cap = cap;
    }
    return a;
}

EmbrArr* rt_arr_push_i64_slow(EmbrArr* a, int64_t x) { a = arr_room(a, 0); a->data[a->len++] = x; return a; }
EmbrArr* rt_arr_push_f64_slow(EmbrArr* a, double x) { a = arr_room(a, 1); memcpy(&a->data[a->len++], &x, 8); return a; }
EmbrArr* rt_arr_push_str(EmbrArr* a, const EmbrStr* x) {
    a = arr_room(a, 2);
    a->data[a->len++] = (int64_t)(intptr_t)rt_retain_str((EmbrStr*)x);
    return a;
}

EmbrArr* rt_arr_push_arr(EmbrArr* a, EmbrArr* x) {
    a = arr_room(a, 3);
    a->data[a->len++] = (int64_t)(intptr_t)rt_retain_arr(x);
    return a;
}
EmbrArr* rt_arr_push_map(EmbrArr* a, EmbrMap* x) {
    a = arr_room(a, 4);
    a->data[a->len++] = (int64_t)(intptr_t)rt_retain_map(x);
    return a;
}

EmbrArr* rt_arr_push_any(EmbrArr* a, EmbrAny* x) {
    a = arr_room(a, 5);
    a->data[a->len++] = (int64_t)(intptr_t)rt_retain_any(x);
    return a;
}

#define ARR_CHECK(cx, a, i, line, bad) \
    if (EMBR_UNLIKELY(!(a) || (i) < 0 || (i) >= (a)->len)) { rt_raise(cx, "vm", line, "array index %lld out of bounds", (long long)(i)); return bad; }

void rt_arr_oob(RtCtx* cx, int64_t i, int line) { rt_raise(cx, "vm", line, "array index %lld out of bounds", (long long)i); }
EmbrStr* rt_arr_get_str(RtCtx* cx, const EmbrArr* a, int64_t i, int line) { ARR_CHECK(cx, a, i, line, NULL) return rt_retain_str((EmbrStr*)(intptr_t)a->data[i]); }

EmbrArr* rt_arr_get_arr(RtCtx* cx, const EmbrArr* a, int64_t i, int line) { ARR_CHECK(cx, a, i, line, NULL) return rt_retain_arr((EmbrArr*)(intptr_t)a->data[i]); }
EmbrMap* rt_arr_get_map(RtCtx* cx, const EmbrArr* a, int64_t i, int line) { ARR_CHECK(cx, a, i, line, NULL) return rt_retain_map((EmbrMap*)(intptr_t)a->data[i]); }

EmbrAny* rt_arr_get_any(RtCtx* cx, const EmbrArr* a, int64_t i, int line) { ARR_CHECK(cx, a, i, line, NULL) return rt_retain_any((EmbrAny*)(intptr_t)a->data[i]); }
void rt_arr_set_any(RtCtx* cx, EmbrArr* a, int64_t i, EmbrAny* x, int line) {
    ARR_CHECK(cx, a, i, line, )
    rt_retain_any(x);
    rt_release_any((EmbrAny*)(intptr_t)a->data[i]);
    a->data[i] = (int64_t)(intptr_t)x;
}

void rt_arr_set_str(RtCtx* cx, EmbrArr* a, int64_t i, const EmbrStr* x, int line) {
    ARR_CHECK(cx, a, i, line, )
    rt_retain_str((EmbrStr*)x);
    rt_release_str((EmbrStr*)(intptr_t)a->data[i]);
    a->data[i] = (int64_t)(intptr_t)x;
}

void rt_arr_set_arr(RtCtx* cx, EmbrArr* a, int64_t i, EmbrArr* x, int line) {
    ARR_CHECK(cx, a, i, line, )
    rt_retain_arr(x);
    rt_release_arr((EmbrArr*)(intptr_t)a->data[i]);
    a->data[i] = (int64_t)(intptr_t)x;
}
void rt_arr_set_map(RtCtx* cx, EmbrArr* a, int64_t i, EmbrMap* x, int line) {
    ARR_CHECK(cx, a, i, line, )
    rt_retain_map(x);
    rt_release_map((EmbrMap*)(intptr_t)a->data[i]);
    a->data[i] = (int64_t)(intptr_t)x;
}

/* the child is unique before the caller changes it: a copy takes its place in the slot when someone else holds it. a missing child (never made
   by the compiled code, but cheap to be safe about) becomes an empty one */
EmbrArr* rt_arr_child_arr(RtCtx* cx, EmbrArr* a, int64_t i, int64_t ckind, int line) {
    ARR_CHECK(cx, a, i, line, NULL)
    EmbrArr* c = (EmbrArr*)(intptr_t)a->data[i];
    c = c ? rt_arr_unique(c) : rt_arr_new(ckind, 0);
    a->data[i] = (int64_t)(intptr_t)c;
    return c;
}
EmbrMap* rt_arr_child_map(RtCtx* cx, EmbrArr* a, int64_t i, int64_t ckind, int line) {
    ARR_CHECK(cx, a, i, line, NULL)
    EmbrMap* c = (EmbrMap*)(intptr_t)a->data[i];
    c = c ? rt_map_unique(c) : rt_map_new(ckind);
    a->data[i] = (int64_t)(intptr_t)c;
    return c;
}

EmbrStr* rt_join(const EmbrArr* a, const EmbrStr* sep) {
    int64_t n = 0;
    const int64_t cnt = rt_arr_len(a);
    for (int64_t i = 0; i < cnt; ++i) n += ((EmbrStr*)(intptr_t)a->data[i])->len;
    if (cnt > 1) n += (cnt - 1) * sep->len;
    EmbrStr* s = str_alloc(n, n);
    char* p = s->data;
    for (int64_t i = 0; i < cnt; ++i) {
        if (i) { memcpy(p, sep->data, (size_t)sep->len); p += sep->len; }
        const EmbrStr* e = (EmbrStr*)(intptr_t)a->data[i];
        memcpy(p, e->data, (size_t)e->len); p += e->len;
    }
    s->data[n] = 0;
    return s;
}

/* ---- maps */

EmbrMap* rt_map_new(int64_t kind) {
    RT_STAT(alloc_map);
    EmbrMap* m = xalloc(sizeof *m);
    m->rc = 1; m->len = 0; m->cap = 0; m->kind = kind; m->keys = NULL; m->vals = NULL;
    return m;
}

void rt_free_map(EmbrMap* m) {
    RT_STAT(free_map);
    for (int64_t i = 0; i < m->cap; ++i) {
        if (!m->keys[i]) continue;
        rt_release_str(m->keys[i]);
        if (m->kind >= 2) rt_elem_release(m->kind, m->vals[i]);
    }
    free(m->keys); free(m->vals); free(m);
}

static uint64_t hash_str(const EmbrStr* s) {
    uint64_t h = 1469598103934665603ull;
    for (int64_t i = 0; i < s->len; ++i) { h ^= (unsigned char)s->data[i]; h *= 1099511628211ull; }
    return h;
}

/* the slot holding k, or the empty slot where it would go. cap has to be above 0 */
static int64_t map_slot(const EmbrMap* m, const EmbrStr* k) {
    const uint64_t mask = (uint64_t)m->cap - 1;
    uint64_t i = hash_str(k) & mask;
    while (m->keys[i] && !rt_str_eq(m->keys[i], k)) i = (i + 1) & mask;
    return (int64_t)i;
}

static void map_alloc(EmbrMap* m, int64_t cap) {
    m->cap = cap;
    m->keys = calloc((size_t)cap, sizeof *m->keys);
    m->vals = calloc((size_t)cap, sizeof *m->vals);
    if (!m->keys || !m->vals) { fflush(stdout); fputs("error: out of memory\n", stderr); exit(1); }
}

static void map_grow(EmbrMap* m) {
    RT_STAT(map_grow);
    EmbrStr** ok = m->keys; int64_t* ov = m->vals; const int64_t oc = m->cap;
    map_alloc(m, oc ? oc * 2 : 8);
    for (int64_t i = 0; i < oc; ++i) {
        if (!ok[i]) continue;
        const int64_t s = map_slot(m, ok[i]);
        m->keys[s] = ok[i]; m->vals[s] = ov[i];
    }
    free(ok); free(ov);
}

EmbrMap* rt_map_unique(EmbrMap* m) {
    if (!m || m->rc == 1) return m;
    EmbrMap* c = rt_map_new(m->kind);
    if (m->cap) {
        map_alloc(c, m->cap);
        for (int64_t i = 0; i < m->cap; ++i) {
            if (!m->keys[i]) continue;
            c->keys[i] = rt_retain_str(m->keys[i]);
            c->vals[i] = m->vals[i];
            if (m->kind >= 2) rt_elem_retain(m->kind, m->vals[i]);
        }
        c->len = m->len;
    }
    rt_release_map(m);
    return c;
}

/* v is stored as given: a str value has already been retained */
static void map_put(EmbrMap* m, const EmbrStr* k, int64_t v) {
    RT_STAT(map_put);
    if ((m->len + 1) * 2 > m->cap) map_grow(m);
    const int64_t s = map_slot(m, k);
    if (m->keys[s]) {
        if (m->kind >= 2) rt_elem_release(m->kind, m->vals[s]);
    } else {
        m->keys[s] = rt_retain_str((EmbrStr*)k);
        m->len++;
    }
    m->vals[s] = v;
}

void rt_map_set_i64(EmbrMap* m, const EmbrStr* k, int64_t x) { map_put(m, k, x); }
void rt_map_set_f64(EmbrMap* m, const EmbrStr* k, double x) { int64_t b; memcpy(&b, &x, 8); map_put(m, k, b); }
void rt_map_set_str(EmbrMap* m, const EmbrStr* k, const EmbrStr* x) { map_put(m, k, (int64_t)(intptr_t)rt_retain_str((EmbrStr*)x)); }
void rt_map_set_arr(EmbrMap* m, const EmbrStr* k, EmbrArr* x) { map_put(m, k, (int64_t)(intptr_t)rt_retain_arr(x)); }
void rt_map_set_map(EmbrMap* m, const EmbrStr* k, EmbrMap* x) { map_put(m, k, (int64_t)(intptr_t)rt_retain_map(x)); }
void rt_map_set_any(EmbrMap* m, const EmbrStr* k, EmbrAny* x) { map_put(m, k, (int64_t)(intptr_t)rt_retain_any(x)); }

static int64_t map_find(RtCtx* cx, const EmbrMap* m, const EmbrStr* k, int line) {
    RT_STAT(map_find);
    if (EMBR_UNLIKELY(!m || !m->cap)) goto missing;
    {
        const int64_t s = map_slot(m, k);
        if (m->keys[s]) return s;
    }
missing:
    rt_raise(cx, "vm", line, "key not found: %.*s", (int)k->len, k->data);
    return -1;
}

int64_t rt_map_get_i64(RtCtx* cx, const EmbrMap* m, const EmbrStr* k, int line) { const int64_t s = map_find(cx, m, k, line); return s < 0 ? 0 : m->vals[s]; }
double rt_map_get_f64(RtCtx* cx, const EmbrMap* m, const EmbrStr* k, int line) { const int64_t s = map_find(cx, m, k, line); double d = 0; if (s >= 0) memcpy(&d, &m->vals[s], 8); return d; }
EmbrStr* rt_map_get_str(RtCtx* cx, const EmbrMap* m, const EmbrStr* k, int line) { const int64_t s = map_find(cx, m, k, line); return s < 0 ? NULL : rt_retain_str((EmbrStr*)(intptr_t)m->vals[s]); }
EmbrAny* rt_map_get_any(RtCtx* cx, const EmbrMap* m, const EmbrStr* k, int line) { const int64_t s = map_find(cx, m, k, line); return s < 0 ? NULL : rt_retain_any((EmbrAny*)(intptr_t)m->vals[s]); }
EmbrArr* rt_map_get_arr(RtCtx* cx, const EmbrMap* m, const EmbrStr* k, int line) { const int64_t s = map_find(cx, m, k, line); return s < 0 ? NULL : rt_retain_arr((EmbrArr*)(intptr_t)m->vals[s]); }
EmbrMap* rt_map_get_map(RtCtx* cx, const EmbrMap* m, const EmbrStr* k, int line) { const int64_t s = map_find(cx, m, k, line); return s < 0 ? NULL : rt_retain_map((EmbrMap*)(intptr_t)m->vals[s]); }

EmbrArr* rt_map_child_arr(RtCtx* cx, EmbrMap* m, const EmbrStr* k, int64_t ckind, int line) {
    const int64_t s = map_find(cx, m, k, line);
    if (s < 0) return NULL;
    EmbrArr* c = (EmbrArr*)(intptr_t)m->vals[s];
    c = c ? rt_arr_unique(c) : rt_arr_new(ckind, 0);
    m->vals[s] = (int64_t)(intptr_t)c;
    return c;
}
EmbrMap* rt_map_child_map(RtCtx* cx, EmbrMap* m, const EmbrStr* k, int64_t ckind, int line) {
    const int64_t s = map_find(cx, m, k, line);
    if (s < 0) return NULL;
    EmbrMap* c = (EmbrMap*)(intptr_t)m->vals[s];
    c = c ? rt_map_unique(c) : rt_map_new(ckind);
    m->vals[s] = (int64_t)(intptr_t)c;
    return c;
}

int64_t rt_map_slot_index(const EmbrMap* m, const EmbrStr* k) {      /* the index of the slot that holds k, or -1 */
    if (!m || !m->cap) return -1;
    const int64_t s = map_slot(m, k);
    return m->keys[s] ? s : -1;
}
int rt_map_lookup(const EmbrMap* m, const EmbrStr* k, int64_t* slot) {
    if (!m || !m->cap) return 0;
    const int64_t s = map_slot(m, k);
    if (!m->keys[s]) return 0;
    *slot = m->vals[s];
    return 1;
}
void rt_map_put_slot(EmbrMap* m, const EmbrStr* k, int64_t slot) { map_put(m, k, slot); }

int64_t rt_map_has(const EmbrMap* m, const EmbrStr* k) {
    return m && m->cap && m->keys[map_slot(m, k)] != NULL;
}

typedef struct { EmbrStr* k; int64_t v; } KeyVal;

static int cmp_keyval(const void* x, const void* y) {
    const EmbrStr* a = ((const KeyVal*)x)->k; const EmbrStr* b = ((const KeyVal*)y)->k;
    const int64_t n = a->len < b->len ? a->len : b->len;
    const int c = memcmp(a->data, b->data, (size_t)n);
    if (c) return c;
    return a->len < b->len ? -1 : a->len > b->len;
}

static KeyVal* sorted_pairs(const EmbrMap* m) {
    RT_STAT(map_sort); RT_STAT_ADD(map_sort_elems, m->len);
    KeyVal* p = xalloc((size_t)(m->len ? m->len : 1) * sizeof *p);
    int64_t n = 0;
    for (int64_t i = 0; i < m->cap; ++i) if (m->keys[i]) { p[n].k = m->keys[i]; p[n].v = m->vals[i]; ++n; }
    qsort(p, (size_t)n, sizeof *p, cmp_keyval);
    return p;
}

EmbrArr* rt_map_keys(const EmbrMap* m) {
    EmbrArr* a = rt_arr_new(2, m ? m->len : 0);
    if (!m || !m->len) return a;
    KeyVal* p = sorted_pairs(m);
    for (int64_t i = 0; i < m->len; ++i) a->data[a->len++] = (int64_t)(intptr_t)rt_retain_str(p[i].k);
    free(p);
    return a;
}

EmbrArr* rt_map_values(const EmbrMap* m) {
    EmbrArr* a = rt_arr_new(m ? m->kind : 0, m ? m->len : 0);
    if (!m || !m->len) return a;
    KeyVal* p = sorted_pairs(m);
    for (int64_t i = 0; i < m->len; ++i) {
        a->data[a->len++] = p[i].v;
        if (m->kind >= 2) rt_elem_retain(m->kind, p[i].v);
    }
    free(p);
    return a;
}

/* ---- errors that are caught */

static EmbrStr* str_from(const char* d, int64_t n) {
    EmbrStr* s = str_alloc(n, n);
    memcpy(s->data, d, (size_t)n);
    return s;
}

EmbrErr* rt_err_take(RtCtx* cx) {
    EmbrErr* e = xalloc(sizeof *e + (size_t)cx->ntrace * sizeof(EmbrFrame));
    e->rc = 1;
    e->file = cx->file; e->ntrace = cx->ntrace;
    for (int i = 0; i < cx->ntrace; ++i) e->trace[i] = cx->trace[i];
    e->msg = str_from(cx->msg, cx->msglen);
    e->kind = str_from(cx->kind, (int64_t)strlen(cx->kind));
    e->line = (double)cx->line;
    free(cx->msg);
    cx->msg = NULL; cx->msglen = 0; cx->err = 0; cx->ntrace = 0; cx->dropped = 0;
    return e;
}

void rt_free_err(EmbrErr* e) {
    rt_release_str(e->msg);
    rt_release_str(e->kind);
    free(e);
}

/* ---- closures */

EmbrCell* rt_cell_new(int64_t kind) {
    EmbrCell* c = xalloc(sizeof *c);
    c->rc = 1; c->kind = kind;
    c->v.i = 0;
    if (kind == 2) c->v.s = rt_empty_str;
    else if (kind >= 3) c->v.c = NULL;
    return c;
}

void rt_free_cell(EmbrCell* c) {
    switch (c->kind) {
    case 2: rt_release_str(c->v.s); break;
    case 3: rt_release_arr(c->v.a); break;
    case 4: rt_release_map(c->v.m); break;
    case 5: rt_release_err(c->v.e); break;
    case 6: rt_release_clo(c->v.c); break;
    case 7: rt_release_any(c->v.y); break;
    default: break;
    }
    free(c);
}

EmbrClosure* rt_clo_new(void* fn, const char* name, int64_t n) {
    EmbrClosure* c = xalloc(sizeof *c + (size_t)n * sizeof(EmbrCell*));
    c->rc = 1; c->fn = fn; c->name = name; c->n = n;
    return c;
}

void rt_free_clo(EmbrClosure* c) {
    for (int64_t i = 0; i < c->n; ++i) rt_release_cell(c->cells[i]);
    free(c);
}

void rt_break_cell_cycle(EmbrCell* c) {
    EmbrClosure* cl = c->v.c;
    if (!cl || cl->rc != 1) return;
    for (int64_t i = 0; i < cl->n; ++i) {
        if (cl->cells[i] != c) continue;
        c->v.c = NULL;
        rt_release_clo(cl);      /* frees it, which lets go of its count of this cell */
        return;
    }
}

/* ---- constants of a plugin */

EmbrStr* rt_str_from(const char* data, int64_t len) {
    EmbrStr* s = str_alloc(len, len);
    memcpy(s->data, data, (size_t)len);
    return s;
}

void rt_immortal_str(EmbrStr* s) { if (s && s->rc < EMBR_IMMORTAL) s->rc = EMBR_IMMORTAL; }

void rt_immortal_arr(EmbrArr* a) {
    if (!a || a->rc >= EMBR_IMMORTAL) return;
    a->rc = EMBR_IMMORTAL;
    if (a->kind == 2) for (int64_t i = 0; i < a->len; ++i) rt_immortal_str((EmbrStr*)(intptr_t)a->data[i]);
    else if (a->kind == 3) for (int64_t i = 0; i < a->len; ++i) rt_immortal_arr((EmbrArr*)(intptr_t)a->data[i]);
    else if (a->kind == 4) for (int64_t i = 0; i < a->len; ++i) rt_immortal_map((EmbrMap*)(intptr_t)a->data[i]);
    else if (a->kind == 5) for (int64_t i = 0; i < a->len; ++i) rt_immortal_any((EmbrAny*)(intptr_t)a->data[i]);
}

void rt_immortal_map(EmbrMap* m) {
    if (!m || m->rc >= EMBR_IMMORTAL) return;
    m->rc = EMBR_IMMORTAL;
    for (int64_t i = 0; i < m->cap; ++i) {
        if (!m->keys[i]) continue;
        rt_immortal_str(m->keys[i]);
        if (m->kind == 2) rt_immortal_str((EmbrStr*)(intptr_t)m->vals[i]);
        else if (m->kind == 3) rt_immortal_arr((EmbrArr*)(intptr_t)m->vals[i]);
        else if (m->kind == 4) rt_immortal_map((EmbrMap*)(intptr_t)m->vals[i]);
        else if (m->kind == 5) rt_immortal_any((EmbrAny*)(intptr_t)m->vals[i]);
    }
}

EmbrStr* rt_str_alloc(int64_t len) { return str_alloc(len, len); }
void* rt_xalloc(size_t n) { return xalloc(n); }

/* backward shift deletion: the entries after the hole that were pushed there by a collision move back, so no tombstones are needed */
int64_t rt_remove_key(EmbrMap* m, const EmbrStr* key) {
    if (!m || !m->cap) return 0;
    int64_t i = map_slot(m, key);
    if (!m->keys[i]) return 0;
    rt_release_str(m->keys[i]);
    if (m->kind >= 2) rt_elem_release(m->kind, m->vals[i]);
    m->keys[i] = NULL; m->vals[i] = 0; m->len--;
    const int64_t mask = m->cap - 1;
    int64_t j = i;
    for (;;) {
        j = (j + 1) & mask;
        if (!m->keys[j]) break;
        const int64_t k = (int64_t)(hash_str(m->keys[j]) & (uint64_t)mask);
        if (i <= j ? (i < k && k <= j) : (i < k || k <= j)) continue;     /* its ideal slot is after the hole: it stays */
        m->keys[i] = m->keys[j]; m->vals[i] = m->vals[j];
        m->keys[j] = NULL; m->vals[j] = 0;
        i = j;
    }
    return 1;
}

/* ---- print with several arguments */

void rt_put_i64(int64_t v) { printf("%lld", (long long)v); }
void rt_put_f64(double d) { char buf[400]; fmt_f64(buf, sizeof buf, d); fputs(buf, stdout); }
void rt_put_str(const EmbrStr* s) { fwrite(s->data, 1, (size_t)s->len, stdout); }
int64_t rt_range_step(RtCtx* cx, int64_t start, int64_t second, int line) {
    if (start == second) { rt_raise(cx, "vm", line, "the step of a range can't be zero: its first two numbers are equal"); return 1; }
    return (int64_t)((uint64_t)second - (uint64_t)start);
}
int64_t rt_range_count(int64_t start, int64_t step, int64_t end) {
    uint64_t diff, mag;
    if (step > 0) { if (end < start) return 0; diff = (uint64_t)end - (uint64_t)start; mag = (uint64_t)step; }
    else          { if (end > start) return 0; diff = (uint64_t)start - (uint64_t)end; mag = (uint64_t)0 - (uint64_t)step; }
    const uint64_t q = diff / mag;
    return q >= (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)q + 1;
}
EmbrArr* rt_range(RtCtx* cx, int64_t start, int64_t second, int64_t has_second, int64_t end, int line) {
    int64_t step = 1;
    if (has_second) { step = rt_range_step(cx, start, second, line); if (cx->err) return NULL; }
    const int64_t n = rt_range_count(start, step, end);
    if (n > ((int64_t)1 << 31)) { rt_raise(cx, "vm", line, "this range has %lld numbers, too many to make as an array (use it in a for loop)", (long long)n); return NULL; }
    EmbrArr* a = rt_arr_new(0, n);
    for (int64_t i = 0; i < n; ++i) a->data[a->len++] = (int64_t)((uint64_t)start + (uint64_t)i * (uint64_t)step);
    return a;
}
/* ---- print of containers. a value is read by its kind (the number kept in the array or map), so nested ones need nothing more. the text goes to a
   FILE, so that the same code gives print and the text form of a value (str, +) */
static void f_put_str(FILE* f, const EmbrStr* s) { fwrite(s->data, 1, (size_t)s->len, f); }
static void f_put_f64(FILE* f, double d) { char buf[400]; fmt_f64(buf, sizeof buf, d); fputs(buf, f); }
static void f_put_arr(FILE* f, const EmbrArr* a);
static void f_put_map(FILE* f, const EmbrMap* m);
static void f_put_any(FILE* f, const EmbrAny* x, int quoted);
static void f_put_slot(FILE* f, int64_t kind, int64_t slot) {
    if (kind == 0) fprintf(f, "%lld", (long long)slot);
    else if (kind == 1) { double d; memcpy(&d, &slot, 8); f_put_f64(f, d); }
    else if (kind == 2) { const EmbrStr* s = (const EmbrStr*)(intptr_t)slot; fputc('"', f); f_put_str(f, s); fputc('"', f); }
    else if (kind == 3) f_put_arr(f, (const EmbrArr*)(intptr_t)slot);
    else if (kind == 4) f_put_map(f, (const EmbrMap*)(intptr_t)slot);
    else f_put_any(f, (const EmbrAny*)(intptr_t)slot, 1);
}
static void f_put_arr(FILE* f, const EmbrArr* a) {
    fputc('[', f);
    for (int64_t i = 0; i < rt_arr_len(a); ++i) { if (i) fputs(", ", f); f_put_slot(f, a->kind, a->data[i]); }
    fputc(']', f);
}
static void f_put_map(FILE* f, const EmbrMap* m) {
    fputc('{', f);
    if (m && m->len) {
        KeyVal* p = sorted_pairs(m);
        for (int64_t i = 0; i < m->len; ++i) {
            if (i) fputs(", ", f);
            f_put_str(f, p[i].k);
            fputs(": ", f);
            f_put_slot(f, m->kind, p[i].v);
        }
        free(p);
    }
    fputc('}', f);
}
/* a str on its own is printed as it is, inside an array or a map it is in quotes */
static void f_put_any(FILE* f, const EmbrAny* x, int quoted) {
    if (!x) { fputs("0", f); return; }
    switch (x->tag) {
    case 0: fprintf(f, "%lld", (long long)x->v.i); break;
    case 1: f_put_f64(f, x->v.f); break;
    case 2: if (quoted) { fputc('"', f); f_put_str(f, x->v.s); fputc('"', f); } else f_put_str(f, x->v.s); break;
    case 3: f_put_arr(f, x->v.a); break;
    case 6: fprintf(f, "<%s 0x%llx>", x->v.h->tag, (unsigned long long)(uintptr_t)x->v.h->p); break;
    default: f_put_map(f, x->v.m); break;
    }
}
void rt_put_arr(const EmbrArr* a) { f_put_arr(stdout, a); }
void rt_put_map(const EmbrMap* m) { f_put_map(stdout, m); }
void rt_put_any(const EmbrAny* x) { f_put_any(stdout, x, 0); }
/* the text form of a value of any kind: what print shows, and what str() gives */
EmbrStr* rt_any_text(const EmbrAny* x) {
    char* buf = NULL; size_t len = 0;
    FILE* f = open_memstream(&buf, &len);
    if (!f) return rt_str_from("", 0);
    f_put_any(f, x, 0);
    fclose(f);
    EmbrStr* s = rt_str_from(buf ? buf : "", (int64_t)len);
    free(buf);
    return s;
}
EmbrArr* rt_err_trace(const EmbrErr* e) {
    EmbrArr* r = rt_arr_new(4, e->ntrace);
    for (int i = 0; i < e->ntrace; ++i) {
        EmbrMap* m = rt_map_new(5);
        EmbrStr* k;
        EmbrStr* v;
        EmbrAny* x;
        { const char* ff = e->trace[i].file ? e->trace[i].file : e->file; k = rt_str_from("file", 4); v = rt_str_from(ff ? ff : "", (int64_t)strlen(ff ? ff : "")); }
        x = rt_any_of_str(v); rt_map_put_slot(m, k, (int64_t)(intptr_t)x); rt_release_str(k); rt_release_str(v);
        k = rt_str_from("fn", 2); v = rt_str_from(e->trace[i].fn, (int64_t)strlen(e->trace[i].fn));
        x = rt_any_of_str(v); rt_map_put_slot(m, k, (int64_t)(intptr_t)x); rt_release_str(k); rt_release_str(v);
        k = rt_str_from("line", 4);
        x = rt_any_of_int(e->trace[i].line); rt_map_put_slot(m, k, (int64_t)(intptr_t)x); rt_release_str(k);
        r->data[r->len++] = (int64_t)(intptr_t)m;
    }
    return r;
}
void rt_put_err(const EmbrErr* e) {
    fputs("{kind: \"", stdout); rt_put_str(e->kind);
    fputs("\", line: ", stdout); rt_put_f64(e->line);
    fputs(", message: \"", stdout); rt_put_str(e->msg);
    fputs("\", trace: [", stdout);
    for (int i = 0; i < e->ntrace; ++i) {
        if (i) fputs(", ", stdout);
        { const char* ff = e->trace[i].file ? e->trace[i].file : e->file; printf("{file: \"%s\", fn: \"%s\", line: %d}", ff ? ff : "", e->trace[i].fn, e->trace[i].line); }
    }
    fputs("]}", stdout);
}
/* merge sort of the slots in `v` (length n) with a scratch buffer. the right one is taken only when it is strictly first, so equal ones keep their order */
static int merge_sort(RtCtx* cx, int64_t* v, int64_t* tmp, int64_t n, void* ctx, RtCmp cmp, int desc) {
    if (n < 2) return 1;
    const int64_t mid = n / 2;
    if (!merge_sort(cx, v, tmp, mid, ctx, cmp, desc) || !merge_sort(cx, v + mid, tmp, n - mid, ctx, cmp, desc)) return 0;
    int64_t i = 0, j = mid, k = 0;
    while (i < mid && j < n) {
        const int c = desc ? cmp(cx, ctx, v[i], v[j]) : cmp(cx, ctx, v[j], v[i]);
        if (cx->err) return 0;
        const int right_first = c < 0;      /* (descending asks the comparator the other way round, so equal ones still keep their order) */
        tmp[k++] = right_first ? v[j++] : v[i++];
    }
    while (i < mid) tmp[k++] = v[i++];
    while (j < n) tmp[k++] = v[j++];
    memcpy(v, tmp, (size_t)n * 8);
    return 1;
}
static EmbrArr* copy_arr(const EmbrArr* a) {
    EmbrArr* r = rt_arr_new(a ? a->kind : 0, rt_arr_len(a));
    for (int64_t i = 0; i < rt_arr_len(a); ++i) { r->data[r->len++] = a->data[i]; if (a->kind >= 2) rt_elem_retain(a->kind, a->data[i]); }
    return r;
}
EmbrArr* rt_sort_cmp(RtCtx* cx, const EmbrArr* a, void* ctx, RtCmp cmp, int64_t descending) {
    RT_STAT(sort_call);
    EmbrArr* r = copy_arr(a);
    if (r->len > 1) {
        int64_t* tmp = xalloc((size_t)r->len * 8);
        const int ok = merge_sort(cx, r->data, tmp, r->len, ctx, cmp, descending != 0);
        free(tmp);
        if (!ok) { rt_release_arr(r); return NULL; }
    }
    return r;
}
typedef struct { const int64_t* w; int isf; } ByWeight;
static int cmp_weight(RtCtx* cx, void* ctx, int64_t x, int64_t y) {
    (void)cx;
    const ByWeight* bw = ctx;      /* x and y are positions in the weights */
    if (bw->isf) { double p, q; memcpy(&p, &bw->w[x], 8); memcpy(&q, &bw->w[y], 8); return p < q ? -1 : p > q; }
    return bw->w[x] < bw->w[y] ? -1 : bw->w[x] > bw->w[y];
}
static EmbrArr* sort_by(const EmbrArr* a, const EmbrArr* weights, int64_t descending, int isf) {
    const int64_t n = rt_arr_len(a);
    RtCtx none; memset(&none, 0, sizeof none);
    int64_t* pos = xalloc((size_t)(n ? n : 1) * 8);
    int64_t* tmp = xalloc((size_t)(n ? n : 1) * 8);
    for (int64_t i = 0; i < n; ++i) pos[i] = i;
    ByWeight bw = {weights ? weights->data : NULL, isf};
    merge_sort(&none, pos, tmp, n, &bw, cmp_weight, descending != 0);
    EmbrArr* r = rt_arr_new(a ? a->kind : 0, n);
    for (int64_t i = 0; i < n; ++i) { r->data[r->len++] = a->data[pos[i]]; if (a->kind >= 2) rt_elem_retain(a->kind, a->data[pos[i]]); }
    free(pos); free(tmp);
    return r;
}
EmbrArr* rt_sort_by_i64(const EmbrArr* a, const EmbrArr* weights, int64_t descending) { return sort_by(a, weights, descending, 0); }
EmbrArr* rt_sort_by_f64(const EmbrArr* a, const EmbrArr* weights, int64_t descending) { return sort_by(a, weights, descending, 1); }

/* ---- maps made from maps. a value is retained by its kind */
EmbrMap* rt_map_merge(const EmbrMap* a, const EmbrMap* b, int64_t kind) {
    EmbrMap* c = rt_map_new(kind);
    const EmbrMap* both[2] = {a, b};
    for (int w = 0; w < 2; ++w) {
        const EmbrMap* m = both[w];
        if (!m) continue;
        for (int64_t i = 0; i < m->cap; ++i) if (m->keys[i]) { if (kind >= 2) rt_elem_retain(kind, m->vals[i]); map_put(c, m->keys[i], m->vals[i]); }
    }
    return c;
}
static EmbrMap* map_filter(const EmbrMap* m, const EmbrArr* keys, int64_t kind, int keep) {
    EmbrMap* c = rt_map_new(kind);
    if (!m) return c;
    for (int64_t i = 0; i < m->cap; ++i) {
        if (!m->keys[i]) continue;
        int listed = 0;
        for (int64_t j = 0; j < rt_arr_len(keys) && !listed; ++j) listed = rt_str_eq(m->keys[i], (const EmbrStr*)(intptr_t)keys->data[j]);
        if (listed == keep) { if (kind >= 2) rt_elem_retain(kind, m->vals[i]); map_put(c, m->keys[i], m->vals[i]); }
    }
    return c;
}
EmbrMap* rt_map_pick(const EmbrMap* m, const EmbrArr* keys, int64_t kind) { return map_filter(m, keys, kind, 1); }
EmbrMap* rt_map_omit(const EmbrMap* m, const EmbrArr* keys, int64_t kind) { return map_filter(m, keys, kind, 0); }

void rt_min_do_empty(RtCtx* cx, int line) { rt_raise(cx, "min_do", line, "empty array"); }
void rt_max_do_empty(RtCtx* cx, int line) { rt_raise(cx, "max_do", line, "empty array"); }
void rt_find_missing(RtCtx* cx, int line) { rt_raise(cx, "find_do", line, "no element matches (pass a default to avoid this error)"); }
void rt_unpack_check(RtCtx* cx, const EmbrArr* a, int64_t n, int line) {
    const int64_t have = rt_arr_len(a);
    if (have < n) rt_raise(cx, "vm", line, "cannot unpack %lld variable%s from an array of %lld element%s", (long long)n, n == 1 ? "" : "s", (long long)have, have == 1 ? "" : "s");
}
void rt_put_sep(void) { fputc(' ', stdout); }
void rt_put_end(void) { fputc('\n', stdout); }
