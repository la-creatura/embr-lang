/* rt_nat.c
   the natives of embrlib, str and embrmath that compiled code calls, written in C on the runtime's own strings and arrays. each one does what the
   plugin does, with the plugin's own error kind and message (a test runs both and compares). see rt.h for the calling convention. */
#define _GNU_SOURCE
#include "rt.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ALLOC (INT64_C(256) * 1024 * 1024)       /* kMaxScriptAlloc */

/* the plugins turn a number into an int index with these limits, so a huge one clamps instead of overflowing */
static int64_t idx32(int64_t v) { return v < INT_MIN ? INT_MIN : v > INT_MAX ? INT_MAX : v; }

static EmbrArr* arr_like(const EmbrArr* a, int64_t cap) { return rt_arr_new(a->kind, cap); }

/* ---- embrlib */

/* from_chars as the plugin uses it: no sign but '-', no spaces, no hex, and the digits it understood are taken even when text follows */
double rt_num_str(RtCtx* cx, const EmbrStr* s, int line) {
    const char* p = s->data;
    const char* end = s->data + s->len;
    const char* q = p;
    if (q < end && *q == '-') ++q;
    const char* body = q;
    int ok = 0;
    if (end - q >= 3 && strncasecmp(q, "inf", 3) == 0) { q += 3; if (end - q >= 5 && strncasecmp(q, "inity", 5) == 0) q += 5; ok = 1; }
    else if (end - q >= 3 && strncasecmp(q, "nan", 3) == 0) { q += 3; ok = 1; }
    else {
        const char* digits = q;
        while (q < end && isdigit((unsigned char)*q)) ++q;
        const char* frac = q;
        if (q < end && *q == '.') { ++q; while (q < end && isdigit((unsigned char)*q)) ++q; }
        ok = (q > digits) && !(q - digits == 1 && *digits == '.') && (frac > digits || q - frac > 1);
        if (ok && q < end && (*q == 'e' || *q == 'E')) {
            const char* e = q + 1;
            if (e < end && (*e == '+' || *e == '-')) ++e;
            const char* ed = e;
            while (e < end && isdigit((unsigned char)*e)) ++e;
            if (e > ed) q = e;
        }
    }
    (void)body;
    if (!ok) { rt_raise(cx, "num", line, "cannot convert to number: %.*s", (int)s->len, s->data); return 0.0; }
    char buf[512];
    size_t n = (size_t)(q - p);
    if (n >= sizeof buf) { rt_raise(cx, "num", line, "cannot convert to number: %.*s", (int)s->len, s->data); return 0.0; }
    memcpy(buf, p, n);
    buf[n] = 0;
    return strtod(buf, NULL);
}

EmbrStr* rt_slice_str(const EmbrStr* s, int64_t start, int64_t end) {
    const int64_t n = s->len;
    start = idx32(start); end = idx32(end);
    start = start < 0 ? 0 : start > n ? n : start;
    end = end > n ? n : end;
    if (end < start) end = start;
    return rt_str_from(s->data + start, end - start);
}

EmbrStr* rt_str_append_slice(EmbrStr* a, const EmbrStr* s, int64_t start, int64_t end) {
    const int64_t n = s->len;
    start = idx32(start); end = idx32(end);
    start = start < 0 ? 0 : start > n ? n : start;
    end = end > n ? n : end;
    if (end < start) end = start;
    return rt_str_append_bytes(a, s, start, end - start);
}

EmbrArr* rt_slice_arr(const EmbrArr* a, int64_t start, int64_t end) {
    const int64_t n = rt_arr_len(a);
    start = idx32(start); end = idx32(end);
    start = start < 0 ? 0 : start > n ? n : start;
    end = end > n ? n : end;
    if (end < start) end = start;
    EmbrArr* r = a ? arr_like(a, end - start) : rt_arr_new(0, 0);
    for (int64_t i = start; i < end; ++i) {
        r->data[r->len++] = a->data[i];
        if (a->kind >= 2) rt_elem_retain(a->kind, a->data[i]);
    }
    return r;
}

