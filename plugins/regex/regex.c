/* regex.c
 * regular expressions for embr (ECMAScript syntax), written in C against embr_plugin.h. the engine is rx.c.
 *
 * usage
 *   import "regex"
 *   print(re_test("hello world", "wor.d"))          # 1
 *   print(re_match("2024-01-02", "(\\d+)-(\\d+)-(\\d+)"))
 *     # ["2024-01-02", "2024", "01", "02"], index 0 is the whole match
 *   print(re_find_all("a1 b22 c333", "[a-z](\\d+)"))
 *     # [["a1", "1"], ["b22", "22"], ["c333", "333"]]
 *   print(re_replace("2024-01-02", "-", "/"))        # "2024/01/02"
 *   print(re_match_named("2024-01", "(?<y>\\d+)-(?<m>\\d+)")["m"])   # "01"
 *   print(re_find_all("a\nb", "^\\w$", "m"))         # [["a"], ["b"]]: the "m" flag makes ^ and $ match at each line
 *
 * good to know
 *   - a pattern with a backreference or a lookahead runs on the backtracking matcher, which refuses subjects over 5000
 *     bytes and gives up after a step limit. every other pattern runs in linear time on any subject
 *   - a bad pattern is a normal error that a script can catch
 */
#include <embr_plugin.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rx.h"

#define BACKTRACK_MAX_SUBJECT 5000
#define MAX_RESULT ((size_t)256 * 1024 * 1024)   /* the same limit scripts have on one string */

/* ---- a growable byte buffer ---- */

typedef struct { char* p; size_t len, cap; } Buf;

static int buf_add(Buf* b, const char* s, size_t n) {
    if (n == 0) return 0;
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->len + n) cap *= 2;
        char* np = realloc(b->p, cap);
        if (!np) return 1;
        b->p = np; b->cap = cap;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    return 0;
}

static int oom(EmbrCtx* ctx) { return embrp_fail(ctx, "out of memory"); }

/* a pattern and a matcher over one subject, made for one call */
typedef struct {
    RxProg*    prog;
    RxMatcher* m;
    const char* s; size_t n;
    size_t     pos;
} Run;

static void run_free(Run* r) { rx_matcher_free(r->m); rx_free(r->prog); }

/* args: subject, pattern, then the optional flags at flags_at. the one flag is "m" */
static int run_open(EmbrCtx* ctx, const char* fn, const EmbrValue* args, size_t nargs, size_t flags_at, Run* r) {
    size_t pn, fnl;
    memset(r, 0, sizeof *r);
    r->s = embrp_str(ctx, &args[0], &r->n);
    const char* pat = embrp_str(ctx, &args[1], &pn);
    int multiline = 0;
    if (nargs > flags_at) {
        const char* fl = embrp_str(ctx, &args[flags_at], &fnl);
        for (size_t k = 0; k < fnl; ++k) {
            if (fl[k] == 'm') multiline = 1;
            else {
                char msg[96];
                snprintf(msg, sizeof msg, "unknown flag '%c' (the one flag is \"m\")", fl[k]);
                return embrp_fail(ctx, msg);
            }
        }
    }
    char err[200];
    int code = 0;
    r->prog = rx_compile(pat, pn, multiline, err, sizeof err, &code);
    if (!r->prog) {
        if (code == RX_NOMEM) return oom(ctx);
        char msg[512];
        snprintf(msg, sizeof msg, "invalid pattern \"%.*s\": %s", (int)(pn > 200 ? 200 : pn), pat, err);
        return embrp_fail(ctx, msg);
    }
    const char* why = rx_backtrack_reason(r->prog);
    if (why && r->n > BACKTRACK_MAX_SUBJECT) {
        char msg[400];
        snprintf(msg, sizeof msg, "this pattern uses %s, which needs the backtracking engine; subjects over %d bytes are refused for it (got %zu). "
                 "rewrite the pattern without it, or split the input.", why, BACKTRACK_MAX_SUBJECT, r->n);
        rx_free(r->prog); r->prog = NULL;
        (void)fn;
        return embrp_fail(ctx, msg);
    }
    r->m = rx_matcher_new(r->prog, r->s, r->n);
    if (!r->m) { rx_free(r->prog); r->prog = NULL; return oom(ctx); }
    return 0;
}

/* the next match, like JavaScript's matchAll: the next search starts where the last match ended, and after an empty match
   one byte further on. 1 for a match, 0 for no more, -1 after an error was recorded */
static int next_match(EmbrCtx* ctx, Run* r, int* caps) {
    if (r->pos > r->n) return 0;
    int k = rx_search(r->m, r->pos, caps);
    if (k < 0) return embrp_fail(ctx, "the pattern took too many steps (or ran out of memory) on this subject"), -1;
    if (k) r->pos = (size_t)caps[1] + (caps[1] == caps[0] ? 1 : 0);
    return k;
}

