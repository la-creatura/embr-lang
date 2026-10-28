/* rx.c
 * the regex engine in plain C: parser, compiler, Pike VM and a bounded backtracking matcher. see rx.h.
 *
 * good to know
 *   - captures inside a quantified group keep their last value (they are not reset on each round)
 *   - an error in the parser is a longjmp to rx_compile, which frees everything the parser owns
 */
#include "rx.h"

#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint64_t w[4]; } Set;

static int  set_has(const Set* s, unsigned c) { return (int)((s->w[c >> 6] >> (c & 63)) & 1u); }
static void set_add(Set* s, unsigned c) { s->w[c >> 6] |= (uint64_t)1 << (c & 63); }
static void set_or(Set* a, const Set* b) { for (int i = 0; i < 4; ++i) a->w[i] |= b->w[i]; }
static Set  set_not(Set a) { for (int i = 0; i < 4; ++i) a.w[i] = ~a.w[i]; return a; }

static int is_word_byte(unsigned c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}
static Set digit_set(void) { Set s = {{0, 0, 0, 0}}; for (unsigned c = '0'; c <= '9'; ++c) set_add(&s, c); return s; }
static Set word_set(void)  { Set s = {{0, 0, 0, 0}}; for (unsigned c = 0; c < 256; ++c) if (is_word_byte(c)) set_add(&s, c); return s; }
static Set space_set(void) { Set s = {{0, 0, 0, 0}}; for (const char* q = " \t\n\v\f\r"; *q; ++q) set_add(&s, (unsigned char)*q); return s; }

/* ---- the program ---- */

enum { OP_CHAR, OP_ANY, OP_CLASS, OP_SPLIT, OP_JMP, OP_SAVE, OP_BOL, OP_EOL, OP_WORDB, OP_NOTWORDB, OP_MATCH,
       /* only in programs for the backtracking matcher */
       OP_BACKREF, OP_LOOK, OP_LOOKEND, OP_REPSTART, OP_REPEND };

typedef struct { uint8_t op; int x, y; } Inst;

struct RxProg {
    Inst*  insts;   size_t ninsts;
    Set*   classes; size_t nclasses;
    int    ngroups;
    char** names;                 /* names[g], "" when group g has none (ngroups + 1 entries) */
    int    multiline;
    int    bt;                    /* needs the backtracking matcher */
    int    nslots;                /* capture slots + loop registers (backtracking only) */
    char   why[32];
};

int rx_ngroups(const RxProg* p) { return p->ngroups; }
const char* rx_group_name(const RxProg* p, int g) { return p->names[g]; }
const char* rx_backtrack_reason(const RxProg* p) { return p->bt ? p->why : NULL; }

void rx_free(RxProg* p) {
    if (!p) return;
    if (p->names) for (int g = 0; g <= p->ngroups; ++g) free(p->names[g]);
    free(p->names);
    free(p->insts);
    free(p->classes);
    free(p);
}

/* ---- the parser ---- */

enum { N_EMPTY, N_CHAR, N_ANY, N_CLASS, N_CAT, N_ALT, N_REP, N_GROUP, N_BOL, N_EOL, N_WORDB, N_NOTWORDB, N_LOOK, N_BACKREF, N_NAMEREF };

typedef struct {
    int kind;
    int c;            /* CHAR: byte, CLASS: class index, GROUP: capture index (-1: none), LOOK: 1 when negative, BACKREF: group, NAMEREF: index into refnames */
    int min, max;     /* REP bounds, max -1 is unbounded */
    int greedy;
    int kid0, nk;     /* the kids are pool[kid0 .. kid0 + nk) */
} Node;

typedef struct { unsigned char b[4]; int n; } Seq;

typedef struct {
    const char* p; size_t n, i;
    Node* nodes;   size_t nnodes, nodescap;
    int*  pool;    size_t npool, poolcap;
    int*  tmp;     size_t ntmp, tmpcap;          /* kids being collected, a stack shared by the recursive calls */
    Set*  classes; size_t nclasses, classescap;
    Seq*  seqs;    size_t nseqs, seqscap;
    char** names;  size_t nnames, namescap;      /* group names, index = group number */
    char** refnames; size_t nrefnames, refcap;   /* names used by \k<name> */
    int   ngroups;
    Inst* insts;   size_t ninsts, instscap;
    int   bt; char why[32];
    int   nregs;
    int   multiline;
    jmp_buf jb;
    char  err[160];
    int   code;
} P;

#if defined(__GNUC__)
__attribute__((noreturn))
#endif
static void fail(P* p, int code, const char* msg) {
    snprintf(p->err, sizeof p->err, "%s", msg);
    p->code = code;
    longjmp(p->jb, 1);
}

static void* grow(P* p, void* ptr, size_t* cap, size_t need, size_t esz) {
    if (need <= *cap) return ptr;
    size_t nc = *cap ? *cap * 2 : 16;
    while (nc < need) nc *= 2;
    void* np = realloc(ptr, nc * esz);
    if (!np) fail(p, RX_NOMEM, "out of memory");
    *cap = nc;
    return np;
}

