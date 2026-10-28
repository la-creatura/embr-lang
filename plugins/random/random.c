/* random.c
 * secure randomness plus seedable shuffling and choice for embr, written in C against embr_plugin.h
 *
 * usage
 *   import "random"
 *   print(len(rand_bytes(16)))          # 16 raw random bytes (a byte string)
 *   print(rand_token(8))                # 16 hex chars, e.g. "9f2c41a7e0b35d68"
 *   print(rand_uuid4())                 # "b1f7e0c2-..."
 *   rand_seed(42)                       # make the functions below reproducible
 *   print(rand_shuffle([1,2,3,4]))
 *   print(rand_choice(["a","b","c"]))
 *   print(rand_sample([1,2,3,4,5], 2))
 *
 * good to know
 *   - rand_bytes, rand_token and rand_uuid4 always read the operating system's entropy source, never the seeded generator,
 *     so seeding can't make a token predictable
 *   - rand_seed only seeds the generator behind rand_shuffle, rand_choice and rand_sample. embrmath has its own generator
 *     (math_seed), plugins don't share state: to get one reproducible stream for everything, seed both
 *   - the seeded generator is MT19937-64 and numbers below n are drawn like libstdc++ does it (a 128-bit multiply), so a
 *     seed gives the same shuffle as the C++ plugin this replaced. it is kept per thread, so threads don't race
 */
#if defined(_WIN32)
#  define _CRT_RAND_S   /* before the first stdlib.h, for rand_s */
#endif
#include <embr_plugin.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#  include <errno.h>
#  include <fcntl.h>
#  include <unistd.h>
#  if defined(__linux__)
#    include <sys/random.h>
#  endif
#endif

#if defined(_MSC_VER)
#  define THREAD_LOCAL __declspec(thread)
#else
#  define THREAD_LOCAL __thread
#endif

#define VEC_TAG "vec.handle"   /* keep in sync with plugins/vec/vec.cpp */

/* fills buf from the operating system's secure source. 0, or nonzero after the error was recorded (it never falls back to
   something predictable) */
static int secure_fill(EmbrCtx* ctx, uint8_t* buf, size_t n) {
#if defined(_WIN32)
    for (size_t i = 0; i < n;) {
        unsigned int r;
        if (rand_s(&r) != 0) return embrp_fail(ctx, "no secure random source available");
        for (int k = 0; k < 4 && i < n; ++k, r >>= 8) buf[i++] = (uint8_t)r;
    }
    return 0;
#else
    size_t got = 0;
#  if defined(__linux__)
    while (got < n) {
        ssize_t r = getrandom(buf + got, n - got, 0);
        if (r < 0) { if (errno == EINTR) continue; break; }
        got += (size_t)r;
    }
    if (got == n) return 0;
#  endif
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return embrp_fail(ctx, "no secure random source available");
    while (got < n) {
        ssize_t r = read(fd, buf + got, n - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) { close(fd); return embrp_fail(ctx, "failed to read secure random bytes"); }
        got += (size_t)r;
    }
    close(fd);
    return 0;
#endif
}

/* ---- MT19937-64, the generator behind the seeded functions ---- */

#define MT_N 312
#define MT_M 156

typedef struct { uint64_t mt[MT_N]; int idx; int ready; } Mt;
static THREAD_LOCAL Mt g_rng;

static void mt_seed(Mt* g, uint64_t value) {
    g->mt[0] = value;
    for (int i = 1; i < MT_N; ++i)
        g->mt[i] = 6364136223846793005ULL * (g->mt[i - 1] ^ (g->mt[i - 1] >> 62)) + (uint64_t)i;
    g->idx = MT_N;
    g->ready = 1;
}

