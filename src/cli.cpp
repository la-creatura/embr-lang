// cli.cpp
// usage:
//   embr                        # start REPL
//   embr file.embr ...          # run one or more script files. exits 1 if a file can't be read or raises an error
//   embr file.embr -- a b       # same; a, b are handed to the script (os_args() in the os plugin)
//   embr --sandbox ...           # restrict imports to plugins that can't touch files/processes/network/threads
//   embr --allow-plugins=a,b ... # restrict imports to exactly these plugins
//   embr --no-plugins ...        # allow no plugin imports at all
//   embr test [--filter=T] [PATH...]  # run script tests (test_* functions, see modules/testing.embr)
//   embr check FILE...           # parse + compile only (no run, no plugins); errors as file:line:col: message
//   embr fmt [--check] [--stdout] [--indent=N] FILE...  # format source files in place (--check: just list files that would change)
//   embr dis [--chunk=N] FILE    # show the VM code a script compiles to (nothing is run, no plugin is loaded); FILE can be a .vmbr
//   embr compile FILE [-o OUT]   # compile a script to a .vmbr file (default: next to the script)
//   embr FILE.vmbr               # run a compiled script (no source, so errors show no source line)
//   embr --cache-dir=DIR ...     # keep compiled scripts, and the modules they import, in DIR as .vmbr files and reuse
//                                # them while the script is unchanged. without this flag embr never writes a compiled
//                                # file, except `embr compile`
//   embr --max-steps=N ...       # stop the script with an error (not catchable by the script) after N loop turns + function calls
//   embr --timeout=SECONDS ...   # same, after that much time. both apply to async/parallel workers too
//   embr cache clean [--all] DIR # delete the .vmbr files in DIR that are stale, damaged or from another build (--all: every one)
//   embr plugins [--json]        # the installed plugins: version, kind, capabilities, and why one cannot be loaded
//   embr plugin NAME [--json]    # one plugin's reference: description, capabilities, every function with its signature and doc
//   embr modules [--json]        # the installed script modules: version, what they need (declared and from their imports)
//   embr module NAME|FILE [--json]  # one module's reference, read without running it: __module__, imports, functions and their docs
//   embr pack DIR [-o OUT.pmbr]  # make a package (one file with a module or a plugin per platform, libraries and assets) from DIR
//   embr packages [--json]       # the installed packages: version, kind, what they need, the platforms they have builds for
//   embr package NAME|FILE [--json]  # one package, read without unpacking or running anything
//   embr unpack FILE DIR         # write every file of a package into DIR
//   embr merge A.pmbr B.pmbr ... -o OUT.pmbr  # one package from the same package built on different platforms
//   embr package FILE --registry-entry URL    # the index.json entry `embr pkg` wants for a package served at URL
//   embr cache clean --packages [--all]       # remove unpacked packages nothing uses any more (--all: every one)
//   embr abi                     # the C++ ABI tag of this embr (what a package names a plugin build by) and the exact toolchain
//   embr keygen NAME             # make a signing key pair: NAME.key (keep it secret) and NAME.pub (give it out)
//   embr sign FILE.pmbr --key NAME.key  # sign a package (replaces an older signature). `embr pack DIR --sign NAME.key` does it while packing
//   embr trust KEY.pub|HEX [NOTE]       # add a public key to your trust list; embr untrust KEY.pub|HEX removes it; embr trusted lists them
//   embr --require-signed ...    # refuse a package that is not signed by a trusted key
//   embr --deny-caps=a,b ...     # refuse plugins that need these capabilities: filesystem, network, process, env, threads, native
//   embr lsp                     # language server on stdin/stdout: errors, definition, references, rename, hover, completion
//   embr --version               # which build directory made this binary, and its backends
//   embr --backend NAME ...      # run on a specific backend (see --version for the list; today only "vm", the default)
//   embr --vm ...                # same as --backend vm, kept so older scripts keep working
//   embr pkg install <name> [version] [--registry <url>]
//   embr pkg list | remove <name> | versions <name>
//                                # package manager (modules/pkg.embr, see its header for the registry protocol)

#include <embr/embr.h>
#include "fmt.h"
#include "lsp.h"

#include <iostream>
#include <memory>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <filesystem>
#include <optional>
#include <algorithm>

#ifdef _WIN32
#  include <conio.h>
#  include <io.h>
#else
#  include <termios.h>
#  include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace embr_debug {

using ChildFn = std::function<void(std::ostream&, const std::string&, bool)>;

static void printExpr(embr::Expr* e, std::ostream& out, const std::string& prefix, bool isLast);
static void printStmt(embr::Stmt* s, std::ostream& out, const std::string& prefix, bool isLast);

static std::string truncate(const std::string& s, size_t maxLen = 60) {
    if (s.size() <= maxLen) return s;
    return s.substr(0, maxLen) + "...";
}

static std::string escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '\n': out += "\\n";  break;
            case '\t': out += "\\t";  break;
            case '\r': out += "\\r";  break;
            case '"':  out += "\\\""; break;
            default:   out += c;
        }
    }
    return out;
}

static std::string atLine(const embr::SourceRange& r) {
    return r.valid() ? ("  (L" + std::to_string(r.startLine) + ")") : "";
}

static std::string paramsToString(const std::vector<embr::Param>& params) {
    std::string s;
    for (size_t i = 0; i < params.size(); ++i) {
        if (i) s += ", ";
        const auto& p = params[i];
        if (p.variadic) s += "...";
        s += p.name;
        if (!p.type.isAny()) s += ": " + p.type.name();
        if (p.optional && !p.variadic) s += "?";
    }
    return s;
}

static void printNode(std::ostream& out, const std::string& prefix, bool isLast,
                      const std::string& label, const std::vector<ChildFn>& kids) {
    out << prefix << (isLast ? "\\-- " : "|-- ") << label << "\n";
    std::string childPrefix = prefix + (isLast ? "    " : "|   ");
    for (size_t i = 0; i < kids.size(); ++i)
        kids[i](out, childPrefix, i + 1 == kids.size());
}

static ChildFn blockPrinter(const std::vector<embr::StmtPtr>& block, std::string label) {
    return [&block, label](std::ostream& out, const std::string& prefix, bool isLast) {
        std::vector<ChildFn> kids;
        for (auto& s : block)
            kids.push_back([sp = s.get()](std::ostream& o, const std::string& p, bool last) {
                printStmt(sp, o, p, last);
            });
        printNode(out, prefix, isLast,
                 label + " (" + std::to_string(block.size()) + ")", kids);
    };
}

static void printExpr(embr::Expr* e, std::ostream& out, const std::string& prefix, bool isLast) {
    using Kind = embr::Expr::Kind;
    std::vector<ChildFn> kids;
    std::string label;

    auto addExpr = [&](embr::Expr* ep) {
        kids.push_back([ep](std::ostream& o, const std::string& p, bool last) { printExpr(ep, o, p, last); });
    };

    switch (e->kind) {
        case Kind::Number:
            label = "Number " + embr::formatNumber(static_cast<embr::NumberExpr*>(e)->v);
            break;
        case Kind::Int:
            label = "Int " + std::to_string(static_cast<embr::IntExpr*>(e)->v);
            break;
        case Kind::String:
            label = "String \"" + truncate(escape(static_cast<embr::StringExpr*>(e)->v)) + "\"";
            break;
        case Kind::Var:
            label = "Var " + static_cast<embr::VarExpr*>(e)->name;
            break;
        case Kind::Unary: {
            auto* u = static_cast<embr::UnaryExpr*>(e);
            label = "Unary " + u->op;
            addExpr(u->right.get());
            break;
        }
        case Kind::Binary: {
            auto* b = static_cast<embr::BinaryExpr*>(e);
            label = "Binary " + b->op;
            addExpr(b->left.get());
            addExpr(b->right.get());
            break;
        }
        case Kind::And:
        case Kind::Or: {
            auto* lg = static_cast<embr::LogicExpr*>(e);
            label = (e->kind == Kind::And) ? "And" : "Or";
            addExpr(lg->left.get());
            addExpr(lg->right.get());
            break;
        }
        case Kind::Call: {
            auto* c = static_cast<embr::CallExpr*>(e);
            label = "Call (" + std::to_string(c->args.size()) + " args)";
            addExpr(c->callee.get());
            for (auto& a : c->args) addExpr(a.get());
            break;
        }
        case Kind::Array: {
            auto* a = static_cast<embr::ArrayExpr*>(e);
            label = "Array [" + std::to_string(a->elements.size()) + "]";
            for (auto& el : a->elements) addExpr(el.get());
            break;
        }
        case Kind::Map: {
            auto* m = static_cast<embr::MapExpr*>(e);
            label = "Map {" + std::to_string(m->entries.size()) + "}";
            for (auto& [k, v] : m->entries) {
                kids.push_back([k = k.get(), v = v.get()](std::ostream& o, const std::string& p, bool last) {
                    std::vector<ChildFn> pairKids;
                    pairKids.push_back([k](std::ostream& o2, const std::string& p2, bool l2) { printExpr(k, o2, p2, l2); });
                    pairKids.push_back([v](std::ostream& o2, const std::string& p2, bool l2) { printExpr(v, o2, p2, l2); });
                    printNode(o, p, last, "Entry", pairKids);
                });
            }
            break;
        }
        case Kind::Index: {
            auto* idx = static_cast<embr::IndexExpr*>(e);
            label = "Index";
            addExpr(idx->object.get());
            addExpr(idx->index.get());
            break;
        }
        case Kind::Ref: {
            label = "Ref";
            addExpr(static_cast<embr::RefExpr*>(e)->target.get());
            break;
        }
        case Kind::Lambda: {
            auto* lam = static_cast<embr::LambdaExpr*>(e);
            label = "Lambda (" + paramsToString(lam->params) + ")";
            if (!lam->returnType.isAny()) label += " -> " + lam->returnType.name();
            for (auto& s : lam->ownedBody)
                kids.push_back([sp = s.get()](std::ostream& o, const std::string& p, bool last) {
                    printStmt(sp, o, p, last);
                });
            break;
        }
    }
    label += atLine(e->range);
    printNode(out, prefix, isLast, label, kids);
}

