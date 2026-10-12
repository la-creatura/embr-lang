// str.cpp
// core string plugin for embr: split/replace/strip/case/find/pad/etc.
//
// usage
//   import "str"
//   print(str_upper("hi"))                                    # HI
//   print(str_split("a,b,c", ","))                            # ["a", "b", "c"]
//   print(str_format("{} scored {:.1f}%", "ada", 97.456))     # ada scored 97.5%
//
// encoding: byte-oriented by design, not Unicode-aware. embr strings are plain bytes, and this plugin works on them
// directly: str_ord/str_chr use one byte (0-255), str_isalpha/isdigit/isalnum use the C locale's ::isalpha family
// (ASCII only unless the process locale says otherwise), and str_split/str_reverse/etc. cut by byte offset, which
// can split a multi-byte UTF-8 character in half. that keeps str_* fast and dependency-free, and is what most
// callers want (ASCII identifiers, delimiters, protocol bytes). for real codepoint semantics, `import "unicode"`

#include <embr/embr.h>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace embr;

static Param pStr(std::string n)                      { return Param::req(std::move(n), TS::Str); }
static Param pNum(std::string n)                      { return Param::req(std::move(n), TS::Num); }
static Param pOpt(std::string n, TypeSet m = TS::Any) { return Param::opt(std::move(n), m);       }

[[noreturn]] static void throwError(const std::string& fn, const std::string& msg) {
    raiseError("[str:" + fn + "]", msg);
}

Value str_split(const std::string& str, const std::string& sep) {
    std::vector<Value> result;

    if (sep.empty()) {
        for (char c : str) result.emplace_back(std::string(1, c));
    } else {
        size_t start = 0, end;
        while ((end = str.find(sep, start)) != std::string::npos) {
            result.emplace_back(str.substr(start, end - start));
            start = end + sep.size();
        }
        result.emplace_back(str.substr(start));
    }
    return Value(result);
}

std::string str_replace(const std::string& in, const std::string& oldStr, const std::string& newStr) {
    if (oldStr.empty()) throwError("str_replace", "'old' cannot be empty");

    // builds the result by appending, left to right. replacing inside one string shifts the rest of it on every match,
    // which is quadratic on a long string
    std::string out;
    out.reserve(in.size());
    size_t pos = 0, at;
    while ((at = in.find(oldStr, pos)) != std::string::npos) {
        out.append(in, pos, at - pos);
        out += newStr;
        pos = at + oldStr.size();
    }
    out.append(in, pos, std::string::npos);
    return out;
}

std::string str_strip(const std::string& s) {
    const char* chars = " \t\n\r\f\v";
    size_t start = s.find_first_not_of(chars);
    if (start == std::string::npos) return std::string();
    size_t end = s.find_last_not_of(chars);
    return s.substr(start, end - start + 1);
}

