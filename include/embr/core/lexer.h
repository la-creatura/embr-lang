#ifndef EMBR_CORE_LEXER_H
#define EMBR_CORE_LEXER_H

#include "diagnostics.h"
#include "token.h"

#include <string>
#include <vector>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <unordered_map>

namespace embr {

// every keyword and its token. the language server's completion lists these too
inline const std::unordered_map<std::string, TokenType>& keywordTable() {
    static const std::unordered_map<std::string, TokenType> kw = {
        {"if",TokenType::If},        {"elif",TokenType::Elif},    {"else",TokenType::Else},
        {"end",TokenType::End},
        {"fn",TokenType::Fn},        {"return",TokenType::Return},{"while",TokenType::While},
        {"for",TokenType::For},      {"in",TokenType::In},
        {"break",TokenType::Break},  {"continue",TokenType::Continue},
        {"import",TokenType::Import},{"local",TokenType::Local},{"const",TokenType::Const},
        {"and",TokenType::And},      {"or",TokenType::Or},
        {"try",TokenType::Try},      {"catch",TokenType::Catch},
    };
    return kw;
}

class Lexer {
public:
    explicit Lexer(const std::string& src) : s_(src), map_(src) {}

    std::vector<Token> tokenize() {
        std::vector<Token> out;
        while (!eof()) {
            skipWs();
            if (eof()) break;
            char c = peek();
            if (std::isalpha((unsigned char)c) || c == '_') out.push_back(lexIdent());
            else if (std::isdigit(c))                       out.push_back(lexNum());
            else if (c == '"')                              out.push_back(lexStr());
            else                                            out.push_back(lexSym());
        }
        out.push_back({TokenType::Eof, "", line_, col_});
        return out;
    }

    const SourceMap& sourceMap() const { return map_; }

private:
    const std::string& s_;
    SourceMap          map_;
    size_t pos_  = 0;
    int    line_ = 1, col_ = 1;

    bool eof()  const { return pos_ >= s_.size(); }
    char peek() const { return eof() ? '\0' : s_[pos_]; }
    char peek(size_t n) const { return pos_ + n < s_.size() ? s_[pos_ + n] : '\0'; }
    char get() {
        char c = s_[pos_++];
        if (c == '\n') { ++line_; col_ = 1; } else ++col_;
        return c;
    }

    void skipWs() {
        while (!eof()) {
            char c = peek();
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') get();
            else if (c == '#' && pos_ + 1 < s_.size() && s_[pos_ + 1] == '[') skipBlockComment();
            else if (c == '#') { while (!eof() && peek() != '\n') get(); }
            else break;
        }
    }

    // #[ ... ]#  a block comment, to comment out several statements without a '#' on every line
    // it doesn't nest: the first ']#' closes it, even inside an inner #[
    // the delimiters are two characters on both ends so they can't clash with a lone '[' or ']' (array syntax)
    // or a lone '#' (line comment)
    void skipBlockComment() {
        int sl = line_, sc = col_;
        get(); get(); // consume '#['
        while (!eof() && !(peek() == ']' && pos_ + 1 < s_.size() && s_[pos_ + 1] == '#')) get();
        if (eof()) raiseError("lexer", "unterminated block comment (expected ']#')", {sl, sc, line_, col_}, map_);
        get(); get(); // consume ']#'
    }

    Token lexIdent() {
        int sl = line_, sc = col_; std::string t;
        while (!eof() && (std::isalnum((unsigned char)peek()) || peek() == '_')) t += get();
        const auto& kw = keywordTable();
        auto it = kw.find(t);
        return {it != kw.end() ? it->second : TokenType::Identifier, t, sl, sc};
    }