static uint64_t mt_next(Mt* g) {
    if (g->idx >= MT_N) {
        static const uint64_t mag[2] = {0, 0xB5026F5AA96619E9ULL};
        const uint64_t upper = 0xFFFFFFFF80000000ULL, lower = 0x7FFFFFFFULL;
        int i;
        for (i = 0; i < MT_N - MT_M; ++i) {
            uint64_t x = (g->mt[i] & upper) | (g->mt[i + 1] & lower);
            g->mt[i] = g->mt[i + MT_M] ^ (x >> 1) ^ mag[x & 1];
        }
        for (; i < MT_N - 1; ++i) {
            uint64_t x = (g->mt[i] & upper) | (g->mt[i + 1] & lower);
            g->mt[i] = g->mt[i + (MT_M - MT_N)] ^ (x >> 1) ^ mag[x & 1];
        }
        uint64_t x = (g->mt[MT_N - 1] & upper) | (g->mt[0] & lower);
        g->mt[MT_N - 1] = g->mt[MT_M - 1] ^ (x >> 1) ^ mag[x & 1];
        g->idx = 0;
    }
    uint64_t y = g->mt[g->idx++];
    y ^= (y >> 29) & 0x5555555555555555ULL;
    y ^= (y << 17) & 0x71D67FFFEDA60000ULL;
    y ^= (y << 37) & 0xFFF7EEE000000000ULL;
    y ^= y >> 43;
    return y;
}

/* the first use on a thread seeds from the secure source (32 bits, like std::random_device) */
static int rng_ready(EmbrCtx* ctx) {
    if (g_rng.ready) return 0;
    uint8_t b[4];
    if (secure_fill(ctx, b, sizeof b)) return 1;
    mt_seed(&g_rng, (uint64_t)b[0] | ((uint64_t)b[1] << 8) | ((uint64_t)b[2] << 16) | ((uint64_t)b[3] << 24));
    return 0;
}

/* the 128-bit product of two 64-bit numbers */
static void mul64(uint64_t a, uint64_t b, uint64_t* hi, uint64_t* lo) {
    uint64_t a0 = a & 0xFFFFFFFFu, a1 = a >> 32, b0 = b & 0xFFFFFFFFu, b1 = b >> 32;
    uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    uint64_t mid = (p00 >> 32) + (p01 & 0xFFFFFFFFu) + (p10 & 0xFFFFFFFFu);
    *lo = (p00 & 0xFFFFFFFFu) | (mid << 32);
    *hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
}

/* uniform in [0, n), n > 0: the high half of next * n, and a redraw when the low half is in the biased part (Lemire's method,
   which is what libstdc++ does) */
static size_t below(size_t n) {
    uint64_t hi, lo;
    mul64(mt_next(&g_rng), (uint64_t)n, &hi, &lo);
    if (lo < n) {
        uint64_t threshold = (0 - (uint64_t)n) % n;
        while (lo < threshold) mul64(mt_next(&g_rng), (uint64_t)n, &hi, &lo);
    }
    return (size_t)hi;
}

/* a count argument: an int, not negative, not above max */
static int count_arg(EmbrCtx* ctx, const EmbrValue* v, const char* what, int64_t max, int64_t* out) {
    char msg[96];
    if (embrp_type(ctx, v) != EMBR_TAG_INT) { snprintf(msg, sizeof msg, "%s must be an integer", what); return embrp_fail(ctx, msg); }
    int64_t n = embrp_to_int(ctx, v);
    if (n < 0) { snprintf(msg, sizeof msg, "%s must not be negative", what); return embrp_fail(ctx, msg); }
    if (n > max) { snprintf(msg, sizeof msg, "%s too large (max %lld)", what, (long long)max); return embrp_fail(ctx, msg); }
    *out = n;
    return 0;
}

static int rand_bytes(EmbrCtx* ctx, const EmbrValue* args, size_t argc, EmbrValue* out) {
    int64_t n = 0; (void)argc;
    if (count_arg(ctx, &args[0], "n", 1 << 20, &n)) return 1;
    char* p = embrp_set_str_alloc(ctx, out, (size_t)n);
    if (!p && n) return 1;
    return n ? secure_fill(ctx, (uint8_t*)p, (size_t)n) : 0;
}

