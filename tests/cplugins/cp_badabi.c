/* cp_badabi.c
 * a C plugin that says it was built for another plugin ABI. embr must refuse it with a clear message.
 * used by sandbox_smoke.sh. */
#include <embr_plugin.h>

static const EmbrPluginInfo info = { EMBR_PLUGIN_ABI_VERSION + 1u, sizeof(EmbrPluginInfo), "cp_badabi", "0", "wrong abi", EMBR_CAP_NONE, NULL, 0, NULL, 0, NULL, NULL };
EMBR_PLUGIN_EXPORT const EmbrPluginInfo* embr_plugin_info(void) { return &info; }