    Token lexNum() {
        int sl = line_, sc = col_; std::string t; bool dot = false;
        // 0x1F and 0b101: the token keeps the text as written, intLiteral() (token.h) reads the value
        if (peek() == '0' && pos_ + 1 < s_.size() && (s_[pos_ + 1] == 'x' || s_[pos_ + 1] == 'X' || s_[pos_ + 1] == 'b' || s_[pos_ + 1] == 'B')) {
            bool hex = s_[pos_ + 1] == 'x' || s_[pos_ + 1] == 'X';
            t += get(); t += get();
            auto ok = [&](char c) { return hex ? std::isxdigit((unsigned char)c) != 0 : (c == '0' || c == '1'); };
            while (!eof() && ok(peek())) t += get();
            if (t.size() == 2)
                raiseError("lexer", hex ? "hex literal has no digits" : "binary literal has no digits", {sl,sc,line_,col_}, map_);
            if (!eof() && (std::isalnum((unsigned char)peek()) || peek() == '_' || peek() == '.'))
                raiseError("lexer", "invalid digit in number literal", {sl,sc,line_,col_}, map_);
            if (!intLiteral(t))
                raiseError("lexer", "number literal does not fit an int", {sl,sc,line_,col_}, map_);
            return {TokenType::Number, t, sl, sc};
        }
        while (!eof() && (std::isdigit(peek()) || peek() == '.')) {
            if (peek() == '.') {
                if (pos_ + 1 < s_.size() && s_[pos_ + 1] == '.') break;   // `0...10`: the dots are the range's, not a decimal point
                if (dot) raiseError("lexer","multiple decimal points",{sl,sc,line_,col_},map_);
                dot = true;
            }
            t += get();
        }
        if (t.back() == '.')
            raiseError("lexer","trailing decimal point",{sl,sc,line_,col_},map_);
        // exponent: e/E, an optional sign, then at least one digit (otherwise the 'e' starts an
        // identifier, exactly as before). an exponent always makes the literal a float.
        if (!eof() && (peek() == 'e' || peek() == 'E')) {
            size_t p = pos_ + 1;
            if (p < s_.size() && (s_[p] == '+' || s_[p] == '-')) ++p;
            if (p < s_.size() && std::isdigit((unsigned char)s_[p])) {
                while (pos_ < p) t += get();                       // 'e' and the sign
                while (!eof() && std::isdigit((unsigned char)peek())) t += get();
                double v = std::strtod(t.c_str(), nullptr);
                if (std::isinf(v))
                    raiseError("lexer","number literal out of range",{sl,sc,line_,col_},map_);
            }
        }
        return {TokenType::Number, t, sl, sc};
    }

    static int hexDigit(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }
    static void appendUtf8(std::string& out, unsigned cp) {
        if (cp < 0x80)         out += (char)cp;
        else if (cp < 0x800)   { out += (char)(0xC0 | (cp >> 6));  out += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
        else                   { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F));
                                 out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
    }

    // `"""text"""`: everything up to the next three quotes, as written. no escapes
    Token lexRaw() {
        int sl = line_, sc = col_; get(); get(); get(); std::string t;
        while (!eof()) {
            if (peek() == '"' && peek(1) == '"' && peek(2) == '"') { get(); get(); get(); return {TokenType::RawString, t, sl, sc}; }
            t += get();
        }
        raiseError("lexer", "unterminated \"\"\" text", {sl, sc, line_, col_}, map_);
    }

    Token lexStr() {
        if (peek(1) == '"' && peek(2) == '"') return lexRaw();
        int sl = line_, sc = col_; get(); std::string t;
        while (!eof()) {
            char c = get();
            if (c == '"') return {TokenType::String, t, sl, sc};
            if (c == '\\') {
                if (eof()) raiseError("lexer","unterminated escape",{sl,sc,line_,col_},map_);
                switch (char nx = get()) {
                    case 'n':t+='\n';break; case 't':t+='\t';break;
                    case 'r':t+='\r';break; case '0':t+='\0';break;
                    case '"':t+='"'; break; case '\\':t+='\\';break;
                    case 'x': {                               // \xNN: one byte, exactly two hex digits
                        int v = 0;
                        for (int k = 0; k < 2; ++k) {
                            int h = eof() ? -1 : hexDigit(peek());
                            if (h < 0) raiseError("lexer","\\x needs exactly two hex digits",{sl,sc,line_,col_},map_);
                            v = v * 16 + h; get();
                        }
                        t += (char)v; break;
                    }
                    case 'u': {                               // \uXXXX or \u{X...}: a code point, UTF-8 encoded
                        unsigned cp = 0; int digits = 0;
                        if (!eof() && peek() == '{') {
                            get();
                            while (!eof() && peek() != '}') {
                                int h = hexDigit(peek());
                                if (h < 0 || ++digits > 6) raiseError("lexer","invalid \\u{...} escape",{sl,sc,line_,col_},map_);
                                cp = cp * 16 + (unsigned)h; get();
                            }
                            if (eof() || digits == 0) raiseError("lexer","invalid \\u{...} escape",{sl,sc,line_,col_},map_);
                            get();                              // '}'
                        } else {
                            for (; digits < 4; ++digits) {
                                int h = eof() ? -1 : hexDigit(peek());
                                if (h < 0) raiseError("lexer","\\u needs exactly four hex digits",{sl,sc,line_,col_},map_);
                                cp = cp * 16 + (unsigned)h; get();
                            }
                        }
                        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
                            raiseError("lexer","\\u escape is not a valid Unicode scalar value",{sl,sc,line_,col_},map_);
                        appendUtf8(t, cp);
                        break;
                    }
                    default: raiseError("lexer",std::string("unknown escape \\")+nx,
                                        {sl,sc,line_,col_},map_);
                }
            } else t += c;
        }
        raiseError("lexer","unterminated string literal",{sl,sc,line_,col_},map_);
    }

