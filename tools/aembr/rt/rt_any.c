/* rt_any.c
   values of no fixed type, for compiled code (see tools/aembr/DESIGN.md). an any is a counted box with a tag and a value; an array or a map that holds
   anys has kind 5. the operators work as the VM's do on its values: the same results and the same error messages.

   good to know
     - a box is never changed once it is made. a change to the array or map in it makes a new box (rt_any_set_index), unless nobody else holds it
     - an array of ints that is handed to an any stays an array of ints inside it. writing a str into it turns it into an array of anys first */
#include "rt.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static EmbrAny* box(int64_t tag) {
    RT_STAT(alloc_any);
    EmbrAny* x = rt_xalloc(sizeof *x);
    x->rc = 1; x->tag = tag; x->v.i = 0;
    return x;
}

EmbrAny* rt_any_of_int(int64_t i) { EmbrAny* x = box(0); x->v.i = i; return x; }
EmbrAny* rt_any_of_float(double d) { EmbrAny* x = box(1); x->v.f = d; return x; }
EmbrAny* rt_any_of_str(const EmbrStr* s) { EmbrAny* x = box(2); x->v.s = rt_retain_str((EmbrStr*)(s ? s : rt_empty_str)); return x; }
EmbrAny* rt_any_of_arr(EmbrArr* a) { EmbrAny* x = box(3); x->v.a = a ? rt_retain_arr(a) : rt_arr_new(5, 0); return x; }
EmbrAny* rt_any_of_map(EmbrMap* m) { EmbrAny* x = box(4); x->v.m = m ? rt_retain_map(m) : rt_map_new(5); return x; }

EmbrAny* rt_any_of_ptr(void* p, const char* tag, void (*fin)(void*)) {
    EmbrAny* x = box(6);
    x->v.h = rt_xalloc(sizeof *x->v.h);
    x->v.h->p = p; x->v.h->fin = fin;
    snprintf(x->v.h->tag, sizeof x->v.h->tag, "%s", tag ? tag : "");
    return x;
}

void rt_free_any(EmbrAny* x) {
    RT_STAT(free_any);
    switch (x->tag) {
    case 6: if (x->v.h->fin) x->v.h->fin(x->v.h->p); free(x->v.h); break;
    case 2: rt_release_str(x->v.s); break;
    case 3: rt_release_arr(x->v.a); break;
    case 4: rt_release_map(x->v.m); break;
    default: break;
    }
    free(x);
}

void rt_immortal_any(EmbrAny* x) {
    if (!x || x->rc >= EMBR_IMMORTAL) return;
    x->rc = EMBR_IMMORTAL;
    switch (x->tag) {
    case 2: rt_immortal_str(x->v.s); break;
    case 3: rt_immortal_arr(x->v.a); break;
    case 4: rt_immortal_map(x->v.m); break;
    default: break;
    }
}

EmbrAny* rt_any_of_slot(int64_t kind, int64_t slot) {
    switch (kind) {
    case 0: return rt_any_of_int(slot);
    case 1: { double d; memcpy(&d, &slot, 8); return rt_any_of_float(d); }
    case 2: return rt_any_of_str((const EmbrStr*)(intptr_t)slot);
    case 3: return rt_any_of_arr((EmbrArr*)(intptr_t)slot);
    case 4: return rt_any_of_map((EmbrMap*)(intptr_t)slot);
    default: return rt_retain_any((EmbrAny*)(intptr_t)slot);
    }
}

/* the value as a slot of an array or map of this kind, with the count that slot keeps. 0 when the value is not of that kind */
static int to_slot(const EmbrAny* v, int64_t kind, int64_t* slot) {
    if (kind == 5) { *slot = (int64_t)(intptr_t)rt_retain_any((EmbrAny*)v); return 1; }
    if (v->tag != kind) return 0;
    switch (kind) {
    case 0: *slot = v->v.i; break;
    case 1: memcpy(slot, &v->v.f, 8); break;
    case 2: *slot = (int64_t)(intptr_t)rt_retain_str(v->v.s); break;
    case 3: *slot = (int64_t)(intptr_t)rt_retain_arr(v->v.a); break;
    default: *slot = (int64_t)(intptr_t)rt_retain_map(v->v.m); break;
    }
    return 1;
}

