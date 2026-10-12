// embrmath.cpp
// math plugin for embr: <cmath> wrappers, min/max/clamp, and a seedable PRNG
//
// usage
//   import "embrmath"
//   print(math_sqrt(16))
//   print(math_clamp(12, 0, 10))
//   math_seed(42)
//   print(math_randint(1, 6))
//
// naming follows str.cpp's str_* convention: everything is math_* (not bare sqrt, min, ...) so it can't collide with
// a script's own variables
//
// good to know
//   - abs, floor, ceil, round, min, max and clamp are overload groups (see the wiki, type-system): __1 takes ints and gives an
//     int, __2 takes floats, and min, max and clamp have a __3 for a mix. a call picks the member from the argument types, so
//     the checker knows `math_abs(n)` of an int is an int. a `num` argument (int or float) is picked when the script runs

#include <embr/embr.h>
#include <cmath>
#include <random>

using namespace embr;

static Param pNum(std::string n)   { return Param::req(std::move(n), TS::Num); }
static Param pFloat(std::string n) { return Param::req(std::move(n), TS::Float); }

static void throwError(const std::string& fn, const std::string& msg) {
    raiseError("[embrmath:" + fn + "]", msg);
}

// PRNG state, seeded from a real random source by default. math_seed() reseeds it deterministically (for
// reproducible tests)
//
// thread_local, not a plain static: a static would be one lock-free generator shared by every Interpreter/thread
// that loads this .so, which is a data race. thread_local gives each thread (embr's model is one isolated
// Interpreter per thread, see plugins/parallel) its own generator. the catch: math_seed() reseeds only the calling
// thread's stream, which is what you want for reproducible worker tasks
static thread_local std::mt19937_64 g_rng{std::random_device{}()};

// the int members (__1): an int in gives an int out, so `n = 0` then `n = math_abs(n - 5)` keeps n an int
int64_t math_abs_int(int64_t x) {
    if (x == INT64_MIN) throwError("math_abs", "the smallest int has no int absolute value");
    return x < 0 ? -x : x;
}
int64_t math_same_int(int64_t x) { return x; }   // floor, ceil and round of an int
int64_t math_min_int(int64_t a, int64_t b) { return b < a ? b : a; }
int64_t math_max_int(int64_t a, int64_t b) { return b > a ? b : a; }
int64_t math_clamp_int(int64_t x, int64_t lo, int64_t hi) {
    if (lo > hi) throwError("math_clamp", "lo (" + std::to_string(lo) + ") must not exceed hi (" + std::to_string(hi) + ")");
    return x < lo ? lo : x > hi ? hi : x;
}

// the float members (__2, and __3 for a mix). floor, ceil and round give a whole number as an int; one too big for an int, or not
// finite, stays a float
static Value wholeNumber(double d) {
    if (d >= -9223372036854775808.0 && d < 9223372036854775808.0) return Value((int64_t)d);
    return Value(d);
}
Value math_abs  (const ArgSpan& args) { return Value(std::fabs(args[0].asNumber())); }
Value math_floor(const ArgSpan& args) { return wholeNumber(std::floor(args[0].asNumber())); }
Value math_ceil (const ArgSpan& args) { return wholeNumber(std::ceil (args[0].asNumber())); }
Value math_round(const ArgSpan& args) { return wholeNumber(std::round(args[0].asNumber())); }

// min, max and clamp give back one of the arguments as it was
Value math_min(const ArgSpan& args) { return args[1].asNumber() < args[0].asNumber() ? args[1] : args[0]; }
Value math_max(const ArgSpan& args) { return args[1].asNumber() > args[0].asNumber() ? args[1] : args[0]; }

Value math_clamp(const ArgSpan& args) {
    double x = args[0].asNumber(), lo = args[1].asNumber(), hi = args[2].asNumber();
    if (lo > hi) throwError("math_clamp", "lo (" + formatNumber(lo) + ") must not exceed hi (" + formatNumber(hi) + ")");
    return x < lo ? args[1] : x > hi ? args[2] : args[0];
}

