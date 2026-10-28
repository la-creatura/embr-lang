// differential test for plugins/regex/rx.c: on random patterns and subjects the Pike VM must agree with std::regex
// (ECMAScript grammar) on whether there is a match, where, and what every capture group holds. std::regex is only a
// reliable oracle on short subjects (it overflows the stack on long ones, which is why rx.c exists), so subjects
// here are tiny and long inputs are covered by tests/regex_test.cpp. the backtracking matcher (backreferences,
// lookahead) has a short list of cases with known answers at the end
//
// deterministic (fixed seed), so a failure can be reproduced. prints the first mismatches and exits nonzero
//
// half the patterns run with the multiline flag. some groups are named (?<nK>...): std::regex has no named groups, so
// the oracle gets the same pattern with plain ( ) groups, and the names must land on the right group numbers

#include "../plugins/regex/rx.h"
#include <cstdio>
#include <cstring>
#include <random>
#include <regex>

static std::mt19937_64 rng(12345);
static size_t rnd(size_t n) { return rng() % n; }
static int nextName = 0;   // names in the pattern being built: n0, n1, ...

static std::string genAtom(int depth);
static std::string genAlt(int depth, bool nonEmpty);

static std::string genAtom(int depth) {
    switch (rnd(depth > 0 ? 15 : 12)) {
        case 0: case 1: case 2: return std::string(1, "abc"[rnd(3)]);
        case 3: return ".";
        case 4: return "[ab]";
        case 5: return "[^a]";
        case 6: return "\\d";
        case 7: return "\\w";
        case 8: return "\\s";
        case 9: return std::string(1, "^$"[rnd(2)]);
        case 10: return rnd(2) ? "\\b" : "\\B";
        // escapes: the other shorthand classes, control characters, \x and \u (ASCII only: std::regex on char has no
        // UTF-8 view of a bigger code point), and classes with escapes and ranges inside
        case 11: {
            static const char* esc[] = {"\\D", "\\W", "\\S", "\\n", "\\t", "\\0", "\\r", "\\x61", "\\x09",
                                        "\\u0062", "\\-", "\\.", "\\["};
            return esc[rnd(sizeof esc / sizeof *esc)];
        }
        case 12: {
            static const char* cls[] = {"[\\d_]", "[^\\s]", "[a\\-c]", "[\\x61-c]", "[\\t ]", "[\\Wa]", "[^\\D1]",
                                        "[\\n\\r]", "[\\0-\\x09]", "[\\u0061b]", "[-a]", "[a-]", "[\\S\\s]"};
            return cls[rnd(sizeof cls / sizeof *cls)];
        }
        // group bodies always consume at least one byte (genAlt(..., true)): libstdc++ does not follow the
        // ECMAScript rule that rejects an empty loop iteration, so quantified NULLABLE groups like (a?)+
        // give different capture/extent results there even though this engine follows the spec. the
        // oracle is only trustworthy on loop bodies that cannot match empty.
        case 13: {
            std::string open = rnd(3) == 0 ? "(?<n" + std::to_string(nextName++) + ">" : "(";
            return open + genAlt(depth - 1, true) + ")";
        }
        default: return "(?:" + genAlt(depth - 1, true) + ")";
    }
}
static std::string genTerm(int depth) {
    std::string a = genAtom(depth);
    if (a == "^" || a == "$" || a == "\\b" || a == "\\B") return a;   // don't quantify assertions (std differs)
    static const char* q[] = {"", "", "", "*", "+", "?", "{2}", "{1,2}", "{0,1}", "*?", "+?", "??", "{1,3}?"};
    return a + q[rnd(13)];
}
static std::string genCat(int depth, bool nonEmpty) {
    std::string s;
    if (nonEmpty) {                                   // a mandatory, unquantified, byte-consuming first atom
        static const char* must[] = {"a", "b", "c", ".", "[ab]", "[^a]", "\\d", "\\w", "\\s"};
        s += must[rnd(9)];
    }
    size_t n = nonEmpty ? rnd(3) : 1 + rnd(3);
    for (size_t i = 0; i < n; ++i) s += genTerm(depth);
    return s;
}
static std::string genAlt(int depth, bool nonEmpty) {
    std::string s = genCat(depth, nonEmpty);
    if (rnd(4) == 0) s += "|" + genCat(depth, nonEmpty);
    return s;
}
static std::string genSubject() {
    static const char al[] = "aabbc 1_\n\r\t-\0";
    std::string s;
    size_t n = rnd(9);
    for (size_t i = 0; i < n; ++i) s += al[rnd(sizeof al - 1)];
    return s;
}

