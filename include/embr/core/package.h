// package.h
// the `.pmbr` package file: one file that holds everything one `import "name"` can pick, a script module, a native plugin for
// each platform, libraries the module wraps, and assets, plus what the package says about itself.
//
// usage
//   embr pack DIR -o x.pmbr         # DIR has a meta.embr and the files to pack
//   embr package x.pmbr             # read what it says about itself, without unpacking or running anything
//   import "x"                      # finds x.pmbr like it finds x.so or x.embr
//
// the file
//   "PMBR", the file format number, the plugin interface number (0 when there is no plugin), the entry count, the package's own
//   version, then a table of contents with one row per entry (label, offset, length, sha256), then the sha256 of all of that.
//   then the entries, one after the other. numbers are little endian. a label is a path like `lib/linux-x86_64/x.so`
//
// good to know
//   - `meta.embr` is always an entry. it holds a literal `__package__ = {...}`, read like `__module__` is (core/modinfo.h):
//     name, version, description, capabilities, and the entry point, a `module` or a `plugin` per platform (one import, one
//     entry point). `libraries` names native libraries a module wraps; a package with any must declare `native`
//   - the sha256 of the table of contents is the package's id. unpacking goes to a folder named by it, so a changed package
//     never reuses the files of an older one. the table of contents itself is kept in the folder as `.header`
//   - the id and the hashes catch damage and mix-ups, not a forger: whoever makes the file makes the hashes. what ties a package to
//     its publisher is a signature (below), or the whole file's sha256 that a registry publishes
//   - a package can be signed: after the last file comes "PSIG", an Ed25519 public key (32 bytes) and a signature (64) of the
//     package id. the table of contents is not touched, so a package is signed after it is packed (`embr sign`), and anything
//     changed afterwards, in any file, breaks it. a wrong signature always refuses the package; a missing one only when the host
//     requires signed packages. whose key is trusted is a list: the user's (`embr trust`) and the project's (`trusted_keys` in embr.json)

#ifndef EMBR_CORE_PACKAGE_H
#define EMBR_CORE_PACKAGE_H

#include "diagnostics.h"
#include "ed25519.h"
#include "lexer.h"
#include "parser.h"
#include "plugininfo.h"
#include "sha256.h"

#include <algorithm>
#include <cstdint>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace embr {

inline constexpr char     kPmbrMagic[4] = {'P', 'M', 'B', 'R'};
inline constexpr uint32_t kPmbrFormat = 1;
inline constexpr const char* kPackageMetaLabel = "meta.embr";
inline constexpr const char* kPackageMetaName = "__package__";
inline constexpr const char* kPackageHeaderFile = ".header";

constexpr size_t kPmbrMaxEntries = 4096;
constexpr size_t kPmbrMaxLabel = 255;
constexpr uint64_t kPmbrMaxEntrySize = 1ull << 30;

// the platform this embr runs on, as a package names it: "linux-x86_64", "windows-x86_64", "macos-arm64"
inline std::string hostTarget() {
#if defined(_WIN32)
    const char* os = "windows";
#elif defined(__APPLE__)
    const char* os = "macos";
#else
    const char* os = "linux";
#endif
#if defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
    const char* cpu = "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    const char* cpu = "x86_64";
#elif defined(__i386__) || defined(_M_IX86)
    const char* cpu = "x86";
#else
    const char* cpu = "unknown";
#endif
    return std::string(os) + "-" + cpu;
}

