/* rt.h
   the runtime that every program written by aembr is built with. plain C, no C++ runtime.

   usage (generated code only)
     RtCtx cx; rt_ctx_init(&cx, "prog.embr"); embr_main(&cx); return rt_finish(&cx);

   good to know
     - an error is a flag, not an exception. a function that can raise takes the context as its first parameter, and the caller
       looks at cx->err after every call that can raise (see tools/aembr/DESIGN.md, errors). a frame that sees it set adds a line to
       the trace and returns at once
     - int + - * and unary - wrap around (two's complement), written with unsigned arithmetic so the C compiler's flags do not matter
     - a string or array is a heap object with a reference count (not atomic, a value never crosses threads). a variable owns one
       count. a runtime function borrows its arguments and returns a new reference, except push and append, which take their array
       (so one nobody else holds grows in place)
     - the stack budget is 1 MB. EMBR_STACK_KB in the environment changes it for one run */
#ifndef AEMBR_RT_H
#define AEMBR_RT_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define EMBR_UNLIKELY(x) __builtin_expect(!!(x), 0)

/* ---- counters, for a build made with --stats (the runtime of libaembr_rt_stats.a): what a program did, printed to stderr when it ends if AEMBR_STATS is set.
   without --stats every RT_STAT is nothing */
#ifdef AEMBR_STATS
#define RT_STAT_LIST(X) \
    X(calls) X(chk) X(param_retain) \
    X(retain_str) X(release_str) X(free_str) X(retain_arr) X(release_arr) X(free_arr) X(retain_map) X(release_map) X(free_map) \
    X(retain_any) X(release_any) X(free_any) X(retain_clo) X(release_clo) X(retain_cell) X(release_cell) \
    X(alloc_str) X(alloc_arr) X(alloc_arrdata) X(alloc_map) X(alloc_any) X(alloc_other) X(alloc_bytes) \
    X(str_concat) X(str_concat_bytes) X(str_append_inplace) X(str_append_copy) \
    X(arr_get) X(arr_set) X(arr_push) X(arr_grow) X(arr_cow) X(arr_cow_elems) \
    X(map_put) X(map_find) X(map_grow) X(map_sort) X(map_sort_elems) \
    X(any_op) X(any_conv) X(sort_call) \
    X(in_node) X(out_node)
enum { 
#define X(n) RT_S_##n,
RT_STAT_LIST(X)
#undef X
RT_NSTAT };
extern uint64_t rt_stat_v[RT_NSTAT];
#define RT_STAT(n) (++rt_stat_v[RT_S_##n])
#define RT_STAT_ADD(n, k) (rt_stat_v[RT_S_##n] += (uint64_t)(k))
#define RT_ERR(cx) (RT_STAT(chk), (cx)->err)
#else
#define RT_STAT(n) ((void)0)
#define RT_STAT_ADD(n, k) ((void)0)
#define RT_ERR(cx) ((cx)->err)
#endif
#define EMBR_LIKELY(x) __builtin_expect(!!(x), 1)

#define EMBR_ADD(a, b) ((int64_t)((uint64_t)(a) + (uint64_t)(b)))
#define EMBR_SUB(a, b) ((int64_t)((uint64_t)(a) - (uint64_t)(b)))
#define EMBR_MUL(a, b) ((int64_t)((uint64_t)(a) * (uint64_t)(b)))
#define EMBR_NEG(a)    ((int64_t)(0u - (uint64_t)(a)))

#define EMBR_MAX_TRACE 32

typedef struct { const char* fn; int line; const char* file; } EmbrFrame;    /* file: where the call is written; NULL is the program's own file */

typedef struct RtCtx {
    int         err;                 /* set when something raised */
    int         line;                /* where it was raised */
    const char* kind;                /* "vm" for the runtime's own errors, "error" for error("text") later on */
    char        kindbuf[64];         /* the kind, when it was made at run time (a plugin's own) */
    char*       msg;                 /* malloc'd, msglen bytes and a 0 after them */
    int64_t     msglen;
    uintptr_t   limit;               /* the lowest stack address a function that can raise may run at */
    const char* file;                /* the source file, for the trace */
    int         ntrace;
    int         dropped;             /* frames that did not fit in the trace */
    EmbrFrame   trace[EMBR_MAX_TRACE];
} RtCtx;