static int mk_node(P* p, Node n) {
    p->nodes = grow(p, p->nodes, &p->nodescap, p->nnodes + 1, sizeof(Node));
    p->nodes[p->nnodes] = n;
    return (int)p->nnodes++;
}
static Node blank(int kind) { Node n; memset(&n, 0, sizeof n); n.kind = kind; n.greedy = 1; return n; }

static int mk_with_kids(P* p, int kind, const int* kids, int k) {
    Node n = blank(kind);
    p->pool = grow(p, p->pool, &p->poolcap, p->npool + (size_t)k, sizeof(int));
    n.kid0 = (int)p->npool;
    n.nk = k;
    for (int j = 0; j < k; ++j) p->pool[p->npool++] = kids[j];
    return mk_node(p, n);
}
static void tmp_push(P* p, int v) {
    p->tmp = grow(p, p->tmp, &p->tmpcap, p->ntmp + 1, sizeof(int));
    p->tmp[p->ntmp++] = v;
}
static int new_class(P* p, Set s) {
    p->classes = grow(p, p->classes, &p->classescap, p->nclasses + 1, sizeof(Set));
    p->classes[p->nclasses] = s;
    return (int)p->nclasses++;
}
static int char_node(P* p, int byte) { Node n = blank(N_CHAR); n.c = byte & 0xFF; return mk_node(p, n); }
static int class_node(P* p, Set s) { Node n = blank(N_CLASS); n.c = new_class(p, s); return mk_node(p, n); }

static int more(const P* p) { return p->i < p->n; }
static char peek(const P* p) { return p->p[p->i]; }

static int parse_alt(P* p, int depth);

static char* dup_range(P* p, const char* s, size_t n) {
    char* d = malloc(n + 1);
    if (!d) fail(p, RX_NOMEM, "out of memory");
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}

/* a group name up to the '>' (the '<' is already read) */
static char* read_name(P* p) {
    size_t start = p->i;
    while (more(p) && peek(p) != '>') {
        char c = peek(p);
        int ok = c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (p->i > start && c >= '0' && c <= '9');
        if (!ok) fail(p, RX_SYNTAX, "invalid group name");
        ++p->i;
    }
    if (!more(p) || p->i == start) fail(p, RX_SYNTAX, "invalid group name");
    char* name = dup_range(p, p->p + start, p->i - start);
    ++p->i;   /* the '>' */
    return name;
}

static int named_group(P* p) {
    char* name = read_name(p);
    for (size_t g = 1; g < p->nnames; ++g)
        if (p->names[g] && strcmp(p->names[g], name) == 0) { free(name); fail(p, RX_SYNTAX, "duplicate group name"); }
    int cap = ++p->ngroups;
    p->names = grow(p, p->names, &p->namescap, (size_t)cap + 1, sizeof(char*));
    while (p->nnames < (size_t)cap) p->names[p->nnames++] = NULL;
    p->names[p->nnames++] = name;
    return cap;
}

static void need_group_slot(P* p, int cap) {
    p->names = grow(p, p->names, &p->namescap, (size_t)cap + 1, sizeof(char*));
    while (p->nnames <= (size_t)cap) p->names[p->nnames++] = NULL;
}

/* {n} {n,} {n,m}: returns 0 (consuming nothing) when it is not a valid quantifier, so the '{' is a literal */
static int parse_braces(P* p, int* mn, int* mx) {
    size_t j = p->i + 1;
    long a = 0, b = -1;
    {   /* first number */
        long v = 0; int any = 0;
        while (j < p->n && p->p[j] >= '0' && p->p[j] <= '9') { v = v * 10 + (p->p[j++] - '0'); if (v > 100000) v = 100000; any = 1; }
        if (!any) return 0;
        a = v;
    }
    if (j < p->n && p->p[j] == ',') {
        ++j;
        if (j < p->n && p->p[j] == '}') b = -1;
        else {
            long v = 0; int any = 0;
            while (j < p->n && p->p[j] >= '0' && p->p[j] <= '9') { v = v * 10 + (p->p[j++] - '0'); if (v > 100000) v = 100000; any = 1; }
            if (!any) return 0;
            b = v;
        }
    } else b = a;
    if (j >= p->n || p->p[j] != '}') return 0;
    if (a > RX_MAX_REPEAT || b > RX_MAX_REPEAT) fail(p, RX_SYNTAX, "quantifier too large (max 1000)");
    if (b != -1 && a > b) fail(p, RX_SYNTAX, "quantifier range out of order");
    *mn = (int)a; *mx = (int)b;
    p->i = j + 1;
    return 1;
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static int read_hex(P* p, int digits, unsigned* out) {
    if (p->i + (size_t)digits > p->n) return 0;
    unsigned v = 0;
    for (int k = 0; k < digits; ++k) {
        int h = hex_val(p->p[p->i + (size_t)k]);
        if (h < 0) return 0;
        v = v * 16 + (unsigned)h;
    }
    *out = v; p->i += (size_t)digits;
    return 1;
}
static int utf8_of(unsigned cp, unsigned char* b) {
    if (cp < 0x80) { b[0] = (unsigned char)cp; return 1; }
    if (cp < 0x800) { b[0] = (unsigned char)(0xC0 | (cp >> 6)); b[1] = (unsigned char)(0x80 | (cp & 0x3F)); return 2; }
    b[0] = (unsigned char)(0xE0 | (cp >> 12)); b[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F)); b[2] = (unsigned char)(0x80 | (cp & 0x3F));
    return 3;
}

