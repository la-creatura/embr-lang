#ifndef EMBR_CORE_DIAGNOSTICS_H
#define EMBR_CORE_DIAGNOSTICS_H

// error types, source-location tracking, and small portability shims
// shared by every later stage (lexer, parser, AST, interpreter).

#include <string>
#include <vector>
#include <sstream>
#include <stdexcept>
#include <algorithm>
#include <system_error>
#include <charconv>
#include <cstdlib>
#include <cerrno>
#include <cctype>
#include <cmath>

namespace embr {

// forward declaration. SourceRange::fromToken needs a complete Token, which isn't defined until core/token.h
// its body lives there. only the declaration lives here
struct Token;

// cxxdroid and macOS ship a libc++ without double from_chars
struct parse_result { const char* ptr; std::errc ec; };

inline parse_result parse_double(const char* first, const char* last, double& value) {
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611
    auto res = std::from_chars(first, last, value);
    return {res.ptr, res.ec};
#else
    // strtod needs a NUL at the end. from_chars takes no leading space or '+', and stops a hex number at the x
    std::string text(first, last);
    if (text.empty() || std::isspace(static_cast<unsigned char>(text[0])) || text[0] == '+') return {first, std::errc::invalid_argument};
    const size_t x = text.find_first_of("xX");
    if (x != std::string::npos) text.resize(x);
    char* end;
    errno = 0;
    const double d = std::strtod(text.c_str(), &end);
    if (end == text.c_str()) return {first, std::errc::invalid_argument};
    const char* stop = first + (end - text.c_str());
    // too big or too small to keep is an error
    if (errno == ERANGE && (d == 0 || d == HUGE_VAL || d == -HUGE_VAL)) return {stop, std::errc::result_out_of_range};
    value = d;
    return {stop, std::errc{}};
#endif
}

static const char* R = "\033[31m", *Y = "\033[33m", *X = "\033[0m";

// split source in lines for error
// the folder a script's file is in, for SourceMap::setBaseDir. a name like <input> (not a file) gives the empty string
inline std::string dirOfFile(const std::string& file) {
    if (file.empty() || file[0] == '<') return std::string();
    const size_t slash = file.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : file.substr(0, slash);
}

class SourceMap {
public:
    SourceMap() = default;
    explicit SourceMap(const std::string& src) { load(src); }

    void load(const std::string& src) {
        lines_.clear();
        size_t start = 0, end = src.find('\n');
        while (end != std::string::npos) {
            lines_.push_back(src.substr(start, end - start));
            start = end + 1;
            end   = src.find('\n', start);
        }
        lines_.push_back(src.substr(start));
    }

    const std::string& get(int line) const {
        static const std::string empty;
        if (line < 1 || line > static_cast<int>(lines_.size())) return empty;
        return lines_[line - 1];
    }

    bool empty() const { return lines_.empty(); }

    // the folder of the file this text came from: where `const import "path"` looks for a relative path. empty: the current folder
    const std::string& baseDir() const { return base_; }
    void setBaseDir(std::string d) { base_ = std::move(d); }

private:
    std::vector<std::string> lines_;
    std::string              base_;
};

struct SourceRange {
    int startLine = 0, startCol = 0;
    int endLine   = 0, endCol   = 0;

    SourceRange() = default;
    SourceRange(int sl, int sc, int el, int ec)
        : startLine(sl), startCol(sc), endLine(el), endCol(ec) {}

    bool valid() const { return startLine > 0; }

    // defined out-of-line in core/token.h once Token is a complete type
    static SourceRange fromToken(const Token& tok);

    static SourceRange merge(const SourceRange& a, const SourceRange& b) {
        if (!a.valid()) return b;
        if (!b.valid()) return a;
        int sl = std::min(a.startLine, b.startLine);
        int sc = (a.startLine == b.startLine) ? std::min(a.startCol, b.startCol)
                                               : (a.startLine < b.startLine ? a.startCol : b.startCol);
        int el = std::max(a.endLine, b.endLine);
        int ec = (a.endLine == b.endLine) ? std::max(a.endCol, b.endCol)
                                           : (a.endLine > b.endLine ? a.endCol : b.endCol);
        return {sl, sc, el, ec};
    }
};

// one script-function activation an error unwound through: the function's name and where it was called
// from (file + line of the call expression in the caller). line 0 / empty file = called from native code
// (a plugin's callback, e.g. a sort comparator) where there is no script call site.
struct TraceFrame {
    std::string fn;
    std::string file;
    int         line = 0;
};

struct EmbrError : std::runtime_error {
    bool        hasLocation;
    SourceRange range;
    // unformatted message passed to raiseError(), try ... catch e ... end
    // binds e's "message" field to this, since the fully formatted what() is
    // meant for terminal display, not for a script to inspect/compare/re-report
    std::string message;
    // raiseError()'s first argument ("assert", "error", "runtime", "value", "parser", "vm", or a plugin's own
    // tag like "ffi_call"). shown in what() and given to catch as its "kind", so a script can tell what raised
    // an error without matching the message text
    std::string context;
    // call stack, innermost call first, filled in as the error unwinds out of script functions (the
    // VM's handleTry appends one frame per function it unwinds through). script natives/builtins are not frames.
    std::vector<TraceFrame> trace;
    // an error a script must not be able to catch (the step or time limit ran out). try / catch skips it
    bool fatal = false;
    // the raiser chose the kind on purpose (a C plugin's embrp_fail_kind): a call of a native does not replace it with the native's name
    bool keepContext = false;
    explicit EmbrError(const std::string& formatted, bool loc = false, SourceRange r = {},
                        std::string raw = {}, std::string ctx = {})
        : std::runtime_error(formatted), hasLocation(loc), range(r),
          message(raw.empty() ? formatted : std::move(raw)), context(std::move(ctx)) {}
};

[[noreturn]] inline void raiseError(
    const std::string& context,
    const std::string& msg,
    const SourceRange& range,
    const SourceMap&   src)
{
    std::ostringstream out;
    out << Y << "[" << context << "] " << X << msg;
    if (range.valid())
        out << Y << " at line " << range.startLine << ", col " << range.startCol << X;
    out << "\n";
    if (range.valid()) {
        const std::string& ln = src.get(range.startLine);
        if (!ln.empty()) {
            out << "  " << ln << "\n  "
                << std::string(std::max(0, range.startCol - 1), ' ')
                << R << std::string(std::max(1, range.endCol - range.startCol), '^') << X
                << "\n";
        }
    }
    throw EmbrError(out.str(), range.valid(), range, msg, context);
}

[[noreturn]] inline void raiseError(const std::string& context, const std::string& msg,
                                    const SourceRange& range = {}) {
    std::ostringstream out;
    out << "[" << context << "] " << msg;
    if (range.valid()) out << " at line " << range.startLine << ", col " << range.startCol;
    out << "\n";
    throw EmbrError(out.str(), range.valid(), range, msg, context);
}

// "stack trace (most recent call first):" block for an uncaught error, or "" when it has no frames
inline std::string formatTrace(const EmbrError& e) {
    if (e.trace.empty()) return "";
    std::ostringstream out;
    out << "stack trace (most recent call first):\n";
    for (const auto& f : e.trace) {
        out << "  in " << f.fn << "()";
        if (f.line > 0) {
            out << " called at line " << f.line;
            if (!f.file.empty()) out << " of " << f.file;
        } else {
            out << " called from native code";
        }
        out << "\n";
    }
    return out.str();
}

} // namespace embr

#endif // EMBR_CORE_DIAGNOSTICS_H