void rt_ctx_init(RtCtx* cx, const char* file);
/* the command line of the program: os_args() gives the words after the program name */
void rt_set_args(int argc, char** argv);
extern int rt_argc;
extern char** rt_argv;
/* --fatal-overflow: the stack limit and the context of the program, for a function that has no context of its own */
extern uintptr_t rt_stack_limit_g;
void rt_stack_overflow(int line) __attribute__((noreturn));
static inline int rt_stack_low_g(void) { return (uintptr_t)__builtin_frame_address(0) < rt_stack_limit_g; }
/* the first error wins: once cx->err is set a later raise changes nothing. `kind` has to be a string that lives for the whole run */
void rt_raise(RtCtx* cx, const char* kind, int line, const char* fmt, ...) __attribute__((format(printf, 4, 5)));
void rt_raise_bytes(RtCtx* cx, const char* kind, int line, const char* data, int64_t len);
/* like rt_raise with a message as it is, and a kind that is copied (it may be made at run time) */
void rt_raise_dyn_kind(RtCtx* cx, const char* kind, int line, const char* msg);
/* a native written in the library std (std/<plugin>.embr) raises with line 0; the call site gives it the line of the call */
static inline void rt_fix_line(RtCtx* cx, int line) { if (cx->line == 0) cx->line = line; }
/* a frame the error returned through: `fn` was called at `line` */
void rt_trace(RtCtx* cx, const char* fn, int line, const char* file);
/* flushes output, prints an uncaught error in the VM's shape (message, line, trace), returns the exit code: 0 or 1 */
int  rt_finish(RtCtx* cx);

void rt_print_i64(int64_t v);
void rt_print_f64(double d);

static inline int embr_stack_low(const RtCtx* cx) { return (uintptr_t)__builtin_frame_address(0) < cx->limit; }

/* the operations that can raise. they return 0 after raising, the caller checks cx->err */
static inline double embr_div_f64(RtCtx* cx, double a, double b, int line) {
    if (EMBR_UNLIKELY(b == 0.0)) { rt_raise(cx, "vm", line, "division by zero"); return 0.0; }
    return a / b;
}
/* b is not 0 (the generated code has tested it, and jumped to the handler if it was) */
static inline int64_t embr_mod_i64_nz(int64_t a, int64_t b) { return b == -1 ? 0 : a % b; }
static inline int64_t embr_mod_i64(RtCtx* cx, int64_t a, int64_t b, int line) {
    if (EMBR_UNLIKELY(b == 0)) { rt_raise(cx, "vm", line, "modulo by zero"); return 0; }
    return b == -1 ? 0 : a % b;       /* INT64_MIN % -1 traps on some machines, its answer is 0 */
}
static inline double embr_mod_f64(RtCtx* cx, double a, double b, int line) {
    if (EMBR_UNLIKELY(b == 0.0)) { rt_raise(cx, "vm", line, "modulo by zero"); return 0.0; }
    return fmod(a, b);
}
/* the count is in 0..63 (tested by the generated code) */
static inline int64_t embr_shl_nc(int64_t a, int64_t n) { return (int64_t)((uint64_t)a << n); }
static inline int64_t embr_shr_nc(int64_t a, int64_t n) { return a >> n; }
static inline int64_t embr_shl(RtCtx* cx, int64_t a, int64_t n, int line) {
    if (EMBR_UNLIKELY((uint64_t)n > 63u)) { rt_raise(cx, "vm", line, "shift count %lld is out of range, it has to be 0 to 63", (long long)n); return 0; }
    return (int64_t)((uint64_t)a << n);   /* << wraps */
}
static inline int64_t embr_shr(RtCtx* cx, int64_t a, int64_t n, int line) {
    if (EMBR_UNLIKELY((uint64_t)n > 63u)) { rt_raise(cx, "vm", line, "shift count %lld is out of range, it has to be 0 to 63", (long long)n); return 0; }
    return a >> n;                        /* >> keeps the sign */
}

/* ---- strings and arrays */

#include <stdlib.h>

#define EMBR_IMMORTAL (INT64_C(1) << 40)     /* the count of a literal: it never reaches 0 */

typedef struct { int64_t rc, len, cap; char data[]; } EmbrStr;
/* the elements are 8 byte slots: an int, the bits of a float, or a pointer. kind: 0 int, 1 float, 2 str, 3 arr, 4 map, 5 any (release walks the pointers) */
typedef struct { int64_t rc, len, cap; int64_t* data; int64_t kind; } EmbrArr;

/* keys are strings (open addressing, half full at most, no removal), the values are 8 byte slots like an array's. kind as in EmbrArr */
typedef struct { int64_t rc, len, cap, kind; EmbrStr** keys; int64_t* vals; } EmbrMap;

/* a value of no fixed type: a box with a tag (the same numbers as the kinds below: 0 int, 1 float, 2 str, 3 arr, 4 map) and the value. it is counted like a
   string; the array and the map it holds are counted too, and it is never changed once made (a change makes a new box, see rt_any_set_index) */
/* tag 6 is a handle a plugin made (embr has no bool or nil: json_parse gives true, false and null as typed pointers). the tag is the plugin's name for the
   type, the finalizer (if any) runs when the box is freed */
typedef struct RtPtr { void* p; char tag[48]; void (*fin)(void*); } RtPtr;
typedef struct EmbrAny { int64_t rc; int64_t tag; union { int64_t i; double f; EmbrStr* s; EmbrArr* a; EmbrMap* m; RtPtr* h; } v; } EmbrAny;

extern EmbrStr* const rt_empty_str;          /* what a str variable holds before its first value */

/* a count at or above EMBR_IMMORTAL is never changed: a literal, and a constant a plugin made when it was imported, which several threads can
   read at once. nothing writes to such an object, so there is nothing to race on, and `rc == 1` (the test for "nobody else holds it") is false */
