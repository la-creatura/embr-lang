/* rt_plugin.h
   what the entry points of a compiled plugin use to copy values between the host (embr, through embr_plugin.h) and the compiled code. plain
   inline C, included only by plugin output, after rt.h and embr_plugin.h.

   good to know
     - a string, array or map from the host is copied in (the host owns its own, only embr may touch it), and a result is copied out. the
       copies are the price of an opaque host value, see tools/aembr/DESIGN.md, one function
     - an element of the wrong type raises an ordinary error in the compiled context, which the entry point turns into embrp_fail */
#ifndef AEMBR_RT_PLUGIN_H
#define AEMBR_RT_PLUGIN_H

#include <string.h>

/* an error in the compiled context becomes an error of the call. frees the message. returns 1 (what an entry point returns) */
static inline int rtp_finish(EmbrCtx* h, RtCtx* cx) {
    if (!cx->err) return 0;
    embrp_fail_kind(h, cx->kind ? cx->kind : "error", cx->msg ? cx->msg : "error");
    free(cx->msg);
    cx->msg = NULL;
    return 1;
}

/* ---- in: the host's value to a compiled one (a new count the caller owns) */

static inline EmbrStr* rtp_in_str(EmbrCtx* h, const EmbrValue* v);

/* the arguments from `from` on, for a variadic parameter */
static inline EmbrArr* rtp_in_rest_str(RtCtx* cx, EmbrCtx* h, const EmbrValue* args, size_t n, size_t from) {
    EmbrArr* a = rt_arr_new(2, n > from ? (int64_t)(n - from) : 0);
    for (size_t i = from; i < n; ++i) {
        if (args[i].tag != EMBR_TAG_STRING) { rt_raise(cx, "vm", 0, "argument %zu is %s, expected str", i + 1, embrp_type_name(h, &args[i])); rt_release_arr(a); return NULL; }
        a->data[a->len++] = (int64_t)(intptr_t)rtp_in_str(h, &args[i]);
    }
    return a;
}
static inline EmbrArr* rtp_in_rest_i64(RtCtx* cx, EmbrCtx* h, const EmbrValue* args, size_t n, size_t from) {
    EmbrArr* a = rt_arr_new(0, n > from ? (int64_t)(n - from) : 0);
    for (size_t i = from; i < n; ++i) {
        if (args[i].tag != EMBR_TAG_INT) { rt_raise(cx, "vm", 0, "argument %zu is %s, expected int", i + 1, embrp_type_name(h, &args[i])); rt_release_arr(a); return NULL; }
        a->data[a->len++] = args[i].u.i;
    }
    return a;
}
static inline EmbrArr* rtp_in_rest_f64(RtCtx* cx, EmbrCtx* h, const EmbrValue* args, size_t n, size_t from) {
    EmbrArr* a = rt_arr_new(1, n > from ? (int64_t)(n - from) : 0);
    for (size_t i = from; i < n; ++i) {
        if (args[i].tag != EMBR_TAG_FLOAT) { rt_raise(cx, "vm", 0, "argument %zu is %s, expected float", i + 1, embrp_type_name(h, &args[i])); rt_release_arr(a); return NULL; }
        memcpy(&a->data[a->len++], &args[i].u.d, 8);
    }
    return a;
}

/* the host a plugin function was called by, for C text: $ctx is rt_host_ctx, $out the slot of the result of a `-> host` function */
static __thread EmbrCtx* rt_host_ctx;
static __thread EmbrValue* rt_host_out;
/* an embrp_ call in C text that gives back nonzero: the function raises, as if the text had called rt_raise */
#define RT_HOST(cx, call) do { if (call) rt_raise((cx), "vm", 0, "the host could not take the value (nested too deeply?)"); } while (0)

