// cp_undeclared.cpp
// a C++ plugin that does not describe itself (no EMBR_PLUGIN_INFO), like one built before there was a description.
// it loads normally, but a host that denies any capability refuses it, since it cannot check what it needs.
// used by sandbox_smoke.sh
#include <embr/embr.h>

using namespace embr;

EMBR_PLUGIN {
    interp->bind("cp_undeclared_ok", [](const std::vector<Value>&) -> Value { return Value(int64_t(1)); });
}