static inline EmbrStr* rt_retain_str(EmbrStr* s) { if (s && s->rc < EMBR_IMMORTAL) { RT_STAT(retain_str); ++s->rc; } return s; }
/* not inlined: when a literal is known at the call, the compiler would warn about freeing it, on a path that is never taken */
static __attribute__((noinline, noclone)) void rt_free_str(EmbrStr* s) { RT_STAT(free_str); free(s); }
static inline void rt_release_str(EmbrStr* s) { if (s && s->rc < EMBR_IMMORTAL) { RT_STAT(release_str); if (--s->rc == 0) rt_free_str(s); } }
static inline void rt_set_str(EmbrStr** d, EmbrStr* v) { rt_retain_str(v); rt_release_str(*d); *d = v; }   /* d = v, keeping its own count */
static inline void rt_own_str(EmbrStr** d, EmbrStr* v) { EmbrStr* o = *d; *d = v; rt_release_str(o); }   /* d takes over v's count */

static inline EmbrArr* rt_retain_arr(EmbrArr* a) { if (a && a->rc < EMBR_IMMORTAL) { RT_STAT(retain_arr); ++a->rc; } return a; }
void rt_free_arr(EmbrArr* a);
static inline void rt_release_arr(EmbrArr* a) { if (a && a->rc < EMBR_IMMORTAL) { RT_STAT(release_arr); if (--a->rc == 0) rt_free_arr(a); } }
static inline void rt_set_arr(EmbrArr** d, EmbrArr* v) { rt_retain_arr(v); rt_release_arr(*d); *d = v; }
static inline void rt_own_arr(EmbrArr** d, EmbrArr* v) { EmbrArr* o = *d; *d = v; rt_release_arr(o); }
static inline int64_t rt_arr_len(const EmbrArr* a) { return a ? a->len : 0; }

static inline EmbrMap* rt_retain_map(EmbrMap* m) { if (m && m->rc < EMBR_IMMORTAL) { RT_STAT(retain_map); ++m->rc; } return m; }
void rt_free_map(EmbrMap* m);
static inline void rt_release_map(EmbrMap* m) { if (m && m->rc < EMBR_IMMORTAL) { RT_STAT(release_map); if (--m->rc == 0) rt_free_map(m); } }

void rt_free_any(EmbrAny* x);
static inline EmbrAny* rt_retain_any(EmbrAny* x) { if (x && x->rc < EMBR_IMMORTAL) { RT_STAT(retain_any); ++x->rc; } return x; }
static inline void rt_release_any(EmbrAny* x) { if (x && x->rc < EMBR_IMMORTAL) { RT_STAT(release_any); if (--x->rc == 0) rt_free_any(x); } }
static inline void rt_set_any(EmbrAny** d, EmbrAny* v) { rt_retain_any(v); rt_release_any(*d); *d = v; }
static inline void rt_own_any(EmbrAny** d, EmbrAny* v) { EmbrAny* o = *d; *d = v; rt_release_any(o); }
void rt_immortal_any(EmbrAny* x);

/* an element of kind 2, 3, 4 or 5 is a pointer to a str, an array, a map or an any that the container holds one count of. these work on one slot */
static inline void rt_elem_retain(int64_t kind, int64_t slot) {
    if (kind == 2) rt_retain_str((EmbrStr*)(intptr_t)slot);
    else if (kind == 3) rt_retain_arr((EmbrArr*)(intptr_t)slot);
    else if (kind == 4) rt_retain_map((EmbrMap*)(intptr_t)slot);
    else if (kind == 5) rt_retain_any((EmbrAny*)(intptr_t)slot);
}
static inline void rt_elem_release(int64_t kind, int64_t slot) {
    if (kind == 2) rt_release_str((EmbrStr*)(intptr_t)slot);
    else if (kind == 3) rt_release_arr((EmbrArr*)(intptr_t)slot);
    else if (kind == 4) rt_release_map((EmbrMap*)(intptr_t)slot);
    else if (kind == 5) rt_release_any((EmbrAny*)(intptr_t)slot);
}

/* a plugin's constants, made when it is imported, are marked once they are complete (and what they hold with them) */
void rt_immortal_str(EmbrStr* s);
void rt_immortal_arr(EmbrArr* a);
void rt_immortal_map(EmbrMap* m);
EmbrStr* rt_str_from(const char* data, int64_t len);       /* a new str holding a copy of the bytes */
EmbrStr* rt_str_alloc(int64_t len);                        /* a new str of len bytes to be written (the 0 after them is there) */
void*    rt_xalloc(size_t n);                              /* malloc that ends the program when memory is out */
void     rt_fmt_f64(char* buf, size_t n, double d);        /* a float the way print() shows it */
static inline void rt_set_map(EmbrMap** d, EmbrMap* v) { rt_retain_map(v); rt_release_map(*d); *d = v; }
static inline void rt_own_map(EmbrMap** d, EmbrMap* v) { EmbrMap* o = *d; *d = v; rt_release_map(o); }
static inline int64_t rt_map_len(const EmbrMap* m) { return m ? m->len : 0; }