/* a value of no fixed type, all the way down. NULL after raising */
static inline EmbrAny* rtp_in_any(RtCtx* cx, EmbrCtx* h, const EmbrValue* v) {
    RT_STAT(in_node);
    switch (v->tag) {
    case EMBR_TAG_INT: return rt_any_of_int(v->u.i);
    case EMBR_TAG_FLOAT: return rt_any_of_float(v->u.d);
    case EMBR_TAG_STRING: { EmbrStr* s = rtp_in_str(h, v); EmbrAny* x = rt_any_of_str(s); rt_release_str(s); return x; }
    case EMBR_TAG_ARRAY: {
        const size_t n = embrp_array_len(h, v);
        EmbrArr* r = rt_arr_new(5, (int64_t)n);
        for (size_t i = 0; i < n; ++i) {
            EmbrAny* x = rtp_in_any(cx, h, embrp_array_at(h, v, i));
            if (!x) { rt_release_arr(r); return NULL; }
            r->data[r->len++] = (int64_t)(intptr_t)x;
        }
        EmbrAny* x = rt_any_of_arr(r);
        rt_release_arr(r);
        return x;
    }
    case EMBR_TAG_MAP: {
        size_t n = 0;
        const EmbrMapEntry* es = embrp_map_entries(h, v, &n);
        EmbrMap* r = rt_map_new(5);
        for (size_t i = 0; i < n; ++i) {
            EmbrAny* x = rtp_in_any(cx, h, es[i].value);
            if (!x) { rt_release_map(r); return NULL; }
            EmbrStr* k = rt_str_from(es[i].key, (int64_t)es[i].key_len);
            rt_map_put_slot(r, k, (int64_t)(intptr_t)x);
            rt_release_str(k);
        }
        EmbrAny* x = rt_any_of_map(r);
        rt_release_map(r);
        return x;
    }
    case EMBR_TAG_POINTER: {
        const char* tag = embrp_ptr_tag(h, v);
        return rt_any_of_ptr(embrp_ptr_get(h, v, tag), tag, NULL);      /* (a handle the host owns stays the host's) */
    }
    default:
        rt_raise(cx, "vm", 0, "a function value cannot be held by compiled code");
        return NULL;
    }
}
/* an array or a map of arrays, maps or anys: read as an any, then checked against the type */
static inline EmbrArr* rtp_in_nested_arr(RtCtx* cx, EmbrCtx* h, const EmbrValue* v, const char* elem) {
    if (v->tag == EMBR_TAG_ARRAY && elem[0] == 'A' && elem[1] == 'a' && !elem[2]) {
        /* rows of cells of no fixed type (csv_stringify): read straight into the array, no box around each row. a row that is not an array takes the general way */
        const size_t n = embrp_array_len(h, v);
        int plain = 1;
        for (size_t i = 0; plain && i < n; ++i) plain = embrp_array_at(h, v, i)->tag == EMBR_TAG_ARRAY;
        if (plain) {
            EmbrArr* r = rt_arr_new(3, (int64_t)n);
            for (size_t i = 0; i < n; ++i) {
                const EmbrValue* rv = embrp_array_at(h, v, i);
                const size_t m = embrp_array_len(h, rv);
                EmbrArr* row = rt_arr_new(5, (int64_t)m);
                for (size_t k = 0; k < m; ++k) {
                    EmbrAny* x = rtp_in_any(cx, h, embrp_array_at(h, rv, k));
                    if (!x) { rt_release_arr(row); rt_release_arr(r); return NULL; }
                    row->data[row->len++] = (int64_t)(intptr_t)x;
                }
                r->data[r->len++] = (int64_t)(intptr_t)row;
            }
            return r;
        }
    }
    EmbrAny* x = rtp_in_any(cx, h, v);
    if (!x) return NULL;
    EmbrArr* r = rt_any_to_arr(cx, x, elem, NULL, 0);
    rt_release_any(x);
    return r;
}
static inline EmbrMap* rtp_in_nested_map(RtCtx* cx, EmbrCtx* h, const EmbrValue* v, const char* elem) {
    EmbrAny* x = rtp_in_any(cx, h, v);
    if (!x) return NULL;
    EmbrMap* r = rt_any_to_map(cx, x, elem, NULL, 0);
    rt_release_any(x);
    return r;
}

