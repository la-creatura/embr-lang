// random.cpp
// secure randomness plus seedable shuffling/choice for embr
//
// usage
//   import "random"
//   print(len(rand_bytes(16)))          # 16 raw random bytes (a byte string)
//   print(rand_token(8))                # 16 hex chars, e.g. "9f2c41a7e0b35d68"
//   print(rand_uuid4())                 # "b1f7e0c2-..."
//   rand_seed(42)                       # make the functions below reproducible
//   print(rand_shuffle([1,2,3,4]))
//   print(rand_choice(["a","b","c"]))
//   print(rand_sample([1,2,3,4,5], 2))
//
// note on embrmath: plugins/embrmath has its own seedable PRNG (math_seed/math_random/math_randint). this plugin
// doesn't share its state (plugins don't depend on each other): rand_seed() seeds only the generator behind
// rand_shuffle/rand_choice/rand_sample. to get one reproducible stream for everything, seed both
// rand_bytes/rand_token/rand_uuid4 always read the OS entropy source and never the seeded generator, so seeding
// can't make a token predictable
// the seeded generator is thread_local, like embrmath's, so threads don't race

#include <embr/embr.h>
#include "../vec/vec.h"
#include <cstdio>
#include <cstring>
#include <random>

#if !defined(_WIN32)
#  include <fcntl.h>
#  include <unistd.h>
#  if defined(__linux__)
#    include <sys/random.h>
#  endif
#endif

using namespace embr;

static Param pNum(std::string n)                      { return Param::req(std::move(n), TS::Num); }
static Param pArr(std::string n)                      { return Param::req(std::move(n), TS::Arr); }

[[noreturn]] static void fail(const std::string& fn, const std::string& msg) {
    raiseError("[random:" + fn + "]", msg);
}

static thread_local std::mt19937_64 g_rng{std::random_device{}()};

// fills buf from the operating system's CSPRNG; raises if none is available
// (never silently falls back to something predictable)
static void secureFill(const char* fn, uint8_t* buf, size_t n) {
#if defined(_WIN32)
    (void)fn;                // only the other platforms can fail and name the function
    std::random_device rd;   // backed by BCryptGenRandom/RtlGenRandom on MSVC and modern MinGW
    for (size_t i = 0; i < n; ++i) buf[i] = (uint8_t)rd();
    return;
#else
    size_t got = 0;
#  if defined(__linux__)
    while (got < n) {
        ssize_t r = getrandom(buf + got, n - got, 0);
        if (r < 0) { if (errno == EINTR) continue; break; }
        got += (size_t)r;
    }
    if (got == n) return;
#  endif
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) fail(fn, "no secure random source available");
    while (got < n) {
        ssize_t r = read(fd, buf + got, n - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) { close(fd); fail(fn, "failed to read secure random bytes"); }
        got += (size_t)r;
    }
    close(fd);
#endif
}

static int64_t countArg(const Value& v, const char* fn, const char* what, int64_t max) {
    if (!v.isInt()) fail(fn, std::string(what) + " must be an integer");
    int64_t n = v.asInt();
    if (n < 0) fail(fn, std::string(what) + " must not be negative");
    if (n > max) fail(fn, std::string(what) + " too large (max " + std::to_string(max) + ")");
    return n;
}

static size_t below(size_t n) {   // uniform in [0, n), n > 0
    return std::uniform_int_distribution<size_t>(0, n - 1)(g_rng);
}

