// fuzz.cpp
// a dependency-free fuzz driver for embr (works with gcc, no libFuzzer needed)
//
//   frontend  mutated source bytes -> Lexer -> Parser -> VM compiler. arbitrary bytes may only raise an EmbrError:
//             never crash, hang, leak another exception type, or overflow the stack. compileSource() also runs the
//             stack checker over what the compiler made, so a "vmbr" error here is a finding
//   fmt       mutated source bytes -> the `embr fmt` formatter. it must refuse (the source doesn't lex) or return
//             text that formats to itself. FormatError, or a second pass that changes anything, is a finding
//   json      (only in the -DFUZZ_JSON build) mutated bytes -> json_parse, plus a round trip: whatever json_parse
//             accepts, json_stringify -> json_parse must accept too. json_stringify may refuse invalid UTF-8,
//             then the {"lossy": 1} form must round-trip
//   <plugin>  (only in a -DFUZZ_PLUGIN_SRC/-DFUZZ_PLUGIN_NAME build: csv encoding path regex str unicode time vec)
//             mutated bytes -> that plugin's functions, called from a small embr script. the input is split at the
//             first \x01 into two strings, and a number is derived from the bytes. a script may raise (fine).
//             crashes, hangs, sanitizer reports and a printed ROUNDTRIP-FAIL (csv, encoding, vec) are findings
//   vmbr      mutated bytes of real compiled scripts (.vmbr images) -> vm::deserializeProgram. arbitrary bytes may only
//             raise an EmbrError. what it accepts must disassemble, and write -> read -> write must give the same bytes.
//             most inputs get their checksum fixed up again, otherwise nearly everything dies at the checksum and the
//             parser and validator are barely reached. what loads is then run for a limited number of steps with no
//             plugins and no file access: the checks (indexes, stack balance, captured variables) promise that it
//             can't crash the VM, whatever it does
//   pmbr      mutated bytes of small .pmbr package files -> readPackageFile + analyzePackageBytes (core/package.h). arbitrary bytes
//             may only be refused with a reason: never crash, hang or read outside the input. most inputs get the sha256 of
//             their table of contents fixed up again, otherwise nearly everything dies there. what is accepted must have every
//             entry inside the file, and its labels must be safe names. a signed package is one of the seeds
//   programs  small generated valid programs run twice on the VM, each in a fresh Interpreter: once as compiled normally,
//             once compiled without the in-place fast ops (Compiler::Options::inPlaceOps). stdout and "did it error"
//             must match, and the sanitizers watch both runs. so it finds wrong answers from the fast ops as well as
//             nondeterminism, state leaking between interpreters and crashes
//
// build + run with tools/fuzz/run.sh (scratch build dir, never touches bin/linux/).
//
//   embr_fuzz frontend [-n N] [-seed S] [-t SECONDS] [corpus files...]
//   embr_fuzz vmbr [-n N] [-seed S] [source files for seeds...]
//   embr_fuzz pmbr [-n N] [-seed S]
//   embr_fuzz programs [-n N] [-seed S]
//   embr_fuzz replay FILE        (re-run one saved crash-*/hang-* input)
//
// every failure saves its input as crash-*.bin / hang-*.bin / programs-*.embr in the cwd.
// also exports LLVMFuzzerTestOneInput so a clang build can use real libFuzzer:
//   clang++ -fsanitize=fuzzer,address,undefined -DFUZZ_LIBFUZZER ...

#include <embr/embr.h>
#include "../../src/fmt.h"
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <unistd.h>

#ifdef FUZZ_PROGRAMS_EMBRLIB_SRC
// the `programs` target needs push, append, insert ... in the interpreter, or every program that uses them just prints "E"
// in both runs and the comparison says nothing about the in-place ops. run.sh builds that target with embrlib compiled in
#  include FUZZ_PROGRAMS_EMBRLIB_SRC
#endif
#ifdef FUZZ_PLUGIN_SRC
#  include FUZZ_PLUGIN_SRC
#endif
#ifdef FUZZ_C_PLUGIN
// the plugin is a C file linked in (run.sh), read through the same loader a script's `import` uses
extern "C" const EmbrPluginInfo* embr_plugin_info(void);
static void embr_register(embr::Interpreter* i) { embr::loadCPlugin(*i, embr_plugin_info(), "<fuzz>"); }
#endif

using namespace embr;

// ---- crash / hang capture -------------------------------------------------
static std::string g_current;      // the input being run right now
static const char* g_kind = "crash";
static int g_saveCounter = 0;

static void saveInput(const char* kind, const std::string& data, const char* ext = "bin") {
    char name[128];
    std::snprintf(name, sizeof name, "%s-%d-%d.%s", kind, (int)getpid(), g_saveCounter++, ext);
    std::ofstream(name, std::ios::binary) << data;
    std::fprintf(stderr, "fuzz: saved %s (%zu bytes)\n", name, data.size());
}
static void onFatal(int sig) {
    saveInput(sig == SIGALRM ? "hang" : "crash", g_current);
    std::fprintf(stderr, "fuzz: fatal signal %d while running the saved input\n", sig);
    _exit(sig == SIGALRM ? 124 : 134);
}
// ASan and UBSan terminate through their own exit path, not abort(), so SIGABRT above never fires for
// their reports; the sanitizer runtime calls this death callback right after printing one instead
extern "C" void __sanitizer_set_death_callback(void (*)(void));
static void onSanitizerDeath() { saveInput("crash", g_current); }

static void installHandlers() {
    __sanitizer_set_death_callback(onSanitizerDeath);
    for (int s : {SIGSEGV, SIGABRT, SIGFPE, SIGBUS, SIGILL, SIGALRM}) std::signal(s, onFatal);
}

// ---- output capture -------------------------------------------------------
struct Capture {
    std::ostringstream out, err;
    std::streambuf *so, *se;
    Capture()  { so = std::cout.rdbuf(out.rdbuf()); se = std::cerr.rdbuf(err.rdbuf()); }
    ~Capture() { std::cout.rdbuf(so); std::cerr.rdbuf(se); }
};

