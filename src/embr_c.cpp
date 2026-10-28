// embr_c.cpp
// the plain C interface in include/embr_c.h, built on the C++ embr.h. every call catches C++ exceptions, so none reaches C.
//
// good to know
//   - one VmRunner per state, kept between runs (like the embr program does), so a top-level name made by one
//     embr_run_string is there for the next. it is destroyed before the interpreter, see embr_state

#define EMBR_C_BUILDING
#include "../include/embr_c.h"
#include <embr/embr.h>

#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

struct embr_state {
    embr::Interpreter interp;                          // declared first, so it is destroyed last
    std::unique_ptr<embr::vm::VmRunner> runner;
    std::string error;                                 // embr_last_error
    std::string scratch;                               // embr_get_string's result
};

struct embr_call {
    const std::vector<embr::Value>* args;
    embr::Value result = embr::Value(int64_t(0));
    std::string failure;
};

namespace {

// "file:line:col: kind: message", without the colours the terminal output has
std::string plainError(const embr::EmbrError& e, const std::string& filename) {
    std::string out = filename;
    if (e.hasLocation) out += ":" + std::to_string(e.range.startLine) + ":" + std::to_string(e.range.startCol);
    if (!out.empty()) out += ": ";
    if (!e.context.empty()) out += e.context + ": ";
    return out + e.message;
}

int run(embr_state* e, const std::string& src, const std::string& filename) {
    e->error.clear();
    try {
        if (!e->runner) e->runner = std::make_unique<embr::vm::VmRunner>(e->interp);
        {   // the parser recovers from a mistake and goes on, so it is asked for its errors first: the first one stops the run
            embr::Lexer lex(src);
            embr::SourceMap sm = lex.sourceMap();
            sm.setBaseDir(embr::dirOfFile(filename));
            embr::Parser parser(lex.tokenize(), sm);
            std::vector<embr::EmbrError> errors;
            parser.collectErrors(&errors);
            parser.parse();
            if (!errors.empty()) { e->error = plainError(errors.front(), filename); return 1; }
        }
        embr::vm::CompileOptions opts;
        opts.preloadPlugins = true;    // the plugins it imports at the top are known before it is checked
        auto prog = embr::vm::compileSource(src, e->interp, filename, opts);
        e->runner->run(prog);
        return 0;
    } catch (const embr::EmbrError& err) {
        e->error = plainError(err, filename);
    } catch (const std::exception& err) {
        e->error = std::string("internal error: ") + err.what();
    } catch (...) {
        e->error = "internal error";
    }
    return 1;
}

const embr::Value* global(embr_state* e, const char* name) {
    if (!e || !name || !e->interp.hasGlobal(name)) return nullptr;
    return &e->interp.getGlobal(name);
}

embr_type typeOf(const embr::Value& v) {
    if (v.isInt()) return EMBR_TYPE_INT;
    if (v.isNumber()) return EMBR_TYPE_FLOAT;
    if (v.isString()) return EMBR_TYPE_STRING;
    return EMBR_TYPE_OTHER;
}

const embr::Value* arg(embr_call* c, int i) {
    if (!c || i < 0 || (size_t)i >= c->args->size()) return nullptr;
    return &(*c->args)[(size_t)i];
}

void set(embr_state* e, const char* name, embr::Value v) {
    if (!e || !name) return;
    try { e->interp.defineGlobal(name, std::move(v)); } catch (...) {}
}

} // namespace