EmbrArr* rt_arr_promote_any(EmbrArr* a) {
    if (a->kind == 5) return a;
    for (int64_t i = 0; i < a->len; ++i) {
        EmbrAny* b = rt_any_of_slot(a->kind, a->data[i]);
        if (a->kind >= 2) rt_elem_release(a->kind, a->data[i]);
        a->data[i] = (int64_t)(intptr_t)b;
    }
    a->kind = 5;
    return a;
}
static void map_promote_any(EmbrMap* m) {
    if (m->kind == 5) return;
    for (int64_t i = 0; i < m->cap; ++i) {
        if (!m->keys[i]) continue;
        EmbrAny* b = rt_any_of_slot(m->kind, m->vals[i]);
        if (m->kind >= 2) rt_elem_release(m->kind, m->vals[i]);
        m->vals[i] = (int64_t)(intptr_t)b;
    }
    m->kind = 5;
}

/* ---- names and errors */

static const char* type_word(const EmbrAny* x) {
    if (!x) return "nothing";
    static const char* const names[] = {"int", "number", "string", "array", "map", "function", "pointer"};
    return names[x->tag];
}
EmbrStr* rt_any_type_name(const EmbrAny* x) { const char* w = type_word(x); return rt_str_from(w, (int64_t)strlen(w)); }

static void bad_operands(RtCtx* cx, const char* op, const EmbrAny* a, const EmbrAny* b, int line) {
    rt_raise(cx, "vm", line, "unsupported operands for '%s': %s and %s", op, type_word(a), type_word(b));
}

/* ---- casts: the types are known where the any is used. `tmpl` is the message when the value is not of the type: %W is the type wanted and %G the type it
   was, in the VM's words ("argument 'n' to 'f': expected %W but got %G") */

static void cast_fail(RtCtx* cx, int line, const EmbrStr* tmpl, const char* want, const char* got) {
    char buf[512];
    size_t n = 0;
    const char* t = tmpl ? tmpl->data : "expected %W but got %G";
    for (; *t && n + 40 < sizeof buf; ++t) {
        if (t[0] == '%' && (t[1] == 'W' || t[1] == 'G')) {
            const char* w = t[1] == 'W' ? want : got;
            n += (size_t)snprintf(buf + n, sizeof buf - n, "%s", w);
            ++t;
        } else buf[n++] = *t;
    }
    buf[n] = 0;
    rt_raise_dyn_kind(cx, "vm", line, buf);
}

int64_t rt_any_to_i64(RtCtx* cx, const EmbrAny* x, const EmbrStr* tmpl, int line) {
    RT_STAT(any_conv);
    if (x && x->tag == 0) return x->v.i;
    cast_fail(cx, line, tmpl, "int", type_word(x));
    return 0;
}
double rt_any_to_f64(RtCtx* cx, const EmbrAny* x, const EmbrStr* tmpl, int line) {
    RT_STAT(any_conv);
    if (x && x->tag == 1) return x->v.f;
    cast_fail(cx, line, tmpl, "number", type_word(x));
    return 0.0;
}
EmbrStr* rt_any_to_str(RtCtx* cx, const EmbrAny* x, const EmbrStr* tmpl, int line) {
    RT_STAT(any_conv);
    if (x && x->tag == 2) return rt_retain_str(x->v.s);
    cast_fail(cx, line, tmpl, "string", type_word(x));
    return NULL;
}

static int64_t kind_of_desc(char c) { return c == 'i' ? 0 : c == 'f' ? 1 : c == 's' ? 2 : c == 'A' ? 3 : c == 'M' ? 4 : 5; }

static int conv(RtCtx* cx, const EmbrAny* v, const char* d, int64_t* slot, const EmbrStr* tmpl, int line);

