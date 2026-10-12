// cp_badbuild.cpp
// a C++ plugin whose description says it was built with another toolchain. embr must refuse it with a message, and
// must not run its embr_register. used by sandbox_smoke.sh and plugin_info_smoke.sh
#include <embr/embr.h>

#include <cstdio>

using namespace embr;

extern "C" EMBR_PLUGIN_EXPORT const EmbrPluginInfo* embr_plugin_info(void) {
    static const EmbrPluginInfo info = {
        EMBR_PLUGIN_ABI_VERSION, sizeof(EmbrPluginInfo), "cp_badbuild", "0", "built with another toolchain",
        EMBR_CAP_NONE, nullptr, 0, nullptr, 0, "cxx-abi 1; some other compiler 0.1; some other standard library", "cxx1-some-other-abi"
    };
    return &info;
}

EMBR_PLUGIN {
    (void)interp;
    std::fputs("REGISTER-RAN\n", stderr);
}
