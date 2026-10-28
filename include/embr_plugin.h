/* embr_plugin.h
 * the plain C interface for writing an embr plugin in C (or Rust, Zig, Go... anything that can make a C shared library).
 * no C++ crosses this boundary: no exceptions, no std::string, no std::function. a plugin built with this header does
 * not need the compiler or the standard library embr was built with.
 *
 * usage
 *   #include <embr_plugin.h>
 *   static int twice(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
 *       (void)n;
 *       embrp_set_int(ctx, out, embrp_to_int(ctx, &args[0]) * 2);
 *       return 0;
 *   }
 *   static const EmbrParam twice_params[] = { { "n", EMBR_T_INT, 0 } };
 *   static const EmbrFunc funcs[] = {
 *       { "twice", "twice(n: int) -> int, doubles n", twice, NULL, twice_params, 1, EMBR_T_INT, EMBR_FUNC_PURE },
 *   };
 *   static const EmbrPluginInfo info = {
 *       EMBR_PLUGIN_ABI_VERSION, sizeof(EmbrPluginInfo), "twice", "1.0.0", "doubles a number",
 *       EMBR_CAP_NONE, funcs, 1, NULL, 0, NULL, NULL
 *   };
 *   EMBR_PLUGIN_EXPORT const EmbrPluginInfo* embr_plugin_info(void) { return &info; }
 *
 * good to know
 *   - the plugin is data: embr_plugin_info() returns a table, embr reads it and binds the functions. nothing runs at load
 *   - arguments are borrowed. they, and anything you read out of them (embrp_str, embrp_array_at, embrp_map_get), stay valid
 *     until the function returns. copy to keep
 *   - a result is written into `out` with embrp_set_*. a value you build in the middle (an array element) goes in a
 *     slot from embrp_temp(), which lives until the function returns. you never retain or release a value yourself
 *   - an error is a nonzero return after embrp_fail(ctx, "message"). the script sees an ordinary error it can catch. an
 *     embrp_set_* / embrp_array_push call that fails (value nested too deeply) records the error for you: return nonzero
 *   - a function must be a leaf: it cannot call script code
 *   - to keep a value after the function returns (a callback, a cache), embrp_root() it, and embrp_unroot() when done.
 *     rooted values are seen by the gc. a value you do not root is gone when the function returns
 *   - the Value layout (EmbrValue) is the same as the one inside embr, 16 bytes, so embrp_type, embrp_to_int and
 *     embrp_to_float read it directly with no call. strings, arrays, maps, functions and pointers are counted boxes only
 *     embr may touch
 *   - a function with at most 4 parameters, none optional or variadic, is called on a fast path: embr checks the
 *     argument types from EmbrParam and calls it directly, with nothing built around the call
 */
#ifndef EMBR_PLUGIN_H
#define EMBR_PLUGIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#  define EMBR_PLUGIN_EXPORT __declspec(dllexport)
#elif defined(__GNUC__)
#  define EMBR_PLUGIN_EXPORT __attribute__((visibility("default")))
#else
#  define EMBR_PLUGIN_EXPORT
#endif

/* bumped on any change that breaks a plugin built against an older header. embr refuses a plugin with another number */
#define EMBR_PLUGIN_ABI_VERSION 1u

/* a value. same layout as embr's own Value */
typedef struct EmbrValue {
    union { double d; int64_t i; void* p; } u;
    uint32_t nest_depth;
    uint8_t  tag;            /* one of EMBR_TAG_*, read it with embrp_type() */
    uint8_t  reserved[3];
} EmbrValue;

/* what a value is */
enum {
    EMBR_TAG_FLOAT = 0, EMBR_TAG_INT = 1, EMBR_TAG_STRING = 2, EMBR_TAG_ARRAY = 3,
    EMBR_TAG_MAP = 4, EMBR_TAG_FUNCTION = 5, EMBR_TAG_POINTER = 6
};