static void printStmt(embr::Stmt* s, std::ostream& out, const std::string& prefix, bool isLast) {
    using Kind = embr::Stmt::Kind;
    std::vector<ChildFn> kids;
    std::string label;

    auto addExpr = [&](embr::Expr* ep) {
        kids.push_back([ep](std::ostream& o, const std::string& p, bool last) { printExpr(ep, o, p, last); });
    };

    switch (s->kind) {
        case Kind::Assign: {
            auto* a = static_cast<embr::AssignStmt*>(s);
            label = "Assign";
            addExpr(a->target.get());
            addExpr(a->value.get());
            break;
        }
        case Kind::Local: {
            auto* l = static_cast<embr::LocalStmt*>(s);
            label = "Local ";
            if (l->bracketed) label += "[";
            for (size_t i = 0; i < l->targets.size(); ++i) {
                if (i) label += ", ";
                const auto& t = l->targets[i];
                if (!t.type.isAny()) label += t.type.name() + " ";
                label += t.name;
            }
            if (l->bracketed) label += "]";
            addExpr(l->value.get());
            break;
        }
        case Kind::MultiAssign: {
            auto* m = static_cast<embr::MultiAssignStmt*>(s);
            label = "MultiAssign ";
            for (size_t i = 0; i < m->names.size(); ++i) {
                if (i) label += ", ";
                label += m->names[i];
            }
            addExpr(m->value.get());
            break;
        }
        case Kind::If: {
            auto* i = static_cast<embr::IfStmt*>(s);
            label = "If";
            addExpr(i->cond.get());
            kids.push_back(blockPrinter(i->thenBlock, "Then"));
            if (!i->elseBlock.empty())
                kids.push_back(blockPrinter(i->elseBlock, "Else"));
            break;
        }
        case Kind::Fn: {
            auto* f = static_cast<embr::FnStmt*>(s);
            label = "Fn " + f->name + "(" + paramsToString(f->params) + ")";
            if (!f->returnType.isAny()) label += " -> " + f->returnType.name();
            kids.push_back(blockPrinter(f->body, "Body"));
            break;
        }
        case Kind::Return: {
            auto* r = static_cast<embr::ReturnStmt*>(s);
            label = "Return";
            addExpr(r->value.get());
            break;
        }
        case Kind::While: {
            auto* w = static_cast<embr::WhileStmt*>(s);
            label = "While";
            addExpr(w->cond.get());
            kids.push_back(blockPrinter(w->body, "Body"));
            break;
        }
        case Kind::For: {
            auto* f = static_cast<embr::ForStmt*>(s);
            label = "For " + (f->keyName.empty() ? f->varName : (f->keyName + ", " + f->varName));
            addExpr(f->iterable.get());
            kids.push_back(blockPrinter(f->body, "Body"));
            break;
        }
        case Kind::Break:
            label = "Break";
            break;
        case Kind::Continue:
            label = "Continue";
            break;
        case Kind::Import:
            label = "Import";
            addExpr(static_cast<embr::ImportStmt*>(s)->pathExpr.get());
            break;
        case Kind::Expr: {
            auto* es = static_cast<embr::ExprStmt*>(s);
            label = "ExprStmt";
            addExpr(es->expr.get());
            break;
        }
        case Kind::Try: {
            auto* t = static_cast<embr::TryStmt*>(s);
            label = "Try";
            kids.push_back(blockPrinter(t->tryBlock, "Try"));
            kids.push_back(blockPrinter(t->catchBlock, "Catch " + t->catchVar));
            break;
        }
    }
    label += atLine(s->range);
    printNode(out, prefix, isLast, label, kids);
}

static void printProgram(const std::vector<embr::StmtPtr>& prog, std::ostream& out) {
    out << "Program (" << prog.size() << " statement" << (prog.size() == 1 ? "" : "s") << ")\n";
    for (size_t i = 0; i < prog.size(); ++i)
        printStmt(prog[i].get(), out, "", i + 1 == prog.size());
}

static void printAstFromSource(const std::string& src) {
    embr::Lexer lex(src);
    auto tokens = lex.tokenize();
    embr::SourceMap sm = lex.sourceMap();
    embr::Parser parser(std::move(tokens), sm);
    auto prog = parser.parse();
    printProgram(prog, std::cout);
}

} // namespace embr_debug

#ifndef EMBR_BUILD_NAME
#define EMBR_BUILD_NAME "unknown"
#endif

// bin/ is shared by every CMake preset, so a binary that looks like the wrong build is very
// often just "the last preset to build overwrote it" rather than a real bug
[[maybe_unused]] static const char* kSharedBinHint =
    "note: this binary was built by build/" EMBR_BUILD_NAME ". bin/<platform>/ is shared by all\n"
    "presets, so building another preset afterwards overwrites it. If you expected a different\n"
    "build, rebuild that preset (e.g. `cmake --build build/linux-debug`).\n";

static std::string backendSummary() {
    std::string s;
    for (const auto& n : embr::backendNames()) s += (s.empty() ? "" : ", ") + n;
    return s.empty() ? "none" : s;
}

static embr::PluginPolicy g_policy;
static uint64_t g_maxSteps = 0;   // --max-steps=N
static double   g_timeout = 0;    // --timeout=SECONDS
// the VM backend's persistent top-level runner: top-level variables live in it across run() calls (REPL,
// several files). main() resets it before the interpreter is destroyed so its frames don't outlive
// (and leak through) the scopes they reference.
static std::unique_ptr<embr::vm::VmRunner> g_vmRunner;

// runs src on the backend. the only place cli.cpp has to choose, since
// embrlib and every other plugin already go through embr::invoke() (core/invoke.h)
//
// the VM runner is shared across every call in the process (several file arguments, the REPL) as a static, so
// top-level variables and functions persist from one call to the next. that's safe because cli.cpp makes only one
// Interpreter per process
// set by --cache-dir=DIR. empty (the default) means compiled scripts are never written to disk
static std::string g_cacheDir;

// returns false if the script raised an error (it is printed here), so the caller can exit non-zero
// closedWorld: this file is the whole program, so the type checker may infer parameter types from its calls
static bool runSourceDispatch(const std::string& src, embr::Interpreter& interp,
                              const std::string& filename, const std::string& cacheDir = "", bool closedWorld = false) {
    // backend switch: a new backend needs its own case here (and in runTestCommand below)
    // kept at file scope (see g_vmRunner) so main() can release it BEFORE the interpreter is destroyed
    if (!g_vmRunner) g_vmRunner = std::make_unique<embr::vm::VmRunner>(interp);
    auto& vmRunner = *g_vmRunner;
    try {
        embr::vm::CompileOptions copts;
        copts.closedWorld = closedWorld;
        copts.preloadPlugins = closedWorld;   // the script you run: learn the plugins it imports first, so their signatures are checked
        auto prog = embr::vm::compileSourcePreferCached(src, interp, filename, cacheDir, copts);
        vmRunner.run(prog);
    } catch (const embr::EmbrError& e) {
        std::cerr << e.what() << embr::formatTrace(e);
        return false;
    }
    return true;
}

// escapes s for embedding inside a double-quoted embr string literal. only used to hand the CLI's own argv
// (untrusted process arguments, not script source) to the pkg.embr module as a real array literal (see
// runPkgCommand below), instead of splicing argv text into code
static std::string escapeEmbrStringLiteral(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\\' || c == '"') out += '\\';
        out += c;
    }
    return out;
}

// exit code from pkg_main(), set by the __cli_set_exit binding below.
// static storage (not a captured local) so the no-capture lambda passed to
// bindSig can reference it directly, the same way it could reference any
// other file-scope state.
static int g_pkgExitCode = 0;

// `embr pkg <args...>`: runs modules/pkg.embr's pkg_main(argv) with the remaining arguments (after "pkg", minus
// the backend flags) as an embr array of strings. it's a tiny generated script, not a C++ dispatcher, so the package manager's
// command handling lives in one place (pkg_main), like modules/ffi.embr owns ffi_cdef's parsing
static int runPkgCommand(const std::vector<std::string>& pkgArgs, embr::Interpreter& interp) {
    interp.bindSig("__cli_set_exit", {embr::Param::req("code", embr::TS::Num)},
    [](const std::vector<embr::Value>& args) -> embr::Value {
        g_pkgExitCode = (int)args[0].asNumber();
        return embr::Value(0.0);
    });

    std::string src = "import \"pkg.embr\"\n__cli_set_exit(pkg_main([";
    for (size_t i = 0; i < pkgArgs.size(); ++i) {
        if (i) src += ", ";
        src += "\"" + escapeEmbrStringLiteral(pkgArgs[i]) + "\"";
    }
    src += "]))\n";

    if (!runSourceDispatch(src, interp, "<pkg>")) return g_pkgExitCode ? g_pkgExitCode : 1;
    return g_pkgExitCode;
}

// runs a compiled script (a .vmbr file). it has no source text, so an error shows no source line.
// false if it could not be read or raised an error
static bool runCompiledFile(const std::string& path, embr::Interpreter& interp) {
    auto bytes = embr::vm::readWholeFile(path);
    if (!bytes) { std::cerr << "Error: cannot open: " << path << "\n"; return false; }
    if (std::string why = embr::checkCompiledBeforeRun(interp, fs::absolute(path), *bytes, /*declaredOnly=*/true); !why.empty()) { std::cerr << "Error: " << why << "\n"; return false; }
    if (!g_vmRunner) g_vmRunner = std::make_unique<embr::vm::VmRunner>(interp);
    try {
        auto prog = embr::vm::deserializeProgram(*bytes, path);
        embr::vm::registerChunkTeardown(interp, prog);
        interp.setSource(embr::SourceMap(), path);
        interp.scriptDir = fs::absolute(path).parent_path().string();
        g_vmRunner->run(prog);
    } catch (const embr::EmbrError& e) {
        std::cerr << e.what() << embr::formatTrace(e);
        return false;
    }
    return true;
}

// runs one script file (.embr or a compiled .vmbr). false if it could not be read or raised an error
static bool runFile(const std::string& path, embr::Interpreter& interp) {
    if (fs::path(path).extension() == ".vmbr") return runCompiledFile(path, interp);
    std::ifstream f(path);
    if (!f) { std::cerr << "Error: cannot open: " << path << "\n"; return false; }
    std::string src((std::istreambuf_iterator<char>(f)), {});

    interp.scriptDir = fs::absolute(path).parent_path().string();
    // a script that declares `__module__` is checked like a module it imports: a wrong declaration, or a need the host denies, stops it before it starts
    if (std::string why = embr::checkModuleBeforeRun(interp, fs::absolute(path), src, /*declaredOnly=*/true); !why.empty()) { std::cerr << "Error: " << why << "\n"; return false; }
    return runSourceDispatch(src, interp, path, g_cacheDir, /*closedWorld=*/true);
}

