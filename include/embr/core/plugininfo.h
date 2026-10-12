// plugininfo.h
// what a plugin says about itself (name, version, capabilities, one doc line per function), for a plugin written in C++.
// a C plugin says the same in the table it returns (include/embr_plugin.h).
//
// usage, at the end of a plugin's source
//   EMBR_PLUGIN_INFO("regex", "1.0.0", "regular expressions on a linear-time engine", EMBR_CAP_NONE, EMBR_CAP_NONE,
//       {"re_test", "re_test(str, pattern, flags?) -> num, 1 if it matches anywhere, else 0"},
//       {"re_match", "re_match(str, pattern, flags?) -> arr, the whole match and then the groups"});
//
// good to know
//   - the macro exports embr_plugin_info(), which embr reads BEFORE it runs embr_register. so a plugin that asks for a
//     capability the host denies is refused before any of its code runs, and a plugin built with another compiler or
//     standard library gets a message instead of a crash
//   - the signature of a function is not written here: embr reads it from the function embr_register binds. a doc line
//     is the part a signature cannot say
//   - kCxxAbiVersion is a number to raise by hand when the C++ interface changes in a way the compiler id does not show

#ifndef EMBR_CORE_PLUGININFO_H
#define EMBR_CORE_PLUGININFO_H

#include "../../embr_plugin.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace embr {

inline constexpr int kCxxAbiVersion = 1;

#define EMBR_PI_STR2(x) #x
#define EMBR_PI_STR(x) EMBR_PI_STR2(x)

// a text that is the same for the embr executable and for every plugin built with the same compiler, standard library
// and C++ interface. compared as a whole, so two builds that differ in any part are refused
inline const char* buildId() {
    static const std::string id = [] {
        std::string s = "cxx-abi " + std::to_string(kCxxAbiVersion) + "; ";
#if defined(__clang__)
        s += std::string("clang ") + __clang_version__;
#elif defined(__GNUC__)
        s += std::string("gcc ") + __VERSION__;
#elif defined(_MSC_VER)
        s += "msvc " EMBR_PI_STR(_MSC_VER);
#else
        s += "unknown compiler";
#endif
        s += "; ";
#if defined(_LIBCPP_VERSION)
        s += "libc++ " EMBR_PI_STR(_LIBCPP_VERSION);
#elif defined(_GLIBCXX_RELEASE)
        s += "libstdc++ " EMBR_PI_STR(_GLIBCXX_RELEASE);
#else
        s += "unknown standard library";
#endif
        s += sizeof(void*) == 8 ? "; 64-bit" : "; 32-bit";
        return s;
    }();
    return id.c_str();
}

// the C++ ABI this build speaks, as a short name: the object ABI family, the C++ runtime and its major version, and what changes the
// layout of types or whether exceptions can cross (the old string ABI, a debug library, no exceptions). two builds with the same tag can
// share objects, whichever compiler made them: gcc and clang on the same libstdc++ share "cxx1-itanium-libstdcxx13". it is what embr
// compares when it loads a plugin, and what a package uses to name each build of a plugin (core/package.h). buildId() above is the
// exact toolchain, for messages
inline const char* abiTag() {
    static const std::string tag = [] {
        std::string s = "cxx" + std::to_string(kCxxAbiVersion) + "-";
#if defined(_MSC_VER)
        s += "msvc-v";
#  if _MSC_VER >= 1930 && _MSC_VER < 1950
        s += "143";
#  elif _MSC_VER >= 1920
        s += "142";
#  elif _MSC_VER >= 1910
        s += "141";
#  else
        s += std::to_string(_MSC_VER);
#  endif
#  if !defined(_DLL)
        s += "-static";
#  endif
#  if defined(_ITERATOR_DEBUG_LEVEL) && _ITERATOR_DEBUG_LEVEL != 0
        s += "-debug";
#  endif
#else
        s += "itanium-";
#  if defined(_LIBCPP_VERSION)
        s += "libcxx" + std::to_string(_LIBCPP_VERSION >= 100000 ? _LIBCPP_VERSION / 10000 : _LIBCPP_VERSION / 1000);
#  elif defined(_GLIBCXX_RELEASE)
        s += "libstdcxx" EMBR_PI_STR(_GLIBCXX_RELEASE);
#    if defined(_GLIBCXX_USE_CXX11_ABI) && _GLIBCXX_USE_CXX11_ABI == 0
        s += "-cxx98";
#    endif
#    if defined(_GLIBCXX_DEBUG)
        s += "-debugstl";
#    endif
#  else
        s += "unknownstl";
#  endif
#endif
        if (sizeof(void*) != 8) s += "-32";
#if !defined(__cpp_exceptions) && !defined(_CPPUNWIND)
        s += "-noexc";
#endif
        return s;
    }();
    return tag.c_str();
}