/* sets of types, for a parameter or a result: a bit per tag. combine with | */
enum {
    EMBR_T_FLOAT = 1u << 0, EMBR_T_INT = 1u << 1, EMBR_T_STRING = 1u << 2, EMBR_T_ARRAY = 1u << 3,
    EMBR_T_MAP = 1u << 4, EMBR_T_FUNCTION = 1u << 5, EMBR_T_POINTER = 1u << 6,
    EMBR_T_NUM = EMBR_T_FLOAT | EMBR_T_INT,
    EMBR_T_ANY = 0xFFu
};

/* what a plugin may do, a bit each. the host can refuse a plugin that asks for something it does not allow */
enum {
    EMBR_CAP_NONE       = 0,
    EMBR_CAP_FILESYSTEM = 1u << 0,   /* reads or writes files, lists folders */
    EMBR_CAP_NETWORK    = 1u << 1,
    EMBR_CAP_PROCESS    = 1u << 2,   /* starts programs, exits, signals */
    EMBR_CAP_ENV        = 1u << 3,   /* reads or changes environment variables or the working folder */
    EMBR_CAP_THREADS    = 1u << 4,
    EMBR_CAP_NATIVE     = 1u << 5    /* loads or calls other native code (like ffi) */
};

typedef struct EmbrApi EmbrApi;

/* one call. the plugin never makes one, it gets a pointer. `api` is the table of functions below, `userdata` is the
   pointer the plugin gave in EmbrFunc */
typedef struct EmbrCtx {
    const EmbrApi* api;
    void*          userdata;
} EmbrCtx;

/* a native function. args[0..n) are borrowed. write the result to *out (it starts as the float 0). return 0, or nonzero
   after embrp_fail */
typedef int (*EmbrFn)(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out);

enum { EMBR_PARAM_OPTIONAL = 1u << 0, EMBR_PARAM_VARIADIC = 1u << 1 };
typedef struct EmbrParam {
    const char* name;
    uint32_t    types;   /* EMBR_T_*, embr checks the arguments before calling the function */
    uint32_t    flags;   /* EMBR_PARAM_*. an optional or variadic parameter comes after the required ones */
} EmbrParam;

/* EMBR_FUNC_PURE: the same arguments always give the same result and nothing else changes (no state, files, time).
   a `const` in a script may then call it while the script is compiled */
enum { EMBR_FUNC_PURE = 1u << 0 };
typedef struct EmbrFunc {
    const char*      name;          /* the name a script calls */
    const char*      doc;           /* one or two sentences, with the signature */
    EmbrFn           fn;            /* NULL: only documents a function the plugin's C++ embr_register binds (see below) */
    void*            userdata;      /* handed back as ctx->userdata */
    const EmbrParam* params;
    uint32_t         nparams;
    uint32_t         result_types;  /* what it always returns (EMBR_T_ANY when it varies) */
    uint32_t         flags;         /* EMBR_FUNC_* */
    uint32_t         result_elem;   /* when it returns an array: what the elements always are (EMBR_T_*), 0 when it varies */
} EmbrFunc;

typedef struct EmbrPluginInfo {
    uint32_t         abi_version;   /* EMBR_PLUGIN_ABI_VERSION */
    uint32_t         struct_size;   /* sizeof(EmbrPluginInfo), so a later version can add fields at the end */
    const char*      name;          /* the name a script imports */
    const char*      version;       /* the plugin's own version, free text like "1.2.0" */
    const char*      description;
    uint64_t         capabilities;  /* EMBR_CAP_* */
    const EmbrFunc*  funcs;
    uint32_t         nfuncs;
    /* optional (NULL for none), run once each time the plugin is imported into an interpreter, before the functions are
       bound. return 0, or nonzero after embrp_fail to refuse the import */
    int            (*init)(struct EmbrCtx* ctx);
    /* capabilities the plugin only uses when they are allowed: it switches those functions off otherwise (embrlib's file
       functions work this way). a sandbox that denies one of these still loads the plugin. EMBR_CAP_* */
    uint64_t         optional_capabilities;
    /* only for a C++ plugin (one that also exports embr_register): a text naming the compiler, standard library and
       embr C++ interface it was built with, from embr::buildId(). embr refuses a plugin whose text differs from its own,
       with a message, instead of crashing on a mismatched layout. NULL for a C plugin, whose interface is this file */
    const char*      build_id;
    /* only for a C++ plugin: the C++ ABI the plugin was built for, from embr::abiTag(), like "cxx1-itanium-libstdcxx13". embr loads
       a plugin whose tag equals its own, even when build_id differs (it prints a warning then: another compiler of the same ABI,
       say clang and gcc on libstdc++), and refuses one with another tag. NULL for a C plugin */
    const char*      abi_tag;
} EmbrPluginInfo;

