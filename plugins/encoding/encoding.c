/* encoding.c
 * byte-string encodings and digests for embr: base64, hex, sha256, crc32. written in C against embr_plugin.h
 *
 * usage
 *   import "encoding"
 *   print(base64_encode("hello"))             # aGVsbG8=
 *   print(base64_decode("aGVsbG8="))          # hello
 *   print(hex_encode("hi"))                   # 6869
 *   print(sha256("abc"))                      # ba7816bf...
 *   print(crc32("123456789"))                 # 3421780262
 *
 * good to know
 *   - embr strings are plain byte strings (see the str plugin's notes), so the decoders can return bytes that are
 *     not valid UTF-8. that is by design
 *   - every decoder raises on malformed input rather than returning garbage
 *   - this was the first plugin moved from C++ to the C interface, see the wiki, writing-plugins
 */
#include <embr_plugin.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- helpers ---- */

/* sends buf (len bytes) as the result and frees it */
static int give(EmbrCtx* ctx, EmbrValue* out, char* buf, size_t len) {
    int rc = embrp_set_str(ctx, out, buf, len);
    free(buf);
    return rc;
}

static int failf(EmbrCtx* ctx, const char* what, size_t pos) {
    char msg[96];
    snprintf(msg, sizeof msg, "%s at position %zu", what, pos);
    return embrp_fail(ctx, msg);
}

/* ---- base64 (RFC 4648) ---- */

static int b64_encode(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    size_t len;
    const unsigned char* in = (const unsigned char*)embrp_str(ctx, &args[0], &len);
    int url = n >= 2 && embrp_truthy(ctx, &args[1]);
    int pad = !(n >= 3 && embrp_truthy(ctx, &args[2]));
    const char* tbl = url ? "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
                          : "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char* buf = malloc((len + 2) / 3 * 4 + 1);
    if (!buf) return embrp_fail(ctx, "base64_encode: out of memory");
    size_t at = 0, i = 0;
    for (; i + 2 < len; i += 3) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
        buf[at++] = tbl[v >> 18]; buf[at++] = tbl[(v >> 12) & 63]; buf[at++] = tbl[(v >> 6) & 63]; buf[at++] = tbl[v & 63];
    }
    size_t rem = len - i;
    if (rem == 1) {
        uint32_t v = (uint32_t)in[i] << 16;
        buf[at++] = tbl[v >> 18]; buf[at++] = tbl[(v >> 12) & 63];
        if (pad) { buf[at++] = '='; buf[at++] = '='; }
    } else if (rem == 2) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8);
        buf[at++] = tbl[v >> 18]; buf[at++] = tbl[(v >> 12) & 63]; buf[at++] = tbl[(v >> 6) & 63];
        if (pad) buf[at++] = '=';
    }
    return give(ctx, out, buf, at);
}

static int b64_value(char c, int url) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == (url ? '-' : '+')) return 62;
    if (c == (url ? '_' : '/')) return 63;
    return -1;
}

static int b64_decode(EmbrCtx* ctx, const EmbrValue* args, size_t argc, EmbrValue* out) {
    size_t len;
    const char* in = embrp_str(ctx, &args[0], &len);
    int url = argc >= 2 && embrp_truthy(ctx, &args[1]);
    size_t n = len, padding = 0;
    while (n > 0 && in[n - 1] == '=') { --n; ++padding; }
    if (padding > 2) return embrp_fail(ctx, "too much '=' padding");
    /* padding is optional only when absent entirely; if present it must complete the quantum */
    if (padding && (n + padding) % 4 != 0) return embrp_fail(ctx, "incorrect padding");
    if (n % 4 == 1) return embrp_fail(ctx, "invalid length");
    char* buf = malloc(n / 4 * 3 + 3);
    if (!buf) return embrp_fail(ctx, "base64_decode: out of memory");
    size_t at = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < n; ++i) {
        int v = b64_value(in[i], url);
        if (v < 0) { free(buf); return failf(ctx, "invalid character", i); }
        acc = (acc << 6) | (uint32_t)v; bits += 6;
        if (bits >= 8) { bits -= 8; buf[at++] = (char)((acc >> bits) & 0xFF); }
    }
    /* leftover bits must be zero (canonical encoding) */
    if (bits && (acc & ((1u << bits) - 1))) { free(buf); return embrp_fail(ctx, "non-canonical trailing bits"); }
    return give(ctx, out, buf, at);
}

/* ---- hex ---- */

static const char hex_digits[] = "0123456789abcdef";

static void to_hex(char* dst, const unsigned char* p, size_t n) {
    for (size_t i = 0; i < n; ++i) { dst[2 * i] = hex_digits[p[i] >> 4]; dst[2 * i + 1] = hex_digits[p[i] & 15]; }
}