/* what `catch e` binds: e["message"], e["kind"] and e["line"] (a float, as in the VM) */
/* the frames the error passed on its way up to the `try` that caught it (the function names are literals that live for the whole run) */
typedef struct { int64_t rc; EmbrStr* msg; EmbrStr* kind; double line; const char* file; int ntrace; EmbrFrame trace[]; } EmbrErr;
void rt_free_err(EmbrErr* e);
static inline EmbrErr* rt_retain_err(EmbrErr* e) { if (e) ++e->rc; return e; }
static inline void rt_release_err(EmbrErr* e) { if (e && --e->rc == 0) rt_free_err(e); }
static inline void rt_set_err(EmbrErr** d, EmbrErr* v) { rt_retain_err(v); rt_release_err(*d); *d = v; }
static inline void rt_own_err(EmbrErr** d, EmbrErr* v) { EmbrErr* o = *d; *d = v; rt_release_err(o); }
EmbrErr* rt_err_take(RtCtx* cx);                               /* the error that was raised, as an object; the context is clear again */
void     rt_raise_str(RtCtx* cx, int line, const EmbrStr* msg);   /* error("text") */

/* ---- closures. a variable that a closure uses lives in a cell, shared by the function and every closure that captured it */

struct EmbrClosure;
/* kind: 0 int, 1 float, 2 str, 3 arr, 4 map, 5 err, 6 closure, 7 any. the cell lets go of its value when its count reaches 0 */
typedef struct EmbrCell {
    int64_t rc;
    int64_t kind;
    union { int64_t i; double f; EmbrStr* s; EmbrArr* a; EmbrMap* m; EmbrErr* e; struct EmbrClosure* c; EmbrAny* y; } v;
} EmbrCell;

/* `fn` is the C function (it takes the context, the closure itself, then the arguments). `name` is shown in a stack trace */
typedef struct EmbrClosure {
    int64_t     rc;
    void*       fn;
    const char* name;
    int64_t     n;
    EmbrCell*   cells[];
} EmbrClosure;

EmbrCell* rt_cell_new(int64_t kind);
void rt_free_cell(EmbrCell* c);
static inline EmbrCell* rt_retain_cell(EmbrCell* c) { RT_STAT(retain_cell); ++c->rc; return c; }
/* a closure that calls itself through its own variable holds the cell that holds it. when the function drops the variable and nothing else
   holds that closure, the cell and the closure are the only two things left, so the closure is let go first */
void rt_break_cell_cycle(EmbrCell* c);
static inline void rt_release_cell(EmbrCell* c) {
    if (!c) return;
    if (c->rc == 2 && c->kind == 6) rt_break_cell_cycle(c);
    if (--c->rc == 0) rt_free_cell(c);
}
static inline void rt_own_cell(EmbrCell** d, EmbrCell* v) { EmbrCell* o = *d; *d = v; rt_release_cell(o); }

EmbrClosure* rt_clo_new(void* fn, const char* name, int64_t n);
void rt_free_clo(EmbrClosure* c);
static inline EmbrClosure* rt_retain_clo(EmbrClosure* c) { if (c) { RT_STAT(retain_clo); ++c->rc; } return c; }
static inline void rt_release_clo(EmbrClosure* c) { if (c) RT_STAT(release_clo); if (c && --c->rc == 0) rt_free_clo(c); }
static inline void rt_set_clo(EmbrClosure** d, EmbrClosure* v) { rt_retain_clo(v); rt_release_clo(*d); *d = v; }
static inline void rt_own_clo(EmbrClosure** d, EmbrClosure* v) { EmbrClosure* o = *d; *d = v; rt_release_clo(o); }

EmbrStr* rt_str_concat(const EmbrStr* a, const EmbrStr* b);
EmbrStr* rt_str_append(EmbrStr* a, const EmbrStr* b);            /* takes a */
EmbrStr* rt_str_append_bytes(EmbrStr* a, const EmbrStr* src, int64_t off, int64_t len);   /* takes a */
int64_t  rt_str_eq(const EmbrStr* a, const EmbrStr* b);
EmbrStr* rt_str_index(RtCtx* cx, const EmbrStr* s, int64_t i, int line);
EmbrStr* rt_str_of_i64(int64_t v);
EmbrStr* rt_str_of_f64(double v);
EmbrStr* rt_join(const EmbrArr* a, const EmbrStr* sep);
void     rt_print_str(const EmbrStr* s);
/* print(a, b, c): the parts are written with a space between and a newline at the end */
void     rt_put_i64(int64_t v);
void     rt_put_f64(double d);
void     rt_put_str(const EmbrStr* s);
int64_t  rt_range_step(RtCtx* cx, int64_t start, int64_t second, int line);   /* second - start; raises when they are equal */
int64_t  rt_range_count(int64_t start, int64_t step, int64_t end);          /* how many numbers [start, start+step ... end] has */
EmbrArr* rt_range(RtCtx* cx, int64_t start, int64_t second, int64_t has_second, int64_t end, int line);   /* the numbers, as an array */
/* print an array, a map or an error the way the VM does: strings in quotes, keys bare and sorted, an error as {kind, line, message, trace}. no newline */
void     rt_put_arr(const EmbrArr* a);
void     rt_put_map(const EmbrMap* m);
void     rt_put_err(const EmbrErr* e);
/* ---- any (rt_any.c): values of no fixed type. the functions that take an any borrow it; one that gives an any gives a new reference. the ones that
   can raise take the context first and the line last. a type is written for a cast as text: i f s a (any), A<elem> an array, M<elem> a map (Ai, AAi, Ma) */