// a path made of names with "/" between them: no empty name, no "." or "..", no backslash or colon, no control characters,
// nothing that would leave the folder it is unpacked into on any platform. `{target}` is allowed in a library name in meta
// only, never in a label
inline bool validLabel(const std::string& s, std::string* why = nullptr) {
    auto bad = [&](const char* w) { if (why) *why = w; return false; };
    if (s.empty() || s.size() > kPmbrMaxLabel) return bad("empty or too long");
    if (s == kPackageHeaderFile) return bad("the name is kept for the unpacked table of contents");
    size_t i = 0;
    while (i <= s.size()) {
        size_t j = s.find('/', i);
        if (j == std::string::npos) j = s.size();
        if (j == i) return bad("an empty name in the path");
        std::string seg = s.substr(i, j - i);
        if (seg == "." || seg == "..") return bad("'.' or '..' in the path");
        if (seg.back() == '.' || seg.back() == ' ') return bad("a name ends in a dot or a space");
        for (unsigned char c : seg)
            if (c < 0x20 || c == 0x7f || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
                return bad("a character that is not allowed in a name");
        i = j + 1;
        if (j == s.size()) break;
    }
    return true;
}

struct PackageEntry {
    std::string   label;
    uint64_t      offset = 0, length = 0;
    unsigned char sha[32] = {};
};

inline constexpr char kPsigMagic[4] = {'P', 'S', 'I', 'G'};
constexpr size_t kPsigSize = 4 + 32 + 64;

struct PackageSignature {
    bool          present = false;
    bool          valid = false;
    unsigned char key[32] = {};
    unsigned char sig[64] = {};
    std::string   keyHex() const { return hexOf(key, 32); }
};

struct PackageFile {
    PackageSignature           signature;
    uint64_t                   dataEnd = 0;      // where the last file ends (a signature, if any, follows)
    uint32_t                   format = 0;
    uint32_t                   abi = 0;          // the plugin interface number the package's plugin was built for, 0 for none
    std::string                version;          // the package's own version
    std::vector<PackageEntry>  entries;
    std::string                header;           // the bytes of the table of contents, up to and including its sha256
    std::string                id;               // hex sha256 of the table of contents
    const PackageEntry* find(const std::string& label) const {
        for (const auto& e : entries) if (e.label == label) return &e;
        return nullptr;
    }
};

namespace pkg_detail {
inline void put32(std::string& o, uint32_t v) { for (int i = 0; i < 4; ++i) o.push_back(static_cast<char>((v >> (8 * i)) & 0xFF)); }
inline void put64(std::string& o, uint64_t v) { for (int i = 0; i < 8; ++i) o.push_back(static_cast<char>((v >> (8 * i)) & 0xFF)); }
struct Reader {
    const std::string& in;
    size_t pos = 0;
    bool ok = true;
    explicit Reader(const std::string& s) : in(s) {}
    bool have(size_t n) { if (!ok || in.size() - pos < n) { ok = false; return false; } return true; }
    uint32_t u32() { if (!have(4)) return 0; uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(static_cast<unsigned char>(in[pos + i])) << (8 * i); pos += 4; return v; }
    uint64_t u64() { uint64_t lo = u32(); return lo | (static_cast<uint64_t>(u32()) << 32); }
    uint32_t u16() { if (!have(2)) return 0; uint32_t v = static_cast<unsigned char>(in[pos]) | (static_cast<uint32_t>(static_cast<unsigned char>(in[pos + 1])) << 8); pos += 2; return v; }
    std::string bytes(size_t n) { if (!have(n)) return ""; std::string s = in.substr(pos, n); pos += n; return s; }
};
} // namespace pkg_detail

// what a signature signs: a fixed label (so a signature made for something else is not taken for this) and the package id
inline std::string signedMessage(const std::string& idHex) {
    std::string m = "embr-package-v1\n";
    for (size_t i = 0; i + 1 < idHex.size(); i += 2) m.push_back(static_cast<char>(std::stoi(idHex.substr(i, 2), nullptr, 16)));
    return m;
}

// the table of contents of a package file image. false, with `why`, if it is not one this embr can read. nothing in the entries is
// read here, only checked to lie inside the file and not to overlap
inline bool readPackageFile(const std::string& bytes, PackageFile& pf, std::string& why) {
    using namespace pkg_detail;
    pf = PackageFile{};
    Reader r(bytes);
    if (bytes.size() < 4 || std::memcmp(bytes.data(), kPmbrMagic, 4) != 0) { why = "not a package file (no PMBR at the start)"; return false; }
    r.pos = 4;
    pf.format = r.u32();
    if (!r.ok) { why = "the file is cut short"; return false; }
    if (pf.format != kPmbrFormat) { why = "written for package format " + std::to_string(pf.format) + ", this embr reads format " + std::to_string(kPmbrFormat); return false; }
    pf.abi = r.u32();
    uint32_t n = r.u32();
    uint32_t vlen = r.u32();
    if (!r.ok || n > kPmbrMaxEntries || vlen > 64) { why = "the table of contents is damaged"; return false; }
    pf.version = r.bytes(vlen);
    std::set<std::string> seen, seenLower;
    for (uint32_t i = 0; i < n; ++i) {
        PackageEntry e;
        uint32_t ll = r.u16();
        e.label = r.bytes(ll);
        e.offset = r.u64();
        e.length = r.u64();
        if (r.have(32)) { std::memcpy(e.sha, bytes.data() + r.pos, 32); r.pos += 32; }
        if (!r.ok) { why = "the table of contents is cut short"; return false; }
        std::string lw;
        if (!validLabel(e.label, &lw)) { why = "entry " + std::to_string(i) + " has a bad label (" + lw + ")"; return false; }
        std::string lower = e.label;
        for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (!seen.insert(e.label).second || !seenLower.insert(lower).second) { why = "the label '" + e.label + "' appears twice (or differs only in case)"; return false; }
        if (e.length > kPmbrMaxEntrySize) { why = "'" + e.label + "' is too large"; return false; }
        pf.entries.push_back(std::move(e));
    }
    size_t headerEnd = r.pos;
    unsigned char want[32];
    if (!r.have(32)) { why = "the table of contents is cut short"; return false; }
    std::memcpy(want, bytes.data() + r.pos, 32);
    r.pos += 32;
    unsigned char got[32];
    sha256(bytes.data(), headerEnd, got);
    if (std::memcmp(got, want, 32) != 0) { why = "the table of contents does not match its sha256 (the file is damaged)"; return false; }
    pf.header = bytes.substr(0, r.pos);
    pf.id = hexOf(got, 32);
    // every entry lies after the table and inside the file, and none overlap
    std::vector<const PackageEntry*> order;
    for (const auto& e : pf.entries) {
        if (e.offset < pf.header.size() || e.offset > bytes.size() || e.length > bytes.size() - e.offset) {
            why = "'" + e.label + "' lies outside the file";
            return false;
        }
        order.push_back(&e);
    }
    std::sort(order.begin(), order.end(), [](const PackageEntry* a, const PackageEntry* b) { return a->offset < b->offset; });
    for (size_t i = 1; i < order.size(); ++i)
        if (order[i]->offset < order[i - 1]->offset + order[i - 1]->length) { why = "'" + order[i]->label + "' overlaps '" + order[i - 1]->label + "'"; return false; }
    if (!pf.find(kPackageMetaLabel)) { why = std::string("there is no ") + kPackageMetaLabel + " entry"; return false; }
    pf.dataEnd = pf.header.size();
    for (const auto& e : pf.entries) pf.dataEnd = std::max<uint64_t>(pf.dataEnd, e.offset + e.length);
    if (bytes.size() == pf.dataEnd) return true;
    if (bytes.size() == pf.dataEnd + kPsigSize && std::memcmp(bytes.data() + pf.dataEnd, kPsigMagic, 4) == 0) {
        pf.signature.present = true;
        std::memcpy(pf.signature.key, bytes.data() + pf.dataEnd + 4, 32);
        std::memcpy(pf.signature.sig, bytes.data() + pf.dataEnd + 36, 64);
        const std::string msg = signedMessage(pf.id);
        pf.signature.valid = ed25519::verify(reinterpret_cast<const unsigned char*>(msg.data()), msg.size(), pf.signature.sig, pf.signature.key);
        return true;
    }
    why = "there are " + std::to_string(bytes.size() - pf.dataEnd) + " bytes after the last file that are not a signature";
    return false;
}

// the package signed with the secret seed (32 bytes). an older signature is replaced. false, with `why`, if the image is not a package
inline bool signPackageBytes(const std::string& bytes, const unsigned char seed[32], std::string& out, std::string& why) {
    PackageFile pf;
    if (!readPackageFile(bytes, pf, why)) return false;
    unsigned char pub[32], sec[64], sig[64];
    ed25519::keypair(seed, pub, sec);
    const std::string msg = signedMessage(pf.id);
    ed25519::sign(reinterpret_cast<const unsigned char*>(msg.data()), msg.size(), sec, sig);
    out = bytes.substr(0, static_cast<size_t>(pf.dataEnd));
    out.append(kPsigMagic, 4);
    out.append(reinterpret_cast<const char*>(pub), 32);
    out.append(reinterpret_cast<const char*>(sig), 64);
    return true;
}

// the bytes of one entry, checked against its sha256. false if the file changed since the table was read
inline bool packageEntryBytes(const std::string& bytes, const PackageEntry& e, std::string& out) {
    if (e.offset > bytes.size() || e.length > bytes.size() - e.offset) return false;
    out = bytes.substr(static_cast<size_t>(e.offset), static_cast<size_t>(e.length));
    unsigned char d[32];
    sha256(out.data(), out.size(), d);
    return std::memcmp(d, e.sha, 32) == 0;
}

// a package file image from (label, bytes) pairs, ordered by label so the same inputs always make the same bytes
inline std::string writePackage(uint32_t abi, const std::string& version, std::vector<std::pair<std::string, std::string>> files) {
    using namespace pkg_detail;
    std::sort(files.begin(), files.end());
    std::string head(kPmbrMagic, 4);
    put32(head, kPmbrFormat);
    put32(head, abi);
    put32(head, static_cast<uint32_t>(files.size()));
    put32(head, static_cast<uint32_t>(version.size()));
    head += version;
    size_t tocSize = 0;
    for (const auto& f : files) tocSize += 2 + f.first.size() + 8 + 8 + 32;
    uint64_t offset = head.size() + tocSize + 32;
    std::string toc;
    for (const auto& f : files) {
        toc.push_back(static_cast<char>(f.first.size() & 0xFF));
        toc.push_back(static_cast<char>((f.first.size() >> 8) & 0xFF));
        toc += f.first;
        put64(toc, offset);
        put64(toc, f.second.size());
        unsigned char d[32];
        sha256(f.second.data(), f.second.size(), d);
        toc.append(reinterpret_cast<const char*>(d), 32);
        offset += f.second.size();
    }
    std::string out = head + toc;
    unsigned char h[32];
    sha256(out.data(), out.size(), h);
    out.append(reinterpret_cast<const char*>(h), 32);
    for (const auto& f : files) out += f.second;
    return out;
}

// ---- what a package says about itself ----

struct PackageMeta {
    std::string                        name, version, description;
    uint64_t                           caps = 0;
    std::string                        module;     // the entry point when it is a script module (a label)
    std::map<std::string, std::map<std::string, std::string>> plugin;   // the entry point when it is a native plugin: platform -> (C++ ABI tag -> label). tag "" is a build that does not say; "c" is a C plugin, which any host takes
    std::vector<std::string>           libraries;  // native libraries a module wraps (labels, `{target}` allowed)
    std::map<std::string, std::string> requires_;  // other packages (or plugins, modules) it needs: name -> exact version
    std::vector<std::string>           problems;
    bool                               parsed = false;
};

inline bool validPackageName(const std::string& s) {
    if (s.empty() || s.size() > 64 || !std::isalnum(static_cast<unsigned char>(s[0]))) return false;
    for (unsigned char c : s) if (!(std::isalnum(c) || c == '_' || c == '-')) return false;
    return true;
}

// replaces `{target}` in a library label by the platform
inline std::string expandTarget(const std::string& label, const std::string& target) {
    std::string out = label;
    for (size_t p; (p = out.find("{target}")) != std::string::npos;) out.replace(p, 8, target);
    return out;
}

// reads `__package__ = {literal map}` from meta.embr, without running it
inline PackageMeta parsePackageMeta(const std::string& text) {
    PackageMeta m;
    std::vector<StmtPtr> prog;
    std::vector<EmbrError> errors;
    try {
        Lexer lex(text);
        auto tokens = lex.tokenize();
        SourceMap sm = lex.sourceMap();
        Parser parser(std::move(tokens), sm);
        parser.collectErrors(&errors);
        prog = parser.parse();
    } catch (const EmbrError& e) { m.problems.push_back(std::string("meta.embr does not parse: ") + e.message); return m; }
    if (!errors.empty()) { m.problems.push_back("meta.embr does not parse: " + errors.front().message); return m; }
    m.parsed = true;
    const MapExpr* map = nullptr;
    for (const auto& st : prog) {
        if (st->kind != Stmt::Kind::Assign) { m.problems.push_back("meta.embr may only hold the " + std::string(kPackageMetaName) + " assignment"); continue; }
        auto* a = static_cast<const AssignStmt*>(st.get());
        if (!a->target || a->target->kind != Expr::Kind::Var || static_cast<const VarExpr*>(a->target.get())->name != kPackageMetaName) {
            m.problems.push_back("meta.embr may only hold the " + std::string(kPackageMetaName) + " assignment");
            continue;
        }
        if (map) { m.problems.push_back(std::string(kPackageMetaName) + " is assigned twice"); continue; }
        if (!a->value || a->value->kind != Expr::Kind::Map) { m.problems.push_back(std::string(kPackageMetaName) + " must be a map written out in the file"); continue; }
        map = static_cast<const MapExpr*>(a->value.get());
    }
    if (!map) { if (m.problems.empty()) m.problems.push_back(std::string("meta.embr has no ") + kPackageMetaName); return m; }
    auto str = [&](const Expr* e, const std::string& what, std::string& out) {
        if (!e || e->kind != Expr::Kind::String) { m.problems.push_back("\"" + what + "\" must be a string written out in the file"); return false; }
        out = static_cast<const StringExpr*>(e)->v;
        return true;
    };
    for (const auto& kv : map->entries) {
        if (!kv.first || kv.first->kind != Expr::Kind::String) { m.problems.push_back("a key of " + std::string(kPackageMetaName) + " must be a string"); continue; }
        const std::string key = static_cast<const StringExpr*>(kv.first.get())->v;
        const Expr* v = kv.second.get();
        if (key == "name") str(v, key, m.name);
        else if (key == "version") str(v, key, m.version);
        else if (key == "description") str(v, key, m.description);
        else if (key == "module") str(v, key, m.module);
        else if (key == "capabilities" || key == "libraries") {
            if (!v || v->kind != Expr::Kind::Array || static_cast<const ArrayExpr*>(v)->isRange) { m.problems.push_back("\"" + key + "\" must be an array of strings"); continue; }
            for (const auto& el : static_cast<const ArrayExpr*>(v)->elements) {
                std::string s;
                if (!str(el.get(), key, s)) continue;
                if (key == "libraries") { m.libraries.push_back(s); continue; }
                uint64_t bit = 0;
                std::string bad;
                if (!capMask(s, bit, bad) || bit == 0) m.problems.push_back("'" + s + "' is not a capability (filesystem, network, process, env, threads, native)");
                else m.caps |= bit;
            }
        } else if (key == "requires") {
            if (!v || v->kind != Expr::Kind::Map) { m.problems.push_back("\"requires\" must be a map from a name to an exact version"); continue; }
            for (const auto& rk : static_cast<const MapExpr*>(v)->entries) {
                std::string n, ver;
                if (str(rk.first.get(), "requires", n) && str(rk.second.get(), "requires", ver)) m.requires_[n] = ver;
            }
        } else if (key == "plugin") {
            if (!v || v->kind != Expr::Kind::Map) { m.problems.push_back("\"plugin\" must be a map from platform to a label, or to a map from C++ ABI tag to label"); continue; }
            for (const auto& pk : static_cast<const MapExpr*>(v)->entries) {
                std::string t, l;
                if (!str(pk.first.get(), "plugin", t)) continue;
                if (pk.second && pk.second->kind == Expr::Kind::Map) {          // several builds of one platform, by C++ ABI tag
                    for (const auto& bk : static_cast<const MapExpr*>(pk.second.get())->entries) {
                        std::string tag;
                        if (str(bk.first.get(), "plugin", tag) && str(bk.second.get(), "plugin", l)) m.plugin[t][tag] = l;
                    }
                } else if (str(pk.second.get(), "plugin", l)) m.plugin[t][""] = l;
            }
        } else m.problems.push_back("unknown key \"" + key + "\" in " + kPackageMetaName + " (name, version, description, capabilities, module, plugin, libraries, requires)");
    }
    return m;
}

// meta.embr text for a PackageMeta, in one fixed layout (`embr merge` writes the merged package's meta this way)
inline std::string writePackageMeta(const PackageMeta& m) {
    auto q = [](const std::string& x) {
        std::string o = "\"";
        for (char c : x) {
            if (c == '"' || c == '\\') { o += '\\'; o += c; }
            else if (c == '\n') o += "\\n";
            else if (c == '\t') o += "\\t";
            else o += c;
        }
        return o + "\"";
    };
    std::string o = std::string(kPackageMetaName) + " = {\n";
    o += "    \"name\": " + q(m.name) + ",\n    \"version\": " + q(m.version) + ",\n    \"description\": " + q(m.description) + ",\n";
    o += "    \"capabilities\": [";
    bool first = true;
    for (const auto& n : capNames(m.caps)) { o += (first ? "" : ", ") + q(n); first = false; }
    o += "],\n";
    if (!m.module.empty()) o += "    \"module\": " + q(m.module) + ",\n";
    if (!m.plugin.empty()) {
        o += "    \"plugin\": {";
        first = true;
        for (const auto& kv : m.plugin) {
            o += (first ? "" : ", ") + q(kv.first) + ": ";
            first = false;
            if (kv.second.size() == 1 && kv.second.begin()->first.empty()) { o += q(kv.second.begin()->second); continue; }
            o += "{";
            bool f2 = true;
            for (const auto& b : kv.second) { o += (f2 ? "" : ", ") + q(b.first) + ": " + q(b.second); f2 = false; }
            o += "}";
        }
        o += "},\n";
    }
    if (!m.libraries.empty()) {
        o += "    \"libraries\": [";
        first = true;
        for (const auto& l : m.libraries) { o += (first ? "" : ", ") + q(l); first = false; }
        o += "],\n";
    }
    if (!m.requires_.empty()) {
        o += "    \"requires\": {";
        first = true;
        for (const auto& kv : m.requires_) { o += (first ? "" : ", ") + q(kv.first) + ": " + q(kv.second); first = false; }
        o += "},\n";
    }
    return o + "}\n";
}

// does the meta make sense on its own and next to the table of contents? adds to m.problems
inline void checkPackageMeta(const PackageFile& pf, PackageMeta& m) {
    auto problem = [&](const std::string& s) { m.problems.push_back(s); };
    if (!validPackageName(m.name)) problem("\"name\" must be letters, digits, '_' and '-' (it is the name used to import the package)");
    if (m.version.empty()) problem("\"version\" is missing");
    if (m.module.empty() == m.plugin.empty()) problem("a package has exactly one entry point: a \"module\" or a \"plugin\" (a library for each platform)");
    if (!m.module.empty()) {
        const auto ext = std::filesystem::path(m.module).extension();
        if (ext != ".embr" && ext != ".vmbr") problem("\"module\" must be a .embr or .vmbr file");
        else if (!pf.find(m.module)) problem("\"module\" names '" + m.module + "', which is not in the package");
        if (pf.abi != 0) problem("the file says it has a plugin but meta names a module");
    }
    for (const auto& [target, builds] : m.plugin) {
        if (builds.empty()) problem("\"plugin\" for " + target + " has no build");
        for (const auto& [tag, label] : builds) {
            for (unsigned char c : tag) if (!(std::isalnum(c) || c == '_' || c == '.' || c == '+' || c == '-')) { problem("'" + tag + "' is not a C++ ABI tag (see `embr abi`) or \"c\""); break; }
            if (!pf.find(label)) problem("\"plugin\" for " + target + (tag.empty() ? "" : " (" + tag + ")") + " names '" + label + "', which is not in the package");
        }
    }
    if (!m.plugin.empty() && pf.abi == 0) problem("the file does not say which plugin interface its plugin was built for");
    for (const auto& lib : m.libraries) {
        if (lib.find("{target}") == std::string::npos) {
            if (!pf.find(lib)) problem("\"libraries\" names '" + lib + "', which is not in the package");
        } else {
            bool any = false;
            for (const auto& e : pf.entries) {
                // the part before `{target}` and after must match, with one platform name between
                size_t p = lib.find("{target}");
                const std::string pre = lib.substr(0, p), post = lib.substr(p + 8);
                if (e.label.size() > pre.size() + post.size() && e.label.compare(0, pre.size(), pre) == 0 &&
                    e.label.compare(e.label.size() - post.size(), post.size(), post) == 0) any = true;
            }
            if (!any) problem("\"libraries\" names '" + lib + "', and no platform has a file for it");
        }
    }
    for (const auto& [n, ver] : m.requires_) {
        if (!validPackageName(n)) problem("\"requires\" names '" + n + "', which is not a package name");
        else if (n == m.name) problem("a package cannot require itself");
        if (ver.empty() || ver.find_first_of(" \t\n<>=!~^*") != std::string::npos) problem("\"requires\" wants an exact version for " + n + ", like \"1.2.0\"");
    }
    if (!m.libraries.empty() && !(m.caps & EMBR_CAP_NATIVE)) problem("it bundles native libraries, so \"capabilities\" must include native");
}

// what `embr package`, `embr packages` and `pmbr_meta` show: the meta, the table of contents, and what is wrong
struct PackageInfo {
    std::string               path;
    bool                      parsed = false;    // the table of contents and meta could be read
    PackageFile               file;
    PackageMeta               meta;
    std::vector<std::string>  problems;
    std::vector<std::string>  targets;           // the platforms that have a build (plugin) or a library (module)
    std::vector<std::string>  builds;            // a plugin's builds as "platform/tag" (tag omitted when the build does not say)
    uint64_t                  size = 0;
};

inline std::optional<std::string> readPackageBytes(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return std::nullopt;
    return std::string((std::istreambuf_iterator<char>(f)), {});
}

// reads a package file for a person or a tool: nothing is unpacked and nothing in it is run
inline PackageInfo analyzePackageBytes(const std::string& bytes, const std::string& path) {
    PackageInfo info;
    info.path = path;
    info.size = bytes.size();
    std::string why;
    if (!readPackageFile(bytes, info.file, why)) { info.problems.push_back(why); return info; }
    std::string metaText;
    if (!packageEntryBytes(bytes, *info.file.find(kPackageMetaLabel), metaText)) { info.problems.push_back("meta.embr does not match its sha256"); return info; }
    if (info.file.signature.present && !info.file.signature.valid)
        info.problems.push_back("the signature does not match the package: it was changed after it was signed, or the signature is not for this package");
    info.meta = parsePackageMeta(metaText);
    if (info.meta.parsed) checkPackageMeta(info.file, info.meta);
    info.parsed = info.meta.parsed;
    for (const auto& p : info.meta.problems) info.problems.push_back(p);
    for (const auto& kv : info.meta.plugin) {
        info.targets.push_back(kv.first);
        for (const auto& b : kv.second) info.builds.push_back(kv.first + (b.first.empty() ? "" : "/" + b.first));
    }
    if (info.meta.plugin.empty()) {
        for (const auto& lib : info.meta.libraries) {
            size_t p = lib.find("{target}");
            if (p == std::string::npos) continue;
            const std::string pre = lib.substr(0, p), post = lib.substr(p + 8);
            for (const auto& e : info.file.entries)
                if (e.label.size() > pre.size() + post.size() && e.label.compare(0, pre.size(), pre) == 0 &&
                    e.label.compare(e.label.size() - post.size(), post.size(), post) == 0) {
                    std::string t = e.label.substr(pre.size(), e.label.size() - pre.size() - post.size());
                    if (std::find(info.targets.begin(), info.targets.end(), t) == info.targets.end()) info.targets.push_back(t);
                }
        }
    }
    std::sort(info.targets.begin(), info.targets.end());
    return info;
}

inline PackageInfo analyzePackageFile(const std::filesystem::path& p) {
    auto bytes = readPackageBytes(p);
    if (!bytes) { PackageInfo i; i.path = p.string(); i.problems.push_back("cannot open the file"); return i; }
    return analyzePackageBytes(*bytes, p.string());
}

// ---- merging ----

// one package from several that are the same package built on different platforms (each made by `embr pack` where its library was built).
// they must agree on everything but the platforms: name, version, description, capabilities, entry point, plugin interface and requires.
// the plugin maps and the libraries are joined, the files are the union, and a file with the same label must have the same bytes
inline bool mergePackages(const std::vector<std::string>& images, std::string& out, std::string& why) {
    if (images.size() < 2) { why = "merging needs at least two packages"; return false; }
    PackageMeta merged;
    uint32_t abi = 0;
    std::map<std::string, std::string> files;    // label -> bytes
    std::map<std::string, std::string> sha;      // label -> raw sha256 (32 bytes)
    for (size_t i = 0; i < images.size(); ++i) {
        PackageInfo p = analyzePackageBytes(images[i], "package " + std::to_string(i + 1));
        if (!p.parsed || !p.problems.empty()) { why = "package " + std::to_string(i + 1) + ": " + (p.problems.empty() ? "cannot read it" : p.problems.front()); return false; }
        const PackageMeta& m = p.meta;
        auto differs = [&](const char* what) { why = std::string("the packages differ in ") + what + ", so they are not the same package built twice"; return false; };
        if (i == 0) {
            merged = m;
            abi = p.file.abi;
        } else {
            if (m.name != merged.name) return differs("name");
            if (m.version != merged.version) return differs("version");
            if (m.description != merged.description) return differs("description");
            if (m.caps != merged.caps) return differs("capabilities");
            if (m.module != merged.module) return differs("the module entry point");
            if (m.requires_ != merged.requires_) return differs("requires");
            if (p.file.abi != abi) return differs("the plugin interface number");
            for (const auto& [target, builds] : m.plugin)
                for (const auto& [tag, label] : builds) {
                    auto& slot = merged.plugin[target];
                    auto it = slot.find(tag);
                    if (it != slot.end() && it->second != label) { why = "both packages have a plugin for " + target + (tag.empty() ? "" : " (" + tag + ")") + ", with different library names"; return false; }
                    slot[tag] = label;
                }
            for (const auto& l : m.libraries)
                if (std::find(merged.libraries.begin(), merged.libraries.end(), l) == merged.libraries.end()) merged.libraries.push_back(l);
        }
        for (const auto& e : p.file.entries) {
            if (e.label == kPackageMetaLabel) continue;
            std::string data;
            if (!packageEntryBytes(images[i], e, data)) { why = "'" + e.label + "' does not match its sha256 (package " + std::to_string(i + 1) + " is damaged)"; return false; }
            std::string raw(reinterpret_cast<const char*>(e.sha), 32);
            auto it = sha.find(e.label);
            if (it != sha.end() && it->second != raw) { why = "'" + e.label + "' is a different file in two of the packages"; return false; }
            sha[e.label] = raw;
            files[e.label] = std::move(data);
        }
    }
    std::sort(merged.libraries.begin(), merged.libraries.end());
    std::vector<std::pair<std::string, std::string>> all(files.begin(), files.end());
    all.emplace_back(kPackageMetaLabel, writePackageMeta(merged));
    out = writePackage(abi, merged.version, std::move(all));
    PackageInfo check = analyzePackageBytes(out, "the merged package");
    if (!check.parsed || !check.problems.empty()) { why = "the merged package is not valid: " + (check.problems.empty() ? std::string("cannot read it") : check.problems.front()); out.clear(); return false; }
    return true;
}

// ---- keys and who is trusted ----

inline bool hexToBytes(const std::string& hex, unsigned char* out, size_t n) {
    if (hex.size() != n * 2) return false;
    for (size_t i = 0; i < n; ++i) {
        int hi = std::isxdigit(static_cast<unsigned char>(hex[2 * i])) ? std::stoi(hex.substr(2 * i, 1), nullptr, 16) : -1;
        int lo = std::isxdigit(static_cast<unsigned char>(hex[2 * i + 1])) ? std::stoi(hex.substr(2 * i + 1, 1), nullptr, 16) : -1;
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<unsigned char>(hi * 16 + lo);
    }
    return true;
}

// a key file is two lines: what it is ("embr-secret-key 1" or "embr-public-key 1") and the key in hex
inline std::string keyFileText(bool secret, const unsigned char* key32) {
    return std::string(secret ? "embr-secret-key 1\n" : "embr-public-key 1\n") + hexOf(key32, 32) + "\n";
}

inline bool readKeyFile(const std::filesystem::path& file, bool secret, unsigned char* out32, std::string& why) {
    auto text = readPackageBytes(file);
    if (!text) { why = "cannot open " + file.string(); return false; }
    std::string head = std::string(secret ? "embr-secret-key 1" : "embr-public-key 1");
    if (text->compare(0, head.size(), head) != 0) { why = file.string() + " is not an embr " + (secret ? "secret" : "public") + " key file"; return false; }
    std::string hex;
    for (char c : text->substr(head.size())) if (!std::isspace(static_cast<unsigned char>(c))) hex.push_back(c);
    if (!hexToBytes(hex, out32, 32)) { why = file.string() + " does not hold a 64 digit key"; return false; }
    return true;
}

// the user's list of trusted public keys: EMBR_TRUST_FILE, else a file in the user's config folder. one key a line (64 hex digits,
// then anything as a note)
inline std::filesystem::path userTrustFile() {
    if (const char* e = std::getenv("EMBR_TRUST_FILE"); e && *e) return std::filesystem::path(e);
#if defined(_WIN32)
    if (const char* a = std::getenv("APPDATA"); a && *a) return std::filesystem::path(a) / "embr" / "trusted_keys";
#elif defined(__APPLE__)
    if (const char* h = std::getenv("HOME"); h && *h) return std::filesystem::path(h) / "Library" / "Application Support" / "embr" / "trusted_keys";
#else
    if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && *x) return std::filesystem::path(x) / "embr" / "trusted_keys";
    if (const char* h = std::getenv("HOME"); h && *h) return std::filesystem::path(h) / ".config" / "embr" / "trusted_keys";
#endif
    return std::filesystem::temp_directory_path() / "embr-trusted_keys";
}