static EmbrArr* conv_arr(RtCtx* cx, const EmbrArr* src, const char* elem, const EmbrStr* tmpl, int line) {
    const int64_t kind = kind_of_desc(*elem);
    if (src->kind == kind && kind <= 2) return rt_retain_arr((EmbrArr*)src);              /* a flat array of the right kind is shared */
    EmbrArr* r = rt_arr_new(kind, src->len);
    for (int64_t i = 0; i < src->len; ++i) {
        EmbrAny* b = rt_any_of_slot(src->kind, src->data[i]);
        int64_t slot = 0;
        const int ok = conv(cx, b, elem, &slot, tmpl, line);
        rt_release_any(b);
        if (!ok) { rt_release_arr(r); return NULL; }
        r->data[r->len++] = slot;
    }
    return r;
}
static EmbrMap* conv_map(RtCtx* cx, const EmbrMap* src, const char* elem, const EmbrStr* tmpl, int line) {
    const int64_t kind = kind_of_desc(*elem);
    if (src->kind == kind && kind <= 2) return rt_retain_map((EmbrMap*)src);
    EmbrMap* r = rt_map_new(kind);
    for (int64_t i = 0; i < src->cap; ++i) {
        if (!src->keys[i]) continue;
        EmbrAny* b = rt_any_of_slot(src->kind, src->vals[i]);
        int64_t slot = 0;
        const int ok = conv(cx, b, elem, &slot, tmpl, line);
        rt_release_any(b);
        if (!ok) { rt_release_map(r); return NULL; }
        rt_map_put_slot(r, src->keys[i], slot);
    }
    return r;
}
/* the value as the type at d: its slot (with a count if it is a pointer), or 0 after raising */
static int conv(RtCtx* cx, const EmbrAny* v, const char* d, int64_t* slot, const EmbrStr* tmpl, int line) {
    switch (*d) {
    case 'i': if (v && v->tag == 0) { *slot = v->v.i; return 1; } break;
    case 'f': if (v && v->tag == 1) { memcpy(slot, &v->v.f, 8); return 1; } break;
    case 's': if (v && v->tag == 2) { *slot = (int64_t)(intptr_t)rt_retain_str(v->v.s); return 1; } break;
    case 'a': if (v) { *slot = (int64_t)(intptr_t)rt_retain_any((EmbrAny*)v); return 1; } break;
    case 'A': if (v && v->tag == 3) { EmbrArr* r = conv_arr(cx, v->v.a, d + 1, tmpl, line); if (!r) return 0; *slot = (int64_t)(intptr_t)r; return 1; } break;
    case 'M': if (v && v->tag == 4) { EmbrMap* r = conv_map(cx, v->v.m, d + 1, tmpl, line); if (!r) return 0; *slot = (int64_t)(intptr_t)r; return 1; } break;
    default: break;
    }
    if (!cx->err) {
        const char* want = *d == 'i' ? "int" : *d == 'f' ? "number" : *d == 's' ? "string" : *d == 'A' ? "array" : "map";
        cast_fail(cx, line, tmpl, want, type_word(v));
    }
    return 0;
}
EmbrArr* rt_any_to_arr(RtCtx* cx, const EmbrAny* x, const char* elem, const EmbrStr* tmpl, int line) {
    RT_STAT(any_conv);
    if (x && x->tag == 3) return conv_arr(cx, x->v.a, elem, tmpl, line);
    cast_fail(cx, line, tmpl, "array", type_word(x));
    return NULL;
}
EmbrMap* rt_any_to_map(RtCtx* cx, const EmbrAny* x, const char* elem, const EmbrStr* tmpl, int line) {
    RT_STAT(any_conv);
    if (x && x->tag == 4) return conv_map(cx, x->v.m, elem, tmpl, line);
    cast_fail(cx, line, tmpl, "map", type_word(x));
    return NULL;
}

/* ---- arithmetic. an int that does not fit becomes a float, as in the VM */

#define NUM(x) ((x) && ((x)->tag == 0 || (x)->tag == 1))
static double as_d(const EmbrAny* x) { return x->tag == 0 ? (double)x->v.i : x->v.f; }

