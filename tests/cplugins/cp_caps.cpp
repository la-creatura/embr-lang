// cp_caps.cpp
// a C++ plugin that declares the network capability. a host that denies it must refuse the plugin BEFORE embr_register
// runs, so this one writes REGISTER-RAN to stderr when it does run. used by sandbox_smoke.sh
#include <embr/embr.h>

#include <cstdio>

using namespace embr;

EMBR_PLUGIN {
    std::fputs("REGISTER-RAN\n", stderr);
    interp->bind("cp_caps_ok", [](const std::vector<Value>&) -> Value { return Value(int64_t(1)); });
}

EMBR_PLUGIN_INFO("cp_caps", "0", "a test plugin that needs the network", EMBR_CAP_NETWORK, EMBR_CAP_FILESYSTEM,
    {"cp_caps_ok", "gives 1"});
