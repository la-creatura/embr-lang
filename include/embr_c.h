/* embr_c.h
 * a plain C interface to embr, for programs in C or any language with a C FFI (python ctypes, rust, go, ...).
 * link with libembr_c (static) or libembr_c_shared. nothing C++ crosses this boundary: no exceptions, no std::string.
 *
 * usage
 *   embr_state* e = embr_new();
 *   embr_set_int(e, "width", 80);
 *   if (embr_run_string(e, "area = width * 2", "config") != 0) puts(embr_last_error(e));
 *   int64_t area; embr_get_int(e, "area", &area);
 *   embr_free(e);
 *
 * good to know
 *   - a string you get back (embr_last_error, embr_get_string, embr_arg_string) belongs to embr. it stays valid until
 *     the next call on the same state (for an argument: until the callback returns). copy it to keep it
 *   - one state is one interpreter. use a state from one thread at a time
 *   - plugins (import "json") are found next to the program, in EMBR_PATH and the usual places, see the wiki, module-system
 */
#ifndef EMBR_C_H
#define EMBR_C_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) && defined(EMBR_C_SHARED)
#  ifdef EMBR_C_BUILDING
#    define EMBR_C_API __declspec(dllexport)
#  else
#    define EMBR_C_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__)
#  define EMBR_C_API __attribute__((visibility("default")))
#else
#  define EMBR_C_API
#endif

typedef struct embr_state embr_state;   /* an interpreter */
typedef struct embr_call  embr_call;    /* one call of a function the host registered */

/* what a value is, for embr_get_type and embr_arg_type */
typedef enum {
    EMBR_TYPE_NONE   = 0,   /* no such global, or no such argument */
    EMBR_TYPE_INT    = 1,
    EMBR_TYPE_FLOAT  = 2,
    EMBR_TYPE_STRING = 3,
    EMBR_TYPE_OTHER  = 4    /* an array, map, function or pointer: not readable through this interface */
} embr_type;

EMBR_C_API embr_state* embr_new(void);
EMBR_C_API void        embr_free(embr_state* e);

/* run code. 0 when it ran to the end, 1 on an error (embr_last_error says what). names it makes stay for the next run */
EMBR_C_API int         embr_run_string(embr_state* e, const char* source, const char* filename);
EMBR_C_API int         embr_run_file(embr_state* e, const char* path);
EMBR_C_API const char* embr_last_error(embr_state* e);   /* "" when the last call had no error */

/* globals: get returns 0 and fills *out when the global exists and has that type, 1 otherwise */
EMBR_C_API embr_type   embr_get_type(embr_state* e, const char* name);
EMBR_C_API int         embr_get_int(embr_state* e, const char* name, int64_t* out);
EMBR_C_API int         embr_get_float(embr_state* e, const char* name, double* out);   /* an int is converted */
EMBR_C_API int         embr_get_string(embr_state* e, const char* name, const char** out);
EMBR_C_API void        embr_set_int(embr_state* e, const char* name, int64_t value);
EMBR_C_API void        embr_set_float(embr_state* e, const char* name, double value);
EMBR_C_API void        embr_set_string(embr_state* e, const char* name, const char* value);

/* a function the host gives to scripts. return 0 after an embr_return_* (or none: the result is 0), or nonzero after
   embr_fail to raise an error the script can catch */
typedef int (*embr_fn)(embr_call* call, void* userdata);
EMBR_C_API void        embr_register(embr_state* e, const char* name, embr_fn fn, void* userdata);

EMBR_C_API int         embr_arg_count(embr_call* c);
EMBR_C_API embr_type   embr_arg_type(embr_call* c, int i);
EMBR_C_API int64_t     embr_arg_int(embr_call* c, int i);          /* 0 when it isn't an int */
EMBR_C_API double      embr_arg_float(embr_call* c, int i);        /* an int is converted, 0 when it isn't a number */
EMBR_C_API const char* embr_arg_string(embr_call* c, int i);       /* "" when it isn't a string */
EMBR_C_API void        embr_return_int(embr_call* c, int64_t value);
EMBR_C_API void        embr_return_float(embr_call* c, double value);
EMBR_C_API void        embr_return_string(embr_call* c, const char* value);
EMBR_C_API int         embr_fail(embr_call* c, const char* message);   /* returns 1, so `return embr_fail(c, "...");` */

/* sandboxing, before running untrusted code: only these plugins may be imported (a bare name, looked for next to the
   program and in EMBR_PATH only), and embrlib's file functions can be turned off. count 0 allows no plugin at all */
EMBR_C_API void        embr_allow_plugins(embr_state* e, const char* const* names, int count);
EMBR_C_API void        embr_allow_file_io(embr_state* e, int on);   /* off also refuses plugins that need the filesystem */
/* refuse plugins that need these capabilities: a comma list of filesystem, network, process, env, threads, native
   ("network,process"). plugins then load only from the places the host controls, like embr_allow_plugins. returns 0, or 1
   when a word in the list is not a capability (nothing is changed then) */
EMBR_C_API int         embr_deny_capabilities(embr_state* e, const char* list);

#ifdef __cplusplus
}
#endif

#endif /* EMBR_C_H */