static inline EmbrStr* rtp_in_str(EmbrCtx* h, const EmbrValue* v) {
    size_t n = 0;
    const char* p = embrp_str(h, v, &n);
    return rt_str_from(p, (int64_t)n);
}

static inline void rtp_bad_element(RtCtx* cx, EmbrCtx* h, size_t i, const EmbrValue* e, const char* want) {
    rt_raise(cx, "vm", 0, "element %zu is %s, expected %s", i, e ? embrp_type_name(h, e) : "missing", want);
}

/* the elements of a host array, read in one call when the host has it (and one call each when it is older). `d` is NULL for the second way */
static inline size_t rtp_elements(RtCtx* cx, EmbrCtx* h, const EmbrValue* v, const EmbrValue** d) {
    *d = NULL;
    if (!embrp_has_bulk(h)) return embrp_array_len(h, v);
    size_t n = 0;
    *d = embrp_array_data(h, v, &n);
    if (!*d && !cx->err) rt_raise(cx, "vm", 0, "could not read the array");
    return n;
}
static inline const EmbrValue* rtp_element(EmbrCtx* h, const EmbrValue* v, const EmbrValue* d, size_t i) {
    return d ? &d[i] : embrp_array_at(h, v, i);
}

static inline EmbrArr* rtp_in_arr_i64(RtCtx* cx, EmbrCtx* h, const EmbrValue* v) {
    const EmbrValue* d;
    const size_t n = rtp_elements(cx, h, v, &d);
    EmbrArr* a = rt_arr_new(0, (int64_t)n);
    for (size_t i = 0; i < n && !cx->err; ++i) {
        const EmbrValue* e = rtp_element(h, v, d, i);
        if (!e || e->tag != EMBR_TAG_INT) { rtp_bad_element(cx, h, i, e, "int"); break; }
        a->data[a->len++] = e->u.i;
    }
    return a;
}

static inline EmbrArr* rtp_in_arr_f64(RtCtx* cx, EmbrCtx* h, const EmbrValue* v) {
    const EmbrValue* d;
    const size_t n = rtp_elements(cx, h, v, &d);
    EmbrArr* a = rt_arr_new(1, (int64_t)n);
    for (size_t i = 0; i < n && !cx->err; ++i) {
        const EmbrValue* e = rtp_element(h, v, d, i);
        if (!e || e->tag != EMBR_TAG_FLOAT) { rtp_bad_element(cx, h, i, e, "float"); break; }
        memcpy(&a->data[a->len++], &e->u.d, 8);
    }
    return a;
}

static inline EmbrArr* rtp_in_arr_str(RtCtx* cx, EmbrCtx* h, const EmbrValue* v) {
    const EmbrValue* d;
    const size_t n = rtp_elements(cx, h, v, &d);
    EmbrArr* a = rt_arr_new(2, (int64_t)n);
    for (size_t i = 0; i < n && !cx->err; ++i) {
        const EmbrValue* e = rtp_element(h, v, d, i);
        if (!e || e->tag != EMBR_TAG_STRING) { rtp_bad_element(cx, h, i, e, "str"); break; }
        a->data[a->len++] = (int64_t)(intptr_t)rtp_in_str(h, e);
    }
    return a;
}