// ---- frontend target ------------------------------------------------------
// returns false on a contract violation (unexpected exception type)
static bool fuzzFrontend(const std::string& src) {
    Capture cap;
    Interpreter interp;
    try {
        vm::compileSource(src, interp, "<fuzz>");
    } catch (const EmbrError& e) {
        // compileSource() checks the compiler's own code with the stack checker (debug builds). a refusal means the
        // two disagree about some construct, whichever one is wrong
        if (e.context == "vmbr") { std::fprintf(stderr, "fuzz: the stack checker refused compiled code: %s\n", e.message.c_str()); return false; }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fuzz: non-EmbrError exception: %s\n", e.what());
        return false;
    }
    // the module reader (core/modinfo.h) takes any text too: it must never crash or throw. a `__module__` is added so the
    // declaration code runs as well, not only the walk
    try {
        PluginPolicy policy;
        policy.restricted = true;
        ModuleInfo a = analyzeModule(src, "<fuzz>.embr", policy);
        ModuleInfo b = analyzeModule("__module__ = {\"version\": \"1\", \"capabilities\": [\"network\"]}\n" + src, "<fuzz>.embr", policy);
        (void)moduleJson(a, true); (void)moduleText(b); (void)moduleValue(b, true);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fuzz: module reader: %s\n", e.what());
        return false;
    }
    return true;
}

static bool fuzzFmt(const std::string& src) {
    try {
        std::string once = embr_fmt::formatSource(src);
        if (embr_fmt::formatSource(once) != once) { std::fprintf(stderr, "fuzz: fmt is not idempotent\n"); return false; }
    } catch (const EmbrError&) {
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fuzz: fmt: %s\n", e.what());
        return false;
    }
    return true;
}

// a .vmbr image whose last 8 bytes (the checksum) are right again
static std::string withChecksum(std::string b) {
    if (b.size() < 8) return b;
    b.resize(b.size() - 8);
    uint64_t h = vm::blockHash(b.data(), b.size());
    for (int i = 0; i < 8; ++i) b.push_back((char)((h >> (8 * i)) & 0xFF));
    return b;
}

// edits the instructions of a valid image (change an op, delete, duplicate or swap instructions, change operands) and
// writes it again with a good checksum. byte flips rarely make code that gets past the first undefined name, this does
static size_t rnd(size_t n);   // defined with the other mutation helpers below
static std::string structMutate(const std::string& image) {
    try {
        vm::CompiledProgram p = vm::deserializeProgram(image, "<fuzz>");
        vm::CompiledChunk& c = *p.chunks[rnd(p.chunks.size())];
        int edits = 1 + (int)rnd(3);
        for (int e = 0; e < edits && !c.code.empty(); ++e) {
            size_t i = rnd(c.code.size());
            switch (rnd(7)) {
                case 0: c.code[i].op = (vm::Op)rnd((size_t)vm::kOpCount); break;
                case 1: c.code.erase(c.code.begin() + (long)i); c.lines.erase(c.lines.begin() + (long)i); break;
                case 2: c.code.insert(c.code.begin() + (long)i, c.code[i]); c.lines.insert(c.lines.begin() + (long)i, c.lines[i]); break;
                case 3: if (i + 1 < c.code.size()) { std::swap(c.code[i], c.code[i + 1]); std::swap(c.lines[i], c.lines[i + 1]); } break;
                case 4: c.code[i].operand = (int)rnd(c.code.size() + 2); break;
                case 5: c.code[i].arg = (int)rnd(c.code.size() + 2); break;
                case 6: c.code[i].operand = (int)rnd(4); break;
            }
        }
        std::string out = vm::serializeProgram(p, 0, "");
        vm::releaseProgram(p);
        return out;
    } catch (const EmbrError&) {
        return image;
    }
}

static bool fuzzPmbr(const std::string& bytes) {
    try {
        PackageFile pf;
        std::string why;
        if (readPackageFile(bytes, pf, why)) {
            for (const auto& e : pf.entries) {
                std::string label = e.label, lw;
                if (!validLabel(label, &lw)) { std::fprintf(stderr, "fuzz: pmbr accepted the label '%s' (%s)\n", label.c_str(), lw.c_str()); return false; }
                if (e.offset + e.length > bytes.size()) { std::fprintf(stderr, "fuzz: pmbr accepted an entry outside the file\n"); return false; }
                std::string data;
                packageEntryBytes(bytes, e, data);
            }
            analyzePackageBytes(bytes, "<fuzz>.pmbr");
            std::string merged, mwhy;
            mergePackages({bytes, bytes}, merged, mwhy);   // the same package twice: joins, or says why not
        }
    } catch (const EmbrError&) {
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fuzz: pmbr: non-EmbrError exception: %s\n", e.what());
        return false;
    }
    return true;
}

static bool fuzzVmbr(const std::string& bytes) {
    try {
        // the description a compiled module carries is read by tools that never load the code, so it is checked on its own
        {
            std::string m;
            if (vmbr::readMeta(bytes, m)) {
                ModuleInfo d;
                decodeModuleInfo(m, d);
                PluginPolicy policy;
                policy.restricted = true;      // never open a library a mutated import name points at
                std::set<std::string> visiting;
                analyzeCompiledBytes(bytes, "<fuzz>.vmbr", policy, visiting, 0);
            }
        }
        vm::CompiledProgram p = vm::deserializeProgram(bytes, "<fuzz>");
        std::ostringstream dis;
        vm::disassemble(p, dis);
        std::string b1 = vm::serializeProgram(p, 0, "");
        vm::CompiledProgram p2 = vm::deserializeProgram(b1, "<fuzz>");
        std::string b2 = vm::serializeProgram(p2, 0, "");
        vm::releaseProgram(p2);
        // what loads has passed the checks, so it must run without crashing, however little sense it makes. it runs with
        // no plugins and no file access, and for a limited number of steps (a loop that never ends is fine)
        {
            Capture cap;
            Interpreter interp;
            interp.pluginPolicy.restricted = true;
            interp.pluginPolicy.fileIo = false;
            interp.pluginPolicy.allowed.clear();
            vm::registerChunkTeardown(interp, p);
            interp.setSource(SourceMap(), "<fuzz>");
            try {
                vm::VmRunner runner(interp);
                runner.runLimited(p, 20000);
                runner.releaseFrames();
            } catch (const EmbrError&) {}
        }
        if (b1 != b2) { std::fprintf(stderr, "fuzz: vmbr write -> read -> write changed the bytes\n"); return false; }
    } catch (const EmbrError&) {
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fuzz: vmbr: non-EmbrError exception: %s\n", e.what());
        return false;
    }
    return true;
}

#ifdef FUZZ_JSON
static bool fuzzJson(const std::string& src) {
    Capture cap;
    Interpreter interp;
    embr_register(&interp);
    interp.bind("fuzz_input", [&src](const std::vector<Value>&) -> Value { return Value(src); });
    const char* prog =
        "try\n"
        "  v = json_parse(fuzz_input())\n"
        "  try\n"
        "    v2 = json_parse(json_stringify(v))\n"
        "  catch e\n"
        // json_stringify refuses a string that is not valid UTF-8 (documented), and json_parse lets such bytes
        // through, so the strict form may fail. the lossy form has to round-trip then
        "    try\n"
        "      v2 = json_parse(json_stringify(v, 0, {\"lossy\": 1}))\n"
        "    catch e2\n"
        "      print(\"ROUNDTRIP-FAIL\")\n"
        "    end\n"
        "  end\n"
        "catch e\n"
        "end\n";
    try { runSource(prog, interp, "<fuzz>"); }
    catch (const std::exception& e) { std::fprintf(stderr, "fuzz: exception %s\n", e.what()); return false; }
    return cap.out.str().find("ROUNDTRIP-FAIL") == std::string::npos;
}
#endif


