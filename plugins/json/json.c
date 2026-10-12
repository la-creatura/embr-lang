/* json.c
 * JSON plugin for embr, written in C against embr_plugin.h
 *
 * usage
 *   import "json"
 *   v = json_parse("{\"a\": [1,2,3], \"b\": true}")
 *   print(v["a"][1])
 *   print(json_stringify(v))         # compact
 *   print(json_stringify(v, 2))      # pretty-printed, 2-space indent
 *   print(json_valid("{not json"))   # 0
 *
 * embr has no bool or nil type (on purpose, see plugins/embrtypes/embrtypes.cpp, which adds both as typed pointers).
 * mapping used here:
 *   JSON null   -> a "nil" typed pointer, null (falsy)
 *   JSON true   -> a "bool" typed pointer, a fixed non-null sentinel (truthy)
 *   JSON false  -> a "bool" typed pointer, null (falsy)
 *   JSON number -> a float if it has a '.' or exponent, else an int64 exactly
 *   JSON string -> a string
 *   JSON array  -> an array
 *   JSON object -> a map (JSON object keys are always strings, matches embr maps)
 *
 * good to know
 *   - the "bool"/"nil" tags are the same convention embrtypes.cpp uses (null pointer = false, a fixed non-null
 *     sentinel = true, a null pointer with the "nil" tag = nil). a value from either plugin is recognized by the other
 *     because they share tag strings, not a compile-time link. true uses the same fixed sentinel address (1) as
 *     embrtypes.h, so a true from either plugin compares equal. keep the two in sync if this ever changes
 *   - a vec (plugins/vec) is written as the array it holds. keep its tag in sync with vec.cpp
 *   - the parser recurses once per nesting level of the input, which is untrusted, so the depth is capped (1000)
 *   - json_valid only checks, it builds no values
 */
#include <embr_plugin.h>

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DEPTH 1000
#define VEC_TAG "vec.handle"

/* ---- growable byte buffer ---- */

typedef struct { char* p; size_t len, cap; } Buf;

static int buf_reserve(Buf* b, size_t extra) {
    size_t cap;
    char* np;
    if (b->len + extra <= b->cap) return 0;
    cap = b->cap ? b->cap : 128;
    while (cap < b->len + extra) cap *= 2;
    np = realloc(b->p, cap);
    if (!np) return 1;
    b->p = np; b->cap = cap;
    return 0;
}
static int buf_add(Buf* b, const char* s, size_t n) {
    if (n == 0) return 0;   /* memcpy with a null buffer is undefined even for 0 bytes */
    if (buf_reserve(b, n)) return 1;
    memcpy(b->p + b->len, s, n);
    b->len += n;
    return 0;
}
static int buf_add_c(Buf* b, char c) {
    if (b->len == b->cap && buf_reserve(b, 1)) return 1;
    b->p[b->len++] = c;
    return 0;
}
static int buf_add_z(Buf* b, const char* z) { return buf_add(b, z, strlen(z)); }

#define TRY(x) do { if (x) return 1; } while (0)
#define OOM(ctx) embrp_fail((ctx), "out of memory")

/* ---- UTF-8 ---- */

/* length (1-4) of the valid UTF-8 sequence starting at s[i], or 0 if the bytes there are not valid UTF-8 (stray
   continuation byte, truncated sequence, overlong form, surrogate half, or above U+10FFFF). same rules as the
   unicode plugin's decoder: plugins share conventions by copy, so keep the two in sync */