static int seq_node(P* p, const unsigned char* b, int n) {
    if (n == 1) return char_node(p, b[0]);
    size_t base = p->ntmp;
    for (int k = 0; k < n; ++k) tmp_push(p, char_node(p, b[k]));
    int r = mk_with_kids(p, N_CAT, p->tmp + base, n);
    p->ntmp = base;
    return r;
}

static int parse_escape(P* p) {
    if (!more(p)) fail(p, RX_SYNTAX, "trailing backslash");
    char c = p->p[p->i++];
    switch (c) {
        case 'd': return class_node(p, digit_set());
        case 'D': return class_node(p, set_not(digit_set()));
        case 'w': return class_node(p, word_set());
        case 'W': return class_node(p, set_not(word_set()));
        case 's': return class_node(p, space_set());
        case 'S': return class_node(p, set_not(space_set()));
        case 'b': return mk_node(p, blank(N_WORDB));
        case 'B': return mk_node(p, blank(N_NOTWORDB));
        case 'n': return char_node(p, '\n');
        case 'r': return char_node(p, '\r');
        case 't': return char_node(p, '\t');
        case 'f': return char_node(p, '\f');
        case 'v': return char_node(p, '\v');
        case '0': return char_node(p, 0);
        case 'x': { unsigned v; if (read_hex(p, 2, &v)) return char_node(p, (int)v); return char_node(p, 'x'); }
        case 'u': {
            unsigned v; unsigned char b[4];
            if (!read_hex(p, 4, &v)) return char_node(p, 'u');
            return seq_node(p, b, utf8_of(v, b));
        }
        case 'c': return char_node(p, 'c');
        case 'k':
            if (more(p) && peek(p) == '<') {
                ++p->i;
                char* name = read_name(p);
                p->refnames = grow(p, p->refnames, &p->refcap, p->nrefnames + 1, sizeof(char*));
                p->refnames[p->nrefnames] = name;
                Node n = blank(N_NAMEREF); n.c = (int)p->nrefnames++;
                p->bt = 1; snprintf(p->why, sizeof p->why, "named backreference");
                return mk_node(p, n);
            }
            return char_node(p, 'k');
        default:
            if (c >= '1' && c <= '9') {
                long v = c - '0';
                while (more(p) && peek(p) >= '0' && peek(p) <= '9' && v < 100000) v = v * 10 + (p->p[p->i++] - '0');
                Node n = blank(N_BACKREF); n.c = (int)v;
                p->bt = 1; snprintf(p->why, sizeof p->why, "backreference");
                return mk_node(p, n);
            }
            return char_node(p, (unsigned char)c);
    }
}

/* one class member: a byte, a set (\d), or a character above 0x7f as its UTF-8 bytes */
typedef struct { int is_set; int byte; Set set; Seq seq; } Item;

static Item parse_class_item(P* p) {
    Item it; memset(&it, 0, sizeof it);
    char c = p->p[p->i++];
    unsigned char lead = (unsigned char)c;
    if (lead >= 0xC2 && lead <= 0xF4) {
        size_t len = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
        if (p->i - 1 + len <= p->n) {
            int ok = 1;
            for (size_t k = 1; k < len; ++k) ok = ok && ((unsigned char)p->p[p->i - 1 + k] & 0xC0) == 0x80;
            if (ok) {
                for (size_t k = 0; k < len; ++k) it.seq.b[k] = (unsigned char)p->p[p->i - 1 + k];
                it.seq.n = (int)len;
                p->i += len - 1;
                return it;
            }
        }
    }
    if (c != '\\') { it.byte = lead; return it; }
    if (!more(p)) fail(p, RX_SYNTAX, "trailing backslash");
    char e = p->p[p->i++];
    switch (e) {
        case 'd': it.is_set = 1; it.set = digit_set(); return it;
        case 'D': it.is_set = 1; it.set = set_not(digit_set()); return it;
        case 'w': it.is_set = 1; it.set = word_set(); return it;
        case 'W': it.is_set = 1; it.set = set_not(word_set()); return it;
        case 's': it.is_set = 1; it.set = space_set(); return it;
        case 'S': it.is_set = 1; it.set = set_not(space_set()); return it;
        case 'b': it.byte = 8; return it;
        case 'n': it.byte = '\n'; return it;
        case 'r': it.byte = '\r'; return it;
        case 't': it.byte = '\t'; return it;
        case 'f': it.byte = '\f'; return it;
        case 'v': it.byte = '\v'; return it;
        case '0': it.byte = 0; return it;
        case 'x': { unsigned v; it.byte = read_hex(p, 2, &v) ? (int)v : 'x'; return it; }
        case 'u': {
            unsigned v;
            if (!read_hex(p, 4, &v)) { it.byte = 'u'; return it; }
            if (v < 0x80) it.byte = (int)v;
            else it.seq.n = utf8_of(v, it.seq.b);
            return it;
        }
        default:
            if (e >= '1' && e <= '9') fail(p, RX_SYNTAX, "backreference in class");
            it.byte = (unsigned char)e; return it;
    }
}