EmbrAny* rt_any_add(RtCtx* cx, const EmbrAny* a, const EmbrAny* b, int line) {
    RT_STAT(any_op);
    if (NUM(a) && NUM(b)) {
        int64_t r;
        if (a->tag == 0 && b->tag == 0) return __builtin_add_overflow(a->v.i, b->v.i, &r) ? rt_any_of_float((double)a->v.i + (double)b->v.i) : rt_any_of_int(r);
        return rt_any_of_float(as_d(a) + as_d(b));
    }
    if ((a && a->tag == 2) || (b && b->tag == 2)) {          /* a str with anything: the text of both */
        EmbrStr* l = rt_any_text(a);
        EmbrStr* r = rt_any_text(b);
        EmbrStr* s = rt_str_concat(l, r);
        rt_release_str(l); rt_release_str(r);
        EmbrAny* x = rt_any_of_str(s);
        rt_release_str(s);
        return x;
    }
    bad_operands(cx, "+", a, b, line);
    return NULL;
}
EmbrAny* rt_any_sub(RtCtx* cx, const EmbrAny* a, const EmbrAny* b, int line) {
    RT_STAT(any_op);
    if (NUM(a) && NUM(b)) {
        int64_t r;
        if (a->tag == 0 && b->tag == 0) return __builtin_sub_overflow(a->v.i, b->v.i, &r) ? rt_any_of_float((double)a->v.i - (double)b->v.i) : rt_any_of_int(r);
        return rt_any_of_float(as_d(a) - as_d(b));
    }
    bad_operands(cx, "-", a, b, line);
    return NULL;
}
EmbrAny* rt_any_mul(RtCtx* cx, const EmbrAny* a, const EmbrAny* b, int line) {
    RT_STAT(any_op);
    if (NUM(a) && NUM(b)) {
        int64_t r;
        if (a->tag == 0 && b->tag == 0) return __builtin_mul_overflow(a->v.i, b->v.i, &r) ? rt_any_of_float((double)a->v.i * (double)b->v.i) : rt_any_of_int(r);
        return rt_any_of_float(as_d(a) * as_d(b));
    }
    bad_operands(cx, "*", a, b, line);
    return NULL;
}
EmbrAny* rt_any_div(RtCtx* cx, const EmbrAny* a, const EmbrAny* b, int line) {
    RT_STAT(any_op);
    if (NUM(a) && NUM(b)) {
        const double d = as_d(b);
        if (d == 0.0) { rt_raise(cx, "vm", line, "division by zero"); return NULL; }
        return rt_any_of_float(as_d(a) / d);
    }
    bad_operands(cx, "/", a, b, line);
    return NULL;
}
EmbrAny* rt_any_mod(RtCtx* cx, const EmbrAny* a, const EmbrAny* b, int line) {
    RT_STAT(any_op);
    if (NUM(a) && NUM(b)) {
        if (a->tag == 0 && b->tag == 0) {
            if (b->v.i == 0) { rt_raise(cx, "vm", line, "modulo by zero"); return NULL; }
            return rt_any_of_int(b->v.i == -1 ? 0 : a->v.i % b->v.i);
        }
        const double d = as_d(b);
        if (d == 0.0) { rt_raise(cx, "vm", line, "modulo by zero"); return NULL; }
        return rt_any_of_float(fmod(as_d(a), d));
    }
    bad_operands(cx, "%", a, b, line);
    return NULL;
}
EmbrAny* rt_any_neg(RtCtx* cx, const EmbrAny* a, int line) {
    RT_STAT(any_op);
    if (a && a->tag == 0) return a->v.i == INT64_MIN ? rt_any_of_float(-(double)a->v.i) : rt_any_of_int(-a->v.i);
    if (a && a->tag == 1) return rt_any_of_float(-a->v.f);
    rt_raise(cx, "vm", line, "unsupported operand for unary '-': %s", type_word(a));
    return NULL;
}

/* ---- comparison. == compares numbers as numbers, and anything else by its text, as the VM does */

int64_t rt_any_eq(const EmbrAny* a, const EmbrAny* b) {
    RT_STAT(any_op);
    if (NUM(a) && NUM(b)) return (a->tag == 0 && b->tag == 0) ? a->v.i == b->v.i : as_d(a) == as_d(b);
    if (a && b && a->tag == 2 && b->tag == 2) return rt_str_eq(a->v.s, b->v.s);
    EmbrStr* l = rt_any_text(a);
    EmbrStr* r = rt_any_text(b);
    const int64_t eq = rt_str_eq(l, r);
    rt_release_str(l); rt_release_str(r);
    return eq;
}
#define ORDER(NAME, OP, TEXT)                                                                                    \
    int64_t NAME(RtCtx* cx, const EmbrAny* a, const EmbrAny* b, int line) {                                      \
        if (NUM(a) && NUM(b)) return (a->tag == 0 && b->tag == 0) ? a->v.i OP b->v.i : as_d(a) OP as_d(b);       \
        bad_operands(cx, TEXT, a, b, line);                                                                      \
        return 0;                                                                                                \
    }
ORDER(rt_any_lt, <, "<")
ORDER(rt_any_le, <=, "<=")
ORDER(rt_any_gt, >, ">")
ORDER(rt_any_ge, >=, ">=")