#ifdef FUZZ_PLUGIN_SRC
// ---- plugin targets -------------------------------------------------------
// every statement runs inside its own try, so one raising call doesn't hide the next one. a, b are the two
// input strings, n is a number picked from the bytes (see fuzzPlugin).
static std::string tryEach(std::initializer_list<const char*> exprs) {
    std::string out;
    for (const char* e : exprs) out += std::string("try\n  v = ") + e + "\ncatch e\nend\n";
    return out;
}

static std::string pluginScript(const std::string& name) {
    std::string pre = "a = fuzz_input()\nb = fuzz_input2()\nn = fuzz_num()\n";
    if (name == "csv")
        return pre + tryEach({"csv_parse(a)", "csv_parse(a, b)", "csv_parse_dicts(a)", "csv_parse_dicts(a, b)",
                              "csv_stringify([[a, b], [n]])", "csv_stringify([[a]], b)"}) +
            // whatever csv_parse accepts, csv_stringify must write back as something that parses the same
            "ok = 0\ntry\n  r = csv_parse(a)\n  ok = 1\ncatch e\nend\n"
            "if ok\n  try\n    if csv_parse(csv_stringify(r)) != r\n      print(\"ROUNDTRIP-FAIL\")\n    end\n"
            "  catch e\n    print(\"ROUNDTRIP-FAIL\")\n  end\nend\n";
    if (name == "encoding")
        return pre + tryEach({"base64_decode(a)", "base64_decode(a, 1)", "base64_encode(a)", "base64_encode(a, 1, 1)",
                              "hex_decode(a)", "hex_encode(a)", "crc32(a)", "sha256(a)", "sha256_raw(a)"}) +
            "try\n  if base64_decode(base64_encode(a)) != a\n    print(\"ROUNDTRIP-FAIL\")\n  end\ncatch e\n  print(\"ROUNDTRIP-FAIL\")\nend\n"
            "try\n  if base64_decode(base64_encode(a, 1, 1), 1) != a\n    print(\"ROUNDTRIP-FAIL\")\n  end\ncatch e\n  print(\"ROUNDTRIP-FAIL\")\nend\n"
            "try\n  if hex_decode(hex_encode(a)) != a\n    print(\"ROUNDTRIP-FAIL\")\n  end\ncatch e\n  print(\"ROUNDTRIP-FAIL\")\nend\n"
            // a decoded value must re-encode to something that decodes to the same bytes
            "ok = 0\ntry\n  d = base64_decode(a)\n  ok = 1\ncatch e\nend\n"
            "if ok\n  try\n    if base64_decode(base64_encode(d)) != d\n      print(\"ROUNDTRIP-FAIL\")\n    end\n"
            "  catch e\n    print(\"ROUNDTRIP-FAIL\")\n  end\nend\n";
    if (name == "regex")
        return pre + tryEach({"re_test(a, b)", "re_test(b, a)", "re_match(a, b)", "re_match(b, a)",
                              "re_find_all(a, b)", "re_find_all(b, a)", "re_replace(a, b, a)", "re_replace(a, b, \"$1$0$9\\\\\")",
                              "re_replace(b, a, b)", "re_find_all(\"aaaaaaaaaaaaaaaaaaaaaaaaaaaa!\", a)",
                              "re_match_named(a, b)", "re_find_all(a, b, \"m\")", "re_replace(a, b, \"$<n>$1\", \"m\")",
                              "re_test(a, b, a)"});
    if (name == "str")
        return pre + tryEach({"str_split(a, b)", "str_replace(a, b, a)", "str_strip(a)", "str_ltrim(a)", "str_rtrim(a)",
                              "str_lower(a)", "str_upper(a)", "str_find(a, b)", "str_find(a, b, n)", "str_startswith(a, b)",
                              "str_endswith(a, b)", "str_isdigit(a)", "str_isalpha(a)", "str_isalnum(a)", "str_count(a, b)",
                              "str_ord(a)", "str_chr(n)", "str_repeat(a, n)", "str_reverse(a)", "str_pad_left(a, n)",
                              "str_pad_left(a, n, b)", "str_pad_right(a, n, b)", "str_format(a, b, n)", "str_format(a, a, b)",
                              "str_format(a)", "num_fixed(n, n)"});
    if (name == "unicode")
        return pre + tryEach({"unicode_len(a)", "unicode_ord(a)", "unicode_chr(n)", "unicode_chars(a)", "unicode_valid(a)",
                              "unicode_isalpha(a)", "unicode_isdigit(a)", "unicode_isalnum(a)"});
    if (name == "time")
        return pre + tryEach({"time_format(n, a)", "time_format(0, a)", "time_format(n, \"%Y-%m-%d %H:%M:%S\")",
                              "time_parse(a, b)", "time_parse(a, \"%Y-%m-%d %H:%M:%S\")", "time_parse(\"2024-02-29 12:00:00\", a)"});
    if (name == "path")
        // the plugin must give what std::filesystem gives (the first version of it was built on that): ref_* below are those
        return pre + tryEach({"path_join(a, b)", "path_join(a, b, a)", "path_relative(a, b)", "path_is_abs(a)"}) +
            "fn same(name, got, want)\n  if got != want\n    print(\"ROUNDTRIP-FAIL\", name)\n  end\nend\n"
            "for s in [a, b, a + b, a + \"/\" + b, b + \"//\" + a]\n"
            "  same(\"dirname\", path_dirname(s), ref_dirname(s))\n"
            "  same(\"basename\", path_basename(s), ref_basename(s))\n"
            "  same(\"extname\", path_extname(s), ref_extname(s))\n"
            "  same(\"stem\", path_stem(s), ref_stem(s))\n"
            "  same(\"normalize\", path_normalize(s), ref_normalize(s))\n"
            "end\n"
            "same(\"join\", path_join(a, b), ref_join(a, b))\n"
            "same(\"relative\", path_relative(a, b), ref_relative(a, b))\n";
    if (name == "vec")
        // indices come from the number picked out of the bytes (-1, 0, 5, 1e18, ...). the callbacks change the vec
        // while it is being walked. no doubling loops: the element cap is far above what memory can hold
        return pre + "v = vec_from([a, b, n])\n" + tryEach({
                "vec_get(v, n)", "vec_set(v, n, a)", "vec_insert(v, n, b)", "vec_remove(v, n)", "vec_swap(v, n, 0)",
                "vec_swap(v, 0, n)", "vec_slice(v, n)", "vec_slice(v, 0, n)", "vec_slice(v, n, n)", "vec_pop(v)",
                "vec_extend(v, [a, b])", "vec_extend(v, v)", "vec_extend(v, a)", "vec_get(a, 0)", "vec_len(n)",
                "vec_each_do(v, fn(x, i) vec_push(v, x) return i > 5 end)",
                "vec_each_do(v, fn(x, i) vec_clear(v) end)",
                "vec_each_do(v, fn(x, i) vec_remove(v, 0) end)",
                "vec_each_do(v, fn(x, i) vec_assign(v, [a]) end)",
                "vec_each_do(v, fn(x, i) error(a) end)",
                "vec_each_do(v, a)",
                "vec_map_do(v, fn(x, i) vec_clear(v) return x end)",
                "vec_map_do(v, fn(x, i) vec_insert(v, 0, x) return i end)",
                "vec_map_do(v, fn(x, i) return v end)",
                "vec_assign(v, v)", "vec_assign(v, [])", "vec_copy(v)", "vec_to_array(v)", "vec_reverse(v)",
                "vec_clear(v)", "vec_push(v, v)", "is_vec(v)"}) +
            // building from an array and reading it back must give the same array, and reversing twice is a no-op
            "w = vec_from([a, b, n, [a, b], {\"k\": a}])\n"
            "try\n  if vec_to_array(w) != [a, b, n, [a, b], {\"k\": a}]\n    print(\"ROUNDTRIP-FAIL\")\n  end\n"
            "  vec_reverse(w)\n  vec_reverse(w)\n"
            "  if vec_to_array(w) != [a, b, n, [a, b], {\"k\": a}]\n    print(\"ROUNDTRIP-FAIL\")\n  end\n"
            "  if vec_to_array(vec_copy(w)) != vec_to_array(w)\n    print(\"ROUNDTRIP-FAIL\")\n  end\n"
            "catch e\n  print(\"ROUNDTRIP-FAIL\")\nend\n";
    return pre;
}