/* a map, with the kind of its values: 0 int, 1 float, 2 str */
static inline EmbrMap* rtp_in_map(RtCtx* cx, EmbrCtx* h, const EmbrValue* v, int kind) {
    size_t n = 0;
    const EmbrMapEntry* es = embrp_map_entries(h, v, &n);
    EmbrMap* m = rt_map_new(kind);
    for (size_t i = 0; i < n; ++i) {
        const EmbrValue* e = es[i].value;
        EmbrStr* k = rt_str_from(es[i].key, (int64_t)es[i].key_len);
        if (kind == 0 && e->tag == EMBR_TAG_INT) rt_map_set_i64(m, k, e->u.i);
        else if (kind == 1 && e->tag == EMBR_TAG_FLOAT) rt_map_set_f64(m, k, e->u.d);
        else if (kind == 2 && e->tag == EMBR_TAG_STRING) { EmbrStr* s = rtp_in_str(h, e); rt_map_set_str(m, k, s); rt_release_str(s); }
        else { rt_raise(cx, "vm", 0, "the value of \"%.*s\" is %s, expected %s", (int)es[i].key_len, es[i].key, embrp_type_name(h, e),
                        kind == 0 ? "int" : kind == 1 ? "float" : "str"); rt_release_str(k); break; }
        rt_release_str(k);
    }
    return m;
}

/* ---- out: a compiled value into the host's result slot. 0, or nonzero with the error recorded by the host */

static inline int rtp_out_str(EmbrCtx* h, EmbrValue* out, const EmbrStr* s) {
    return embrp_set_str(h, out, s->data, (size_t)s->len);
}

static inline int rtp_out_arr_i64(EmbrCtx* h, EmbrValue* out, const EmbrArr* a) {
    const int64_t n = rt_arr_len(a);
    if (embrp_set_array(h, out, (size_t)n)) return 1;
    if (embrp_has_bulk(h)) return n ? embrp_array_push_ints(h, out, (const int64_t*)a->data, (size_t)n) : 0;
    for (int64_t i = 0; i < n; ++i) if (embrp_array_push_int(h, out, a->data[i])) return 1;
    return 0;
}

static inline int rtp_out_arr_f64(EmbrCtx* h, EmbrValue* out, const EmbrArr* a) {
    const int64_t n = rt_arr_len(a);
    if (embrp_set_array(h, out, (size_t)n)) return 1;
    if (embrp_has_bulk(h)) return n ? embrp_array_push_floats(h, out, (const double*)a->data, (size_t)n) : 0;     /* the slots hold the bits of the doubles */
    for (int64_t i = 0; i < n; ++i) { double d; memcpy(&d, &a->data[i], 8); if (embrp_array_push_float(h, out, d)) return 1; }
    return 0;
}

static inline int rtp_out_arr_str(EmbrCtx* h, EmbrValue* out, const EmbrArr* a) {
    const int64_t n = rt_arr_len(a);
    if (embrp_set_array(h, out, (size_t)n)) return 1;
    for (int64_t i = 0; i < n; ++i) {
        const EmbrStr* s = (const EmbrStr*)(intptr_t)a->data[i];
        if (embrp_array_push_str(h, out, s->data, (size_t)s->len)) return 1;
    }
    return 0;
}

static inline int rtp_out_map(EmbrCtx* h, EmbrValue* out, const EmbrMap* m) {
    if (embrp_set_map(h, out)) return 1;
    if (!m) return 0;
    for (int64_t i = 0; i < m->cap; ++i) {
        if (!m->keys[i]) continue;
        const EmbrStr* k = m->keys[i];
        if (m->kind == 2) {
            const EmbrStr* s = (const EmbrStr*)(intptr_t)m->vals[i];
            if (embrp_map_set_str(h, out, k->data, (size_t)k->len, s->data, (size_t)s->len)) return 1;
        } else {
            EmbrValue* t = embrp_temp(h);
            if (m->kind == 0) { if (embrp_set_int(h, t, m->vals[i])) return 1; }
            else { double d; memcpy(&d, &m->vals[i], 8); if (embrp_set_float(h, t, d)) return 1; }
            if (embrp_map_set(h, out, k->data, (size_t)k->len, t)) return 1;
        }
    }
    return 0;
}

