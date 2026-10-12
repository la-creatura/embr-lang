/* csv.c
 * CSV parse/stringify for embr (RFC 4180 with a few documented leniencies). written in C against embr_plugin.h
 *
 * usage
 *   import "csv"
 *   rows = csv_parse("a,b\n1,\"x, y\"\n")      # [["a","b"],["1","x, y"]]
 *   recs = csv_parse_dicts("n,v\nA,1\nB,2\n")  # [{"n":"A","v":"1"}, {"n":"B","v":"2"}]
 *   print(csv_stringify([["a","b"],[1,"x,y"]]))
 *
 * rules
 *   - fields are always strings on parse (no type guessing)
 *   - a quoted field may contain the delimiter, newlines, and "" (an escaped quote)
 *   - line endings: \n, \r\n and a bare \r all end a record. a final record needs no terminator, and a trailing
 *     terminator does not add an empty record
 *   - completely blank lines are skipped (not a one-empty-field record)
 *   - a leading UTF-8 BOM is ignored
 *   - malformed input raises with "line L, column C": an unterminated quote, a quote in the middle of an unquoted
 *     field, or text after a closing quote
 *   - the optional delimiter is a single byte other than '"', '\r', '\n'
 *   - no recursion anywhere, so hostile input can't overflow the stack
 *
 * csv_stringify quotes only when needed (delimiter, quote, CR/LF, leading/trailing space, or a record that is one
 * empty cell), ends every record with "\n", and accepts str and number cells. any other cell type raises. rows and
 * cells can be arrays or vecs (a vec is read where it is)
 */
#include <embr_plugin.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- small growable buffers ---- */

typedef struct { char* p; size_t len, cap; } Buf;
typedef struct { size_t off, len; } Cell;

static int buf_reserve(Buf* b, size_t extra) {
    if (b->len + extra <= b->cap) return 0;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->len + extra) cap *= 2;
    char* np = realloc(b->p, cap);
    if (!np) return 1;
    b->p = np; b->cap = cap;
    return 0;
}
static int buf_add(Buf* b, const char* s, size_t n) {
    if (n == 0) return 0;   /* memcpy with a null buffer is undefined even for 0 bytes */
    if (buf_reserve(b, n)) return 1;
    memcpy(b->p + b->len, s, n);
    b->len += n;
    return 0;
}
static int buf_add_c(Buf* b, char c) { return buf_add(b, &c, 1); }

/* what a parse found, as flat tables: every field's bytes in `data`, `cells` says where, `row_end[r]` is the index one past row r's last cell */
typedef struct {
    Buf     data;
    Cell*   cells;  size_t ncells, cells_cap;
    size_t* row_end; size_t nrows, rows_cap;
} Table;

static void table_free(Table* t) { free(t->data.p); free(t->cells); free(t->row_end); }

static int push_cell(Table* t, size_t off, size_t len) {
    if (t->ncells == t->cells_cap) {
        size_t cap = t->cells_cap ? t->cells_cap * 2 : 64;
        Cell* np = realloc(t->cells, cap * sizeof *np);
        if (!np) return 1;
        t->cells = np; t->cells_cap = cap;
    }
    t->cells[t->ncells].off = off; t->cells[t->ncells].len = len; t->ncells++;
    return 0;
}
static int push_row(Table* t) {
    if (t->nrows == t->rows_cap) {
        size_t cap = t->rows_cap ? t->rows_cap * 2 : 32;
        size_t* np = realloc(t->row_end, cap * sizeof *np);
        if (!np) return 1;
        t->row_end = np; t->rows_cap = cap;
    }
    t->row_end[t->nrows++] = t->ncells;
    return 0;
}

static int oom(EmbrCtx* ctx) { return embrp_fail(ctx, "out of memory"); }

static int fail_at(EmbrCtx* ctx, const char* what, size_t line, size_t col) {
    char msg[128];
    snprintf(msg, sizeof msg, "%s at line %zu, column %zu", what, line, col);
    return embrp_fail(ctx, msg);
}

