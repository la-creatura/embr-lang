#ifndef EMBR_CORE_TOKEN_H
#define EMBR_CORE_TOKEN_H

#include "diagnostics.h"

#include <string>

namespace embr {

enum class TokenType {
    Identifier, Number, String,
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

} // namespace embr

#endif // EMBR_CORE_TOKEN_H