struct TrustedKey { std::string hex, note; };

inline std::vector<TrustedKey> readTrustFile(const std::filesystem::path& file) {
    std::vector<TrustedKey> out;
    std::ifstream in(file);
    for (std::string line; std::getline(in, line);) {
        size_t a = line.find_first_not_of(" \t\r");
        if (a == std::string::npos || line[a] == '#') continue;
        size_t b = line.find_first_of(" \t\r", a);
        TrustedKey k;
        k.hex = line.substr(a, b == std::string::npos ? std::string::npos : b - a);
        unsigned char tmp[32];
        if (!hexToBytes(k.hex, tmp, 32)) continue;
        for (auto& c : k.hex) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (b != std::string::npos) {
            size_t n = line.find_first_not_of(" \t", b);
            if (n != std::string::npos) k.note = line.substr(n, line.find_last_not_of(" \t\r") - n + 1);
        }
        out.push_back(std::move(k));
    }
    return out;
}

inline bool writeTrustFile(const std::filesystem::path& file, const std::vector<TrustedKey>& keys, std::string& why) {
    std::error_code ec;
    if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::trunc);
    for (const auto& k : keys) out << k.hex << (k.note.empty() ? "" : " " + k.note) << "\n";
    out.close();
    if (!out) { why = "cannot write " + file.string(); return false; }
    return true;
}