EmbrAny* rt_any_of_int(int64_t i);
EmbrAny* rt_any_of_float(double d);
EmbrAny* rt_any_of_str(const EmbrStr* s);
EmbrAny* rt_any_of_arr(EmbrArr* a);
EmbrAny* rt_any_of_map(EmbrMap* m);
EmbrAny* rt_any_of_ptr(void* p, const char* tag, void (*fin)(void*));
EmbrAny* rt_any_of_slot(int64_t kind, int64_t slot);                   /* the element of an array or a map of this kind, as an any */
EmbrArr* rt_arr_promote_any(EmbrArr* a);                               /* a unique array whose elements are anys now (a is changed and returned) */
int64_t  rt_any_to_i64(RtCtx* cx, const EmbrAny* x, const EmbrStr* tmpl, int line);   /* tmpl: the message, %W the type wanted, %G the type it was */
double   rt_any_to_f64(RtCtx* cx, const EmbrAny* x, const EmbrStr* tmpl, int line);
EmbrStr* rt_any_to_str(RtCtx* cx, const EmbrAny* x, const EmbrStr* tmpl, int line);
EmbrArr* rt_any_to_arr(RtCtx* cx, const EmbrAny* x, const char* elem, const EmbrStr* tmpl, int line);   /* elem: the type of the elements, as text */
EmbrMap* rt_any_to_map(RtCtx* cx, const EmbrAny* x, const char* elem, const EmbrStr* tmpl, int line);
EmbrAny* rt_any_add(RtCtx* cx, const EmbrAny* a, const EmbrAny* b, int line);
EmbrAny* rt_any_sub(RtCtx* cx, const EmbrAny* a, const EmbrAny* b, int line);
EmbrAny* rt_any_mul(RtCtx* cx, const EmbrAny* a, const EmbrAny* b, int line);
EmbrAny* rt_any_div(RtCtx* cx, const EmbrAny* a, const EmbrAny* b, int line);
EmbrAny* rt_any_mod(RtCtx* cx, const EmbrAny* a, const EmbrAny* b, int line);
EmbrAny* rt_any_neg(RtCtx* cx, const EmbrAny* a, int line);
int64_t  rt_any_eq(const EmbrAny* a, const EmbrAny* b);
int64_t  rt_any_lt(RtCtx* cx, const EmbrAny* a, const EmbrAny* b, int line);
int64_t  rt_any_le(RtCtx* cx, const EmbrAny* a, const EmbrAny* b, int line);
int64_t  rt_any_gt(RtCtx* cx, const EmbrAny* a, const EmbrAny* b, int line);
int64_t  rt_any_ge(RtCtx* cx, const EmbrAny* a, const EmbrAny* b, int line);
int64_t  rt_any_truthy(const EmbrAny* a);
EmbrAny* rt_any_index(RtCtx* cx, const EmbrAny* c, const EmbrAny* i, int line);
EmbrAny* rt_any_at(RtCtx* cx, const EmbrAny* c, int64_t i, int line);          /* element i of an array or a str, for a loop */
int64_t  rt_any_len(RtCtx* cx, const EmbrAny* c, int line);
int64_t  rt_any_iter_len(RtCtx* cx, const EmbrAny* c, int line);               /* how many a for loop visits: an array or a str */
void     rt_any_set_index(RtCtx* cx, EmbrAny** var, const EmbrAny* i, const EmbrAny* v, int line);   /* *var is made unique first */
void     rt_any_push(RtCtx* cx, EmbrAny** var, const EmbrAny* v, int line);
EmbrArr* rt_any_keys(RtCtx* cx, const EmbrAny* c, int line);
EmbrArr* rt_any_values(RtCtx* cx, const EmbrAny* c, int line);                 /* an array of anys */
int64_t  rt_any_has(RtCtx* cx, const EmbrAny* c, const EmbrAny* k, int line);
EmbrStr* rt_any_type_name(const EmbrAny* x);
/* the address of the slot of element i (or of the key) of an any, an array of anys or a map of anys, to change the any in it in place. the container is
   made unique first, and an any holding another kind of array or map becomes one of anys. the address is good until the container is changed */
EmbrAny** rt_any_slot(RtCtx* cx, EmbrAny** var, const EmbrAny* idx, int line);
EmbrAny** rt_arr_slot_any(RtCtx* cx, EmbrArr* a, int64_t i, int line);
EmbrAny** rt_map_slot_any(RtCtx* cx, EmbrMap* m, const EmbrStr* k, int line);