int64_t rt_any_truthy(const EmbrAny* a) {
    RT_STAT(any_op);
    if (!a) return 0;
    switch (a->tag) {
    case 0: return a->v.i != 0;
    case 1: return a->v.f != 0.0;
    case 2: return a->v.s->len > 0;
    case 3: return rt_arr_len(a->v.a) > 0;
    case 6: return a->v.h->p != NULL;
    default: return rt_map_len(a->v.m) > 0;
    }
}

/* ---- containers */

EmbrAny* rt_any_index(RtCtx* cx, const EmbrAny* c, const EmbrAny* i, int line) {
    RT_STAT(any_op);
    if (c && c->tag == 4) {
        if (!i || i->tag != 2) { rt_raise(cx, "vm", line, "a map key has to be a string, not %s", type_word(i)); return NULL; }
        int64_t slot;
        if (!rt_map_lookup(c->v.m, i->v.s, &slot)) { rt_raise(cx, "vm", line, "key not found: %.*s", (int)i->v.s->len, i->v.s->data); return NULL; }
        return rt_any_of_slot(c->v.m->kind, slot);
    }
    if (c && (c->tag == 3 || c->tag == 2)) {
        if (!i || i->tag != 0) { rt_raise(cx, "vm", line, "an index into %s has to be a number, not %s", type_word(c), type_word(i)); return NULL; }
        return rt_any_at(cx, c, i->v.i, line);
    }
    rt_raise(cx, "type", line, "cannot index %s", type_word(c));
    return NULL;
}
EmbrAny* rt_any_at(RtCtx* cx, const EmbrAny* c, int64_t i, int line) {
    RT_STAT(any_op);
    if (c && c->tag == 3) {
        const EmbrArr* a = c->v.a;
        if (!a || i < 0 || i >= a->len) { rt_raise(cx, "vm", line, "array index %lld out of bounds", (long long)i); return NULL; }
        return rt_any_of_slot(a->kind, a->data[i]);
    }
    if (c && c->tag == 2) {
        if (i < 0 || i >= c->v.s->len) { rt_raise(cx, "vm", line, "string index %lld out of bounds", (long long)i); return NULL; }
        EmbrStr* ch = rt_str_from(c->v.s->data + i, 1);
        EmbrAny* x = rt_any_of_str(ch);
        rt_release_str(ch);
        return x;
    }
    rt_raise(cx, "type", line, "cannot index %s", type_word(c));
    return NULL;
}
int64_t rt_any_len(RtCtx* cx, const EmbrAny* c, int line) {
    RT_STAT(any_op);
    if (c) {
        if (c->tag == 2) return c->v.s->len;
        if (c->tag == 3) return rt_arr_len(c->v.a);
        if (c->tag == 4) return rt_map_len(c->v.m);
    }
    rt_raise(cx, "len", line, "no __<tag>_len defined and val is not a str/arr/map (got %s)", type_word(c));
    return 0;
}
int64_t rt_any_iter_len(RtCtx* cx, const EmbrAny* c, int line) {
    RT_STAT(any_op);
    if (c && c->tag == 2) return c->v.s->len;
    if (c && c->tag == 3) return rt_arr_len(c->v.a);
    if (c && c->tag == 4) rt_raise(cx, "vm", line, "a for loop over a map is not compiled: the VM visits the keys in an order nobody defines (use for k in keys(m))");
    else rt_raise(cx, "vm", line, "cannot loop over %s", type_word(c));
    return 0;
}

/* the box in *var made one nobody else holds, with the array or map in it unique too */
static EmbrAny* unique_box(EmbrAny** var) {
    EmbrAny* x = *var;
    if (x->rc != 1) {
        EmbrAny* c = box(x->tag);
        if (x->tag == 3) c->v.a = rt_retain_arr(x->v.a); else c->v.m = rt_retain_map(x->v.m);
        rt_release_any(x);
        *var = x = c;
    }
    if (x->tag == 3) x->v.a = rt_arr_unique(x->v.a); else x->v.m = rt_map_unique(x->v.m);
    return x;
}