/* a mask of every capability, for a host that denies them all */
enum { EMBR_CAP_ALL = EMBR_CAP_FILESYSTEM | EMBR_CAP_NETWORK | EMBR_CAP_PROCESS | EMBR_CAP_ENV | EMBR_CAP_THREADS | EMBR_CAP_NATIVE };

/* the symbol a plugin exports to describe itself. a plugin that has it and no embr_register is a C plugin: embr binds
   the functions in the table. a C++ plugin has both: embr reads this table first (version, capabilities, build id,
   one doc entry per function, with fn NULL) and only then runs embr_register, so a plugin that asks for a capability the
   host denies is refused before any of its code runs */
typedef const EmbrPluginInfo* (*EmbrPluginInfoFn)(void);

typedef struct EmbrRoot EmbrRoot;   /* a value kept alive by embrp_root */

/* one entry of a map, see embrp_map_entries */
typedef struct EmbrMapEntry {
    const char*      key;
    size_t           key_len;   /* a key may hold NUL bytes */
    const EmbrValue* value;
} EmbrMapEntry;

/* the host's functions. use the embrp_* wrappers below, they pass the ctx for you. the table only grows at the end
   within one ABI version. what needs no embr code (the tag, an int, a float) is read straight from the EmbrValue by an
   inline wrapper, not through the table */