// capabilities by name, for messages, the command line and scripts
struct CapName { uint64_t bit; const char* name; };
inline const std::array<CapName, 6>& capTable() {
    static const std::array<CapName, 6> t = {{
        {EMBR_CAP_FILESYSTEM, "filesystem"}, {EMBR_CAP_NETWORK, "network"}, {EMBR_CAP_PROCESS, "process"},
        {EMBR_CAP_ENV, "env"}, {EMBR_CAP_THREADS, "threads"}, {EMBR_CAP_NATIVE, "native"},
    }};
    return t;
}
inline std::vector<std::string> capNames(uint64_t mask) {
    std::vector<std::string> out;
    for (const auto& c : capTable()) if (mask & c.bit) out.push_back(c.name);
    return out;
}
// "network,process" -> a mask. returns false and names the bad word in `bad` when one is not a capability
inline bool capMask(const std::string& list, uint64_t& mask, std::string& bad) {
    mask = 0;
    std::string cur;
    for (char ch : list + ",") {
        if (ch != ',') { cur += ch; continue; }
        if (cur.empty()) continue;
        bool found = false;
        for (const auto& c : capTable()) if (cur == c.name) { mask |= c.bit; found = true; }
        if (!found) { bad = cur; return false; }
        cur.clear();
    }
    return true;
}

// one doc line, for EMBR_PLUGIN_INFO
struct PluginDoc { const char* name; const char* doc; };

// the table embr_plugin_info() returns for a C++ plugin: one entry per documented function, with no code (embr_register binds it)
template <size_t N> struct PluginInfoHolder {
    std::array<EmbrFunc, N> funcs;
    EmbrPluginInfo          info;
    PluginInfoHolder(const char* name, const char* version, const char* description, uint64_t caps, uint64_t optionalCaps,
                     const PluginDoc (&docs)[N]) {
        for (size_t i = 0; i < N; ++i) {
            funcs[i] = EmbrFunc{};
            funcs[i].name = docs[i].name;
            funcs[i].doc = docs[i].doc;
        }
        info = EmbrPluginInfo{};
        info.abi_version = EMBR_PLUGIN_ABI_VERSION;
        info.struct_size = sizeof(EmbrPluginInfo);
        info.name = name;
        info.version = version;
        info.description = description;
        info.capabilities = caps;
        info.funcs = funcs.data();
        info.nfuncs = (uint32_t)N;
        info.optional_capabilities = optionalCaps;
        info.build_id = buildId();
        info.abi_tag = abiTag();
    }
};

} // namespace embr

#define EMBR_PLUGIN_INFO(NAME, VERSION, DESCRIPTION, CAPS, OPTIONAL_CAPS, ...)                                          \
    extern "C" EMBR_PLUGIN_EXPORT const EmbrPluginInfo* embr_plugin_info(void) {                                       \
        static const ::embr::PluginDoc kDocs[] = { __VA_ARGS__ };                                                      \
        static const ::embr::PluginInfoHolder<sizeof kDocs / sizeof kDocs[0]> kHolder(                                 \
            NAME, VERSION, DESCRIPTION, (uint64_t)(CAPS), (uint64_t)(OPTIONAL_CAPS), kDocs);                          \
        return &kHolder.info;                                                                                          \
    }

#endif // EMBR_CORE_PLUGININFO_H
