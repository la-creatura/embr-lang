/* time.c
 * time and date plugin for embr: wall clock, monotonic elapsed time, sleep, and UTC date formatting and parsing.
 * written in C against embr_plugin.h
 *
 * usage
 *   import "time"
 *   t0 = time_monotonic()
 *   time_sleep(0.1)
 *   print(time_monotonic() - t0 >= 0.1)
 *   print(time_format(0, "%Y-%m-%d"))              # "1970-01-01"
 *   print(time_parse("2024-01-02", "%Y-%m-%d"))     # seconds since epoch
 *   print(time_format(0, "%H:%M %z", 3600))         # "01:00 +0100"
 *
 * good to know
 *   - calendar math is done in UTC with this file's own day count conversion (Howard Hinnant's days_from_civil and
 *     civil_from_days, public domain), not gmtime or timegm: those are not portable. the struct tm still goes through
 *     strftime, so time_format has the full strftime syntax
 *   - time_parse reads a format the way C++'s std::get_time does (the "C" locale): names are matched without regard to case,
 *     a number takes at most as many digits as its field has, a space in the format skips any white space, and what is left of the
 *     text after the format ends is ignored. parse_time below is a port of libstdc++'s time_get, so a format that worked with the
 *     C++ plugin gives the same answer
 *   - time zones are fixed offsets in seconds east of UTC, there is no named zone ("Europe/Berlin") and no DST table
 *   - %z is handled by this file in time_format and time_parse, %Z is not supported
 */
#include <embr_plugin.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <errno.h>
#endif

/* ---- the clocks ---- */

static double clock_wall(void) {
#if defined(_WIN32)
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;   /* 100 ns since 1601 */
    return (double)(t - 116444736000000000ULL) / 1e7;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
}

static double clock_steady(void) {
#if defined(_WIN32)
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
}

static int time_now(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    (void)args; (void)n;
    return embrp_set_float(ctx, out, clock_wall());
}

static int time_now_ms(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    (void)args; (void)n;
#if defined(_WIN32)
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return embrp_set_int(ctx, out, (int64_t)((t - 116444736000000000ULL) / 10000));
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return embrp_set_int(ctx, out, (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000));
#endif
}

static int time_monotonic(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    (void)args; (void)n;
    return embrp_set_float(ctx, out, clock_steady());
}

static int time_sleep(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    (void)n;
    double secs = embrp_to_float(ctx, &args[0]);
    if (!(secs >= 0)) return embrp_fail(ctx, "seconds must not be negative or NaN");
    if (secs > 1e9) return embrp_fail(ctx, "seconds too large (max 1e9)");   /* about 31 years, far past any sleep a script means */
#if defined(_WIN32)
    double ms = secs * 1000.0;
    while (ms > 0) { DWORD step = ms > 1e8 ? 100000000u : (DWORD)ceil(ms); Sleep(step); ms -= step; }
#else
    struct timespec req, rem;
    req.tv_sec = (time_t)secs;
    req.tv_nsec = (long)((secs - (double)req.tv_sec) * 1e9);
    while (nanosleep(&req, &rem) != 0 && errno == EINTR) req = rem;
#endif
    return embrp_set_float(ctx, out, 0.0);
}

/* ---- calendar math ---- */

/* days since 1970-01-01 for a proleptic Gregorian y/m/d (days_from_civil) */
static long long days_from_civil(long long y, unsigned m, unsigned d) {
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3u : 9u)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long long)doe - 719468;
}

/* the inverse (civil_from_days) */
static void civil_from_days(long long z, int* y, unsigned* m, unsigned* d) {
    z += 719468;
    long long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long yy = (long long)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp + (mp < 10 ? 3 : -9u);
    *y = (int)(yy + (*m <= 2 ? 1 : 0));
}

/* epoch seconds to a UTC struct tm. the caller has checked the range */
static void tm_from_epoch(double epoch, struct tm* tm) {
    long long secs = (long long)floor(epoch);
    long long days = secs >= 0 ? secs / 86400 : -((-secs + 86399) / 86400);
    long long rem = secs - days * 86400;
    int y; unsigned mo, d;
    civil_from_days(days, &y, &mo, &d);
    memset(tm, 0, sizeof *tm);
    tm->tm_year = y - 1900;
    tm->tm_mon = (int)mo - 1;
    tm->tm_mday = (int)d;
    tm->tm_hour = (int)(rem / 3600);
    tm->tm_min = (int)((rem % 3600) / 60);
    tm->tm_sec = (int)(rem % 60);
}

