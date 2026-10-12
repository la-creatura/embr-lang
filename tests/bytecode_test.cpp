// unit tests for the .vmbr helpers in backends/vm_bytecode.h: round trip, same bytes every time, a damaged
// file is refused and never crashes, and cleanCache() removes exactly the files that can't be used any more

#include "harness.h"
#include <filesystem>
#include <array>

namespace fs = std::filesystem;
using namespace embr::vm;

static int g_failed = 0, g_passed = 0;
static void report(const char* name, bool ok, const std::string& why = "") {
    if (ok) { ++g_passed; std::cout << "\033[32m [+]\033[0m " << name << "\n"; return; }
    ++g_failed;
    std::cout << "\033[31m [-]\033[0m " << name << "\n";
    if (!why.empty()) std::cout << "        " << why << "\n";
}

// covers closures with upvalues, boxed locals, try/catch, for loops, typed locals, in-place string and array ops
static const char* kSource =
    "import \"embrlib\"\n"
    "fn counter()\n  local n = 0\n  return fn() n = n + 1 return n end\nend\n"
    "c = counter()\nc()\nprint(c())\n"
    "arr = []\nfor i in [1, 2, 3]\n  arr = push(arr, i * 2.5)\nend\nprint(arr)\n"
    "s = \"a\"\ns = s + \"b\"\nprint(s)\n"
    "try\n  error(\"boom\")\ncatch e\n  print(e[\"message\"])\nend\n"
    "fn typed(x: int) -> int\n  return x + 1\nend\nprint(typed(41))\n";

static std::string run(const CompiledProgram& prog, embr::Interpreter& interp) {
    std::ostringstream cap;
    std::streambuf* orig = std::cout.rdbuf(cap.rdbuf());
    try { VmRunner r(interp); r.run(prog); } catch (const embr::EmbrError& e) { cap << "ERR " << e.what(); }
    std::cout.rdbuf(orig);
    return cap.str();
}

static void testRoundTrip() {
    embr::Interpreter a;
    auto prog = compileSource(kSource, a, "t.embr");
    const uint64_t h = hashSource(kSource);
    std::string bytes = serializeProgram(prog, h, "t.embr");

    embr::Interpreter b;
    auto again = compileSource(kSource, b, "t.embr");
    report("same script gives the same bytes every time", serializeProgram(again, h, "t.embr") == bytes);

    VmbrHeader hdr;
    auto loaded = deserializeProgram(bytes, "t.embr", &hdr);
    report("header keeps the source hash and path", hdr.sourceHash == h && hdr.sourcePath == "t.embr");
    report("write, read, write again gives the same bytes", serializeProgram(loaded, h, "t.embr") == bytes);

    std::ostringstream d1, d2;
    disassemble(prog, d1); disassemble(loaded, d2);
    report("a loaded program disassembles the same", d1.str() == d2.str());

    embr::Interpreter c1, c2;
    registerChunkTeardown(c2, loaded);
    std::string out1 = run(prog, c1), out2 = run(loaded, c2);
    report("a loaded program runs the same", out1 == out2 && out1 == "2\n[2.5, 5, 7.5]\nab\nboom\n42\n", "got: " + out2);

    // the description a module carries sits in the file, is read without the VM, and is part of the checksum
    {
        const std::string desc = embr::moduleMetaBlob("__module__ = {\"version\": \"2.0.0\", \"capabilities\": [\"network\"]}\nimport \"http\"\n# says hi\nfn hi()\n    return 1\nend\n", "/x/mod.embr");
        std::string withMeta = serializeProgram(prog, h, "t.embr", desc);
        std::string got;
        embr::ModuleInfo info;
        report("a description round-trips through the file",
               embr::vmbr::readMeta(withMeta, got) && got == desc && embr::decodeModuleInfo(got, info) &&
               info.version == "2.0.0" && info.declaredCaps == EMBR_CAP_NETWORK && info.imports.size() == 1 &&
               info.functions.size() == 1 && info.functions[0].doc == "says hi");
        VmbrHeader mh;
        auto withLoaded = deserializeProgram(withMeta, "t.embr", &mh);
        report("a file with a description still loads, and the header has it", mh.meta == desc);
        releaseProgram(withLoaded);
        std::string flipped = withMeta;
        flipped[withMeta.find(desc) + 3] ^= 1;
        report("a changed byte in the description is caught by the checksum", !embr::vmbr::readMeta(flipped, got));
        report("a file written without one has an empty description", embr::vmbr::readMeta(bytes, got) && got.empty());
        embr::ModuleInfo junk;
        report("a damaged description is refused, not read past the end",
               !embr::decodeModuleInfo(desc.substr(0, desc.size() - 3), junk) && !embr::decodeModuleInfo("", junk));
    }

    auto renamed = deserializeProgram(bytes, "other.embr");
    report("sourceFile is the name given on load", renamed.chunks[0]->sourceFile == "other.embr");
    releaseProgram(renamed);
    releaseProgram(again);
}