static size_t utf8_seq_len(const unsigned char* s, size_t n, size_t i) {
    static const unsigned min_for_len[4] = {0, 0x80, 0x800, 0x10000};
    unsigned char c = s[i];
    size_t extra, k;
    unsigned cp;
    if (c < 0x80) return 1;
    else if ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07; }
    else return 0;
    if (i + extra >= n) return 0;                       /* truncated sequence */
    for (k = 1; k <= extra; ++k) {
        unsigned char cc = s[i + k];
        if ((cc & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (cc & 0x3F);
    }
    if (cp < min_for_len[extra] || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) return 0;
    return extra + 1;
}

static int append_utf8(Buf* out, unsigned cp) {
    char b[4];
    size_t n;
    if (cp <= 0x7F) { b[0] = (char)cp; n = 1; }
    else if (cp <= 0x7FF) { b[0] = (char)(0xC0 | (cp >> 6)); b[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp <= 0xFFFF) {
        b[0] = (char)(0xE0 | (cp >> 12)); b[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); b[2] = (char)(0x80 | (cp & 0x3F)); n = 3;
    } else {
        b[0] = (char)(0xF0 | (cp >> 18)); b[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        b[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); b[3] = (char)(0x80 | (cp & 0x3F)); n = 4;
    }
    return buf_add(out, b, n);
}

static int is_digit(char c) { return c >= '0' && c <= '9'; }

/* ---- number text, the same as embr prints a number ---- */

/* a whole number up to 1e15 has no point, any other has up to six decimals without trailing zeros */
static size_t number_text(const EmbrValue* v, char* buf, size_t cap) {
    if (v->tag == EMBR_TAG_INT) return (size_t)snprintf(buf, cap, "%lld", (long long)v->u.i);
    double d = v->u.d;
    if (d >= -1e15 && d <= 1e15 && d == (double)(long long)d) return (size_t)snprintf(buf, cap, "%lld", (long long)d);
    size_t n = (size_t)snprintf(buf, cap, "%f", d);
    if (n >= cap) n = cap - 1;
    if (memchr(buf, '.', n)) {
        while (n > 0 && buf[n - 1] == '0') --n;
        if (n > 0 && buf[n - 1] == '.') --n;
    }
    return n;
}

/* ---- parsing ---- */

typedef struct {
    EmbrCtx*       ctx;
    const char*    s;
    size_t         n, pos;
    int            depth;
    int            strict;    /* json_parse(text, {"strict_utf8": 1}): see parse_string */
    int            build;     /* 0 for json_valid: check only */
    int            quiet;     /* json_valid: a syntax error is not an error, it is the answer. pfail sets `bad` and records nothing */
    int            bad;
    Buf            str;       /* the string being decoded */
    Buf            keys;      /* object keys being waited on, a stack: a key stays here while its value is parsed */
    EmbrValue**    tmp;       /* a slot per nesting level for the container being built, made when first needed */
    size_t         tmp_cap;
} Parser;

static void parser_init(Parser* P, EmbrCtx* ctx, const char* s, size_t n) {
    memset(P, 0, sizeof *P);
    P->ctx = ctx; P->s = s; P->n = n;
}

/* where a parsed value goes: into a slot, onto the end of an array, or under a key of a map (the key is kept in
   P->keys at key_at, an offset because the buffer can move while the value is parsed) */
enum { SK_SLOT, SK_ARRAY, SK_MAP, SK_NONE };
typedef struct { int kind; EmbrValue* target; size_t key_at, klen; } Sink;

static void parser_free(Parser* P) { free(P->str.p); free(P->keys.p); free(P->tmp); }

/* 1-based line:column (column counted in bytes) plus the raw offset */
static int pfail(Parser* P, const char* msg) {
    size_t end, line = 1, line_start = 0, i;
    char buf[200];
    if (P->quiet) { P->bad = 1; return 1; }
    end = P->pos < P->n ? P->pos : P->n;
    for (i = 0; i < end; ++i)
        if (P->s[i] == '\n') { ++line; line_start = i + 1; }
    snprintf(buf, sizeof buf, "%.120s at line %zu, column %zu (position %zu)", msg, line, end - line_start + 1, P->pos);
    return embrp_fail(P->ctx, buf);
}

static int at_end(const Parser* P) { return P->pos >= P->n; }
static char peek(const Parser* P) { return at_end(P) ? '\0' : P->s[P->pos]; }

static void skip_ws(Parser* P) {
    while (!at_end(P)) {
        char c = P->s[P->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++P->pos;
        else break;
    }
}

static int consume_lit(Parser* P, const char* lit) {
    size_t n = strlen(lit);
    if (P->n - P->pos >= n && memcmp(P->s + P->pos, lit, n) == 0) { P->pos += n; return 1; }
    return 0;
}

static int parse_hex4(Parser* P, unsigned* out) {
    unsigned v = 0;
    int i;
    if (P->pos + 4 > P->n) return pfail(P, "truncated \\u escape");
    for (i = 0; i < 4; ++i) {
        char c = P->s[P->pos++];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return pfail(P, "invalid hex digit in \\u escape");
    }
    *out = v;
    return 0;
}

/* decodes the string at P->pos (a quote) into P->str */
static int parse_string(Parser* P) {
    P->str.len = 0;
    ++P->pos;   /* opening quote */
    for (;;) {
        char c;
        if (at_end(P)) return pfail(P, "unterminated string");
        c = P->s[P->pos++];
        if (c == '"') break;
        if (c == '\\') {
            char e;
            if (at_end(P)) return pfail(P, "unterminated escape");
            e = P->s[P->pos++];
            switch (e) {
                case '"':  if (buf_add_c(&P->str, '"')) return OOM(P->ctx); break;
                case '\\': if (buf_add_c(&P->str, '\\')) return OOM(P->ctx); break;
                case '/':  if (buf_add_c(&P->str, '/')) return OOM(P->ctx); break;
                case 'b':  if (buf_add_c(&P->str, '\b')) return OOM(P->ctx); break;
                case 'f':  if (buf_add_c(&P->str, '\f')) return OOM(P->ctx); break;
                case 'n':  if (buf_add_c(&P->str, '\n')) return OOM(P->ctx); break;
                case 'r':  if (buf_add_c(&P->str, '\r')) return OOM(P->ctx); break;
                case 't':  if (buf_add_c(&P->str, '\t')) return OOM(P->ctx); break;
                case 'u': {
                    unsigned cp, lo;
                    TRY(parse_hex4(P, &cp));
                    /* surrogate pair (\uD800-\uDBFF followed by \uDC00-\uDFFF). a lone or mismatched surrogate can't be
                       encoded as valid UTF-8, so it is rejected rather than emitted */
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (!(P->pos + 1 < P->n && P->s[P->pos] == '\\' && P->s[P->pos + 1] == 'u'))
                            return pfail(P, "unpaired high surrogate in \\u escape");
                        P->pos += 2;
                        TRY(parse_hex4(P, &lo));
                        if (lo < 0xDC00 || lo > 0xDFFF) return pfail(P, "high surrogate not followed by a low surrogate");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return pfail(P, "unpaired low surrogate in \\u escape");
                    }
                    if (append_utf8(&P->str, cp)) return OOM(P->ctx);
                    break;
                }
                default: {
                    char msg[32];
                    snprintf(msg, sizeof msg, "invalid escape '\\%c'", e);
                    return pfail(P, msg);
                }
            }
        } else if (P->strict) {
            /* RFC 8259: JSON text is UTF-8 and a raw control character must be escaped. the default parser is lenient
               (raw bytes pass through); this opt-in mode rejects both */
            unsigned char uc = (unsigned char)c;
            if (uc < 0x20) { --P->pos; return pfail(P, "unescaped control character in string"); }
            if (uc >= 0x80) {
                size_t n = utf8_seq_len((const unsigned char*)P->s, P->n, P->pos - 1);
                if (n == 0) { --P->pos; return pfail(P, "invalid UTF-8 in string"); }
                if (buf_add(&P->str, P->s + P->pos - 1, n)) return OOM(P->ctx);
                P->pos += n - 1;
            } else if (buf_add_c(&P->str, c)) return OOM(P->ctx);
        } else if (buf_add_c(&P->str, c)) return OOM(P->ctx);
    }
    return 0;
}

/* puts a finished value (a slot that is not the sink's own) into the sink */
static int commit(Parser* P, const Sink* sk, const EmbrValue* v) {
    switch (sk->kind) {
        case SK_SLOT:  return embrp_set_copy(P->ctx, sk->target, v);
        case SK_ARRAY: return embrp_array_push(P->ctx, sk->target, v);
        case SK_MAP:   return embrp_map_set(P->ctx, sk->target, (P->keys.p ? P->keys.p : "") + sk->key_at, sk->klen, v);
        default:       return 0;
    }
}

/* a number goes through a slot on the stack: it owns nothing, so there is nothing to give back */
static int emit_number(Parser* P, const Sink* sk, int is_int, int64_t i, double d) {
    EmbrValue v;
    if (!P->build) return 0;
    if (sk->kind == SK_SLOT) return is_int ? embrp_set_int(P->ctx, sk->target, i) : embrp_set_float(P->ctx, sk->target, d);
    memset(&v, 0, sizeof v);
    TRY(is_int ? embrp_set_int(P->ctx, &v, i) : embrp_set_float(P->ctx, &v, d));
    return commit(P, sk, &v);
}

static int emit_string(Parser* P, const Sink* sk) {
    const char* p = P->str.p ? P->str.p : "";
    if (!P->build) return 0;
    switch (sk->kind) {
        case SK_SLOT:  return embrp_set_str(P->ctx, sk->target, p, P->str.len);
        case SK_ARRAY: return embrp_array_push_str(P->ctx, sk->target, p, P->str.len);
        case SK_MAP:   return embrp_map_set_str(P->ctx, sk->target, (P->keys.p ? P->keys.p : "") + sk->key_at, sk->klen, p, P->str.len);
        default:       return 0;
    }
}

static int parse_number(Parser* P, const Sink* sk) {
    size_t start = P->pos, tlen;
    int is_float = 0, neg = 0;
    const char* tok;
    if (peek(P) == '-') { neg = 1; ++P->pos; }
    if (at_end(P) || !is_digit(peek(P))) return pfail(P, "invalid number");
    if (peek(P) == '0' && P->pos + 1 < P->n && is_digit(P->s[P->pos + 1]))
        return pfail(P, "invalid number (no leading zeros in JSON)");
    while (!at_end(P) && is_digit(peek(P))) ++P->pos;
    if (!at_end(P) && peek(P) == '.') {
        is_float = 1;
        ++P->pos;
        if (at_end(P) || !is_digit(peek(P))) return pfail(P, "invalid number (digits must follow '.')");
        while (!at_end(P) && is_digit(peek(P))) ++P->pos;
    }
    if (!at_end(P) && (peek(P) == 'e' || peek(P) == 'E')) {
        is_float = 1;
        ++P->pos;
        if (!at_end(P) && (peek(P) == '+' || peek(P) == '-')) ++P->pos;
        if (at_end(P) || !is_digit(peek(P))) return pfail(P, "invalid number exponent");
        while (!at_end(P) && is_digit(peek(P))) ++P->pos;
    }
    tok = P->s + start;
    tlen = P->pos - start;

    /* no '.' or exponent -> an exact int64, so large integer ids round-trip precisely instead of losing precision
       through a double. falls through to a float on overflow, e.g. a literal over 64 bits */
    if (!is_float) {
        uint64_t acc = 0, limit = neg ? 9223372036854775808ULL : 9223372036854775807ULL;
        size_t i;
        int fits = 1;
        for (i = neg ? 1 : 0; i < tlen; ++i) {
            unsigned d = (unsigned)(tok[i] - '0');
            if (acc > (limit - d) / 10) { fits = 0; break; }
            acc = acc * 10 + d;
        }
        if (fits) return emit_number(P, sk, 1, neg ? (int64_t)(0 - acc) : (int64_t)acc, 0.0);
    }
    {
        double d;
        if (embrp_parse_float(P->ctx, tok, tlen, &d)) {
            char msg[120];
            size_t k = tlen < 80 ? tlen : 80;   /* the token is not NUL ended, and a long one is cut */
            memcpy(msg, "invalid number literal: ", 24);
            memcpy(msg + 24, tok, k);
            msg[24 + k] = 0;
            return pfail(P, msg);
        }
        return emit_number(P, sk, 0, 0, d);
    }
}

static int parse_value(Parser* P, const Sink* sk);

/* the slot a container is built in when the sink is not a slot itself: one per nesting level */
static EmbrValue* tmp_slot(Parser* P) {
    if ((size_t)P->depth >= P->tmp_cap) {
        size_t cap = P->tmp_cap ? P->tmp_cap * 2 : 8, i;
        EmbrValue** np;
        while (cap <= (size_t)P->depth) cap *= 2;
        np = realloc(P->tmp, cap * sizeof *np);
        if (!np) return NULL;
        for (i = P->tmp_cap; i < cap; ++i) np[i] = NULL;
        P->tmp = np; P->tmp_cap = cap;
    }
    if (!P->tmp[P->depth]) P->tmp[P->depth] = embrp_temp(P->ctx);
    return P->tmp[P->depth];
}

/* where a container or a literal is made: the sink's own slot, or this level's temp slot (then copied into the sink) */
static EmbrValue* dest_for(Parser* P, const Sink* sk) {
    if (!P->build) return NULL;
    if (sk->kind == SK_SLOT) return sk->target;
    return tmp_slot(P);
}

static int finish(Parser* P, const Sink* sk, EmbrValue* dst) {
    if (!P->build || sk->kind == SK_SLOT) return 0;
    return commit(P, sk, dst);
}

static int parse_array(Parser* P, const Sink* sk) {
    EmbrValue* dst = dest_for(P, sk);
    Sink child;
    if (P->build && !dst) return OOM(P->ctx);
    ++P->pos;   /* [ */
    if (P->build) TRY(embrp_set_array(P->ctx, dst, 0));
    child.kind = P->build ? SK_ARRAY : SK_NONE; child.target = dst; child.key_at = child.klen = 0;
    skip_ws(P);
    if (peek(P) == ']') { ++P->pos; return finish(P, sk, dst); }
    for (;;) {
        TRY(parse_value(P, &child));
        skip_ws(P);
        if (peek(P) == ',') { ++P->pos; continue; }
        if (peek(P) == ']') { ++P->pos; break; }
        return pfail(P, "expected ',' or ']' in array");
    }
    return finish(P, sk, dst);
}

static int parse_object(Parser* P, const Sink* sk) {
    EmbrValue* dst = dest_for(P, sk);
    Sink child;
    if (P->build && !dst) return OOM(P->ctx);
    ++P->pos;   /* { */
    if (P->build) TRY(embrp_set_map(P->ctx, dst));
    child.kind = P->build ? SK_MAP : SK_NONE; child.target = dst;
    skip_ws(P);
    if (peek(P) == '}') { ++P->pos; return finish(P, sk, dst); }
    for (;;) {
        size_t key_at = P->keys.len;
        skip_ws(P);
        if (peek(P) != '"') return pfail(P, "expected string key");
        TRY(parse_string(P));
        child.key_at = key_at;
        child.klen = P->str.len;
        if (P->build && buf_add(&P->keys, P->str.p ? P->str.p : "", P->str.len)) return OOM(P->ctx);
        skip_ws(P);
        if (peek(P) != ':') return pfail(P, "expected ':' after object key");
        ++P->pos;
        TRY(parse_value(P, &child));
        P->keys.len = key_at;
        skip_ws(P);
        if (peek(P) == ',') { ++P->pos; continue; }
        if (peek(P) == '}') { ++P->pos; break; }
        return pfail(P, "expected ',' or '}' in object");
    }
    return finish(P, sk, dst);
}

static int parse_literal(Parser* P, const Sink* sk, const char* lit, const char* what, void* ptr, const char* tag) {
    EmbrValue* dst;
    if (!consume_lit(P, lit)) return pfail(P, what);
    dst = dest_for(P, sk);
    if (!P->build) return 0;
    if (!dst) return OOM(P->ctx);
    TRY(embrp_set_ptr(P->ctx, dst, ptr, tag, NULL));
    return finish(P, sk, dst);
}

static int parse_inner(Parser* P, const Sink* sk) {
    char c;
    skip_ws(P);
    if (at_end(P)) return pfail(P, "unexpected end of input");
    c = peek(P);
    if (c == '{') return parse_object(P, sk);
    if (c == '[') return parse_array(P, sk);
    if (c == '"') {
        TRY(parse_string(P));
        return emit_string(P, sk);
    }
    if (c == 't') return parse_literal(P, sk, "true", "invalid literal (expected 'true')", (void*)1, "bool");
    if (c == 'f') return parse_literal(P, sk, "false", "invalid literal (expected 'false')", NULL, "bool");
    if (c == 'n') return parse_literal(P, sk, "null", "invalid literal (expected 'null')", NULL, "nil");
    if (c == '-' || is_digit(c)) return parse_number(P, sk);
    {
        char msg[32];
        snprintf(msg, sizeof msg, "unexpected character '%c'", c);
        return pfail(P, msg);
    }
}

static int parse_value(Parser* P, const Sink* sk) {
    int rc;
    if (++P->depth > MAX_DEPTH) return pfail(P, "input nested too deeply (max depth 1000) while parsing a nested value");
    rc = parse_inner(P, sk);
    --P->depth;
    return rc;
}

/* reads a boolean-ish option from args[idx] (an optional map), e.g. {"strict_utf8": 1} */
static int opt_flag(EmbrCtx* ctx, const EmbrValue* args, size_t argc, size_t idx, const char* key) {
    const EmbrValue* v;
    if (argc <= idx || embrp_type(ctx, &args[idx]) != EMBR_TAG_MAP) return 0;
    v = embrp_map_get(ctx, &args[idx], key);
    return v && embrp_truthy(ctx, v);
}

/* json_parse(text: str, opts?: map) -> any
   parses a full JSON document and returns the equivalent embr value. raises (with position) on malformed input or
   trailing garbage after the top-level value */
static int json_parse(EmbrCtx* ctx, const EmbrValue* args, size_t argc, EmbrValue* out) {
    Parser P;
    Sink top;
    int rc;
    size_t n;
    {
        const char* text = embrp_str(ctx, &args[0], &n);
        parser_init(&P, ctx, text, n);
    }
    P.build = 1;
    P.strict = opt_flag(ctx, args, argc, 1, "strict_utf8");
    top.kind = SK_SLOT; top.target = out; top.key_at = top.klen = 0;
    rc = parse_value(&P, &top);
    if (!rc) { skip_ws(&P); if (!at_end(&P)) rc = pfail(&P, "trailing characters after JSON value"); }
    parser_free(&P);
    return rc;
}

/* json_valid(text: str, opts?: map) -> num
   1 if text is a syntactically valid JSON document, 0 otherwise. never raises */
static int json_valid(EmbrCtx* ctx, const EmbrValue* args, size_t argc, EmbrValue* out) {
    Parser P;
    Sink none;
    int rc, bad;
    size_t n;
    {
        const char* text = embrp_str(ctx, &args[0], &n);
        parser_init(&P, ctx, text, n);
    }
    P.build = 0;
    P.quiet = 1;
    P.strict = opt_flag(ctx, args, argc, 1, "strict_utf8");
    none.kind = SK_NONE; none.target = NULL; none.key_at = none.klen = 0;
    rc = parse_value(&P, &none);
    if (!rc) { skip_ws(&P); if (!at_end(&P)) { P.bad = 1; rc = 1; } }
    bad = P.bad;
    parser_free(&P);
    if (rc && !bad) return 1;   /* not a syntax error but running out of memory, which is already recorded */
    return embrp_set_int(ctx, out, rc ? 0 : 1);
}

/* ---- serialization ---- */

typedef struct {
    EmbrCtx* ctx;
    Buf      out;
    int      indent;   /* <= 0 means compact (no inserted whitespace at all) */
    int      lossy;
} Writer;

static int escape_into(Writer* W, const char* str, size_t len) {
    const unsigned char* s = (const unsigned char*)str;
    size_t i;
    Buf* out = &W->out;
    if (buf_add_c(out, '"')) return OOM(W->ctx);
    for (i = 0; i < len; ++i) {
        unsigned char c = s[i];
        if (c >= 0x80) {
            /* a string that is not valid UTF-8 cannot be represented in JSON: emitting the raw bytes would silently
               produce a document other parsers reject. raise, or (opt-in) substitute U+FFFD */
            size_t n = utf8_seq_len(s, len, i);
            if (n == 0) {
                if (!W->lossy) {
                    char msg[200];
                    snprintf(msg, sizeof msg, "string contains invalid UTF-8 at byte %zu (JSON text must be UTF-8; "
                             "pass {\"lossy\": 1} to replace such bytes with U+FFFD)", i);
                    return embrp_fail(W->ctx, msg);
                }
                if (buf_add_z(out, "\xEF\xBF\xBD")) return OOM(W->ctx);
                continue;
            }
            if (buf_add(out, str + i, n)) return OOM(W->ctx);
            i += n - 1;
            continue;
        }
        switch (c) {
            case '"':  if (buf_add_z(out, "\\\"")) return OOM(W->ctx); break;
            case '\\': if (buf_add_z(out, "\\\\")) return OOM(W->ctx); break;
            case '\b': if (buf_add_z(out, "\\b")) return OOM(W->ctx); break;
            case '\f': if (buf_add_z(out, "\\f")) return OOM(W->ctx); break;
            case '\n': if (buf_add_z(out, "\\n")) return OOM(W->ctx); break;
            case '\r': if (buf_add_z(out, "\\r")) return OOM(W->ctx); break;
            case '\t': if (buf_add_z(out, "\\t")) return OOM(W->ctx); break;
            default:
                if (c < 0x20) {
                    char b[8];
                    snprintf(b, sizeof b, "\\u%04x", c);
                    if (buf_add_z(out, b)) return OOM(W->ctx);
                } else if (buf_add_c(out, (char)c)) return OOM(W->ctx);
        }
    }
    if (buf_add_c(out, '"')) return OOM(W->ctx);
    return 0;
}

static int newline(Writer* W, int d) {
    size_t i, n;
    if (W->indent <= 0) return 0;
    if (buf_add_c(&W->out, '\n')) return OOM(W->ctx);
    n = (size_t)W->indent * (size_t)d;
    for (i = 0; i < n; ++i) if (buf_add_c(&W->out, ' ')) return OOM(W->ctx);
    return 0;
}

static int write_value(Writer* W, const EmbrValue* v, int depth);

/* an array, or the elements of a vec, written as they are (no copy) */
static int write_list(Writer* W, const EmbrValue* v, int depth) {
    size_t n = embrp_array_len(W->ctx, v), i;
    if (n == 0) return buf_add_z(&W->out, "[]") ? OOM(W->ctx) : 0;
    if (buf_add_c(&W->out, '[')) return OOM(W->ctx);
    for (i = 0; i < n; ++i) {
        if (i && buf_add_c(&W->out, ',')) return OOM(W->ctx);
        TRY(newline(W, depth + 1));
        TRY(write_value(W, embrp_array_at(W->ctx, v, i), depth + 1));
    }
    TRY(newline(W, depth));
    return buf_add_c(&W->out, ']') ? OOM(W->ctx) : 0;
}

static int write_value(Writer* W, const EmbrValue* v, int depth) {
    EmbrCtx* ctx = W->ctx;
    if (depth > MAX_DEPTH) return embrp_fail(ctx, "value nested too deeply (max depth 1000)");
    switch (embrp_type(ctx, v)) {
        case EMBR_TAG_FLOAT: {
            char b[400];
            size_t n;
            if (v->u.d != v->u.d || v->u.d - v->u.d != 0)   /* JSON has no NaN or Infinity */
                return buf_add_z(&W->out, "null") ? OOM(ctx) : 0;
            n = number_text(v, b, sizeof b);
            return buf_add(&W->out, b, n) ? OOM(ctx) : 0;
        }
        case EMBR_TAG_INT: {
            char b[32];
            int n = snprintf(b, sizeof b, "%lld", (long long)v->u.i);
            return buf_add(&W->out, b, (size_t)n) ? OOM(ctx) : 0;
        }
        case EMBR_TAG_STRING: {
            size_t n;
            const char* s = embrp_str(ctx, v, &n);
            return escape_into(W, s, n);
        }
        case EMBR_TAG_ARRAY:
            return write_list(W, v, depth);
        case EMBR_TAG_MAP: {
            size_t n, i;
            const EmbrMapEntry* e = embrp_map_entries(ctx, v, &n);   /* sorted, so the same map writes the same way every run */
            if (n == 0) return buf_add_z(&W->out, "{}") ? OOM(ctx) : 0;
            if (buf_add_c(&W->out, '{')) return OOM(ctx);
            for (i = 0; i < n; ++i) {
                if (i && buf_add_c(&W->out, ',')) return OOM(ctx);
                TRY(newline(W, depth + 1));
                TRY(escape_into(W, e[i].key, e[i].key_len));
                if (buf_add_z(&W->out, W->indent > 0 ? ": " : ":")) return OOM(ctx);
                TRY(write_value(W, e[i].value, depth + 1));
            }
            TRY(newline(W, depth));
            return buf_add_c(&W->out, '}') ? OOM(ctx) : 0;
        }
        case EMBR_TAG_FUNCTION:
            return embrp_fail(ctx, "cannot serialize a function value to JSON");
        case EMBR_TAG_POINTER: {
            const char* tag = embrp_ptr_tag(ctx, v);
            if (strcmp(tag, "bool") == 0) return buf_add_z(&W->out, embrp_truthy(ctx, v) ? "true" : "false") ? OOM(ctx) : 0;
            if (strcmp(tag, "nil") == 0) return buf_add_z(&W->out, "null") ? OOM(ctx) : 0;
            if (strcmp(tag, VEC_TAG) == 0) return write_list(W, v, depth);   /* a vec is written as the array it holds */
            return embrp_fail(ctx, "cannot serialize a pointer value to JSON");
        }
    }
    return embrp_fail(ctx, "cannot serialize this value to JSON");
}

/* json_stringify(value: any, indent?: num, opts?: map) -> str
   json_stringify(v) is compact, json_stringify(v, 2) is pretty printed with a 2-space indent. map keys are sorted.
   functions and pointers have no JSON representation and raise */
static int json_stringify(EmbrCtx* ctx, const EmbrValue* args, size_t argc, EmbrValue* out) {
    Writer W;
    int rc;
    memset(&W, 0, sizeof W);
    W.ctx = ctx;
    if (argc >= 2) {
        int64_t n;
        if (embrp_type(ctx, &args[1]) == EMBR_TAG_INT) n = embrp_to_int(ctx, &args[1]);
        else {
            double d = embrp_to_float(ctx, &args[1]);
            if (!(d >= -9223372036854775808.0 && d < 9223372036854775808.0)) {
                char b[400], msg[460];
                size_t k = number_text(&args[1], b, sizeof b);
                snprintf(msg, sizeof msg, "indent must be a finite number in integer range, got %.*s", (int)k, b);
                return embrp_fail(ctx, msg);
            }
            n = (int64_t)d;
        }
        W.indent = n > 0 ? (int)(n < 16 ? n : 16) : 0;   /* 16 spaces per level is already absurd */
    }
    W.lossy = opt_flag(ctx, args, argc, 2, "lossy");
    rc = write_value(&W, &args[0], 0);
    if (!rc) rc = embrp_set_str(ctx, out, W.out.p ? W.out.p : "", W.out.len);
    free(W.out.p);
    return rc;
}

static const EmbrParam p_parse[]     = { { "text", EMBR_T_STRING, 0 }, { "opts", EMBR_T_MAP, EMBR_PARAM_OPTIONAL } };
static const EmbrParam p_stringify[] = { { "value", EMBR_T_ANY, 0 }, { "indent", EMBR_T_NUM, EMBR_PARAM_OPTIONAL }, { "opts", EMBR_T_MAP, EMBR_PARAM_OPTIONAL } };

static const EmbrFunc funcs[] = {
    { "json_parse", "json_parse(text: str, opts?: map) -> any. raises with a position on malformed input. opts: {\"strict_utf8\": 1} also refuses raw control characters and invalid UTF-8 in strings",
      json_parse, NULL, p_parse, 2, EMBR_T_ANY, 0, 0 },
    { "json_valid", "json_valid(text: str, opts?: map) -> num, 1 if text is valid JSON, else 0. never raises",
      json_valid, NULL, p_parse, 2, EMBR_T_INT, 0, 0 },
    { "json_stringify", "json_stringify(value, indent?: num, opts?: map) -> str. map keys are sorted. opts: {\"lossy\": 1} replaces invalid UTF-8 with U+FFFD instead of raising",
      json_stringify, NULL, p_stringify, 3, EMBR_T_STRING, 0, 0 },
};

static const EmbrPluginInfo info = {
    EMBR_PLUGIN_ABI_VERSION, sizeof(EmbrPluginInfo), "json", "1.0.0",
    "JSON parse, validate and stringify", EMBR_CAP_NONE, funcs, sizeof funcs / sizeof funcs[0], NULL, 0, NULL,
    NULL
};

EMBR_PLUGIN_EXPORT const EmbrPluginInfo* embr_plugin_info(void) { return &info; }