static bool fuzzPlugin(const std::string& src) {
    Capture cap;
    Interpreter interp;
    embr_register(&interp);
    size_t sep = src.find('\x01');
    std::string a = sep == std::string::npos ? src : src.substr(0, sep);
    std::string b = sep == std::string::npos ? src : src.substr(sep + 1);
    static const double nums[] = {-1, 0, 1, 5, 255, 256, 2147483647.0, 4294967296.0, 1e12, 1e18, 1e300, -1e18,
                                  0.5, 1700000000, 253402300800.0, -62135596800.0};
    double n = nums[std::hash<std::string>{}(src) % (sizeof nums / sizeof *nums)];
    interp.bind("fuzz_input",  [a](const std::vector<Value>&) -> Value { return Value(a); });
    interp.bind("fuzz_input2", [b](const std::vector<Value>&) -> Value { return Value(b); });
    interp.bind("fuzz_num",    [n](const std::vector<Value>&) -> Value { return Value(n); });
    if (std::string(FUZZ_PLUGIN_NAME) == "path") {
        namespace fs = std::filesystem;
        auto bindRef = [&](const char* nm, std::string (*f)(const std::string&)) {
            interp.bind(nm, [f](const std::vector<Value>& v) -> Value { return Value(f(v[0].asString())); });
        };
        bindRef("ref_dirname", +[](const std::string& in) { fs::path p(in); if (!p.has_filename() && p.has_relative_path()) p = p.parent_path(); return p.parent_path().generic_string(); });
        bindRef("ref_basename", +[](const std::string& in) { fs::path p(in); if (!p.has_filename() && p.has_relative_path()) p = p.parent_path(); return p.filename().generic_string(); });
        bindRef("ref_extname", +[](const std::string& in) { return fs::path(in).extension().generic_string(); });
        bindRef("ref_stem", +[](const std::string& in) { return fs::path(in).stem().generic_string(); });
        bindRef("ref_normalize", +[](const std::string& in) {
            if (in.empty()) return std::string(".");
            fs::path nrm = fs::path(in).lexically_normal();
            std::string r = nrm.generic_string();
            while (r.size() > 1 && (r.back() == '/' || r.back() == '\\') && nrm.has_relative_path()) r.pop_back();
            return r.empty() ? std::string(".") : r;
        });
        interp.bind("ref_join", [](const std::vector<Value>& v) -> Value {
            fs::path out(v[0].asString());
            if (!v[1].asString().empty()) out /= fs::path(v[1].asString());
            return Value(out.generic_string());
        });
        interp.bind("ref_relative", [](const std::vector<Value>& v) -> Value {
            return Value(fs::path(v[0].asString()).lexically_relative(v[1].asString()).generic_string());
        });
    }
    static const std::string script = pluginScript(FUZZ_PLUGIN_NAME);
    try { runSource(script, interp, "<fuzz>"); }
    catch (const EmbrError&) {}
    catch (const std::exception& e) { std::fprintf(stderr, "fuzz: exception %s\n", e.what()); return false; }
    return cap.out.str().find("ROUNDTRIP-FAIL") == std::string::npos;
}
#endif

// ---- mutation -------------------------------------------------------------
static std::mt19937_64 rng;
static size_t rnd(size_t n) { return n ? rng() % n : 0; }

static const char* kTokens[] = {
    "fn ", "end\n", "if ", "elif ", "else\n", "while ", "for ", " in ", "try\n", "catch e\n",
    "return ", "break\n", "continue\n", "local ", "import ", "and ", "or ", "!", "-", "==", "!=",
    "<=", ">=", "(", ")", "[", "]", "{", "}", ",", ":", ".", "...", "->", "\"", "\\", "#", "#[", "]#",
    "\n", "  ", "0", "1.5", "99999999999999999999", "1e5", "0x", "..", "=", "+=", "int ", "auto ",
    "\"\\n\"", "\"\\x\"", "\r\n", "\t", "\xff", "\xc3\x28", "\0",
    // plugin inputs: the a/b separator, regex, strftime and format-string pieces, utf-8 edge cases
    "\x01", "*", "+", "?", "|", "(?:", "(?=", "(?<n>", "(?<", "$<", "\\u00e9", "\xc3\xa9", "\\1", "{2,}", "{0,99999}", "[^", "[a-", "^", "$", "\\d", "\\b",
    "%Y", "%S", "%z", "%", "{}", "{0}", "{:.2f}", "{:", "}}", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xc0\x80", "==",
};