/* a value of no fixed type, all the way down */
static inline int rtp_out_any(EmbrCtx* h, EmbrValue* out, const EmbrAny* x);
/* an array of ints, floats, strs or arrays of those, without a box for each element */
static inline int rtp_out_arrk(EmbrCtx* h, EmbrValue* out, const EmbrArr* a) {
    switch (a ? a->kind : 0) {
    case 0: return rtp_out_arr_i64(h, out, a);
    case 1: return rtp_out_arr_f64(h, out, a);
    case 2: return rtp_out_arr_str(h, out, a);
    case 4: { EmbrAny* x = rt_any_of_arr((EmbrArr*)a); const int bad = rtp_out_any(h, out, x); rt_release_any(x); return bad; }     /* maps: the slow way */
    default: {
        const int64_t n = rt_arr_len(a);
        if (embrp_set_array(h, out, (size_t)n)) return 1;
        for (int64_t i = 0; i < n; ++i) {
            EmbrValue* t = embrp_temp(h);
            if (a->kind == 3 ? rtp_out_arrk(h, t, (const EmbrArr*)(intptr_t)a->data[i]) : rtp_out_any(h, t, (const EmbrAny*)(intptr_t)a->data[i])) return 1;
            if (embrp_array_push(h, out, t)) return 1;
        }
        return 0;
    }
    }
}
static inline int rtp_out_any(EmbrCtx* h, EmbrValue* out, const EmbrAny* x) {
    RT_STAT(out_node);
    if (!x) return embrp_set_int(h, out, 0);
    switch (x->tag) {
    case 0: return embrp_set_int(h, out, x->v.i);
    case 1: return embrp_set_float(h, out, x->v.f);
    case 2: return embrp_set_str(h, out, x->v.s->data, (size_t)x->v.s->len);
    case 3: {
        const EmbrArr* a = x->v.a;
        if (!a || a->kind <= 3 || a->kind == 5) return rtp_out_arrk(h, out, a);
        const int64_t n = rt_arr_len(a);
        if (embrp_set_array(h, out, (size_t)n)) return 1;
        for (int64_t i = 0; i < n; ++i) {
            EmbrAny* e = rt_any_of_slot(a->kind, a->data[i]);
            EmbrValue* t = embrp_temp(h);
            const int bad = rtp_out_any(h, t, e) || embrp_array_push(h, out, t);
            rt_release_any(e);
            if (bad) return 1;
        }
        return 0;
    }
    case 4: {
        const EmbrMap* m = x->v.m;
        if (embrp_set_map(h, out)) return 1;
        for (int64_t i = 0; m && i < m->cap; ++i) {
            if (!m->keys[i]) continue;
            EmbrValue* t = embrp_temp(h);
            if (m->kind == 5) {      /* the values are boxes already */
                if (rtp_out_any(h, t, (const EmbrAny*)(intptr_t)m->vals[i]) || embrp_map_set(h, out, m->keys[i]->data, (size_t)m->keys[i]->len, t)) return 1;
                continue;
            }
            EmbrAny* e = rt_any_of_slot(m->kind, m->vals[i]);
            const int bad = rtp_out_any(h, t, e) || embrp_map_set(h, out, m->keys[i]->data, (size_t)m->keys[i]->len, t);
            rt_release_any(e);
            if (bad) return 1;
        }
        return 0;
    }
    default: return embrp_set_ptr(h, out, x->v.h->p, x->v.h->tag, NULL);
    }
}
static inline int rtp_out_nested_arr(EmbrCtx* h, EmbrValue* out, const EmbrArr* a) {
    EmbrAny* x = rt_any_of_arr((EmbrArr*)a);
    const int bad = rtp_out_any(h, out, x);
    rt_release_any(x);
    return bad;
}
static inline int rtp_out_nested_map(EmbrCtx* h, EmbrValue* out, const EmbrMap* m) {
    EmbrAny* x = rt_any_of_map((EmbrMap*)m);
    const int bad = rtp_out_any(h, out, x);
    rt_release_any(x);
    return bad;
}

#endif