int64_t rt_index_of_i64(const EmbrArr* a, int64_t x) {
    for (int64_t i = 0; i < rt_arr_len(a); ++i) if (a->data[i] == x) return i;
    return -1;
}
int64_t rt_index_of_f64(const EmbrArr* a, double x) {
    for (int64_t i = 0; i < rt_arr_len(a); ++i) { double d; memcpy(&d, &a->data[i], 8); if (d == x) return i; }
    return -1;
}
int64_t rt_index_of_str(const EmbrArr* a, const EmbrStr* x) {
    for (int64_t i = 0; i < rt_arr_len(a); ++i) if (rt_str_eq((const EmbrStr*)(intptr_t)a->data[i], x)) return i;
    return -1;
}

int64_t rt_sum_i64(const EmbrArr* a) {
    uint64_t t = 0;
    for (int64_t i = 0; i < rt_arr_len(a); ++i) t += (uint64_t)a->data[i];
    return (int64_t)t;
}
double rt_sum_f64(const EmbrArr* a) {
    double t = 0.0;
    for (int64_t i = 0; i < rt_arr_len(a); ++i) { double d; memcpy(&d, &a->data[i], 8); t += d; }
    return t;
}

/* min and max compare as numbers, as the plugin does (an int is compared as a double) */
#define EXTREMUM_I(NAME, FNAME, WORD, CMP)                                                                  \
    int64_t NAME(RtCtx* cx, const EmbrArr* a, int line) {                                                   \
        if (rt_arr_len(a) == 0) { rt_raise(cx, FNAME, line, "empty array has no " WORD); return 0; }        \
        int64_t best = a->data[0];                                                                          \
        for (int64_t i = 1; i < a->len; ++i) if ((double)a->data[i] CMP (double)best) best = a->data[i];    \
        return best;                                                                                        \
    }
#define EXTREMUM_F(NAME, FNAME, WORD, CMP)                                                                  \
    double NAME(RtCtx* cx, const EmbrArr* a, int line) {                                                    \
        if (rt_arr_len(a) == 0) { rt_raise(cx, FNAME, line, "empty array has no " WORD); return 0.0; }      \
        double best; memcpy(&best, &a->data[0], 8);                                                         \
        for (int64_t i = 1; i < a->len; ++i) { double d; memcpy(&d, &a->data[i], 8); if (d CMP best) best = d; } \
        return best;                                                                                        \
    }
EXTREMUM_I(rt_min_i64, "min", "min", <)
EXTREMUM_I(rt_max_i64, "max", "max", >)
EXTREMUM_F(rt_min_f64, "min", "min", <)
EXTREMUM_F(rt_max_f64, "max", "max", >)

static int cmp_i_asc(const void* x, const void* y) { double a = (double)*(const int64_t*)x, b = (double)*(const int64_t*)y; return a < b ? -1 : a > b; }
static int cmp_i_desc(const void* x, const void* y) { return cmp_i_asc(y, x); }
static int cmp_f_asc(const void* x, const void* y) { double a, b; memcpy(&a, x, 8); memcpy(&b, y, 8); return a < b ? -1 : a > b; }
static int cmp_f_desc(const void* x, const void* y) { return cmp_f_asc(y, x); }

EmbrArr* rt_sort_arr(const EmbrArr* a, int64_t descending) {
    EmbrArr* r = rt_arr_new(a ? a->kind : 0, rt_arr_len(a));
    if (!a || !a->len) return r;
    memcpy(r->data, a->data, (size_t)a->len * 8);
    r->len = a->len;
    int (*cmp)(const void*, const void*) = a->kind == 1 ? (descending ? cmp_f_desc : cmp_f_asc) : (descending ? cmp_i_desc : cmp_i_asc);
    qsort(r->data, (size_t)r->len, 8, cmp);
    return r;
}