static double epoch_from_fields(long long year, int mon0, int mday, int hour, int min, int sec) {
    long long days = days_from_civil(year, (unsigned)(mon0 + 1), (unsigned)mday);
    return (double)(days * 86400 + hour * 3600 + min * 60 + sec);
}

/* the optional offset argument: seconds east of UTC, within a day and a bit either side (real zones are -12h to +14h) */
static int offset_arg(EmbrCtx* ctx, const EmbrValue* args, size_t n, size_t i, double* off) {
    *off = 0;
    if (n <= i) return 0;
    double v = embrp_to_float(ctx, &args[i]);
    if (!(v > -86400 && v < 86400)) return embrp_fail(ctx, "offset must be between -86400 and 86400 seconds");
    *off = floor(v);
    return 0;
}

/* "+0130" for 5400 seconds, "-0500" for -18000 */
static void format_offset(long long off, char* buf, size_t cap) {
    int a = (int)((off < 0 ? -off : off) % 86400);
    snprintf(buf, cap, "%c%02d%02d", off < 0 ? '-' : '+', a / 3600, (a % 3600) / 60);
}

/* ---- time_format ---- */

typedef struct { char* p; size_t len, cap; } Buf;

static int buf_add(Buf* b, const char* s, size_t n) {
    if (n == 0) return 0;
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 64;
        while (cap < b->len + n + 1) cap *= 2;
        char* np = realloc(b->p, cap);
        if (!np) return 1;
        b->p = np; b->cap = cap;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
    return 0;
}

/* the format with %z replaced by the offset text, and %%z left alone */
static int expand_z(Buf* out, const char* fmt, size_t n, long long off) {
    char z[16];
    format_offset(off, z, sizeof z);
    for (size_t i = 0; i < n; i++) {
        if (fmt[i] == '%' && i + 1 < n) {
            if (buf_add(out, fmt[i + 1] == 'z' ? z : fmt + i, fmt[i + 1] == 'z' ? strlen(z) : 2)) return 1;
            i++;
        } else if (buf_add(out, fmt + i, 1)) return 1;
    }
    if (out->len == 0 && buf_add(out, "", 0)) return 1;
    return 0;
}

static int time_format(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    double offset;
    if (offset_arg(ctx, args, n, 2, &offset)) return 1;
    double epoch = embrp_to_float(ctx, &args[0]) + offset;
    /* NaN, Infinity and huge values would be undefined in the integer conversion (and do not fit in tm_year either) */
    if (!(epoch > -1e15 && epoch < 1e15)) {
        EmbrValue* t = embrp_temp(ctx);
        size_t k = 0;
        const char* txt = "?";
        if (!embrp_set_float(ctx, t, epoch)) txt = embrp_to_string(ctx, t, &k);
        char msg[160];
        snprintf(msg, sizeof msg, "epoch out of the supported range (+-1e15 seconds), got %.40s", txt);
        return embrp_fail(ctx, msg);
    }
    struct tm tm;
    tm_from_epoch(epoch, &tm);
    size_t fl;
    const char* fmt = embrp_str(ctx, &args[1], &fl);
    Buf f = {0, 0, 0};
    if (expand_z(&f, fmt, fl, (long long)offset)) { free(f.p); return embrp_fail(ctx, "out of memory"); }
    const char* ff = f.p ? f.p : "";
    /* strftime returns 0 both on truncation and for a legitimately empty result (an empty format), so the buffer grows until
       it fits, up to a cap, instead of silently giving "" for a long format */
    size_t cap = 256;
    char* buf = malloc(cap);
    size_t got = 0;
    int rc = 0;
    if (!buf) rc = embrp_fail(ctx, "out of memory");
    while (!rc) {
        got = strftime(buf, cap, ff, &tm);
        if (got != 0 || f.len == 0) break;
        if (cap >= ((size_t)4 << 20)) { rc = embrp_fail(ctx, "formatted output too large (over 4 MiB)"); break; }
        cap *= 4;
        char* nb = realloc(buf, cap);
        if (!nb) { rc = embrp_fail(ctx, "out of memory"); break; }
        buf = nb;
    }
    if (!rc) rc = embrp_set_str(ctx, out, buf, got);
    free(buf); free(f.p);
    return rc;
}

/* ---- time_utc_offset ---- */

