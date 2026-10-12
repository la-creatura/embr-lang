// time.cpp
// time/date plugin for embr: wall clock, monotonic elapsed-time measurement, sleep, and UTC date formatting/parsing
//
// usage
//   import "time"
//   t0 = time_monotonic()
//   time_sleep(0.1)
//   print(time_monotonic() - t0 >= 0.1)
//   print(time_format(0, "%Y-%m-%d"))              # "1970-01-01"
//   print(time_parse("2024-01-02", "%Y-%m-%d"))     # seconds since epoch
//   print(time_format(0, "%H:%M %z", 3600))         # "01:00 +0100"
//
// calendar math (time_format/time_parse) is done in UTC with this file's own day-count conversion (Howard Hinnant's
// days_from_civil/civil_from_days, public domain), not gmtime/timegm: those aren't portable (timegm is POSIX-only,
// gmtime_r/gmtime_s differ, MinGW has neither reliably). the resulting struct tm still goes through
// std::strftime/std::get_time, so callers get full strftime syntax
//
// good to know
//   - time zones are fixed offsets in seconds east of UTC, there is no named zone ("Europe/Berlin") or DST table
//   - %z is handled by this file (strftime/get_time disagree on it across platforms), %Z is not supported

#include <embr/embr.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <thread>

using namespace embr;

static Param pNum(std::string n)                      { return Param::req(std::move(n), TS::Num); }
static Param pStr(std::string n)                      { return Param::req(std::move(n), TS::Str); }

static void throwError(const std::string& fn, const std::string& msg) {
    raiseError("[time:" + fn + "]", msg);
}

// days since the epoch (1970-01-01) for a proleptic-Gregorian y/m/d
// (Howard Hinnant's days_from_civil, public domain: http://howardhinnant.github.io/date_algorithms.html)
static long long daysFromCivil(long long y, unsigned m, unsigned d) {
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long long)doe - 719468;
}

// inverse of daysFromCivil (civil_from_days)
static void civilFromDays(long long z, int& y, unsigned& m, unsigned& d) {
    z += 719468;
    long long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long yy = (long long)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp + (mp < 10 ? 3 : -9);
    y = (int)(yy + (m <= 2 ? 1 : 0));
}

// epoch seconds -> UTC struct tm, via our own portable civil-calendar math
static std::tm tmFromEpoch(double epoch) {
    // NaN/Inf/huge values would be undefined behaviour in the integer conversion below (and could not
    // be represented in tm_year anyway); +-1e15 seconds is about +-31.7 million years
    if (!(epoch > -1e15 && epoch < 1e15))
        throwError("time_format", "epoch out of the supported range (+-1e15 seconds), got " + formatNumber(epoch));
    long long secs = (long long)std::floor(epoch);
    long long days = secs >= 0 ? secs / 86400 : -((-secs + 86399) / 86400);
    long long rem  = secs - days * 86400;

    int y; unsigned mo, d;
    civilFromDays(days, y, mo, d);

    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon  = (int)mo - 1;
    tm.tm_mday = (int)d;
    tm.tm_hour = (int)(rem / 3600);
    tm.tm_min  = (int)((rem % 3600) / 60);
    tm.tm_sec  = (int)(rem % 60);
    return tm;
}

// UTC struct tm (as populated by std::get_time) -> epoch seconds
static double epochFromTm(const std::tm& tm) {
    long long days = daysFromCivil(tm.tm_year + 1900, (unsigned)(tm.tm_mon + 1), (unsigned)tm.tm_mday);
    return (double)(days * 86400 + tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec);
}


// the optional "offset" argument: seconds east of UTC, within a day and a bit either side (real zones are -12h..+14h)
static double offsetArg(const ArgSpan& args, size_t i, const char* fn) {
    if (args.size() <= i) return 0;
    double off = args[i].asNumber();
    if (!(off > -86400 && off < 86400)) throwError(fn, "offset must be between -86400 and 86400 seconds");
    return std::floor(off);
}

// "+0130" for 5400 seconds, "-0500" for -18000
static std::string formatOffset(long long off) {
    char buf[16];
    int a = (int)((off < 0 ? -off : off) % 86400);
    std::snprintf(buf, sizeof buf, "%c%02d%02d", off < 0 ? '-' : '+', a / 3600, (a % 3600) / 60);
    return buf;
}