void rt_any_set_index(RtCtx* cx, EmbrAny** var, const EmbrAny* i, const EmbrAny* v, int line) {
    RT_STAT(any_op);
    EmbrAny* c = *var;
    if (!c || (c->tag != 3 && c->tag != 4)) { rt_raise(cx, "type", line, "cannot index %s", type_word(c)); return; }
    if (c->tag == 4) {
        if (!i || i->tag != 2) { rt_raise(cx, "vm", line, "a map key has to be a string, not %s", type_word(i)); return; }
        c = unique_box(var);
        int64_t slot;
        if (!to_slot(v, c->v.m->kind, &slot)) { map_promote_any(c->v.m); to_slot(v, 5, &slot); }
        rt_map_put_slot(c->v.m, i->v.s, slot);
        return;
    }
    if (!i || i->tag != 0) { rt_raise(cx, "vm", line, "an index into array has to be a number, not %s", type_word(i)); return; }
    if (i->v.i < 0 || i->v.i >= rt_arr_len(c->v.a)) { rt_raise(cx, "vm", line, "array index %lld out of bounds", (long long)i->v.i); return; }
    c = unique_box(var);
    EmbrArr* a = c->v.a;
    int64_t slot;
    if (!to_slot(v, a->kind, &slot)) { rt_arr_promote_any(a); to_slot(v, 5, &slot); }
    rt_elem_release(a->kind, a->data[i->v.i]);
    a->data[i->v.i] = slot;
}

void rt_any_push(RtCtx* cx, EmbrAny** var, const EmbrAny* v, int line) {
    RT_STAT(any_op);
    EmbrAny* c = *var;
    if (!c || c->tag != 3) { rt_raise(cx, "type", line, "push needs an array, this is %s", type_word(c)); return; }
    c = unique_box(var);
    int64_t slot;
    if (!to_slot(v, c->v.a->kind, &slot)) { rt_arr_promote_any(c->v.a); to_slot(v, 5, &slot); }
    c->v.a = rt_arr_push_i64(c->v.a, slot);
}

EmbrArr* rt_any_keys(RtCtx* cx, const EmbrAny* c, int line) {
    RT_STAT(any_op);
    if (c && c->tag == 4) return rt_map_keys(c->v.m);
    rt_raise(cx, "keys", line, "keys needs a map, this is %s", type_word(c));
    return NULL;
}
EmbrArr* rt_any_values(RtCtx* cx, const EmbrAny* c, int line) {
    RT_STAT(any_op);
    if (c && c->tag == 4) return rt_arr_promote_any(rt_map_values(c->v.m));
    rt_raise(cx, "values", line, "values needs a map, this is %s", type_word(c));
    return NULL;
}
int64_t rt_any_has(RtCtx* cx, const EmbrAny* c, const EmbrAny* k, int line) {
    RT_STAT(any_op);
    (void)cx; (void)line;
    if (c && c->tag == 4) return k && k->tag == 2 && rt_map_has(c->v.m, k->v.s);
    if (c && c->tag == 3) return k && k->tag == 0 && k->v.i >= 0 && k->v.i < rt_arr_len(c->v.a);
    return 0;
}

EmbrAny** rt_arr_slot_any(RtCtx* cx, EmbrArr* a, int64_t i, int line) {
    if (!a || i < 0 || i >= a->len) { rt_raise(cx, "vm", line, "array index %lld out of bounds", (long long)i); return NULL; }
    return (EmbrAny**)&a->data[i];
}
EmbrAny** rt_map_slot_any(RtCtx* cx, EmbrMap* m, const EmbrStr* k, int line) {
    if (m && m->cap) {
        const int64_t s = rt_map_slot_index(m, k);
        if (s >= 0) return (EmbrAny**)&m->vals[s];
    }
    rt_raise(cx, "vm", line, "key not found: %.*s", (int)k->len, k->data);
    return NULL;
}
EmbrAny** rt_any_slot(RtCtx* cx, EmbrAny** var, const EmbrAny* idx, int line) {
    EmbrAny* c = *var;
    if (!c || (c->tag != 3 && c->tag != 4)) { rt_raise(cx, "type", line, "cannot index %s", type_word(c)); return NULL; }
    if (c->tag == 4) {
        if (!idx || idx->tag != 2) { rt_raise(cx, "vm", line, "a map key has to be a string, not %s", type_word(idx)); return NULL; }
        c = unique_box(var);
        map_promote_any(c->v.m);
        return rt_map_slot_any(cx, c->v.m, idx->v.s, line);
    }
    if (!idx || idx->tag != 0) { rt_raise(cx, "vm", line, "an index into array has to be a number, not %s", type_word(idx)); return NULL; }
    c = unique_box(var);
    rt_arr_promote_any(c->v.a);
    return rt_arr_slot_any(cx, c->v.a, idx->v.i, line);
}