EmbrStr* rt_reverse_str(const EmbrStr* s) {
    EmbrStr* r = rt_str_alloc(s->len);
    for (int64_t i = 0; i < s->len; ++i) r->data[i] = s->data[s->len - 1 - i];
    return r;
}

EmbrArr* rt_reverse_arr(const EmbrArr* a) {
    EmbrArr* r = rt_arr_new(a ? a->kind : 0, rt_arr_len(a));
    for (int64_t i = rt_arr_len(a) - 1; i >= 0; --i) {
        r->data[r->len++] = a->data[i];
        if (a->kind >= 2) rt_elem_retain(a->kind, a->data[i]);
    }
    return r;
}

EmbrArr* rt_concat_arr(const EmbrArr* a, const EmbrArr* b) {
    const int64_t n = rt_arr_len(a), m = rt_arr_len(b);
    EmbrArr* r = rt_arr_new(a ? a->kind : (b ? b->kind : 0), n + m);
    for (int64_t i = 0; i < n; ++i) { r->data[r->len++] = a->data[i]; if (r->kind >= 2) rt_elem_retain(r->kind, a->data[i]); }
    for (int64_t i = 0; i < m; ++i) { r->data[r->len++] = b->data[i]; if (r->kind >= 2) rt_elem_retain(r->kind, b->data[i]); }
    return r;
}

EmbrArr* rt_pop_arr(RtCtx* cx, const EmbrArr* a, int line) {
    if (rt_arr_len(a) == 0) { rt_raise(cx, "pop", line, "cannot pop empty array"); return NULL; }
    return rt_slice_arr(a, 0, a->len - 1);
}

int64_t rt_has_arr(const EmbrArr* a, int64_t i) { return i >= 0 && i < rt_arr_len(a); }

int64_t rt_remove_last_i64(RtCtx* cx, EmbrArr* a, int line) {
    if (rt_arr_len(a) == 0) { rt_raise(cx, "remove_last", line, "cannot remove from an empty array"); return 0; }
    return a->data[--a->len];
}
double rt_remove_last_f64(RtCtx* cx, EmbrArr* a, int line) {
    if (rt_arr_len(a) == 0) { rt_raise(cx, "remove_last", line, "cannot remove from an empty array"); return 0.0; }
    double d; memcpy(&d, &a->data[--a->len], 8); return d;
}
EmbrStr* rt_remove_last_str(RtCtx* cx, EmbrArr* a, int line) {
    if (rt_arr_len(a) == 0) { rt_raise(cx, "remove_last", line, "cannot remove from an empty array"); return NULL; }
    return (EmbrStr*)(intptr_t)a->data[--a->len];       /* the array's count goes to the caller */
}

EmbrArr* rt_remove_last_arr(RtCtx* cx, EmbrArr* a, int line) {
    if (rt_arr_len(a) == 0) { rt_raise(cx, "remove_last", line, "cannot remove from an empty array"); return NULL; }
    return (EmbrArr*)(intptr_t)a->data[--a->len];       /* the array's count goes to the caller */
}
EmbrMap* rt_remove_last_map(RtCtx* cx, EmbrArr* a, int line) {
    if (rt_arr_len(a) == 0) { rt_raise(cx, "remove_last", line, "cannot remove from an empty array"); return NULL; }
    return (EmbrMap*)(intptr_t)a->data[--a->len];
}