static void testDamage() {
    embr::Interpreter a;
    auto prog = compileSource(kSource, a, "t.embr");
    std::string bytes = serializeProgram(prog, hashSource(kSource), "t.embr");

    bool allCutsRefused = true;
    for (size_t n = 0; n < bytes.size(); ++n) {
        try { deserializeProgram(bytes.substr(0, n), "t.embr"); allCutsRefused = false; break; }
        catch (const embr::EmbrError&) {}
    }
    report("every shortened file is refused", allCutsRefused);

    bool flipsRefused = true;
    for (size_t i = 0; i < bytes.size(); ++i) {
        std::string m = bytes; m[i] = (char)(m[i] ^ 0x5A);
        try { deserializeProgram(m, "t.embr"); flipsRefused = false; break; }
        catch (const embr::EmbrError&) {}
    }
    report("every single changed byte is refused", flipsRefused);

    // a file whose checksum is right but whose code points outside its tables must still be refused
    auto bad = compileSource(kSource, a, "t.embr");
    for (auto& in : bad.chunks[0]->code) if (in.op == Op::PUSH_STR) { in.operand = 9999; break; }
    bool refused = false;
    try { deserializeProgram(serializeProgram(bad, 0), "t.embr"); } catch (const embr::EmbrError&) { refused = true; }
    report("an out of range string index is refused", refused);

    auto badJump = compileSource(kSource, a, "t.embr");
    for (auto& in : badJump.chunks[0]->code) if (in.op == Op::JUMP) { in.operand = 99999; break; }
    refused = false;
    try { deserializeProgram(serializeProgram(badJump, 0), "t.embr"); } catch (const embr::EmbrError&) { refused = true; }
    report("an out of range jump is refused", refused);
}

static void write(const fs::path& p, const std::string& s) { std::ofstream f(p, std::ios::binary); f << s; }
static bool fileExists(const fs::path& p) { std::error_code ec; return fs::exists(p, ec); }

static void testCache() {
    fs::path root = fs::current_path() / "bc_cache_test";
    std::error_code ec; fs::remove_all(root, ec); fs::create_directories(root / "cache");
    fs::path src1 = root / "one.embr", src2 = root / "two.embr", src3 = root / "three.embr";
    write(src1, "print(1)\n"); write(src2, "print(2)\n"); write(src3, "print(3)\n");
    std::string cache = (root / "cache").string();

    embr::Interpreter i1;
    compileSourcePreferCached("print(1)\n", i1, src1.string(), cache);
    compileSourcePreferCached("print(2)\n", i1, src2.string(), cache);
    compileSourcePreferCached("print(3)\n", i1, src3.string(), cache);
    size_t n = 0; for (auto& e : fs::directory_iterator(root / "cache")) { (void)e; ++n; }
    report("prefer-cached writes one file per script", n == 3);
    report("an empty cache dir writes nothing", [&] {
        embr::Interpreter i; size_t before = n;
        compileSourcePreferCached("print(9)\n", i, (root / "nine.embr").string(), "");
        size_t after = 0; for (auto& e : fs::directory_iterator(root / "cache")) { (void)e; ++after; }
        return after == before;
    }());

    write(src2, "print(22)\n");        // changed
    fs::remove(src3);                  // gone
    write(root / "cache" / "junk.vmbr", "not a compiled file");
    write(root / "cache" / "notes.txt", "keep me");
    size_t removed = cleanCache(cache);
    report("cleanCache removes changed, missing-source and damaged files", removed == 3, "removed " + std::to_string(removed));
    report("cleanCache keeps valid files and other files",
           fileExists(root / "cache" / cacheFileName(src1.string())) && fileExists(root / "cache" / "notes.txt"));
    removed = cleanCache(cache, true);
    report("cleanCache(all) removes every .vmbr but nothing else", removed == 1 && fileExists(root / "cache" / "notes.txt"));
    report("cleanCache on a missing folder is fine", cleanCache((root / "nope").string()) == 0);
    fs::remove_all(root, ec);
}

