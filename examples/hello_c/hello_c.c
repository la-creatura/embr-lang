/* hello_c.c
 * the worked example for a plugin written in C (include/embr_plugin.h). it is built and run by CTest.
 *
 * usage
 *   import "hello_c"
 *   print(hc_twice(21))                  # 42
 *   print(hc_join(["a", "b", "c"], "-")) # a-b-c
 *   c = hc_counter()
 *   hc_counter_add(c, 5)
 *   print(hc_counter_get(c))             # 5
 *   print(hc_pair(3))                    # {"n": 3, "square": 9}
 *   print(hc_chars("abc"))               # ["a", "b", "c"]
 *   print(hc_kinds([1, 2.5, "x", [1]]))  # ["int", "number", "string", "array"]
 *   print(hc_loads())                    # 1  (init ran once for this import)
 *   print(hc_upper("héllo"))             # HéLLO  (writes the result string in place)
 *   print(hc_show([1, "a", 2.5]))        # [1, a, 2.5]  (any value as text, the way print() shows it)
 *   print(hc_sum([1, 2, 3]))             # 6  (reads the array in one call, embrp_array_data)
 *   print(hc_squares(4))                 # [0, 1, 4, 9]  (writes the numbers in one call, embrp_array_push_ints)
 *
 * good to know
 *   - the plugin is a table (embr_plugin_info). embr reads it and binds the functions, nothing runs at load
 *   - an error is a nonzero return after embr_fail
 */
#include <embr_plugin.h>

#include <stdlib.h>
#include <string.h>

static int hc_twice(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    (void)n;
    return embrp_set_int(ctx, out, embrp_to_int(ctx, &args[0]) * 2);
}

/* hc_join(parts: arr, sep?: str) -> str */
static int hc_join(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    size_t sep_len = 1;
    const char* sep = ",";
    if (n > 1) sep = embrp_str(ctx, &args[1], &sep_len);
    size_t count = embrp_array_len(ctx, &args[0]);
    size_t cap = 1;
    for (size_t i = 0; i < count; ++i) {
        const EmbrValue* item = embrp_array_at(ctx, &args[0], i);
        if (embrp_type(ctx, item) != EMBR_TAG_STRING) return embrp_fail(ctx, "hc_join: every element must be a string");
        size_t len; embrp_str(ctx, item, &len);
        cap += len + sep_len;
    }
    char* buf = malloc(cap);
    if (!buf) return embrp_fail(ctx, "hc_join: out of memory");
    size_t at = 0;
    for (size_t i = 0; i < count; ++i) {
        size_t len;
        const char* s = embrp_str(ctx, embrp_array_at(ctx, &args[0], i), &len);
        if (i) { memcpy(buf + at, sep, sep_len); at += sep_len; }
        memcpy(buf + at, s, len); at += len;
    }
    int rc = embrp_set_str(ctx, out, buf, at);
    free(buf);
    return rc;
}

/* a handle: the C object behind it is a plain struct, freed when the last copy of the pointer value is gone */
typedef struct { int64_t value; } Counter;
static void counter_free(void* p) { free(p); }

static int hc_counter(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    (void)args; (void)n;
    Counter* c = calloc(1, sizeof *c);
    if (!c) return embrp_fail(ctx, "hc_counter: out of memory");
    return embrp_set_ptr(ctx, out, c, "hello_c.counter", counter_free);
}

static int hc_counter_add(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    (void)n;
    Counter* c = embrp_ptr_get(ctx, &args[0], "hello_c.counter");
    if (!c) return embrp_fail(ctx, "hc_counter_add: expected a counter from hc_counter()");
    c->value += embrp_to_int(ctx, &args[1]);
    return embrp_set_int(ctx, out, c->value);
}

static int hc_counter_get(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    (void)n;
    Counter* c = embrp_ptr_get(ctx, &args[0], "hello_c.counter");
    if (!c) return embrp_fail(ctx, "hc_counter_get: expected a counter from hc_counter()");
    return embrp_set_int(ctx, out, c->value);
}

/* hc_pair(n: int) -> map, builds a map with values from temp slots */
static int hc_pair(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    (void)n;
    int64_t x = embrp_to_int(ctx, &args[0]);
    EmbrValue* t = embrp_temp(ctx);
    if (embrp_set_map(ctx, out)) return 1;
    if (embrp_set_int(ctx, t, x) || embrp_map_set(ctx, out, "n", 1, t)) return 1;
    if (embrp_set_int(ctx, t, x * x) || embrp_map_set(ctx, out, "square", 6, t)) return 1;
    return 0;
}