// every lex, parse and compile error in src, as data. used by `embr check` and `embr lsp`
// nothing is run. plugins the file imports by a literal name are loaded so calls to their functions are type checked, like
// `embr FILE` does, but only from the trusted places (next to embr, EMBR_PATH), the same as --sandbox. one in the
// script's own folder is not loaded, and calls to it are not checked
//
// `embr check FILE...` prints each error as `file:line:col: message` (plain text, for editors and CI). the parser
// recovers after an error, so several can be reported per file. returns the number of files with errors
static std::vector<embr::EmbrError> diagnose(const std::string& src, const std::string& path) {
    std::vector<embr::EmbrError> errors;
    try {
        embr::Lexer lex(src);
        auto tokens = lex.tokenize();
        embr::SourceMap sm = lex.sourceMap();
        embr::Parser parser(std::move(tokens), sm);
        parser.collectErrors(&errors);
        auto prog = parser.parse();
        embr::Interpreter interp;
        interp.pluginPolicy.restricted = true;
        interp.pluginPolicy.fileIo = false;
        if (errors.empty()) {
            std::streambuf* saved = std::cerr.rdbuf(nullptr);   // no "loaded plugin" lines in check's output
            for (auto& st : prog) {
                if (st->kind != embr::Stmt::Kind::Import) continue;
                auto* im = static_cast<embr::ImportStmt*>(st.get());
                if (!im->pathExpr || im->pathExpr->kind != embr::Expr::Kind::String) continue;
                const std::string& name = static_cast<embr::StringExpr*>(im->pathExpr.get())->v;
                interp.pluginPolicy.allowed.insert(name);
                try { embr::importNativePlugin(interp, name, /*runModules=*/false); } catch (...) {}   // not found or a script module: the run reports it
            }
            std::cerr.rdbuf(saved);
        }
        if (errors.empty() && src.find(embr::kModuleMetaName) != std::string::npos) {
            // a wrong `__module__` (not a literal, an unknown key, a capability it leaves out) is an error of the file itself
            for (const auto& p : embr::analyzeModule(src, path, interp.pluginPolicy).problems) {
                try { embr::raiseError("module", p.text, p.range, sm); } catch (const embr::EmbrError& e) { errors.push_back(e); }
            }
        }
        if (errors.empty()) {          // type mistakes next, all of them, as `embr FILE` would see them
            embr::vm::ModuleTypeResolver modules(&interp);
            embr::typecheck::Options topt = modules.optionsFor(std::filesystem::path(path).parent_path().string());
            topt.closedWorld = true;
            topt.source = &sm;
            for (auto& e : embr::typecheck::check(prog, topt)) errors.push_back(e);
        }
        if (errors.empty()) {          // compile-time errors (e.g. a stray break) only matter once it parses
            embr::vm::CompileOptions copts; copts.closedWorld = true;
            embr::vm::compileSource(src, interp, path, copts);
        }
    } catch (const embr::EmbrError& e) {
        errors.push_back(e);          // lexer errors stop tokenizing, so there is exactly one
    }
    return errors;
}

static int checkFiles(const std::vector<std::string>& paths) {
    int bad = 0;
    for (const auto& path : paths) {
        std::ifstream f(path);
        if (!f) { std::cerr << path << ": error: cannot open file\n"; ++bad; continue; }
        std::string src((std::istreambuf_iterator<char>(f)), {});

        auto loc = [&](const embr::EmbrError& e) {
            std::string out = path;
            if (e.hasLocation) out += ":" + std::to_string(e.range.startLine) + ":" + std::to_string(e.range.startCol);
            return out + ": " + e.context + (e.context.empty() ? "" : ": ") + e.message;
        };
        auto errors = diagnose(src, path);
        for (const auto& e : errors) std::cerr << loc(e) << "\n";
        if (!errors.empty()) ++bad;
    }
    return bad;
}

// `embr dis [--chunk=N] FILE`: compiles FILE and prints the VM code, one instruction per line (see vm_disasm.h).
// nothing is run and no plugin is loaded. --chunk=N shows only that chunk (0 is the top level, then one per function)
// embr plugins [--json]
static int runPluginsCommand(const std::vector<std::string>& args) {
    bool json = false;
    for (const auto& a : args) {
        if (a == "--json") json = true;
        else { std::cerr << "usage: embr plugins [--json]\n"; return 2; }
    }
    auto list = embr::installedPlugins();
    if (json) {
        std::cout << "[";
        for (size_t i = 0; i < list.size(); ++i) {
            const auto& p = list[i];
            auto arr = [](uint64_t m) { std::string s = "["; bool f = true; for (const auto& n : embr::capNames(m)) { s += (f ? "" : ", ") + embr::jsonQuote(n); f = false; } return s + "]"; };
            std::cout << (i ? ",\n " : "\n ") << "{\"name\": " << embr::jsonQuote(p.name) << ", \"version\": " << embr::jsonQuote(p.version)
                      << ", \"description\": " << embr::jsonQuote(p.description) << ", \"kind\": " << embr::jsonQuote(p.cxx ? "cxx" : "c")
                      << ", \"declared\": " << (p.declared ? "true" : "false") << ", \"abi\": " << p.abi << ", \"build_id\": " << embr::jsonQuote(p.buildId) << ", \"abi_tag\": " << embr::jsonQuote(p.abiTag)
                      << ", \"path\": " << embr::jsonQuote(p.path) << ", \"capabilities\": " << arr(p.caps)
                      << ", \"optional_capabilities\": " << arr(p.optionalCaps) << ", \"functions\": " << p.functions
                      << ", \"problem\": " << embr::jsonQuote(p.problem) << "}";
        }
        std::cout << (list.empty() ? "]\n" : "\n]\n");
        return 0;
    }
    size_t wName = 4, wVer = 7, wNeeds = 5;
    for (const auto& p : list) {
        wName = std::max(wName, p.name.size());
        wVer = std::max(wVer, p.version.size());
        wNeeds = std::max(wNeeds, embr::capListText(p.caps).size());
    }
    auto pad = [](std::string s, size_t w) { return s + std::string(w > s.size() ? w - s.size() : 0, ' '); };
    std::cout << pad("NAME", wName) << "  " << pad("VERSION", wVer) << "  KIND  " << pad("NEEDS", wNeeds) << "  DESCRIPTION\n";
    for (const auto& p : list) {
        std::cout << pad(p.name, wName) << "  " << pad(p.version.empty() ? "-" : p.version, wVer) << "  " << pad(p.cxx ? "c++" : "c", 4) << "  "
                  << pad(p.declared ? embr::capListText(p.caps) : "unknown", wNeeds) << "  " << p.description << "\n";
        if (!p.problem.empty()) std::cout << pad("", wName) << "  cannot be loaded: " << p.problem << "\n";
    }
    return 0;
}

// embr plugin NAME [--json]
static int runPluginCommand(const std::vector<std::string>& args) {
    bool json = false;
    std::string name;
    for (const auto& a : args) {
        if (a == "--json") json = true;
        else if (name.empty() && a.rfind("--", 0) != 0) name = a;
        else { std::cerr << "usage: embr plugin NAME [--json]\n"; return 2; }
    }
    if (name.empty()) { std::cerr << "usage: embr plugin NAME [--json]\n"; return 2; }
    embr::Interpreter interp;    // loads the plugin like `import` does, so the signatures come from the functions it really binds
    std::streambuf* saved = std::cerr.rdbuf(nullptr);
    std::string err;
    try { err = embr::importNativePlugin(interp, name); } catch (const std::exception& e) { err = e.what(); }
    std::cerr.rdbuf(saved);
    if (!err.empty() || interp.plugins.empty()) {
        std::cerr << "embr: " << (err.empty() ? "no such plugin: " + name : err) << "\n";
        return 1;
    }
    const auto& rec = interp.plugins.back();
    if (json) std::cout << embr::pluginJson(interp, rec, true) << "\n";
    else std::cout << embr::pluginText(interp, rec);
    return 0;
}

// embr modules [--json]
static int runModulesCommand(const std::vector<std::string>& args) {
    bool json = false;
    for (const auto& a : args) {
        if (a == "--json") json = true;
        else { std::cerr << "usage: embr modules [--json]\n"; return 2; }
    }
    embr::PluginPolicy policy;
    auto list = embr::installedModules(policy);
    if (json) {
        std::cout << "[";
        for (size_t i = 0; i < list.size(); ++i) std::cout << (i ? ",\n " : "\n ") << embr::moduleJson(list[i], false);
        std::cout << (list.empty() ? "]\n" : "\n]\n");
        return 0;
    }
    size_t wName = 4, wVer = 7, wNeeds = 5;
    for (const auto& m : list) {
        wName = std::max(wName, m.name.size());
        wVer = std::max(wVer, m.version.size());
        wNeeds = std::max(wNeeds, embr::capListText(m.effectiveCaps()).size() + (m.dynamicImports && !m.declared ? 1 : 0));
    }
    auto pad = [](std::string s, size_t w) { return s + std::string(w > s.size() ? w - s.size() : 0, ' '); };
    std::cout << pad("NAME", wName) << "  " << pad("VERSION", wVer) << "  " << pad("NEEDS", wNeeds) << "  DESCRIPTION\n";
    for (const auto& m : list) {
        std::string needs = embr::capListText(m.effectiveCaps()) + (m.dynamicImports && !m.declared ? "+" : "");
        std::cout << pad(m.name, wName) << "  " << pad(m.version.empty() ? "-" : m.version, wVer) << "  " << pad(m.parsed ? needs : "?", wNeeds) << "  " << m.description << "\n";
        for (const auto& p : m.problems) std::cout << pad("", wName) << "  problem: " << p.text << "\n";
        if (!m.parsed) std::cout << pad("", wName) << "  does not parse\n";
    }
    return 0;
}

// embr module NAME|FILE [--json]
static int runModuleCommand(const std::vector<std::string>& args) {
    bool json = false;
    std::string name;
    for (const auto& a : args) {
        if (a == "--json") json = true;
        else if (name.empty() && a.rfind("--", 0) != 0) name = a;
        else { std::cerr << "usage: embr module NAME|FILE [--json]\n"; return 2; }
    }
    if (name.empty()) { std::cerr << "usage: embr module NAME|FILE [--json]\n"; return 2; }
    std::filesystem::path file;
    if (!embr::findModuleFile(name, std::filesystem::current_path().string(), file)) {
        std::cerr << "embr: cannot find module \"" << name << "\"\n";
        return 1;
    }
    embr::PluginPolicy policy;
    std::set<std::string> visiting;
    embr::ModuleInfo info = embr::analyzeModuleFile(file, policy, visiting);
    if (!info.parsed) { std::cerr << "embr: " << file.string() << " does not parse (run `embr check` for the error)\n"; return 1; }
    if (json) std::cout << embr::moduleJson(info, true) << "\n";
    else std::cout << embr::moduleText(info);
    return info.problems.empty() ? 0 : 1;
}