// a one-chunk program from a list of {op, operand, arg}. strings and constants are filled in so index checks pass
static CompiledProgram handmade(std::initializer_list<std::array<int, 3>> ops) {
    auto c = std::make_shared<CompiledChunk>();
    c->name = "<top>";
    c->strings = {"x"};
    c->nums = {1.0};
    c->ints = {1};
    for (auto& o : ops) {
        Instruction in; in.op = (Op)o[0]; in.operand = o[1]; in.arg = o[2];
        c->code.push_back(in); c->lines.push_back(1);
    }
    CompiledProgram prog; prog.chunks.push_back(c);
    return prog;
}
static bool refusedByChecker(CompiledProgram prog) {
    try { verifyStack(prog); } catch (const embr::EmbrError&) { return true; }
    return false;
}
#define OP(o, a, b) std::array<int, 3>{(int)Op::o, a, b}

// compiled without the in-place ops, a program has none of them and gives the same answers
static void testInPlaceOff() {
    const char* src =
        "import \"embrlib\"\n"
        "a = [1, 2, 3]\nb = a\na[1] = 20\nprint(a[1] + b[1])\n"
        "m = {\"k\": 1}\nm[\"k\"] = m[\"k\"] + 1\nprint(m[\"k\"])\n"
        "s = \"x\"\ns = s + \"y\"\nprint(s)\n"
        "arr = []\narr = push(arr, 7)\narr = push(arr, 8)\nprint(arr)\n"
        "fn f()\n  local v = [0, 0]\n  v[1] = 5\n  return v[1]\nend\nprint(f())\n";
    auto hasInPlace = [](const CompiledProgram& p) {
        for (auto& c : p.chunks) for (auto& in : c->code)
            if (in.op == Op::APPEND_VAR || in.op == Op::INDEX_GET_VAR || in.op == Op::INDEX_SET_VAR ||
                in.op == Op::STASH_VAR || in.op == Op::APPEND_ARR) return true;
        return false;
    };
    embr::Interpreter on, off;
    CompileOptions fastOpts, slowOpts; slowOpts.inPlaceOps = false;
    auto fast = compileSource(src, on, "t.embr", fastOpts);
    auto slow = compileSource(src, off, "t.embr", slowOpts);
    report("compiled normally, a program uses the in-place ops", hasInPlace(fast));
    report("compiled with inPlaceOps off, it uses none of them", !hasInPlace(slow));
    embr::Interpreter r1, r2;
    std::string o1 = run(fast, r1), o2 = run(slow, r2);
    report("both give the same output", o1 == o2 && o1 == "22\n2\nxy\n[7, 8]\n5\n", "fast: " + o1 + " slow: " + o2);
}

// stores that carry their variable's type, and stores proved to fit, behave exactly like stores that look the type up
static void testTypedStores() {
    const char* src =
        "fn lh(v) return v end import \"embrlib\"\n"
        "n = 0\ni = 0\nwhile i < 5\n  n = n + i\n  i = i + 1\nend\nprint(n)\n"
        "s = \"a\"\ns = s + \"b\"\ns = \"c\"\nprint(s)\n"
        "fn f(x)\n  total = 0\n  k = 0\n  while k < x\n    total = total + k\n    k = k + 1\n  end\n  return total\nend\nprint(f(5))\n"
        "w = lh(1.5)\ntry\n  w = lh(\"s\")\n  print(\"unreachable\")\ncatch e\n  print(\"caught\")\nend\n"
        "m = 1\ntry\n  m = m / 2\n  print(\"unreachable\")\ncatch e2\n  print(e2[\"message\"])\nend\n";
    auto hasText = [](const CompiledProgram& p, const std::string& what) {
        std::ostringstream o; disassemble(p, o);
        return o.str().find(what) != std::string::npos;
    };
    CompileOptions typed, plain; plain.useTypes = false;
    typed.typeCheck = plain.typeCheck = false;       // `m = m / 2` is a mistake the checker would refuse, this is about running it
    embr::Interpreter a, b, c, d;
    auto pt = compileSource(src, a, "t.embr", typed);
    auto pp = compileSource(src, b, "t.embr", plain);
    report("typed stores show up in the code", hasText(pt, " : int") && hasText(pt, "[proven]"));
    report("without the type information there are none", !hasText(pp, " : int") && !hasText(pp, "[proven]"));
    std::string o1 = run(pt, c), o2 = run(pp, d);
    report("both give the same output", o1 == o2 && o1.find("caught") != std::string::npos && o1.find("cannot assign") != std::string::npos,
           "typed: " + o1 + " plain: " + o2);
}