// the keys a project trusts: "trusted_keys": ["<64 hex digits>", ...] in the embr.json of `dir`. read by looking for that array,
// not as full JSON: only 64 digit hex strings in it count
inline std::set<std::string> projectTrustedKeys(const std::filesystem::path& dir) {
    std::set<std::string> out;
    auto text = readPackageBytes(dir / "embr.json");
    if (!text) return out;
    size_t k = text->find("\"trusted_keys\"");
    if (k == std::string::npos) return out;
    size_t open = text->find('[', k), close = text->find(']', k);
    if (open == std::string::npos || close == std::string::npos || close < open) return out;
    std::string body = text->substr(open + 1, close - open - 1), cur;
    bool in = false;
    for (char c : body) {
        if (c == '"') {
            if (in) {
                unsigned char tmp[32];
                if (hexToBytes(cur, tmp, 32)) { for (auto& ch : cur) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); out.insert(cur); }
                cur.clear();
            }
            in = !in;
        } else if (in) cur.push_back(c);
    }
    return out;
}

// ---- unpacking ----

// where unpacked packages live: EMBR_PACKAGE_DIR, else the user's cache folder
inline std::filesystem::path packageCacheRoot() {
    if (const char* e = std::getenv("EMBR_PACKAGE_DIR"); e && *e) return std::filesystem::path(e);
#if defined(_WIN32)
    if (const char* a = std::getenv("LOCALAPPDATA"); a && *a) return std::filesystem::path(a) / "embr" / "packages";
#elif defined(__APPLE__)
    if (const char* h = std::getenv("HOME"); h && *h) return std::filesystem::path(h) / "Library" / "Caches" / "embr" / "packages";
#else
    if (const char* x = std::getenv("XDG_CACHE_HOME"); x && *x) return std::filesystem::path(x) / "embr" / "packages";
    if (const char* h = std::getenv("HOME"); h && *h) return std::filesystem::path(h) / ".cache" / "embr" / "packages";
#endif
    return std::filesystem::temp_directory_path() / "embr-packages";
}

