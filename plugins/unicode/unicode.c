/* unicode.c
 * small UTF-8-aware companion to str, written in C against embr_plugin.h
 *
 * usage
 *   import "unicode"
 *   print(unicode_len("héllo"))        # 5 (code points), not str_len's 6 (bytes)
 *   print(unicode_ord("é"))            # 233, not str_ord's single leading byte
 *   print(unicode_chr(233))            # "é", UTF-8 encoded
 *   print(unicode_chars("héllo"))      # ["h", "é", "l", "l", "o"]
 *   print(unicode_valid("\xff\xfe"))   # 0
 *
 * good to know
 *   - str_ord, str_chr, str_isalpha and the like are byte based on purpose. this plugin is the opt-in "i mean code points"
 *     half, kept apart so a script that never touches non-ASCII text pays nothing
 *   - isalpha, isdigit and isalnum use the C library's wide character classes (iswalpha and so on) on the decoded
 *     code point, so they know whatever the current locale knows. that is not a full Unicode database
 *   - programs start in the "C" locale, so the plugin asks for the environment's LC_CTYPE when it is imported. that only
 *     widens classification, and every import asks for the same locale
 */
#include <embr_plugin.h>

#include <locale.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wctype.h>

typedef struct { uint32_t cp; size_t len; int valid; } Decoded;

/* one code point at s[pos], pos < n. on bad bytes: len 1, so a caller can skip forward */
static inline Decoded decode_one(const unsigned char* s, size_t n, size_t pos) {
    Decoded d = {0, 1, 0};
    unsigned c0 = s[pos];
    if (c0 < 0x80) { d.cp = c0; d.valid = 1; return d; }
    size_t extra; uint32_t cp;
    if      ((c0 & 0xE0) == 0xC0) { extra = 1; cp = c0 & 0x1F; }
    else if ((c0 & 0xF0) == 0xE0) { extra = 2; cp = c0 & 0x0F; }
    else if ((c0 & 0xF8) == 0xF0) { extra = 3; cp = c0 & 0x07; }
    else return d;
    if (pos + extra >= n) return d;
    for (size_t i = 1; i <= extra; ++i) {
        unsigned c = s[pos + i];
        if ((c & 0xC0) != 0x80) return d;
        cp = (cp << 6) | (c & 0x3F);
    }
    /* overlong encodings and surrogate halves are refused */
    static const uint32_t min_for_len[4] = {0, 0x80, 0x800, 0x10000};
    if (cp < min_for_len[extra] || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) return d;
    d.cp = cp; d.len = extra + 1; d.valid = 1;
    return d;
}

static int bad_byte(EmbrCtx* ctx, size_t pos) {
    char msg[64];
    snprintf(msg, sizeof msg, "invalid UTF-8 at byte %zu", pos);
    return embrp_fail(ctx, msg);
}

static int unicode_ord(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    size_t len; (void)n;
    const unsigned char* s = (const unsigned char*)embrp_str(ctx, &args[0], &len);
    if (len == 0) return embrp_fail(ctx, "empty string");
    Decoded d = decode_one(s, len, 0);
    if (!d.valid) return bad_byte(ctx, 0);
    return embrp_set_int(ctx, out, (int64_t)d.cp);
}

static int unicode_chr(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    (void)n;
    int64_t code;
    if (embrp_type(ctx, &args[0]) == EMBR_TAG_INT) code = embrp_to_int(ctx, &args[0]);
    else {
        double d = embrp_to_float(ctx, &args[0]);
        if (!(d >= -9223372036854775808.0 && d < 9223372036854775808.0)) {
            size_t k; char msg[160];
            const char* t = embrp_to_string(ctx, &args[0], &k);
            snprintf(msg, sizeof msg, "argument must be a finite number in integer range, got %.40s", t);
            return embrp_fail(ctx, msg);
        }
        code = (int64_t)d;
    }
    if (code < 0 || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF))
        return embrp_fail(ctx, "code point out of range (0-0x10FFFF, excluding surrogates)");
    unsigned char b[4];
    uint32_t cp = (uint32_t)code;
    size_t k;
    if (cp <= 0x7F) { b[0] = (unsigned char)cp; k = 1; }
    else if (cp <= 0x7FF) { b[0] = (unsigned char)(0xC0 | (cp >> 6)); b[1] = (unsigned char)(0x80 | (cp & 0x3F)); k = 2; }
    else if (cp <= 0xFFFF) {
        b[0] = (unsigned char)(0xE0 | (cp >> 12)); b[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F)); b[2] = (unsigned char)(0x80 | (cp & 0x3F)); k = 3;
    } else {
        b[0] = (unsigned char)(0xF0 | (cp >> 18)); b[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
        b[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F)); b[3] = (unsigned char)(0x80 | (cp & 0x3F)); k = 4;
    }
    return embrp_set_str(ctx, out, (const char*)b, k);
}

static int unicode_len(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    size_t len, count = 0, pos = 0; (void)n;
    const unsigned char* s = (const unsigned char*)embrp_str(ctx, &args[0], &len);
    while (pos < len) {
        if (s[pos] < 0x80) { ++count; ++pos; continue; }   /* plain ASCII needs no decoding */
        Decoded d = decode_one(s, len, pos);
        if (!d.valid) return bad_byte(ctx, pos);
        ++count;
        pos += d.len;
    }
    return embrp_set_int(ctx, out, (int64_t)count);
}

