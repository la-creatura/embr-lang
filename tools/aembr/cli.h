// cli.h
// what the command line of aembr says, parsed once in main.cpp and read by the commands.

#ifndef AEMBR_CLI_H
#define AEMBR_CLI_H

#include "emit.h"

#include <cstdint>
#include <string>
#include <vector>

namespace aembr {

struct Cli {
    std::string              cmd, in, out, signKey, exeDir;
    bool                     emitC = false, emitIr = false, asPlugin = false;
    BuildOptions             bo;
    uint64_t                 denyCaps = 0;
    std::vector<std::string> progArgs;
};

}  // namespace aembr

#endif