static std::string mutate(std::string s, const std::vector<std::string>& pool) {
    int rounds = 1 + (int)rnd(4);
    for (int i = 0; i < rounds; ++i) {
        switch (rnd(9)) {
            case 0: if (!s.empty()) s[rnd(s.size())] ^= (char)(1u << rnd(8)); break;
            case 1: if (!s.empty()) s[rnd(s.size())] = (char)rnd(256); break;
            case 2: s.insert(rnd(s.size() + 1), kTokens[rnd(sizeof kTokens / sizeof *kTokens)]); break;
            case 3: if (!s.empty()) { size_t a = rnd(s.size()); s.erase(a, 1 + rnd(std::min<size_t>(16, s.size() - a))); } break;
            case 4: if (!s.empty()) { size_t a = rnd(s.size()), n = 1 + rnd(std::min<size_t>(64, s.size() - a));
                        s.insert(rnd(s.size() + 1), s.substr(a, n)); } break;
            case 5: { const std::string& o = pool[rnd(pool.size())];
                      if (!o.empty()) { size_t a = rnd(o.size()), n = 1 + rnd(std::min<size_t>(200, o.size() - a));
                          s.insert(rnd(s.size() + 1), o.substr(a, n)); } } break;
            case 6: { // deep nesting of one opener : probes the recursive-descent stack
                      static const char* op[] = {"(", "[", "{", "-", "!", "fn(x) return ", "if 1\n", "while 1\n", "try\n", "a["};
                      std::string rep; size_t n = 1 + rnd(rnd(3) == 0 ? 200000 : 3000);
                      const char* o = op[rnd(sizeof op / sizeof *op)];
                      for (size_t k = 0; k < n; ++k) rep += o;
                      s.insert(rnd(s.size() + 1), rep); } break;
            case 7: if (s.size() > 1) s.resize(1 + rnd(s.size() - 1)); break;   // truncate
            default: if (!s.empty()) { size_t a = rnd(s.size()), b = rnd(s.size()); std::swap(s[a], s[b]); } break;
        }
        if (s.size() > (256u << 10)) s.resize(256u << 10);   // big inputs only make a run slow under ASan, not more likely to find something
    }
    return s;
}

// ---- program generator for the programs target ----------------------------
struct Gen {
    std::ostringstream o;
    int ind = 0;
    static const int kVars = 5;

    void line(const std::string& s) { o << std::string(ind * 2, ' ') << s << "\n"; }
    bool inF0 = false;   // f0's own body must not call f0 (unbounded recursion just burns time on depth errors)
    std::string var() { return "x" + std::to_string(rnd(kVars)); }

    // expressions of one kind, so most assignments keep a variable's type. the type checker accepts those programs and the
    // typed and untyped stores can be compared on them. the plain expr() above is the chaos: a variable of any type
    std::string iexpr(int d) {
        if (d <= 0 || rnd(3) == 0) return rnd(2) ? std::to_string(rnd(20)) : "x" + std::to_string(rnd(kVars));
        switch (rnd(6)) {
            case 0: case 1: case 2: { static const char* ops[] = {"+", "-", "*", "%"};
                                      const char* op = ops[rnd(4)];
                                      return "(" + iexpr(d - 1) + " " + op + " " + (op[0] == '%' ? std::to_string(1 + rnd(9)) : iexpr(d - 1)) + ")"; }
            case 3: return "len(" + aexpr(1) + ")";
            case 4: return "len(" + sexpr(1) + ")";
            default: return "-" + iexpr(d - 1);
        }
    }
    std::string sexpr(int d) {
        if (d <= 0 || rnd(3) == 0) return rnd(2) ? "\"" + std::string(1 + rnd(3), (char)('a' + rnd(3))) + "\"" : "s" + std::to_string(rnd(2));
        return "(" + sexpr(d - 1) + " + " + (rnd(2) ? sexpr(d - 1) : iexpr(d - 1)) + ")";
    }
    std::string aexpr(int d) {
        if (d <= 0 || rnd(3) == 0) return rnd(2) ? "[" + std::to_string(rnd(9)) + ", " + std::to_string(rnd(9)) + "]" : "a" + std::to_string(rnd(2));
        return rnd(2) ? "(" + aexpr(d - 1) + " + " + aexpr(d - 1) + ")" : "push(" + aexpr(d - 1) + ", " + iexpr(d - 1) + ")";
    }