// writes every entry of the package to its own file under a folder named by the package id, each in the folder its label says.
// does nothing when that folder is already there with the same table of contents. a half-written folder is never left in place:
// the files go to a temporary folder that is renamed when complete. false, with `why`, when an entry does not match its sha256
inline bool unpackPackage(const std::string& bytes, const PackageFile& pf, const std::string& name, std::filesystem::path& dirOut, std::string& why) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path root = packageCacheRoot();
    const fs::path dir = root / (name + "-" + pf.id.substr(0, 32));
    dirOut = dir;
    {
        std::ifstream h(dir / kPackageHeaderFile, std::ios::binary);
        if (h) {
            std::string have((std::istreambuf_iterator<char>(h)), {});
            if (have == pf.header) return true;
        }
    }
    // the entries are checked before anything is written
    std::vector<std::string> data(pf.entries.size());
    for (size_t i = 0; i < pf.entries.size(); ++i)
        if (!packageEntryBytes(bytes, pf.entries[i], data[i])) { why = "'" + pf.entries[i].label + "' does not match its sha256 (the file is damaged)"; return false; }
    fs::create_directories(root, ec);
    if (ec) { why = "cannot make " + root.string() + ": " + ec.message(); return false; }
    const fs::path tmp = root / (name + "-" + pf.id.substr(0, 32) + ".tmp" + std::to_string(std::random_device{}()));
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    if (ec) { why = "cannot make " + tmp.string() + ": " + ec.message(); return false; }
    auto cleanup = [&] { std::error_code e2; fs::remove_all(tmp, e2); };
    for (size_t i = 0; i < pf.entries.size(); ++i) {
        const fs::path file = tmp / fs::path(pf.entries[i].label);
        fs::create_directories(file.parent_path(), ec);
        std::ofstream f(file, std::ios::binary | std::ios::trunc);
        f.write(data[i].data(), static_cast<std::streamsize>(data[i].size()));
        f.close();
        if (!f) { why = "cannot write " + file.string(); cleanup(); return false; }
    }
    {
        std::ofstream f(tmp / kPackageHeaderFile, std::ios::binary | std::ios::trunc);
        f.write(pf.header.data(), static_cast<std::streamsize>(pf.header.size()));
        f.close();
        if (!f) { why = "cannot write the table of contents"; cleanup(); return false; }
    }
    fs::rename(tmp, dir, ec);
    if (ec) {
        // another process unpacked the same package first: use its folder if it is the same
        std::ifstream h(dir / kPackageHeaderFile, std::ios::binary);
        std::string have = h ? std::string((std::istreambuf_iterator<char>(h)), {}) : std::string();
        cleanup();
        if (have != pf.header) { why = "cannot move " + tmp.string() + " to " + dir.string() + ": " + ec.message(); return false; }
    }
    return true;
}

