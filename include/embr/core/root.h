#ifndef EMBR_CPP_INCLUDED
#define EMBR_CPP_INCLUDED

// this is the amalgamation entry point not to be #include'd
// exists so the modular sources under core/ and backends/ have one place that declares the intended include order
// using the generated embr.h is preferable

// every backend is switched on by its own EMBR_WITH_<NAME> flag (CMake sets them from EMBR_BACKENDS). with none
// set, you get the VM, which is the default backend
#if !defined(EMBR_WITH_VM)
#  define EMBR_WITH_VM 1
#endif

//   EMBR_PLUGIN { interp->bind("myfn", ...); }
#define EMBR_PLUGIN \
    extern "C" EMBR_PLUGIN_EXPORT void embr_register(embr::Interpreter *interp)

#include "platform.h"
#include "diagnostics.h"
#include "types.h"
#include "fwd.h"
#include "token.h"
#include "value.h"
#include "lexer.h"
#include "ast.h"
#include "parser.h"
#include "registry.h"
#include "cplugin.h"
#include "typecheck.h"
#include "describe.h"
#include "module.h"
#include "modinfo.h"
#include "packageimport.h"
#include "../backends/vm.h"
#include "../backends/vm_disasm.h"
#include "../backends/vm_bytecode.h"
#include "invoke.h"

#endif // EMBR_CPP_INCLUDED