static int parse_class(P* p) {
    int negate = 0;
    if (more(p) && peek(p) == '^') { negate = 1; ++p->i; }
    Set set = {{0, 0, 0, 0}};
    int any = 0;
    int closed = 0;
    p->nseqs = 0;
    const char* multi = "a character above 0x7f can't be in a negated class or a range (matching is byte by byte)";
    while (more(p)) {
        if (peek(p) == ']') { ++p->i; closed = 1; break; }
        Item lo = parse_class_item(p);
        int dash = p->i + 1 < p->n && peek(p) == '-' && p->p[p->i + 1] != ']';
        if (lo.seq.n) {
            if (negate || dash) fail(p, RX_SYNTAX, multi);
            p->seqs = grow(p, p->seqs, &p->seqscap, p->nseqs + 1, sizeof(Seq));
            p->seqs[p->nseqs++] = lo.seq;
            continue;
        }
        if (!lo.is_set && dash) {
            ++p->i;
            Item hi = parse_class_item(p);
            if (hi.seq.n) fail(p, RX_SYNTAX, multi);
            if (hi.is_set) { set_add(&set, (unsigned)lo.byte); set_add(&set, '-'); set_or(&set, &hi.set); continue; }
            if (hi.byte < lo.byte) fail(p, RX_SYNTAX, "class range out of order");
            for (int b = lo.byte; b <= hi.byte; ++b) set_add(&set, (unsigned)b);
        } else if (lo.is_set) set_or(&set, &lo.set);
        else set_add(&set, (unsigned)lo.byte);
    }
    if (!closed) fail(p, RX_SYNTAX, "missing ']'");
    if (negate) set = set_not(set);
    for (int i = 0; i < 4; ++i) if (set.w[i]) any = 1;
    if (p->nseqs == 0) return class_node(p, set);
    /* [aé] is (?:[a]|é): the single bytes, or one of the longer characters */
    size_t base = p->ntmp;
    if (any) tmp_push(p, class_node(p, set));
    for (size_t k = 0; k < p->nseqs; ++k) {
        Seq sq = p->seqs[k];
        tmp_push(p, seq_node(p, sq.b, sq.n));
    }
    int k = (int)(p->ntmp - base);
    int r = k == 1 ? p->tmp[base] : mk_with_kids(p, N_ALT, p->tmp + base, k);
    p->ntmp = base;
    return r;
}

static int parse_atom(P* p, int depth) {
    char c = p->p[p->i++];
    switch (c) {
        case '(': {
            int cap = -1;
            int look = 0, neg = 0;
            if (more(p) && peek(p) == '?') {
                ++p->i;
                if (!more(p)) fail(p, RX_SYNTAX, "invalid group");
                char k = p->p[p->i++];
                if (k == ':') { /* non-capturing */ }
                else if (k == '=' || k == '!') {
                    look = 1; neg = k == '!';
                    p->bt = 1; snprintf(p->why, sizeof p->why, "lookahead");
                }
                else if (k == '<' && more(p) && (peek(p) == '=' || peek(p) == '!')) fail(p, RX_SYNTAX, "lookbehind is not supported");
                else if (k == '<') cap = named_group(p);
                else fail(p, RX_SYNTAX, "invalid group syntax");
            } else {
                cap = ++p->ngroups;
                need_group_slot(p, cap);
            }
            int body = parse_alt(p, depth + 1);
            if (!more(p) || peek(p) != ')') fail(p, RX_SYNTAX, "unbalanced '('");
            ++p->i;
            if (look) {
                int r = mk_with_kids(p, N_LOOK, &body, 1);
                p->nodes[r].c = neg;
                return r;
            }
            int r = mk_with_kids(p, N_GROUP, &body, 1);
            p->nodes[r].c = cap;
            return r;
        }
        case '[': return parse_class(p);
        case '.': return mk_node(p, blank(N_ANY));
        case '^': return mk_node(p, blank(N_BOL));
        case '$': return mk_node(p, blank(N_EOL));
        case '*': case '+': case '?': fail(p, RX_SYNTAX, "nothing to repeat"); return 0;
        case '\\': return parse_escape(p);
        default: return char_node(p, (unsigned char)c);
    }
}

static int parse_term(P* p, int depth) {
    int atom = parse_atom(p, depth);
    if (!more(p)) return atom;
    int mn = 0, mx = 0;
    char c = peek(p);
    if (c == '*')      { mn = 0; mx = -1; ++p->i; }
    else if (c == '+') { mn = 1; mx = -1; ++p->i; }
    else if (c == '?') { mn = 0; mx = 1;  ++p->i; }
    else if (c == '{' && parse_braces(p, &mn, &mx)) { /* consumed */ }
    else return atom;
    int r = mk_with_kids(p, N_REP, &atom, 1);
    p->nodes[r].min = mn; p->nodes[r].max = mx;
    if (more(p) && peek(p) == '?') { p->nodes[r].greedy = 0; ++p->i; }
    return r;
}