// embr packages [--json]
static int runPackagesCommand(const std::vector<std::string>& args) {
    bool json = false;
    for (const auto& a : args) {
        if (a == "--json") json = true;
        else { std::cerr << "usage: embr packages [--json]\n"; return 2; }
    }
    auto list = embr::installedPackages();
    if (json) {
        std::cout << "[";
        for (size_t i = 0; i < list.size(); ++i) std::cout << (i ? ",\n " : "\n ") << embr::packageJson(list[i]);
        std::cout << (list.empty() ? "]\n" : "\n]\n");
        return 0;
    }
    size_t wName = 4, wVer = 7, wNeeds = 5;
    for (const auto& p : list) {
        wName = std::max(wName, p.meta.name.size());
        wVer = std::max(wVer, p.meta.version.size());
        wNeeds = std::max(wNeeds, embr::capListText(p.meta.caps).size());
    }
    auto pad = [](std::string s, size_t w) { return s + std::string(w > s.size() ? w - s.size() : 0, ' '); };
    std::cout << pad("NAME", wName) << "  " << pad("VERSION", wVer) << "  " << pad("KIND", 6) << "  " << pad("NEEDS", wNeeds) << "  DESCRIPTION\n";
    for (const auto& p : list) {
        std::cout << pad(p.meta.name, wName) << "  " << pad(p.meta.version.empty() ? "-" : p.meta.version, wVer) << "  " << pad(embr::packageKind(p), 6) << "  "
                  << pad(p.parsed ? embr::capListText(p.meta.caps) : "?", wNeeds) << "  " << p.meta.description << "\n";
        for (const auto& pr : p.problems) std::cout << pad("", wName) << "  problem: " << pr << "\n";
    }
    return 0;
}

// embr package NAME|FILE [--json]
static int runPackageCommand(const std::vector<std::string>& args) {
    bool json = false;
    std::string name, entryUrl;
    for (size_t i = 0; i < args.size(); ++i) {
        const auto& a = args[i];
        if (a == "--json") json = true;
        else if (a == "--registry-entry" && i + 1 < args.size()) entryUrl = args[++i];
        else if (name.empty() && a.rfind("--", 0) != 0) name = a;
        else { std::cerr << "usage: embr package NAME|FILE [--json | --registry-entry URL]\n"; return 2; }
    }
    if (name.empty()) { std::cerr << "usage: embr package NAME|FILE [--json | --registry-entry URL]\n"; return 2; }
    fs::path file;
    if (!embr::findPackageFile(name, file)) { std::cerr << "embr: cannot find package \"" << name << "\"\n"; return 1; }
    embr::PackageInfo info = embr::analyzePackageFile(file);
    if (!entryUrl.empty()) {
        auto bytes = embr::readPackageBytes(file);
        if (!bytes || !info.parsed) { std::cerr << "embr: " << file.string() << " cannot be read as a package\n"; return 1; }
        std::cout << "\"" << info.meta.name << "\": {\"versions\": {\"" << info.meta.version << "\": " << embr::packageRegistryEntry(*bytes, info, entryUrl) << "}}\n";
        return info.problems.empty() ? 0 : 1;
    }
    if (json) std::cout << embr::packageJson(info) << "\n";
    else std::cout << embr::packageText(info);
    return info.problems.empty() && info.parsed ? 0 : 1;
}

static bool loadSigningKey(const std::string& path, unsigned char seed[32]);

// embr pack DIR [-o OUT]
static int runPackCommand(const std::vector<std::string>& args) {
    std::string dir, out, key;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "-o" && i + 1 < args.size()) out = args[++i];
        else if (args[i].rfind("--output=", 0) == 0) out = args[i].substr(9);
        else if (args[i] == "--sign" && i + 1 < args.size()) key = args[++i];
        else if (dir.empty() && args[i].rfind("-", 0) != 0) dir = args[i];
        else { std::cerr << "usage: embr pack DIR [-o OUT.pmbr] [--sign NAME.key]\n"; return 2; }
    }
    if (dir.empty()) { std::cerr << "usage: embr pack DIR [-o OUT.pmbr] [--sign NAME.key]\n"; return 2; }
    unsigned char seed[32];
    if (!key.empty() && !loadSigningKey(key, seed)) return 1;
    std::string image;
    std::vector<std::string> notes;
    std::string why = embr::buildPackage(dir, image, notes);
    if (!why.empty()) { std::cerr << "embr pack: " << why << "\n"; return 1; }
    if (!key.empty()) {
        std::string signedImage;
        if (!embr::signPackageBytes(image, seed, signedImage, why)) { std::cerr << "embr pack: " << why << "\n"; return 1; }
        image = std::move(signedImage);
    }
    if (out.empty()) out = (fs::path(dir).parent_path() / (fs::path(dir).filename().string() + ".pmbr")).string();
    fs::path tmp = out + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write(image.data(), (std::streamsize)image.size());
        f.close();
        if (!f) { std::cerr << "embr pack: cannot write " << out << "\n"; return 1; }
    }
    std::error_code ec;
    fs::rename(tmp, out, ec);
    if (ec) { std::cerr << "embr pack: cannot write " << out << ": " << ec.message() << "\n"; fs::remove(tmp, ec); return 1; }
    for (const auto& n : notes) std::cerr << "embr pack: note: " << n << "\n";
    embr::PackageInfo info = embr::analyzePackageBytes(image, out);
    std::cout << "wrote " << out << " (" << info.meta.name << " " << info.meta.version << ", " << info.file.entries.size() << " files, " << image.size() << " bytes)\n";
    return 0;
}

// the secret seed of a key file, for signing
static bool loadSigningKey(const std::string& path, unsigned char seed[32]) {
    std::string why;
    if (!embr::readKeyFile(path, /*secret=*/true, seed, why)) { std::cerr << "embr: " << why << "\n"; return false; }
    return true;
}

static bool writeFileAtomic(const std::string& out, const std::string& bytes, std::string& why) {
    fs::path tmp = out + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write(bytes.data(), (std::streamsize)bytes.size());
        f.close();
        if (!f) { why = "cannot write " + out; return false; }
    }
    std::error_code ec;
    fs::rename(tmp, out, ec);
    if (ec) { why = "cannot write " + out + ": " + ec.message(); fs::remove(tmp, ec); return false; }
    return true;
}

// embr keygen NAME
static int runKeygenCommand(const std::vector<std::string>& args) {
    if (args.size() != 1) { std::cerr << "usage: embr keygen NAME   (writes NAME.key and NAME.pub)\n"; return 2; }
    const std::string secretFile = args[0] + ".key", publicFile = args[0] + ".pub";
    std::error_code ec;
    if (fs::exists(secretFile, ec) || fs::exists(publicFile, ec)) { std::cerr << "embr keygen: " << secretFile << " or " << publicFile << " exists already, not overwriting\n"; return 1; }
    unsigned char seed[32], pub[32], sec[64];
    if (!embr::ed25519::randomBytes(seed, sizeof seed)) { std::cerr << "embr keygen: the system has no random source\n"; return 1; }
    embr::ed25519::keypair(seed, pub, sec);
    {
        std::ofstream f(secretFile, std::ios::binary | std::ios::trunc);
        f << embr::keyFileText(true, seed);
        f.close();
        if (!f) { std::cerr << "embr keygen: cannot write " << secretFile << "\n"; return 1; }
#if !defined(_WIN32)
        fs::permissions(secretFile, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
#endif
    }
    {
        std::ofstream f(publicFile, std::ios::binary | std::ios::trunc);
        f << embr::keyFileText(false, pub);
        f.close();
        if (!f) { std::cerr << "embr keygen: cannot write " << publicFile << "\n"; return 1; }
    }
    std::cout << "wrote " << secretFile << " (keep it secret) and " << publicFile << "\npublic key: " << embr::hexOf(pub, 32) << "\n";
    return 0;
}

// embr sign FILE.pmbr --key NAME.key [-o OUT]
static int runSignCommand(const std::vector<std::string>& args) {
    std::string file, key, out;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--key" && i + 1 < args.size()) key = args[++i];
        else if (args[i] == "-o" && i + 1 < args.size()) out = args[++i];
        else if (file.empty() && args[i].rfind("-", 0) != 0) file = args[i];
        else { std::cerr << "usage: embr sign FILE.pmbr --key NAME.key [-o OUT.pmbr]\n"; return 2; }
    }
    if (file.empty() || key.empty()) { std::cerr << "usage: embr sign FILE.pmbr --key NAME.key [-o OUT.pmbr]\n"; return 2; }
    unsigned char seed[32];
    if (!loadSigningKey(key, seed)) return 1;
    auto bytes = embr::readPackageBytes(file);
    if (!bytes) { std::cerr << "embr sign: cannot open " << file << "\n"; return 1; }
    std::string signedBytes, why;
    if (!embr::signPackageBytes(*bytes, seed, signedBytes, why)) { std::cerr << "embr sign: " << file << ": " << why << "\n"; return 1; }
    if (out.empty()) out = file;
    if (!writeFileAtomic(out, signedBytes, why)) { std::cerr << "embr sign: " << why << "\n"; return 1; }
    unsigned char sec[64], pub[32];
    embr::ed25519::keypair(seed, pub, sec);
    std::cout << "signed " << out << " with the key " << embr::hexOf(pub, 32) << "\n";
    return 0;
}

// a key given as a file or as 64 hex digits
static bool publicKeyArg(const std::string& arg, std::string& hex) {
    unsigned char k[32];
    std::string why;
    std::error_code ec;
    if (fs::is_regular_file(arg, ec)) {
        if (!embr::readKeyFile(arg, /*secret=*/false, k, why)) { std::cerr << "embr: " << why << "\n"; return false; }
        hex = embr::hexOf(k, 32);
        return true;
    }
    if (!embr::hexToBytes(arg, k, 32)) { std::cerr << "embr: '" << arg << "' is neither a public key file nor 64 hex digits\n"; return false; }
    hex = embr::hexOf(k, 32);
    return true;
}

