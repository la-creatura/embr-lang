// fmt.h
// the source formatter behind `embr fmt`.
//
// it works on the lexer's tokens, not the AST, because the parser throws comments away.
// the text between two tokens (whitespace and comments) is read straight from the source,
// so every comment survives. it fixes indentation and spacing and keeps your line breaks.
//
// good to know
//   - before returning, formatSource() lexes its own output and compares it with the input.
//     if a token or a comment differs it throws instead of returning, so a formatter bug can
//     never change what a program does.
//   - indent is 4 spaces per level by default, like the rest of the repo's .embr files.
//   - it never joins or splits lines, and never wraps long ones.
//   - blank lines are kept, but a run of them becomes one.

#ifndef EMBR_FMT_H
#define EMBR_FMT_H

#include <embr/embr.h>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

namespace embr_fmt {

using embr::TokenType;

// one thing between two tokens: a comment, with how many newlines came before it
struct Comment {
    std::string text;
    int         newlinesBefore;   // since the previous token or comment (0 = same line)
    bool        block;            // #[ ... ]# rather than # to end of line
};

struct Piece {
    TokenType   type;
    std::string raw;                  // exact source text of the token
    int         newlinesBefore = 0;   // newlines between the previous token (or comment) and this one
    std::vector<Comment> before;      // comments between the previous token and this one
    bool        touchesPrev = false;  // no space or comment between the previous token and this one
    bool        inElemType = false;   // part of an element type written `arr<int>`: the `<`, `>` and the names between them, which take no spaces
};

struct Scanned {
    std::vector<Piece>   pieces;
    std::vector<Comment> trailing;    // comments after the last token
};

// finds the element types `arr<int>`, `map<arr<str>>` and marks their pieces. only the written-together form counts, with a type name after the
// `<`, so `arr < 3` and `if n<m` stay comparisons
inline bool isTypeWord(const std::string& w) {
    return w == "arr" || w == "array" || w == "map" || w == "num" || w == "number" || w == "float" || w == "double" || w == "int" ||
           w == "integer" || w == "str" || w == "string" || w == "fn" || w == "func" || w == "function" || w == "ptr" || w == "pointer" || w == "any";
}
// the type name at i with an optional <...>; returns the index after it, or 0 when there is none. marks when `mark`
inline size_t matchElemType(std::vector<Piece>& ps, size_t i, bool mark) {
    if (i >= ps.size() || ps[i].type != TokenType::Identifier || !isTypeWord(ps[i].raw)) return 0;
    const bool container = ps[i].raw == "arr" || ps[i].raw == "array" || ps[i].raw == "map";
    if (!container || i + 1 >= ps.size() || ps[i + 1].type != TokenType::Less || !ps[i + 1].touchesPrev) return i + 1;
    if (i + 2 >= ps.size() || !ps[i + 2].touchesPrev) return 0;
    size_t after = matchElemType(ps, i + 2, mark);
    if (after == 0 || after >= ps.size() || ps[after].type != TokenType::Greater || !ps[after].touchesPrev) return 0;
    if (mark) { ps[i + 1].inElemType = true; ps[i + 2].inElemType = true; ps[after].inElemType = true; }
    return after + 1;
}
inline void markElemTypes(std::vector<Piece>& ps) {
    for (size_t i = 0; i + 1 < ps.size(); ++i) {
        if (ps[i].type != TokenType::Identifier || !ps[i + 1].touchesPrev || ps[i + 1].type != TokenType::Less) continue;
        size_t end = matchElemType(ps, i, false);
        if (end > i + 1) { matchElemType(ps, i, true); i = end - 1; }
    }
}

// splits src into tokens and the comments between them. throws embr::EmbrError if it doesn't lex.
inline Scanned scan(const std::string& src) {
    embr::Lexer lex(src);
    std::vector<embr::Token> toks = lex.tokenize();

    std::vector<size_t> lineStart = {0};
    for (size_t i = 0; i < src.size(); ++i)
        if (src[i] == '\n') lineStart.push_back(i + 1);

    // reads whitespace and comments in src[from, to) into comments, returns newlines after the last one
    auto readGap = [&](size_t from, size_t to, std::vector<Comment>& out) {
        int nl = 0;
        size_t i = from;
        while (i < to) {
            char c = src[i];
            if (c == '\n') { ++nl; ++i; }
            else if (c == '#' && i + 1 < to && src[i + 1] == '[') {
                size_t e = src.find("]#", i + 2);
                e = (e == std::string::npos || e + 2 > to) ? to : e + 2;
                out.push_back({src.substr(i, e - i), nl, true});
                nl = 0; i = e;
            } else if (c == '#') {
                size_t e = i;
                while (e < to && src[e] != '\n') ++e;
                while (e > i && (src[e - 1] == '\r' || src[e - 1] == ' ' || src[e - 1] == '\t')) --e;
                out.push_back({src.substr(i, e - i), nl, false});
                nl = 0; i = e;
                while (i < to && src[i] != '\n') ++i;
            } else ++i;
        }
        return nl;
    };

    Scanned out;
    size_t prevEnd = 0;
    for (const embr::Token& t : toks) {
        size_t start = t.type == TokenType::Eof ? src.size()
                       : lineStart[(size_t)t.line - 1] + (size_t)t.col - 1;
        std::vector<Comment> gap;
        int nl = readGap(prevEnd, start, gap);
        if (t.type == TokenType::Eof) { out.trailing = std::move(gap); break; }

        size_t end = start + t.text.size();
        if (t.type == TokenType::String) {           // the token holds the decoded text, so find the raw end
            end = start + 1;
            while (end < src.size() && src[end] != '"') end += src[end] == '\\' ? 2 : 1;
            ++end;
        }
        Piece p;
        p.type = t.type;
        p.raw = src.substr(start, end - start);
        p.newlinesBefore = nl;
        p.before = std::move(gap);
        p.touchesPrev = start == prevEnd && !out.pieces.empty();
        out.pieces.push_back(std::move(p));
        prevEnd = end;
    }
    markElemTypes(out.pieces);
    return out;
}

inline bool isOperand(TokenType t) {
    return t == TokenType::Identifier || t == TokenType::Number || t == TokenType::String ||
           t == TokenType::RParen || t == TokenType::RBracket || t == TokenType::RCurly;
}

inline bool isBinary(TokenType t) {
    switch (t) {
        case TokenType::Plus: case TokenType::Minus: case TokenType::Star: case TokenType::Slash:
        case TokenType::Percent: case TokenType::Equal: case TokenType::EqualEqual: case TokenType::BangEqual:
        case TokenType::Greater: case TokenType::Less: case TokenType::GreaterEqual: case TokenType::LessEqual:
        case TokenType::PlusEq: case TokenType::MinusEq: case TokenType::StarEq: case TokenType::SlashEq:
        case TokenType::RArrow: case TokenType::And: case TokenType::Or: case TokenType::In:
        case TokenType::Pipe: case TokenType::Caret: case TokenType::LShift:
            return true;
        default: return false;
    }
}

// the formatted text for one piece sequence, tracking just enough state to place spaces
struct Layout {
    // deeper than this is not indented any further, so absurdly nested input can't make the output huge
    static constexpr int kMaxIndentLevels = 32;
    int  indentWidth = 4;      // spaces per level
    std::string out;
    std::string line;          // the line being built, without indent
    // one entry per open if/while/for/try/fn block or ( [ {: the indent level of the line that opened it.
    // a line inside the thing is indented one level more than that line, so `f(fn(x)` and the body under
    // it indent once, not twice.
    std::vector<int> frames;
    bool lineHasCode = false;
    bool pendingBlank = false;
    bool anyLine = false;
    bool unaryBefore = false;  // the last token was a prefix operator: no space after it
    TokenType prev = TokenType::Eof;
    TokenType prevPrev = TokenType::Eof;

