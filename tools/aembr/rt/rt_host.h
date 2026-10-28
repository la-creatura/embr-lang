/* rt_host.h
   what generated code uses to call a C plugin (include/embr_plugin.h), after rt.h and embr_plugin.h. see rt_host.c. */
#ifndef AEMBR_RT_HOST_H
#define AEMBR_RT_HOST_H

/* an argument: kind 0 int (i), 1 float (f), 2 str, 3 array, 4 map, 5 any (p points at the compiled value). an array or a map is copied all the way down */
typedef struct { int kind; int64_t i; double f; void* p; } RtPArg;
/* the result, in the field for the kind that was asked for: i, f, or p (a new EmbrStr, EmbrArr) */
typedef struct { int64_t i; double f; void* p; } RtPRes;

/* loads the library at `path` and checks its table. NULL after raising */
const EmbrPluginInfo* rt_plugin_open(RtCtx* cx, const char* path, const char* name, const char* scriptDir);
/* the entry point of a function, by name. NULL after raising */
EmbrFn rt_plugin_fn(RtCtx* cx, const EmbrPluginInfo* info, const char* name);
/* calls it. `want` is what the caller expects back: 0 int, 1 float, 2 str, 3 arr<int>, 4 arr<float>, 5 arr<str>, 6 nothing (the result is dropped).
   raises with the name of the function as the kind, or the kind the plugin chose */
void rt_plugin_call(RtCtx* cx, EmbrFn fn, const char* fname, const char* scriptDir, const RtPArg* args, int n, int want, RtPRes* res, int line);

#endif