static int hex_encode(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    (void)n;
    size_t len;
    const unsigned char* in = (const unsigned char*)embrp_str(ctx, &args[0], &len);
    char* buf = malloc(len * 2 + 1);
    if (!buf) return embrp_fail(ctx, "hex_encode: out of memory");
    to_hex(buf, in, len);
    return give(ctx, out, buf, len * 2);
}

static int nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hex_decode(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    (void)n;
    size_t len;
    const char* in = embrp_str(ctx, &args[0], &len);
    if (len % 2) return embrp_fail(ctx, "odd number of hex digits");
    char* buf = malloc(len / 2 + 1);
    if (!buf) return embrp_fail(ctx, "hex_decode: out of memory");
    for (size_t i = 0; i < len; i += 2) {
        int hi = nibble(in[i]), lo = nibble(in[i + 1]);
        if (hi < 0) { free(buf); return failf(ctx, "invalid hex digit", i); }
        if (lo < 0) { free(buf); return failf(ctx, "invalid hex digit", i + 1); }
        buf[i / 2] = (char)((hi << 4) | lo);
    }
    return give(ctx, out, buf, len / 2);
}

/* ---- sha256 (FIPS 180-4) ---- */

static const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha256_block(uint32_t h[8], const unsigned char* b) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = ((uint32_t)b[i * 4] << 24) | ((uint32_t)b[i * 4 + 1] << 16) | ((uint32_t)b[i * 4 + 2] << 8) | (uint32_t)b[i * 4 + 3];
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = hh + S1 + ch + K[i] + w[i];
        uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t mj = (a & bb) ^ (a & c) ^ (bb & c);
        uint32_t t2 = S0 + mj;
        hh = g; g = f; f = e; e = d + t1; d = c; c = bb; bb = a; a = t1 + t2;
    }
    h[0] += a; h[1] += bb; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

/* the digest of msg[0..len), 32 bytes. the message is read in place, only the last one or two blocks are copied */
static void sha256_digest(const unsigned char* msg, size_t len, unsigned char out[32]) {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    size_t full = len / 64;
    for (size_t i = 0; i < full; ++i) sha256_block(h, msg + i * 64);
    unsigned char tail[128];
    size_t rest = len - full * 64;
    memset(tail, 0, sizeof tail);
    if (rest) memcpy(tail, msg + full * 64, rest);
    tail[rest] = 0x80;
    size_t tail_len = rest < 56 ? 64 : 128;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; ++i) tail[tail_len - 1 - i] = (unsigned char)(bits >> (i * 8));
    for (size_t off = 0; off < tail_len; off += 64) sha256_block(h, tail + off);
    for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 4; ++j) out[i * 4 + j] = (unsigned char)(h[i] >> (24 - 8 * j));
}

static int sha256_hex(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    (void)n;
    size_t len;
    const unsigned char* in = (const unsigned char*)embrp_str(ctx, &args[0], &len);
    unsigned char d[32];
    char hex[64];
    sha256_digest(in, len, d);
    to_hex(hex, d, 32);
    return embrp_set_str(ctx, out, hex, 64);
}

static int sha256_raw(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    (void)n;
    size_t len;
    const unsigned char* in = (const unsigned char*)embrp_str(ctx, &args[0], &len);
    unsigned char d[32];
    sha256_digest(in, len, d);
    return embrp_set_str(ctx, out, (const char*)d, 32);
}

/* ---- crc32 (IEEE 802.3, as used by zip/gzip/png) ---- */