/* the delimiter: args[idx] when given, else a comma. returns 0, or 1 after failing */
static int delim_arg(EmbrCtx* ctx, const EmbrValue* args, size_t argc, size_t idx, char* out) {
    *out = ',';
    if (argc <= idx) return 0;
    size_t n;
    const char* d = embrp_str(ctx, &args[idx], &n);
    if (n != 1 || d[0] == '"' || d[0] == '\r' || d[0] == '\n')
        return embrp_fail(ctx, "delimiter must be a single character other than '\"', CR or LF");
    *out = d[0];
    return 0;
}

/* ---- parsing (iterative, one pass) ---- */

static int parse_csv(EmbrCtx* ctx, const unsigned char* s, size_t n, char delim, Table* t) {
    size_t i = 0;
    if (n >= 3 && s[0] == 0xEF && s[1] == 0xBB && s[2] == 0xBF) i = 3;
    size_t line = 1, line_start = i;
    int in_quotes = 0, after_quote = 0;
    int field_start = 1;    /* nothing consumed yet for the current field */
    int row_started = 0;    /* anything consumed on the current record (blank-line detection) */
    size_t qline = 0, qcol = 0;   /* where the open quote began */
    size_t field_off = t->data.len;

#define END_FIELD() do { \
        if (push_cell(t, field_off, t->data.len - field_off)) return oom(ctx); \
        field_off = t->data.len; field_start = 1; after_quote = 0; } while (0)
#define END_ROW() do { END_FIELD(); if (push_row(t)) return oom(ctx); row_started = 0; } while (0)
#define ADD(ch) do { if (buf_add_c(&t->data, (ch))) return oom(ctx); } while (0)

    while (i < n) {
        char c = (char)s[i];
        if (in_quotes) {
            if (c == '"') {
                if (i + 1 < n && s[i + 1] == '"') { ADD('"'); i += 2; continue; }
                in_quotes = 0; after_quote = 1; ++i; continue;
            }
            if (c == '\n') { ++line; line_start = i + 1; }
            ADD(c); ++i; continue;
        }
        if (c == '\r' || c == '\n') {
            size_t len = (c == '\r' && i + 1 < n && s[i + 1] == '\n') ? 2 : 1;
            if (row_started) END_ROW();          /* else: blank line, skipped */
            i += len; ++line; line_start = i; continue;
        }
        if (after_quote && c != delim)
            return fail_at(ctx, "unexpected character after closing quote", line, i - line_start + 1);
        if (c == delim) { END_FIELD(); row_started = 1; ++i; continue; }
        if (c == '"') {
            if (!field_start) return fail_at(ctx, "unexpected quote inside an unquoted field", line, i - line_start + 1);
            qline = line; qcol = i - line_start + 1;
            in_quotes = 1; field_start = 0; row_started = 1; ++i; continue;
        }
        ADD(c); field_start = 0; row_started = 1; ++i;
    }
    if (in_quotes) return fail_at(ctx, "unterminated quoted field opened", qline, qcol);
    if (row_started) END_ROW();
#undef END_FIELD
#undef END_ROW
#undef ADD
    return 0;
}

/* csv_parse(text: str, delimiter?: str) -> arr of arr of str */
static int csv_parse(EmbrCtx* ctx, const EmbrValue* args, size_t argc, EmbrValue* out) {
    char d;
    size_t n;
    if (delim_arg(ctx, args, argc, 1, &d)) return 1;
    const unsigned char* s = (const unsigned char*)embrp_str(ctx, &args[0], &n);
    Table t;
    memset(&t, 0, sizeof t);
    int rc = parse_csv(ctx, s, n, d, &t);
    if (!rc) {
        EmbrValue* row = embrp_temp(ctx);
        rc = embrp_set_array(ctx, out, t.nrows);
        size_t at = 0;
        for (size_t r = 0; !rc && r < t.nrows; ++r) {
            rc = embrp_set_array(ctx, row, t.row_end[r] - at);
            for (; !rc && at < t.row_end[r]; ++at)
                rc = embrp_array_push_str(ctx, row, t.data.p + t.cells[at].off, t.cells[at].len);
            if (!rc) rc = embrp_array_push(ctx, out, row);
        }
    }
    table_free(&t);
    return rc;
}

/* csv_parse_dicts(text: str, delimiter?: str) -> arr of map
   the first record is the header. every later record must have exactly as many fields as the header (raises with the
   record number otherwise). duplicate header names raise */
