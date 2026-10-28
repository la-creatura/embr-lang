// main.cpp
// the aembr command line: compile an embr file to a native program or a C plugin, package it, and read what made a file.
//
// usage
//   aembr check FILE [--plugin]            say what aembr cannot compile, change nothing
//   aembr build FILE [-o OUT] [--emit-c] [--emit-ir] [--cc=CC] [--deny-caps=LIST] [-O0|-O1|-O2|-O3] [--infer]
//   aembr run FILE [-- args]               build into a temp folder, run, remove
//   aembr plugin FILE [-o OUT.so] [--emit-c] [--cc=CC]    build the functions of FILE as a C plugin that `import "name"` loads
//   aembr package FILE [-o OUT.pmbr] [--sign NAME.key]    a plugin with its source and build record, as a package (.pmbr)
//   aembr info FILE                        what made a program, a plugin or a package
//   aembr verify PKG.pmbr                  check a package against its own source
//
// good to know
//   - exit codes: 0 fine, 1 the file was refused or has errors, 2 wrong usage or the build itself failed. verify: 0 verified, 1 a mismatch,
//     2 could not verify
//   - a plugin exports every top level function whose name does not start with __, takes its name from the output file, and its version,
//     description and capabilities from the file's `__module__` (the same literal `embr module` reads)
//   - -lNAME, -LDIR (linked after the runtime), -IDIR and -DNAME go to the C compiler, for C text that uses a library or a header
//   - --infer lets a closed file leave parameter and return types unwritten: the type checker's answer is used when it is one type, else the
//     function is refused. check, build, run, plugin and package all take it
//   - a source file may end in .aembr or .embr, both are read the same way (.aembr says the file is written for aembr's subset)
//   - --fatal-overflow (programs): a stack overflow prints its error and stops at once instead of returning through every frame. nothing can catch it and there is no trace,
//     but a recursive function that can raise nothing else needs no error check after its calls (fib: half the instructions)
//   - --stats links a runtime that counts (retains, allocations, error checks, ...); `run --stats` prints the counts to stderr when the program ends,
//     and a program or plugin built with it prints them if AEMBR_STATS is set in its environment
//   - --emit-c and --emit-ir print to stdout and build nothing

#include "cli.h"
#include "packaging.h"
#include "pipeline.h"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace {

int usage() {
    std::cerr << "usage: aembr check FILE [--plugin]\n"
                 "       aembr build FILE [-o OUT] [--emit-c] [--emit-ir] [--cc=CC] [--deny-caps=LIST] [-O0|-O1|-O2|-O3] [--infer] [--stats] [--fatal-overflow] [-lNAME] [-LDIR] [-IDIR] [-DNAME]\n"
                 "       aembr run FILE [-- args]\n"
                 "       aembr plugin FILE [-o OUT.so] [--emit-c] [--cc=CC] [-lNAME] [-LDIR] [-IDIR] [-DNAME]\n"
                 "       aembr package FILE [-o OUT.pmbr] [--sign NAME.key]\n"
                 "       aembr info FILE\n"
                 "       aembr verify PKG.pmbr\n";
    return 2;
}

