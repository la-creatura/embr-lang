// unit tests for plugins/parallel/parallel.cpp. embrlib (write_file, str) and table (one "rejects an unsafe
// value" test) are imported only as fixtures
//
// the worker_spawn tests write a small fixture module with write_file() (like core_test.cpp's import tests), since
// worker_spawn resolves module_path the same way `import` does
//
// those tests compare by suffix, not exact equality (runSuffixTest() below). the fixture module imports plugins at
// its top level, and each import prints "[runtime] loaded plugin: <absolute path>". that path isn't portable, and
// with several worker threads the order of the banners isn't fixed. each test only asserts on what its script
// prints after every worker has been joined, so a suffix match is just as strong
//
// the "shared_table_update under real concurrency" test matters most: 4 real threads each add 1 to a shared
// counter 50 times, and the total must be exactly 200. with a missing or broken lock it would sometimes come out short

#include "harness.h"

// like embr_test::Test/runTest, but passes when the captured output ENDS
// WITH expectedSuffix rather than equals it -- see file header for why the
// worker_spawn-based tests need this instead of the normal exact-match Test.
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
    using embr_test::Test;
    std::vector<Test> tests = {

        { "channel_send/channel_recv: basic round trip",
          "ch = channel()\nchannel_send(ch, 42)\nprint(channel_recv(ch))\n",
          "42\n" },

        { "channel_try_send: a full bounded channel says no instead of waiting, an unbounded one always takes it",
          "ch = channel(2)\n"
          "print(channel_try_send(ch, 1))\nprint(channel_try_send(ch, 2))\nprint(channel_try_send(ch, 3))\n"
          "print(channel_recv(ch))\nprint(channel_try_send(ch, 3))\n"
          "u = channel()\nprint(channel_try_send(u, 1))\n"
          "channel_close(ch)\ntry\nchannel_try_send(ch, 9)\ncatch e\nprint(\"closed\")\nend\n",
          "1\n1\n0\n1\n1\n1\nclosed\n" },

        { "channel_try_recv: found flag reflects whether something was queued",
          "ch = channel()\n"
          "r = channel_try_recv(ch)\n"
          "print(r[0])\n"
          "channel_send(ch, \"x\")\n"
          "r2 = channel_try_recv(ch)\n"
          "print(r2[0])\n"
          "print(r2[1])\n",
          "0\n1\nx\n" },

        { "channel_len/channel_closed: reflect queue state",
          "ch = channel()\n"
          "print(channel_len(ch))\n"
          "print(channel_closed(ch))\n"
          "channel_send(ch, 1)\n"
          "print(channel_len(ch))\n"
          "channel_close(ch)\n"
          "print(channel_closed(ch))\n",
          "0\n0\n1\n1\n" },

        { "channel_close: channel_recv raises once closed and drained",
          "ch = channel()\n"
          "channel_send(ch, 1)\n"
          "channel_close(ch)\n"
          "print(channel_recv(ch))\n"
          "try\nchannel_recv(ch)\nprint(\"unreachable\")\ncatch e\nprint(\"caught\")\nend\n",
          "1\ncaught\n" },

        { "channel_send: allows nested plain data (arrays/maps of numbers/strings)",
          "ch = channel()\n"
          "channel_send(ch, {\"a\": [1,2,3], \"b\": \"hi\"})\n"
          "v = channel_recv(ch)\n"
          "print(v[\"a\"][1])\n"
          "print(v[\"b\"])\n",
          "2\nhi\n" },

        { "channel_send: rejects a value that isn't safe to share across threads (a plain table() handle)",
          "t = table()\n"
          "ch = channel()\n"
          "try\nchannel_send(ch, t)\nprint(\"unreachable\")\ncatch e\nprint(\"caught\")\nend\n",
          "caught\n" },

        { "channel_send: rejects a closure",
          "ch = channel()\n"
          "try\nchannel_send(ch, fn(x) return x end)\nprint(\"unreachable\")\ncatch e\nprint(\"caught\")\nend\n",
          "caught\n" },

        { "channel_send: allows a nested shared_table handle",
          "ch = channel()\n"
          "channel_send(ch, {\"t\": shared_table()})\n"
          "v = channel_recv(ch)\n"
          "print(shared_table_len(v[\"t\"]))\n",
          "0\n" },

        { "shared_table: basic set/get/has/delete/len",
          "t = shared_table()\n"
          "shared_table_set(t, \"x\", 1)\n"
          "print(shared_table_get(t, \"x\"))\n"
          "print(shared_table_has(t, \"y\"))\n"
          "print(shared_table_delete(t, \"x\"))\n"
          "print(shared_table_has(t, \"x\"))\n"
          "print(shared_table_len(t))\n",
          "1\n0\n1\n0\n0\n" },

        { "shared_table: keys/values/items and clear",
          "t = shared_table()\n"
          "shared_table_set(t, 1, 10)\n"
          "shared_table_set(t, 2, 20)\n"
          "ksum = 0\nfor k in shared_table_keys(t)\n  ksum = ksum + k\nend\n"
          "vsum = 0\nfor v in shared_table_values(t)\n  vsum = vsum + v\nend\n"
          "isum = 0\nfor pair in shared_table_items(t)\n  isum = isum + pair[0] + pair[1]\nend\n"
          "print(ksum)\nprint(vsum)\nprint(isum)\n"
          "shared_table_clear(t)\n"
          "print(shared_table_len(t))\n",
          "3\n30\n33\n0\n" },

        { "shared_table_get: raises 'key not found' instead of a sentinel",
          "t = shared_table()\n"
          "try\nshared_table_get(t, \"missing\")\nprint(\"unreachable\")\ncatch e\nprint(\"caught\")\nend\n",
          "caught\n" },

        { "shared_table_update: atomic read-modify-write, seeding from the default on first use",
          "t = shared_table()\n"
          "print(shared_table_update(t, \"n\", 10, fn(c) return c + 1 end))\n"
          "print(shared_table_update(t, \"n\", 10, fn(c) return c + 1 end))\n"
          "print(shared_table_get(t, \"n\"))\n",
          "11\n12\n12\n" },

        { "shared_table: a nested handle retrieved back is the SAME underlying table, not a copy",
          "inner = shared_table()\n"
          "outer = shared_table()\n"
          "shared_table_set(outer, \"inner\", inner)\n"
          "got_inner = shared_table_get(outer, \"inner\")\n"
          "shared_table_set(got_inner, \"x\", 99)\n"
          "print(shared_table_get(inner, \"x\"))\n",
          "99\n" },
    };

    std::vector<SuffixTest> workerTests = {

        { "worker_spawn/worker_join: runs a real task in its own Interpreter and returns a wrapped result",
          R"EMBR(write_file("test_parallel_worker.embr", "import \"parallel\"\nimport \"embrlib\"\n\nfn double_it(x)\n    return x * 2\nend\n\nfn maybe_raise(x)\n    if x < 0\n        error(\"negative input: \" + str(x))\n    end\n    return x\nend\n\nfn increment_counter(x, counter)\n    shared_table_update(counter, \"count\", 0, fn(c) return c + 1 end)\n    return x\nend\n")
)EMBR"
          "in_ch = channel()\n"
          "out_ch = channel()\n"
          "w = worker_spawn(\"test_parallel_worker.embr\", \"double_it\", in_ch, out_ch)\n"
          "channel_send(in_ch, 21)\n"
          "r = channel_recv(out_ch)\n"
          "channel_close(in_ch)\n"
          "worker_join(w)\n"
          "print(r[\"ok\"])\n"
          "print(r[\"value\"])\n",
          "1\n42\n" },

        { "worker_spawn: a task that raises inside the worker comes back as {ok:0,error:...} instead of crashing",
          "in_ch = channel()\n"
          "out_ch = channel()\n"
          "w = worker_spawn(\"test_parallel_worker.embr\", \"maybe_raise\", in_ch, out_ch)\n"
          "channel_send(in_ch, -5)\n"
          "r = channel_recv(out_ch)\n"
          "channel_close(in_ch)\n"
          "worker_join(w)\n"
          "print(r[\"ok\"])\n"
          "print(r[\"error\"][\"message\"])\n",
          "0\nnegative input: -5\n" },

        { "worker_spawn: extra_args are passed to every call, processes several tasks in order on one worker",
          "in_ch = channel()\n"
          "out_ch = channel()\n"
          "counter = shared_table()\n"
          "w = worker_spawn(\"test_parallel_worker.embr\", \"increment_counter\", in_ch, out_ch, [counter])\n"
          "channel_send(in_ch, 1)\n"
          "channel_send(in_ch, 2)\n"
          "channel_send(in_ch, 3)\n"
          "channel_close(in_ch)\n"
          "a = channel_recv(out_ch)[\"value\"]\n"
          "b = channel_recv(out_ch)[\"value\"]\n"
          "c = channel_recv(out_ch)[\"value\"]\n"
          "worker_join(w)\n"
          "print(a)\nprint(b)\nprint(c)\n"
          "print(shared_table_get(counter, \"count\"))\n",
          "1\n2\n3\n3\n" },

        { "shared_table_update under real concurrency: 4 workers x 50 increments each land all 200, none lost",
          "counter = shared_table()\n"
          "shared_table_set(counter, \"count\", 0)\n"
          "n_workers = 4\n"
          "per_worker = 50\n"
          "workers = []\n"
          "in_chs = []\n"
          "out_ch = channel()\n"
          "i = 0\n"
          "while i < n_workers\n"
          "  in_ch = channel()\n"
          "  w = worker_spawn(\"test_parallel_worker.embr\", \"increment_counter\", in_ch, out_ch, [counter])\n"
          "  workers = push(workers, w)\n"
          "  in_chs = push(in_chs, in_ch)\n"
          "  i = i + 1\n"
          "end\n"
          "i = 0\n"
          "while i < n_workers\n"
          "  j = 0\n"
          "  while j < per_worker\n"
          "    channel_send(in_chs[i], j)\n"
          "    j = j + 1\n"
          "  end\n"
          "  channel_close(in_chs[i])\n"
          "  i = i + 1\n"
          "end\n"
          "total = n_workers * per_worker\n"
          "got = 0\n"
          "while got < total\n"
          "  channel_recv(out_ch)\n"
          "  got = got + 1\n"
          "end\n"
          "i = 0\n"
          "while i < n_workers\n"
          "  worker_join(workers[i])\n"
          "  i = i + 1\n"
          "end\n"
          "print(shared_table_get(counter, \"count\"))\n",
          "200\n" },

        // the same boxes are copied and dropped on two threads at once. their counts must be atomic by then (a value is marked
        // shared when it is sent). run under ThreadSanitizer this fails with a data race if the marking is left out
        { "channel_send: one value kept by the sender and copied by the worker at the same time keeps its count right",
          R"EMBR(write_file("test_parallel_rc_worker.embr", "import \"embrlib\"\n\nfn churn(x)\n    i = 0\n    while i < 300\n        y = [x, x]\n        i = i + 1\n    end\n    return len(x)\nend\n")
)EMBR"
          "data = [\"alpha\", [\"beta\", \"gamma\"]]\n"
          "in_ch = channel()\n"
          "out_ch = channel()\n"
          "w = worker_spawn(\"test_parallel_rc_worker.embr\", \"churn\", in_ch, out_ch)\n"
          "n = 300\n"
          "i = 0\n"
          "while i < n\n"
          "  channel_send(in_ch, data)\n"
          "  j = 0\n"
          "  while j < 50\n"
          "    z = [data, data]\n"
          "    j = j + 1\n"
          "  end\n"
          "  i = i + 1\n"
          "end\n"
          "channel_close(in_ch)\n"
          "total = 0\n"
          "i = 0\n"
          "while i < n\n"
          "  r = channel_recv(out_ch)\n"
          "  total = total + r[\"value\"]\n"
          "  i = i + 1\n"
          "end\n"
          "worker_join(w)\n"
          "print(total)\n"
          "print(data[1][1])\n",
          "600\ngamma\n" },
    };

    const std::vector<std::string> plugins = {"parallel", "embrlib", "table"};

    int passed = 0, failed = 0;
    std::cout << "\n\033[1m parallel plugin test suite \033[0m\n";
    for (const auto& t : tests) {
        auto interp = embr_test::freshInterpreter(plugins);
        auto r = embr_test::runTest(t, *interp);
        embr_test::printResult(r);
        r.passed ? ++passed : ++failed;
    }
    for (const auto& t : workerTests) {
        auto interp = embr_test::freshInterpreter(plugins);
        bool ok = runSuffixTest(t, *interp);
        ok ? ++passed : ++failed;
    }

    std::cout << "\n"
              << "  " << passed << " passed, " << failed << " failed"
              << (failed ? "  \033[31mX\033[0m" : "  \033[32m√\033[0m") << "\n\n";
    return failed == 0 ? 0 : 1;
}