static int time_utc_offset(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    double t = n >= 1 ? embrp_to_float(ctx, &args[0]) : clock_wall();
    if (!(t > -1e12 && t < 1e12)) return embrp_fail(ctx, "time out of the supported range (+-1e12 seconds)");
    time_t tt = (time_t)floor(t);
    struct tm local;
    memset(&local, 0, sizeof local);
#if defined(_WIN32)
    if (localtime_s(&local, &tt) != 0) return embrp_fail(ctx, "cannot read the local time zone");
#else
    if (!localtime_r(&tt, &local)) return embrp_fail(ctx, "cannot read the local time zone");
#endif
    double as_utc = epoch_from_fields((long long)local.tm_year + 1900, local.tm_mon, local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec);
    return embrp_set_int(ctx, out, (int64_t)(long long)(as_utc - (double)tt));
}

/* ---- time_parse: a port of libstdc++'s time_get in the "C" locale (what std::get_time does) ---- */

typedef struct { int sec, min, hour, mday, mon, year, wday, yday; } Tm;

/* what a format has seen so far, for the fix-ups at the end */
typedef struct {
    unsigned have_I : 1, have_wday : 1, have_yday : 1, have_mon : 1, have_mday : 1, have_uweek : 1, have_wweek : 1;
    unsigned have_century : 1, is_pm : 1, want_century : 1, want_xday : 1;
    int week_no, century;
} State;

enum { ERR_FAIL = 1, ERR_EOF = 2 };

typedef struct { const char* s; size_t n, pos; } In;   /* the iterator: the next byte is s[pos], and the end is pos == n */