struct EmbrApi {
    uint32_t abi_version;
    uint32_t struct_size;
    /* reading. a pointer you get back is valid until the function returns */
    const char*        (*str)(const EmbrValue* v, size_t* len);      /* "" and 0 when not a string. always ends in a 0 byte */
    size_t             (*array_len)(const EmbrValue* v);             /* an array, or a list handle such as a vec. 0 for anything else */
    const EmbrValue*   (*array_at)(const EmbrValue* v, size_t i);    /* NULL when out of range */
    size_t             (*map_len)(const EmbrValue* v);
    const EmbrValue*   (*map_get)(const EmbrValue* v, const char* key);   /* NULL when there is no such key */
    /* every entry of a map, sorted by key (byte order), so the same map always comes out the same way. *count is the
       number of entries (0 for anything that is not a map). valid until the function returns */
    const EmbrMapEntry* (*map_entries)(EmbrCtx* ctx, const EmbrValue* map, size_t* count);
    /* reads a float the way embr's own parser does (locale independent, exact). 0 on success, 1 if text[0..len) is not
       a complete number or does not fit in a double */
    int                (*parse_float)(const char* text, size_t len, double* out);
    void*              (*ptr_get)(const EmbrValue* v, const char* tag);   /* NULL unless it is a pointer with that tag */
    const char*        (*ptr_tag)(const EmbrValue* v);               /* the tag of a pointer, "" for anything else */
    /* any value as text, the way print() shows it (numbers as print() does, a string as it is, an array as [a, b]...).
       valid until the function returns. *len gets the length (may be NULL) */
    const char*        (*to_string)(EmbrCtx* ctx, const EmbrValue* v, size_t* len);
    int                (*truthy)(const EmbrValue* v);                /* 0.0, 0, "", [] and {} are false, everything else is true */
    /* writing into a slot (*out, or one from temp). each returns 0, or nonzero with the error recorded */
    int                (*set_int)(EmbrCtx* ctx, EmbrValue* slot, int64_t x);
    int                (*set_float)(EmbrCtx* ctx, EmbrValue* slot, double x);
    int                (*set_str)(EmbrCtx* ctx, EmbrValue* slot, const char* s, size_t len);
    /* makes the slot a string of `len` bytes and returns where to write them (NULL after an error). the bytes are
       uninitialised: write all len of them before the function returns. saves building the text somewhere else and
       copying it. do not touch the slot again until you are done writing */
    char*              (*set_str_alloc)(EmbrCtx* ctx, EmbrValue* slot, size_t len);
    int                (*set_copy)(EmbrCtx* ctx, EmbrValue* slot, const EmbrValue* from);
    int                (*set_array)(EmbrCtx* ctx, EmbrValue* slot, size_t reserve);   /* an empty array */
    int                (*array_push)(EmbrCtx* ctx, EmbrValue* array, const EmbrValue* item);
    int                (*array_push_str)(EmbrCtx* ctx, EmbrValue* array, const char* s, size_t len);   /* push without a temp slot */
    int                (*array_push_int)(EmbrCtx* ctx, EmbrValue* array, int64_t x);
    int                (*array_push_float)(EmbrCtx* ctx, EmbrValue* array, double x);
    int                (*set_map)(EmbrCtx* ctx, EmbrValue* slot);                     /* an empty map */
    int                (*map_set)(EmbrCtx* ctx, EmbrValue* map, const char* key, size_t key_len, const EmbrValue* item);
    int                (*map_set_str)(EmbrCtx* ctx, EmbrValue* map, const char* key, size_t key_len, const char* s, size_t len);   /* a key may hold NUL bytes */
    /* a pointer value with a tag ("myplugin.thing"). `finalize` (may be NULL) runs with p when the last copy is gone */
    int                (*set_ptr)(EmbrCtx* ctx, EmbrValue* slot, void* p, const char* tag, void (*finalize)(void* p));
    EmbrValue*         (*temp)(EmbrCtx* ctx);
    int                (*fail)(EmbrCtx* ctx, const char* message);   /* returns 1 */
    /* keeping a value after the function returns, seen by the gc */
    EmbrRoot*          (*root)(EmbrCtx* ctx, const EmbrValue* v);
    const EmbrValue*   (*root_get)(const EmbrRoot* r);
    void               (*unroot)(EmbrCtx* ctx, EmbrRoot* r);
    /* the folder of the script that is running, where a relative path resolves ("" when there is none, as in the REPL: then
       use the process's working folder). valid until the function returns */
    const char*        (*script_dir)(EmbrCtx* ctx);
    /* added later within version 1. a host built before them has a smaller table, so ask first: embrp_has_bulk() for the first three,
       and embrp_fail_kind() falls back to embrp_fail by itself */
    /* the elements of an array as one block of EmbrValue, read only, valid until the function returns. NULL (and *len 0) when it is not
       an array. a lazy range is turned into an array first. saves one call per element */
    const EmbrValue*   (*array_data)(EmbrCtx* ctx, const EmbrValue* v, size_t* len);
    /* append n numbers at once */
    int                (*array_push_ints)(EmbrCtx* ctx, EmbrValue* array, const int64_t* xs, size_t n);
    int                (*array_push_floats)(EmbrCtx* ctx, EmbrValue* array, const double* xs, size_t n);
    /* like fail, but the script's e["kind"] is `kind` instead of the label "[plugin:function]" */
    int                (*fail_kind)(EmbrCtx* ctx, const char* kind, const char* message);
    /* the words after the script name on the command line (embr script.embr -- a b gives "a", "b"). NULL past the end */
    size_t             (*script_arg_count)(EmbrCtx* ctx);
    const char*        (*script_arg)(EmbrCtx* ctx, size_t i, size_t* len);
    /* raw bytes to the host's standard output (fd 1) or error (fd 2), in order with what print() writes. 0, or nonzero with the error recorded */
    int                (*stdio_write)(EmbrCtx* ctx, int fd, const char* p, size_t n);
    int                (*stdio_flush)(EmbrCtx* ctx);
    /* up to n raw bytes from the host's standard input. it waits until n bytes came, so fewer means end of input */
    size_t             (*stdin_read)(EmbrCtx* ctx, char* buf, size_t n);
};

