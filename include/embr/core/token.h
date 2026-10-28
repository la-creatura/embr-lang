#ifndef EMBR_CORE_TOKEN_H
#define EMBR_CORE_TOKEN_H

#include "diagnostics.h"

#include <cstdint>
#include <optional>
#include <string>

namespace embr {

enum class TokenType {
    Identifier, Number, String, RawString,
    LParen, RParen,  LBracket, RBracket, LCurly, RCurly,
    Colon,  Comma,   Dot,     Ellipsis,
    Plus,   Minus,   Star,   Slash,
    PlusEq, MinusEq, StarEq, SlashEq,
    Percent,
    Equal,  EqualEqual,
    Greater, Less, GreaterEqual, LessEqual,
    Bang, BangEqual,
    Amp, Pipe, Caret, Tilde, LShift,
    LArrow, RArrow,
    And, Or,
    If, Elif, Else, End, Fn, Return, While, For, In, Break, Continue, Import, Local, Const,
    Try, Catch,
    Eof
};

struct Token {
    TokenType   type;
    std::string text;
    int         line, col;
};

// declared in core/diagnostics.h before Token existed
inline SourceRange SourceRange::fromToken(const Token& tok) {
    return {tok.line, tok.col, tok.line, tok.col + (int)tok.text.size()};
}

// the value of a hex (0x1F) or binary (0b101) literal. empty when it does not fit in 63 bits.
// a plain decimal text is not handled here.
inline std::optional<int64_t> intLiteral(const std::string& t) {
    int base = (t[1] == 'x' || t[1] == 'X') ? 16 : 2;
    uint64_t v = 0;
    for (size_t i = 2; i < t.size(); ++i) {
        char c = t[i];
        unsigned d = c <= '9' ? c - '0' : (c | 32) - 'a' + 10;
        if (v > (uint64_t(INT64_MAX) - d) / base) return std::nullopt;
        v = v * base + d;
    }
    return (int64_t)v;
}

inline bool isRadixLiteral(const std::string& t) {
    return t.size() > 1 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X' || t[1] == 'b' || t[1] == 'B');
}

} // namespace embr

#endif // EMBR_CORE_TOKEN_H