static int is_space(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'; }
static int lower_c(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int upper_c(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }

static const char* const k_days[14] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday",
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char* const k_months[24] = {
    "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December",
    "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
static const char* const k_ampm[2] = { "AM", "PM" };

/* a number of at most len digits, in min..max. *fail is set when there are none or it is out of range */
static void extract_num(In* in, int* member, int min, int max, size_t len, int* fail) {
    size_t i = 0;
    int value = 0;
    for (; in->pos < in->n && i < len; ++in->pos, ++i) {
        int c = (unsigned char)in->s[in->pos];
        if (c >= '0' && c <= '9') {
            value = value * 10 + (c - '0');
            if (value > max) break;
        } else break;
    }
    if (i && value >= min && value <= max) *member = value;
    else *fail = ERR_FAIL;
}

/* one of the names, matched without regard to case, taking the longest that fits (libstdc++'s _M_extract_name) */
static void extract_name(In* in, int* member, const char* const* names, size_t indexlen, int* fail) {
    size_t matches[24], lengths[24];
    size_t nmatches = 0, pos = 0;
    int testvalid = 1, begupdated = 0;
    if (in->pos < in->n) {
        int c = (unsigned char)in->s[in->pos];
        int cl = lower_c(c), cu = upper_c(c);
        for (size_t i1 = 0; i1 < indexlen; ++i1)
            if (cl == lower_c((unsigned char)names[i1][0]) || cu == upper_c((unsigned char)names[i1][0])) {
                lengths[nmatches] = strlen(names[i1]);
                matches[nmatches++] = i1;
            }
    }
    while (nmatches > 1) {
        size_t minlen = lengths[0];
        for (size_t i2 = 1; i2 < nmatches; ++i2) if (lengths[i2] < minlen) minlen = lengths[i2];
        ++pos;
        ++in->pos;
        if (pos == minlen) {
            int match_longer = 0;
            if (in->pos < in->n) {
                int cl = lower_c((unsigned char)in->s[in->pos]), cu = upper_c((unsigned char)in->s[in->pos]);
                for (size_t i3 = 0; i3 < nmatches; ++i3) {
                    const char* name = names[matches[i3]];
                    if (lengths[i3] > pos && (lower_c((unsigned char)name[pos]) == cl || upper_c((unsigned char)name[pos]) == cu)) {
                        match_longer = 1;
                        break;
                    }
                }
            }
            for (size_t i4 = 0; i4 < nmatches;) {
                if (match_longer == (lengths[i4] == pos)) {
                    matches[i4] = matches[--nmatches];
                    lengths[i4] = lengths[nmatches];
                } else ++i4;
            }
            if (match_longer) {
                minlen = lengths[0];
                for (size_t i5 = 1; i5 < nmatches; ++i5) if (lengths[i5] < minlen) minlen = lengths[i5];
            } else {
                /* May is both the full and the abbreviated name: take the smaller index */
                if (nmatches == 2 && (indexlen & 1) == 0) {
                    if (matches[0] < indexlen / 2) {
                        if (matches[1] == matches[0] + indexlen / 2) nmatches = 1;
                    } else if (matches[1] == matches[0] - indexlen / 2) {
                        matches[0] = matches[1];
                        lengths[0] = lengths[1];
                        nmatches = 1;
                    }
                }
                begupdated = 1;
                break;
            }
        }
        if (pos < minlen && in->pos < in->n) {
            int cl = lower_c((unsigned char)in->s[in->pos]), cu = upper_c((unsigned char)in->s[in->pos]);
            for (size_t i6 = 0; i6 < nmatches;) {
                const char* name = names[matches[i6]];
                if (lower_c((unsigned char)name[pos]) != cl && upper_c((unsigned char)name[pos]) != cu) {
                    matches[i6] = matches[--nmatches];
                    lengths[i6] = lengths[nmatches];
                } else ++i6;
            }
        } else break;
    }
    if (nmatches == 1) {
        if (!begupdated) { ++in->pos; ++pos; }
        const char* name = names[matches[0]];
        size_t len = lengths[0];
        while (pos < len && in->pos < in->n &&
               (lower_c((unsigned char)name[pos]) == lower_c((unsigned char)in->s[in->pos]) ||
                upper_c((unsigned char)name[pos]) == upper_c((unsigned char)in->s[in->pos])))
            ++in->pos, ++pos;
        if (len == pos) *member = (int)matches[0];
        else testvalid = 0;
    } else testvalid = 0;
    if (!testvalid) *fail = ERR_FAIL;
}

/* the format of one nested specifier in the "C" locale: %x is the date, %X the time, %c nothing, %r the 12-hour clock */
static const char* const k_fmt_date = "%m/%d/%y";
static const char* const k_fmt_time = "%H:%M:%S";
static const char* const k_fmt_datetime = "";
static const char* const k_fmt_ampm = "%I:%M:%S %p";

static void via_format(In* in, int* err, Tm* tm, const char* fmt, State* st);

static void spec_nested(In* in, int* fail, Tm* tm, const char* fmt, State* st) {
    int e = 0;
    via_format(in, &e, tm, fmt, st);
    if (e) *fail = ERR_FAIL;
}

/* one pass over a format (libstdc++'s _M_extract_via_format). stops without an error when the text ends first */
static void via_format(In* in, int* err, Tm* tm, const char* fmt, State* st) {
    size_t len = strlen(fmt);
    int tmperr = 0;
    size_t i = 0;
    for (; in->pos < in->n && i < len && !tmperr; ++i) {
        if (fmt[i] == '%') {
            int c = (unsigned char)fmt[++i];
            int mem = 0;
            if (c == 'E' || c == 'O') c = (unsigned char)fmt[++i];
            switch (c) {
                case 'a': case 'A':
                    extract_name(in, &mem, k_days, 14, &tmperr);
                    if (!tmperr) { tm->wday = mem % 7; st->have_wday = 1; }
                    break;
                case 'h': case 'b': case 'B':
                    extract_name(in, &mem, k_months, 24, &tmperr);
                    if (!tmperr) { tm->mon = mem % 12; st->have_mon = 1; st->want_xday = 1; }
                    break;
                case 'c':
                    spec_nested(in, &tmperr, tm, k_fmt_datetime, st);
                    if (!tmperr) st->want_xday = 1;
                    break;
                case 'C':
                    extract_num(in, &mem, 0, 99, 2, &tmperr);
                    if (!tmperr) { st->century = mem; st->have_century = 1; st->want_xday = 1; }
                    break;
                case 'd': case 'e':
                    if (is_space((unsigned char)in->s[in->pos])) ++in->pos;
                    extract_num(in, &mem, 1, 31, 2, &tmperr);
                    if (!tmperr) { tm->mday = mem; st->have_mday = 1; st->want_xday = 1; }
                    break;
                case 'D':
                    spec_nested(in, &tmperr, tm, "%m/%d/%y", st);
                    if (!tmperr) st->want_xday = 1;
                    break;
                case 'H':
                    extract_num(in, &mem, 0, 23, 2, &tmperr);
                    if (!tmperr) { tm->hour = mem; st->have_I = 0; }
                    break;
                case 'I':
                    extract_num(in, &mem, 1, 12, 2, &tmperr);
                    if (!tmperr) { tm->hour = mem % 12; st->have_I = 1; }
                    break;
                case 'j':
                    extract_num(in, &mem, 1, 366, 3, &tmperr);
                    if (!tmperr) { tm->yday = mem - 1; st->have_yday = 1; }
                    break;
                case 'm':
                    extract_num(in, &mem, 1, 12, 2, &tmperr);
                    if (!tmperr) { tm->mon = mem - 1; st->have_mon = 1; }
                    break;
                case 'M':
                    extract_num(in, &mem, 0, 59, 2, &tmperr);
                    if (!tmperr) tm->min = mem;
                    break;
                case 'n': case 't':
                    while (in->pos < in->n && is_space((unsigned char)in->s[in->pos])) ++in->pos;
                    break;
                case 'p':
                    extract_name(in, &mem, k_ampm, 2, &tmperr);
                    if (!tmperr && mem) st->is_pm = 1;
                    break;
                case 'r': spec_nested(in, &tmperr, tm, k_fmt_ampm, st); break;
                case 'R': spec_nested(in, &tmperr, tm, "%H:%M", st); break;
                case 'S':
                    extract_num(in, &mem, 0, 60, 2, &tmperr);
                    if (!tmperr) tm->sec = mem;
                    break;
                case 'T': spec_nested(in, &tmperr, tm, "%H:%M:%S", st); break;
                case 'U':
                    extract_num(in, &mem, 0, 53, 2, &tmperr);
                    if (!tmperr) { st->week_no = mem; st->have_uweek = 1; }
                    break;
                case 'w':
                    extract_num(in, &mem, 0, 6, 1, &tmperr);
                    if (!tmperr) { tm->wday = mem; st->have_wday = 1; }
                    break;
                case 'W':
                    extract_num(in, &mem, 0, 53, 2, &tmperr);
                    if (!tmperr) { st->week_no = mem; st->have_wweek = 1; }
                    break;
                case 'x': spec_nested(in, &tmperr, tm, k_fmt_date, st); break;
                case 'X': spec_nested(in, &tmperr, tm, k_fmt_time, st); break;
                case 'y':
                    extract_num(in, &mem, 0, 99, 2, &tmperr);
                    if (!tmperr) {
                        st->want_century = 1;
                        st->want_xday = 1;
                        /* as an extension, two digits followed by one or two more are read like %Y */
                        c = 0;
                        if (in->pos < in->n) c = (unsigned char)in->s[in->pos];
                        if (c >= '0' && c <= '9') {
                            ++in->pos;
                            mem = mem * 10 + (c - '0');
                            if (in->pos < in->n) {
                                c = (unsigned char)in->s[in->pos];
                                if (c >= '0' && c <= '9') { ++in->pos; mem = mem * 10 + (c - '0'); }
                            }
                            mem -= 1900;
                            st->want_century = 0;
                        } else if (mem < 69) mem += 100;   /* 00 to 68 is 2000 to 2068, 69 to 99 is 1969 to 1999 */
                        tm->year = mem;
                    }
                    break;
                case 'Y':
                    extract_num(in, &mem, 0, 9999, 4, &tmperr);
                    if (!tmperr) { tm->year = mem - 1900; st->want_century = 0; st->want_xday = 1; }
                    break;
                case 'Z':
                    /* a time zone name: only the upper case start is looked at, the name itself is not used */
                    if (in->s[in->pos] >= 'A' && in->s[in->pos] <= 'Z') {
                        static const char* const zones[14] = {
                            "GMT", "HST", "AKST", "PST", "MST", "CST", "EST", "AST", "NST", "CET", "IST", "EET", "CST", "JST" };
                        int tmp = 0;
                        extract_name(in, &tmp, zones, 14, &tmperr);
                        if (in->pos < in->n && !tmperr && tmp == 0 && (in->s[in->pos] == '-' || in->s[in->pos] == '+')) {
                            extract_num(in, &tmp, 0, 23, 2, &tmperr);
                            extract_num(in, &tmp, 0, 59, 2, &tmperr);
                        }
                    } else tmperr |= ERR_FAIL;
                    break;
                case '%':
                    if (in->s[in->pos] == '%') ++in->pos;
                    else tmperr |= ERR_FAIL;
                    break;
                default:
                    tmperr |= ERR_FAIL;   /* not recognized */
            }
        } else if (is_space((unsigned char)fmt[i])) {
            while (in->pos < in->n && is_space((unsigned char)in->s[in->pos])) ++in->pos;
        } else {
            if (lower_c((unsigned char)fmt[i]) == lower_c((unsigned char)in->s[in->pos]) ||
                upper_c((unsigned char)fmt[i]) == upper_c((unsigned char)in->s[in->pos]))
                ++in->pos;
            else tmperr |= ERR_FAIL;
        }
    }
    if (tmperr || i != len) *err |= ERR_FAIL;
}

static int leap_year(int tm_year) { int y = tm_year + 1900; return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
/* the first day of each month as a day of the year, for a common year and then a leap year, and a stop at the end. a day number
   past the end of the year runs on into the other row (as the C++ library's table does), which gives a month past 12 */
static const int k_mon_yday[2 * 13 + 1] = {
    0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365,
    0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335, 366, 1000000 };

/* the month (0-based, may be past 11) and day of the month for a day of the year (0 or more) */
static void mon_of_yday(int lp, int yday, int* mon, int* mday) {
    const int* t = k_mon_yday + lp * 13;
    int t_mon = 0;
    while (t[t_mon] <= yday) t_mon++;
    *mon = t_mon - 1;
    *mday = yday - t[t_mon - 1] + 1;
}

/* day of the week, 0 = Sunday, of a date given as a tm_year and a day of the year (0-based) */
static int wday_of(int tm_year, int yday) {
    long long y = (long long)tm_year + 1900;
    long long days = days_from_civil(y, 1, 1) + yday;
    long long w = (days + 4) % 7;   /* 1970-01-01 was a Thursday */
    return (int)(w < 0 ? w + 7 : w);
}

/* the fix-ups after a whole format (libstdc++'s __time_get_state::_M_finalize_state) */
static void finalize_state(State* st, Tm* tm) {
    if (st->have_I && st->is_pm) tm->hour += 12;
    if (st->have_century) {
        if (st->want_century) tm->year = tm->year % 100;
        else tm->year = 0;
        tm->year += (st->century - 19) * 100;
    }
    int wday_given = st->have_wday;
    if (st->want_xday && !st->have_wday) {
        if (!(st->have_mon && st->have_mday) && st->have_yday) {
            /* tm_mon and tm_mday are not both known: work them out from the day of the year */
            int mon, mday;
            mon_of_yday(leap_year(tm->year), tm->yday, &mon, &mday);
            if (!st->have_mon) tm->mon = mon;
            if (!st->have_mday) tm->mday = mday;
            st->have_mon = 1;
            st->have_mday = 1;
        }
        if (st->have_mon || st->have_wday) {
            /* the weekday is not used for the result, but the week number below needs the day of the year */
            if (st->have_mon && st->have_mday && !st->have_yday) {
                tm->yday = k_mon_yday[leap_year(tm->year) * 13 + (tm->mon < 0 ? 0 : tm->mon > 12 ? 12 : tm->mon)] + tm->mday - 1;
            }
            tm->wday = wday_of(tm->year, tm->yday);
        }
        st->have_wday = 1;
    }
    if ((st->have_uweek || st->have_wweek) && wday_given) {
        /* a week number and a weekday: the date is the first such week day of the year plus the weeks */
        int jan1 = wday_of(tm->year, 0);
        int first, wd = tm->wday;
        if (st->have_uweek) first = (7 - jan1) % 7;
        else { first = (8 - jan1) % 7; wd = (wd + 6) % 7; }
        int yday = first + (st->week_no - 1) * 7 + wd;
        if (yday >= 0) {   /* week 0 can fall before the year: left alone */
            int mon, mday;
            mon_of_yday(leap_year(tm->year), yday, &mon, &mday);
            if (!st->have_mon) tm->mon = mon;
            if (!st->have_mday) tm->mday = mday;
            tm->yday = yday;
        }
    }
}

/* std::istream's state for the pieces of one time_parse: a failure or the end of the text stops the next piece */
typedef struct { In in; int fail, eof; } Stream;

static int stream_good(const Stream* ss) { return !ss->fail && !ss->eof; }

/* the sentry of a formatted read: it skips white space, and ends at the end of the text */
static int sentry_skipws(Stream* ss) {
    if (!stream_good(ss)) { ss->fail = 1; return 0; }
    while (ss->in.pos < ss->in.n && is_space((unsigned char)ss->in.s[ss->in.pos])) ++ss->in.pos;
    if (ss->in.pos >= ss->in.n) { ss->eof = 1; ss->fail = 1; return 0; }
    return 1;
}

/* ss >> std::get_time(&tm, fmt) for a format of len bytes */
static void stream_get_time(Stream* ss, Tm* tm, const char* fmt, size_t len) {
    if (!sentry_skipws(ss)) return;
    In* in = &ss->in;
    int err = 0;
    State st;
    memset(&st, 0, sizeof st);
    size_t f = 0;
    while (f < len && err == 0) {
        if (in->pos >= in->n) { err = ERR_EOF | ERR_FAIL; break; }
        if (fmt[f] == '%') {
            size_t start = f;
            if (++f == len) { err = ERR_FAIL; break; }
            char c = fmt[f];
            if (c == 'E' || c == 'O') {
                if (++f == len) { err = ERR_FAIL; break; }
            }
            char one[4];
            size_t k = f - start + 1;
            memcpy(one, fmt + start, k);
            one[k] = 0;
            via_format(in, &err, tm, one, &st);
            if (in->pos >= in->n) err |= ERR_EOF;
            ++f;
        } else if (is_space((unsigned char)fmt[f])) {
            ++f;
            while (f < len && is_space((unsigned char)fmt[f])) ++f;
            while (in->pos < in->n && is_space((unsigned char)in->s[in->pos])) ++in->pos;
        } else if (lower_c((unsigned char)in->s[in->pos]) == lower_c((unsigned char)fmt[f]) ||
                   upper_c((unsigned char)in->s[in->pos]) == upper_c((unsigned char)fmt[f])) {
            ++in->pos;
            ++f;
        } else {
            err = ERR_FAIL;
            break;
        }
    }
    finalize_state(&st, tm);
    if (err & ERR_FAIL) ss->fail = 1;
    if (err & ERR_EOF) ss->eof = 1;
}

/* "+hhmm", "+hh:mm", "-hhmm" or "Z" from the text, as seconds east of UTC. 0 when it is not one */
static int read_z(Stream* ss, long long* off) {
    /* std::ws */
    if (!stream_good(ss)) { ss->fail = 1; }
    else {
        while (ss->in.pos < ss->in.n && is_space((unsigned char)ss->in.s[ss->in.pos])) ++ss->in.pos;
        if (ss->in.pos >= ss->in.n) ss->eof = 1;
    }
    int c;   /* peek */
    if (!stream_good(ss)) { ss->fail = 1; c = -1; }
    else if (ss->in.pos >= ss->in.n) { ss->eof = 1; c = -1; }
    else c = (unsigned char)ss->in.s[ss->in.pos];
    if (c == 'Z' || c == 'z') { ++ss->in.pos; *off = 0; return 1; }
    if (c != '+' && c != '-') return 0;
    ++ss->in.pos;
    char d[4];
    for (int i = 0; i < 2; i++) {
        if (ss->in.pos >= ss->in.n) return 0;
        d[i] = ss->in.s[ss->in.pos++];
        if (d[i] < '0' || d[i] > '9') return 0;
    }
    if (ss->in.pos >= ss->in.n) ss->eof = 1;
    else if (ss->in.s[ss->in.pos] == ':') ++ss->in.pos;
    for (int i = 2; i < 4; i++) {
        if (ss->in.pos >= ss->in.n) return 0;
        d[i] = ss->in.s[ss->in.pos++];
        if (d[i] < '0' || d[i] > '9') return 0;
    }
    long long h = (d[0] - '0') * 10 + (d[1] - '0'), m = (d[2] - '0') * 10 + (d[3] - '0');
    if (h > 23 || m > 59) return 0;
    *off = (c == '-' ? -1 : 1) * (h * 3600 + m * 60);
    return 1;
}

static int time_parse(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) {
    size_t sl, fl;
    const char* str = embrp_str(ctx, &args[0], &sl);
    const char* fmt = embrp_str(ctx, &args[1], &fl);
    double offd;
    if (offset_arg(ctx, args, n, 2, &offd)) return 1;
    long long off = (long long)offd;

    Tm tm;
    memset(&tm, 0, sizeof tm);
    Stream ss;
    memset(&ss, 0, sizeof ss);
    ss.in.s = str; ss.in.n = sl;
    /* the library cannot be trusted with %z, so the format is cut at each %z and the offset is read by hand */
    size_t pos = 0;
    int bad = 0;
    for (;;) {
        size_t z = pos;
        /* find "%z" from pos, skipping "%%z" */
        z = pos;
        for (;;) {
            size_t k = z;
            while (k + 1 < fl && !(fmt[k] == '%' && fmt[k + 1] == 'z')) ++k;
            if (!(k + 1 < fl)) { z = (size_t)-1; break; }
            z = k;
            if (z > 0 && fmt[z - 1] == '%') { z += 2; continue; }
            break;
        }
        size_t plen = (z == (size_t)-1 ? fl : z) - pos;
        if (plen) {
            stream_get_time(&ss, &tm, fmt + pos, plen);
            if (ss.fail) { bad = 1; break; }
        }
        if (z == (size_t)-1) break;
        if (!read_z(&ss, &off)) { bad = 1; break; }
        pos = z + 2;
    }
    if (bad) {
        size_t cap = sl + fl + 64;
        char* msg = malloc(cap);
        if (!msg) return embrp_fail(ctx, "out of memory");
        snprintf(msg, cap, "\"%.*s\" does not match format \"%.*s\"", (int)sl, str, (int)fl, fmt);
        int rc = embrp_fail(ctx, msg);
        free(msg);
        return rc;
    }
    double e = epoch_from_fields((long long)tm.year + 1900, tm.mon, tm.mday, tm.hour, tm.min, tm.sec);
    return embrp_set_float(ctx, out, e - (double)off);
}

/* ---- the table ---- */

static const EmbrParam p_seconds[] = { { "seconds", EMBR_T_NUM, 0 } };
static const EmbrParam p_format[]  = { { "epoch", EMBR_T_NUM, 0 }, { "fmt", EMBR_T_STRING, 0 }, { "offset", EMBR_T_NUM, EMBR_PARAM_OPTIONAL } };
static const EmbrParam p_parse[]   = { { "str", EMBR_T_STRING, 0 }, { "fmt", EMBR_T_STRING, 0 }, { "offset", EMBR_T_NUM, EMBR_PARAM_OPTIONAL } };
static const EmbrParam p_offset[]  = { { "t", EMBR_T_NUM, EMBR_PARAM_OPTIONAL } };

static const EmbrFunc funcs[] = {
    { "time_format", "time_format(epoch: num, fmt: str, offset?: num) -> str. fmt is strftime-style. shows the time in the zone offset seconds east of UTC (default 0, must be within +-86400). %z prints that offset like +0130. no named zones, no %Z",
      time_format, NULL, p_format, 3, EMBR_T_STRING, 0, 0 },
    { "time_monotonic", "time_monotonic() -> num. steady-clock seconds from an unspecified fixed point. for stopwatch use only, wall-clock changes don't affect it",
      time_monotonic, NULL, NULL, 0, EMBR_T_FLOAT, 0, 0 },
    { "time_now", "time_now() -> num. seconds since the Unix epoch (UTC), sub-second precision",
      time_now, NULL, NULL, 0, EMBR_T_FLOAT, 0, 0 },
    { "time_now_ms", "time_now_ms() -> int. whole milliseconds since the epoch (int64-exact)",
      time_now_ms, NULL, NULL, 0, EMBR_T_INT, 0, 0 },
    { "time_parse", "time_parse(str: str, fmt: str, offset?: num) -> num. fmt is get_time-compatible. reads the text as being in the zone offset seconds east of UTC, unless fmt has %z (accepts +hhmm, +hh:mm, Z), which wins. raises (quoting the input and format) on input it can't parse",
      time_parse, NULL, p_parse, 3, EMBR_T_FLOAT, 0, 0 },
    { "time_sleep", "time_sleep(seconds: num) -> num. raises on negative",
      time_sleep, NULL, p_seconds, 1, EMBR_T_FLOAT, 0, 0 },
    { "time_utc_offset", "time_utc_offset(t?: num) -> int. seconds east of UTC for the machine's local zone at time t (default now), so time_format(t, fmt, time_utc_offset(t)) prints local time",
      time_utc_offset, NULL, p_offset, 1, EMBR_T_INT, 0, 0 },
};

static const EmbrPluginInfo info = {
    EMBR_PLUGIN_ABI_VERSION, sizeof(EmbrPluginInfo), "time", "1.0.0", "wall clock, stopwatch, sleep and UTC date formatting",
    EMBR_CAP_NONE, funcs, sizeof funcs / sizeof funcs[0], NULL, 0, NULL, NULL
};

EMBR_PLUGIN_EXPORT const EmbrPluginInfo* embr_plugin_info(void) { return &info; }
