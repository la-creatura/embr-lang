/* cp_needs_fs.c
 * a C plugin that says it needs the filesystem. a sandbox that turns file access off must refuse it.
 * used by sandbox_smoke.sh. */
#include <embr_plugin.h>

static int cp_ok(EmbrCtx* ctx, const EmbrValue* args, size_t n, EmbrValue* out) { (void)args; (void)n; return embrp_set_int(ctx, out, 1); }
static const EmbrFunc funcs[] = { { "cp_ok", "cp_ok() -> int", cp_ok, NULL, NULL, 0, EMBR_T_INT, 0, 0 } };
static const EmbrPluginInfo info = { EMBR_PLUGIN_ABI_VERSION, sizeof(EmbrPluginInfo), "cp_needs_fs", "0", "asks for files", EMBR_CAP_FILESYSTEM, funcs, 1, NULL, 0, NULL, NULL };
EMBR_PLUGIN_EXPORT const EmbrPluginInfo* embr_plugin_info(void) { return &info; }