// embr trust KEY [NOTE...]
static int runTrustCommand(const std::vector<std::string>& args) {
    if (args.empty()) { std::cerr << "usage: embr trust KEY.pub|HEX [NOTE]\n"; return 2; }
    std::string hex, note, why;
    if (!publicKeyArg(args[0], hex)) return 1;
    for (size_t i = 1; i < args.size(); ++i) note += (note.empty() ? "" : " ") + args[i];
    auto keys = embr::readTrustFile(embr::userTrustFile());
    for (const auto& k : keys) if (k.hex == hex) { std::cout << "already trusted: " << hex << "\n"; return 0; }
    keys.push_back({hex, note});
    if (!embr::writeTrustFile(embr::userTrustFile(), keys, why)) { std::cerr << "embr trust: " << why << "\n"; return 1; }
    std::cout << "trusted " << hex << " (" << embr::userTrustFile().string() << ")\n";
    return 0;
}

// embr untrust KEY
static int runUntrustCommand(const std::vector<std::string>& args) {
    if (args.size() != 1) { std::cerr << "usage: embr untrust KEY.pub|HEX\n"; return 2; }
    std::string hex, why;
    if (!publicKeyArg(args[0], hex)) return 1;
    auto keys = embr::readTrustFile(embr::userTrustFile());
    size_t before = keys.size();
    keys.erase(std::remove_if(keys.begin(), keys.end(), [&](const embr::TrustedKey& k) { return k.hex == hex; }), keys.end());
    if (keys.size() == before) { std::cout << "that key was not in your trust list\n"; return 0; }
    if (!embr::writeTrustFile(embr::userTrustFile(), keys, why)) { std::cerr << "embr untrust: " << why << "\n"; return 1; }
    std::cout << "no longer trusted: " << hex << "\n";
    return 0;
}

// embr trusted
static int runTrustedCommand(const std::vector<std::string>&) {
    auto keys = embr::readTrustFile(embr::userTrustFile());
    for (const auto& k : keys) std::cout << k.hex << "  yours" << (k.note.empty() ? "" : "  " + k.note) << "\n";
    std::error_code ec;
    for (const auto& k : embr::projectTrustedKeys(fs::current_path(ec))) std::cout << k << "  this project (embr.json)\n";
    if (keys.empty() && embr::projectTrustedKeys(fs::current_path(ec)).empty()) std::cout << "no trusted keys (" << embr::userTrustFile().string() << ")\n";
    return 0;
}

// embr merge A.pmbr B.pmbr ... -o OUT
static int runMergeCommand(const std::vector<std::string>& args) {
    std::string out;
    std::vector<std::string> in;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "-o" && i + 1 < args.size()) out = args[++i];
        else if (args[i].rfind("--output=", 0) == 0) out = args[i].substr(9);
        else in.push_back(args[i]);
    }
    if (in.size() < 2 || out.empty()) { std::cerr << "usage: embr merge A.pmbr B.pmbr ... -o OUT.pmbr\n"; return 2; }
    std::vector<std::string> images;
    for (const auto& f : in) {
        auto b = embr::readPackageBytes(f);
        if (!b) { std::cerr << "embr merge: cannot open " << f << "\n"; return 1; }
        images.push_back(std::move(*b));
    }
    std::string merged, why;
    if (!embr::mergePackages(images, merged, why)) { std::cerr << "embr merge: " << why << "\n"; return 1; }
    fs::path tmp = out + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write(merged.data(), (std::streamsize)merged.size());
        f.close();
        if (!f) { std::cerr << "embr merge: cannot write " << out << "\n"; return 1; }
    }
    std::error_code ec;
    fs::rename(tmp, out, ec);
    if (ec) { std::cerr << "embr merge: cannot write " << out << ": " << ec.message() << "\n"; fs::remove(tmp, ec); return 1; }
    embr::PackageInfo info = embr::analyzePackageBytes(merged, out);
    std::string t;
    for (const auto& x : info.targets) t += (t.empty() ? "" : ", ") + x;
    std::cout << "wrote " << out << " (" << info.meta.name << " " << info.meta.version << ", " << info.file.entries.size() << " files" << (t.empty() ? "" : ", builds for " + t) << ")\n";
    return 0;
}

// embr unpack FILE DIR
static int runUnpackCommand(const std::vector<std::string>& args) {
    if (args.size() != 2) { std::cerr << "usage: embr unpack FILE DIR\n"; return 2; }
    auto bytes = embr::readPackageBytes(args[0]);
    if (!bytes) { std::cerr << "embr: cannot open " << args[0] << "\n"; return 1; }
    embr::PackageFile pf;
    std::string why;
    if (!embr::readPackageFile(*bytes, pf, why)) { std::cerr << "embr: " << args[0] << ": " << why << "\n"; return 1; }
    fs::path dir = args[1];
    std::error_code ec;
    if (fs::exists(dir, ec) && !fs::is_empty(dir, ec)) { std::cerr << "embr: " << dir.string() << " is not empty\n"; return 1; }
    for (const auto& e : pf.entries) {
        std::string data;
        if (!embr::packageEntryBytes(*bytes, e, data)) { std::cerr << "embr: '" << e.label << "' does not match its sha256 (the file is damaged)\n"; return 1; }
        fs::path f = dir / fs::path(e.label);
        fs::create_directories(f.parent_path(), ec);
        std::ofstream o(f, std::ios::binary | std::ios::trunc);
        o.write(data.data(), (std::streamsize)data.size());
        o.close();
        if (!o) { std::cerr << "embr: cannot write " << f.string() << "\n"; return 1; }
    }
    std::ofstream h(dir / embr::kPackageHeaderFile, std::ios::binary | std::ios::trunc);
    h.write(pf.header.data(), (std::streamsize)pf.header.size());
    std::cout << "unpacked " << pf.entries.size() << " files into " << dir.string() << "\n";
    return 0;
}

static int runDisCommand(const std::vector<std::string>& args) {
    int only = -1;
    std::vector<std::string> paths;
    for (const auto& a : args) {
        if (a.rfind("--chunk=", 0) == 0) only = std::atoi(a.c_str() + 8);
        else paths.push_back(a);
    }
    if (paths.empty()) { std::cerr << "usage: embr dis [--chunk=N] FILE\n"; return 2; }
    int bad = 0;
    for (const auto& path : paths) {
        if (fs::path(path).extension() == ".vmbr") {      // already compiled: just read it back
            auto bytes = embr::vm::readWholeFile(path);
            if (!bytes) { std::cerr << path << ": error: cannot open file\n"; ++bad; continue; }
            try {
                auto prog = embr::vm::deserializeProgram(*bytes, path);
                if (paths.size() > 1) std::cout << "# " << path << "\n";
                if (only >= 0 && (size_t)only < prog.chunks.size()) embr::vm::disassembleChunk(prog, (size_t)only, std::cout);
                else embr::vm::disassemble(prog, std::cout);
                embr::vm::releaseProgram(prog);
            } catch (const embr::EmbrError& e) { std::cerr << path << ": " << e.message << "\n"; ++bad; }
            continue;
        }
        std::ifstream f(path);
        if (!f) { std::cerr << path << ": error: cannot open file\n"; ++bad; continue; }
        std::string src((std::istreambuf_iterator<char>(f)), {});
        auto errors = diagnose(src, path);
        if (!errors.empty()) {
            for (const auto& e : errors) {
                std::cerr << path;
                if (e.hasLocation) std::cerr << ":" << e.range.startLine << ":" << e.range.startCol;
                std::cerr << ": " << e.message << "\n";
            }
            ++bad;
            continue;
        }
        try {
            embr::Interpreter interp;
            embr::vm::CompileOptions copts;   // same as running it, so the listing matches a cached copy
            copts.closedWorld = true;
            copts.preloadPlugins = true;
            auto prog = embr::vm::compileSource(src, interp, path, copts);
            if (paths.size() > 1) std::cout << "# " << path << "\n";
            if (only >= 0) {
                if ((size_t)only >= prog.chunks.size()) { std::cerr << path << ": error: no chunk " << only << " (it has " << prog.chunks.size() << ")\n"; ++bad; continue; }
                embr::vm::disassembleChunk(prog, (size_t)only, std::cout);
            } else {
                embr::vm::disassemble(prog, std::cout);
            }
        } catch (const embr::EmbrError& e) {
            std::cerr << path << ": " << e.message << "\n";
            ++bad;
        }
    }
    return bad ? 1 : 0;
}

// `embr compile FILE [-o OUT]`: compiles a script and writes it as a .vmbr file, by default next to the script
// (name.embr -> name.vmbr). nothing is run. -o is only for a single file
static int runCompileCommand(const std::vector<std::string>& args) {
    std::string out;
    std::vector<std::string> paths;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "-o" && i + 1 < args.size()) out = args[++i];
        else if (args[i].rfind("--output=", 0) == 0) out = args[i].substr(9);
        else paths.push_back(args[i]);
    }
    if (paths.empty() || (!out.empty() && paths.size() > 1)) { std::cerr << "usage: embr compile FILE [-o OUT]   (-o only with one FILE)\n"; return 2; }
    int bad = 0;
    for (const auto& path : paths) {
        std::ifstream f(path);
        if (!f) { std::cerr << path << ": error: cannot open file\n"; ++bad; continue; }
        std::string src((std::istreambuf_iterator<char>(f)), {});
        auto errors = diagnose(src, path);
        if (!errors.empty()) {
            for (const auto& e : errors) {
                std::cerr << path;
                if (e.hasLocation) std::cerr << ":" << e.range.startLine << ":" << e.range.startCol;
                std::cerr << ": " << e.message << "\n";
            }
            ++bad;
            continue;
        }
        try {
            embr::Interpreter interp;
            auto prog = embr::vm::compileSource(src, interp, path);
            std::string dest = out.empty() ? fs::path(path).replace_extension(".vmbr").string() : out;
            std::error_code ec;
            // the file carries what the script says about itself (`embr module FILE.vmbr` reads it back, and a host that denies
            // capabilities can refuse the module before it runs)
            std::string meta = embr::moduleMetaBlob(src, fs::absolute(path, ec).string());
            if (!embr::vm::writeCompiledFile(dest, prog, embr::vm::hashSource(src), fs::absolute(path, ec).string(), meta)) {
                std::cerr << dest << ": error: cannot write file\n";
                ++bad;
                continue;
            }
            std::cout << "wrote " << dest << "\n";
        } catch (const embr::EmbrError& e) {
            std::cerr << path << ": " << e.message << "\n";
            ++bad;
        }
    }
    return bad ? 1 : 0;
}

