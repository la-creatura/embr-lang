// lower.h
// turns a checked embr program into the intermediate form, and refuses what aembr does not compile yet.
//
// usage
//   ir::Module m = aembr::lower(front);     // throws aembr::Refusal

#ifndef AEMBR_LOWER_H
#define AEMBR_LOWER_H

#include "front.h"
#include "ir.h"

namespace aembr {

struct LowerOptions {
    bool        plugin = false;     // the top level names are globals that the functions only read, see lower.cpp
    bool        fatalOverflow = false;   // --fatal-overflow: a stack overflow stops the program at once (see ir::Module::fatalOverflow)
    bool        infer = false;      // a parameter or result that is not written takes the type the checker found (--infer)
    uint64_t    denyCaps = 0;       // EMBR_CAP_* bits: importing a plugin that needs one of these is refused
};
ir::Module lower(const Front& front, const LowerOptions& opts = LowerOptions());

// `file:line:col: outside the compiled subset: reason`
std::string formatRefusal(const std::string& file, const Refusal& r);

}  // namespace aembr

#endif