/* hc_chars(s: str) -> arr of str, one string per byte. pushes strings without a temp slot, and says its elements are strings */
static int hc_chars(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    (void)n;
    size_t len, i;
    const char* s = embrp_str(ctx, &args[0], &len);
    if (embrp_set_array(ctx, out, len)) return 1;
    for (i = 0; i < len; ++i)
        if (embrp_array_push_str(ctx, out, s + i, 1)) return 1;
    return 0;
}

/* hc_kinds(list: arr) -> arr of str, the type name of every element. reads an array (or a vec) where it is */
static int hc_kinds(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    (void)n;
    size_t count = embrp_array_len(ctx, &args[0]), i;
    if (embrp_set_array(ctx, out, count)) return 1;
    for (i = 0; i < count; ++i) {
        const char* name = embrp_type_name(ctx, embrp_array_at(ctx, &args[0], i));
        if (embrp_array_push_str(ctx, out, name, strlen(name))) return 1;
    }
    return 0;
}

/* init runs once each time the plugin is imported into an interpreter, before the functions are bound */
static int loads = 0;
static int hc_init(EmbrCtx* ctx) { (void)ctx; ++loads; return 0; }
static int hc_loads(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    (void)args; (void)n;
    return embrp_set_int(ctx, out, loads);
}

/* hc_upper(s: str) -> str. the result string is made by the host and written in place, so the text is not built
   somewhere else and copied */
static int hc_upper(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    size_t len, i;
    const char* s = embrp_str(ctx, &args[0], &len);
    char* d = embrp_set_str_alloc(ctx, out, len);
    (void)n;
    if (!d) return 1;
    for (i = 0; i < len; ++i) d[i] = (s[i] >= 'a' && s[i] <= 'z') ? (char)(s[i] - 32) : s[i];
    return 0;
}

/* hc_show(v) -> str, any value as text, the way print() shows it */
static int hc_show(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    size_t len;
    const char* s = embrp_to_string(ctx, &args[0], &len);
    (void)n;
    return embrp_set_str(ctx, out, s, len);
}

/* hc_sum(list: arr) -> int, the sum of the ints. a host built before embrp_array_data has a smaller table, so ask first and read one element
   at a time when the call is not there */
static int hc_sum(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    size_t count, i;
    int64_t sum = 0;
    (void)n;
    if (embrp_has_bulk(ctx)) {
        const EmbrValue* d = embrp_array_data(ctx, &args[0], &count);
        for (i = 0; i < count; ++i) {
            if (d[i].tag != EMBR_TAG_INT) return embrp_fail_kind(ctx, "value", "hc_sum: every element must be an int");
            sum += d[i].u.i;
        }
    } else {
        count = embrp_array_len(ctx, &args[0]);
        for (i = 0; i < count; ++i) {
            const EmbrValue* e = embrp_array_at(ctx, &args[0], i);
            if (embrp_type(ctx, e) != EMBR_TAG_INT) return embrp_fail(ctx, "hc_sum: every element must be an int");
            sum += embrp_to_int(ctx, e);
        }
    }
    return embrp_set_int(ctx, out, sum);
}

/* hc_squares(n: int) -> arr of int, 0, 1, 4, ... written with one call (or one each on an older host) */
static int hc_squares(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    int64_t count = embrp_to_int(ctx, &args[0]), i;
    int64_t* xs;
    int rc;
    (void)n;
    if (count < 0 || count > 1000000) return embrp_fail(ctx, "hc_squares: n has to be 0 to 1000000");
    if (embrp_set_array(ctx, out, (size_t)count)) return 1;
    if (!count) return 0;
    xs = malloc((size_t)count * sizeof *xs);
    if (!xs) return embrp_fail(ctx, "hc_squares: out of memory");
    for (i = 0; i < count; ++i) xs[i] = i * i;
    if (embrp_has_bulk(ctx)) rc = embrp_array_push_ints(ctx, out, xs, (size_t)count);
    else { rc = 0; for (i = 0; i < count && !rc; ++i) rc = embrp_array_push_int(ctx, out, xs[i]); }
    free(xs);
    return rc;
}

/* hc_fail_kind(kind: str, message: str): raises an error with a kind of its own, so e["kind"] is not "[hello_c:hc_fail_kind]" */
static int hc_fail_kind(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    (void)n; (void)out;
    return embrp_fail_kind(ctx, embrp_str(ctx, &args[0], NULL), embrp_str(ctx, &args[1], NULL));
}

/* hc_fail(msg: str): always raises, shows the error path */
static int hc_fail(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    (void)n; (void)out;
    return embrp_fail(ctx, embrp_str(ctx, &args[0], NULL));
}