// replaces %z in a strftime format with the offset text, leaving "%%z" alone
static std::string expandZ(const std::string& fmt, long long off) {
    std::string out;
    for (size_t i = 0; i < fmt.size(); i++) {
        if (fmt[i] == '%' && i + 1 < fmt.size()) {
            if (fmt[i + 1] == 'z') out += formatOffset(off);
            else                   out += fmt.substr(i, 2);
            i++;
        } else out += fmt[i];
    }
    return out;
}

// local utc offset at epoch time t, in seconds east of UTC. works it out as (local fields read as UTC) - t
static long long localOffset(double t) {
    if (!(t > -1e12 && t < 1e12)) throwError("time_utc_offset", "time out of the supported range (+-1e12 seconds)");
    std::time_t tt = (std::time_t)std::floor(t);
    std::tm local{};
#ifdef _WIN32
    if (localtime_s(&local, &tt) != 0) throwError("time_utc_offset", "cannot read the local time zone");
#else
    if (!localtime_r(&tt, &local)) throwError("time_utc_offset", "cannot read the local time zone");
#endif
    return (long long)(epochFromTm(local) - (double)tt);
}

// time_utc_offset(t: num = now) -> num, seconds east of UTC for the machine's local zone at time t
Value time_utc_offset(const ArgSpan& args) {
    double t = args.empty() ? std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count()
                            : args[0].asNumber();
    return Value((int64_t)localOffset(t));
}

// reads "+hhmm", "+hh:mm", "-hhmm" or "Z" from the stream, returns the offset in seconds. false if it isn't one
static bool readZ(std::istringstream& ss, long long& off) {
    ss >> std::ws;
    int c = ss.peek();
    if (c == 'Z' || c == 'z') { ss.get(); off = 0; return true; }
    if (c != '+' && c != '-') return false;
    ss.get();
    char d[4];
    for (int i = 0; i < 2; i++) { d[i] = (char)ss.get(); if (d[i] < '0' || d[i] > '9') return false; }
    if (ss.peek() == ':') ss.get();
    for (int i = 2; i < 4; i++) { d[i] = (char)ss.get(); if (d[i] < '0' || d[i] > '9') return false; }
    long long h = (d[0] - '0') * 10 + (d[1] - '0'), m = (d[2] - '0') * 10 + (d[3] - '0');
    if (h > 23 || m > 59) return false;
    off = (c == '-' ? -1 : 1) * (h * 3600 + m * 60);
    return true;
}

// time_now() -> num, seconds since the Unix epoch (UTC), with sub-second precision
double time_now() {
    auto now = std::chrono::system_clock::now();
    return std::chrono::duration<double>(now.time_since_epoch()).count();
}

// time_now_ms() -> num, whole milliseconds since the Unix epoch (as an exact int64)
int64_t time_now_ms() {
    auto now = std::chrono::system_clock::now();
    auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch());
    return (int64_t)ms.count();
}

// time_monotonic() -> num, seconds from an unspecified fixed starting point, never going backwards. unlike
// time_now() it isn't tied to the calendar and ignores system clock changes. for stopwatch-style timing:
// `t0 = time_monotonic(); ...; elapsed = time_monotonic() - t0`
double time_monotonic() {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(now.time_since_epoch()).count();
}

// time_sleep(seconds: num) -> 0
void time_sleep(double secs) {
    if (!(secs >= 0)) throwError("time_sleep", "seconds must not be negative or NaN");
    // duration<double> -> integer ticks overflows (UB) for enormous values; 1e9 s is ~31 years, far past
    // any sleep a script means
    if (secs > 1e9) throwError("time_sleep", "seconds too large (max 1e9)");
    std::this_thread::sleep_for(std::chrono::duration<double>(secs));
}