    Token lexSym() {
        int sl = line_, sc = col_; char c = get();
        switch (c) {
            case '+': if(peek()=='='){get();return{TokenType::PlusEq,      "+=",sl,sc};} return{TokenType::Plus,   "+",sl,sc};
            case '-': if(peek()=='>'){get();return{TokenType::RArrow,      "->",sl,sc};}
                      if(peek()=='='){get();return{TokenType::MinusEq,     "-=",sl,sc};} return{TokenType::Minus,  "-",sl,sc};
            case '*': if(peek()=='='){get();return{TokenType::StarEq,      "*=",sl,sc};} return{TokenType::Star,   "*",sl,sc};
            case '/': if(peek()=='='){get();return{TokenType::SlashEq,     "/=",sl,sc};} return{TokenType::Slash,  "/",sl,sc};
            case '%': return {TokenType::Percent, "%",sl,sc};

            case '(': return {TokenType::LParen,  "(",sl,sc};
            case ')': return {TokenType::RParen,  ")",sl,sc};
            case '[': return {TokenType::LBracket,"[",sl,sc};
            case ']': return {TokenType::RBracket,"]",sl,sc};
            case '{': return {TokenType::LCurly,  "{",sl,sc};
            case '}': return {TokenType::RCurly,  "}",sl,sc};

            case ',': return {TokenType::Comma,   ",",sl,sc};
            case '.':
                if (peek()=='.') {
                    get();
                    if (peek()=='.') { get(); return {TokenType::Ellipsis, "...", sl, sc}; }
                    raiseError("lexer","unexpected '..' (did you mean '...'?)",{sl,sc,line_,col_},map_);
                }
                return {TokenType::Dot, ".", sl, sc};
            case ':': return {TokenType::Colon,   ":",sl,sc};

            case '=': if(peek()=='='){get();return{TokenType::EqualEqual,  "==",sl,sc};} return{TokenType::Equal,  "=",sl,sc};
            case '>': if(peek()=='='){get();return{TokenType::GreaterEqual,">=",sl,sc};} return{TokenType::Greater,">",sl,sc};
            case '<': if(peek()=='='){get();return{TokenType::LessEqual,   "<=",sl,sc};}
                      if(peek()=='<'){get();return{TokenType::LShift,      "<<",sl,sc};} return{TokenType::Less,   "<",sl,sc};
            case '&': return {TokenType::Amp, "&", sl, sc};
            case '|': return {TokenType::Pipe, "|", sl, sc};
            case '^': return {TokenType::Caret, "^", sl, sc};
            case '~': return {TokenType::Tilde, "~", sl, sc};
            case '!': if(peek()=='='){get();return{TokenType::BangEqual,   "!=",sl,sc};} return{TokenType::Bang,   "!",sl,sc};

        }
        raiseError("lexer", std::string("unexpected character: ")+c, {sl,sc,line_,col_}, map_);
    }
};

} // namespace embr

#endif // EMBR_CORE_LEXER_H