std::string exeDir(const char* argv0) {
    char buf[4096];
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
    std::string p = n > 0 ? std::string(buf, (size_t)n) : std::string(argv0);
    const size_t slash = p.rfind('/');
    return slash == std::string::npos ? "." : p.substr(0, slash);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    aembr::Cli c;
    c.cmd = argv[1];
    c.exeDir = exeDir(argv[0]);
    if (c.cmd == "--version") { std::cout << "aembr " << aembr::kAembrVersion << "\n"; return 0; }
    static const char* const known[] = {"check", "build", "run", "plugin", "package", "info", "verify"};
    bool ok = false;
    for (const char* k : known) ok = ok || c.cmd == k;
    if (!ok) return usage();
    c.asPlugin = c.cmd == "plugin";

    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--") { for (++i; i < argc; ++i) c.progArgs.push_back(argv[i]); break; }
        if (a == "-o" && i + 1 < argc) c.out = argv[++i];
        else if (a == "--emit-c") c.emitC = true;
        else if (a == "--infer") c.bo.infer = true;
        else if (a == "--stats") c.bo.stats = true;
        else if (a == "--fatal-overflow") c.bo.fatalOverflow = true;
        else if (a.size() > 2 && (a.rfind("-l", 0) == 0 || a.rfind("-L", 0) == 0)) c.bo.libs.push_back(a);
        else if (a.size() > 2 && (a.rfind("-I", 0) == 0 || a.rfind("-D", 0) == 0)) c.bo.cflags.push_back(a);
        else if (a == "--emit-ir") c.emitIr = true;
        else if (a == "--plugin" && c.cmd == "check") c.asPlugin = true;
        else if (a == "--sign" && i + 1 < argc) c.signKey = argv[++i];
        else if (a.rfind("--cc=", 0) == 0) c.bo.cc = a.substr(5);
        else if (a.rfind("--deny-caps=", 0) == 0) {
            std::string bad;
            if (!embr::capMask(a.substr(12), c.denyCaps, bad)) { std::cerr << "aembr: --deny-caps: " << bad << " is not a capability (filesystem, network, process, env, threads, native)\n"; return 2; }
        }
        else if (a.size() == 3 && a.rfind("-O", 0) == 0 && a[2] >= '0' && a[2] <= '3') c.bo.opt = a[2] - '0';
        else if (!a.empty() && a[0] != '-' && c.in.empty()) c.in = a;
        else { std::cerr << "aembr: unknown argument " << a << "\n"; return usage(); }
    }
    if (c.in.empty()) return usage();

    if (c.cmd == "package") return aembr::packageCommand(c);
    if (c.cmd == "info") return aembr::infoCommand(c);
    if (c.cmd == "verify") return aembr::verifyCommand(c);

    aembr::CompileRequest rq;
    rq.srcPath = c.in;
    rq.plugin = c.asPlugin;
    rq.bo = c.bo;
    rq.denyCaps = c.denyCaps;
    rq.exeDir = c.exeDir;
    if (c.asPlugin) {
        if (c.out.empty()) {
            std::filesystem::path p(c.in);
            c.out = (p.parent_path() / (p.stem().string() + ".so")).string();
        }
        rq.pluginName = std::filesystem::path(c.out).stem().string();
    }
    rq.stage = c.cmd == "check" ? aembr::Stage::Check : c.emitIr ? aembr::Stage::Ir : aembr::Stage::C;
    aembr::Compiled comp;
    std::string err;
    if (int rc = aembr::compileSource(rq, comp, err)) { std::cerr << err; return rc; }
    if (c.cmd == "check") return 0;
    if (c.emitIr) { std::cout << aembr::ir::printIr(comp.mod); return 0; }
    if (c.emitC) { std::cout << comp.c; return 0; }

    std::string tmpDir;
    if (c.cmd == "run") {
        const char* t = std::getenv("TMPDIR");
        std::string tpl = std::string(t && *t ? t : "/tmp") + "/aembr-XXXXXX";
        std::vector<char> buf(tpl.begin(), tpl.end()); buf.push_back(0);
        if (!::mkdtemp(buf.data())) { std::cerr << "aembr: cannot make a temp folder\n"; return 2; }
        tmpDir = buf.data();
        c.out = tmpDir + "/prog";
    } else if (c.out.empty()) {
        c.out = c.in;
        const size_t dot = c.out.rfind('.');
        if (dot != std::string::npos && dot > c.out.rfind('/') + 1) c.out = c.out.substr(0, dot);
        else c.out += ".out";
    }

    c.bo.shared = c.asPlugin;
    if (c.bo.stats && c.cmd == "run") ::setenv("AEMBR_STATS", "1", 1);
    int rc = 0;
    if (!aembr::buildExecutable(comp.c, c.out, c.bo, c.exeDir, err)) { std::cerr << err; rc = 2; }
    else if (c.cmd == "run") { rc = aembr::runProgram(c.out, c.progArgs); if (rc < 0) { std::cerr << "aembr: cannot run the program\n"; rc = 2; } }
    if (!tmpDir.empty()) { std::remove(c.out.c_str()); ::rmdir(tmpDir.c_str()); }
    return rc;
}