// time_format(epoch: num, fmt: str, offset: num = 0) -> str
// fmt is a strftime() format string (e.g. "%Y-%m-%d %H:%M:%S"). the time is shown in the zone that is
// `offset` seconds east of UTC (default UTC), and %z prints that offset
Value time_format(const ArgSpan& args) {
    double offset = offsetArg(args, 2, "time_format");
    std::tm tm = tmFromEpoch(args[0].asNumber() + offset);
    const std::string fmt = expandZ(args[1].asString(), (long long)offset);

    // strftime returns 0 both on truncation and on a legitimately empty result (fmt == ""); grow the
    // buffer geometrically until it fits, up to a cap, rather than silently returning "" for a long format
    std::string buf(256, '\0');
    size_t n = std::strftime(buf.data(), buf.size(), fmt.c_str(), &tm);
    while (n == 0 && !fmt.empty()) {
        if (buf.size() >= (size_t)4 << 20)
            throwError("time_format", "formatted output too large (over 4 MiB)");
        buf.assign(buf.size() * 4, '\0');
        n = std::strftime(buf.data(), buf.size(), fmt.c_str(), &tm);
    }
    buf.resize(n);
    return Value(std::move(buf));
}

// time_parse(str: str, fmt: str, offset: num = 0) -> num, seconds since the Unix epoch
// fmt is a std::get_time()-compatible format string (the same %Y/%m/%d/... specifiers as strftime). the text is
// read as a time in the zone `offset` seconds east of UTC, unless fmt has %z, which then wins. raises if str
// doesn't match fmt
Value time_parse(const ArgSpan& args) {
    const std::string& str = args[0].asString();
    const std::string& fmt = args[1].asString();
    long long off = (long long)offsetArg(args, 2, "time_parse");

    auto fail = [&]() { throwError("time_parse", "\"" + str + "\" does not match format \"" + fmt + "\""); };

    std::tm tm{};
    std::istringstream ss(str);
    // get_time can't be trusted with %z, so cut the format at each %z and read the offset by hand
    size_t pos = 0;
    while (true) {
        size_t z = pos;
        while ((z = fmt.find("%z", z)) != std::string::npos && z > 0 && fmt[z - 1] == '%') z += 2;
        std::string piece = fmt.substr(pos, z == std::string::npos ? std::string::npos : z - pos);
        if (!piece.empty()) {
            ss >> std::get_time(&tm, piece.c_str());
            if (ss.fail()) fail();
        }
        if (z == std::string::npos) break;
        if (!readZ(ss, off)) fail();
        pos = z + 2;
    }

    return Value(epochFromTm(tm) - (double)off);
}

EMBR_PLUGIN {
    interp->bindTyped("time_now", {},       time_now);
    interp->bindTyped("time_now_ms", {},    time_now_ms);
    interp->bindTyped("time_monotonic", {}, time_monotonic);

    interp->bindTyped("time_sleep",   {"seconds"}, time_sleep);
    interp->bindSpan("time_format",  {pNum("epoch"), pStr("fmt"), Param::opt("offset", TS::Num)}, time_format);
    interp->bindSpan("time_parse",   {pStr("str"), pStr("fmt"), Param::opt("offset", TS::Num)}, time_parse);
    interp->bindSpan("time_utc_offset", {Param::opt("t", TS::Num)}, time_utc_offset);

    // what each always returns (see Value::Callable::returns)
    interp->setResultTypes({
        {"time_now", TS::Float}, {"time_now_ms", TS::Int}, {"time_monotonic", TS::Float}, {"time_sleep", TS::Float},
        {"time_format", TS::Str}, {"time_parse", TS::Float}, {"time_utc_offset", TS::Int},
    });
}

// what this plugin says about itself. embr reads it before it runs embr_register (see core/plugininfo.h)
EMBR_PLUGIN_INFO("time", "1.0.0", "wall clock, stopwatch, sleep and UTC date formatting", EMBR_CAP_NONE, EMBR_CAP_NONE,
    {"time_format", "fmt is strftime-style. shows the time in the zone offset seconds east of UTC (default 0, must be within +-86400). %z prints that offset like +0130. no named zones, no %Z"},
    {"time_monotonic", "steady-clock seconds from an unspecified fixed point. for stopwatch use only, wall-clock changes don't affect it"},
    {"time_now", "seconds since Unix epoch (UTC), sub-second precision"},
    {"time_now_ms", "whole milliseconds since epoch (int64-exact)"},
    {"time_parse", "fmt is get_time-compatible. reads the text as being in the zone offset seconds east of UTC, unless fmt has %z (accepts +hhmm, +hh:mm, Z), which wins. raises (quoting the input and format) on input it can't parse"},
    {"time_sleep", "raises on negative"},
    {"time_utc_offset", "seconds east of UTC for the machine's local zone at time t (default now), so time_format(t, fmt, time_utc_offset(t)) prints local time"});