static int rand_token(EmbrCtx* ctx, const EmbrValue* args, size_t argc, EmbrValue* out) {
    int64_t n = 16;
    if (argc >= 1 && count_arg(ctx, &args[0], "nbytes", 1 << 16, &n)) return 1;
    uint8_t* raw = malloc((size_t)n + 1);
    char* hex = malloc((size_t)n * 2 + 1);
    int rc = 0;
    if (!raw || !hex) rc = embrp_fail(ctx, "out of memory");
    else if (!(rc = secure_fill(ctx, raw, (size_t)n))) {
        static const char d[] = "0123456789abcdef";
        for (int64_t i = 0; i < n; ++i) { hex[2 * i] = d[raw[i] >> 4]; hex[2 * i + 1] = d[raw[i] & 15]; }
        rc = embrp_set_str(ctx, out, hex, (size_t)n * 2);
    }
    free(raw); free(hex);
    return rc;
}

static int rand_uuid4(EmbrCtx* ctx, const EmbrValue* args, size_t argc, EmbrValue* out) {
    uint8_t b[16]; (void)args; (void)argc;
    if (secure_fill(ctx, b, 16)) return 1;
    b[6] = (uint8_t)((b[6] & 0x0F) | 0x40);
    b[8] = (uint8_t)((b[8] & 0x3F) | 0x80);
    char buf[37];
    snprintf(buf, sizeof buf, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return embrp_set_str(ctx, out, buf, 36);
}

static int rand_seed(EmbrCtx* ctx, const EmbrValue* args, size_t argc, EmbrValue* out) {
    (void)argc;
    if (embrp_type(ctx, &args[0]) != EMBR_TAG_INT) return embrp_fail(ctx, "seed must be an integer");
    mt_seed(&g_rng, (uint64_t)embrp_to_int(ctx, &args[0]));
    return embrp_set_float(ctx, out, 0.0);
}

static int is_list(EmbrCtx* ctx, const EmbrValue* v) {
    uint32_t tag = embrp_type(ctx, v);
    return tag == EMBR_TAG_ARRAY || (tag == EMBR_TAG_POINTER && strcmp(embrp_ptr_tag(ctx, v), VEC_TAG) == 0);
}

static int rand_shuffle(EmbrCtx* ctx, const EmbrValue* args, size_t argc, EmbrValue* out) {
    (void)argc;
    if (rng_ready(ctx)) return 1;
    size_t n = embrp_array_len(ctx, &args[0]);
    /* the positions are shuffled, not the values, and the result is built from them */
    size_t* pos = malloc((n ? n : 1) * sizeof *pos);
    if (!pos) return embrp_fail(ctx, "out of memory");
    for (size_t k = 0; k < n; ++k) pos[k] = k;
    for (size_t i = n; i > 1; --i) { size_t j = below(i), t = pos[i - 1]; pos[i - 1] = pos[j]; pos[j] = t; }
    int rc = embrp_set_array(ctx, out, n);
    for (size_t k = 0; !rc && k < n; ++k) rc = embrp_array_push(ctx, out, embrp_array_at(ctx, &args[0], pos[k]));
    free(pos);
    return rc;
}

static int rand_choice(EmbrCtx* ctx, const EmbrValue* args, size_t argc, EmbrValue* out) {
    (void)argc;
    if (!is_list(ctx, &args[0])) {
        char msg[160];
        snprintf(msg, sizeof msg, "expected an array or a vec, got ptr with tag '%.100s'", embrp_ptr_tag(ctx, &args[0]));
        return embrp_fail(ctx, msg);
    }
    size_t n = embrp_array_len(ctx, &args[0]);
    if (n == 0) return embrp_fail(ctx, "cannot choose from an empty array");
    if (rng_ready(ctx)) return 1;
    return embrp_set_copy(ctx, out, embrp_array_at(ctx, &args[0], below(n)));
}

static int rand_sample(EmbrCtx* ctx, const EmbrValue* args, size_t argc, EmbrValue* out) {
    int64_t k = 0; (void)argc;
    if (count_arg(ctx, &args[1], "k", INT64_MAX, &k)) return 1;
    size_t n = embrp_array_len(ctx, &args[0]);
    if ((size_t)k > n) {
        char msg[120];
        snprintf(msg, sizeof msg, "k (%lld) exceeds array length (%zu)", (long long)k, n);
        return embrp_fail(ctx, msg);
    }
    if (rng_ready(ctx)) return 1;
    size_t* idx = malloc((n ? n : 1) * sizeof *idx);
    if (!idx) return embrp_fail(ctx, "out of memory");
    for (size_t i = 0; i < n; ++i) idx[i] = i;
    for (size_t i = 0; i < (size_t)k; ++i) { size_t j = i + below(n - i), t = idx[i]; idx[i] = idx[j]; idx[j] = t; }
    int rc = embrp_set_array(ctx, out, (size_t)k);
    for (size_t i = 0; !rc && i < (size_t)k; ++i) rc = embrp_array_push(ctx, out, embrp_array_at(ctx, &args[0], idx[i]));
    free(idx);
    return rc;
}

static const EmbrParam p_n[]      = { { "n", EMBR_T_NUM, 0 } };
static const EmbrParam p_nbytes[] = { { "nbytes", EMBR_T_NUM, EMBR_PARAM_OPTIONAL } };
static const EmbrParam p_arr[]    = { { "arr", EMBR_T_ARRAY, 0 } };
static const EmbrParam p_choice[] = { { "arr", EMBR_T_ARRAY | EMBR_T_POINTER, 0 } };
static const EmbrParam p_sample[] = { { "arr", EMBR_T_ARRAY, 0 }, { "k", EMBR_T_NUM, 0 } };

static const EmbrFunc funcs[] = {
    { "rand_bytes", "rand_bytes(n: num) -> str. n raw bytes from the OS CSPRNG (getrandom, /dev/urandom fallback); max 1 MiB; raises if no secure source. Never uses the seeded generator",
      rand_bytes, NULL, p_n, 1, EMBR_T_STRING, 0, 0 },
    { "rand_choice", "rand_choice(arr: arr|ptr). one element, chosen with the seeded generator; raises on an empty array. a vec is read in place",
      rand_choice, NULL, p_choice, 1, EMBR_T_ANY, 0, 0 },
    { "rand_sample", "rand_sample(arr: arr, k: num) -> arr. k distinct positions; raises if k > len(arr)",
      rand_sample, NULL, p_sample, 2, EMBR_T_ARRAY, 0, 0 },
    { "rand_seed", "rand_seed(n: num) -> num. seeds the shuffle, choice and sample generator",
      rand_seed, NULL, p_n, 1, EMBR_T_FLOAT, 0, 0 },
    { "rand_shuffle", "rand_shuffle(arr: arr) -> arr. a new array (Fisher-Yates); the input is untouched",
      rand_shuffle, NULL, p_arr, 1, EMBR_T_ARRAY, 0, 0 },
    { "rand_token", "rand_token(nbytes?: num) -> str. lowercase hex of nbytes secure bytes (default 16 gives 32 chars; max 65536)",
      rand_token, NULL, p_nbytes, 1, EMBR_T_STRING, 0, 0 },
    { "rand_uuid4", "rand_uuid4() -> str. RFC 4122 v4 from the OS CSPRNG",
      rand_uuid4, NULL, NULL, 0, EMBR_T_STRING, 0, 0 },
};

static const EmbrPluginInfo info = {
    EMBR_PLUGIN_ABI_VERSION, sizeof(EmbrPluginInfo), "random", "1.0.0", "secure randomness, seedable shuffle/choice",
    EMBR_CAP_NONE, funcs, sizeof funcs / sizeof funcs[0], NULL, 0, NULL, NULL
};

EMBR_PLUGIN_EXPORT const EmbrPluginInfo* embr_plugin_info(void) { return &info; }
