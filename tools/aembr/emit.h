// emit.h
// the C text of a program (emit_c.cpp) and the call to the system C compiler (build.cpp).

#ifndef AEMBR_EMIT_H
#define AEMBR_EMIT_H

#include "ir.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace aembr {

// `record`: the text of the build record (record.h) to carry inside the output, or "" for none
std::string emitC(const ir::Module& m, const std::string& record = "");

// what a plugin says about itself (it goes into embr_plugin_info), found by build.cpp's caller from the file's `__module__`
struct PluginMeta {
    std::string name, version, description;
    uint64_t    caps = 0;                       // EMBR_CAP_* bits
    std::map<std::string, std::string> docs;    // function name -> its signature and the comment above it
};
// the C text of a plugin: the functions of the file as entry points of the C plugin interface (include/embr_plugin.h), every top level
// function whose name does not start with `__`. false: `err` says which function cannot be exported and why
bool emitPluginC(const ir::Module& m, const PluginMeta& meta, std::string& out, std::string& err, const std::string& record = "");

struct BuildOptions {
    std::string cc;          // empty: $CC, else cc
    std::string rtDir;       // where rt.h and libaembr_rt.a are; empty: next to the aembr binary, in aembr-rt/
    int         opt = 2;
    std::vector<std::string> cflags;   // -I, -D and the like, given before the C file (for C text that uses a header)
    std::vector<std::string> libs;     // -l and -L, given after the runtime library
    bool        fatalOverflow = false;   // --fatal-overflow: a stack overflow stops the program at once; there is nothing to catch and no trace (programs only)
    bool        infer = false;    // --infer: unwritten parameter and result types are worked out (recorded in the build record's flags)
    bool        stats = false;    // --stats: link the runtime that counts (libaembr_rt_stats.a); AEMBR_STATS=1 makes the program print the counts when it ends
    bool        shared = false;   // a plugin: a shared library, not a program
};

// writes `cText` to a temp file, compiles and links it into `outPath`. false: `err` says why
bool buildExecutable(const std::string& cText, const std::string& outPath, const BuildOptions& o, const std::string& exeDir, std::string& err);

// what a build is made with, for the record every output carries (record.h)
std::string compilerName(const BuildOptions& o);                          // --cc, else $CC, else cc
std::string compilerVersion(const BuildOptions& o);                       // the first line of `cc --version`, "" when it cannot run
std::string compileFlags(const BuildOptions& o);                          // the flags that change the program
std::string runtimeDir(const BuildOptions& o, const std::string& exeDir);
std::string runtimeSha256(const std::string& rtDir);                      // of the library and the headers, "" when one is missing

// runs a program, returns its exit code (or -1 when it could not start)
int runProgram(const std::string& path, const std::vector<std::string>& args);

}  // namespace aembr

#endif
