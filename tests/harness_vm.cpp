// harness_vm.cpp
// the one place the test suites run a script on the VM, so the VM is compiled here once and not in every suite.
//
// good to know
//   - a suite that uses the VM's own API (bytecode, typecheck, vm_internals, coroutine, core) still includes it and pays for it
//   - a new backend adds a function like this one and a case in runSourceAny() in harness.h

#include "harness.h"

namespace embr_test {

void runSourceVm(const std::string& src, embr::Interpreter& interp, const std::string& filename) {
    embr::vm::runSource(src, interp, filename);
}

} // namespace embr_test