// remember which package file an unpacked folder came from (one path a line), so `embr cache clean --packages` can tell when nobody
// uses it any more
inline void recordPackageSource(const std::filesystem::path& dir, const std::filesystem::path& file) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const std::string src = fs::weakly_canonical(file, ec).string();
    std::string have;
    {
        std::ifstream in(dir / ".sources", std::ios::binary);
        if (in) have.assign(std::istreambuf_iterator<char>(in), {});
    }
    if (have.find(src + "\n") != std::string::npos) return;
    std::ofstream out(dir / ".sources", std::ios::binary | std::ios::app);
    out << src << "\n";
}

// removes unpacked packages nothing needs: half-written ones, ones whose table of contents is not what the folder name says, ones whose
// package file is gone or has changed since (a newer version replaced it), and old temporary folders. `all` removes every folder.
// returns what was removed (folder names)
inline std::vector<std::string> cleanPackageCache(const std::filesystem::path& root, bool all) {
    namespace fs = std::filesystem;
    std::error_code ec;
    std::vector<std::string> removed;
    if (!fs::is_directory(root, ec)) return removed;
    std::vector<fs::path> dirs;
    for (const auto& e : fs::directory_iterator(root, ec))
        if (e.is_directory(ec)) dirs.push_back(e.path());
    std::sort(dirs.begin(), dirs.end());
    for (const auto& d : dirs) {
        const std::string name = d.filename().string();
        bool drop = all;
        if (!drop) {
            const size_t tmp = name.find(".tmp");
            if (tmp != std::string::npos) {
                // a folder being written right now belongs to another process; only an old one is left over
                auto t = fs::last_write_time(d, ec);
                drop = !ec && fs::file_time_type::clock::now() - t > std::chrono::minutes(10);
            } else {
                std::ifstream h(d / kPackageHeaderFile, std::ios::binary);
                std::string header = h ? std::string((std::istreambuf_iterator<char>(h)), {}) : std::string();
                const size_t dash = name.rfind('-');
                bool ok = header.size() > 32 && dash != std::string::npos;
                std::string id;
                if (ok) {
                    unsigned char got[32];
                    sha256(header.data(), header.size() - 32, got);
                    ok = std::memcmp(got, header.data() + header.size() - 32, 32) == 0;
                    id = hexOf(got, 32);
                    ok = ok && name.substr(dash + 1) == id.substr(0, 32);
                }
                if (!ok) drop = true;
                else {
                    // used while some package file that lists it still has this id
                    bool used = false;
                    std::ifstream src(d / ".sources", std::ios::binary);
                    for (std::string line; src && std::getline(src, line);) {
                        auto bytes = readPackageBytes(line);
                        PackageFile pf;
                        std::string why;
                        if (bytes && readPackageFile(*bytes, pf, why) && pf.id == id) { used = true; break; }
                    }
                    drop = !used;
                }
            }
        }
        if (drop) {
            fs::remove_all(d, ec);
            if (!ec) removed.push_back(name);
        }
    }
    return removed;
}

} // namespace embr

#endif // EMBR_CORE_PACKAGE_H
