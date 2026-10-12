#ifndef EMBR_TEST_HARNESS_H
#define EMBR_TEST_HARNESS_H

// shared driver for embr's CTest-based unit test executables
//
// each plugin (and the core language) gets its own small executable built from a single .cpp that
// #includes this header
// calls runSuite() with its own list of Tests and the plugins it needs imported
// keeping each translation unit small means editing one plugin's tests only recompiles and relinks that one binary

#include <embr/embr.h>

// the C library to ffi_open() in tests that call real libc functions (strlen, qsort, abs, printf, ...)
#if defined(__APPLE__)
#  define EMBR_TEST_LIBC "libSystem.B.dylib"
#elif defined(_WIN32)
#  define EMBR_TEST_LIBC "msvcrt.dll"
#else
#  define EMBR_TEST_LIBC "libc.so.6"
#endif

// the EMBR_PATH entry separator
#ifdef _WIN32
#  define EMBR_TEST_PATHSEP ";"
#else
#  define EMBR_TEST_PATHSEP ":"
#endif

#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>
#include <memory>

#include <cstdlib>

namespace embr_test {

// these suites exercise language/stdlib behavior against one backend at a time. EMBR_TEST_BACKEND=<name> picks it
// (a name from embr::backendNames(), the default backend if unset). tests/CMakeLists.txt registers every suite once
// per backend (<name>_<backend>), so a new backend gets the whole matrix without touching the suites
inline std::string backendName() {
    const char* e = std::getenv("EMBR_TEST_BACKEND");
    return (e && *e) ? std::string(e) : embr::backendNames().front();
}

// compiles and runs one script on the VM. defined in harness_vm.cpp, once, so the suites that only run scripts don't each compile
// the whole VM (that was most of the time a test took to build)
void runSourceVm(const std::string& src, embr::Interpreter& interp, const std::string& filename);

inline void runSourceAny(const std::string& src, embr::Interpreter& interp,
                         const std::string& filename) {
    const std::string b = backendName();
    // backend switch: a new backend needs its own case here (and a function like runSourceVm in harness_vm.cpp)
    if (b == "vm") { runSourceVm(src, interp, filename); return; }
    std::cerr << "embr tests: unknown EMBR_TEST_BACKEND '" << b << "'\n";
    std::exit(2);
}

struct Test {
    std::string name;
    std::string code;
    std::string expectedOutput;  // exact stdout content
    std::string stdinData = {};  // piped to input() calls
};

struct Result {
    std::string name;
    bool        passed = false;
    std::string actual;
    std::string expected;
    std::string errorMsg;
};

inline Result runTest(const Test& t, embr::Interpreter& interp) {
    std::ostringstream capOut;
    std::istringstream fakeIn(t.stdinData);
    std::streambuf* origOut = std::cout.rdbuf(capOut.rdbuf());
    std::streambuf* origIn  = std::cin.rdbuf(fakeIn.rdbuf());

    Result r; r.name = t.name; r.expected = t.expectedOutput;
    try {
        runSourceAny(t.code, interp, t.name);
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

inline void printResult(const Result& r) {
    static const char* G = "\033[32m";
    if (r.passed) {
        std::cout << G << " [+]" << embr::X << " " << r.name << "\n";
    } else {
        std::cout << embr::R << " [-]" << embr::X << " " << r.name << "\n";
        if (!r.errorMsg.empty())
            std::cout << embr::Y << "        error: " << embr::X << r.errorMsg << "\n";
        if (!r.expected.empty() || !r.actual.empty()) {
            std::cout << "        expected: " << embr::valueRepr(embr::Value(r.expected)) << "\n";
            std::cout << "        actual:   " << embr::valueRepr(embr::Value(r.actual))   << "\n";
        }
    }
}

// a new Interpreter with the plugins in `plugins` already imported (their stderr banner is hidden). every test uses one of its
// own: a variable's type is the type of its first value, and a global keeps it for as long as the interpreter lives, so tests
// that reuse a name for different types can't share an interpreter
inline std::unique_ptr<embr::Interpreter> freshInterpreter(const std::vector<std::string>& plugins) {
    // a test may change EMBR_PATH (the module path tests do), and the plugins are found through it, so every interpreter
    // starts from the value it had when the suite began
    static const char* first = std::getenv("EMBR_PATH");
    static const std::string saved = first ? first : "";
    static const bool had = first != nullptr;
#ifdef _WIN32
    if (had) _putenv_s("EMBR_PATH", saved.c_str());
#else
    if (had) setenv("EMBR_PATH", saved.c_str(), 1);
#endif
    auto interp = std::make_unique<embr::Interpreter>();
    std::ostringstream quiet;
    std::streambuf* origErr = std::cerr.rdbuf(quiet.rdbuf());
    for (const auto& p : plugins) runSourceAny("import \"" + p + "\"", *interp, "");
    std::cerr.rdbuf(origErr);
    return interp;
}

// runs each test against a fresh Interpreter that has already import()ed each name in plugins
// individual Test::code strings should NOT re-import the same plugin
// (the "[runtime] loaded plugin: " banner goes to stderr, which this harness does not capture, so a
// re-import no longer pollutes the compared stdout -- it is just redundant work)
// prints a summary and returns a process exit code (0 = all passed) that CTest reads as pass/fail
// (std::cout is made unbuffered by runSuite, so a test that hangs still shows how far the suite got)
inline int runSuite(const std::string& suiteName,
                     const std::vector<Test>& tests,
                     const std::vector<std::string>& plugins = {}) {
#ifdef _WIN32
    embr::enableAnsi();
#endif
    std::cout << std::unitbuf;

    int passed = 0, failed = 0;
    std::cout << "\n\033[1m " << suiteName << " \033[0m\n";
    for (const auto& t : tests) {
        // every test gets an interpreter of its own. a variable's type is the type of its first value, and a global
        // keeps it for as long as the interpreter lives, so tests that reuse a name for different types can't share one
        auto interp = freshInterpreter(plugins);
        Result r = runTest(t, *interp);
        printResult(r);
        r.passed ? ++passed : ++failed;
    }

    std::cout << "\n"
              << "  " << passed << " passed, " << failed << " failed"
              << (failed ? "  \033[31mX\033[0m" : "  \033[32m√\033[0m") << "\n\n";
    return failed == 0 ? 0 : 1;
}

} // namespace embr_test

#endif // EMBR_TEST_HARNESS_H