/* reading straight from the value */
static inline uint32_t        embrp_type(EmbrCtx* c, const EmbrValue* v) { (void)c; return v->tag; }
static inline int64_t         embrp_to_int(EmbrCtx* c, const EmbrValue* v) { (void)c; return v->tag == EMBR_TAG_INT ? v->u.i : 0; }   /* 0 when it is not an int */
static inline double          embrp_to_float(EmbrCtx* c, const EmbrValue* v) {   /* an int is converted, 0 when not a number */
    (void)c;
    return v->tag == EMBR_TAG_FLOAT ? v->u.d : v->tag == EMBR_TAG_INT ? (double)v->u.i : 0.0;
}
/* "number", "int", "string", "array", "map", "function" or "pointer": for an error message */
static inline const char*     embrp_type_name(EmbrCtx* c, const EmbrValue* v) {
    static const char* const names[] = { "number", "int", "string", "array", "map", "function", "pointer" };
    (void)c;
    return v->tag < 7 ? names[v->tag] : "?";
}

/* reading through embr */
static inline const char*     embrp_str(EmbrCtx* c, const EmbrValue* v, size_t* len) { return c->api->str(v, len); }
static inline size_t          embrp_array_len(EmbrCtx* c, const EmbrValue* v) { return c->api->array_len(v); }
static inline const EmbrValue* embrp_array_at(EmbrCtx* c, const EmbrValue* v, size_t i) { return c->api->array_at(v, i); }
static inline size_t          embrp_map_len(EmbrCtx* c, const EmbrValue* v) { return c->api->map_len(v); }
static inline const EmbrValue* embrp_map_get(EmbrCtx* c, const EmbrValue* v, const char* k) { return c->api->map_get(v, k); }
static inline const EmbrMapEntry* embrp_map_entries(EmbrCtx* c, const EmbrValue* v, size_t* n) { return c->api->map_entries(c, v, n); }
static inline int             embrp_parse_float(EmbrCtx* c, const char* s, size_t n, double* out) { return c->api->parse_float(s, n, out); }
static inline void*           embrp_ptr_get(EmbrCtx* c, const EmbrValue* v, const char* tag) { return c->api->ptr_get(v, tag); }
static inline const char*     embrp_ptr_tag(EmbrCtx* c, const EmbrValue* v) { return c->api->ptr_tag(v); }
static inline int             embrp_truthy(EmbrCtx* c, const EmbrValue* v) { return c->api->truthy(v); }
static inline const char*     embrp_to_string(EmbrCtx* c, const EmbrValue* v, size_t* len) { return c->api->to_string(c, v, len); }

/* writing */
/* a slot that holds a number or nothing owns no memory, so a number is written straight into it. any other slot
   (an old string or array) has to be released first, which only embr can do */