// math_seed(n): reseeds the shared PRNG deterministically. call this at
// the top of a script before math_random()/math_randint() for reproducible
// output (e.g. in a test).
Value math_seed(const ArgSpan& args) {
    g_rng.seed((uint64_t)numToInt64(args[0], "math_seed", "seed"));
    return Value(0.0);
}

// math_random() -> float in [0, 1)
double math_random() {
    return std::uniform_real_distribution<double>(0.0, 1.0)(g_rng);
}

// math_randint(lo, hi) -> integer in [lo, hi], inclusive both ends
Value math_randint(const ArgSpan& args) {
    int64_t lo = numToInt64(args[0], "math_randint", "lo"), hi = numToInt64(args[1], "math_randint", "hi");
    if (lo > hi) throwError("math_randint", "lo (" + std::to_string(lo) + ") must not exceed hi (" + std::to_string(hi) + ")");
    return Value(std::uniform_int_distribution<int64_t>(lo, hi)(g_rng));
}

EMBR_PLUGIN {
    interp->define("PI", Value(3.14159265358979323846));
    interp->define("E",  Value(2.71828182845904523536));

    // the ones that always take numbers and give a float are typed: the parameters and the result type come from the C++ signature
    interp->bindTyped("math_sqrt", {"x"}, +[](double x) { return std::sqrt(x); });
    interp->bindTyped("math_pow",  {"x", "y"}, +[](double x, double y) { return std::pow(x, y); });
    // overload groups: __1 for ints, __2 for floats, __3 for a mix (see the file header)
    interp->bindTyped("math_abs__1",   {"x"}, math_abs_int);
    interp->bindSpan("math_abs__2",    {pFloat("x")}, math_abs);
    interp->bindTyped("math_floor__1", {"x"}, math_same_int);
    interp->bindSpan("math_floor__2",  {pFloat("x")}, math_floor);
    interp->bindTyped("math_ceil__1",  {"x"}, math_same_int);
    interp->bindSpan("math_ceil__2",   {pFloat("x")}, math_ceil);
    interp->bindTyped("math_round__1", {"x"}, math_same_int);
    interp->bindSpan("math_round__2",  {pFloat("x")}, math_round);

    interp->bindTyped("math_sin", {"x"}, +[](double x) { return std::sin(x); });
    interp->bindTyped("math_cos", {"x"}, +[](double x) { return std::cos(x); });
    interp->bindTyped("math_tan", {"x"}, +[](double x) { return std::tan(x); });
    interp->bindTyped("math_asin", {"x"}, +[](double x) { return std::asin(x); });
    interp->bindTyped("math_acos", {"x"}, +[](double x) { return std::acos(x); });
    interp->bindTyped("math_atan", {"x"}, +[](double x) { return std::atan(x); });
    // math_atan2(y, x): same argument order as std::atan2 and every other language's atan2
    interp->bindTyped("math_atan2", {"y", "x"}, +[](double y, double x) { return std::atan2(y, x); });

    interp->bindTyped("math_log",   {"x"}, +[](double x) { return std::log(x); });   // natural log
    interp->bindTyped("math_log2",  {"x"}, +[](double x) { return std::log2(x); });
    interp->bindTyped("math_log10", {"x"}, +[](double x) { return std::log10(x); });
    interp->bindTyped("math_exp",   {"x"}, +[](double x) { return std::exp(x); });

    interp->bindTyped("math_min__1",   {"a", "b"}, math_min_int);
    interp->bindSpan("math_min__2",    {pFloat("a"), pFloat("b")}, math_min);
    interp->bindSpan("math_min__3",    {pNum("a"), pNum("b")}, math_min);
    interp->bindTyped("math_max__1",   {"a", "b"}, math_max_int);
    interp->bindSpan("math_max__2",    {pFloat("a"), pFloat("b")}, math_max);
    interp->bindSpan("math_max__3",    {pNum("a"), pNum("b")}, math_max);
    interp->bindTyped("math_clamp__1", {"x", "lo", "hi"}, math_clamp_int);
    interp->bindSpan("math_clamp__2",  {pFloat("x"), pFloat("lo"), pFloat("hi")}, math_clamp);
    interp->bindSpan("math_clamp__3",  {pNum("x"), pNum("lo"), pNum("hi")}, math_clamp);

    interp->bindSpan("math_seed",    {pNum("n")}, math_seed);
    interp->bindTyped("math_random", {}, math_random);
    interp->bindSpan("math_randint", {pNum("lo"), pNum("hi")}, math_randint);

    // what each of the others always returns (see Value::Callable::returns)
    interp->setResultTypes({
        {"math_abs__2", TS::Float}, {"math_floor__2", TS::Num}, {"math_ceil__2", TS::Num}, {"math_round__2", TS::Num},
        {"math_min__2", TS::Float}, {"math_max__2", TS::Float}, {"math_clamp__2", TS::Float},
        {"math_min__3", TS::Num}, {"math_max__3", TS::Num}, {"math_clamp__3", TS::Num},
        {"math_seed", TS::Float}, {"math_randint", TS::Int},
    });

    // the same answer for the same arguments and no side effects, so a const may call them while compiling
    interp->setPure({
        "math_sqrt", "math_pow", "math_sin", "math_cos", "math_tan", "math_asin", "math_acos", "math_atan",
        "math_atan2", "math_log", "math_log2", "math_log10", "math_exp",
        "math_abs__1", "math_abs__2", "math_floor__1", "math_floor__2", "math_ceil__1", "math_ceil__2",
        "math_round__1", "math_round__2", "math_min__1", "math_min__2", "math_min__3", "math_max__1",
        "math_max__2", "math_max__3", "math_clamp__1", "math_clamp__2", "math_clamp__3",
    });
}