// `embr cache clean [--all] DIR`: removes the .vmbr files in DIR that can't be used any more (damaged, from another
// build, script changed or gone), or every one with --all. other files are left alone
static int runCacheCommand(const std::vector<std::string>& args) {
    bool all = false, packages = false;
    std::vector<std::string> rest;
    for (const auto& a : args) { if (a == "--all") all = true; else if (a == "--packages") packages = true; else rest.push_back(a); }
    if (packages) {      // unpacked packages (core/package.h), in EMBR_PACKAGE_DIR or the user's cache folder
        if (rest.size() != 1 || rest[0] != "clean") { std::cerr << "usage: embr cache clean --packages [--all]\n"; return 2; }
        auto gone = embr::cleanPackageCache(embr::packageCacheRoot(), all);
        for (const auto& g : gone) std::cout << "removed " << g << "\n";
        std::cout << "removed " << gone.size() << (gone.size() == 1 ? " folder" : " folders") << " from " << embr::packageCacheRoot().string() << "\n";
        return 0;
    }
    if (rest.size() != 2 || rest[0] != "clean") { std::cerr << "usage: embr cache clean [--all] DIR | embr cache clean --packages [--all]\n"; return 2; }
    std::error_code ec;
    if (!fs::is_directory(rest[1], ec)) { std::cerr << rest[1] << ": error: not a folder\n"; return 2; }
    size_t n = embr::vm::cleanCache(rest[1], all);
    std::cout << "removed " << n << (n == 1 ? " file" : " files") << "\n";
    return 0;
}

// `embr fmt [--check] [--stdout] [--indent=N] FILE...`: formats each file in place (see fmt.h). --check writes nothing and
// exits 1 if any file would change; --stdout prints the result instead of writing it. exit status 2 if a file
// can't be formatted (it doesn't lex, or can't be read).
static int runFmtCommand(const std::vector<std::string>& args) {
    bool check = false, toStdout = false;
    int indent = 4;
    std::vector<std::string> paths;
    for (const auto& a : args) {
        if (a.rfind("--indent=", 0) == 0) {
            indent = std::atoi(a.c_str() + 9);
            if (indent < 1 || indent > 16) { std::cerr << "embr fmt: --indent must be 1 to 16\n"; return 2; }
        }
        else if (a == "--check") check = true;
        else if (a == "--stdout") toStdout = true;
        else paths.push_back(a);
    }
    if (paths.empty()) { std::cerr << "usage: embr fmt [--check] [--stdout] [--indent=N] FILE...\n"; return 2; }
    int status = 0;
    for (const auto& path : paths) {
        std::ifstream f(path, std::ios::binary);
        if (!f) { std::cerr << path << ": error: cannot open file\n"; status = 2; continue; }
        std::string src((std::istreambuf_iterator<char>(f)), {});
        std::string out;
        try {
            out = embr_fmt::formatSource(src, indent);
        } catch (const embr::EmbrError& e) {
            std::cerr << path << ": error: " << e.context << ": " << e.message << "\n";
            status = 2; continue;
        } catch (const embr_fmt::FormatError& e) {
            std::cerr << path << ": error: " << e.what() << " (file left untouched)\n";
            status = 2; continue;
        }
        if (toStdout) { std::cout << out; continue; }
        if (out == src) continue;
        if (check) { std::cout << path << "\n"; if (status == 0) status = 1; continue; }
        std::ofstream o(path, std::ios::binary | std::ios::trunc);
        if (!o || !(o << out)) { std::cerr << path << ": error: cannot write file\n"; status = 2; }
    }
    return status;
}

// `embr test [--filter=TEXT] [PATH...]`: runs script tests. PATH is a file or a directory (searched recursively for
// test_*.embr / *_test.embr, default "."). each file gets a fresh interpreter (on the backend chosen with --backend), and
// every top-level function named test_* runs on its own inside a try/catch, in name order, so one failure doesn't
// stop the rest. an optional setup() runs before each test and teardown() after it (also after a failure). a file
// that fails to parse, or whose top level raises, counts as one failed test. prints PASS/FAIL per test (with the
// error and call trace) and a summary. exit status: 0 all passed, 1 any failure, 2 nothing to run / usage.
// assertions are in modules/testing.embr
struct TestTotals { int passed = 0, failed = 0; };

static void runTestFile(const fs::path& file, const std::string& filter, TestTotals& tot) {
    std::cout << file.string() << "\n";
    auto failFile = [&](const std::string& why) {
        std::cout << "  FAIL (while loading)\n";
        std::istringstream ls(why);
        for (std::string line; std::getline(ls, line); ) std::cout << "        " << line << "\n";
        ++tot.failed;
    };
    std::ifstream f(file);
    if (!f) { failFile("cannot open file"); return; }
    std::string src((std::istreambuf_iterator<char>(f)), {});

    embr::Interpreter interp;
    interp.pluginPolicy = g_policy;
    interp.scriptDir = fs::absolute(file).parent_path().string();

    std::unique_ptr<embr::vm::CompiledProgram> compiled;   // must outlive the runner that executes it
    std::unique_ptr<embr::vm::VmRunner>        vmRunner;
    try {
        embr::Lexer lex(src);
        auto tokens = lex.tokenize();
        embr::SourceMap sm = lex.sourceMap();
        std::vector<embr::EmbrError> perr;
        embr::Parser parser(std::move(tokens), sm);
        parser.collectErrors(&perr);
        auto prog = parser.parse();
        if (!perr.empty()) {
            std::string why;
            for (const auto& e : perr) why += e.message + (e.hasLocation ? " (line " + std::to_string(e.range.startLine) + ")" : "") + "\n";
            failFile(why); return;
        }
        // backend switch: a new backend needs its own case here (and in runSourceDispatch above)
        compiled = std::make_unique<embr::vm::CompiledProgram>(embr::vm::compileSource(src, interp, file.string()));
        vmRunner = std::make_unique<embr::vm::VmRunner>(interp);
        vmRunner->run(*compiled);
    } catch (const embr::EmbrError& e) {
        failFile(e.message + embr::formatTrace(e)); return;
    }

    std::vector<std::string> names;
    for (const auto& [name, val] : interp.globals())
        if (name.rfind("test_", 0) == 0 && val.isCallable() &&
            (filter.empty() || name.find(filter) != std::string::npos))
            names.push_back(name);
    std::sort(names.begin(), names.end());
    if (names.empty()) { std::cout << "  (no test_* functions" << (filter.empty() ? "" : " matching the filter") << ")\n"; return; }

    auto call = [&](const std::string& fn) { embr::invoke(interp, interp.getGlobal(fn), {}); };
    const bool hasSetup = interp.hasGlobal("setup") && interp.getGlobal("setup").isCallable();
    const bool hasTeardown = interp.hasGlobal("teardown") && interp.getGlobal("teardown").isCallable();
    for (const auto& name : names) {
        bool ok = true;
        std::string msg, trace;
        try {
            if (hasSetup) call("setup");
            call(name);
        } catch (const embr::EmbrError& e) { ok = false; msg = e.message; trace = embr::formatTrace(e); }
        if (hasTeardown) {
            try { call("teardown"); }
            catch (const embr::EmbrError& e) { if (ok) { ok = false; msg = "teardown failed: " + e.message; trace = embr::formatTrace(e); } }
        }
        if (ok) { std::cout << "  PASS " << name << "\n"; ++tot.passed; continue; }
        std::cout << "  FAIL " << name << "\n";
        std::istringstream ms(msg + "\n" + trace);
        for (std::string line; std::getline(ms, line); ) if (!line.empty()) std::cout << "        " << line << "\n";
        ++tot.failed;
    }
}

static int runTestCommand(const std::vector<std::string>& args) {
    std::string filter;
    std::vector<fs::path> roots;
    for (const auto& a : args) {
        if (a.rfind("--filter=", 0) == 0) filter = a.substr(9);
        else roots.emplace_back(a);
    }
    if (roots.empty()) roots.emplace_back(".");

    std::vector<fs::path> files;
    for (const auto& r : roots) {
        std::error_code ec;
        if (fs::is_directory(r, ec)) {
            for (const auto& e : fs::recursive_directory_iterator(r, ec)) {
                if (!e.is_regular_file()) continue;
                std::string n = e.path().filename().string();
                auto endsWith = [&](const std::string& suffix) {
                    return n.size() > suffix.size() && n.compare(n.size() - suffix.size(), suffix.size(), suffix) == 0;
                };
                bool isTest = endsWith(".embr") && (n.rfind("test_", 0) == 0 || endsWith("_test.embr"));
                if (isTest) files.push_back(e.path());
            }
        } else if (fs::is_regular_file(r, ec)) files.push_back(r);
        else { std::cerr << "embr test: no such file or directory: " << r.string() << "\n"; return 2; }
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) { std::cerr << "embr test: no test files found (test_*.embr or *_test.embr)\n"; return 2; }

    TestTotals tot;
    for (const auto& f : files) runTestFile(f, filter, tot);
    std::cout << "\n" << tot.passed << " passed, " << tot.failed << " failed\n";
    return tot.failed ? 1 : 0;
}

// nesting depth
//   +1 for each block-opener keyword (if, fn, while)
//   -1 for end
static int blockDelta(const std::string& line) {
    static const std::unordered_map<std::string,int> delta = {
        {"if",1},{"fn",1},{"while",1},{"for",1},
        {"elif",0},{"else",0},
        {"end",-1},
    };
    std::istringstream ss(line);
    std::string word; int d = 0;
    while (ss >> word) {
        auto it = delta.find(word);
        if (it != delta.end()) d += it->second;
    }
    return d;
}

static int totalDepth(const std::vector<std::string>& lines) {
    int d = 0;
    for (auto& l : lines) d += blockDelta(l);
    return d;
}

