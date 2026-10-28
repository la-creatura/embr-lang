// packaging.h
// the commands that make and read packages (`.pmbr`) of compiled plugins: package, info and verify. the tooling around a package (signing,
// capabilities, trust) is embr's own, see wiki package-format; aembr adds the source and a record of how the library was built.

#ifndef AEMBR_PACKAGING_H
#define AEMBR_PACKAGING_H

#include "cli.h"

namespace aembr {

int packageCommand(const Cli& c);
int infoCommand(const Cli& c);
// 0 verified, 1 a mismatch (the claim or the bytes), 2 could not verify
int verifyCommand(const Cli& c);

}  // namespace aembr

#endif