    int innerLevel() const { return frames.empty() ? 0 : frames.back() + 1; }

    int indentLevel(TokenType first) const {
        using T = TokenType;
        bool closes = first == T::End || first == T::Elif || first == T::Else || first == T::Catch ||
                      first == T::RParen || first == T::RBracket || first == T::RCurly;
        return closes && !frames.empty() ? frames.back() : innerLevel();
    }

    void flushLine(int indent) {
        if (line.empty()) return;
        if (pendingBlank && anyLine) out += "\n";
        pendingBlank = false;
        out += std::string((size_t)std::min(indent, kMaxIndentLevels) * (size_t)indentWidth, ' ') + line + "\n";
        anyLine = true;
        line.clear();
        lineHasCode = false;
    }

    int lineIndent = 0;
    void endLine() { flushLine(lineIndent); prev = TokenType::Eof; unaryBefore = false; }

    bool needSpace(TokenType cur) const {
        using T = TokenType;
        if (prev == T::Eof) return false;                       // first thing on the line
        if (unaryBefore) return false;
        if (cur == T::Comma || cur == T::RParen || cur == T::RBracket || cur == T::RCurly || cur == T::Colon) return false;
        if (prev == T::LParen || prev == T::LBracket || prev == T::LCurly) return false;
        if (cur == T::Dot || prev == T::Dot || prev == T::Ellipsis) return false;
        if (cur == T::Ellipsis && isOperand(prev)) return false;   // a range, `[0...10]`; a rest parameter follows a comma or a bracket and keeps its space
        if (cur == T::LParen) return !(isOperand(prev) && prev != T::Number && prev != T::String) && prev != T::Fn;
        if (cur == T::LBracket) return !isOperand(prev);
        return true;
    }

    // a piece is a prefix operator if it is ! or ~, or a - or & after something that isn't a value (`f(&x)` against `a & b`)
    bool isPrefix(TokenType cur) const {
        if (cur == TokenType::Bang || cur == TokenType::Tilde) return true;
        return (cur == TokenType::Minus || cur == TokenType::Amp) && !isOperand(prev);
    }