// ---------------------------------------------------------------------------
// raw terminal input
//
// on POSIX, RawMode puts the tty in non-canonical, no-echo mode while it exists and restores it when destroyed
// on Windows _getch() already reads unbuffered and unechoed, so RawMode does nothing
// arrow keys arrive as ESC '[' 'A'/'B'/'C'/'D' on POSIX, and as a 0/224 prefix byte then 72/80/75/77 on Windows
// ---------------------------------------------------------------------------
namespace term {

#ifdef _WIN32
struct RawMode { RawMode() {} ~RawMode() {} };
inline int rawByte() { return _getch(); }
#else
struct RawMode {
    termios orig{};
    bool    ok = false;
    RawMode() {
        if (tcgetattr(STDIN_FILENO, &orig) == 0) {
            termios raw = orig;
            raw.c_lflag &= ~(ICANON | ECHO | ISIG);
            raw.c_cc[VMIN]  = 1;
            raw.c_cc[VTIME] = 0;
            // TCSADRAIN, not TCSAFLUSH: flush would throw away what was already typed or pasted for the next line
            if (tcsetattr(STDIN_FILENO, TCSADRAIN, &raw) == 0) ok = true;
        }
    }
    ~RawMode() { if (ok) tcsetattr(STDIN_FILENO, TCSADRAIN, &orig); }
    RawMode(const RawMode&) = delete;
    RawMode& operator=(const RawMode&) = delete;
};
inline int rawByte() {
    char c;
    ssize_t n = ::read(STDIN_FILENO, &c, 1);
    if (n <= 0) return -1;
    return (unsigned char)c;
}
#endif


enum class Key { Char, Enter, Backspace, Up, Down, Left, Right, Eof, Interrupt, Other };

struct KeyEvent { Key key; char ch = 0; };

inline KeyEvent readKey() {
#ifdef _WIN32
    int c = rawByte();
    if (c == EOF)  return {Key::Eof};
    if (c == 3)    return {Key::Interrupt};
    if (c == '\r' || c == '\n') return {Key::Enter};
    if (c == 8)    return {Key::Backspace};
    if (c == 0 || c == 224) {
        int c2 = rawByte();
        switch (c2) {
            case 72: return {Key::Up};
            case 80: return {Key::Down};
            case 75: return {Key::Left};
            case 77: return {Key::Right};
        }
        return {Key::Other};
    }
    return {Key::Char, (char)c};
#else
    int c = rawByte();
    if (c < 0)  return {Key::Eof};
    if (c == 3) return {Key::Interrupt};   // Ctrl-C
    if (c == 4) return {Key::Eof};         // Ctrl-D
    if (c == '\r' || c == '\n') return {Key::Enter};
    if (c == 127 || c == 8) return {Key::Backspace};
    if (c == 27) {
        int c1 = rawByte();
        if (c1 == '[') {
            int c2 = rawByte();
            switch (c2) {
                case 'A': return {Key::Up};
                case 'B': return {Key::Down};
                case 'C': return {Key::Right};
                case 'D': return {Key::Left};
            }
        }
        return {Key::Other};
    }
    return {Key::Char, (char)c};
#endif
}

} // namespace term

// ---------------------------------------------------------------------------
// single-line editor: history navigation with up/down, left/right cursor
// movement, backspace. behaves like a plain unix shell prompt.
// ---------------------------------------------------------------------------
struct LineResult { std::string text; bool eof = false; bool interrupted = false; };

static LineResult readLineSingle(std::vector<std::string>& history, const std::string& prompt) {
    term::RawMode raw;

    std::string buf;
    size_t      cursor = 0;
    int         histIdx = (int)history.size(); // one past the end = "editing a new line"
    std::string saved;                          // buffer stashed while browsing history

    auto redraw = [&]() {
        std::cout << "\r\x1b[2K" << prompt << buf;
        size_t back = buf.size() - cursor;
        if (back > 0) std::cout << "\x1b[" << back << "D";
        std::cout << std::flush;
    };

    redraw();

    while (true) {
        term::KeyEvent ev = term::readKey();
        switch (ev.key) {
            case term::Key::Enter:
                std::cout << "\n";
                return { buf, false, false };
            case term::Key::Eof:
                std::cout << "\n";
                return { "", true, false };
            case term::Key::Interrupt:
                std::cout << "^C\n";
                return { "", false, true };
            case term::Key::Backspace:
                if (cursor > 0) { buf.erase(cursor - 1, 1); --cursor; redraw(); }
                break;
            case term::Key::Left:
                if (cursor > 0) { --cursor; redraw(); }
                break;
            case term::Key::Right:
                if (cursor < buf.size()) { ++cursor; redraw(); }
                break;
            case term::Key::Up:
                if (histIdx > 0) {
                    if (histIdx == (int)history.size()) saved = buf;
                    --histIdx;
                    buf = history[histIdx];
                    cursor = buf.size();
                    redraw();
                }
                break;
            case term::Key::Down:
                if (histIdx < (int)history.size()) {
                    ++histIdx;
                    buf = (histIdx == (int)history.size()) ? saved : history[histIdx];
                    cursor = buf.size();
                    redraw();
                }
                break;
            case term::Key::Char:
                buf.insert(buf.begin() + (long)cursor, ev.ch);
                ++cursor;
                redraw();
                break;
            default:
                break;
        }
    }
}

// ---------------------------------------------------------------------------
// multi-line editor: used once a block opener (if/fn/while) has been typed
// up/down move between lines, left/right move within and across lines, like a small text editor. Enter splits
// the current line. down on the last line makes a temporary blank line below. Enter on an empty last line
// (with balanced block depth) runs the whole buffer
// ---------------------------------------------------------------------------
struct MultiLineResult { std::optional<std::string> source; bool eof = false; };

static std::string promptFor(size_t row) { return row == 0 ? ">>> " : "... "; }

static MultiLineResult runMultiLineEditor(std::vector<std::string> lines, int startRow) {
    term::RawMode raw;

    int row = startRow;
    int col = (int)lines[row].size();
    int screenLines = (int)lines.size();

    auto redraw = [&]() {
        if (screenLines > 1) std::cout << "\x1b[" << (screenLines - 1) << "A";
        std::cout << "\r\x1b[0J";
        for (size_t i = 0; i < lines.size(); ++i) {
            std::cout << promptFor(i) << lines[i];
            if (i + 1 < lines.size()) std::cout << "\n";
        }
        screenLines = (int)lines.size();

        int upMove = (int)lines.size() - 1 - row;
        if (upMove > 0) std::cout << "\x1b[" << upMove << "A";
        std::cout << "\r";
        int rightMove = 4 /* prompt width */ + col;
        if (rightMove > 0) std::cout << "\x1b[" << rightMove << "C";
        std::cout << std::flush;
    };

    redraw();

    while (true) {
        term::KeyEvent ev = term::readKey();
        switch (ev.key) {
            case term::Key::Eof:
                std::cout << "\n";
                return { std::nullopt, true };

            case term::Key::Interrupt:
                std::cout << "\n^C\n";
                return { std::nullopt, false };

            case term::Key::Left:
                if (col > 0) {
                    --col;
                } else if (row > 0) {
                    --row;
                    col = (int)lines[row].size();
                }
                redraw();
                break;

            case term::Key::Right:
                if (col < (int)lines[row].size()) {
                    ++col;
                } else if (row + 1 < (int)lines.size()) {
                    ++row;
                    col = 0;
                }
                redraw();
                break;

            case term::Key::Up:
                if (row > 0) {
                    --row;
                    col = std::min(col, (int)lines[row].size());
                    redraw();
                }
                break;

            case term::Key::Down:
                if (row + 1 < (int)lines.size()) {
                    ++row;
                    col = std::min(col, (int)lines[row].size());
                } else {
                    // at the last line: open a temporary blank line so
                    // Enter can be used there to submit the block
                    lines.push_back("");
                    ++row;
                    col = 0;
                }
                redraw();
                break;

            case term::Key::Backspace:
                if (col > 0) {
                    lines[row].erase((size_t)col - 1, 1);
                    --col;
                } else if (row > 0) {
                    int prevLen = (int)lines[row - 1].size();
                    lines[row - 1] += lines[row];
                    lines.erase(lines.begin() + row);
                    --row;
                    col = prevLen;
                }
                redraw();
                break;

            case term::Key::Enter: {
                bool isLastLine  = (row + 1 == (int)lines.size());
                bool lineIsEmpty = lines[row].empty();

                if (isLastLine && lineIsEmpty && totalDepth(lines) <= 0) {
                    std::cout << "\n";
                    // drop the trailing blank line(s) used purely for
                    // navigation before joining into source
                    while (!lines.empty() && lines.back().empty())
                        lines.pop_back();
                    std::string src;
                    for (auto& l : lines) src += l + "\n";
                    return { src, false };
                }

                // normal editor behaviour: split the line at the cursor
                std::string tail = lines[row].substr((size_t)col);
                lines[row].resize((size_t)col);
                lines.insert(lines.begin() + row + 1, tail);
                ++row;
                col = 0;
                redraw();
                break;
            }

            case term::Key::Char:
                lines[row].insert(lines[row].begin() + col, ev.ch);
                ++col;
                redraw();
                break;

            default:
                break;
        }
    }
}

// ---------------------------------------------------------------------------
// REPL
// ---------------------------------------------------------------------------
// true if src lexes and parses with no errors. the REPL uses it to ask "is this line an expression?"
static bool parsesClean(const std::string& src) {
    try {
        embr::Lexer lex(src);
        auto tokens = lex.tokenize();
        embr::SourceMap sm = lex.sourceMap();
        embr::Parser parser(std::move(tokens), sm);
        std::vector<embr::EmbrError> errors;
        parser.collectErrors(&errors);
        parser.parse();
        return errors.empty();
    } catch (const embr::EmbrError&) {
        return false;
    }
}

static bool stdinIsTty() {
#ifdef _WIN32
    return _isatty(0) != 0;
#else
    return isatty(0) != 0;
#endif
}

// a call like print(x) returns 0 because every native needs some value; echoing that 0 would be noise
static bool looksLikeCall(const std::string& line) {
    return !line.empty() && line.back() == ')' &&
           (std::isalpha((unsigned char)line[0]) || line[0] == '_');
}

static void repl(embr::Interpreter& interp) {
    const bool tty = stdinIsTty();
    if (tty) std::cout << "embr REPL  (type 'exit' to quit)\n";
    std::vector<std::string> history;

    // prints the value of a bare expression line, see the wrapping in the loop below
    bool echoZero = true;
    interp.bindSig("__repl_show", {embr::Param::req("val")},
    [&echoZero](const std::vector<embr::Value>& args) -> embr::Value {
        const embr::Value& v = args[0];
        if (!(v.isNumber() && v.asNumber() == 0.0 && !echoZero))
            std::cout << embr::valueRepr(v) << "\n";
        return embr::Value(0.0);
    });

    while (true) {
        LineResult first;
        if (tty) {
            first = readLineSingle(history, ">>> ");
        } else {
            // piped input: no raw mode, no prompt, no keystroke echo
            if (!std::getline(std::cin, first.text)) first.eof = true;
        }
        if (first.eof) break;
        if (first.interrupted) continue;

        std::string line = first.text;
        if (line.empty()) continue;
        if (line == "exit") break;

        if (!history.empty() && history.back() == line) {
            // don't spam duplicate consecutive entries
        } else {
            history.push_back(line);
        }

        int depth = blockDelta(line);

        std::string source;
        if (depth <= 0) {
            // single-line mode: run immediately
            source = line + "\n";
            // a bare expression prints its value: wrap it in __repl_show(...) when that still parses
            // (statements like `x = 1` or `if ...` don't, so they run as written)
            std::string wrapped = "__repl_show(" + line + ")\n";
            if (parsesClean(wrapped)) {
                echoZero = !looksLikeCall(line);
                source = wrapped;
            }
        } else if (!tty) {
            // piped input: keep reading lines until the block closes
            source = line + "\n";
            std::string more;
            while (depth > 0 && std::getline(std::cin, more)) {
                source += more + "\n";
                depth += blockDelta(more);
            }
        } else {
            // multi-line mode: hand off to the block editor
            std::vector<std::string> lines = { line, "" };
            MultiLineResult mres = runMultiLineEditor(std::move(lines), 1);
            if (mres.eof) break;
            if (!mres.source) continue; // cancelled (Ctrl-C)
            source = *mres.source;
            history.push_back(source); // recall the whole block later, too
        }

        try {
            runSourceDispatch(source, interp, "<repl>");
        } catch (const embr::EmbrError& e) {
            std::cerr << e.what();
        } catch (const std::exception& e) {
            std::cerr << "[error] " << e.what() << "\n";
        }
    }
}