static int csv_parse_dicts(EmbrCtx* ctx, const EmbrValue* args, size_t argc, EmbrValue* out) {
    char d;
    size_t n;
    if (delim_arg(ctx, args, argc, 1, &d)) return 1;
    const unsigned char* s = (const unsigned char*)embrp_str(ctx, &args[0], &n);
    Table t;
    memset(&t, 0, sizeof t);
    int rc = parse_csv(ctx, s, n, d, &t);
    if (!rc) rc = embrp_set_array(ctx, out, t.nrows);
    if (!rc && t.nrows > 0) {
        const char* data = t.data.p;
        size_t hn = t.row_end[0];
        for (size_t i = 0; !rc && i < hn; ++i)
            for (size_t j = 0; !rc && j < i; ++j)
                if (t.cells[i].len == t.cells[j].len && memcmp(data + t.cells[i].off, data + t.cells[j].off, t.cells[i].len) == 0) {
                    static const char head[] = "duplicate header name '";
                    char* msg = malloc(sizeof head + t.cells[i].len + 2);
                    if (!msg) { rc = oom(ctx); break; }
                    memcpy(msg, head, sizeof head - 1);
                    memcpy(msg + sizeof head - 1, data + t.cells[i].off, t.cells[i].len);
                    msg[sizeof head - 1 + t.cells[i].len] = '\'';
                    msg[sizeof head + t.cells[i].len] = 0;
                    rc = embrp_fail(ctx, msg);   /* the error holds only the first 511 bytes, which is plenty */
                    free(msg);
                }
        EmbrValue* m = embrp_temp(ctx);
        for (size_t r = 1; !rc && r < t.nrows; ++r) {
            size_t first = t.row_end[r - 1], count = t.row_end[r] - first;
            if (count != hn) {
                char msg[96];
                snprintf(msg, sizeof msg, "record %zu has %zu fields, header has %zu", r, count, hn);
                rc = embrp_fail(ctx, msg);
                break;
            }
            rc = embrp_set_map(ctx, m);
            for (size_t c = 0; !rc && c < hn; ++c)
                rc = embrp_map_set_str(ctx, m, data + t.cells[c].off, t.cells[c].len, data + t.cells[first + c].off, t.cells[first + c].len);
            if (!rc) rc = embrp_array_push(ctx, out, m);
        }
    }
    table_free(&t);
    return rc;
}

/* ---- stringify ---- */

/* the text of a number cell, the same as embr prints it: a whole number up to 1e15 has no point, others have up to six decimals without trailing zeros */
static size_t number_text(const EmbrValue* v, char* buf, size_t cap) {
    if (v->tag == EMBR_TAG_INT) return (size_t)snprintf(buf, cap, "%lld", (long long)v->u.i);
    double d = v->u.d;
    if (d >= -1e15 && d <= 1e15 && d == (double)(long long)d) return (size_t)snprintf(buf, cap, "%lld", (long long)d);
    size_t n = (size_t)snprintf(buf, cap, "%f", d);
    if (n >= cap) n = cap - 1;
    if (memchr(buf, '.', n)) {
        while (n > 0 && buf[n - 1] == '0') --n;
        if (n > 0 && buf[n - 1] == '.') --n;
    }
    return n;
}

#define VEC_TAG "vec.handle"   /* keep in sync with plugins/vec/vec.cpp */

/* is v an array or a vec */
static int is_list(EmbrCtx* ctx, const EmbrValue* v) {
    uint32_t tag = embrp_type(ctx, v);
    return tag == EMBR_TAG_ARRAY || (tag == EMBR_TAG_POINTER && strcmp(embrp_ptr_tag(ctx, v), VEC_TAG) == 0);
}

static int fail_cell(EmbrCtx* ctx, const EmbrValue* v, size_t r, size_t c) {
    char msg[128];
    snprintf(msg, sizeof msg, "cell [%zu][%zu] must be a string or number, got %s", r, c, embrp_type_name(ctx, v));
    return embrp_fail(ctx, msg);
}