static int parse_cat(P* p, int depth) {
    size_t base = p->ntmp;
    while (more(p) && peek(p) != '|' && peek(p) != ')') tmp_push(p, parse_term(p, depth));
    int k = (int)(p->ntmp - base);
    int r;
    if (k == 0) r = mk_node(p, blank(N_EMPTY));
    else if (k == 1) r = p->tmp[base];
    else r = mk_with_kids(p, N_CAT, p->tmp + base, k);
    p->ntmp = base;
    return r;
}

static int parse_alt(P* p, int depth) {
    if (depth > RX_MAX_DEPTH) fail(p, RX_SYNTAX, "pattern nested too deeply");
    size_t base = p->ntmp;
    tmp_push(p, parse_cat(p, depth));
    while (more(p) && peek(p) == '|') { ++p->i; tmp_push(p, parse_cat(p, depth)); }
    int k = (int)(p->ntmp - base);
    int r = k == 1 ? p->tmp[base] : mk_with_kids(p, N_ALT, p->tmp + base, k);
    p->ntmp = base;
    return r;
}

/* ---- code generation ---- */

static int here(const P* p) { return (int)p->ninsts; }
static int push(P* p, int op, int x, int y) {
    if (p->ninsts >= RX_MAX_INSTS) fail(p, RX_SYNTAX, "pattern too large once repetitions are expanded");
    p->insts = grow(p, p->insts, &p->instscap, p->ninsts + 1, sizeof(Inst));
    Inst in; in.op = (uint8_t)op; in.x = x; in.y = y;
    p->insts[p->ninsts] = in;
    return (int)p->ninsts++;
}

static int nullable(const P* p, int ni) {
    const Node* n = &p->nodes[ni];
    switch (n->kind) {
        case N_CHAR: case N_ANY: case N_CLASS: return 0;
        case N_CAT: for (int k = 0; k < n->nk; ++k) if (!nullable(p, p->pool[n->kid0 + k])) return 0; return 1;
        case N_ALT: for (int k = 0; k < n->nk; ++k) if (nullable(p, p->pool[n->kid0 + k])) return 1; return 0;
        case N_REP: return n->min == 0 || nullable(p, p->pool[n->kid0]);
        case N_GROUP: return nullable(p, p->pool[n->kid0]);
        default: return 1;
    }
}

static void emit(P* p, int ni) {
    Node n = p->nodes[ni];
    const int* kids = p->pool + n.kid0;   /* the pool does not grow while emitting */
    switch (n.kind) {
        case N_EMPTY: break;
        case N_CHAR: push(p, OP_CHAR, n.c, 0); break;
        case N_ANY: push(p, OP_ANY, 0, 0); break;
        case N_CLASS: push(p, OP_CLASS, n.c, 0); break;
        case N_BOL: push(p, OP_BOL, 0, 0); break;
        case N_EOL: push(p, OP_EOL, 0, 0); break;
        case N_WORDB: push(p, OP_WORDB, 0, 0); break;
        case N_NOTWORDB: push(p, OP_NOTWORDB, 0, 0); break;
        case N_BACKREF:
            if (n.c > p->ngroups) fail(p, RX_SYNTAX, "invalid back reference");
            push(p, OP_BACKREF, n.c, 0);
            break;
        case N_NAMEREF: {
            int g = -1;
            for (int k = 1; k <= p->ngroups; ++k)
                if (p->names[k] && strcmp(p->names[k], p->refnames[n.c]) == 0) g = k;
            if (g < 0) fail(p, RX_SYNTAX, "invalid named reference");
            push(p, OP_BACKREF, g, 0);
            break;
        }
        case N_LOOK: {
            int li = push(p, OP_LOOK, n.c, 0);
            emit(p, kids[0]);
            push(p, OP_LOOKEND, 0, 0);
            p->insts[li].y = here(p);
            break;
        }
        case N_CAT: for (int k = 0; k < n.nk; ++k) emit(p, kids[k]); break;
        case N_GROUP:
            if (n.c >= 0) push(p, OP_SAVE, 2 * n.c, 0);
            emit(p, kids[0]);
            if (n.c >= 0) push(p, OP_SAVE, 2 * n.c + 1, 0);
            break;
        case N_ALT: {
            /* split L1, next ; L1: alt0 ; jmp end ; next: split ... ; last alt */
            size_t base = p->ntmp;
            for (int a = 0; a < n.nk; ++a) {
                if (a + 1 < n.nk) {
                    int s = push(p, OP_SPLIT, 0, 0);
                    p->insts[s].x = here(p);
                    emit(p, kids[a]);
                    tmp_push(p, push(p, OP_JMP, 0, 0));
                    p->insts[s].y = here(p);
                } else emit(p, kids[a]);
            }
            for (size_t j = base; j < p->ntmp; ++j) p->insts[p->tmp[j]].x = here(p);
            p->ntmp = base;
            break;
        }
        case N_REP: {
            int body = kids[0];
            for (int k = 0; k < n.min; ++k) emit(p, body);
            if (n.max == -1) {
                /* a body that can match nothing must not loop on nothing: the backtracking matcher checks it */
                int guard = p->bt && nullable(p, body);
                int reg = guard ? 2 * (p->ngroups + 1) + p->nregs++ : 0;
                int s = push(p, OP_SPLIT, 0, 0);
                int b0 = here(p);
                if (guard) push(p, OP_REPSTART, reg, 0);
                emit(p, body);
                if (guard) push(p, OP_REPEND, reg, 0);
                push(p, OP_JMP, s, 0);
                int end = here(p);
                if (n.greedy) { p->insts[s].x = b0; p->insts[s].y = end; }
                else          { p->insts[s].x = end; p->insts[s].y = b0; }
            } else {
                size_t base = p->ntmp;
                for (int k = n.min; k < n.max; ++k) {
                    int s = push(p, OP_SPLIT, 0, 0);
                    tmp_push(p, s);
                    int b0 = here(p);
                    emit(p, body);
                    if (n.greedy) p->insts[s].x = b0; else p->insts[s].y = b0;
                }
                int end = here(p);
                for (size_t j = base; j < p->ntmp; ++j) {
                    if (n.greedy) p->insts[p->tmp[j]].y = end; else p->insts[p->tmp[j]].x = end;
                }
                p->ntmp = base;
            }
            break;
        }
    }
}