int main(int argc, char** argv) {
    // print() writes to a buffered stdout, which a crash or a kill throws away. EMBR_UNBUFFERED=1 flushes every
    // write, so the output of a script that hangs or crashes shows how far it got
    if (const char* ub = std::getenv("EMBR_UNBUFFERED"); ub && *ub == '1') std::cout << std::unitbuf;
    embr::Interpreter interp;
    // declared after interp, so it is destroyed BEFORE it on every return path (locals die in reverse order)
    struct VmRunnerRelease { ~VmRunnerRelease() { g_vmRunner.reset(); } } vmRunnerRelease;
    interp.bindSig("ast", {embr::Param::req("val", embr::TS::Str)},
    [](const std::vector<embr::Value>& args) -> embr::Value {
        embr_debug::printAstFromSource(args[0].asString());
        return embr::Value(0.0);
    });
    interp.bindSig("lex", {embr::Param::req("val", embr::TS::Str)},
    [](const std::vector<embr::Value>& args) -> embr::Value {
        embr::Lexer lex(args[0].asString());
        std::vector<embr::Token> tokens = lex.tokenize();
        for (size_t i = 0; i < tokens.size(); ++i)
            std::cout << "   " << tokens[i].text;
        return embr::Value(0.0);
    });
#ifdef _WIN32
    embr::enableAnsi();
#endif
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--" && !files.empty() && files[0] != "pkg") {
            for (++i; i < argc; ++i) interp.scriptArgs.push_back(argv[i]);
            break;
        }
        if (arg == "--sandbox") {
            // denies every capability: a plugin that needs the filesystem, network, processes, the environment, threads or
            // foreign code is refused, whatever its name. embrlib's file functions switch off with the filesystem
            g_policy.restricted = true; g_policy.anyName = true; g_policy.fileIo = false;
            g_policy.deniedCaps |= EMBR_CAP_ALL;
            continue;
        }
        if (arg.rfind("--deny-caps=", 0) == 0 && (files.empty() || files[0] != "pkg")) {   // after `pkg` it is pkg's own option
            // denies just these capabilities (filesystem, network, process, env, threads, native). plugins load only from the
            // places the host controls, like --sandbox, and embrlib's file functions switch off if filesystem is denied
            uint64_t mask = 0;
            std::string bad;
            if (!embr::capMask(arg.substr(std::string("--deny-caps=").size()), mask, bad)) {
                std::cerr << "embr: --deny-caps: '" << bad << "' is not a capability (filesystem, network, process, env, threads, native)\n";
                return 2;
            }
            g_policy.restricted = true; g_policy.anyName = true;
            g_policy.deniedCaps |= mask;
            if (mask & EMBR_CAP_FILESYSTEM) g_policy.fileIo = false;
            continue;
        }
        if (arg == "--require-signed" && (files.empty() || files[0] != "pkg")) { g_policy.requireSigned = true; continue; }   // after `pkg` it is pkg's own option
        if (arg == "--no-plugins") { g_policy.restricted = true; g_policy.anyName = false; g_policy.fileIo = false; g_policy.allowed.clear(); continue; }
        if (arg.rfind("--allow-plugins=", 0) == 0) {
            // an exact allow-list; does not add the sandbox set and keeps embrlib's file natives ON
            // unless --sandbox/--no-plugins also appears (combine them for both)
            if (!g_policy.restricted) g_policy.fileIo = true;
            g_policy.restricted = true;
            g_policy.anyName = false;
            g_policy.allowed.clear();
            std::string list = arg.substr(std::string("--allow-plugins=").size()), cur;
            for (char ch : list + ",") {
                if (ch == ',') { if (!cur.empty()) g_policy.allowed.insert(cur); cur.clear(); }
                else cur += ch;
            }
            continue;
        }
        if (arg == "--version") {
            std::cout << "embr " EMBR_VERSION " (build/" EMBR_BUILD_NAME "; backends: " << backendSummary() << ")\n";
            return 0;
        }
        if (arg.rfind("--cache-dir=", 0) == 0) { g_cacheDir = arg.substr(std::string("--cache-dir=").size()); continue; }
        if (arg.rfind("--max-steps=", 0) == 0 || arg.rfind("--timeout=", 0) == 0) {
            bool steps = arg[2] == 'm';
            std::string v = arg.substr(arg.find('=') + 1);
            char* end = nullptr;
            double n = std::strtod(v.c_str(), &end);
            if (v.empty() || *end || !(n > 0) || (steps && n >= 1.8e19)) {
                std::cerr << "embr: " << arg.substr(0, arg.find('=')) << " needs a number above 0\n";
                return 2;
            }
            if (steps) g_maxSteps = (uint64_t)n; else g_timeout = n;
            continue;
        }
        if (arg == "--vm") continue;       // the VM is the default backend now, the flag stays so old scripts work
        if (arg == "--backend" || arg.rfind("--backend=", 0) == 0) {
            std::string name;
            if (arg == "--backend") { if (i + 1 < argc) name = argv[++i]; }
            else name = arg.substr(std::string("--backend=").size());
            const auto& known = embr::backendNames();
            if (std::find(known.begin(), known.end(), name) == known.end()) {
                std::cerr << "embr: unknown backend '" << name << "' (this build has: " << backendSummary() << ")\n"
                          << kSharedBinHint;
                return 2;
            }
            // with one backend there is nothing to switch. a second one stores the name here and gets a case in
            // runSourceDispatch() and runTestCommand()
            continue;
        }
        files.push_back(std::move(arg));
    }
    interp.pluginPolicy = g_policy;
    interp.cacheDir = g_cacheDir;
    interp.setBudget(g_maxSteps, g_timeout);
    if (!files.empty() && files[0] == "test") {
        return runTestCommand(std::vector<std::string>(files.begin() + 1, files.end()));
    }
    if (!files.empty() && files[0] == "check") {
        if (files.size() < 2) { std::cerr << "usage: embr check FILE...\n"; return 2; }
        return checkFiles(std::vector<std::string>(files.begin() + 1, files.end())) ? 1 : 0;
    }
    if (!files.empty() && files[0] == "compile") {
        return runCompileCommand(std::vector<std::string>(files.begin() + 1, files.end()));
    }
    if (!files.empty() && files[0] == "cache") {
        return runCacheCommand(std::vector<std::string>(files.begin() + 1, files.end()));
    }
    if (!files.empty() && files[0] == "modules") {
        return runModulesCommand(std::vector<std::string>(files.begin() + 1, files.end()));
    }
    if (!files.empty() && files[0] == "module") {
        return runModuleCommand(std::vector<std::string>(files.begin() + 1, files.end()));
    }
    if (!files.empty() && files[0] == "packages") {
        return runPackagesCommand(std::vector<std::string>(files.begin() + 1, files.end()));
    }
    if (!files.empty() && files[0] == "package") {
        return runPackageCommand(std::vector<std::string>(files.begin() + 1, files.end()));
    }
    if (!files.empty() && files[0] == "pack") {
        return runPackCommand(std::vector<std::string>(files.begin() + 1, files.end()));
    }
    if (!files.empty() && files[0] == "abi") {
        std::cout << embr::abiTag() << "\n" << embr::buildId() << "\n";
        return 0;
    }
    if (!files.empty() && files[0] == "keygen") {
        return runKeygenCommand(std::vector<std::string>(files.begin() + 1, files.end()));
    }
    if (!files.empty() && files[0] == "sign") {
        return runSignCommand(std::vector<std::string>(files.begin() + 1, files.end()));
    }
    if (!files.empty() && files[0] == "trust") {
        return runTrustCommand(std::vector<std::string>(files.begin() + 1, files.end()));
    }
    if (!files.empty() && files[0] == "untrust") {
        return runUntrustCommand(std::vector<std::string>(files.begin() + 1, files.end()));
    }
    if (!files.empty() && files[0] == "trusted") {
        return runTrustedCommand(std::vector<std::string>(files.begin() + 1, files.end()));
    }
    if (!files.empty() && files[0] == "merge") {
        return runMergeCommand(std::vector<std::string>(files.begin() + 1, files.end()));
    }
    if (!files.empty() && files[0] == "unpack") {
        return runUnpackCommand(std::vector<std::string>(files.begin() + 1, files.end()));
    }
    if (!files.empty() && files[0] == "plugins") {
        return runPluginsCommand(std::vector<std::string>(files.begin() + 1, files.end()));
    }
    if (!files.empty() && files[0] == "plugin") {
        return runPluginCommand(std::vector<std::string>(files.begin() + 1, files.end()));
    }
    if (!files.empty() && files[0] == "dis") {
        return runDisCommand(std::vector<std::string>(files.begin() + 1, files.end()));
    }
    if (!files.empty() && files[0] == "lsp") {
        embr_lsp::Server server(std::cin, std::cout, diagnose);
        return server.run();
    }
    if (!files.empty() && files[0] == "fmt") {
        return runFmtCommand(std::vector<std::string>(files.begin() + 1, files.end()));
    }
    if (!files.empty() && files[0] == "pkg") {
        std::vector<std::string> pkgArgs(files.begin() + 1, files.end());
        return runPkgCommand(pkgArgs, interp);
    }
    if (!files.empty()) {
        // every file runs, even after one fails (they share one interpreter). the exit code says if any failed
        int status = 0;
        for (const auto& f : files) if (!runFile(f, interp)) status = 1;
        return status;
    }
    repl(interp);
    return 0;
}
