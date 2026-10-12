// unit tests for VmRunner's C++ API (backends/vm.h), which no embr script or plugin reaches, so harness.h's
// script-driven tests can't cover it
//
// right now that's inject(). a native calling inject() twice before the first buffer was used up by a jump or call
// used to insert at the wrong position and replay already-run instructions, which hung the runner (probe() below
// triggers it). plugins/coroutine uses suspend() instead and never touches this path, so it needs its own test
//
// tests/CMakeLists.txt sets a TIMEOUT on this test, so a regression fails fast instead of hanging CI

#include "harness.h"

static void report(const char* name, bool passed, const std::string& errorMsg, const std::string& expected, const std::string& actual) {
    static const char* G = "\033[32m";
    static const char* R = "\033[31m";
    static const char* Y = "\033[33m";
    static const char* X = "\033[0m";
    if (passed) {
        std::cout << G << " [+]" << X << " " << name << "\n";
        return;
    }
    std::cout << R << " [-]" << X << " " << name << "\n";
    if (!errorMsg.empty()) std::cout << Y << "        error: " << X << errorMsg << "\n";
    std::cout << "        expected: " << embr::valueRepr(embr::Value(expected)) << "\n";
    std::cout << "        actual:   " << embr::valueRepr(embr::Value(actual)) << "\n";
}

static bool testDoubleInjectDoesNotReplayStaleInstructions() {
    const char* name = "inject(): a second call before the first injection is fully "
                        "consumed doesn't replay stale instructions";

    embr::Interpreter interp;
    embr::vm::VmRunner* runnerPtr = nullptr;

    // each call splices in a single, otherwise-inert NOP -- any observable
    // difference in behavior comes purely from inject()'s own bookkeeping,
    // not from anything the injected instruction itself does.
    interp.bind("probe", [&](const std::vector<embr::Value>&) -> embr::Value {
        runnerPtr->inject({ embr::vm::Instruction{embr::vm::Op::NOP} });
        return embr::Value(0.0);
    });

    // two probe() calls in a row, deliberately with no loop/jump between
    // them -- that's exactly the shape that leaves injBuf_ still active for
    // the second call. under the old bug, the second inject() replayed the
    // first injection's stale prefix (including the "print(\"b\")" and the
    // first probe() CALL itself), corrupting the stack and infinite-looping.
    std::string src =
        "print(\"a\")\n"
        "probe()\n"
        "print(\"b\")\n"
        "probe()\n"
        "print(\"c\")\n";

    std::ostringstream capOut;
    std::streambuf* origOut = std::cout.rdbuf(capOut.rdbuf());

    bool passed = true;
    std::string errorMsg;
    try {
        auto prog = embr::vm::compileSource(src, interp, "<probe>");
        embr::vm::VmRunner runner(interp);
        runnerPtr = &runner;
        runner.run(prog);
    } catch (const embr::EmbrError& e) {
        passed = false;
        errorMsg = std::string("EmbrError: ") + e.what();
    } catch (const std::exception& e) {
        passed = false;
        errorMsg = std::string("exception: ") + e.what();
    }

    std::cout.rdbuf(origOut);
    std::string actual = capOut.str();
    if (passed && actual != "a\nb\nc\n") {
        passed = false;
        errorMsg = "output mismatch";
    }

    report(name, passed, errorMsg, "a\nb\nc\n", actual);
    return passed;
}

// runs src with a native probe() that injects a NOP when `inject` is set, and returns what the script printed
static std::string runWithProbe(const std::string& src, bool inject, std::string& error) {
    embr::Interpreter interp;
    embr::vm::VmRunner* runnerPtr = nullptr;
    interp.bind("probe", [&](const std::vector<embr::Value>&) -> embr::Value {
        if (inject) runnerPtr->inject({ embr::vm::Instruction{embr::vm::Op::NOP} });
        return embr::Value(0.0);
    });
    std::ostringstream capOut;
    std::streambuf* origOut = std::cout.rdbuf(capOut.rdbuf());
    try {
        auto prog = embr::vm::compileSource(src, interp, "<probe>");
        embr::vm::VmRunner runner(interp);
        runnerPtr = &runner;
        runner.run(prog);
    } catch (const embr::EmbrError& e) {
        error = std::string("EmbrError: ") + e.what();
    } catch (const std::exception& e) {
        error = std::string("exception: ") + e.what();
    }
    std::cout.rdbuf(origOut);
    return capOut.str();
}

// fused instructions (two or three instructions made into one when a chunk first runs, see quickenChunk) that run from the
// inject() buffer can't use their fast form and run the instructions they replaced (EMBR_FUSED_FALLBACK in execOne).
// nothing else reaches that path, so: the same program with and without an injection must print the same thing
static bool testFusedOpsAfterInjectGiveTheSameResults() {
    const char* name = "inject(): fused arithmetic, comparisons and indexing after an injection fall back and give the same results";
    std::string src =
        "g = 10\n"
        "h = 2.5\n"
        "fn work(n)\n"
        "    probe()\n"
        "    a = n + 1\n"
        "    b = n * a\n"
        "    c = a - b\n"
        "    k = a + b\n"
        "    d = g + 3\n"
        "    e = g * n\n"
        "    m = g - h\n"
        "    xs = [n, a]\n"
        "    f = xs[1]\n"
        "    q = xs[0] + a\n"
        "    if n < 5\n"
        "        print(\"lt\")\n"
        "    end\n"
        "    return [a, b, c, k, d, e, m, f, q]\n"
        "end\n"
        "print(work(4))\n"
        "print(work(7))\n"
        "probe()\n"
        "t = g + 1\n"
        "u = g * g\n"
        "v = g - h\n"
        "print(t + u)\n"
        "print(v)\n";
    const std::string expected = "lt\n[5, 20, -15, 25, 13, 40, 7.5, 5, 9]\n[8, 56, -48, 64, 13, 70, 7.5, 8, 15]\n111\n7.5\n";
    std::string err1, err2;
    std::string plain = runWithProbe(src, false, err1);
    std::string injected = runWithProbe(src, true, err2);
    bool passed = err1.empty() && err2.empty() && plain == expected && injected == expected;
    report(name, passed, err1.empty() ? err2 : err1, expected, injected);
    return passed;
}

int main() {
    std::cout << "\n\033[1m vm_internals \033[0m\n";
    int ok = 0, total = 0;
    for (bool (*t)() : {testDoubleInjectDoesNotReplayStaleInstructions, testFusedOpsAfterInjectGiveTheSameResults}) {
        ++total;
        if (t()) ++ok;
    }
    std::cout << "\n  " << ok << " passed, " << (total - ok) << " failed"
               << (ok == total ? "  \033[32m\xE2\x88\x9A\033[0m" : "  \033[31mX\033[0m") << "\n\n";
    return ok == total ? 0 : 1;
}