RxProg* rx_compile(const char* pat, size_t n, int multiline, char* err, size_t errcap, int* code) {
    P p;
    memset(&p, 0, sizeof p);
    p.p = pat; p.n = n; p.multiline = multiline;
    RxProg* volatile out = NULL;   /* volatile: it is set after setjmp */
    if (code) *code = RX_OK;
    if (setjmp(p.jb) == 0) {
        if (n > RX_MAX_PATTERN) fail(&p, RX_SYNTAX, "pattern too long (max 20000 bytes)");
        int root = parse_alt(&p, 0);
        if (p.i < p.n) fail(&p, RX_SYNTAX, "unbalanced ')'");
        need_group_slot(&p, p.ngroups);
        for (size_t g = 0; g <= (size_t)p.ngroups; ++g) if (!p.names[g]) p.names[g] = dup_range(&p, "", 0);
        push(&p, OP_SAVE, 0, 0);
        emit(&p, root);
        push(&p, OP_SAVE, 1, 0);
        push(&p, OP_MATCH, 0, 0);
        out = malloc(sizeof *out);
        if (!out) fail(&p, RX_NOMEM, "out of memory");
        memset(out, 0, sizeof *out);
        out->insts = p.insts; out->ninsts = p.ninsts; p.insts = NULL;
        out->classes = p.classes; out->nclasses = p.nclasses; p.classes = NULL;
        out->ngroups = p.ngroups;
        out->names = p.names; p.names = NULL; p.nnames = 0;
        out->multiline = multiline;
        out->bt = p.bt;
        out->nslots = 2 * (p.ngroups + 1) + p.nregs;
        memcpy(out->why, p.why, sizeof out->why);
    } else if (err && errcap) {
        snprintf(err, errcap, "%s", p.err);
        if (code) *code = p.code;
    }
    for (size_t k = 0; k < p.nnames; ++k) free(p.names ? p.names[k] : NULL);
    free(p.names);
    for (size_t k = 0; k < p.nrefnames; ++k) free(p.refnames[k]);
    free(p.refnames);
    free(p.nodes); free(p.pool); free(p.tmp); free(p.classes); free(p.seqs); free(p.insts);
    return out;
}

/* ---- matching ---- */

typedef struct { int restore; int a; int b; } Item2;
typedef struct { int* pcs; int npcs; int* caps; uint32_t* mark; uint32_t gen; } List;
typedef struct { int kind; int a; int b; } BtEntry;      /* kind 0: a branch to try (pc a at position b), 1: put slot a back to b */

struct RxMatcher {
    const RxProg* pg;
    const char* s; size_t n;
    int ncap;
    /* Pike VM */
    List l[2];
    int* work;
    Item2* stack; size_t stackcap;
    /* backtracking */
    int* sl; BtEntry* bs; size_t bn, bcap;
    long steps;
    int oom;
};

static int word_at(const RxMatcher* m, size_t pos) { return pos < m->n && is_word_byte((unsigned char)m->s[pos]); }
static int word_before(const RxMatcher* m, size_t pos) { return pos > 0 && is_word_byte((unsigned char)m->s[pos - 1]); }
static int line_break(char c) { return c == '\n' || c == '\r'; }