static int bad_pos(RtCtx* cx, const char* fn, int64_t pos, int64_t len, int line, int allowEnd) {
    pos = idx32(pos);
    if (pos < 0 || pos > len || (!allowEnd && pos == len)) {
        rt_raise(cx, fn, line, "position %lld is out of bounds for an array of length %lld", (long long)pos, (long long)len);
        return 1;
    }
    return 0;
}
static int64_t take_at(EmbrArr* a, int64_t pos) {
    int64_t v = a->data[pos];
    memmove(&a->data[pos], &a->data[pos + 1], (size_t)(a->len - pos - 1) * 8);
    --a->len;
    return v;
}
int64_t rt_remove_at_i64(RtCtx* cx, EmbrArr* a, int64_t pos, int line) {
    if (bad_pos(cx, "remove_at", pos, rt_arr_len(a), line, 0)) return 0;
    return take_at(a, idx32(pos));
}
double rt_remove_at_f64(RtCtx* cx, EmbrArr* a, int64_t pos, int line) {
    if (bad_pos(cx, "remove_at", pos, rt_arr_len(a), line, 0)) return 0.0;
    int64_t v = take_at(a, idx32(pos)); double d; memcpy(&d, &v, 8); return d;
}
EmbrStr* rt_remove_at_str(RtCtx* cx, EmbrArr* a, int64_t pos, int line) {
    if (bad_pos(cx, "remove_at", pos, rt_arr_len(a), line, 0)) return NULL;
    return (EmbrStr*)(intptr_t)take_at(a, idx32(pos));
}

EmbrArr* rt_remove_at_arr(RtCtx* cx, EmbrArr* a, int64_t pos, int line) {
    if (bad_pos(cx, "remove_at", pos, rt_arr_len(a), line, 0)) return NULL;
    return (EmbrArr*)(intptr_t)take_at(a, idx32(pos));
}
EmbrMap* rt_remove_at_map(RtCtx* cx, EmbrArr* a, int64_t pos, int line) {
    if (bad_pos(cx, "remove_at", pos, rt_arr_len(a), line, 0)) return NULL;
    return (EmbrMap*)(intptr_t)take_at(a, idx32(pos));
}

static void insert_slot(EmbrArr* a, int64_t pos, int64_t bits) {
    if (a->len == a->cap) {
        int64_t cap = a->cap ? a->cap * 2 : 4;
        int64_t* d = realloc(a->data, (size_t)cap * 8);
        if (!d) { fflush(stdout); fputs("error: out of memory\n", stderr); exit(1); }
        a->data = d; a->cap = cap;
    }
    memmove(&a->data[pos + 1], &a->data[pos], (size_t)(a->len - pos) * 8);
    a->data[pos] = bits;
    ++a->len;
}
int64_t rt_insert_i64(RtCtx* cx, EmbrArr* a, int64_t pos, int64_t x, int line) {
    if (bad_pos(cx, "insert", pos, rt_arr_len(a), line, 1)) return 0;
    insert_slot(a, idx32(pos), x);
    return a->len;
}
int64_t rt_insert_f64(RtCtx* cx, EmbrArr* a, int64_t pos, double x, int line) {
    if (bad_pos(cx, "insert", pos, rt_arr_len(a), line, 1)) return 0;
    int64_t b; memcpy(&b, &x, 8);
    insert_slot(a, idx32(pos), b);
    return a->len;
}
int64_t rt_insert_str(RtCtx* cx, EmbrArr* a, int64_t pos, const EmbrStr* x, int line) {
    if (bad_pos(cx, "insert", pos, rt_arr_len(a), line, 1)) return 0;
    insert_slot(a, idx32(pos), (int64_t)(intptr_t)rt_retain_str((EmbrStr*)x));
    return a->len;
}

/* ---- the forms with a default argument */

EmbrStr* rt_slice_str_from(const EmbrStr* s, int64_t start) { return rt_slice_str(s, start, INT_MAX); }
EmbrArr* rt_slice_arr_from(const EmbrArr* a, int64_t start) { return rt_slice_arr(a, start, INT_MAX); }
EmbrArr* rt_sort_asc(const EmbrArr* a) { return rt_sort_arr(a, 0); }