    std::string lit() {
        switch (rnd(6)) {
            case 0: return std::to_string(rnd(10));
            case 1: return std::to_string((int)rnd(2000) - 1000);
            case 2: return std::to_string(rnd(100)) + "." + std::to_string(rnd(100));
            case 3: return "\"" + std::string(1 + rnd(3), (char)('a' + rnd(3))) + "\"";
            case 4: return std::to_string(9223372036854775000LL + (long long)rnd(800));
            default: return std::to_string(rnd(5));
        }
    }
    std::string expr(int d) {
        if (d <= 0 || rnd(4) == 0) return rnd(2) ? lit() : var();
        switch (rnd(10)) {
            case 0: case 1: case 2: case 3: {
                static const char* ops[] = {"+", "-", "*", "/", "%", "==", "!=", "<", ">", "<=", ">=", "and", "or"};
                return "(" + expr(d - 1) + " " + ops[rnd(13)] + " " + expr(d - 1) + ")";
            }
            case 4: return std::string(rnd(2) ? "-" : "!") + expr(d - 1);
            case 5: return "[" + expr(d - 1) + ", " + expr(d - 1) + "]";
            case 6: return "{\"k\": " + expr(d - 1) + ", \"j\": " + expr(d - 1) + "}";
            case 7: return expr(d - 1) + "[" + std::to_string(rnd(3)) + "]";
            case 8: return "f" + std::to_string(inF0 ? 1 : rnd(2)) + "(" + expr(d - 1) + ", " + expr(d - 1) + ")";
            default: return "(fn(a) return a + " + expr(d - 1) + " end)(" + expr(d - 1) + ")";
        }
    }
    // every generated statement is wrapped in try/catch printing only "E", so the output only depends on whether
    // a statement raised, not on the error text
    void guarded(const std::string& stmt) {
        line("try"); ++ind; line(stmt); --ind;
        line("catch e"); ++ind; line("print(\"E\")"); --ind; line("end");
    }
    // index writes and push on plain variables, with copies taken first. these are the places the VM changes
    // a variable where it lives (INDEX_SET_VAR, APPEND_ARR), so a copy made earlier must not change
    void indexWrites() {
        switch (rnd(10)) {
            case 0: guarded("arr = [" + expr(1) + ", " + expr(1) + "]\narr[" + std::to_string(rnd(3)) + "] = " + expr(1) + "\nprint(arr)"); break;
            case 1: guarded("pa = [" + expr(1) + ", " + expr(1) + "]\npb = pa\npa = push(pa, " + expr(1) + ")\npa[" +
                            std::to_string(rnd(3)) + "] = " + expr(1) + "\nprint(pa)\nprint(pb)"); break;
            case 2: guarded("pm = {\"k\": 1}\npn = pm\npm[\"k\" + str(" + expr(1) + ")] = " + expr(1) + "\nprint(pm)\nprint(pn)"); break;
            case 3: guarded("pa = [1, 2]\npb = pa\npa[f1(" + expr(1) + ", " + expr(1) + ")] = " + expr(1) + "\nprint(pa)\nprint(pb)"); break;
            case 4: guarded("pa = [1, 2]\npa = push(pa, f1(" + expr(1) + ", " + expr(1) + "))\nprint(pa)"); break;
            case 5: guarded("fn pf(a, b)\n  l = [a, b]\n  k = l\n  l = push(l, f1(a, b))\n  l[f1(0, 1)] = l[0] + b\n"
                            "  l[1] = [l[0], l[1]]\n  l[1][0] = a\n  return [l, k]\nend\nprint(pf(" + expr(1) + ", " + expr(1) + "))"); break;
            case 6: guarded("pa = [1, 2]\npb = pa\nappend(&pa, " + expr(1) + ")\ninsert(&pa, 0, " + expr(1) + ")\nprint(pa)\nprint(pb)"); break;
            case 7: guarded("pg = [[1], [2]]\nph = pg\nappend(&pg[1], " + expr(1) + ")\nprint(remove_last(&pg[0]))\nprint(pg)\nprint(ph)"); break;
            case 8: guarded("pm = {\"k\": [1]}\nappend(&pm[\"k\"], f1(" + expr(1) + ", " + expr(1) + "))\nremove_key(&pm, \"j\")\nprint(pm)"); break;
            default: guarded("pg = [[1, 2], {\"k\": [3]}]\nph = pg\npg[1][\"k\"][0] = " + expr(1) + "\npg[0][1] = pg\nprint(pg)\nprint(ph)"); break;
        }
    }
    void stmt(int d, int loopDepth) {
        switch (rnd(d > 0 ? 8 : 4)) {
            case 0: case 1:
                switch (rnd(10)) {
                    case 0: guarded(var() + " = " + expr(2)); break;                                   // chaos: any type
                    case 1: guarded("s" + std::to_string(rnd(2)) + " = " + sexpr(2)); break;
                    case 2: guarded("a" + std::to_string(rnd(2)) + " = " + aexpr(2)); break;
                    default: guarded(var() + " = " + iexpr(2)); break;
                }
                break;
            case 2: case 3: guarded("print(" + expr(3) + ")"); break;
            case 4: { line("if " + expr(2)); ++ind; stmt(d - 1, loopDepth); --ind;
                      if (rnd(2)) { line("else"); ++ind; stmt(d - 1, loopDepth); --ind; }
                      line("end"); } break;
            case 5: { std::string lv = "lc" + std::to_string(loopDepth);
                      line("for " + lv + " in [" + std::to_string(rnd(9)) + ", " + std::to_string(rnd(9)) + ", " + std::to_string(rnd(9)) + "]"); ++ind;
                      guarded(var() + " = " + lv + " + " + iexpr(1));
                      stmt(d - 1, loopDepth + 1); --ind; line("end"); } break;
            case 6: { std::string c = "wc" + std::to_string(loopDepth);
                      line(c + " = 0"); line("while " + c + " < 3"); ++ind;
                      stmt(d - 1, loopDepth + 1); line(c + " += 1"); --ind; line("end"); } break;
            default: indexWrites(); break;
        }
    }
    std::string program() {
        for (int i = 0; i < kVars; ++i) line("x" + std::to_string(i) + " = " + std::to_string(rnd(9)));
        line("s0 = \"a\""); line("s1 = \"b\""); line("a0 = [1]"); line("a1 = [2, 3]");
        // f0 assigns fresh names (t0, t1), once directly and once first inside an if/else block, so the runs cover
        // an undeclared assignment inside a function (local to the call). the names are then probed at top level,
        // where they must be undefined
        line("fn f0(a, b)"); ++ind; inF0 = true;
        line("t0 = " + iexpr(1));
        line("if " + expr(1)); ++ind; line("t1 = " + iexpr(1)); --ind;
        line("else"); ++ind; line("t1 = " + iexpr(1)); --ind; line("end");
        line("return " + expr(2) + " == t1");
        inF0 = false; --ind; line("end");
        line("f1 = fn(a, b) return a + b end");
        int n = 4 + (int)rnd(8);
        for (int i = 0; i < n; ++i) stmt(2, 0);
        for (int i = 0; i < kVars; ++i) guarded("print(x" + std::to_string(i) + ")");
        guarded("print(s0, s1, a0, a1)");
        guarded("print(t0)");   // must raise: function-locals don't leak
        guarded("print(t1)");
        return o.str();
    }
};

// does the type checker accept this program? the typed and untyped stores only have to agree on programs it accepts: with a
// type mistake in it they may differ (the typed store follows the first assignment in the source, the untyped one the first
// that runs)
static bool staticallyClean(const std::string& src) {
    try {
        Lexer lex(src); auto toks = lex.tokenize(); SourceMap sm = lex.sourceMap();
        Parser parser(std::move(toks), sm);
        auto ast = parser.parse();
        typecheck::Options o; o.source = &sm;
        return typecheck::check(ast, o).empty();
    } catch (const EmbrError&) { return false; }
}

struct RunResult { std::string out; bool errored; };
template <typename RunFn>
static RunResult runOn(const std::string& src, RunFn run) {
    Capture cap;
    Interpreter interp;
#ifdef FUZZ_PROGRAMS_EMBRLIB_SRC
    embr_register(&interp);
#endif
    bool errored = false;
    try { run(src, interp, "<fuzz>"); } catch (const EmbrError&) { errored = true; }
    if (!cap.err.str().empty()) errored = true;
    return {cap.out.str(), errored};
}

// ---- main -----------------------------------------------------------------
static std::string readFile(const std::string& p) {
    std::ifstream f(p, std::ios::binary); std::ostringstream ss; ss << f.rdbuf(); return ss.str();
}