static const EmbrParam p_n[]       = { { "n", EMBR_T_INT, 0 } };
static const EmbrParam p_join[]    = { { "parts", EMBR_T_ARRAY, 0 }, { "sep", EMBR_T_STRING, EMBR_PARAM_OPTIONAL } };
static const EmbrParam p_add[]     = { { "counter", EMBR_T_POINTER, 0 }, { "by", EMBR_T_INT, 0 } };
static const EmbrParam p_counter[] = { { "counter", EMBR_T_POINTER, 0 } };
static const EmbrParam p_msg[]     = { { "message", EMBR_T_STRING, 0 } };
static const EmbrParam p_str[]     = { { "s", EMBR_T_STRING, 0 } };
static const EmbrParam p_any[]     = { { "v", EMBR_T_ANY, 0 } };
static const EmbrParam p_list[]    = { { "list", EMBR_T_ARRAY | EMBR_T_POINTER, 0 } };
static const EmbrParam p_arr[]     = { { "list", EMBR_T_ARRAY, 0 } };
static const EmbrParam p_kindmsg[] = { { "kind", EMBR_T_STRING, 0 }, { "message", EMBR_T_STRING, 0 } };

static const EmbrFunc funcs[] = {
    { "hc_twice",       "hc_twice(n: int) -> int, doubles n",                         hc_twice,       NULL, p_n,       1, EMBR_T_INT,     EMBR_FUNC_PURE, 0 },
    { "hc_join",        "hc_join(parts: arr, sep?: str) -> str, joins strings",       hc_join,        NULL, p_join,    2, EMBR_T_STRING,  EMBR_FUNC_PURE, 0 },
    { "hc_counter",     "hc_counter() -> ptr, a new counter starting at 0",           hc_counter,     NULL, NULL,      0, EMBR_T_POINTER, 0, 0 },
    { "hc_counter_add", "hc_counter_add(counter, by: int) -> int, adds, gives the new value", hc_counter_add, NULL, p_add, 2, EMBR_T_INT, 0, 0 },
    { "hc_counter_get", "hc_counter_get(counter) -> int, the current value",          hc_counter_get, NULL, p_counter, 1, EMBR_T_INT,     0, 0 },
    { "hc_pair",        "hc_pair(n: int) -> map, {n, square}",                        hc_pair,        NULL, p_n,       1, EMBR_T_MAP,     EMBR_FUNC_PURE, 0 },
    { "hc_chars",       "hc_chars(s: str) -> arr of str, one string per byte",        hc_chars,       NULL, p_str,     1, EMBR_T_ARRAY,   EMBR_FUNC_PURE, EMBR_T_STRING },
    { "hc_kinds",       "hc_kinds(list: arr) -> arr of str, the type name of every element", hc_kinds, NULL, p_list, 1, EMBR_T_ARRAY, EMBR_FUNC_PURE, EMBR_T_STRING },
    { "hc_loads",       "hc_loads() -> int, how many times init ran",                 hc_loads,       NULL, NULL,      0, EMBR_T_INT,     0, 0 },
    { "hc_upper",       "hc_upper(s: str) -> str, ASCII letters uppercased, written in place",  hc_upper,       NULL, p_str,     1, EMBR_T_STRING,  EMBR_FUNC_PURE, 0 },
    { "hc_show",        "hc_show(v) -> str, any value as text",                        hc_show,        NULL, p_any,     1, EMBR_T_STRING,  EMBR_FUNC_PURE, 0 },
    { "hc_sum",         "hc_sum(list: arr) -> int, the sum of the ints, read with one call", hc_sum,   NULL, p_arr,     1, EMBR_T_INT,     EMBR_FUNC_PURE, 0 },
    { "hc_squares",     "hc_squares(n: int) -> arr of int, 0, 1, 4, ... written with one call", hc_squares, NULL, p_n,  1, EMBR_T_ARRAY,   EMBR_FUNC_PURE, EMBR_T_INT },
    { "hc_fail_kind",   "hc_fail_kind(kind: str, message: str), raises an error with a kind of its own", hc_fail_kind, NULL, p_kindmsg, 2, EMBR_T_ANY, 0, 0 },
    { "hc_fail",        "hc_fail(message: str), raises an error with that message",   hc_fail,        NULL, p_msg,     1, EMBR_T_ANY,     0, 0 },
};

static const EmbrPluginInfo info = {
    EMBR_PLUGIN_ABI_VERSION, sizeof(EmbrPluginInfo), "hello_c", "1.0.0",
    "the worked example of a plugin written in C", EMBR_CAP_NONE, funcs, sizeof funcs / sizeof funcs[0], hc_init, 0, NULL,
    NULL
};

EMBR_PLUGIN_EXPORT const EmbrPluginInfo* embr_plugin_info(void) { return &info; }