int64_t rt_insert_arr(RtCtx* cx, EmbrArr* a, int64_t pos, EmbrArr* x, int line) {
    if (bad_pos(cx, "insert", pos, rt_arr_len(a), line, 1)) return 0;
    insert_slot(a, idx32(pos), (int64_t)(intptr_t)rt_retain_arr(x));
    return a->len;
}
int64_t rt_insert_map(RtCtx* cx, EmbrArr* a, int64_t pos, EmbrMap* x, int line) {
    if (bad_pos(cx, "insert", pos, rt_arr_len(a), line, 1)) return 0;
    insert_slot(a, idx32(pos), (int64_t)(intptr_t)rt_retain_map(x));
    return a->len;
}

/* ---- more of embrlib: assert, take, chunk, unique, zip, flatten, enumerate, range */

void rt_assert_i64(RtCtx* cx, int64_t ok, int line) { if (!ok) rt_raise(cx, "assert", line, "assertion failed"); }
void rt_assert_f64(RtCtx* cx, double ok, int line) { if (ok == 0.0) rt_raise(cx, "assert", line, "assertion failed"); }
void rt_assert_msg_i64(RtCtx* cx, int64_t ok, const EmbrStr* msg, int line) { if (!ok) rt_raise_bytes(cx, "assert", line, msg->data, msg->len); }
void rt_assert_msg_f64(RtCtx* cx, double ok, const EmbrStr* msg, int line) { if (ok == 0.0) rt_raise_bytes(cx, "assert", line, msg->data, msg->len); }

static EmbrArr* copy_range(const EmbrArr* a, int64_t from, int64_t to, int64_t kind) {
    EmbrArr* r = rt_arr_new(a ? a->kind : kind, to > from ? to - from : 0);
    for (int64_t i = from; i < to; ++i) { r->data[r->len++] = a->data[i]; if (r->kind >= 2) rt_elem_retain(r->kind, a->data[i]); }
    return r;
}
EmbrArr* rt_take_arr(const EmbrArr* a, int64_t n) {
    const int64_t len = rt_arr_len(a);
    return copy_range(a, 0, n <= 0 ? 0 : n < len ? n : len, 0);
}
EmbrArr* rt_chunk_arr(RtCtx* cx, const EmbrArr* a, int64_t n, int64_t ckind, int line) {
    if (n < 1) { rt_raise(cx, "chunk", line, "n must be >= 1"); return NULL; }
    const int64_t len = rt_arr_len(a);
    EmbrArr* r = rt_arr_new(3, (len + n - 1) / n);
    for (int64_t i = 0; i < len; i += n) {
        EmbrArr* piece = copy_range(a, i, i + n < len ? i + n : len, ckind);
        r->data[r->len++] = (int64_t)(intptr_t)piece;
    }
    return r;
}

/* unique: the first of each distinct element, in order. a table of positions (open addressing) keeps it linear */
static uint64_t slot_hash(int64_t kind, int64_t v) {
    uint64_t h = 1469598103934665603ull;
    if (kind == 2) {
        const EmbrStr* s = (const EmbrStr*)(intptr_t)v;
        for (int64_t i = 0; i < s->len; ++i) { h ^= (unsigned char)s->data[i]; h *= 1099511628211ull; }
        return h;
    }
    if (kind == 1) { double d; memcpy(&d, &v, 8); if (d == 0.0) v = 0; }     /* 0.0 and -0.0 are equal */
    h ^= (uint64_t)v; h *= 1099511628211ull; h ^= h >> 29;
    return h;
}
static int slot_equal(int64_t kind, int64_t x, int64_t y) {
    if (kind == 2) return rt_str_eq((const EmbrStr*)(intptr_t)x, (const EmbrStr*)(intptr_t)y);
    if (kind == 1) { double p, q; memcpy(&p, &x, 8); memcpy(&q, &y, 8); return p == q; }
    return x == y;
}
static EmbrArr* unique_arr(const EmbrArr* a, int64_t kind) {
    const int64_t n = rt_arr_len(a);
    EmbrArr* r = rt_arr_new(kind, n);
    int64_t cap = 8;
    while (cap < n * 2) cap *= 2;
    int64_t* seen = rt_xalloc((size_t)cap * 8);
    for (int64_t i = 0; i < cap; ++i) seen[i] = -1;
    for (int64_t i = 0; i < n; ++i) {
        uint64_t h = slot_hash(kind, a->data[i]) & (uint64_t)(cap - 1);
        int dup = 0;
        while (seen[h] >= 0) {
            if (slot_equal(kind, a->data[seen[h]], a->data[i])) { dup = 1; break; }
            h = (h + 1) & (uint64_t)(cap - 1);
        }
        if (dup) continue;
        seen[h] = i;
        r->data[r->len++] = a->data[i];
        if (kind >= 2) rt_elem_retain(kind, a->data[i]);
    }
    free(seen);
    return r;
}
EmbrArr* rt_unique_i64(const EmbrArr* a) { return unique_arr(a, 0); }
EmbrArr* rt_unique_f64(const EmbrArr* a) { return unique_arr(a, 1); }
EmbrArr* rt_unique_str(const EmbrArr* a) { return unique_arr(a, 2); }