static const uint32_t crc_table[256] = {
    0x00000000u, 0x77073096u, 0xee0e612cu, 0x990951bau, 0x076dc419u, 0x706af48fu,
    0xe963a535u, 0x9e6495a3u, 0x0edb8832u, 0x79dcb8a4u, 0xe0d5e91eu, 0x97d2d988u,
    0x09b64c2bu, 0x7eb17cbdu, 0xe7b82d07u, 0x90bf1d91u, 0x1db71064u, 0x6ab020f2u,
    0xf3b97148u, 0x84be41deu, 0x1adad47du, 0x6ddde4ebu, 0xf4d4b551u, 0x83d385c7u,
    0x136c9856u, 0x646ba8c0u, 0xfd62f97au, 0x8a65c9ecu, 0x14015c4fu, 0x63066cd9u,
    0xfa0f3d63u, 0x8d080df5u, 0x3b6e20c8u, 0x4c69105eu, 0xd56041e4u, 0xa2677172u,
    0x3c03e4d1u, 0x4b04d447u, 0xd20d85fdu, 0xa50ab56bu, 0x35b5a8fau, 0x42b2986cu,
    0xdbbbc9d6u, 0xacbcf940u, 0x32d86ce3u, 0x45df5c75u, 0xdcd60dcfu, 0xabd13d59u,
    0x26d930acu, 0x51de003au, 0xc8d75180u, 0xbfd06116u, 0x21b4f4b5u, 0x56b3c423u,
    0xcfba9599u, 0xb8bda50fu, 0x2802b89eu, 0x5f058808u, 0xc60cd9b2u, 0xb10be924u,
    0x2f6f7c87u, 0x58684c11u, 0xc1611dabu, 0xb6662d3du, 0x76dc4190u, 0x01db7106u,
    0x98d220bcu, 0xefd5102au, 0x71b18589u, 0x06b6b51fu, 0x9fbfe4a5u, 0xe8b8d433u,
    0x7807c9a2u, 0x0f00f934u, 0x9609a88eu, 0xe10e9818u, 0x7f6a0dbbu, 0x086d3d2du,
    0x91646c97u, 0xe6635c01u, 0x6b6b51f4u, 0x1c6c6162u, 0x856530d8u, 0xf262004eu,
    0x6c0695edu, 0x1b01a57bu, 0x8208f4c1u, 0xf50fc457u, 0x65b0d9c6u, 0x12b7e950u,
    0x8bbeb8eau, 0xfcb9887cu, 0x62dd1ddfu, 0x15da2d49u, 0x8cd37cf3u, 0xfbd44c65u,
    0x4db26158u, 0x3ab551ceu, 0xa3bc0074u, 0xd4bb30e2u, 0x4adfa541u, 0x3dd895d7u,
    0xa4d1c46du, 0xd3d6f4fbu, 0x4369e96au, 0x346ed9fcu, 0xad678846u, 0xda60b8d0u,
    0x44042d73u, 0x33031de5u, 0xaa0a4c5fu, 0xdd0d7cc9u, 0x5005713cu, 0x270241aau,
    0xbe0b1010u, 0xc90c2086u, 0x5768b525u, 0x206f85b3u, 0xb966d409u, 0xce61e49fu,
    0x5edef90eu, 0x29d9c998u, 0xb0d09822u, 0xc7d7a8b4u, 0x59b33d17u, 0x2eb40d81u,
    0xb7bd5c3bu, 0xc0ba6cadu, 0xedb88320u, 0x9abfb3b6u, 0x03b6e20cu, 0x74b1d29au,
    0xead54739u, 0x9dd277afu, 0x04db2615u, 0x73dc1683u, 0xe3630b12u, 0x94643b84u,
    0x0d6d6a3eu, 0x7a6a5aa8u, 0xe40ecf0bu, 0x9309ff9du, 0x0a00ae27u, 0x7d079eb1u,
    0xf00f9344u, 0x8708a3d2u, 0x1e01f268u, 0x6906c2feu, 0xf762575du, 0x806567cbu,
    0x196c3671u, 0x6e6b06e7u, 0xfed41b76u, 0x89d32be0u, 0x10da7a5au, 0x67dd4accu,
    0xf9b9df6fu, 0x8ebeeff9u, 0x17b7be43u, 0x60b08ed5u, 0xd6d6a3e8u, 0xa1d1937eu,
    0x38d8c2c4u, 0x4fdff252u, 0xd1bb67f1u, 0xa6bc5767u, 0x3fb506ddu, 0x48b2364bu,
    0xd80d2bdau, 0xaf0a1b4cu, 0x36034af6u, 0x41047a60u, 0xdf60efc3u, 0xa867df55u,
    0x316e8eefu, 0x4669be79u, 0xcb61b38cu, 0xbc66831au, 0x256fd2a0u, 0x5268e236u,
    0xcc0c7795u, 0xbb0b4703u, 0x220216b9u, 0x5505262fu, 0xc5ba3bbeu, 0xb2bd0b28u,
    0x2bb45a92u, 0x5cb36a04u, 0xc2d7ffa7u, 0xb5d0cf31u, 0x2cd99e8bu, 0x5bdeae1du,
    0x9b64c2b0u, 0xec63f226u, 0x756aa39cu, 0x026d930au, 0x9c0906a9u, 0xeb0e363fu,
    0x72076785u, 0x05005713u, 0x95bf4a82u, 0xe2b87a14u, 0x7bb12baeu, 0x0cb61b38u,
    0x92d28e9bu, 0xe5d5be0du, 0x7cdcefb7u, 0x0bdbdf21u, 0x86d3d2d4u, 0xf1d4e242u,
    0x68ddb3f8u, 0x1fda836eu, 0x81be16cdu, 0xf6b9265bu, 0x6fb077e1u, 0x18b74777u,
    0x88085ae6u, 0xff0f6a70u, 0x66063bcau, 0x11010b5cu, 0x8f659effu, 0xf862ae69u,
    0x616bffd3u, 0x166ccf45u, 0xa00ae278u, 0xd70dd2eeu, 0x4e048354u, 0x3903b3c2u,
    0xa7672661u, 0xd06016f7u, 0x4969474du, 0x3e6e77dbu, 0xaed16a4au, 0xd9d65adcu,
    0x40df0b66u, 0x37d83bf0u, 0xa9bcae53u, 0xdebb9ec5u, 0x47b2cf7fu, 0x30b5ffe9u,
    0xbdbdf21cu, 0xcabac28au, 0x53b39330u, 0x24b4a3a6u, 0xbad03605u, 0xcdd70693u,
    0x54de5729u, 0x23d967bfu, 0xb3667a2eu, 0xc4614ab8u, 0x5d681b02u, 0x2a6f2b94u,
    0xb40bbe37u, 0xc30c8ea1u, 0x5a05df1bu, 0x2d02ef8du,
};

