// build.cpp
// runs the system C compiler on the text emit_c.cpp wrote, and runs a finished program.
//
// good to know
//   - the compiler is --cc, else $CC, else cc. it is started directly (no shell), so a path with spaces is fine
//   - the flags that matter for the meaning of the program are fixed: -fwrapv (int overflow wraps), -fno-strict-overflow

#include "emit.h"

#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <embr/embr.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <cstring>
#include <fstream>

extern char** environ;

namespace aembr {

namespace {

bool exists(const std::string& p) { struct stat st; return ::stat(p.c_str(), &st) == 0; }

int spawnWait(const std::vector<std::string>& argv, bool search) {
    std::vector<char*> a;
    for (const std::string& s : argv) a.push_back(const_cast<char*>(s.c_str()));
    a.push_back(nullptr);
    pid_t pid;
    int rc = search ? posix_spawnp(&pid, a[0], nullptr, nullptr, a.data(), environ)
                    : posix_spawn(&pid, a[0], nullptr, nullptr, a.data(), environ);
    if (rc != 0) return -1;
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

// runs a program and returns the first line of what it prints. "" when it cannot run
std::string firstLineOf(const std::vector<std::string>& argv) {
    int fds[2];
    if (::pipe(fds) != 0) return "";
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    std::vector<char*> a;
    for (const std::string& s : argv) a.push_back(const_cast<char*>(s.c_str()));
    a.push_back(nullptr);
    pid_t pid;
    const int rc = posix_spawnp(&pid, a[0], &fa, nullptr, a.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    ::close(fds[1]);
    std::string out;
    if (rc == 0) {
        char buf[512];
        ssize_t n;
        while ((n = ::read(fds[0], buf, sizeof buf)) > 0) out.append(buf, (size_t)n);
        int status = 0;
        waitpid(pid, &status, 0);
    }
    ::close(fds[0]);
    const size_t nl = out.find('\n');
    return nl == std::string::npos ? out : out.substr(0, nl);
}

// the flags that change what the compiled program does, in the order they are given
std::vector<std::string> baseFlags(const BuildOptions& o) {
    std::vector<std::string> f = {"-std=gnu11", "-O" + std::to_string(o.opt), "-fwrapv", "-fno-strict-overflow", "-fno-strict-aliasing"};
    // a plugin shows one symbol, embr_plugin_info, so two compiled plugins in one process never meet
    if (o.stats) f.push_back("-DAEMBR_STATS");
    // without the error checks after a call the C compiler could turn a recursion into a loop, and an endless one would never overflow
    if (o.fatalOverflow) f.push_back("-fno-optimize-sibling-calls");
    if (o.shared) for (const char* a : {"-shared", "-fPIC", "-fvisibility=hidden"}) f.push_back(a);
    return f;
}

}  // namespace

std::string compilerName(const BuildOptions& o) {
    if (!o.cc.empty()) return o.cc;
    const char* e = std::getenv("CC");
    return e && *e ? e : "cc";
}
std::string compilerVersion(const BuildOptions& o) { return firstLineOf({compilerName(o), "--version"}); }
std::string compileFlags(const BuildOptions& o) {
    std::string s;
    for (const std::string& f : baseFlags(o)) s += (s.empty() ? "" : " ") + f;
    if (o.infer) s += " --infer";
    if (o.fatalOverflow) s += " --fatal-overflow";
    for (const std::string& a : o.cflags) s += " " + a;
    for (const std::string& a : o.libs) s += " " + a;
    return s;
}
std::string runtimeDir(const BuildOptions& o, const std::string& exeDir) {
    if (!o.rtDir.empty()) return o.rtDir;
    const char* e = std::getenv("AEMBR_RT_DIR");
    if (e && *e) return e;
    // a build tree has aembr-rt next to the binary, an installed prefix has it in lib/embr
    const std::string beside = exeDir + "/aembr-rt";
    if (std::filesystem::is_directory(beside)) return beside;
    const std::string installed = exeDir + "/../lib/embr/aembr-rt";
    return std::filesystem::is_directory(installed) ? installed : beside;
}
std::string runtimeSha256(const std::string& rt) {
    std::string all;
    for (const char* f : {"libaembr_rt.a", "rt.h", "rt_plugin.h", "rt_host.h", "embr_plugin.h"}) {
        std::ifstream in(rt + "/" + f, std::ios::binary);
        if (!in) return "";
        all += std::string((std::istreambuf_iterator<char>(in)), {});
        all += '\0';
    }
    // the library written in aembr is part of what a build is made of
    std::vector<std::string> stdFiles;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(rt + "/std", ec)) stdFiles.push_back(e.path().filename().string());
    std::sort(stdFiles.begin(), stdFiles.end());
    for (const std::string& f : stdFiles) {
        std::ifstream in(rt + "/std/" + f, std::ios::binary);
        all += f;
        all += '\0';
        all += std::string((std::istreambuf_iterator<char>(in)), {});
        all += '\0';
    }
    unsigned char d[32];
    embr::sha256(all.data(), all.size(), d);
    return embr::hexOf(d, 32);
}

bool buildExecutable(const std::string& cText, const std::string& outPath, const BuildOptions& o, const std::string& exeDir, std::string& err) {
    const std::string rt = runtimeDir(o, exeDir);
    if (!exists(rt + "/rt.h") || !exists(rt + (o.stats ? "/libaembr_rt_stats.a" : "/libaembr_rt.a")) || !exists(rt + "/rt_host.h") || (o.shared && (!exists(rt + "/rt_plugin.h") || !exists(rt + "/embr_plugin.h")))) {
        err = "aembr: the runtime is not in " + rt + " (it needs rt.h, libaembr_rt.a and, for a plugin, rt_plugin.h and embr_plugin.h; build the aembr_rt target or set AEMBR_RT_DIR)\n";
        return false;
    }
    const std::string cc = compilerName(o);

    // a fixed file name in a private folder: the compiler records the name of the source in the program, so two builds into different
    // places must see the same one
    const char* t = std::getenv("TMPDIR");
    std::string tpl = std::string(t && *t ? t : "/tmp") + "/aembr-cc-XXXXXX";
    std::vector<char> tbuf(tpl.begin(), tpl.end()); tbuf.push_back(0);
    if (!::mkdtemp(tbuf.data())) { err = "aembr: cannot make a temp folder\n"; return false; }
    const std::string tmp = tbuf.data();
    const std::string cFile = tmp + "/embr_program.c";
    {
        std::ofstream f(cFile, std::ios::binary);
        if (!f) { err = "aembr: cannot write " + cFile + "\n"; ::rmdir(tmp.c_str()); return false; }
        f << cText;
    }
    std::vector<std::string> argv = {cc};
    for (const std::string& f : baseFlags(o)) argv.push_back(f);
    for (const std::string& a : o.cflags) argv.push_back(a);
    for (const std::string& a : {"-I" + rt, cFile, rt + (o.stats ? "/libaembr_rt_stats.a" : "/libaembr_rt.a")}) argv.push_back(a);
    for (const std::string& a : o.libs) argv.push_back(a);
#if defined(__APPLE__)
    // the linker writes a library's install name into it, by default the whole -o path. the file name alone keeps two builds equal
    if (o.shared) argv.push_back("-Wl,-install_name,@rpath/" + std::filesystem::path(outPath).filename().string());
#endif
    for (const std::string& a : {std::string("-lm"), std::string("-ldl"), std::string("-o"), outPath}) argv.push_back(a);
    const int rc = spawnWait(argv, true);
    std::remove(cFile.c_str());
    ::rmdir(tmp.c_str());
    if (rc == -1) { err = "aembr: cannot start the C compiler '" + cc + "' (use --cc=PATH or set CC)\n"; return false; }
    if (rc != 0) { err = "aembr: the C compiler '" + cc + "' failed (exit " + std::to_string(rc) + "). its messages are above: C text of your own is named by its file and line, anything else is a bug in aembr (--emit-c shows the whole C text)\n"; return false; }
    return true;
}

int runProgram(const std::string& path, const std::vector<std::string>& args) {
    std::vector<std::string> argv = {path};
    for (const std::string& a : args) argv.push_back(a);
    return spawnWait(argv, false);
}

}  // namespace aembr