static void testRunLimited() {
    embr::Interpreter a;
    auto endless = compileSource("while 1\nend\n", a, "t.embr");
    bool stopped = false;
    { VmRunner r(a); stopped = !r.runLimited(endless, 5000); r.releaseFrames(); }
    report("runLimited stops a program that never ends", stopped);
    embr::Interpreter b;
    auto finite = compileSource("x = 1 + 2\n", b, "t.embr");
    bool finished = false;
    { VmRunner r(b); finished = r.runLimited(finite, 5000); }
    report("runLimited reports a program that finishes", finished);
}

static void testStackChecker() {
    report("a program that leaves a value and returns is accepted",
           !refusedByChecker(handmade({OP(PUSH_INT, 0, 0), OP(RETURN, 0, 0)})));
    report("popping an empty stack is refused", refusedByChecker(handmade({OP(POP, 0, 0)})));
    report("BUILD_ARRAY with too few values is refused",
           refusedByChecker(handmade({OP(PUSH_INT, 0, 0), OP(PUSH_INT, 0, 0), OP(BUILD_ARRAY, 3, 0)})));
    report("a huge count is refused", refusedByChecker(handmade({OP(BUILD_ARRAY, 2000000000, 0)})));
    report("CALL needs the callee and its arguments",
           refusedByChecker(handmade({OP(PUSH_INT, 0, 0), OP(CALL, 1, 0)})));
    report("RETURN needs a value", refusedByChecker(handmade({OP(RETURN, 0, 0)})));
    report("ITER_NEXT without an open loop is refused", refusedByChecker(handmade({OP(ITER_NEXT, 1, 0)})));
    report("ITER_POP without an open loop is refused", refusedByChecker(handmade({OP(ITER_POP, 0, 0)})));
    report("TRY_POP without a try is refused", refusedByChecker(handmade({OP(TRY_POP, 0, 0)})));
    report("APPEND_VAR needs its value", refusedByChecker(handmade({OP(APPEND_VAR, 0, 1)})));
    report("two paths that meet with different stacks are refused",
           refusedByChecker(handmade({OP(PUSH_INT, 0, 0), OP(JUMP_IF_FALSE, 3, 0), OP(PUSH_INT, 0, 0), OP(POP, 0, 0)})));
    report("paths that meet with the same stack are accepted",
           !refusedByChecker(handmade({OP(PUSH_INT, 0, 0), OP(JUMP_IF_FALSE, 3, 0), OP(NOP, 0, 0), OP(NOP, 0, 0)})));
    report("a for loop over a value, with its exit, is accepted",
           !refusedByChecker(handmade({OP(PUSH_INT, 0, 0), OP(ITER_INIT, 1, 0), OP(ITER_NEXT, 5, 0), OP(POP, 0, 0), OP(JUMP, 2, 0), OP(ITER_POP, 0, 0)})));
    report("a for loop whose body keeps the loop value is refused (the loop head sees two stacks)",
           refusedByChecker(handmade({OP(PUSH_INT, 0, 0), OP(ITER_INIT, 1, 0), OP(ITER_NEXT, 4, 0), OP(JUMP, 2, 0), OP(ITER_POP, 0, 0)})));
    report("a try block with its catch code is accepted",
           !refusedByChecker(handmade({OP(TRY_PUSH, 4, 0), OP(NOP, 0, 0), OP(TRY_POP, 0, 0), OP(JUMP, 5, 0), OP(POP, 0, 0), OP(NOP, 0, 0)})));
    report("code nothing can reach is not checked",
           !refusedByChecker(handmade({OP(PUSH_INT, 0, 0), OP(RETURN, 0, 0), OP(POP, 0, 0), OP(POP, 0, 0)})));
    // closures: what a closure captures has to exist in the frame that makes it
    {
        auto prog = handmade({OP(MAKE_CLOSURE, 1, 0), OP(RETURN, 0, 0)});
        auto child = std::make_shared<CompiledChunk>();
        child->name = "f"; child->parentIdx = 0;
        child->upvalDescs.push_back({true, 0});          // the parent has no boxed locals, so there is no cell 0
        prog.chunks.push_back(child);
        bool refusedBadCapture = false;
        try { deserializeProgram(serializeProgram(prog, 0), "t.embr"); } catch (const embr::EmbrError&) { refusedBadCapture = true; }
        report("a captured variable the enclosing function doesn't have is refused", refusedBadCapture);

        prog.chunks[1]->upvalDescs.clear();
        prog.chunks[1]->parentIdx = -1;                   // not written inside chunk 0
        bool refusedElsewhere = false;
        try { deserializeProgram(serializeProgram(prog, 0), "t.embr"); } catch (const embr::EmbrError&) { refusedElsewhere = true; }
        report("a closure of a function written somewhere else is refused", refusedElsewhere);
    }
    // the checker is part of loading, so a file with such code is refused too
    bool refused = false;
    try { deserializeProgram(serializeProgram(handmade({OP(POP, 0, 0)}), 0), "t.embr"); } catch (const embr::EmbrError&) { refused = true; }
    report("a file with unbalanced code is refused on load", refused);
}

