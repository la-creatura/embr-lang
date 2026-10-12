// pike.h
// a small, linear-time regular-expression engine (a "Pike VM") for the regex plugin. header-only and
// independent of embr, so tests/regex_engine_test.cpp can test it alone (it compares it against std::regex on
// thousands of random patterns)
//
// why: std::regex (libstdc++) recurses over the subject, so an ordinary `[ab]*c` over ~30,000 characters overflows
// the stack and kills the process, and nested quantifiers like `(a+)+$` backtrack exponentially (ReDoS). this
// engine runs any pattern it accepts in O(pattern * subject) time with explicit heap state: no recursion over the
// subject, no backtracking
//
// supported (the ECMAScript subset scripts actually use), matched byte-wise like std::regex is:
//   literals, `.` (any byte except \n \r), classes [a-z] [^...] with \d \w \s \D \W \S inside. a character above 0x7f
//   in a class ([é], [\u00e9]) is one more choice of its UTF-8 bytes; in a negated class or a range it is a SyntaxError,
//   since byte-wise matching can't do those right.
//   escapes \d \D \w \W \s \S \b \B \n \r \t \f \v \0 \xHH \uHHHH (UTF-8 encoded) and identity escapes,
//   anchors ^ $ (the whole string, or each line with the multiline flag; \n and \r end a line),
//   groups ( ), (?: ) and named (?<name> ), alternation |,
//   quantifiers * + ? {n} {n,} {n,m} and their lazy forms (*? +? ?? {n,m}?), leftmost-first semantics.
// NOT supported here (raises rx::Unsupported so the caller can fall back to a backtracking engine):
//   backreferences (\1, \k<name>), lookahead/lookbehind ((?= (?! (?<= (?<!).
// malformed patterns raise rx::SyntaxError.
//
// captures inside a quantified group are NOT reset on each iteration (they keep the last value
// captured), matching libstdc++'s std::regex rather than the ECMAScript spec's reset rule.

#ifndef EMBR_REGEX_PIKE_H
#define EMBR_REGEX_PIKE_H

#include <bitset>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace rx {

struct Unsupported : std::runtime_error { using std::runtime_error::runtime_error; };
struct SyntaxError : std::runtime_error { using std::runtime_error::runtime_error; };

// resource limits: turn a hostile pattern into an ordinary error, like the parser's nesting limits do
constexpr size_t kMaxPattern = 20000;    // bytes of pattern text
constexpr size_t kMaxInsts   = 200000;   // compiled program size (counted quantifiers expand a copy per repeat)
constexpr int    kMaxDepth   = 200;      // nesting of groups
constexpr int    kMaxRepeat  = 1000;     // largest {n} / {n,m} bound

enum class Op : uint8_t { Char, Any, Class, Split, Jmp, Save, Bol, Eol, WordB, NotWordB, Match };

struct Inst {
    Op  op;
    int x = 0;     // Char: byte; Class: class index; Split: preferred target; Jmp: target; Save: slot
    int y = 0;     // Split: other target
};

struct Prog {
    std::vector<Inst>              insts;
    std::vector<std::bitset<256>>  classes;
    int                            ngroups = 0;      // capture groups, not counting the whole match
    std::vector<std::string>       names;            // names[g] is group g's name, "" when it has none (size ngroups + 1)
    bool                           multiline = false; // ^ and $ also match at line breaks
    int ncap() const { return 2 * (ngroups + 1); }
};