static int crc32_fn(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    (void)n;
    size_t len;
    const unsigned char* in = (const unsigned char*)embrp_str(ctx, &args[0], &len);
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) c = crc_table[(c ^ in[i]) & 0xFF] ^ (c >> 8);
    return embrp_set_int(ctx, out, (int64_t)(c ^ 0xFFFFFFFFu));
}

/* ---- the table ---- */

static const EmbrParam p_data[]   = { { "data", EMBR_T_STRING, 0 } };
static const EmbrParam p_text[]   = { { "text", EMBR_T_STRING, 0 } };
static const EmbrParam p_b64e[]   = { { "data", EMBR_T_STRING, 0 }, { "urlsafe", EMBR_T_ANY, EMBR_PARAM_OPTIONAL }, { "nopad", EMBR_T_ANY, EMBR_PARAM_OPTIONAL } };
static const EmbrParam p_b64d[]   = { { "text", EMBR_T_STRING, 0 }, { "urlsafe", EMBR_T_ANY, EMBR_PARAM_OPTIONAL } };

/* every function gives the same answer for the same arguments and changes nothing, so a const may call them while compiling */
static const EmbrFunc funcs[] = {
    { "base64_encode", "base64_encode(data: str, urlsafe?, nopad?) -> str. urlsafe (truthy) uses the '-' '_' alphabet, nopad (truthy) leaves out the '=' padding",
      b64_encode, NULL, p_b64e, 3, EMBR_T_STRING, EMBR_FUNC_PURE, 0 },
    { "base64_decode", "base64_decode(text: str, urlsafe?) -> str. strict: refuses characters outside the alphabet (white space too), bad padding and non-canonical trailing bits. padding may be left out",
      b64_decode, NULL, p_b64d, 2, EMBR_T_STRING, EMBR_FUNC_PURE, 0 },
    { "hex_encode", "hex_encode(data: str) -> str, lowercase",
      hex_encode, NULL, p_data, 1, EMBR_T_STRING, EMBR_FUNC_PURE, 0 },
    { "hex_decode", "hex_decode(text: str) -> str. either case, an even number of digits",
      hex_decode, NULL, p_text, 1, EMBR_T_STRING, EMBR_FUNC_PURE, 0 },
    { "sha256", "sha256(data: str) -> str, 64 lowercase hex characters",
      sha256_hex, NULL, p_data, 1, EMBR_T_STRING, EMBR_FUNC_PURE, 0 },
    { "sha256_raw", "sha256_raw(data: str) -> str, the 32 raw bytes",
      sha256_raw, NULL, p_data, 1, EMBR_T_STRING, EMBR_FUNC_PURE, 0 },
    { "crc32", "crc32(data: str) -> int, IEEE 802.3 as used by zip, gzip and png, 0 to 2^32-1",
      crc32_fn, NULL, p_data, 1, EMBR_T_INT, EMBR_FUNC_PURE, 0 },
};

static const EmbrPluginInfo info = {
    EMBR_PLUGIN_ABI_VERSION, sizeof(EmbrPluginInfo), "encoding", "1.0.0",
    "base64, hex, sha256 and crc32 for byte strings", EMBR_CAP_NONE, funcs, sizeof funcs / sizeof funcs[0], NULL, 0, NULL,
    NULL
};

EMBR_PLUGIN_EXPORT const EmbrPluginInfo* embr_plugin_info(void) { return &info; }