EMBR_PLUGIN {
    // rand_bytes(n: int) -> str   (n raw random bytes; max 1 MiB per call)
    interp->bindSpan("rand_bytes", {pNum("n")},
    [](const ArgSpan& args) -> Value {
        int64_t n = countArg(args[0], "rand_bytes", "n", 1 << 20);
        std::string out((size_t)n, '\0');
        secureFill("rand_bytes", (uint8_t*)out.data(), out.size());
        return Value(std::move(out));
    });

    // rand_token(nbytes?: int) -> str   (hex of nbytes random bytes; default 16 -> 32 chars)
    interp->bindSpan("rand_token", {Param::opt("nbytes", TS::Num)},
    [](const ArgSpan& args) -> Value {
        int64_t n = args.empty() ? 16 : countArg(args[0], "rand_token", "nbytes", 1 << 16);
        std::string raw((size_t)n, '\0');
        secureFill("rand_token", (uint8_t*)raw.data(), raw.size());
        static const char* d = "0123456789abcdef";
        std::string out;
        for (unsigned char c : raw) { out += d[c >> 4]; out += d[c & 15]; }
        return Value(std::move(out));
    });

    // rand_uuid4() -> str   (RFC 4122 version 4, lowercase, from the OS entropy source)
    interp->bindSpan("rand_uuid4", {},
    [](const ArgSpan&) -> Value {
        uint8_t b[16];
        secureFill("rand_uuid4", b, 16);
        b[6] = (b[6] & 0x0F) | 0x40;
        b[8] = (b[8] & 0x3F) | 0x80;
        char buf[37];
        std::snprintf(buf, sizeof buf,
            "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7],b[8],b[9],b[10],b[11],b[12],b[13],b[14],b[15]);
        return Value(std::string(buf));
    });

    // rand_seed(n: int) -> 0   (seeds rand_shuffle/rand_choice/rand_sample only)
    interp->bindSpan("rand_seed", {pNum("n")},
    [](const ArgSpan& args) -> Value {
        if (!args[0].isInt()) fail("rand_seed", "seed must be an integer");
        g_rng.seed((uint64_t)args[0].asInt());
        return Value(0.0);
    });

    // rand_shuffle(arr: arr) -> arr   (new array, Fisher-Yates; input untouched)
    interp->bindSpan("rand_shuffle", {pArr("arr")},
    [](const ArgSpan& args) -> Value {
        // shuffles the positions, not the Values, then builds the result from them (same permutation either way)
        const Value::array_type& src = args[0].asArray();
        std::vector<size_t> pos(src.size());
        for (size_t k = 0; k < pos.size(); ++k) pos[k] = k;
        for (size_t i = pos.size(); i > 1; --i) std::swap(pos[i - 1], pos[below(i)]);
        Value::array_type a;
        a.reserve(src.size());
        for (size_t k : pos) a.push_back(src[k]);
        return Value(std::move(a));
    });

    // rand_choice(arr: arr) -> any   (raises on an empty array)
    interp->bindSpan("rand_choice", {Param::req("arr", TS::Arr|TS::Ptr)},
    [](const ArgSpan& args) -> Value {
        // a vec is read in place, so picking from a big one in a loop stays O(1)
        embrvec::Vec* vp = args[0].isPointer() ? embrvec::vecOf(args[0]) : nullptr;
        if (args[0].isPointer() && !vp) fail("rand_choice", "expected an array or a vec, got ptr with tag '" + args[0].asPointer().type + "'");
        const auto& a = vp ? vp->items : args[0].asArray();
        if (a.empty()) fail("rand_choice", "cannot choose from an empty array");
        return a[below(a.size())];
    });

    // rand_sample(arr: arr, k: int) -> arr   (k distinct positions, no repeats; k <= len)
    interp->bindSpan("rand_sample", {pArr("arr"), pNum("k")},
    [](const ArgSpan& args) -> Value {
        const auto& a = args[0].asArray();
        int64_t k = countArg(args[1], "rand_sample", "k", INT64_MAX);
        if ((size_t)k > a.size())
            fail("rand_sample", "k (" + std::to_string(k) + ") exceeds array length (" +
                 std::to_string(a.size()) + ")");
        std::vector<size_t> idx(a.size());
        for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
        for (size_t i = 0; i < (size_t)k; ++i) std::swap(idx[i], idx[i + below(idx.size() - i)]);
        Value::array_type out;
        for (size_t i = 0; i < (size_t)k; ++i) out.push_back(a[idx[i]]);
        return Value(std::move(out));
    });

    // what each always returns (see Value::Callable::returns)
    interp->setResultTypes({
        {"rand_bytes", TS::Str}, {"rand_token", TS::Str}, {"rand_uuid4", TS::Str}, {"rand_seed", TS::Float},
        {"rand_shuffle", TS::Arr}, {"rand_sample", TS::Arr},
    });
}

// what this plugin says about itself. embr reads it before it runs embr_register (see core/plugininfo.h)
EMBR_PLUGIN_INFO("random", "1.0.0", "secure randomness, seedable shuffle/choice", EMBR_CAP_NONE, EMBR_CAP_NONE,
    {"rand_bytes", "n raw bytes from the OS CSPRNG (getrandom, /dev/urandom fallback); max 1 MiB; raises if no secure source. Never uses the seeded generator"},
    {"rand_choice", "raises on an empty array"},
    {"rand_sample", "k distinct positions; raises if k > len(arr)"},
    {"rand_seed", "seeds the shuffle/choice/sample generator"},
    {"rand_shuffle", "new array (Fisher-Yates); input untouched"},
    {"rand_token", "lowercase hex of nbytes secure bytes (default 16 → 32 chars; max 65536)"},
    {"rand_uuid4", "RFC 4122 v4 from the OS CSPRNG"});