#ifdef FUZZ_LIBFUZZER
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* d, size_t n) {
    std::string s((const char*)d, n);
#  ifdef FUZZ_JSON
    return fuzzJson(s) ? 0 : (std::abort(), 1);
#  elif defined(FUZZ_PLUGIN_SRC)
    return fuzzPlugin(s) ? 0 : (std::abort(), 1);
#  else
    return fuzzFrontend(s) ? 0 : (std::abort(), 1);
#  endif
}
#else
int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s frontend|fmt|vmbr|json|programs|<plugin> [-n N] [-seed S] [-t SECONDS] [files...]\n", argv[0]); return 2; }
    std::string mode = argv[1];
    if (mode == "replay" && argc >= 3) {          // embr_fuzz replay FILE: run one saved input once
        installHandlers();
        g_current = readFile(argv[2]);
        alarm(20);
#ifdef FUZZ_JSON
        bool ok = fuzzJson(g_current);
#elif defined(FUZZ_PLUGIN_SRC)
        bool ok = fuzzPlugin(g_current);
#else
        bool ok = g_current.compare(0, 4, "VMBR") == 0 ? fuzzVmbr(g_current) : (fuzzFrontend(g_current) && fuzzFmt(g_current));
#endif
        std::printf("replay: %s\n", ok ? "ok" : "CONTRACT VIOLATION");
        return ok ? 0 : 1;
    }
    long n = 2000; unsigned long seed = std::random_device{}(); int timeout = 10;
    std::vector<std::string> files;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-n" && i + 1 < argc) n = std::atol(argv[++i]);
        else if (a == "-seed" && i + 1 < argc) seed = std::strtoul(argv[++i], nullptr, 10);
        else if (a == "-t" && i + 1 < argc) timeout = std::atoi(argv[++i]);
        else files.push_back(a);
    }
    rng.seed(seed);
    std::printf("fuzz: mode=%s n=%ld seed=%lu\n", mode.c_str(), n, seed);
    installHandlers();

    std::vector<std::string> pool = {
        "x = 1\nprint(x)\n", "fn f(a, b)\n  return a + b\nend\nprint(f(1, 2))\n",
        "for i in [1, 2, 3]\n  print(i)\nend\n", "try\n  error(\"x\")\ncatch e\n  print(e[\"message\"])\nend\n",
        "m = {\"a\": [1, 2, {\"b\": 3}]}\nprint(m.a[2].b)\n", "g = fn(n) return fn(x) return x + n end end\nprint(g(1)(2))\n",
        "local int x = 5\nx += 1\nwhile x < 9\n  x += 1\n  if x == 7\n    continue\n  end\nend\n",
        "a, b = [1, 2]\nprint(\"s\" + \"t\")\n#[ block ]#\n# line\n",
        "{\"a\": [1, 2.5, \"s\\u00e9\", null, true], \"b\": {\"c\": -1e5}}",
    };
#ifdef FUZZ_PLUGIN_SRC
    pool = { std::string("abc\x01" "def") };   // embr programs make poor plugin inputs; corpus files follow
#endif
    for (auto& f : files) pool.push_back(readFile(f));
#if !defined(FUZZ_JSON) && !defined(FUZZ_PLUGIN_SRC)
    if (mode == "vmbr") {            // the seeds are real compiled scripts, so mutations start from valid files
        std::vector<std::string> images;
        for (auto& src : pool) {
            try {
                Capture cap;
                Interpreter interp;
                vm::CompiledProgram prog = vm::compileSource(src, interp, "<fuzz>");
                images.push_back(vm::serializeProgram(prog, vm::hashSource(src), "<fuzz>", moduleMetaBlob(src, "<fuzz>.embr")));
            } catch (const EmbrError&) {}   // not a script (the json seed, or a corpus file with an error)
        }
        pool = std::move(images);
        if (pool.empty()) { std::fprintf(stderr, "fuzz: vmbr: no seed compiled\n"); return 2; }
    }
    if (mode == "pmbr") {            // seeds: small valid packages, a module and a plugin
        pool.clear();
        auto metaOf = [](const std::string& body) { return "__package__ = {" + body + "}\n"; };
        pool.push_back(writePackage(0, "1.0.0", {
            {"meta.embr", metaOf("\"name\": \"m\", \"version\": \"1.0.0\", \"description\": \"d\", \"capabilities\": [\"native\"], \"module\": \"m.embr\", \"libraries\": [\"lib/{target}/libm.so\"]")},
            {"m.embr", "fn f() return 1 end\n"}, {"lib/linux-x86_64/libm.so", std::string(200, 'x')}, {"assets/data/a.txt", "hello"}}));
        pool.push_back(writePackage(1, "2.0.1", {
            {"meta.embr", metaOf("\"name\": \"p\", \"version\": \"2.0.1\", \"description\": \"d\", \"capabilities\": [], \"plugin\": {\"linux-x86_64\": \"lib/linux-x86_64/p.so\", \"windows-x86_64\": \"lib/windows-x86_64/p.dll\"}")},
            {"lib/linux-x86_64/p.so", std::string(300, 'y')}, {"lib/windows-x86_64/p.dll", std::string(300, 'z')}}));
        pool.push_back(writePackage(0, "", {{"meta.embr", ""}}));
        {   // a signed one: the trailer after the last file is read too
            unsigned char seed[32];
            for (int i = 0; i < 32; ++i) seed[i] = static_cast<unsigned char>(i * 7 + 1);
            std::string signedImg, w;
            if (signPackageBytes(pool[0], seed, signedImg, w)) pool.push_back(std::move(signedImg));
        }
    }
