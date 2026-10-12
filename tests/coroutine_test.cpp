// unit tests for plugins/coroutine/coroutine.cpp
//
// coroutine_create() only accepts a VM-compiled function, so most tests call embr::vm::runSource directly instead of
// harness.h's runSourceAny(). that keeps them on the VM even when a build has another backend, which can't run
// coroutines yet
//
// one test checks the error path: coroutine_create() on a function the VM did not compile must raise a clear error,
// not crash or do nothing

#include "harness.h"

// like embr_test::runTest, but forces the VM backend (embr::vm::runSource)
// instead of harness.h's runSourceAny() -- see file header.
static embr_test::Result runVmTest(const embr_test::Test& t, embr::Interpreter& interp) {
    std::ostringstream capOut;
    std::istringstream fakeIn(t.stdinData);
    std::streambuf* origOut = std::cout.rdbuf(capOut.rdbuf());
    std::streambuf* origIn  = std::cin.rdbuf(fakeIn.rdbuf());

    embr_test::Result r; r.name = t.name; r.expected = t.expectedOutput;
    try {
        embr::vm::runSource(t.code, interp, t.name);
        r.actual = capOut.str();
        r.passed = (r.actual == r.expected);
        if (!r.passed) r.errorMsg = "output mismatch";
    } catch (const embr::EmbrError& e) {
        r.actual = capOut.str(); r.passed = false;
        r.errorMsg = std::string("EmbrError: ") + e.what();
    } catch (const std::exception& e) {
        r.actual = capOut.str(); r.passed = false;
        r.errorMsg = std::string("exception: ") + e.what();
    }
    std::cout.rdbuf(origOut);
    std::cin.rdbuf(origIn);
    return r;
}

static int runVmSuite(const std::string& suiteName, const std::vector<embr_test::Test>& tests) {
    int passed = 0, failed = 0;
    std::cout << "\n\033[1m " << suiteName << " \033[0m\n";
    for (const auto& t : tests) {
        auto interp = embr_test::freshInterpreter({"coroutine"});
        embr_test::Result r = runVmTest(t, *interp);
        embr_test::printResult(r);
        r.passed ? ++passed : ++failed;
    }
    std::cout << "\n  " << passed << " passed, " << failed << " failed"
               << (failed ? "  \033[31mX\033[0m" : "  \033[32m\xE2\x88\x9A\033[0m") << "\n\n";
    return failed == 0 ? 0 : 1;
}

