// pipeline.h
// the steps every aembr command shares: read the file, lower it, work out what it needs, write the record and the C text.
//
// usage
//   aembr::Compiled c;
//   std::string err;
//   int rc = aembr::compileSource(req, c, err);       // 0 fine, 1 refused or wrong (err has the messages), 2 the build setup is wrong
//
// good to know
//   - a plugin is written so that the same source gives the same bytes anywhere: the file is named by its name only (not its folder), and
//     the plugins it calls are found by name when it runs
//   - the record (record.h) is made here, so it can be put inside the C text

#ifndef AEMBR_PIPELINE_H
#define AEMBR_PIPELINE_H

#include "emit.h"
#include "ir.h"
#include "record.h"

#include <cstdint>
#include <string>

namespace aembr {

inline constexpr const char* kAembrVersion = "0.1.0";

enum class Stage { Check, Ir, C };

struct CompileRequest {
    std::string  srcPath;
    std::string  srcText;         // when not empty, this is the source and srcPath only names it
    bool         plugin = false;
    std::string  pluginName;      // a plugin's name (the file it is built to)
    BuildOptions bo;
    uint64_t     denyCaps = 0;
    std::string  exeDir;
    Stage        stage = Stage::C;
};

struct Compiled {
    ir::Module   mod;
    std::string  c;               // the C text (Stage::C)
    BuildRecord  rec;
    PluginMeta   meta;            // a plugin
    // the script modules the file imports: the path relative to the file's folder (what is hashed and packed) and where it was read
    std::vector<std::pair<std::string, std::string>> modules;
};

int compileSource(const CompileRequest& req, Compiled& out, std::string& err);

}  // namespace aembr

#endif