void     rt_put_any(const EmbrAny* x);
EmbrArr* rt_err_trace(const EmbrErr* e);                         /* e["trace"]: an array of maps {file, fn, line} (maps of anys) */
EmbrStr* rt_any_text(const EmbrAny* x);                       /* the text form: what print shows, a str as it is */
/* a stable merge sort of a copy of `a`. cmp(cx, ctx, x, y) gives below 0 when x goes first (the slots are what the array holds). stops when it raises */
typedef int (*RtCmp)(RtCtx* cx, void* ctx, int64_t x, int64_t y);
EmbrArr* rt_sort_cmp(RtCtx* cx, const EmbrArr* a, void* ctx, RtCmp cmp, int64_t descending);
EmbrArr* rt_sort_by_i64(const EmbrArr* a, const EmbrArr* weights, int64_t descending);     /* sort_do: order by the weight of each element, stable */
EmbrArr* rt_sort_by_f64(const EmbrArr* a, const EmbrArr* weights, int64_t descending);
void     rt_find_missing(RtCtx* cx, int line);                               /* find_do found nothing and had no default */
void     rt_unpack_check(RtCtx* cx, const EmbrArr* a, int64_t n, int line);   /* raises unless a has n elements (more are fine) */
void     rt_put_sep(void);
void     rt_put_end(void);


EmbrArr* rt_arr_new(int64_t kind, int64_t cap);
EmbrArr* rt_arr_unique_slow(EmbrArr* a);                         /* the copy, when someone else holds a */
EmbrArr* rt_arr_push_i64_slow(EmbrArr* a, int64_t x);
EmbrArr* rt_arr_push_f64_slow(EmbrArr* a, double x);
void     rt_arr_oob(RtCtx* cx, int64_t i, int line);             /* raises: array index i out of bounds */
/* the fast paths are here, so the C compiler sees them: a bounds check and a load, no call */
static inline EmbrArr* rt_arr_unique(EmbrArr* a) { return (!a || a->rc == 1) ? a : rt_arr_unique_slow(a); }          /* takes a, gives one nobody else holds */
static inline EmbrArr* rt_arr_push_i64(EmbrArr* a, int64_t x) {                                                      /* takes a. a null a is an empty array */
    if (EMBR_LIKELY(a && a->rc == 1 && a->len < a->cap)) { a->data[a->len++] = x; return a; }
    return rt_arr_push_i64_slow(a, x);
}
static inline EmbrArr* rt_arr_push_f64(EmbrArr* a, double x) {
    if (EMBR_LIKELY(a && a->rc == 1 && a->len < a->cap)) { memcpy(&a->data[a->len++], &x, 8); return a; }
    return rt_arr_push_f64_slow(a, x);
}
EmbrArr* rt_arr_push_str(EmbrArr* a, const EmbrStr* x);
EmbrArr* rt_arr_push_arr(EmbrArr* a, EmbrArr* x);                /* an array of arrays: x gets one more count */
EmbrArr* rt_arr_push_map(EmbrArr* a, EmbrMap* x);
EmbrArr* rt_arr_push_any(EmbrArr* a, EmbrAny* x);                 /* an array of anys */
EmbrAny* rt_arr_get_any(RtCtx* cx, const EmbrArr* a, int64_t i, int line);   /* a new reference */
void     rt_arr_set_any(RtCtx* cx, EmbrArr* a, int64_t i, EmbrAny* x, int line);
void     rt_map_set_any(EmbrMap* m, const EmbrStr* k, EmbrAny* x);
EmbrAny* rt_map_get_any(RtCtx* cx, const EmbrMap* m, const EmbrStr* k, int line);
static inline int64_t rt_arr_get_i64(RtCtx* cx, const EmbrArr* a, int64_t i, int line) {
    RT_STAT(arr_get);
    if (EMBR_UNLIKELY(!a || (uint64_t)i >= (uint64_t)a->len)) { rt_arr_oob(cx, i, line); return 0; }
    return a->data[i];
}
static inline double rt_arr_get_f64(RtCtx* cx, const EmbrArr* a, int64_t i, int line) {
    RT_STAT(arr_get);
    if (EMBR_UNLIKELY(!a || (uint64_t)i >= (uint64_t)a->len)) { rt_arr_oob(cx, i, line); return 0.0; }
    double d; memcpy(&d, &a->data[i], 8); return d;
}
EmbrStr* rt_arr_get_str(RtCtx* cx, const EmbrArr* a, int64_t i, int line);   /* a new reference */
static inline void rt_arr_set_i64(RtCtx* cx, EmbrArr* a, int64_t i, int64_t x, int line) {                  /* a is already unique */
    RT_STAT(arr_set);
    if (EMBR_UNLIKELY(!a || (uint64_t)i >= (uint64_t)a->len)) { rt_arr_oob(cx, i, line); return; }
    a->data[i] = x;
}
static inline void rt_arr_set_f64(RtCtx* cx, EmbrArr* a, int64_t i, double x, int line) {
    RT_STAT(arr_set);
    if (EMBR_UNLIKELY(!a || (uint64_t)i >= (uint64_t)a->len)) { rt_arr_oob(cx, i, line); return; }
    memcpy(&a->data[i], &x, 8);
}
void     rt_arr_set_str(RtCtx* cx, EmbrArr* a, int64_t i, const EmbrStr* x, int line);
EmbrArr* rt_arr_get_arr(RtCtx* cx, const EmbrArr* a, int64_t i, int line);   /* a new reference */
EmbrMap* rt_arr_get_map(RtCtx* cx, const EmbrArr* a, int64_t i, int line);
void     rt_arr_set_arr(RtCtx* cx, EmbrArr* a, int64_t i, EmbrArr* x, int line);
void     rt_arr_set_map(RtCtx* cx, EmbrArr* a, int64_t i, EmbrMap* x, int line);
/* the container at element i, made one nobody else holds (a copy goes in its place when it is shared), for changing it in place. a is already
   unique. the pointer is borrowed: it stays good until a is changed. `ckind` is the kind of the elements of that container */