int main() {
    int rc = 0;

    rc |= runVmSuite("coroutine (forced VM backend)", {
        { "basic generator: yields, and each resume's argument becomes "
          "coroutine_yield()'s return value inside the body",
          "fn counter(start)\n"
          "    local n = start\n"
          "    while 1\n"
          "        local step = coroutine_yield(n)\n"
          "        n = n + step\n"
          "    end\n"
          "end\n"
          "local co = coroutine_create(counter)\n"
          "print(coroutine_resume(co, 10))\n"
          "print(coroutine_resume(co, 1))\n"
          "print(coroutine_resume(co, 5))\n",
          "10\n11\n16\n" },

        { "consecutive yields with no loop between them (regression test for "
          "the inject()-buffer gap VmRunner::suspend() was written to avoid -- "
          "see vm.h's own comment on inject())",
          "fn gen()\n"
          "    coroutine_yield(1)\n"
          "    coroutine_yield(2)\n"
          "    return 3\n"
          "end\n"
          "local co = coroutine_create(gen)\n"
          "print(coroutine_resume(co))\n"
          "print(coroutine_resume(co))\n"
          "print(coroutine_resume(co))\n",
          "1\n2\n3\n" },

        { "coroutine_status reflects suspended -> dead",
          "fn once()\n"
          "    coroutine_yield(0)\n"
          "    return 0\n"
          "end\n"
          "local co = coroutine_create(once)\n"
          "print(coroutine_status(co))\n"
          "coroutine_resume(co)\n"
          "print(coroutine_status(co))\n"
          "coroutine_resume(co)\n"
          "print(coroutine_status(co))\n",
          "suspended\nsuspended\ndead\n" },

        { "resuming a dead coroutine raises, catchable with try/catch",
          "fn once()\n"
          "    return 0\n"
          "end\n"
          "local co = coroutine_create(once)\n"
          "coroutine_resume(co)\n"
          "try\n"
          "    coroutine_resume(co)\n"
          "    print(\"unreachable\")\n"
          "catch e\n"
          "    print(e.message)\n"
          "end\n",
          "cannot resume a dead coroutine\n" },

        { "resuming a coroutine from inside its own body raises",
          "local any co = 0\n"
          "fn selfResume()\n"
          "    try\n"
          "        coroutine_resume(co)\n"
          "        print(\"unreachable\")\n"
          "    catch e\n"
          "        print(e.message)\n"
          "    end\n"
          "    coroutine_yield(0)\n"
          "end\n"
          "co = coroutine_create(selfResume)\n"
          "coroutine_resume(co)\n",
          "coroutine is already running\n" },

        { "coroutine_yield outside a coroutine raises",
          "try\n"
          "    coroutine_yield(0)\n"
          "    print(\"unreachable\")\n"
          "catch e\n"
          "    print(e.message)\n"
          "end\n",
          "coroutine_yield() called outside a running coroutine\n" },

        { "nested coroutines: one created and driven to completion from inside another",
          "fn inner()\n"
          "    coroutine_yield(\"inner-1\")\n"
          "    return \"inner-done\"\n"
          "end\n"
          "fn outer()\n"
          "    local ico = coroutine_create(inner)\n"
          "    print(coroutine_resume(ico))\n"
          "    print(coroutine_resume(ico))\n"
          "    coroutine_yield(\"outer-1\")\n"
          "    return \"outer-done\"\n"
          "end\n"
          "local oco = coroutine_create(outer)\n"
          "print(coroutine_resume(oco))\n"
          "print(coroutine_resume(oco))\n",
          "inner-1\ninner-done\nouter-1\nouter-done\n" },

        // each abandoned coroutine used to keep its two frames counted against the 1000-frame call limit forever
        { "abandoning suspended coroutines does not use up the call depth",
          "fn leaf()\n"
          "    coroutine_yield(1)\n"
          "    return 0\n"
          "end\n"
          "fn body()\n"
          "    return leaf()\n"
          "end\n"
          "fn spin()\n"
          "    local co = coroutine_create(body)\n"
          "    coroutine_resume(co)\n"
          "end\n"
          "local i = 0\n"
          "while i < 3000\n"
          "    spin()\n"
          "    i = i + 1\n"
          "end\n"
          "print(\"ok\")\n",
          "ok\n" },

        { "a coroutine that raises gives its call depth back",
          "fn boom()\n"
          "    return 1 / 0\n"
          "end\n"
          "fn body()\n"
          "    return boom()\n"
          "end\n"
          "local i = 0\n"
          "while i < 3000\n"
          "    local co = coroutine_create(body)\n"
          "    try\n"
          "        coroutine_resume(co)\n"
          "    catch e\n"
          "    end\n"
          "    i = i + 1\n"
          "end\n"
          "print(\"ok\")\n",
          "ok\n" },
    });

    // a native is not a VM-compiled script function, so it can't be a coroutine body
    rc |= embr_test::runSuite("coroutine (body that is not a VM function)", {
        {
            "coroutine_create() raises a clear error when the function is not VM-compiled",
            "try\n    coroutine_create(coroutine_status)\n    print(\"unreachable\")\n"
            "catch e\n    print(e.message)\nend\n",
            "coroutine body must be a script function compiled by the VM backend\n"
        },
    }, {"coroutine"});

    return rc;
}
