// describe.h
// turns what a plugin said about itself (core/plugininfo.h, embr_plugin.h) plus the natives it bound into text, a map for
// scripts and JSON for tools. used by the embrmeta plugin, `embr plugins` / `embr plugin NAME` and the language server.
// also the one place that writes a signature the way a script spells it.
//
// good to know
//   - a signature is read from the native the plugin bound, never written by hand, so it cannot drift from the code
//   - a plugin's doc lines come from the plugin. a native with no doc line is still listed, with an empty doc, so a check
//     can find it (tools/docs/check_wiki.py does)

#ifndef EMBR_CORE_DESCRIBE_H
#define EMBR_CORE_DESCRIBE_H

#include "registry.h"
#include "typecheck.h"
#include "plugininfo.h"

#include <string>
#include <vector>

namespace embr {

// a type the way a script writes it: `int`, `str|arr`, `arr<int>`
inline std::string typeText(TypeSet t, const TypeElem& el) {
    std::string out = typecheck::detail::tn(t);
    if (el && (out == "arr" || out == "map")) out += "<" + typeText(el->t, el->el) + ">";
    return out;
}

inline std::string paramText(const Param& p) {
    std::string out = p.variadic ? "..." : p.inout ? "&" : "";
    out += p.name;
    if (p.optional && !p.variadic) out += "?";
    if (p.explicitType || !p.type.isAny() || p.elem) out += ": " + typeText(p.type, p.elem);
    return out;
}

// "fn name(a: str, b?: int) -> str"
inline std::string signatureText(const std::string& name, const std::vector<Param>& params, TypeSet ret, const TypeElem& retEl) {
    std::string out = "fn " + name + "(";
    for (size_t i = 0; i < params.size(); ++i) out += (i ? ", " : "") + paramText(params[i]);
    out += ")";
    if (!ret.isAny() || retEl) out += " -> " + typeText(ret, retEl);
    return out;
}

// one function of a plugin
struct FunctionView {
    std::string name;
    std::string signature;   // "" when the plugin documents a name it does not bind
    std::string doc;         // "" when the plugin did not document it
    std::string group;       // the overload group it is a member of (`math_abs` for math_abs__1), else ""
    bool        bound = false;
};

// the functions of a plugin: everything it bound (found as new globals while it loaded), plus any name it documents
// that it did not bind. sorted by name
inline std::vector<FunctionView> pluginFunctions(const Interpreter& interp, const Interpreter::PluginRecord& r) {
    std::vector<FunctionView> out;
    auto find = [&](const std::string& n) -> FunctionView* {
        for (auto& f : out) if (f.name == n) return &f;
        return nullptr;
    };
    auto addBound = [&](const std::string& n) {
        if (find(n) || !interp.hasGlobal(n)) return;
        const Value& v = interp.getGlobal(n);
        if (v.tag() != TypeTag::Callable) return;
        const auto& c = v.asCallable();
        if (!c.isNative()) return;
        FunctionView f;
        f.name = n;
        f.signature = signatureText(n, c.sig, c.returns, c.returnsElem);
        f.bound = true;
        std::string base;
        if (overloadName(n, &base)) f.group = base;
        out.push_back(std::move(f));
    };
    for (const auto& n : r.bound) addBound(n);
    for (const auto& d : r.fns) {       // a C plugin's own table: bound by the loader, not found by the snapshot
        addBound(d.name);
        FunctionView* f = find(d.name);
        if (!f) { FunctionView g; g.name = d.name; out.push_back(std::move(g)); f = &out.back(); }
        f->doc = d.doc;
    }
    std::sort(out.begin(), out.end(), [](const FunctionView& a, const FunctionView& b) { return a.name < b.name; });
    return out;
}

inline const char* kindText(const Interpreter::PluginRecord& r) {
    return r.kind == Interpreter::PluginRecord::Kind::C ? "c" : "cxx";
}

// a plugin as a script value: a map with the name, version, description, kind ("c" or "cxx"), plugin interface number,
// build id (a C++ plugin), path, capabilities (names), optional_capabilities, and functions (an array of maps with
// name, signature, doc and group)
inline Value pluginValue(const Interpreter& interp, const Interpreter::PluginRecord& r, bool withFunctions) {
    Value::map_type m;
    auto names = [](uint64_t mask) {
        Value::array_type a;
        for (const auto& n : capNames(mask)) a.push_back(Value(n));
        return Value(std::move(a));
    };
    m["name"] = Value(r.name);
    m["version"] = Value(r.version);
    m["description"] = Value(r.description);
    m["kind"] = Value(std::string(kindText(r)));
    m["declared"] = Value(r.declared ? (int64_t)1 : (int64_t)0);
    m["abi"] = Value((int64_t)r.abi);
    m["build_id"] = Value(r.buildId);
    m["abi_tag"] = Value(r.abiTag);
    m["path"] = Value(r.path);
    m["capabilities"] = names(r.caps);
    m["optional_capabilities"] = names(r.optionalCaps);
    if (withFunctions) {
        Value::array_type fns;
        for (const auto& f : pluginFunctions(interp, r)) {
            Value::map_type fm;
            fm["name"] = Value(f.name);
            fm["signature"] = Value(f.signature);
            fm["doc"] = Value(f.doc);
            fm["group"] = Value(f.group);
            fns.push_back(Value(std::move(fm)));
        }
        m["functions"] = Value(std::move(fns));
    }
    return Value(std::move(m));
}

inline std::string jsonQuote(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); out += b; }
                else out += (char)c;
        }
    }
    return out + "\"";
}