#endif

    long bad = 0;
    for (long it = 0; it < n; ++it) {
        alarm(timeout);
        if (mode == "programs") {
            Gen g; std::string prog = g.program();
            g_current = prog;
            vm::CompileOptions noTypes;   // the generated programs reuse variables with any type, so a type mistake must not stop them
            noTypes.typeCheck = false;         // (the checker still runs and its answers are used, see CompileOptions::useTypes)
            {   // a valid program must pass the stack checker that compileSource() runs in debug builds (see fuzzFrontend)
                Capture cap; Interpreter ci;
                try { vm::compileSource(prog, ci, "<gen>", noTypes); }
                catch (const EmbrError& e) {
                    if (e.context == "vmbr") { ++bad; saveInput("programs", prog, "embr"); std::fprintf(stderr, "fuzz: the stack checker refused compiled code: %s\n", e.message.c_str()); continue; }
                }
            }
            auto runVm = [&](const std::string& s, Interpreter& i, const std::string& f) {
                auto p = vm::compileSource(s, i, f, noTypes);
                vm::VmRunner r(i);
                try { r.run(p); } catch (const EmbrError& e) { std::cerr << e.what() << formatTrace(e); }
            };
            // the second run is compiled WITHOUT the in-place fast ops (see Compiler::Options), so the two runs also have
            // to agree about what those ops do. that is the oracle for them, there is no second backend to compare with
            vm::CompileOptions slowOpts = noTypes; slowOpts.inPlaceOps = false;
            auto runSlow = [&](const std::string& s, Interpreter& i, const std::string& fname) {
                auto p = vm::compileSource(s, i, fname, slowOpts);
                vm::VmRunner r(i);
                try { r.run(p); } catch (const EmbrError& e) { std::cerr << e.what() << formatTrace(e); }
            };
            // the third run is compiled without what the type checker worked out: every store looks its variable's type up
            // by name. the typed and proven stores must do exactly what that does, which is the oracle for the checker
            vm::CompileOptions untyped = noTypes; untyped.useTypes = false;
            auto runUntyped = [&](const std::string& s, Interpreter& i, const std::string& fname) {
                auto p = vm::compileSource(s, i, fname, untyped);
                vm::VmRunner r(i);
                try { r.run(p); } catch (const EmbrError& e) { std::cerr << e.what() << formatTrace(e); }
            };
            RunResult a = runOn(prog, runVm);
            // the fourth run has the fused instructions switched off (see quickenChunk), so a fused op has to do exactly what
            // the ordinary instructions it stands in for do
            RunResult d4;
            { bool& fuse = vm::fuseOpsEnabled(); const bool was = fuse; fuse = false; d4 = runOn(prog, runVm); fuse = was; }
            if (d4.out != a.out || d4.errored != a.errored) {
                ++bad;
                saveInput("programs", prog, "embr");
                std::fprintf(stderr, "fuzz: FUSED OPS DIFFER (fused errored=%d, plain errored=%d)\n--- fused ---\n%s--- plain ---\n%s\n",
                             a.errored, d4.errored, a.out.c_str(), d4.out.c_str());
            }
            // the fifth run does without the hot loop (VmRunner::fastRun): every instruction goes through execOne
            RunResult d5;
            { bool& fast = vm::fastLoopEnabled(); const bool was = fast; fast = false; d5 = runOn(prog, runVm); fast = was; }
            if (d5.out != a.out || d5.errored != a.errored) {
                ++bad;
                saveInput("programs", prog, "embr");
                std::fprintf(stderr, "fuzz: HOT LOOP DIFFERS (hot loop errored=%d, ordinary errored=%d)\n--- hot loop ---\n%s--- ordinary ---\n%s\n",
                             a.errored, d5.errored, a.out.c_str(), d5.out.c_str());
            }
            // the sixth run keeps every block's SCOPE_PUSH / SCOPE_POP (Compiler::Options::elideScopes), so leaving them out may not change what a program does
            vm::CompileOptions fullScopes = noTypes; fullScopes.elideScopes = false;
            RunResult d6 = runOn(prog, [&](const std::string& s2, Interpreter& i, const std::string& fname) {
                auto p = vm::compileSource(s2, i, fname, fullScopes);
                vm::VmRunner r(i);
                try { r.run(p); } catch (const EmbrError& e) { std::cerr << e.what() << formatTrace(e); }
            });
            if (d6.out != a.out || d6.errored != a.errored) {
                ++bad;
                saveInput("programs", prog, "embr");
                std::fprintf(stderr, "fuzz: SCOPE ELISION DIFFERS (elided errored=%d, full errored=%d)\n--- elided ---\n%s--- full ---\n%s\n",
                             a.errored, d6.errored, a.out.c_str(), d6.out.c_str());
            }
            RunResult b = runOn(prog, runSlow);
            RunResult c3 = runOn(prog, runUntyped);
            if (staticallyClean(prog) && (c3.out != a.out || c3.errored != a.errored)) {
                ++bad;
                saveInput("programs", prog, "embr");
                std::fprintf(stderr, "fuzz: TYPED STORES DIFFER (typed errored=%d, untyped errored=%d)\n--- typed ---\n%s--- untyped ---\n%s\n",
                             a.errored, c3.errored, a.out.c_str(), c3.out.c_str());
            }
            if (a.out != b.out || a.errored != b.errored) {
                ++bad;
                saveInput("programs", prog, "embr");
                std::fprintf(stderr, "fuzz: DIFFERENT RUNS (first errored=%d, second errored=%d)\n--- first ---\n%s--- second ---\n%s\n",
                             a.errored, b.errored, a.out.c_str(), b.out.c_str());
            }
        } else {
            std::string in = mutate(pool[rnd(pool.size())], pool);
            if (mode == "pmbr" && rnd(4) != 0) {
                // the table of contents ends with its own sha256: put the right one back, over the table as it is now
                PackageFile pf0;
                std::string w0;
                const std::string& seedImg = pool[rnd(pool.size())];
                if (readPackageFile(seedImg, pf0, w0) && in.size() >= pf0.header.size()) {
                    unsigned char d[32];
                    sha256(in.data(), pf0.header.size() - 32, d);
                    std::memcpy(&in[pf0.header.size() - 32], d, 32);
                }
            }
            if (mode == "vmbr") {
                if (rnd(2) == 0) in = structMutate(pool[rnd(pool.size())]);   // every second input: edit the instructions
                else if (rnd(4) != 0) in = withChecksum(std::move(in));
            }
            if (mode == "fmt" && in.size() > (256u << 10)) in.resize(256u << 10);   // output grows with input, keep runs short
#ifdef FUZZ_PLUGIN_SRC
            if (in.size() > 2048) in.resize(2048);   // regex is pattern length times subject length, so big inputs only time out
#endif
            g_current = in;
#ifdef FUZZ_JSON
            bool ok = (mode == "json") ? fuzzJson(in) : true;
#elif defined(FUZZ_PLUGIN_SRC)
            bool ok = (mode == FUZZ_PLUGIN_NAME) ? fuzzPlugin(in) : true;
#else
            bool ok = mode == "fmt" ? fuzzFmt(in) : (mode == "frontend") ? fuzzFrontend(in) : (mode == "vmbr") ? fuzzVmbr(in) : (mode == "pmbr") ? fuzzPmbr(in) : true;
#endif
            if (!ok) { ++bad; saveInput("crash", in); }
            if (in.size() < 400 && rnd(8) == 0) pool.push_back(in);   // grow the pool with small survivors
        }
        if ((it + 1) % 500 == 0) { std::printf("fuzz: %ld/%ld, findings=%ld\n", it + 1, n, bad); std::fflush(stdout); }
    }
    alarm(0);
    std::printf("fuzz: done, %ld iterations, %ld findings\n", n, bad);
    return bad ? 1 : 0;
}
#endif
