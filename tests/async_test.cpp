// unit tests for plugins/async/async.cpp. embrlib (write_file) and table (one "rejects an unsafe value" test) are
// imported only as fixtures
//
// the fixture module (test_async_worker.embr: square/maybe_raise) imports embrlib for error(), so the first time a
// pool worker loads it a "[runtime] loaded plugin: <absolute path>" banner is printed. every test that might be the
// first task on a worker compares by suffix (runSuffixTest() below, see parallel_test.cpp for why that is as strong
// as an exact match). only the two checks that raise before touching the pool (rejecting an unsafe async_spawn
// argument) and the "set pool size before anything started" check are plain exact-match tests
//
// order matters for async_pool_size(): the "before started" case is a plain Test (all plain Tests run before any
// SuffixTest), and the "after started" case is deliberately the last SuffixTest, so the pool is already running

#include "harness.h"

// like embr_test::Test/runTest, but passes when the captured output ENDS
// WITH expectedSuffix rather than equals it -- see file header for why.
struct SuffixTest {
    std::string name, code, expectedSuffix;
};

static bool runSuffixTest(const SuffixTest& t, embr::Interpreter& interp) {
    std::ostringstream capOut;
    std::streambuf* origOut = std::cout.rdbuf(capOut.rdbuf());

    bool passed = false;
    std::string errorMsg;
    try {
        embr_test::runSourceAny(t.code, interp, t.name);
        const std::string& actual = capOut.str();
        passed = actual.size() >= t.expectedSuffix.size() &&
                 actual.compare(actual.size() - t.expectedSuffix.size(),
                                t.expectedSuffix.size(), t.expectedSuffix) == 0;
        if (!passed) errorMsg = "output did not end with expected suffix";
    } catch (const embr::EmbrError& e) {
        errorMsg = std::string("EmbrError: ") + e.what();
    } catch (const std::exception& e) {
        errorMsg = std::string("exception: ") + e.what();
    }

    std::cout.rdbuf(origOut);

    static const char* G = "\033[32m";
    if (passed) {
        std::cout << G << " [+]" << embr::X << " " << t.name << "\n";
    } else {
        std::cout << embr::R << " [-]" << embr::X << " " << t.name << "\n";
        if (!errorMsg.empty())
            std::cout << embr::Y << "        error: " << embr::X << errorMsg << "\n";
        std::cout << "        expected suffix: " << embr::valueRepr(embr::Value(t.expectedSuffix)) << "\n";
    }
    return passed;
}