/* the groups of one match as strings in an array slot, "" for a group that did not take part */
static int caps_to_array(EmbrCtx* ctx, EmbrValue* slot, const Run* r, const int* caps) {
    int ng = rx_ngroups(r->prog) + 1;
    if (embrp_set_array(ctx, slot, (size_t)ng)) return 1;
    for (int g = 0; g < ng; ++g) {
        int a = caps[2 * g], b = caps[2 * g + 1];
        if (embrp_array_push_str(ctx, slot, a < 0 ? "" : r->s + a, a < 0 ? 0 : (size_t)(b - a))) return 1;
    }
    return 0;
}

static int *caps_alloc(const Run* r) { return malloc(sizeof(int) * 2 * (size_t)(rx_ngroups(r->prog) + 1)); }

static int re_test(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    Run r;
    if (run_open(ctx, "re_test", args, n, 2, &r)) return 1;
    int* caps = caps_alloc(&r);
    if (!caps) { run_free(&r); return oom(ctx); }
    int k = next_match(ctx, &r, caps);
    free(caps); run_free(&r);
    if (k < 0) return 1;
    return embrp_set_int(ctx, out, k);
}

static int re_match(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    Run r;
    if (run_open(ctx, "re_match", args, n, 2, &r)) return 1;
    int* caps = caps_alloc(&r);
    if (!caps) { run_free(&r); return oom(ctx); }
    int k = next_match(ctx, &r, caps);
    int rc = k < 0 ? 1 : k ? caps_to_array(ctx, out, &r, caps) : embrp_set_array(ctx, out, 0);
    free(caps); run_free(&r);
    return rc;
}

static int re_match_named(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    Run r;
    if (run_open(ctx, "re_match_named", args, n, 2, &r)) return 1;
    int* caps = caps_alloc(&r);
    if (!caps) { run_free(&r); return oom(ctx); }
    int k = next_match(ctx, &r, caps);
    int rc = k < 0 ? 1 : embrp_set_map(ctx, out);
    for (int g = 1; !rc && k > 0 && g <= rx_ngroups(r.prog); ++g) {
        const char* name = rx_group_name(r.prog, g);
        if (!*name) continue;
        int a = caps[2 * g], b = caps[2 * g + 1];
        rc = embrp_map_set_str(ctx, out, name, strlen(name), a < 0 ? "" : r.s + a, a < 0 ? 0 : (size_t)(b - a));
    }
    free(caps); run_free(&r);
    return rc;
}

static int re_find_all(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    Run r;
    if (run_open(ctx, "re_find_all", args, n, 2, &r)) return 1;
    int* caps = caps_alloc(&r);
    if (!caps) { run_free(&r); return oom(ctx); }
    int rc = embrp_set_array(ctx, out, 0);
    EmbrValue* t = rc ? NULL : embrp_temp(ctx);
    for (int k; !rc && (k = next_match(ctx, &r, caps)) != 0;) {
        if (k < 0) { rc = 1; break; }
        rc = caps_to_array(ctx, t, &r, caps) || embrp_array_push(ctx, out, t);
    }
    free(caps); run_free(&r);
    return rc;
}

/* appends repl to out, expanding $$ $& $` $' $n $nn $<name>. a reference to a group that does not exist is kept as text,
   as in JavaScript. $< is text too when the pattern has no named groups */
static int add_replacement(Buf* out, const char* repl, size_t rn, const Run* r, const int* caps, int has_names) {
    int ngroups = rx_ngroups(r->prog);
    for (size_t i = 0; i < rn; ++i) {
        char ch = repl[i];
        if (ch != '$' || i + 1 >= rn) { if (buf_add(out, &ch, 1)) return 1; continue; }
        char c = repl[i + 1];
        if (c == '$') { if (buf_add(out, "$", 1)) return 1; ++i; }
        else if (c == '&') { if (buf_add(out, r->s + caps[0], (size_t)(caps[1] - caps[0]))) return 1; ++i; }
        else if (c == '`') { if (buf_add(out, r->s, (size_t)caps[0])) return 1; ++i; }
        else if (c == '\'') { if (buf_add(out, r->s + caps[1], r->n - (size_t)caps[1])) return 1; ++i; }
        else if (c == '<' && has_names && memchr(repl + i + 2, '>', rn - i - 2)) {
            const char* close = memchr(repl + i + 2, '>', rn - i - 2);
            size_t nl = (size_t)(close - (repl + i + 2));
            for (int g = 1; g <= ngroups; ++g) {   /* an unknown name gives "" */
                const char* gn = rx_group_name(r->prog, g);
                if (strlen(gn) == nl && memcmp(gn, repl + i + 2, nl) == 0 && caps[2 * g] >= 0)
                    if (buf_add(out, r->s + caps[2 * g], (size_t)(caps[2 * g + 1] - caps[2 * g]))) return 1;
            }
            i = (size_t)(close - repl);
        }
        else if (c >= '0' && c <= '9') {
            int g = c - '0';
            size_t used = 1;
            if (i + 2 < rn && repl[i + 2] >= '0' && repl[i + 2] <= '9') {
                int g2 = g * 10 + (repl[i + 2] - '0');
                if (g2 >= 1 && g2 <= ngroups) { g = g2; used = 2; }
            }
            if (g >= 1 && g <= ngroups) {
                if (caps[2 * g] >= 0 && buf_add(out, r->s + caps[2 * g], (size_t)(caps[2 * g + 1] - caps[2 * g]))) return 1;
                i += used;
            } else if (buf_add(out, &ch, 1)) return 1;   /* no such group: keep the "$n" text */
        } else if (buf_add(out, &ch, 1)) return 1;
    }
    return 0;
}

