// natives.h
// the natives of embrlib, str and embrmath that aembr compiles, as a table: what a script calls, the types it takes, what it gives, and the C
// function in rt/rt_nat.c (or libm) that does the work.
//
// usage
//   const NativeSpec* s = findNative("str_split", {ir::Ty::Str, ir::Ty::Str});     // null when no overload takes these types
//
// good to know
//   - a native that takes either an int or a float in the plugin (an embrmath one) is an overload per type here, as it is in the plugin
//   - the kinds and messages of the errors are the plugins' own, they live in the C functions
//   - what is not here is refused with a message: the natives that call a function (filter, sort_cmp, ...), str_format, the random numbers

#ifndef AEMBR_NATIVES_H
#define AEMBR_NATIVES_H

#include "ir.h"

#include <string>
#include <vector>

namespace aembr {

// params: one letter per parameter. i int, f float, n number (an int is made a float), s str, I F S an arr of int, float, str, A any array,
//         M any map, N an array of arrays, O an array of maps, x an element of the first parameter's container, = the type of the first parameter
// ret: i f s, I F S an array, = the type of the first parameter, e the element type of the first parameter, v nothing
// mutates: the first argument is `&name` and the native changes the array or map in it
struct NativeSpec {
    const char* plugin;
    const char* name;
    const char* params;
    char        ret;
    const char* cname;
    bool        raises;
    bool        mutates;
};

inline const std::vector<NativeSpec>& nativeTable() {
    static const std::vector<NativeSpec> t = {
        // ---- embrlib
        {"embrlib", "num",         "s",   'f', "rt_num_str",           true,  false},
        {"embrlib", "slice",       "si",  's', "rt_slice_str_from",    false, false},
        {"embrlib", "slice",       "sii", 's', "rt_slice_str",         false, false},
        {"embrlib", "slice",       "Ai",  '=', "rt_slice_arr_from",    false, false},
        {"embrlib", "slice",       "Aii", '=', "rt_slice_arr",         false, false},
        {"embrlib", "index_of",    "Ii",  'i', "rt_index_of_i64",      false, false},
        {"embrlib", "index_of",    "Ff",  'i', "rt_index_of_f64",      false, false},
        {"embrlib", "index_of",    "Ss",  'i', "rt_index_of_str",      false, false},
        {"embrlib", "sum",         "I",   'i', "rt_sum_i64",           false, false},
        {"embrlib", "sum",         "F",   'f', "rt_sum_f64",           false, false},
        {"embrlib", "min",         "I",   'i', "rt_min_i64",           true,  false},
        {"embrlib", "min",         "F",   'f', "rt_min_f64",           true,  false},
        {"embrlib", "max",         "I",   'i', "rt_max_i64",           true,  false},
        {"embrlib", "max",         "F",   'f', "rt_max_f64",           true,  false},
        {"embrlib", "sort",        "I",   'I', "rt_sort_asc",          false, false},
        {"embrlib", "sort",        "F",   'F', "rt_sort_asc",          false, false},
        {"embrlib", "sort",        "Ii",  'I', "rt_sort_arr",          false, false},
        {"embrlib", "sort",        "Fi",  'F', "rt_sort_arr",          false, false},
        {"embrlib", "reverse",     "s",   's', "rt_reverse_str",       false, false},
        {"embrlib", "reverse",     "A",   '=', "rt_reverse_arr",       false, false},
        {"embrlib", "concat",      "A=",  '=', "rt_concat_arr",        false, false},
        {"embrlib", "pop",         "A",   '=', "rt_pop_arr",           true,  false},
        {"embrlib", "has",         "Ai",  'i', "rt_has_arr",           false, false},
        {"embrlib", "has",         "Ms",  'i', "rt_map_has",           false, false},
        {"embrlib", "remove_last", "I",   'i', "rt_remove_last_i64",   true,  true},
        {"embrlib", "remove_last", "F",   'f', "rt_remove_last_f64",   true,  true},
        {"embrlib", "remove_last", "S",   's', "rt_remove_last_str",   true,  true},
        {"embrlib", "remove_at",   "Ii",  'i', "rt_remove_at_i64",     true,  true},
        {"embrlib", "remove_at",   "Fi",  'f', "rt_remove_at_f64",     true,  true},
        {"embrlib", "remove_at",   "Si",  's', "rt_remove_at_str",     true,  true},
        {"embrlib", "insert",      "Iii", 'i', "rt_insert_i64",        true,  true},
        {"embrlib", "insert",      "Fif", 'i', "rt_insert_f64",        true,  true},
        {"embrlib", "insert",      "Sis", 'i', "rt_insert_str",        true,  true},
        {"embrlib", "assert",      "i",   'v', "rt_assert_i64",        true,  false},
        {"embrlib", "assert",      "f",   'v', "rt_assert_f64",        true,  false},
        {"embrlib", "assert",      "is",  'v', "rt_assert_msg_i64",    true,  false},
        {"embrlib", "assert",      "fs",  'v', "rt_assert_msg_f64",    true,  false},
        {"embrlib", "take",        "Ai",  '=', "rt_take_arr",          false, false},
        {"embrlib", "chunk",       "Ai",  'c', "rt_chunk_arr",         true,  false},
        {"embrlib", "unique",      "I",   '=', "rt_unique_i64",        false, false},
        {"embrlib", "unique",      "F",   '=', "rt_unique_f64",        false, false},
        {"embrlib", "unique",      "S",   '=', "rt_unique_str",        false, false},
        {"embrlib", "zip",         "A=",  'c', "rt_zip2_arr",          false, false},
        {"embrlib", "flatten",     "N",   'e', "rt_flatten_arr",       false, false},
        {"embrlib", "enumerate",   "I",   'c', "rt_enumerate_arr",     false, false},
        {"embrlib", "enumerate",   "Ii",  'c', "rt_enumerate_from",    false, false},
        {"embrlib", "range",       "ii",  'I', "rt_range_excl",        false, false},
        {"embrlib", "map_merge",   "M=",  '=', "rt_map_merge",         false, false},
        {"embrlib", "map_pick",    "MS",  '=', "rt_map_pick",          false, false},
        {"embrlib", "map_omit",    "MS",  '=', "rt_map_omit",          false, false},
        {"embrlib", "remove_last", "N",   'e', "rt_remove_last_arr",   true,  true},
        {"embrlib", "remove_last", "O",   'e', "rt_remove_last_map",   true,  true},
        {"embrlib", "remove_at",   "Ni",  'e', "rt_remove_at_arr",     true,  true},
        {"embrlib", "remove_at",   "Oi",  'e', "rt_remove_at_map",     true,  true},
        {"embrlib", "insert",      "Nix", 'i', "rt_insert_arr",        true,  true},
        {"embrlib", "insert",      "Oix", 'i', "rt_insert_map",        true,  true},
        {"embrlib", "remove_key",  "Ms",  'i', "rt_remove_key",        false, true},
        // ---- str
        // ---- embrmath (the C library's own functions where they are plain)
    };
    return t;
}

// the plugin a name belongs to, or null (a name aembr does not know as a native)
inline const char* nativePlugin(const std::string& name) {
    for (const NativeSpec& s : nativeTable()) if (name == s.name) return s.plugin;
    return nullptr;
}

// does an argument of type `have` fit the parameter letter `p` (`first` is the type of the first argument, for '=')
inline bool paramFits(char p, ir::Ty have, ir::Ty first) {
    switch (p) {
    case 'i': return have == ir::Ty::Int;
    case 'f': return have == ir::Ty::Float;
    case 'n': return have == ir::Ty::Int || have == ir::Ty::Float;
    case 's': return have == ir::Ty::Str;
    case 'I': return have == ir::Ty::ArrInt;
    case 'F': return have == ir::Ty::ArrFloat;
    case 'S': return have == ir::Ty::ArrStr;
    case 'A': return ir::isArr(have);
    case 'M': return ir::isMap(have);
    case 'N': return ir::isArr(have) && ir::isArr(ir::elemOf(have));
    case 'O': return ir::isArr(have) && ir::isMap(ir::elemOf(have));
    case 'x': return (ir::isArr(first) || ir::isMap(first)) && have == ir::elemOf(first);
    case '=': return have == first;
    }
    return false;
}

inline const NativeSpec* findNative(const std::string& name, const std::vector<ir::Ty>& args) {
    for (const NativeSpec& s : nativeTable()) {
        if (name != s.name || args.size() != std::string(s.params).size()) continue;
        bool ok = true;
        for (size_t i = 0; i < args.size() && ok; ++i) ok = paramFits(s.params[i], args[i], args.empty() ? ir::Ty::Void : args[0]);
        if (ok) return &s;
    }
    return nullptr;
}

// the type a native gives for these arguments
inline ir::Ty nativeResult(const NativeSpec& s, const std::vector<ir::Ty>& args) {
    switch (s.ret) {
    case 'i': return ir::Ty::Int;
    case 'f': return ir::Ty::Float;
    case 's': return ir::Ty::Str;
    case 'I': return ir::Ty::ArrInt;
    case 'F': return ir::Ty::ArrFloat;
    case 'S': return ir::Ty::ArrStr;
    case '=': return args.empty() ? ir::Ty::Void : args[0];
    case 'c': return args.empty() ? ir::Ty::Void : ir::arrOf(args[0]);
    case 'e': return args.empty() ? ir::Ty::Void : ir::elemOf(args[0]);
    default:  return ir::Ty::Void;
    }
}

}  // namespace aembr

#endif