int main() {
    std::cout << std::unitbuf;   // a hung test still shows how far the suite got
    using embr_test::Test;
    std::vector<Test> tests = {

        { "async_queue_limit: reads and sets the limit, 0 means none, a negative one is refused",
          "print(async_queue_limit())\nprint(async_queue_limit(5))\nprint(async_queue_limit(0))\n"
          "try\nasync_queue_limit(-1)\ncatch e\nprint(\"caught\")\nend\n",
          "10000\n5\n0\ncaught\n" },

        { "async_spawn: rejects a value that isn't safe to share across threads (a plain table() handle)",
          "t = table()\n"
          "try\nasync_spawn(\"test_async_worker.embr\", \"square\", t)\nprint(\"unreachable\")\ncatch e\nprint(\"caught\")\nend\n",
          "caught\n" },

        { "async_spawn: rejects a closure",
          "try\nasync_spawn(\"test_async_worker.embr\", \"square\", fn(x) return x end)\nprint(\"unreachable\")\ncatch e\nprint(\"caught\")\nend\n",
          "caught\n" },

        { "async_pool_size: can be set before the pool has started",
          "async_pool_size(3)\nprint(\"ok\")\n",
          "ok\n" },
    };

    std::vector<SuffixTest> workerTests = {

        { "async_spawn/async_await: basic round trip, doesn't block the caller from doing other work first",
          "write_file(\"test_async_worker.embr\","
          " \"import \\\"embrlib\\\"\\n\\nfn square(x)\\n    return x * x\\nend\\n\\nfn maybe_raise(x)\\n    if x < 0\\n        error(\\\"negative input\\\")\\n    end\\n    return x\\nend\\n\")\n"
          "p = async_spawn(\"test_async_worker.embr\", \"square\", 6)\n"
          "print(\"spawned\")\n" // proves async_spawn returned before the task necessarily finished
          // (not asserted on below: the worker's own "[runtime] loaded
          // plugin" banner for its internal `import "embrlib"` can land
          // right after "spawned" -- see file header)
          "r = async_await(p)\n"
          "print(r[\"ok\"])\n"
          "print(r[\"value\"])\n",
          "1\n36\n" },

        { "async_await: a task that raises comes back as {ok:0,error:...} instead of crashing",
          "p = async_spawn(\"test_async_worker.embr\", \"maybe_raise\", -1)\n"
          "r = async_await(p)\n"
          "print(r[\"ok\"])\n"
          "print(r[\"error\"][\"message\"])\n",
          "0\nnegative input\n" },

        { "async_await: safe to call more than once on the same (already resolved) promise",
          "p = async_spawn(\"test_async_worker.embr\", \"square\", 7)\n"
          "r1 = async_await(p)\n"
          "r2 = async_await(p)\n"
          "print(r1[\"value\"])\nprint(r2[\"value\"])\n",
          "49\n49\n" },

        { "async_try_await: ready=1 with the same fields async_await returns, once a promise has resolved",
          "p = async_spawn(\"test_async_worker.embr\", \"square\", 8)\n"
          "async_await(p)\n" // force it to be finished before we try_await
          "r = async_try_await(p)\n"
          "print(r[\"ready\"])\nprint(r[\"ok\"])\nprint(r[\"value\"])\n",
          "1\n1\n64\n" },

        { "async_all: preserves order across multiple concurrently-run tasks",
          "ps = [async_spawn(\"test_async_worker.embr\", \"square\", 1),"
          " async_spawn(\"test_async_worker.embr\", \"square\", 2),"
          " async_spawn(\"test_async_worker.embr\", \"square\", 3),"
          " async_spawn(\"test_async_worker.embr\", \"square\", 4),"
          " async_spawn(\"test_async_worker.embr\", \"square\", 5)]\n"
          "rs = async_all(ps)\n"
          "i = 0\nwhile i < len(rs)\n  print(rs[i][\"value\"])\n  i = i + 1\nend\n",
          "1\n4\n9\n16\n25\n" },

        { "async_queue_limit: a spawner that gets ahead of the pool waits instead of failing or growing the queue",
          "print(async_queue_limit(1))\n"
          "ps = []\ni = 0\nwhile i < 40\n  ps = push(ps, async_spawn(\"test_async_worker.embr\", \"square\", i))\n  i = i + 1\nend\n"
          "rs = async_all(ps)\n"
          "total = 0\nj = 0\nwhile j < len(rs)\n  total = total + rs[j][\"value\"]\n  j = j + 1\nend\n"
          "print(total)\n",
          "1\n20540\n" },

        { "async_all: reports each task's own outcome without aborting the batch on one failure",
          "ps = [async_spawn(\"test_async_worker.embr\", \"maybe_raise\", 1),"
          " async_spawn(\"test_async_worker.embr\", \"maybe_raise\", -1),"
          " async_spawn(\"test_async_worker.embr\", \"maybe_raise\", 3)]\n"
          "rs = async_all(ps)\n"
          "print(rs[0][\"ok\"])\nprint(rs[0][\"value\"])\n"
          "print(rs[1][\"ok\"])\nprint(rs[1][\"error\"][\"message\"])\n"
          "print(rs[2][\"ok\"])\nprint(rs[2][\"value\"])\n",
          "1\n1\n0\nnegative input\n1\n3\n" },

        { "async_try_await: ready=0 for a task still in flight, ready=1 with the result once awaited",
          "write_file(\"test_async_slow_worker.embr\","
          " \"import \\\"time\\\"\\n\\nfn slow_double(x)\\n    time_sleep(0.2)\\n    return x * 2\\nend\\n\")\n"
          "p = async_spawn(\"test_async_slow_worker.embr\", \"slow_double\", 21)\n"
          "early = async_try_await(p)\n"
          "print(\"early_ready=\" + str(early[\"ready\"]))\n"
          // not asserted above (see the file header): the worker's "[runtime] loaded plugin" banner for `import "time"` can
          // appear anywhere up to here. everything below runs after async_await(p) returned, so after that banner, and is safe to check
          "r = async_await(p)\n"
          "late = async_try_await(p)\n"
          "print(r[\"ok\"])\nprint(r[\"value\"])\n"
          "print(late[\"ready\"])\nprint(late[\"value\"])\n",
          "1\n42\n1\n42\n" },

        { "async_pool_size: raises once the pool has already started (from earlier tasks in this test binary)",
          "try\nasync_pool_size(1)\nprint(\"unreachable\")\ncatch e\nprint(\"caught\")\nend\n",
          "caught\n" },
    };

    const std::vector<std::string> plugins = {"async", "embrlib", "table"};

    int passed = 0, failed = 0;
    std::cout << "\n\033[1m async plugin test suite \033[0m\n";
    for (const auto& t : tests) {
        auto interp = embr_test::freshInterpreter(plugins);
        auto r = embr_test::runTest(t, *interp);
        embr_test::printResult(r);
        r.passed ? ++passed : ++failed;
    }
    // these share one interpreter: the shared pool of worker threads lives as long as an interpreter does, and the last test
    // counts on tasks from the ones before it having started it
    auto workerInterp = embr_test::freshInterpreter(plugins);
    for (const auto& t : workerTests) {
        bool ok = runSuffixTest(t, *workerInterp);
        ok ? ++passed : ++failed;
    }

    std::cout << "\n"
              << "  " << passed << " passed, " << failed << " failed"
              << (failed ? "  \033[31mX\033[0m" : "  \033[32m√\033[0m") << "\n\n";
    return failed == 0 ? 0 : 1;
}