    void addToken(const Piece& p, bool startsLine) {
        using T = TokenType;
        if (startsLine) {
            endLine();
            lineIndent = indentLevel(p.type);
            // an fn used as a type name (x: fn) opens no block, see below
        }
        if (p.inElemType) { /* part of an element type: written together */ }
        else if (p.type == T::Greater && prev == T::Greater && p.touchesPrev) { /* the two halves of a >> shift */ }
        else if (needSpace(p.type)) line += ' ';
        // no space after a prefix operator, unless the next token would glue to it into a different one (`! =` -> `!=`)
        else if (unaryBefore && !line.empty() && !p.raw.empty() &&
                 (((line.back() == '!' || line.back() == '-') && p.raw[0] == '=') || (line.back() == '-' && p.raw[0] == '>')))
            line += ' ';
        line += p.raw;
        lineHasCode = true;
        bool prefix = isPrefix(p.type);
        unaryBefore = prefix;

        bool isOpener = p.type == T::If || p.type == T::While || p.type == T::For || p.type == T::Try ||
                        p.type == T::LParen || p.type == T::LBracket || p.type == T::LCurly ||
                        (p.type == T::Fn && prev != T::Colon && prev != T::RArrow);
        bool isCloser = p.type == T::End || p.type == T::RParen || p.type == T::RBracket || p.type == T::RCurly;
        if (isOpener) frames.push_back(lineIndent);
        else if (isCloser && !frames.empty()) frames.pop_back();
        prevPrev = prev;
        prev = p.type;
    }

    // a comment sitting between tokens, or at the end of the file
    void addComment(const Comment& c, bool ownLine) {
        if (!ownLine && lineHasCode) {
            if (c.block) {                 // a block comment in the middle of a line stays inline
                if (!line.empty()) line += ' ';
                line += c.text;
            } else line += "  " + c.text;  // trailing line comment
            return;
        }
        endLine();
        lineIndent = innerLevel();
        line = c.text;
        lineHasCode = false;
        if (!c.block) { flushLine(lineIndent); }
        else {
            // a block comment on its own line may be followed by code on the same line; keep it open
            lineHasCode = true;
            prev = TokenType::Comma;       // acts like "after a value separator": next token gets a space
        }
    }
};

inline std::string layout(const Scanned& s, int indentWidth) {
    Layout L;
    L.indentWidth = indentWidth;
    bool first = true;
    auto gapBlank = [&](int nl) { if (nl >= 2) L.pendingBlank = true; };

    for (const Piece& p : s.pieces) {
        for (const Comment& c : p.before) {
            bool ownLine = c.newlinesBefore > 0 || first;
            if (ownLine) { L.endLine(); gapBlank(c.newlinesBefore); }
            L.addComment(c, ownLine);
            first = false;
        }
        bool newLine = p.newlinesBefore > 0 || first;
        if (newLine) {
            bool afterBlockComment = !p.before.empty() && p.before.back().block && L.lineHasCode && p.newlinesBefore == 0;
            if (!afterBlockComment) { L.endLine(); gapBlank(p.newlinesBefore); }
        }
        L.addToken(p, newLine && L.line.empty());
        first = false;
    }
    for (const Comment& c : s.trailing) {
        bool ownLine = c.newlinesBefore > 0 || first;
        if (ownLine) { L.endLine(); gapBlank(c.newlinesBefore); }
        L.addComment(c, ownLine);
        first = false;
    }
    L.endLine();
    return L.out;
}

struct FormatError : std::runtime_error { using std::runtime_error::runtime_error; };

inline bool sameTokens(const Scanned& a, const Scanned& b) {
    if (a.pieces.size() != b.pieces.size()) return false;
    for (size_t i = 0; i < a.pieces.size(); ++i) {
        if (a.pieces[i].type != b.pieces[i].type || a.pieces[i].raw != b.pieces[i].raw) return false;
        if (a.pieces[i].before.size() != b.pieces[i].before.size()) return false;
        for (size_t k = 0; k < a.pieces[i].before.size(); ++k)
            if (a.pieces[i].before[k].text != b.pieces[i].before[k].text) return false;
    }
    if (a.trailing.size() != b.trailing.size()) return false;
    for (size_t k = 0; k < a.trailing.size(); ++k)
        if (a.trailing[k].text != b.trailing[k].text) return false;
    return true;
}

// returns the formatted source. throws embr::EmbrError if src doesn't lex, FormatError if the
// result would not match the input token for token (a bug in the formatter, nothing is written).
inline std::string formatSource(const std::string& src, int indentWidth = 4) {
    Scanned in = scan(src);
    std::string out = layout(in, indentWidth);
    Scanned back;
    try { back = scan(out); }
    catch (const embr::EmbrError& e) { throw FormatError(std::string("formatted output does not lex: ") + e.message); }
    if (!sameTokens(in, back)) throw FormatError("formatted output differs from the input in tokens or comments");
    return out;
}

} // namespace embr_fmt

#endif // EMBR_FMT_H