// what this plugin says about itself. embr reads it before it runs embr_register (see core/plugininfo.h)
EMBR_PLUGIN_INFO("embrmath", "1.0.0", "math wrappers, min/max/clamp, seedable PRNG", EMBR_CAP_NONE, EMBR_CAP_NONE,
    {"math_abs__1", "absolute value of an int, an int. raises for the smallest int"},
    {"math_abs__2", "absolute value of a float"},
    {"math_acos", "arc cosine of x, in radians"},
    {"math_asin", "arc sine of x, in radians"},
    {"math_atan", "arc tangent of x, in radians"},
    {"math_atan2", "argument order is the same as std::atan2"},
    {"math_ceil__1", "an int is its own ceiling"},
    {"math_ceil__2", "the smallest whole number not below x, as an int (or a float if too big for an int)"},
    {"math_clamp__1", "x limited to lo..hi, all ints. raises if lo > hi"},
    {"math_clamp__2", "x limited to lo..hi, all floats. raises if lo > hi"},
    {"math_clamp__3", "x limited to lo..hi for a mix of ints and floats. raises if lo > hi"},
    {"math_cos", "cosine of x, in radians"},
    {"math_exp", "e raised to the power x"},
    {"math_floor__1", "an int is its own floor"},
    {"math_floor__2", "the largest whole number not above x, as an int (or a float if too big for an int)"},
    {"math_log", "natural logarithm of x"},
    {"math_log10", "base 10 logarithm of x"},
    {"math_log2", "base 2 logarithm of x"},
    {"math_max__1", "the larger of two ints"},
    {"math_max__2", "the larger of two floats"},
    {"math_max__3", "the larger of an int and a float, as it was given"},
    {"math_min__1", "the smaller of two ints"},
    {"math_min__2", "the smaller of two floats"},
    {"math_min__3", "the smaller of an int and a float, as it was given"},
    {"math_pow", "x raised to the power y"},
    {"math_randint", "uniform int, inclusive both ends; raises if lo > hi"},
    {"math_random", "uniform float in [0, 1)"},
    {"math_round__1", "an int is its own rounding"},
    {"math_round__2", "x rounded to the nearest whole number, halves away from zero, as an int (or a float if too big for an int)"},
    {"math_seed", "reseeds the PRNG for the calling thread only"},
    {"math_sin", "sine of x, in radians"},
    {"math_sqrt", "direct <cmath> wrappers, a float result"},
    {"math_tan", "tangent of x, in radians"});