extern "C" {

embr_state* embr_new(void) {
    try { return new embr_state(); } catch (...) { return nullptr; }
}

void embr_free(embr_state* e) {
    if (!e) return;
    try { e->runner.reset(); delete e; } catch (...) {}
}

int embr_run_string(embr_state* e, const char* source, const char* filename) {
    if (!e || !source) return 1;
    return run(e, source, filename ? filename : "<input>");
}

int embr_run_file(embr_state* e, const char* path) {
    if (!e || !path) return 1;
    std::ifstream f(path, std::ios::binary);
    if (!f) { e->error = std::string(path) + ": cannot open file"; return 1; }
    std::string src((std::istreambuf_iterator<char>(f)), {});
    try { e->interp.scriptDir = std::filesystem::absolute(path).parent_path().string(); } catch (...) {}
    return run(e, src, path);
}

const char* embr_last_error(embr_state* e) { return e ? e->error.c_str() : "no state"; }

embr_type embr_get_type(embr_state* e, const char* name) {
    const embr::Value* v = global(e, name);
    return v ? typeOf(*v) : EMBR_TYPE_NONE;
}

int embr_get_int(embr_state* e, const char* name, int64_t* out) {
    const embr::Value* v = global(e, name);
    if (!v || !v->isInt() || !out) return 1;
    *out = v->asInt();
    return 0;
}

int embr_get_float(embr_state* e, const char* name, double* out) {
    const embr::Value* v = global(e, name);
    if (!v || !v->isNumeric() || !out) return 1;
    *out = v->asNumber();
    return 0;
}

int embr_get_string(embr_state* e, const char* name, const char** out) {
    const embr::Value* v = global(e, name);
    if (!v || !v->isString() || !out) return 1;
    e->scratch = v->asString();
    *out = e->scratch.c_str();
    return 0;
}

void embr_set_int(embr_state* e, const char* name, int64_t value)       { set(e, name, embr::Value(value)); }
void embr_set_float(embr_state* e, const char* name, double value)      { set(e, name, embr::Value(value)); }
void embr_set_string(embr_state* e, const char* name, const char* value) { set(e, name, embr::Value(std::string(value ? value : ""))); }

void embr_register(embr_state* e, const char* name, embr_fn fn, void* userdata) {
    if (!e || !name || !fn) return;
    std::string nm = name;
    try {
        e->interp.bind(nm, [nm, fn, userdata](const std::vector<embr::Value>& args) -> embr::Value {
            embr_call call;
            call.args = &args;
            int status = 1;
            try { status = fn(&call, userdata); } catch (...) { call.failure = "the host function threw"; }
            if (status != 0)
                embr::raiseError(nm, call.failure.empty() ? "the host function failed" : call.failure);
            return call.result;
        });
    } catch (...) {}
}

int embr_arg_count(embr_call* c) { return c ? (int)c->args->size() : 0; }

embr_type embr_arg_type(embr_call* c, int i) {
    const embr::Value* v = arg(c, i);
    return v ? typeOf(*v) : EMBR_TYPE_NONE;
}

int64_t embr_arg_int(embr_call* c, int i) {
    const embr::Value* v = arg(c, i);
    return v && v->isInt() ? v->asInt() : 0;
}

double embr_arg_float(embr_call* c, int i) {
    const embr::Value* v = arg(c, i);
    return v && v->isNumeric() ? v->asNumber() : 0.0;
}

const char* embr_arg_string(embr_call* c, int i) {
    const embr::Value* v = arg(c, i);
    return v && v->isString() ? v->asString().c_str() : "";
}

void embr_return_int(embr_call* c, int64_t value)       { if (c) c->result = embr::Value(value); }
void embr_return_float(embr_call* c, double value)      { if (c) c->result = embr::Value(value); }
void embr_return_string(embr_call* c, const char* value) {
    if (c) { try { c->result = embr::Value(std::string(value ? value : "")); } catch (...) {} }
}

int embr_fail(embr_call* c, const char* message) {
    if (c) { try { c->failure = message ? message : ""; } catch (...) {} }
    return 1;
}

void embr_allow_plugins(embr_state* e, const char* const* names, int count) {
    if (!e) return;
    try {
        e->interp.pluginPolicy.restricted = true;
        e->interp.pluginPolicy.allowed.clear();
        for (int i = 0; i < count; ++i)
            if (names && names[i]) e->interp.pluginPolicy.allowed.insert(names[i]);
    } catch (...) {}
}

void embr_allow_file_io(embr_state* e, int on) {
    if (!e) return;
    e->interp.pluginPolicy.fileIo = on != 0;
    // plugins that need the filesystem are refused too, not only embrlib's file functions
    if (on) e->interp.pluginPolicy.deniedCaps &= ~(uint64_t)EMBR_CAP_FILESYSTEM;
    else e->interp.pluginPolicy.deniedCaps |= EMBR_CAP_FILESYSTEM;
}

int embr_deny_capabilities(embr_state* e, const char* list) {
    if (!e || !list) return 1;
    uint64_t mask = 0;
    std::string bad;
    if (!embr::capMask(list, mask, bad)) return 1;
    e->interp.pluginPolicy.restricted = true;
    e->interp.pluginPolicy.anyName = true;
    e->interp.pluginPolicy.deniedCaps |= mask;
    if (mask & EMBR_CAP_FILESYSTEM) e->interp.pluginPolicy.fileIo = false;
    return 0;
}

} // extern "C"