RxMatcher* rx_matcher_new(const RxProg* pg, const char* s, size_t n) {
    RxMatcher* m = calloc(1, sizeof *m);
    if (!m) return NULL;
    m->pg = pg; m->s = s; m->n = n; m->ncap = 2 * (pg->ngroups + 1);
    if (pg->bt) {
        m->sl = malloc(sizeof(int) * (size_t)pg->nslots);
        if (!m->sl) { free(m); return NULL; }
        return m;
    }
    size_t ni = pg->ninsts;
    m->work = malloc(sizeof(int) * (size_t)m->ncap);
    m->stackcap = 2 * ni + 4;
    m->stack = malloc(sizeof(Item2) * m->stackcap);
    int ok = m->work && m->stack;
    for (int k = 0; k < 2; ++k) {
        List* l = &m->l[k];
        l->pcs = malloc(sizeof(int) * ni);
        l->caps = malloc(sizeof(int) * ni * (size_t)m->ncap);
        l->mark = calloc(ni, sizeof(uint32_t));
        ok = ok && l->pcs && l->caps && l->mark;
    }
    if (!ok) { rx_matcher_free(m); return NULL; }
    return m;
}

void rx_matcher_free(RxMatcher* m) {
    if (!m) return;
    for (int k = 0; k < 2; ++k) { free(m->l[k].pcs); free(m->l[k].caps); free(m->l[k].mark); }
    free(m->work); free(m->stack); free(m->sl); free(m->bs);
    free(m);
}

/* ---- the Pike VM ---- */

static void list_reset(List* l) { l->npcs = 0; ++l->gen; }

/* follows Jmp/Split/Save/assertions from pc0 (work is the current captures), adding every consuming instruction and Match
   it reaches to l in priority order. an explicit stack with "put the slot back" entries stands in for recursion */
static void add_thread(RxMatcher* m, List* l, int pc0, size_t pos) {
    const RxProg* pg = m->pg;
    size_t sp = 0;
    m->stack[sp++] = (Item2){0, pc0, 0};
    while (sp) {
        Item2 it = m->stack[--sp];
        if (it.restore) { m->work[it.a] = it.b; continue; }
        int pc = it.a;
        if (l->mark[pc] == l->gen) continue;
        l->mark[pc] = l->gen;
        const Inst* in = &pg->insts[pc];
        switch (in->op) {
            case OP_JMP: m->stack[sp++] = (Item2){0, in->x, 0}; break;
            case OP_SPLIT: m->stack[sp++] = (Item2){0, in->y, 0}; m->stack[sp++] = (Item2){0, in->x, 0}; break;
            case OP_SAVE:
                m->stack[sp++] = (Item2){1, in->x, m->work[in->x]};
                m->work[in->x] = (int)pos;
                m->stack[sp++] = (Item2){0, pc + 1, 0};
                break;
            case OP_BOL:
                if (pos == 0 || (pg->multiline && line_break(m->s[pos - 1]))) m->stack[sp++] = (Item2){0, pc + 1, 0};
                break;
            case OP_EOL:
                if (pos == m->n || (pg->multiline && line_break(m->s[pos]))) m->stack[sp++] = (Item2){0, pc + 1, 0};
                break;
            case OP_WORDB:    if (word_before(m, pos) != word_at(m, pos)) m->stack[sp++] = (Item2){0, pc + 1, 0}; break;
            case OP_NOTWORDB: if (word_before(m, pos) == word_at(m, pos)) m->stack[sp++] = (Item2){0, pc + 1, 0}; break;
            default:   /* Char, Any, Class, Match: a live thread */
                l->pcs[l->npcs++] = pc;
                memcpy(l->caps + (size_t)pc * (size_t)m->ncap, m->work, sizeof(int) * (size_t)m->ncap);
                break;
        }
    }
}

static int pike_search(RxMatcher* m, size_t from, int* caps) {
    const RxProg* pg = m->pg;
    const size_t n = m->n;
    const int ncap = m->ncap;
    int matched = 0;
    List* cur = &m->l[0];
    List* nxt = &m->l[1];
    list_reset(cur); list_reset(nxt);
    for (size_t pos = from; pos <= n; ++pos) {
        if (!matched) {   /* a new attempt starting here: lowest priority */
            for (int k = 0; k < ncap; ++k) m->work[k] = -1;
            add_thread(m, cur, 0, pos);
        }
        if (cur->npcs == 0) {
            if (matched) break;
            list_reset(cur);
            continue;
        }
        list_reset(nxt);
        for (int t = 0; t < cur->npcs; ++t) {
            int pc = cur->pcs[t];
            const Inst* in = &pg->insts[pc];
            const int* tc = cur->caps + (size_t)pc * (size_t)ncap;
            int step = 0;
            switch (in->op) {
                case OP_CHAR:  step = pos < n && (unsigned char)m->s[pos] == (unsigned)in->x; break;
                case OP_ANY:   step = pos < n && m->s[pos] != '\n' && m->s[pos] != '\r'; break;
                case OP_CLASS: step = pos < n && set_has(&pg->classes[in->x], (unsigned char)m->s[pos]); break;
                case OP_MATCH:
                    memcpy(caps, tc, sizeof(int) * (size_t)ncap);
                    matched = 1;
                    t = cur->npcs;   /* drop every lower-priority thread */
                    continue;
                default: break;
            }
            if (step) {
                memcpy(m->work, tc, sizeof(int) * (size_t)ncap);
                add_thread(m, nxt, pc + 1, pos + 1);
            }
        }
        List* tmp = cur; cur = nxt; nxt = tmp;
    }
    return matched;
}

