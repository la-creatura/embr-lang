// cp_sameabi.cpp
// a C++ plugin that says it was built by another compiler but has the C++ ABI of this embr (what clang is to gcc on libstdc++).
// embr loads it with a warning. used by plugin_info_smoke.sh
#include <embr/embr.h>

using namespace embr;

extern "C" EMBR_PLUGIN_EXPORT const EmbrPluginInfo* embr_plugin_info(void) {
    static const EmbrFunc fn = {"cp_sameabi_ok", "gives 1", nullptr, nullptr, nullptr, 0, 0, 0, 0};
    static const EmbrPluginInfo info = {
        EMBR_PLUGIN_ABI_VERSION, sizeof(EmbrPluginInfo), "cp_sameabi", "0", "the C++ ABI of this embr from another toolchain",
        EMBR_CAP_NONE, &fn, 1, nullptr, 0, "cxx-abi 1; some other compiler 9.9; some other library", abiTag()
    };
    return &info;
}

EMBR_PLUGIN {
    interp->bind("cp_sameabi_ok", [](const std::vector<Value>&) -> Value { return Value(int64_t(1)); });
}