EmbrArr* rt_zip2_arr(const EmbrArr* a, const EmbrArr* b, int64_t ckind) {
    const int64_t n = rt_arr_len(a) < rt_arr_len(b) ? rt_arr_len(a) : rt_arr_len(b);
    EmbrArr* r = rt_arr_new(3, n);
    for (int64_t i = 0; i < n; ++i) {
        EmbrArr* pair = rt_arr_new(ckind, 2);
        pair->data[pair->len++] = a->data[i];
        pair->data[pair->len++] = b->data[i];
        if (ckind >= 2) { rt_elem_retain(ckind, a->data[i]); rt_elem_retain(ckind, b->data[i]); }
        r->data[r->len++] = (int64_t)(intptr_t)pair;
    }
    return r;
}
EmbrArr* rt_flatten_arr(const EmbrArr* a, int64_t kind) {
    EmbrArr* r = rt_arr_new(kind, 0);
    for (int64_t i = 0; i < rt_arr_len(a); ++i) {
        const EmbrArr* inner = (const EmbrArr*)(intptr_t)a->data[i];
        for (int64_t j = 0; j < rt_arr_len(inner); ++j) {
            r = rt_arr_push_i64(r, inner->data[j]);          /* (the slot as it is; a pointer gets its own count below) */
            if (kind >= 2) rt_elem_retain(kind, inner->data[j]);
        }
    }
    return r;
}
EmbrArr* rt_enumerate_from(const EmbrArr* a, int64_t start, int64_t ckind) {
    const int64_t n = rt_arr_len(a);
    EmbrArr* r = rt_arr_new(3, n);
    for (int64_t i = 0; i < n; ++i) {
        EmbrArr* pair = rt_arr_new(ckind, 2);
        pair->data[pair->len++] = (int64_t)((uint64_t)start + (uint64_t)i);
        pair->data[pair->len++] = a->data[i];
        r->data[r->len++] = (int64_t)(intptr_t)pair;
    }
    return r;
}
EmbrArr* rt_enumerate_arr(const EmbrArr* a, int64_t ckind) { return rt_enumerate_from(a, 0, ckind); }
EmbrArr* rt_range_excl(int64_t a, int64_t b) {
    if (a == b) return rt_arr_new(0, 0);
    const uint64_t n = a < b ? (uint64_t)b - (uint64_t)a : (uint64_t)a - (uint64_t)b;
    if (n > ((uint64_t)1 << 31)) { fflush(stdout); fputs("error: range: too many numbers to make as an array\n", stderr); exit(1); }
    EmbrArr* r = rt_arr_new(0, (int64_t)n);
    for (uint64_t i = 0; i < n; ++i) r->data[r->len++] = a < b ? (int64_t)((uint64_t)a + i) : (int64_t)((uint64_t)a - i);
    return r;
}