EmbrArr* rt_arr_child_arr(RtCtx* cx, EmbrArr* a, int64_t i, int64_t ckind, int line);
EmbrMap* rt_arr_child_map(RtCtx* cx, EmbrArr* a, int64_t i, int64_t ckind, int line);

EmbrMap* rt_map_new(int64_t kind);
EmbrMap* rt_map_unique(EmbrMap* m);                              /* takes m, gives one nobody else holds */
void     rt_map_set_i64(EmbrMap* m, const EmbrStr* k, int64_t x);        /* m is already unique */
void     rt_map_set_f64(EmbrMap* m, const EmbrStr* k, double x);
void     rt_map_set_str(EmbrMap* m, const EmbrStr* k, const EmbrStr* x);
void     rt_map_set_arr(EmbrMap* m, const EmbrStr* k, EmbrArr* x);
void     rt_map_set_map(EmbrMap* m, const EmbrStr* k, EmbrMap* x);
EmbrArr* rt_map_get_arr(RtCtx* cx, const EmbrMap* m, const EmbrStr* k, int line);   /* a new reference */
EmbrMap* rt_map_get_map(RtCtx* cx, const EmbrMap* m, const EmbrStr* k, int line);
EmbrArr* rt_map_child_arr(RtCtx* cx, EmbrMap* m, const EmbrStr* k, int64_t ckind, int line);   /* as rt_arr_child_arr; the key has to be there */
EmbrMap* rt_map_child_map(RtCtx* cx, EmbrMap* m, const EmbrStr* k, int64_t ckind, int line);
int64_t  rt_map_get_i64(RtCtx* cx, const EmbrMap* m, const EmbrStr* k, int line);
double   rt_map_get_f64(RtCtx* cx, const EmbrMap* m, const EmbrStr* k, int line);
EmbrStr* rt_map_get_str(RtCtx* cx, const EmbrMap* m, const EmbrStr* k, int line);   /* a new reference */
int64_t  rt_map_has(const EmbrMap* m, const EmbrStr* k);
int64_t  rt_map_slot_index(const EmbrMap* m, const EmbrStr* k);                 /* the index of the slot that holds k, or -1 */
int      rt_map_lookup(const EmbrMap* m, const EmbrStr* k, int64_t* slot);   /* 1 and the slot (not counted), or 0 */
void     rt_map_put_slot(EmbrMap* m, const EmbrStr* k, int64_t slot);        /* m is unique; the slot already has the count the map keeps */
EmbrArr* rt_map_keys(const EmbrMap* m);                          /* sorted, like the VM's */
EmbrArr* rt_map_values(const EmbrMap* m);                        /* in the order of the sorted keys */

/* ---- the natives of embrlib, str and embrmath (rt_nat.c). a function that can raise takes the context first and the line last, and gives
   a zero value after raising; arguments are borrowed, a heap result is a new reference. the kinds and messages are the plugins' own */