static inline int             embrp_set_int(EmbrCtx* c, EmbrValue* s, int64_t x) {
    if (s->tag < 2) { s->u.i = x; s->tag = EMBR_TAG_INT; s->nest_depth = 0; return 0; }
    return c->api->set_int(c, s, x);
}
static inline int             embrp_set_float(EmbrCtx* c, EmbrValue* s, double x) {
    if (s->tag < 2) { s->u.d = x; s->tag = EMBR_TAG_FLOAT; s->nest_depth = 0; return 0; }
    return c->api->set_float(c, s, x);
}
static inline int             embrp_set_str(EmbrCtx* c, EmbrValue* s, const char* p, size_t n) { return c->api->set_str(c, s, p, n); }
static inline char*           embrp_set_str_alloc(EmbrCtx* c, EmbrValue* s, size_t n) { return c->api->set_str_alloc(c, s, n); }
static inline int             embrp_set_copy(EmbrCtx* c, EmbrValue* s, const EmbrValue* f) { return c->api->set_copy(c, s, f); }
static inline int             embrp_set_array(EmbrCtx* c, EmbrValue* s, size_t reserve) { return c->api->set_array(c, s, reserve); }
static inline int             embrp_array_push(EmbrCtx* c, EmbrValue* a, const EmbrValue* i) { return c->api->array_push(c, a, i); }
static inline int             embrp_array_push_str(EmbrCtx* c, EmbrValue* a, const char* p, size_t n) { return c->api->array_push_str(c, a, p, n); }
static inline int             embrp_array_push_int(EmbrCtx* c, EmbrValue* a, int64_t x) { return c->api->array_push_int(c, a, x); }
static inline int             embrp_array_push_float(EmbrCtx* c, EmbrValue* a, double x) { return c->api->array_push_float(c, a, x); }
static inline int             embrp_set_map(EmbrCtx* c, EmbrValue* s) { return c->api->set_map(c, s); }
static inline int             embrp_map_set(EmbrCtx* c, EmbrValue* m, const char* k, size_t kn, const EmbrValue* i) { return c->api->map_set(c, m, k, kn, i); }
static inline int             embrp_map_set_str(EmbrCtx* c, EmbrValue* m, const char* k, size_t kn, const char* p, size_t n) { return c->api->map_set_str(c, m, k, kn, p, n); }
static inline int             embrp_set_ptr(EmbrCtx* c, EmbrValue* s, void* p, const char* tag, void (*fin)(void*)) { return c->api->set_ptr(c, s, p, tag, fin); }
static inline EmbrValue*      embrp_temp(EmbrCtx* c) { return c->api->temp(c); }
static inline int             embrp_fail(EmbrCtx* c, const char* msg) { return c->api->fail(c, msg); }
static inline EmbrRoot*       embrp_root(EmbrCtx* c, const EmbrValue* v) { return c->api->root(c, v); }
static inline const EmbrValue* embrp_root_get(EmbrCtx* c, const EmbrRoot* r) { return c->api->root_get(r); }
static inline void            embrp_unroot(EmbrCtx* c, EmbrRoot* r) { c->api->unroot(c, r); }
static inline const char*     embrp_script_dir(EmbrCtx* c) { return c->api->script_dir(c); }

/* the calls added after the first table. a plugin built with this header can run on an older embr: it checks the size of the table it got */
#define EMBRP_HAS_(c, member) ((c)->api->struct_size >= offsetof(EmbrApi, member) + sizeof((c)->api->member))
static inline int             embrp_has_bulk(EmbrCtx* c) { return EMBRP_HAS_(c, array_push_floats); }
static inline const EmbrValue* embrp_array_data(EmbrCtx* c, const EmbrValue* v, size_t* len) { return c->api->array_data(c, v, len); }
static inline int             embrp_array_push_ints(EmbrCtx* c, EmbrValue* a, const int64_t* xs, size_t n) { return c->api->array_push_ints(c, a, xs, n); }
static inline int             embrp_array_push_floats(EmbrCtx* c, EmbrValue* a, const double* xs, size_t n) { return c->api->array_push_floats(c, a, xs, n); }
static inline int             embrp_fail_kind(EmbrCtx* c, const char* kind, const char* msg) {
    return EMBRP_HAS_(c, fail_kind) ? c->api->fail_kind(c, kind, msg) : c->api->fail(c, msg);
}

/* the standard streams and the script's arguments, added last: ask first with embrp_has_stdio() */
static inline int             embrp_has_stdio(EmbrCtx* c) { return EMBRP_HAS_(c, stdin_read); }
static inline size_t          embrp_script_arg_count(EmbrCtx* c) { return c->api->script_arg_count(c); }
static inline const char*     embrp_script_arg(EmbrCtx* c, size_t i, size_t* len) { return c->api->script_arg(c, i, len); }
static inline int             embrp_stdio_write(EmbrCtx* c, int fd, const char* p, size_t n) { return c->api->stdio_write(c, fd, p, n); }
static inline int             embrp_stdio_flush(EmbrCtx* c) { return c->api->stdio_flush(c); }
static inline size_t          embrp_stdin_read(EmbrCtx* c, char* buf, size_t n) { return c->api->stdin_read(c, buf, n); }

#ifdef __cplusplus
}
#endif

#endif /* EMBR_PLUGIN_H */