// a program that has run has fused instructions in it (see quickenChunk), and none of them may reach a file, the disassembler
// or the checkers: they all see the code as the compiler wrote it
static void testFusedOps() {
    const char* src = "fn f(n)\n  i = 0\n  t = 0\n  while i < n\n    t = t + i % 7\n    i = i + 1\n  end\n  return t\nend\n"
                      "print(f(50))\nj = 0\nwhile j < 10\n  j = j + 1\nend\nprint(j)\n";
    embr::Interpreter a;
    auto prog = compileSource(src, a, "t.embr");
    std::ostringstream before, after;
    disassemble(prog, before);
    const std::string bytesBefore = serializeProgram(prog, 1, "t.embr");
    std::string out = run(prog, a);
    bool fused = false;
    for (auto& c : prog.chunks) fused = fused || !c->quickOrig.empty();
    report("running a program fuses some of its instructions", fused);
    disassemble(prog, after);
    report("a program that has run disassembles as the compiler wrote it", before.str() == after.str());
    report("a program that has run is written as the compiler wrote it", serializeProgram(prog, 1, "t.embr") == bytesBefore);
    bool ok = true;
    try { validateProgram(prog); verifyStack(prog); } catch (const embr::EmbrError&) { ok = false; }
    report("a program that has run passes the checkers", ok);
    embr::Interpreter b;
    bool& fuse = fuseOpsEnabled(); const bool was = fuse; fuse = false;
    auto plain = compileSource(src, b, "t.embr");
    std::string out2 = run(plain, b);
    fuse = was;
    bool none = true;
    for (auto& c : plain.chunks) none = none && c->quickOrig.empty();
    report("with fusing off nothing is fused and the output is the same", none && out == out2 && out == "147\n10\n", "got: " + out2);
    embr::Interpreter c3;
    bool& fast = fastLoopEnabled(); const bool wasFast = fast; fast = false;
    auto slow = compileSource(src, c3, "t.embr");
    std::string out3 = run(slow, c3);
    fast = wasFast;
    report("with the hot loop off the output is the same", out3 == out, "got: " + out3);
}

// a parameter that has no local slot (only a damaged compiled file can have one) is an error, not a failed assert or a write outside the frame
static void testParamWithoutSlot() {
    embr::Interpreter a;
    auto prog = compileSource("fn f(x)\n    return x\nend\nprint(f(1))\n", a, "t.embr");
    for (auto& c : prog.chunks) if (c->name == "f" && !c->params.empty()) c->params[0].name = "no_such_slot";
    std::string out = run(prog, a);
    report("a parameter without a slot is refused", out.find("damaged compiled code") != std::string::npos, "got: " + out);
}

int main() {
    std::cout << "\n\033[1m bytecode \033[0m\n";
    try {
        testRoundTrip();
        testDamage();
        testCache();
        testStackChecker();
        testRunLimited();
        testInPlaceOff();
        testTypedStores();
        testFusedOps();
        testParamWithoutSlot();
    } catch (const std::exception& e) {
        report("no exception escapes", false, e.what());
    }
    std::cout << "\n  " << g_passed << " passed, " << g_failed << " failed"
              << (g_failed ? "  \033[31mX\033[0m" : "  \033[32m\xE2\x88\x9A\033[0m") << "\n\n";
    return g_failed ? 1 : 0;
}