static std::string show(const std::string& s) {
    std::string o;
    for (char c : s) { if (c == '\n') o += "\\n"; else if (c == '\r') o += "\\r"; else o += c; }
    return o;
}

// patterns for the backtracking matcher with answers worked out by hand from the ECMAScript rules. e is the whole match
// then each group as start,end (-1,-1 for a group that did not take part), or empty when there is no match
struct BtCase { const char* pat; const char* subj; std::vector<int> e; };

static int backtrackCases() {
    const std::vector<BtCase> cases = {
        {"(a)\\1", "xaab", {1, 3, 1, 2}},
        {"(a+)b\\1", "aabaa", {0, 5, 0, 2}},
        {"(a)|b\\1", "b", {0, 1, -1, -1}},
        {"foo(?=bar)", "foobar", {0, 3}},
        {"foo(?!bar)", "foobar", {}},
        {"foo(?!bar)", "foobaz", {0, 3}},
        {"(?=(a+))a*b\\1", "baaabac", {3, 6, 3, 4}},
        {"(?<x>a)\\k<x>", "baab", {1, 3, 1, 2}},
        {"(a*)*(?=b)b", "aab", {0, 3, 0, 2}},
        {"(a|)*(?=b)b", "ab", {0, 2, 0, 1}},
        {"(?!^)a", "aa", {1, 2}},
        {"(.)(.)\\2\\1", "xabbay", {1, 5, 1, 2, 2, 3}},
        {"(?:(?=a)a|b)+c", "abac", {0, 4}},
        {"x*(?=)", "y", {0, 0}},
    };
    int bad = 0;
    for (const BtCase& c : cases) {
        char err[200];
        int code = 0;
        RxProg* prog = rx_compile(c.pat, std::strlen(c.pat), 0, err, sizeof err, &code);
        if (!prog) { std::printf("MISMATCH (backtracking) /%s/ rejected: %s\n", c.pat, err); ++bad; continue; }
        RxMatcher* m = rx_matcher_new(prog, c.subj, std::strlen(c.subj));
        std::vector<int> caps((size_t)(2 * (rx_ngroups(prog) + 1)), -7);
        int r = rx_search(m, 0, caps.data());
        std::vector<int> got = r == 1 ? caps : std::vector<int>();
        if (!rx_backtrack_reason(prog) || got != c.e) {
            std::printf("MISMATCH (backtracking) /%s/ on \"%s\": got %zu numbers, want %zu\n", c.pat, c.subj, got.size(), c.e.size());
            ++bad;
        }
        rx_matcher_free(m);
        rx_free(prog);
    }
    return bad;
}