static int re_replace(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    Run r;
    if (run_open(ctx, "re_replace", args, n, 3, &r)) return 1;
    size_t rn;
    const char* repl = embrp_str(ctx, &args[2], &rn);
    int plain = memchr(repl, '$', rn) == NULL;   /* nothing to expand: the whole replacement goes in at once */
    int has_names = 0;
    for (int g = 1; g <= rx_ngroups(r.prog); ++g) if (*rx_group_name(r.prog, g)) has_names = 1;
    int* caps = caps_alloc(&r);
    Buf o = {0, 0, 0};
    int rc = 0;
    if (!caps) rc = oom(ctx);
    size_t last = 0;
    for (int k; !rc && (k = next_match(ctx, &r, caps)) != 0;) {
        if (k < 0) { rc = 1; break; }
        if (buf_add(&o, r.s + last, (size_t)caps[0] - last)) { rc = oom(ctx); break; }
        if (plain ? buf_add(&o, repl, rn) : add_replacement(&o, repl, rn, &r, caps, has_names)) { rc = oom(ctx); break; }
        if (o.len > MAX_RESULT) { rc = embrp_fail(ctx, "result would exceed the limit of 268435456 bytes"); break; }
        last = (size_t)caps[1];
    }
    if (!rc && buf_add(&o, r.s + last, r.n - last)) rc = oom(ctx);
    if (!rc) rc = embrp_set_str(ctx, out, o.p ? o.p : "", o.len);
    free(o.p); free(caps); run_free(&r);
    return rc;
}

static const EmbrParam p3[] = { { "string", EMBR_T_STRING, 0 }, { "pattern", EMBR_T_STRING, 0 }, { "flags", EMBR_T_STRING, EMBR_PARAM_OPTIONAL } };
static const EmbrParam p4[] = { { "string", EMBR_T_STRING, 0 }, { "pattern", EMBR_T_STRING, 0 }, { "replacement", EMBR_T_STRING, 0 },
                                { "flags", EMBR_T_STRING, EMBR_PARAM_OPTIONAL } };

static const EmbrFunc funcs[] = {
    { "re_find_all", "re_find_all(string: str, pattern: str, flags?: str) -> arr. every non-overlapping match, each shaped like re_match's result",
      re_find_all, NULL, p3, 3, EMBR_T_ARRAY, 0, 0 },
    { "re_match", "re_match(string: str, pattern: str, flags?: str) -> arr. [whole_match, group1, ...]; unmatched optional groups are empty strings; [] if no match",
      re_match, NULL, p3, 3, EMBR_T_ARRAY, 0, 0 },
    { "re_match_named", "re_match_named(string: str, pattern: str, flags?: str) -> map. the first match's named groups, {name: text}; a group that did not take part is \"\"; {} if no match",
      re_match_named, NULL, p3, 3, EMBR_T_MAP, 0, 0 },
    { "re_replace", "re_replace(string: str, pattern: str, replacement: str, flags?: str) -> str. replaces every match; the replacement may use $1, $2, ..., $<name>, $& (the match), $` and $' (the text before and after it) and $$",
      re_replace, NULL, p4, 4, EMBR_T_STRING, 0, 0 },
    { "re_test", "re_test(string: str, pattern: str, flags?: str) -> int. 1 if it matches anywhere, else 0",
      re_test, NULL, p3, 3, EMBR_T_INT, 0, 0 },
};

static const EmbrPluginInfo info = {
    EMBR_PLUGIN_ABI_VERSION, sizeof(EmbrPluginInfo), "regex", "1.0.0", "regular expressions (ECMAScript grammar)", EMBR_CAP_NONE,
    funcs, sizeof funcs / sizeof funcs[0], NULL, 0, NULL, NULL
};

EMBR_PLUGIN_EXPORT const EmbrPluginInfo* embr_plugin_info(void) { return &info; }