/* csv_stringify(rows: arr of arr, delimiter?: str) -> str */
static int csv_stringify(EmbrCtx* ctx, const EmbrValue* args, size_t argc, EmbrValue* out) {
    char d;
    if (delim_arg(ctx, args, argc, 1, &d)) return 1;
    const EmbrValue* rows = &args[0];
    if (!is_list(ctx, rows)) {
        char msg[160];
        snprintf(msg, sizeof msg, "rows must be an array or a vec, got ptr with tag '%.100s'", embrp_ptr_tag(ctx, rows));
        return embrp_fail(ctx, msg);
    }
    Buf o;
    memset(&o, 0, sizeof o);
    size_t nrows = embrp_array_len(ctx, rows);
    int rc = 0;
    for (size_t r = 0; !rc && r < nrows; ++r) {
        const EmbrValue* row = embrp_array_at(ctx, rows, r);
        if (!is_list(ctx, row)) {
            char msg[96];
            snprintf(msg, sizeof msg, "row %zu must be an array, got %s", r, embrp_type_name(ctx, row));
            rc = embrp_fail(ctx, msg);
            break;
        }
        size_t ncells = embrp_array_len(ctx, row);
        for (size_t c = 0; !rc && c < ncells; ++c) {
            const EmbrValue* cell = embrp_array_at(ctx, row, c);
            char numbuf[400];
            const char* t;
            size_t tn;
            if (c && buf_add_c(&o, d)) { rc = oom(ctx); break; }
            uint32_t tag = embrp_type(ctx, cell);
            if (tag == EMBR_TAG_STRING) t = embrp_str(ctx, cell, &tn);
            else if (tag == EMBR_TAG_INT || tag == EMBR_TAG_FLOAT) { tn = number_text(cell, numbuf, sizeof numbuf); t = numbuf; }
            else { rc = fail_cell(ctx, cell, r, c); break; }
            int quote = (tn == 0 && ncells == 1) ||   /* a bare blank line would be skipped on parse */
                        (tn > 0 && (t[0] == ' ' || t[tn - 1] == ' '));
            for (size_t k = 0; !quote && k < tn; ++k)
                if (t[k] == '"' || t[k] == '\r' || t[k] == '\n' || t[k] == d) quote = 1;
            if (quote) {
                if (buf_add_c(&o, '"')) { rc = oom(ctx); break; }
                for (size_t k = 0; k < tn; ++k) {
                    if (t[k] == '"' && buf_add_c(&o, '"')) { rc = oom(ctx); break; }
                    if (buf_add_c(&o, t[k])) { rc = oom(ctx); break; }
                }
                if (!rc && buf_add_c(&o, '"')) rc = oom(ctx);
            } else if (buf_add(&o, t, tn)) rc = oom(ctx);
        }
        if (!rc && buf_add_c(&o, '\n')) rc = oom(ctx);
    }
    if (!rc) rc = embrp_set_str(ctx, out, o.p ? o.p : "", o.len);
    free(o.p);
    return rc;
}

static const EmbrParam p_parse[]     = { { "text", EMBR_T_STRING, 0 }, { "delimiter", EMBR_T_STRING, EMBR_PARAM_OPTIONAL } };
static const EmbrParam p_stringify[] = { { "rows", EMBR_T_ARRAY | EMBR_T_POINTER, 0 }, { "delimiter", EMBR_T_STRING, EMBR_PARAM_OPTIONAL } };

static const EmbrFunc funcs[] = {
    { "csv_parse", "csv_parse(text: str, delimiter?: str) -> arr of arr of str. fields are always strings",
      csv_parse, NULL, p_parse, 2, EMBR_T_ARRAY, 0, 0 },
    { "csv_parse_dicts", "csv_parse_dicts(text: str, delimiter?: str) -> arr of map. the first record is the header. ragged records and duplicate header names raise",
      csv_parse_dicts, NULL, p_parse, 2, EMBR_T_ARRAY, 0, 0 },
    { "csv_stringify", "csv_stringify(rows: arr of arr, delimiter?: str) -> str. quotes only when needed, every record ends in \\n. cells are strings or numbers",
      csv_stringify, NULL, p_stringify, 2, EMBR_T_STRING, 0, 0 },
};

static const EmbrPluginInfo info = {
    EMBR_PLUGIN_ABI_VERSION, sizeof(EmbrPluginInfo), "csv", "1.0.0",
    "CSV parse and stringify (RFC 4180 with a few documented leniencies)", EMBR_CAP_NONE, funcs, sizeof funcs / sizeof funcs[0], NULL, 0, NULL,
    NULL
};

EMBR_PLUGIN_EXPORT const EmbrPluginInfo* embr_plugin_info(void) { return &info; }