namespace detail {

struct Node {
    enum Kind { Empty, Char, Any, Class, Cat, Alt, Rep, Group, Bol, Eol, WordB, NotWordB } kind = Empty;
    int  c = 0;                 // Char: byte, Class: class index, Group: capture index (-1 = non-capturing)
    int  min = 0, max = 0;      // Rep bounds (max == -1: unbounded)
    bool greedy = true;
    std::vector<int> kids;      // indices into Parser::nodes
};

inline bool isWordByte(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

inline std::bitset<256> digitSet() { std::bitset<256> b; for (int c = '0'; c <= '9'; ++c) b.set(c); return b; }
inline std::bitset<256> wordSet()  { std::bitset<256> b; for (int c = 0; c < 256; ++c) if (isWordByte((unsigned char)c)) b.set(c); return b; }
inline std::bitset<256> spaceSet() { std::bitset<256> b; for (char c : {' ', '\t', '\n', '\v', '\f', '\r'}) b.set((unsigned char)c); return b; }

class Parser {
public:
    explicit Parser(const std::string& p) : p_(p) {
        if (p.size() > kMaxPattern) throw SyntaxError("pattern too long (max " + std::to_string(kMaxPattern) + " bytes)");
    }

    Prog compile(bool multiline) {
        int root = parseAlt(0);
        if (i_ < p_.size()) throw SyntaxError("unbalanced ')'");
        Prog pg;
        pg.ngroups = ngroups_;
        names_.resize((size_t)ngroups_ + 1);
        pg.names = std::move(names_);
        pg.multiline = multiline;
        pg.classes = std::move(classes_);
        prog_ = &pg;
        push({Op::Save, 0, 0});
        emit(root);
        push({Op::Save, 1, 0});
        push({Op::Match, 0, 0});
        prog_ = nullptr;
        return pg;
    }

private:
    const std::string&        p_;
    size_t                    i_ = 0;
    std::vector<Node>         nodes_;
    std::vector<std::bitset<256>> classes_;
    int                       ngroups_ = 0;
    std::vector<std::string>  names_;
    Prog*                     prog_ = nullptr;

    // ---- parsing (recursion bounded by kMaxDepth) ----
    int node(Node n) { nodes_.push_back(std::move(n)); return (int)nodes_.size() - 1; }
    bool more() const { return i_ < p_.size(); }
    char peek() const { return p_[i_]; }

    // `(?<name>`, after the `<`: reads the name and the `>`, and gives the group the next number
    int namedGroup() {
        size_t start = i_;
        while (more() && peek() != '>') {
            char c = peek();
            bool ok = c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (i_ > start && c >= '0' && c <= '9');
            if (!ok) throw SyntaxError("invalid group name");
            ++i_;
        }
        if (!more() || i_ == start) throw SyntaxError("invalid group name");
        std::string name = p_.substr(start, i_ - start);
        ++i_;                                                  // the '>'
        for (const auto& n : names_) if (n == name) throw SyntaxError("duplicate group name '" + name + "'");
        int cap = ++ngroups_;
        names_.resize((size_t)cap + 1);
        names_[(size_t)cap] = name;
        return cap;
    }

    int newClass(const std::bitset<256>& b) { classes_.push_back(b); return (int)classes_.size() - 1; }

    int parseAlt(int depth) {
        if (depth > kMaxDepth) throw SyntaxError("pattern nested too deeply");
        std::vector<int> alts;
        alts.push_back(parseCat(depth));
        while (more() && peek() == '|') { ++i_; alts.push_back(parseCat(depth)); }
        if (alts.size() == 1) return alts[0];
        Node n; n.kind = Node::Alt; n.kids = std::move(alts);
        return node(std::move(n));
    }

    int parseCat(int depth) {
        Node cat; cat.kind = Node::Cat;
        while (more() && peek() != '|' && peek() != ')') cat.kids.push_back(parseTerm(depth));
        if (cat.kids.empty()) return node(Node{});           // Empty
        if (cat.kids.size() == 1) return cat.kids[0];
        return node(std::move(cat));
    }

    int parseTerm(int depth) {
        int atom = parseAtom(depth);
        if (!more()) return atom;
        int mn, mx;
        char c = peek();
        if (c == '*')      { mn = 0; mx = -1; ++i_; }
        else if (c == '+') { mn = 1; mx = -1; ++i_; }
        else if (c == '?') { mn = 0; mx = 1;  ++i_; }
        else if (c == '{' && parseBraces(mn, mx)) { /* consumed */ }
        else return atom;
        Node r; r.kind = Node::Rep; r.min = mn; r.max = mx; r.kids = {atom};
        if (more() && peek() == '?') { r.greedy = false; ++i_; }
        return node(std::move(r));
    }

    // {n} {n,} {n,m}: returns false (consuming nothing) when it isn't a valid quantifier, in which
    // case the '{' is an ordinary literal (ECMAScript's lenient rule)
    bool parseBraces(int& mn, int& mx) {
        size_t j = i_ + 1;
        auto num = [&](long& out) {
            if (j >= p_.size() || p_[j] < '0' || p_[j] > '9') return false;
            long v = 0;
            while (j < p_.size() && p_[j] >= '0' && p_[j] <= '9') {
                v = v * 10 + (p_[j++] - '0');
                if (v > 100000) v = 100000;
            }
            out = v; return true;
        };
        long a, b = -1;
        if (!num(a)) return false;
        if (j < p_.size() && p_[j] == ',') {
            ++j;
            if (j < p_.size() && p_[j] == '}') b = -1;
            else if (!num(b)) return false;
        } else b = a;
        if (j >= p_.size() || p_[j] != '}') return false;
        if (a > kMaxRepeat || b > kMaxRepeat) throw SyntaxError("quantifier too large (max " + std::to_string(kMaxRepeat) + ")");
        if (b != -1 && a > b) throw SyntaxError("quantifier range out of order");
        mn = (int)a; mx = (int)b;
        i_ = j + 1;
        return true;
    }

    int parseAtom(int depth) {
        char c = p_[i_++];
        switch (c) {
            case '(': {
                int cap = -1;
                if (more() && peek() == '?') {
                    ++i_;
                    if (!more()) throw SyntaxError("invalid group");
                    char k = p_[i_++];
                    if (k == ':') { /* non-capturing */ }
                    else if (k == '=' || k == '!') throw Unsupported("lookahead");
                    else if (k == '<' && more() && (peek() == '=' || peek() == '!')) throw Unsupported("lookbehind");
                    else if (k == '<') cap = namedGroup();
                    else throw SyntaxError("invalid group syntax");
                } else {
                    cap = ++ngroups_;
                }
                int body = parseAlt(depth + 1);
                if (!more() || peek() != ')') throw SyntaxError("unbalanced '('");
                ++i_;
                Node g; g.kind = Node::Group; g.c = cap; g.kids = {body};
                return node(std::move(g));
            }
            case '[': return parseClass();
            case '.': { Node n; n.kind = Node::Any; return node(std::move(n)); }
            case '^': { Node n; n.kind = Node::Bol; return node(std::move(n)); }
            case '$': { Node n; n.kind = Node::Eol; return node(std::move(n)); }
            case '*': case '+': case '?': throw SyntaxError("nothing to repeat");
            case '\\': return parseEscape();
            default: { Node n; n.kind = Node::Char; n.c = (unsigned char)c; return node(std::move(n)); }
        }
    }

    static int hexVal(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }
    bool readHex(int digits, unsigned& out) {
        if (i_ + digits > p_.size()) return false;
        unsigned v = 0;
        for (int k = 0; k < digits; ++k) {
            int h = hexVal(p_[i_ + k]);
            if (h < 0) return false;
            v = v * 16 + (unsigned)h;
        }
        out = v; i_ += digits; return true;
    }

    static std::string utf8(unsigned cp) {
        std::string s;
        if (cp < 0x80) s += (char)cp;
        else if (cp < 0x800)  { s += (char)(0xC0 | (cp >> 6)); s += (char)(0x80 | (cp & 0x3F)); }
        else                  { s += (char)(0xE0 | (cp >> 12)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
        return s;
    }

    int charNode(int byte) { Node n; n.kind = Node::Char; n.c = byte & 0xFF; return node(std::move(n)); }

    int parseEscape() {
        if (!more()) throw SyntaxError("trailing backslash");
        char c = p_[i_++];
        switch (c) {
            case 'd': { Node n; n.kind = Node::Class; n.c = newClass(digitSet()); return node(std::move(n)); }
            case 'D': { Node n; n.kind = Node::Class; n.c = newClass(~digitSet()); return node(std::move(n)); }
            case 'w': { Node n; n.kind = Node::Class; n.c = newClass(wordSet()); return node(std::move(n)); }
            case 'W': { Node n; n.kind = Node::Class; n.c = newClass(~wordSet()); return node(std::move(n)); }
            case 's': { Node n; n.kind = Node::Class; n.c = newClass(spaceSet()); return node(std::move(n)); }
            case 'S': { Node n; n.kind = Node::Class; n.c = newClass(~spaceSet()); return node(std::move(n)); }
            case 'b': { Node n; n.kind = Node::WordB; return node(std::move(n)); }
            case 'B': { Node n; n.kind = Node::NotWordB; return node(std::move(n)); }
            case 'n': return charNode('\n');
            case 'r': return charNode('\r');
            case 't': return charNode('\t');
            case 'f': return charNode('\f');
            case 'v': return charNode('\v');
            case '0': return charNode(0);
            case 'x': { unsigned v; if (readHex(2, v)) return charNode((int)v); return charNode('x'); }
            case 'u': {
                unsigned v;
                if (!readHex(4, v)) return charNode('u');
                std::string bytes = utf8(v);
                if (bytes.size() == 1) return charNode((unsigned char)bytes[0]);
                Node cat; cat.kind = Node::Cat;
                for (unsigned char b : bytes) cat.kids.push_back(charNode(b));
                return node(std::move(cat));
            }
            case 'c': return charNode('c');
            case 'k': throw Unsupported("named backreference");
            default:
                if (c >= '1' && c <= '9') throw Unsupported("backreference");
                return charNode((unsigned char)c);
        }
    }

    // one class member: a single byte, a whole set (\d), or a character above 0x7f as its UTF-8 bytes (`seq`),
    // written as itself or as \uHHHH
    struct ClassItem { bool isSet = false; int byte = 0; std::bitset<256> set; std::string seq; };
    ClassItem parseClassItem() {
        ClassItem it;
        char c = p_[i_++];
        unsigned char lead = (unsigned char)c;
        if (lead >= 0xC2 && lead <= 0xF4) {                   // a UTF-8 character written in the pattern
            size_t len = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
            if (i_ - 1 + len <= p_.size()) {
                bool ok = true;
                for (size_t k = 1; k < len; ++k) ok = ok && ((unsigned char)p_[i_ - 1 + k] & 0xC0) == 0x80;
                if (ok) { it.seq = p_.substr(i_ - 1, len); i_ += len - 1; return it; }
            }
        }
        if (c != '\\') { it.byte = (unsigned char)c; return it; }
        if (!more()) throw SyntaxError("trailing backslash");
        char e = p_[i_++];
        switch (e) {
            case 'd': it.isSet = true; it.set = digitSet();  return it;
            case 'D': it.isSet = true; it.set = ~digitSet(); return it;
            case 'w': it.isSet = true; it.set = wordSet();   return it;
            case 'W': it.isSet = true; it.set = ~wordSet();  return it;
            case 's': it.isSet = true; it.set = spaceSet();  return it;
            case 'S': it.isSet = true; it.set = ~spaceSet(); return it;
            case 'b': it.byte = 8; return it;
            case 'n': it.byte = '\n'; return it;
            case 'r': it.byte = '\r'; return it;
            case 't': it.byte = '\t'; return it;
            case 'f': it.byte = '\f'; return it;
            case 'v': it.byte = '\v'; return it;
            case '0': it.byte = 0; return it;
            case 'x': { unsigned v; it.byte = readHex(2, v) ? (int)v : 'x'; return it; }
            case 'u': {
                unsigned v;
                if (!readHex(4, v)) { it.byte = 'u'; return it; }
                if (v < 0x80) it.byte = (int)v;
                else it.seq = utf8(v);                        // UTF-8, the same as \u outside a class
                return it;
            }
            default:
                if (e >= '1' && e <= '9') throw Unsupported("backreference in class");
                it.byte = (unsigned char)e; return it;
        }
    }

    int parseClass() {
        bool negate = false;
        if (more() && peek() == '^') { negate = true; ++i_; }
        std::bitset<256> set;
        std::vector<std::string> seqs;                  // characters above 0x7f, as their UTF-8 bytes
        bool closed = false;
        // matching is byte-wise, so a character of several bytes can only be one more choice. these two can't be done right
        const char* multiByte = "a character above 0x7f can't be in a negated class or a range (matching is byte by byte)";
        while (more()) {
            if (peek() == ']') { ++i_; closed = true; break; }
            ClassItem lo = parseClassItem();
            if (!lo.seq.empty()) {
                if (negate || (i_ + 1 < p_.size() && peek() == '-' && p_[i_ + 1] != ']')) throw SyntaxError(multiByte);
                seqs.push_back(lo.seq);
                continue;
            }
            if (!lo.isSet && i_ + 1 < p_.size() && peek() == '-' && p_[i_ + 1] != ']') {
                ++i_;                                   // '-'
                ClassItem hi = parseClassItem();
                if (!hi.seq.empty()) throw SyntaxError(multiByte);
                if (hi.isSet) { set.set(lo.byte); set.set('-'); set |= hi.set; continue; }
                if (hi.byte < lo.byte) throw SyntaxError("class range out of order");
                for (int b = lo.byte; b <= hi.byte; ++b) set.set(b);
            } else if (lo.isSet) set |= lo.set;
            else set.set(lo.byte);
        }
        if (!closed) throw SyntaxError("missing ']'");
        if (negate) set = ~set;
        Node n; n.kind = Node::Class; n.c = newClass(set);
        if (seqs.empty()) return node(std::move(n));
        // [aé] is (?:[a]|é): the single bytes, or one of the longer characters
        Node alt; alt.kind = Node::Alt;
        if (set.any()) alt.kids.push_back(node(std::move(n)));
        for (const auto& sq : seqs) {
            Node cat; cat.kind = Node::Cat;
            for (unsigned char b : sq) cat.kids.push_back(charNode(b));
            alt.kids.push_back(node(std::move(cat)));
        }
        if (alt.kids.size() == 1) return alt.kids[0];
        return node(std::move(alt));
    }

    // ---- code generation ----
    int here() const { return (int)prog_->insts.size(); }
    int push(Inst in) {
        if (prog_->insts.size() >= kMaxInsts) throw SyntaxError("pattern too large once repetitions are expanded");
        prog_->insts.push_back(in);
        return (int)prog_->insts.size() - 1;
    }

    void emit(int ni) {
        // copy, not reference: nodes_ is not modified during emission, but keep it obviously safe
        const Node n = nodes_[ni];
        switch (n.kind) {
            case Node::Empty: break;
            case Node::Char:  push({Op::Char, n.c, 0}); break;
            case Node::Any:   push({Op::Any, 0, 0}); break;
            case Node::Class: push({Op::Class, n.c, 0}); break;
            case Node::Bol:   push({Op::Bol, 0, 0}); break;
            case Node::Eol:   push({Op::Eol, 0, 0}); break;
            case Node::WordB: push({Op::WordB, 0, 0}); break;
            case Node::NotWordB: push({Op::NotWordB, 0, 0}); break;
            case Node::Cat:   for (int k : n.kids) emit(k); break;
            case Node::Group:
                if (n.c >= 0) push({Op::Save, 2 * n.c, 0});
                emit(n.kids[0]);
                if (n.c >= 0) push({Op::Save, 2 * n.c + 1, 0});
                break;
            case Node::Alt: {
                // split L1, next ; L1: alt0 ; jmp end ; next: split ... ; last alt
                std::vector<int> jumps;
                for (size_t a = 0; a < n.kids.size(); ++a) {
                    if (a + 1 < n.kids.size()) {
                        int s = push({Op::Split, 0, 0});
                        prog_->insts[s].x = here();
                        emit(n.kids[a]);
                        jumps.push_back(push({Op::Jmp, 0, 0}));
                        prog_->insts[s].y = here();
                    } else emit(n.kids[a]);
                }
                for (int j : jumps) prog_->insts[j].x = here();
                break;
            }
            case Node::Rep: {
                int body = n.kids[0];
                for (int k = 0; k < n.min; ++k) emit(body);
                if (n.max == -1) {
                    int s = push({Op::Split, 0, 0});
                    int b0 = here();
                    emit(body);
                    push({Op::Jmp, s, 0});
                    int end = here();
                    if (n.greedy) { prog_->insts[s].x = b0; prog_->insts[s].y = end; }
                    else          { prog_->insts[s].x = end; prog_->insts[s].y = b0; }
                } else {
                    std::vector<int> splits;
                    for (int k = n.min; k < n.max; ++k) {
                        int s = push({Op::Split, 0, 0});
                        splits.push_back(s);
                        int b0 = here();
                        emit(body);
                        if (n.greedy) prog_->insts[s].x = b0; else prog_->insts[s].y = b0;
                    }
                    int end = here();
                    for (int s : splits) {
                        if (n.greedy) prog_->insts[s].y = end; else prog_->insts[s].x = end;
                    }
                }
                break;
            }
        }
    }
};

} // namespace detail

// multiline: ^ and $ also match right after and right before a line break, like the ECMAScript `m` flag
inline Prog compile(const std::string& pattern, bool multiline = false) { return detail::Parser(pattern).compile(multiline); }

// runs a compiled program over one subject; reusable across many search() calls (find_all / replace)
// so the per-search allocation cost is paid once
class Matcher {
public:
    Matcher(const Prog& pg, const std::string& s) : pg_(pg), s_(s), ncap_(pg.ncap()) {
        for (List* l : {&a_, &b_}) {
            l->caps.assign(pg.insts.size() * (size_t)ncap_, -1);
            l->mark.assign(pg.insts.size(), 0);
        }
        work_.assign(ncap_, -1);
    }

    // leftmost-first match starting at or after `from`. fills caps (size ncap: start/end pairs, -1 for an unmatched
    // group) and returns true on success
    bool search(size_t from, std::vector<int>& caps) {
        const size_t n = s_.size();
        bool matched = false;
        List* cur = &a_;
        List* nxt = &b_;
        reset(*cur); reset(*nxt);
        for (size_t pos = from; pos <= n; ++pos) {
            if (!matched) {   // a new attempt starting here: lowest priority
                std::fill(work_.begin(), work_.end(), -1);
                addThread(*cur, 0, pos);
            }
            if (cur->pcs.empty()) {
                if (matched) break;
                // nothing alive: the next iteration adds a fresh start thread. the list's dedupe marks
                // belong to this position, so invalidate them (bump the generation) first
                reset(*cur);
                continue;
            }
            reset(*nxt);
            for (size_t t = 0; t < cur->pcs.size(); ++t) {
                int pc = cur->pcs[t];
                const Inst& in = pg_.insts[pc];
                const int* tc = &cur->caps[(size_t)pc * ncap_];
                bool step = false;
                switch (in.op) {
                    case Op::Char:  step = pos < n && (unsigned char)s_[pos] == (unsigned)in.x; break;
                    case Op::Any:   step = pos < n && s_[pos] != '\n' && s_[pos] != '\r'; break;
                    case Op::Class: step = pos < n && pg_.classes[in.x][(unsigned char)s_[pos]]; break;
                    case Op::Match:
                        caps.assign(tc, tc + ncap_);
                        matched = true;
                        t = cur->pcs.size();                  // drop every lower-priority thread
                        continue;
                    default: break;
                }
                if (step) {
                    std::copy(tc, tc + ncap_, work_.begin());
                    addThread(*nxt, pc + 1, pos + 1);
                }
            }
            std::swap(cur, nxt);
        }
        return matched;
    }

    int ncap() const { return ncap_; }

private:
    struct List {
        std::vector<int>      pcs;
        std::vector<int>      caps;     // insts * ncap, indexed by pc (at most one thread per pc per list)
        std::vector<uint32_t> mark;
        uint32_t              gen = 0;
    };
    const Prog&        pg_;
    const std::string& s_;
    int                ncap_;
    List               a_, b_;
    std::vector<int>   work_;
    struct Item { bool restore; int a; int b; };
    std::vector<Item>  stack_;

    static void reset(List& l) { l.pcs.clear(); ++l.gen; }

    static bool lineBreak(char c) { return c == '\n' || c == '\r'; }
    bool isWordAt(size_t pos) const { return pos < s_.size() && detail::isWordByte((unsigned char)s_[pos]); }

    // follows Jmp/Split/Save/assertions from `pc0` (using and updating work_ as the current captures),
    // adding every consuming instruction / Match it reaches to `l` in priority order. iterative: an
    // explicit stack with "restore capture slot" entries replaces the usual recursion.
    void addThread(List& l, int pc0, size_t pos) {
        stack_.clear();
        stack_.push_back({false, pc0, 0});
        while (!stack_.empty()) {
            Item it = stack_.back(); stack_.pop_back();
            if (it.restore) { work_[it.a] = it.b; continue; }
            int pc = it.a;
            if (l.mark[pc] == l.gen) continue;
            l.mark[pc] = l.gen;
            const Inst& in = pg_.insts[pc];
            switch (in.op) {
                case Op::Jmp:   stack_.push_back({false, in.x, 0}); break;
                case Op::Split: stack_.push_back({false, in.y, 0}); stack_.push_back({false, in.x, 0}); break;
                case Op::Save:
                    stack_.push_back({true, in.x, work_[in.x]});
                    work_[in.x] = (int)pos;
                    stack_.push_back({false, pc + 1, 0});
                    break;
                case Op::Bol:      if (pos == 0 || (pg_.multiline && lineBreak(s_[pos - 1])))            stack_.push_back({false, pc + 1, 0}); break;
                case Op::Eol:      if (pos == s_.size() || (pg_.multiline && lineBreak(s_[pos]))) stack_.push_back({false, pc + 1, 0}); break;
                case Op::WordB:    if (isWordAt(pos - 1) != isWordAt(pos)) stack_.push_back({false, pc + 1, 0}); break;
                case Op::NotWordB: if (isWordAt(pos - 1) == isWordAt(pos)) stack_.push_back({false, pc + 1, 0}); break;
                default: {   // Char, Any, Class, Match: a live thread
                    l.pcs.push_back(pc);
                    std::copy(work_.begin(), work_.end(), l.caps.begin() + (size_t)pc * ncap_);
                    break;
                }
            }
        }
    }
};

} // namespace rx

#endif // EMBR_REGEX_PIKE_H