inline std::string capListText(uint64_t mask) {
    std::string out;
    for (const auto& n : capNames(mask)) out += (out.empty() ? "" : ", ") + n;
    return out.empty() ? "none" : out;
}

// the same as pluginValue, as JSON text (for tools: `embr plugin NAME --json`)
inline std::string pluginJson(const Interpreter& interp, const Interpreter::PluginRecord& r, bool withFunctions) {
    auto arr = [](uint64_t mask) {
        std::string s = "[";
        bool first = true;
        for (const auto& n : capNames(mask)) { s += (first ? "" : ", ") + jsonQuote(n); first = false; }
        return s + "]";
    };
    std::string s = "{";
    s += "\"name\": " + jsonQuote(r.name) + ", \"version\": " + jsonQuote(r.version) + ", \"description\": " + jsonQuote(r.description);
    s += std::string(", \"kind\": ") + jsonQuote(kindText(r)) + ", \"declared\": " + (r.declared ? "true" : "false");
    s += ", \"abi\": " + std::to_string(r.abi) + ", \"build_id\": " + jsonQuote(r.buildId) + ", \"abi_tag\": " + jsonQuote(r.abiTag) + ", \"path\": " + jsonQuote(r.path);
    s += ", \"capabilities\": " + arr(r.caps) + ", \"optional_capabilities\": " + arr(r.optionalCaps);
    if (withFunctions) {
        s += ", \"functions\": [";
        bool first = true;
        for (const auto& f : pluginFunctions(interp, r)) {
            s += std::string(first ? "" : ", ") + "{\"name\": " + jsonQuote(f.name) + ", \"signature\": " + jsonQuote(f.signature) +
                 ", \"doc\": " + jsonQuote(f.doc) + ", \"group\": " + jsonQuote(f.group) + ", \"bound\": " + (f.bound ? "true" : "false") + "}";
            first = false;
        }
        s += "]";
    }
    return s + "}";
}

// the plugin as text for a person: a header, then every function with its signature and doc
inline std::string pluginText(const Interpreter& interp, const Interpreter::PluginRecord& r) {
    std::string s = r.name + " " + (r.version.empty() ? "(no version)" : r.version) + "  " + r.description + "\n";
    s += std::string("  kind: ") + (r.kind == Interpreter::PluginRecord::Kind::C ? "written in C" : "written in C++") +
         (r.declared ? ", plugin interface " + std::to_string(r.abi) : ", no description exported") + "\n";
    s += "  needs: " + capListText(r.caps) + "\n";
    if (r.optionalCaps) s += "  uses when allowed: " + capListText(r.optionalCaps) + "\n";
    if (!r.buildId.empty()) s += "  built with: " + r.buildId + "\n";
    if (!r.abiTag.empty()) s += "  C++ ABI: " + r.abiTag + "\n";
    s += "  file: " + r.path + "\n\nfunctions\n";
    for (const auto& f : pluginFunctions(interp, r)) {
        std::string sig = f.signature.empty() ? f.name : f.signature.substr(f.signature.find(' ') + 1);   // without the leading "fn "
        s += "  " + sig + "\n";
        if (!f.doc.empty()) s += "      " + f.doc + "\n";
    }
    return s;
}

} // namespace embr

#endif // EMBR_CORE_DESCRIBE_H