/* ---- the backtracking matcher ---- */

static int bt_push(RxMatcher* m, int kind, int a, int b) {
    if (m->bn == m->bcap) {
        size_t nc = m->bcap ? m->bcap * 2 : 256;
        BtEntry* nb = realloc(m->bs, nc * sizeof(BtEntry));
        if (!nb) { m->oom = 1; return 1; }
        m->bs = nb; m->bcap = nc;
    }
    m->bs[m->bn].kind = kind; m->bs[m->bn].a = a; m->bs[m->bn].b = b;
    ++m->bn;
    return 0;
}

/* runs from pc at pos until Match or LookEnd (returns 1, *end is where) or until every choice above `base` is spent
   (returns 0 and the stack is back at base). -1: gave up */
static int bt_run(RxMatcher* m, int pc, size_t pos, size_t base, size_t* end) {
    const RxProg* pg = m->pg;
    const size_t n = m->n;
    for (;;) {
        if (++m->steps > RX_MAX_STEPS) return -1;
        const Inst* in = &pg->insts[pc];
        int ok = 0;
        switch (in->op) {
            case OP_CHAR:  if (pos < n && (unsigned char)m->s[pos] == (unsigned)in->x) { ++pos; ++pc; ok = 1; } break;
            case OP_ANY:   if (pos < n && m->s[pos] != '\n' && m->s[pos] != '\r') { ++pos; ++pc; ok = 1; } break;
            case OP_CLASS: if (pos < n && set_has(&pg->classes[in->x], (unsigned char)m->s[pos])) { ++pos; ++pc; ok = 1; } break;
            case OP_JMP: pc = in->x; ok = 1; break;
            case OP_SPLIT:
                if (bt_push(m, 0, in->y, (int)pos)) return -1;
                pc = in->x; ok = 1;
                break;
            case OP_SAVE: case OP_REPSTART:
                if (bt_push(m, 1, in->x, m->sl[in->x])) return -1;
                m->sl[in->x] = (int)pos;
                ++pc; ok = 1;
                break;
            case OP_REPEND:
                if (m->sl[in->x] != (int)pos) { ++pc; ok = 1; }   /* an empty round ends the loop */
                break;
            case OP_BOL:
                if (pos == 0 || (pg->multiline && line_break(m->s[pos - 1]))) { ++pc; ok = 1; }
                break;
            case OP_EOL:
                if (pos == n || (pg->multiline && line_break(m->s[pos]))) { ++pc; ok = 1; }
                break;
            case OP_WORDB:    if (word_before(m, pos) != word_at(m, pos)) { ++pc; ok = 1; } break;
            case OP_NOTWORDB: if (word_before(m, pos) == word_at(m, pos)) { ++pc; ok = 1; } break;
            case OP_BACKREF: {
                int a = m->sl[2 * in->x], b = m->sl[2 * in->x + 1];
                if (a < 0 || b < a) { ++pc; ok = 1; break; }   /* a group that did not take part matches nothing */
                size_t len = (size_t)(b - a);
                if (pos + len <= n && memcmp(m->s + a, m->s + pos, len) == 0) { pos += len; ++pc; ok = 1; }
                break;
            }
            case OP_LOOK: {
                size_t top = m->bn, e2;
                int r = bt_run(m, pc + 1, pos, top, &e2);
                if (r < 0) return -1;
                if (in->x) {   /* negative: the body matching is a failure, and nothing it set may stay */
                    if (r) {
                        while (m->bn > top) { BtEntry b = m->bs[--m->bn]; if (b.kind == 1) m->sl[b.a] = b.b; }
                    } else { pc = in->y; ok = 1; }
                } else if (r) {   /* positive: the body is not tried again, but its captures can still be undone */
                    size_t w = top;
                    for (size_t k = top; k < m->bn; ++k) if (m->bs[k].kind == 1) m->bs[w++] = m->bs[k];
                    m->bn = w;
                    pc = in->y; ok = 1;
                }
                break;
            }
            case OP_MATCH: case OP_LOOKEND: *end = pos; return 1;
            default: break;
        }
        if (ok) continue;
        for (;;) {   /* fail: go back to the latest choice */
            if (m->bn <= base) return 0;
            BtEntry b = m->bs[--m->bn];
            if (b.kind == 1) { m->sl[b.a] = b.b; continue; }
            pc = b.a; pos = (size_t)b.b;
            break;
        }
    }
}

static int bt_search(RxMatcher* m, size_t from, int* caps) {
    const RxProg* pg = m->pg;
    for (size_t start = from; start <= m->n; ++start) {
        for (int k = 0; k < pg->nslots; ++k) m->sl[k] = -1;
        m->bn = 0;
        size_t end;
        int r = bt_run(m, 0, start, 0, &end);
        if (r < 0) return -1;
        if (r) { memcpy(caps, m->sl, sizeof(int) * (size_t)m->ncap); return 1; }
    }
    return 0;
}

int rx_search(RxMatcher* m, size_t from, int* caps) {
    if (m->pg->bt) { m->steps = 0; m->oom = 0; return bt_search(m, from, caps); }
    return pike_search(m, from, caps);
}