std::string str_lower(const std::string& in) {
    std::string s = in;
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

std::string str_upper(const std::string& in) {
    std::string s = in;
    std::transform(s.begin(), s.end(), s.begin(), ::toupper);
    return s;
}

Value str_find(const ArgSpan& args) {
    size_t startPos = 0;
    if (args.size() >= 3) {
        int64_t p = numToInt64(args[2], "str_find", "start");
        startPos = p < 0 ? 0 : (size_t)p;
    }
    size_t pos = args[0].asString().find(args[1].asString(), startPos);
    return Value(pos == std::string::npos ? (int64_t)-1 : (int64_t)pos);
}

bool str_startswith(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

bool str_endswith(const std::string& s, const std::string& f) {
    return s.size() >= f.size() && s.compare(s.size() - f.size(), f.size(), f) == 0;
}

bool str_isdigit(const std::string& s) { return !s.empty() && std::all_of(s.begin(), s.end(), ::isdigit); }
bool str_isalpha(const std::string& s) { return !s.empty() && std::all_of(s.begin(), s.end(), ::isalpha); }
bool str_isalnum(const std::string& s) { return !s.empty() && std::all_of(s.begin(), s.end(), ::isalnum); }

int64_t str_count(const std::string& s, const std::string& sub) {
    if (sub.empty()) return 0;
    size_t count = 0, pos = 0;
    while ((pos = s.find(sub, pos)) != std::string::npos) { ++count; pos += sub.size(); }
    return (int64_t)count;
}

int64_t str_ord(const std::string& s) {
    if (s.empty()) throwError("ord", "empty string");
    return (int64_t)(unsigned char)s[0];
}

Value str_chr(const ArgSpan& args) {
    if (!args[0].isNumeric()) throwError("chr", "argument must be a number");
    int64_t code = numToInt64(args[0], "chr", "argument");
    if (code < 0 || code > 255) throwError("chr", "code point out of range (0-255)");
    return Value(std::string(1, (char)code));
}

Value str_repeat(const ArgSpan& args) {
    const std::string& s = args[0].asString();
    int64_t n = numToInt64(args[1], "str_repeat", "count");
    if (n < 0) throwError("str_repeat", "count must not be negative");
    if (!s.empty() && n > kMaxScriptAlloc / (int64_t)s.size())
        throwError("str_repeat", "result would exceed the limit of " + std::to_string(kMaxScriptAlloc) + " bytes");
    if (s.empty()) return Value(std::string());   // nothing to repeat, and n can be huge
    std::string out;
    out.reserve(s.size() * (size_t)n);
    for (int64_t i = 0; i < n; ++i) out += s;
    return Value(std::move(out));
}

std::string str_reverse(const std::string& in) {
    std::string s = in;
    std::reverse(s.begin(), s.end());
    return s;
}

Value str_pad_left(const ArgSpan& args) {
    std::string s = args[0].asString();
    int64_t width = numToInt64(args[1], "str_pad_left", "width");
    std::string ch = args.size() >= 3 ? args[2].asString() : " ";
    if (ch.size() != 1) throwError("str_pad_left", "pad char must be a single character");
    if ((int64_t)s.size() >= width) return Value(std::move(s));
    checkAllocSize("str_pad_left", "width", width);
    return Value(std::string((size_t)width - s.size(), ch[0]) + s);
}

Value str_pad_right(const ArgSpan& args) {
    std::string s = args[0].asString();
    int64_t width = numToInt64(args[1], "str_pad_right", "width");
    std::string ch = args.size() >= 3 ? args[2].asString() : " ";
    if (ch.size() != 1) throwError("str_pad_right", "pad char must be a single character");
    if ((int64_t)s.size() >= width) return Value(std::move(s));
    checkAllocSize("str_pad_right", "width", width);
    return Value(s + std::string((size_t)width - s.size(), ch[0]));
}

std::string str_ltrim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\n\r\f\v");
    if (start == std::string::npos) return std::string();
    return s.substr(start);
}

std::string str_rtrim(const std::string& s) {
    size_t end = s.find_last_not_of(" \t\n\r\f\v");
    if (end == std::string::npos) return std::string();
    return s.substr(0, end + 1);
}

// ---- str_format ---------------------------------------------------------------------------------
// str_format(template: str, ...args) -> str
//
//   str_format("{} scored {:.1f}%", "ada", 97.456)     -> "ada scored 97.5%"
//   str_format("[{:>6}] [{:<6}] [{:^6}]", "a", "b", "c") -> "[     a] [b     ] [  c   ]"
//   str_format("{:08.3f} {:+d} {:#x}", 3.14159, 5, 255) -> "0003.142 +5 0xff"
//   str_format("{1}-{0} {{literal}}", "a", "b")          -> "b-a {literal}"
//
// placeholder: `{}` (next argument), `{N}` (argument N, 0-based), optionally followed by `:SPEC`
// SPEC = [[fill]align][sign][#][0][width][.precision][type]
//   align     < left, > right, ^ centre (default: numbers right, everything else left); fill is any one
//             byte before the align character (default space)
//   sign      + always show a sign, - only for negatives (default), ' ' a space for non-negatives
//   #         0x / 0o / 0b prefix for x X o b
//   0         pad numbers with zeros after the sign (same as fill 0 with align =... but simpler)
//   width     minimum width in bytes (a multi-byte UTF-8 character counts as several)
//   precision digits after the point for f/e/g; maximum bytes for strings
//   type      d x X o b: an integer (an integral float is accepted); f e g: a float; s: any value as text;
//             omitted: natural form (numbers as print() shows them, strings as-is, arrays/maps as print())
// `{{` and `}}` are literal braces. numbering is either all automatic or all explicit, never mixed.
// fewer arguments than placeholders raises; extra arguments are ignored. decimal points are always '.'.

namespace fmt {

struct Spec {
    char fill = ' '; char align = 0; char sign = '-'; bool alt = false; bool zero = false;
    int  width = 0;  int  prec = -1; char type = 0;
};

[[noreturn]] void bad(const std::string& why) { throwError("str_format", why); }

Spec parseSpec(const std::string& sp) {
    Spec r;
    size_t i = 0, n = sp.size();
    auto isAlign = [](char c) { return c == '<' || c == '>' || c == '^'; };
    if (n >= 2 && isAlign(sp[1])) { r.fill = sp[0]; r.align = sp[1]; i = 2; }
    else if (n >= 1 && isAlign(sp[0])) { r.align = sp[0]; i = 1; }
    if (i < n && (sp[i] == '+' || sp[i] == '-' || sp[i] == ' ')) r.sign = sp[i++];
    if (i < n && sp[i] == '#') { r.alt = true; ++i; }
    if (i < n && sp[i] == '0') { r.zero = true; ++i; }
    auto number = [&](int& out) {
        if (i >= n || sp[i] < '0' || sp[i] > '9') return false;
        long v = 0;
        while (i < n && sp[i] >= '0' && sp[i] <= '9') { v = v * 10 + (sp[i++] - '0'); if (v > 100000) bad("width/precision too large (max 100000)"); }
        out = (int)v; return true;
    };
    number(r.width);
    if (i < n && sp[i] == '.') { ++i; if (!number(r.prec)) bad("missing precision after '.'"); }
    if (i < n) r.type = sp[i++];
    if (i < n) bad("invalid format spec \"" + sp + "\"");
    if (r.type && !std::strchr("dxXobfegs", r.type)) bad(std::string("unknown format type '") + r.type + "'");
    return r;
}

std::string pad(std::string body, const Spec& sp, char defaultAlign) {
    if ((int)body.size() >= sp.width) return body;
    size_t fillN = (size_t)sp.width - body.size();
    char al = sp.align ? sp.align : defaultAlign;
    if (al == '<') return body + std::string(fillN, sp.fill);
    if (al == '>') return std::string(fillN, sp.fill) + body;
    size_t left = fillN / 2;                                  // centre: the odd extra byte goes right
    return std::string(left, sp.fill) + body + std::string(fillN - left, sp.fill);
}

// sign + (prefix) + digits, with zero padding placed after the sign/prefix
std::string finishNumber(bool neg, std::string prefix, std::string digits, const Spec& sp) {
    std::string signStr = neg ? "-" : (sp.sign == '+' ? "+" : (sp.sign == ' ' ? " " : ""));
    if (sp.zero && !sp.align && (int)(signStr.size() + prefix.size() + digits.size()) < sp.width)
        digits = std::string((size_t)sp.width - signStr.size() - prefix.size() - digits.size(), '0') + digits;
    return pad(signStr + prefix + digits, sp, '>');
}

std::string fmtInt(const Value& v, const Spec& sp) {
    int64_t x;
    if (v.isInt()) x = v.asInt();
    else if (v.isNumber() && std::floor(v.asNumber()) == v.asNumber() && std::fabs(v.asNumber()) < 9.2e18) x = (int64_t)v.asNumber();
    else bad(std::string("'") + sp.type + "' needs an integer, got " + v.formatAsString());
    bool neg = x < 0;
    uint64_t m = neg ? (uint64_t)0 - (uint64_t)x : (uint64_t)x;
    int base = sp.type == 'x' || sp.type == 'X' ? 16 : sp.type == 'o' ? 8 : sp.type == 'b' ? 2 : 10;
    std::string digits;
    const char* dg = sp.type == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
    do { digits += dg[m % (uint64_t)base]; m /= (uint64_t)base; } while (m);
    std::reverse(digits.begin(), digits.end());
    std::string prefix;
    if (sp.alt) prefix = base == 16 ? (sp.type == 'X' ? "0X" : "0x") : base == 8 ? "0o" : base == 2 ? "0b" : "";
    return finishNumber(neg, prefix, digits, sp);
}

std::string fmtFloat(const Value& v, const Spec& sp) {
    if (!v.isNumeric()) bad(std::string("'") + sp.type + "' needs a number, got " + v.typeName());
    double d = v.asNumber();
    if (std::isnan(d) || std::isinf(d)) return pad(std::isnan(d) ? "nan" : (d < 0 ? "-inf" : "inf"), sp, '>');
    bool neg = std::signbit(d);
    char form[8];
    std::snprintf(form, sizeof form, "%%.*%c", sp.type);
    int prec = sp.prec >= 0 ? sp.prec : 6;
    std::string buf(512 + (size_t)prec, '\0');
    int n = std::snprintf(buf.data(), buf.size(), form, prec, std::fabs(d));
    if (n < 0) bad("formatting failed");
    if ((size_t)n >= buf.size()) { buf.assign((size_t)n + 1, '\0'); std::snprintf(buf.data(), buf.size(), form, prec, std::fabs(d)); }
    buf.resize((size_t)n);
    return finishNumber(neg, "", buf, sp);
}

std::string formatOne(const Value& v, const Spec& sp) {
    switch (sp.type) {
        case 'd': case 'x': case 'X': case 'o': case 'b': return fmtInt(v, sp);
        case 'f': case 'e': case 'g': return fmtFloat(v, sp);
        default: break;
    }
    if (sp.type == 0 && v.isNumeric()) {                      // natural number form, honouring sign/width
        if (sp.prec >= 0) { Spec g = sp; g.type = 'g'; return fmtFloat(v, g); }
        std::string txt = v.formatAsString();
        bool neg = !txt.empty() && txt[0] == '-';
        return finishNumber(neg, "", neg ? txt.substr(1) : txt, sp);
    }
    std::string txt = v.isString() ? v.asString() : v.formatAsString();
    if (sp.prec >= 0 && (int)txt.size() > sp.prec) txt.resize((size_t)sp.prec);
    return pad(txt, sp, '<');
}

} // namespace fmt

Value str_format(const ArgSpan& args) {
    const std::string& t = args[0].asString();
    size_t nargs = args.size() - 1, nextAuto = 0;
    int mode = 0;                                             // 0 undecided, 1 automatic, 2 explicit
    std::string out;
    for (size_t i = 0; i < t.size(); ++i) {
        char c = t[i];
        if (c == '}') {
            if (i + 1 < t.size() && t[i + 1] == '}') { out += '}'; ++i; continue; }
            fmt::bad("single '}' in template (write '}}' for a literal brace)");
        }
        if (c != '{') { out += c; continue; }
        if (i + 1 < t.size() && t[i + 1] == '{') { out += '{'; ++i; continue; }
        size_t close = t.find('}', i + 1);
        if (close == std::string::npos) fmt::bad("unterminated '{' in template");
        std::string field = t.substr(i + 1, close - i - 1), specText;
        size_t colon = field.find(':');
        if (colon != std::string::npos) { specText = field.substr(colon + 1); field.resize(colon); }
        size_t idx;
        if (field.empty()) {
            if (mode == 2) fmt::bad("cannot mix automatic {} and numbered {N} placeholders");
            mode = 1; idx = nextAuto++;
        } else {
            for (char d : field) if (d < '0' || d > '9') fmt::bad("invalid placeholder '{" + field + "}'");
            if (mode == 1) fmt::bad("cannot mix automatic {} and numbered {N} placeholders");
            mode = 2;
            if (field.size() > 6) fmt::bad("placeholder index too large");
            idx = (size_t)std::stoul(field);
        }
        if (idx >= nargs)
            fmt::bad("template needs argument " + std::to_string(idx) + " but only " + std::to_string(nargs) + " were given");
        out += fmt::formatOne(args[idx + 1], fmt::parseSpec(specText));
        if (out.size() > (size_t)kMaxScriptAlloc) fmt::bad("result too large");
        i = close;
    }
    return Value(std::move(out));
}

// num_fixed(x: num, digits: num) -> str    (num_fixed(3.14159, 2) -> "3.14"; digits 0..100)
Value num_fixed(const ArgSpan& args) {
    int64_t d = numToInt64(args[1], "num_fixed", "digits");
    if (d < 0 || d > 100) throwError("num_fixed", "digits must be between 0 and 100");
    fmt::Spec sp; sp.type = 'f'; sp.prec = (int)d;
    return Value(fmt::fmtFloat(args[0], sp));
}

EMBR_PLUGIN {
    interp->bindTyped("str_split",      {"string", "separator"}, str_split);
    interp->bindTyped("str_replace",    {"string", "old", "new"}, str_replace);
    interp->bindTyped("str_strip",      {"string"}, str_strip);
    interp->bindTyped("str_lower",      {"string"}, str_lower);
    interp->bindTyped("str_upper",      {"string"}, str_upper);
    interp->bindSpan("str_find",       {pStr("string"), pStr("substring"), pOpt("start_pos", TS::Num)}, str_find);
    interp->bindTyped("str_startswith", {"string", "prefix"}, str_startswith);
    interp->bindTyped("str_endswith",   {"string", "suffix"}, str_endswith);
    interp->bindTyped("str_isdigit",    {"string"}, str_isdigit);
    interp->bindTyped("str_isalpha",    {"string"}, str_isalpha);
    interp->bindTyped("str_isalnum",    {"string"}, str_isalnum);
    interp->bindTyped("str_count",      {"string", "substring"}, str_count);
    interp->bindTyped("str_ord",        {"char"}, str_ord);
    interp->bindSpan("str_chr",        {pNum("ord")},    str_chr);
    interp->bindSpan("str_repeat",     {pStr("string"), pNum("count")}, str_repeat);
    interp->bindTyped("str_reverse",    {"string"}, str_reverse);
    interp->bindSpan("str_pad_left",   {pStr("string"), pNum("width"), pOpt("pad_char", TS::Str)}, str_pad_left);
    interp->bindSpan("str_pad_right",  {pStr("string"), pNum("width"), pOpt("pad_char", TS::Str)}, str_pad_right);
    interp->bindSpan("str_format",     {pStr("template"), Param::rest("args", TS::Any)}, str_format);
    interp->bindSpan("num_fixed",      {pNum("x"), pNum("digits")}, num_fixed);
    interp->bindTyped("str_ltrim",      {"string"}, str_ltrim);
    interp->bindTyped("str_rtrim",      {"string"}, str_rtrim);

    // what each always returns (see Value::Callable::returns). the yes/no ones give the int 1 or 0, like a comparison
    interp->setResultTypes({
        {"str_split", TS::Arr}, {"str_replace", TS::Str}, {"str_strip", TS::Str}, {"str_lower", TS::Str}, {"str_upper", TS::Str},
        {"str_find", TS::Int}, {"str_startswith", TS::Int}, {"str_endswith", TS::Int},
        {"str_isdigit", TS::Int}, {"str_isalpha", TS::Int}, {"str_isalnum", TS::Int},
        {"str_count", TS::Int}, {"str_ord", TS::Int}, {"str_chr", TS::Str}, {"str_repeat", TS::Str}, {"str_reverse", TS::Str},
        {"str_pad_left", TS::Str}, {"str_pad_right", TS::Str}, {"str_format", TS::Str}, {"num_fixed", TS::Str},
        {"str_ltrim", TS::Str}, {"str_rtrim", TS::Str},
    });

    // the same answer for the same arguments and no side effects, so a const may call them while compiling
    interp->setPure({
        "str_split", "str_replace", "str_strip", "str_lower", "str_upper", "str_find", "str_startswith",
        "str_endswith", "str_isdigit", "str_isalpha", "str_isalnum", "str_count", "str_ord", "str_chr",
        "str_repeat", "str_reverse", "str_pad_left", "str_pad_right", "str_format", "num_fixed",
        "str_ltrim", "str_rtrim",
    });
    interp->setResultElems({{"str_split", TS::Str}});
}

// what this plugin says about itself. embr reads it before it runs embr_register (see core/plugininfo.h)
EMBR_PLUGIN_INFO("str", "1.0.0", "split, replace, strip, case, find, pad and format for byte strings", EMBR_CAP_NONE, EMBR_CAP_NONE,
    {"num_fixed", "x written with exactly `digits` decimals (0 to 100), like printf's %.Nf"},
    {"str_chr", "raises outside 0–255"},
    {"str_count", "non-overlapping; 0 if substring empty"},
    {"str_endswith", "1 if the string ends with suffix, else 0"},
    {"str_find", "-1 if not found"},
    {"str_format", "fills {} and {N} placeholders, each with an optional :SPEC (fill, align, sign, #, 0, width, .precision, type). see the wiki page for the grammar"},
    {"str_isalnum", "1 iff non-empty and every char qualifies"},
    {"str_isalpha", "1 iff non-empty and every char qualifies"},
    {"str_isdigit", "1 iff non-empty and every char qualifies"},
    {"str_lower", "the string with ASCII letters in lower case"},
    {"str_ltrim", "trims \" \\t\\n\\r\\f\\v\""},
    {"str_ord", "raises on empty string"},
    {"str_pad_left", "default pad \" \"; raises if pad_char isn't exactly 1 char"},
    {"str_pad_right", "default pad \" \"; raises if pad_char isn't exactly 1 char"},
    {"str_repeat", "raises on negative count"},
    {"str_replace", "raises if old is empty"},
    {"str_reverse", "the string with its bytes in reverse order (not code points)"},
    {"str_rtrim", "trims \" \\t\\n\\r\\f\\v\""},
    {"str_split", "empty separator splits into characters"},
    {"str_startswith", "1 if the string starts with prefix, else 0"},
    {"str_strip", "trims \" \\t\\n\\r\\f\\v\""},
    {"str_upper", "the string with ASCII letters in upper case"});