int main() {
#ifndef __GLIBCXX__
    // the oracle is std::regex. libc++ (macos) handles \b differently, e.g. it finds one match of /\b/ in "bb"
    // where ECMAScript has two, so on that library a mismatch says nothing about the Pike engine
    std::puts("skipped: std::regex here is not libstdc++, so it can't act as the oracle");
    return 77;
#endif
#ifdef __SANITIZE_ADDRESS__
    const int N = 8000;    // the comparison is statistical, and sanitizer builds are several times slower
#else
    const int N = 40000;
#endif
    int compared = 0, skipped = 0, mismatches = 0, caps_only = 0;
    for (int it = 0; it < N; ++it) {
        nextName = 0;
        std::string pat = genAlt(2, false), subj = genSubject();
        const bool multiline = it % 2 == 1;
        std::string plain = std::regex_replace(pat, std::regex("\\(\\?<n\\d+>"), "(");
        std::regex ref;
        try { ref = std::regex(plain, multiline ? std::regex::ECMAScript | std::regex::multiline : std::regex::ECMAScript); }
        catch (const std::regex_error&) { ++skipped; continue; }
        char err[200];
        int code = 0;
        RxProg* prog = rx_compile(pat.data(), pat.size(), multiline, err, sizeof err, &code);
        if (!prog) {
            std::printf("MISMATCH (rx rejects a pattern std::regex accepts) /%s/: %s\n", show(pat).c_str(), err);
            if (++mismatches >= 25) break;
            continue;
        }
        if (rx_backtrack_reason(prog)) { rx_free(prog); ++skipped; continue; }
        // every name n<K> belongs to the group that opens at the same place in the pattern
        {
            int group = 0;
            bool namesOk = true;
            for (size_t i = 0; namesOk && i < pat.size(); ++i) {
                if (pat[i] == '\\') { ++i; continue; }
                if (pat[i] != '(' || pat.compare(i, 3, "(?:") == 0) continue;
                ++group;
                std::string want = pat.compare(i, 3, "(?<") == 0 ? pat.substr(i + 3, pat.find('>', i) - i - 3) : "";
                if (group > rx_ngroups(prog) || want != rx_group_name(prog, group)) namesOk = false;
            }
            if (!namesOk) {
                std::printf("MISMATCH (group names) /%s/\n", show(pat).c_str());
                if (++mismatches >= 25) break;
            }
        }
        RxMatcher* m = rx_matcher_new(prog, subj.data(), subj.size());
        std::vector<int> caps((size_t)(2 * (rx_ngroups(prog) + 1)));
        bool got = rx_search(m, 0, caps.data()) == 1;
        std::smatch sm;
        bool want = std::regex_search(subj, sm, ref);
        ++compared;
        bool ok = (got == want);
        bool capsOk = true;
        if (ok && got) {
            for (size_t g = 0; g < sm.size() && g * 2 + 1 < caps.size(); ++g) {
                int s = sm[g].matched ? (int)(sm[g].first - subj.begin()) : -1;
                int e = sm[g].matched ? (int)(sm[g].second - subj.begin()) : -1;
                if (caps[2 * g] != s || caps[2 * g + 1] != e) capsOk = false;
            }
        }
        // the iteration the plugin's re_find_all / re_replace use (every non-overlapping match; the next search starts
        // where the last match ended, one byte further after an empty one, like JavaScript) must give the same list of
        // matches as that loop over std::regex_search
        {
            std::vector<std::pair<int,int>> mine, theirs;
            size_t pos = 0;
            std::vector<int> c2(caps.size());
            // same loop as next_match() in plugins/regex/regex.c
            while (pos <= subj.size() && rx_search(m, pos, c2.data()) == 1) {
                mine.push_back({c2[0], c2[1]});
                pos = (size_t)c2[1] + (c2[1] == c2[0] ? 1 : 0);
            }
            pos = 0;
            std::smatch sm2;
            while (pos <= subj.size()) {
                auto fl = pos ? std::regex_constants::match_prev_avail : std::regex_constants::match_default;
                if (!std::regex_search(subj.cbegin() + (std::ptrdiff_t)pos, subj.cend(), sm2, ref, fl)) break;
                int s0 = (int)(sm2[0].first - subj.cbegin()), e0 = (int)(sm2[0].second - subj.cbegin());
                theirs.push_back({s0, e0});
                pos = (size_t)e0 + (e0 == s0 ? 1 : 0);
            }
            if (mine != theirs) {
                std::printf("MISMATCH (find_all iteration) /%s/ on \"%s\": rx %zu matches, std %zu\n",
                            show(pat).c_str(), show(subj).c_str(), mine.size(), theirs.size());
                if (++mismatches >= 25) break;
            }
        }
        if (!ok || !capsOk) {
            if (ok) ++caps_only;
            std::printf("MISMATCH%s /%s/%s on \"%s\": rx=%s std=%s\n", ok ? " (captures)" : "", show(pat).c_str(), multiline ? "m" : "",
                        show(subj).c_str(), got ? "match" : "none", want ? "match" : "none");
            if (got && want) {
                std::printf("   rx :");
                for (size_t g = 0; g * 2 + 1 < caps.size(); ++g) std::printf(" [%d,%d)", caps[2*g], caps[2*g+1]);
                std::printf("\n   std:");
                for (size_t g = 0; g < sm.size(); ++g)
                    std::printf(" [%d,%d)", sm[g].matched ? (int)(sm[g].first - subj.begin()) : -1,
                                sm[g].matched ? (int)(sm[g].second - subj.begin()) : -1);
                std::printf("\n");
            }
            if (++mismatches >= 25) break;
        }
        rx_matcher_free(m);
        rx_free(prog);
    }
    mismatches += backtrackCases();
    std::printf("regex engine differential: %d compared, %d skipped (invalid/unsupported), %d mismatches (%d captures-only)\n",
                compared, skipped, mismatches, caps_only);
    return mismatches ? 1 : 0;
}