/* embrlib */
double   rt_num_str(RtCtx* cx, const EmbrStr* s, int line);
EmbrStr* rt_slice_str(const EmbrStr* s, int64_t start, int64_t end);
EmbrStr* rt_str_append_slice(EmbrStr* a, const EmbrStr* s, int64_t start, int64_t end);   /* a = a + slice(s, start, end), no temporary string. takes a */
EmbrArr* rt_slice_arr(const EmbrArr* a, int64_t start, int64_t end);
int64_t  rt_index_of_i64(const EmbrArr* a, int64_t x);
int64_t  rt_index_of_f64(const EmbrArr* a, double x);
int64_t  rt_index_of_str(const EmbrArr* a, const EmbrStr* x);
int64_t  rt_sum_i64(const EmbrArr* a);
double   rt_sum_f64(const EmbrArr* a);
int64_t  rt_min_i64(RtCtx* cx, const EmbrArr* a, int line);
int64_t  rt_max_i64(RtCtx* cx, const EmbrArr* a, int line);
double   rt_min_f64(RtCtx* cx, const EmbrArr* a, int line);
double   rt_max_f64(RtCtx* cx, const EmbrArr* a, int line);
EmbrArr* rt_sort_arr(const EmbrArr* a, int64_t descending);          /* an array of ints or floats */
EmbrStr* rt_reverse_str(const EmbrStr* s);
EmbrArr* rt_reverse_arr(const EmbrArr* a);
EmbrArr* rt_concat_arr(const EmbrArr* a, const EmbrArr* b);
EmbrArr* rt_pop_arr(RtCtx* cx, const EmbrArr* a, int line);
int64_t  rt_has_arr(const EmbrArr* a, int64_t i);
/* these change the array or map they get, which the caller made unique first */
int64_t  rt_remove_last_i64(RtCtx* cx, EmbrArr* a, int line);
double   rt_remove_last_f64(RtCtx* cx, EmbrArr* a, int line);
EmbrStr* rt_remove_last_str(RtCtx* cx, EmbrArr* a, int line);
EmbrArr* rt_remove_last_arr(RtCtx* cx, EmbrArr* a, int line);
EmbrMap* rt_remove_last_map(RtCtx* cx, EmbrArr* a, int line);
int64_t  rt_remove_at_i64(RtCtx* cx, EmbrArr* a, int64_t pos, int line);
double   rt_remove_at_f64(RtCtx* cx, EmbrArr* a, int64_t pos, int line);
EmbrStr* rt_remove_at_str(RtCtx* cx, EmbrArr* a, int64_t pos, int line);
EmbrArr* rt_remove_at_arr(RtCtx* cx, EmbrArr* a, int64_t pos, int line);
EmbrMap* rt_remove_at_map(RtCtx* cx, EmbrArr* a, int64_t pos, int line);
int64_t  rt_insert_i64(RtCtx* cx, EmbrArr* a, int64_t pos, int64_t x, int line);
int64_t  rt_insert_f64(RtCtx* cx, EmbrArr* a, int64_t pos, double x, int line);
int64_t  rt_insert_str(RtCtx* cx, EmbrArr* a, int64_t pos, const EmbrStr* x, int line);
int64_t  rt_insert_arr(RtCtx* cx, EmbrArr* a, int64_t pos, EmbrArr* x, int line);
int64_t  rt_insert_map(RtCtx* cx, EmbrArr* a, int64_t pos, EmbrMap* x, int line);
int64_t  rt_remove_key(EmbrMap* m, const EmbrStr* key);
void     rt_assert_i64(RtCtx* cx, int64_t ok, int line);
void     rt_assert_f64(RtCtx* cx, double ok, int line);
void     rt_assert_msg_i64(RtCtx* cx, int64_t ok, const EmbrStr* msg, int line);
void     rt_assert_msg_f64(RtCtx* cx, double ok, const EmbrStr* msg, int line);
EmbrArr* rt_take_arr(const EmbrArr* a, int64_t n);
EmbrArr* rt_chunk_arr(RtCtx* cx, const EmbrArr* a, int64_t n, int64_t ckind, int line);   /* ckind: the kind of the arrays made */
EmbrArr* rt_unique_i64(const EmbrArr* a);
EmbrArr* rt_unique_f64(const EmbrArr* a);
EmbrArr* rt_unique_str(const EmbrArr* a);
EmbrArr* rt_zip2_arr(const EmbrArr* a, const EmbrArr* b, int64_t ckind);
EmbrArr* rt_flatten_arr(const EmbrArr* a, int64_t kind);                              /* kind: of the elements of the result */
EmbrArr* rt_enumerate_arr(const EmbrArr* a, int64_t ckind);
EmbrArr* rt_enumerate_from(const EmbrArr* a, int64_t start, int64_t ckind);
EmbrArr* rt_range_excl(int64_t a, int64_t b);                                         /* range(a, b): a up to b-1, or down to b+1 */
EmbrMap* rt_map_merge(const EmbrMap* a, const EmbrMap* b, int64_t kind);              /* b wins */
EmbrMap* rt_map_pick(const EmbrMap* m, const EmbrArr* keys, int64_t kind);
EmbrMap* rt_map_omit(const EmbrMap* m, const EmbrArr* keys, int64_t kind);
void     rt_min_do_empty(RtCtx* cx, int line);                                          /* min_do and max_do of an empty array */
void     rt_max_do_empty(RtCtx* cx, int line);

/* the forms of the natives above that have a default argument */
EmbrStr* rt_slice_str_from(const EmbrStr* s, int64_t start);
EmbrArr* rt_slice_arr_from(const EmbrArr* a, int64_t start);
EmbrArr* rt_sort_asc(const EmbrArr* a);
EmbrStr* rt_str_pad_left(RtCtx* cx, const EmbrStr* s, int64_t width, const EmbrStr* ch, int line);
EmbrStr* rt_str_pad_right(RtCtx* cx, const EmbrStr* s, int64_t width, const EmbrStr* ch, int line);
EmbrStr* rt_str_pad_left1(RtCtx* cx, const EmbrStr* s, int64_t width, int line);
EmbrStr* rt_num_fixed(RtCtx* cx, double x, int64_t digits, int line);
EmbrStr* rt_str_pad_right1(RtCtx* cx, const EmbrStr* s, int64_t width, int line);

#endif