static int unicode_valid(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    size_t len, pos = 0; (void)n;
    const unsigned char* s = (const unsigned char*)embrp_str(ctx, &args[0], &len);
    int ok = 1;
    while (ok && pos < len) {
        if (s[pos] < 0x80) { ++pos; continue; }
        Decoded d = decode_one(s, len, pos);
        if (!d.valid) ok = 0;
        pos += d.len;
    }
    return embrp_set_int(ctx, out, ok);
}

static int unicode_chars(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    size_t len, pos = 0; (void)n;
    const unsigned char* s = (const unsigned char*)embrp_str(ctx, &args[0], &len);
    if (embrp_set_array(ctx, out, len)) return 1;
    while (pos < len) {
        Decoded d = decode_one(s, len, pos);
        if (!d.valid) return bad_byte(ctx, pos);
        if (embrp_array_push_str(ctx, out, (const char*)s + pos, d.len)) return 1;
        pos += d.len;
    }
    return 0;
}

/* 1 only when the string is not empty, is valid UTF-8, and every code point is in the class */
static int classify_all(EmbrCtx* ctx, const EmbrValue* arg, EmbrValue* out, int (*classify)(wint_t)) {
    size_t len, pos = 0;
    const unsigned char* s = (const unsigned char*)embrp_str(ctx, arg, &len);
    int all = len > 0;
    while (pos < len) {
        Decoded d = decode_one(s, len, pos);
        if (!d.valid) return bad_byte(ctx, pos);
        if (!classify((wint_t)d.cp)) { all = 0; break; }
        pos += d.len;
    }
    return embrp_set_int(ctx, out, all);
}

static int unicode_isalpha(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) { (void)n; return classify_all(ctx, &args[0], out, iswalpha); }
static int unicode_isdigit(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) { (void)n; return classify_all(ctx, &args[0], out, iswdigit); }
static int unicode_isalnum(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) { (void)n; return classify_all(ctx, &args[0], out, iswalnum); }

static int init(EmbrCtx* ctx) { (void)ctx; setlocale(LC_CTYPE, ""); return 0; }

static const EmbrParam p_char[]   = { { "char", EMBR_T_STRING, 0 } };
static const EmbrParam p_code[]   = { { "code", EMBR_T_NUM, 0 } };
static const EmbrParam p_string[] = { { "string", EMBR_T_STRING, 0 } };

static const EmbrFunc funcs[] = {
    { "unicode_chars", "unicode_chars(string: str) -> arr of str. one string per code point; raises on invalid UTF-8",
      unicode_chars, NULL, p_string, 1, EMBR_T_ARRAY, EMBR_FUNC_PURE, EMBR_T_STRING },
    { "unicode_chr", "unicode_chr(code: num) -> str. UTF-8 encoding of code; raises outside 0..0x10FFFF or for a surrogate",
      unicode_chr, NULL, p_code, 1, EMBR_T_STRING, EMBR_FUNC_PURE, 0 },
    { "unicode_isalnum", "unicode_isalnum(string: str) -> int. 1 only if s is non-empty and every code point matches (\"\" gives 0); raises on invalid UTF-8",
      unicode_isalnum, NULL, p_string, 1, EMBR_T_INT, EMBR_FUNC_PURE, 0 },
    { "unicode_isalpha", "unicode_isalpha(string: str) -> int. 1 only if s is non-empty and every code point matches (\"\" gives 0); raises on invalid UTF-8",
      unicode_isalpha, NULL, p_string, 1, EMBR_T_INT, EMBR_FUNC_PURE, 0 },
    { "unicode_isdigit", "unicode_isdigit(string: str) -> int. 1 only if s is non-empty and every code point matches (\"\" gives 0); raises on invalid UTF-8",
      unicode_isdigit, NULL, p_string, 1, EMBR_T_INT, EMBR_FUNC_PURE, 0 },
    { "unicode_len", "unicode_len(string: str) -> int. code points, not bytes (\"héllo\" gives 5); raises on invalid UTF-8",
      unicode_len, NULL, p_string, 1, EMBR_T_INT, EMBR_FUNC_PURE, 0 },
    { "unicode_ord", "unicode_ord(char: str) -> int. code point of the first character; raises on \"\" or invalid UTF-8",
      unicode_ord, NULL, p_char, 1, EMBR_T_INT, EMBR_FUNC_PURE, 0 },
    { "unicode_valid", "unicode_valid(string: str) -> int. 1/0, never raises",
      unicode_valid, NULL, p_string, 1, EMBR_T_INT, EMBR_FUNC_PURE, 0 },
};

static const EmbrPluginInfo info = {
    EMBR_PLUGIN_ABI_VERSION, sizeof(EmbrPluginInfo), "unicode", "1.0.0", "UTF-8 aware length, code points and character classes",
    EMBR_CAP_NONE, funcs, sizeof funcs / sizeof funcs[0], init, 0, NULL, NULL
};

EMBR_PLUGIN_EXPORT const EmbrPluginInfo* embr_plugin_info(void) { return &info; }
