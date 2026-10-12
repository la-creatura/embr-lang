// packageimport.h
// `import "name"` finding name.pmbr (core/package.h): reading it, refusing it when the host denies what it needs, unpacking it
// and then loading its entry point, a plugin library for this platform or a script module. also what `embr package` prints.
//
// good to know
//   - the checks come in two rounds. before anything is unpacked: the table of contents, the meta and the capabilities it declares
//     against the ones the host denies. after: a plugin's library must say what the package says it is (version, name, and no
//     capability the package leaves out), and a module's imports must not need more than it declares
//   - the library or module that runs is read back from the unpacked folder and hashed first, so a file changed there after it was
//     unpacked is not run
//   - a package is one import name. `import "x"` gives all of its entry point and nothing else; its other files are for the entry
//     point to use (package_path in the `package` plugin gives their location)

#ifndef EMBR_CORE_PACKAGEIMPORT_H
#define EMBR_CORE_PACKAGEIMPORT_H

#include "describe.h"
#include "modinfo.h"
#include "module.h"
#include "package.h"

#include <set>
#include <string>
#include <vector>

namespace embr {

// the file at `dir`/label, when its sha256 is the one the package said. "" if it is
inline std::string checkUnpackedFile(const std::filesystem::path& dir, const PackageEntry& e) {
    auto bytes = readPackageBytes(dir / std::filesystem::path(e.label));
    if (!bytes) return "the unpacked file " + (dir / e.label).string() + " is gone";
    unsigned char d[32];
    sha256(bytes->data(), bytes->size(), d);
    if (std::memcmp(d, e.sha, 32) != 0) return "the unpacked file " + (dir / e.label).string() + " was changed after it was unpacked";
    return "";
}

// is this public key (hex) trusted? the user's list, the keys of the project in the working folder (a host that restricts imports ignores
// that file, a sandboxed script could have planted it), and the ones an embedder gave the policy
inline bool keyIsTrusted(const PluginPolicy* policy, const std::string& hex) {
    if (policy && policy->trustedKeys.count(hex)) return true;
    for (const auto& k : readTrustFile(userTrustFile())) if (k.hex == hex) return true;
    if (!policy || !policy->restricted) {
        std::error_code ec;
        if (projectTrustedKeys(std::filesystem::current_path(ec)).count(hex)) return true;
    }
    return false;
}

// "" when the module at `path` needs nothing beyond `declared`, else a message
inline std::string moduleNeedsMoreThan(const Interpreter& interp, const std::filesystem::path& path, uint64_t declared) {
    std::set<std::string> visiting;
    ModuleInfo mi;
    if (path.extension() == ".embr") {
        auto src = tryReadFile(path);
        if (!src) return "";
        mi = analyzeModule(*src, path.string(), interp.pluginPolicy, visiting, 0);
    } else if (path.extension() == ".vmbr") {
        auto cb = readPackageBytes(path);
        if (!cb) return "";
        mi = analyzeCompiledBytes(*cb, path.string(), interp.pluginPolicy, visiting, 0);
        if (mi.noMetadata) return "";
    } else return "";
    if (!mi.parsed) return "";
    if (uint64_t missing = mi.effectiveCaps() & ~declared) return "its module needs " + capListText(missing) + ", which the package does not declare";
    return "";
}

// what `name` is on this machine: a package, a loose plugin or a script module found the way `import` finds it (a restricted host only in
// the folders it controls), with its version. false if there is none
struct Provided { std::string kind, version, path; };
inline bool findProvided(const Interpreter& interp, const std::string& name, Provided& out) {
    namespace fs = std::filesystem;
    auto look = [&](const std::string& ext, const char* subdir) {
        std::vector<fs::path> c;
        if (interp.pluginPolicy.restricted) {
            for (const auto& d : extraDirs()) { c.push_back(d / (name + ext)); c.push_back(d / subdir / (name + ext)); }
        } else {
            c = fileCandidates(name, interp.scriptDir, ext, subdir);
        }
        return findExistingCandidate(c);
    };
    if (fs::path f = look(".pmbr", "packages"); !f.empty()) {
        PackageInfo pi = analyzePackageFile(f);
        if (!pi.parsed) return false;
        out = {"package", pi.meta.version, f.string()};
        return true;
    }
    if (fs::path f = look(PLUGIN_EXT, "plugins"); !f.empty()) {
        PluginFileInfo pi = readPluginFile(f);
        out = {"plugin", pi.version, f.string()};
        return pi.problem.empty();
    }
    if (fs::path f = look(".embr", "modules"); !f.empty()) {
        std::set<std::string> visiting;
        PluginPolicy policy;
        policy.restricted = true;
        ModuleInfo mi = analyzeModuleFile(f, policy, visiting);
        out = {"module", mi.version, f.string()};
        return mi.parsed;
    }
    return false;
}

// "" when everything the package requires is here at the exact version, else why not
inline std::string checkRequires(const Interpreter& interp, const PackageMeta& meta) {
    for (const auto& [name, want] : meta.requires_) {
        Provided have;
        if (!findProvided(interp, name, have)) return "it requires " + name + " " + want + ", which is not installed";
        if (have.version != want) return "it requires " + name + " " + want + " but " + have.path + " is " + (have.version.empty() ? std::string("a build with no version") : "version " + have.version);
    }
    return "";
}

// what importing a package does up to the point of running anything: read it, check what it declares against what the host denies and
// what it requires, unpack it, and record it so package_path() can find its files. `runModules` false: stop early for a module package
// (the caller only wants plugin signatures). on success `info`, `dir` and `entryPath` are set and `ready` is false only for that early stop
struct PreparedPackage {
    PackageInfo  info;
    std::string  bytes;
    std::filesystem::path dir, entryPath;
    bool         ready = false;
};

inline std::string preparePackage(Interpreter& interp, const std::filesystem::path& file, bool runModules, PreparedPackage& pp, bool needEntry = true, bool nameMustMatchFile = true) {
    namespace fs = std::filesystem;
    const std::string stem = file.stem().string();
    auto refuse = [&](const std::string& why) { return "package \"" + stem + "\" (" + file.string() + "): " + why; };
    auto bytes = readPackageBytes(file);
    if (!bytes) return refuse("cannot open the file");
    pp.bytes = std::move(*bytes);
    pp.info = analyzePackageBytes(pp.bytes, file.string());
    const PackageInfo& info = pp.info;
    if (!info.parsed || !info.problems.empty()) return refuse(info.problems.empty() ? "cannot read it" : info.problems.front());
    const PackageMeta& meta = info.meta;
    if (nameMustMatchFile && meta.name != stem) return refuse("the file is called " + stem + ".pmbr but the package calls itself \"" + meta.name + "\"");

    if (uint64_t hit = meta.caps & interp.pluginPolicy.deniedCaps) {
        std::string names;
        for (const auto& n : capNames(hit)) names += (names.empty() ? "" : ", ") + n;
        return "package \"" + meta.name + "\" is not allowed in this sandbox: it needs " + names;
    }
    if (interp.pluginPolicy.requireSigned) {
        const PackageSignature& sg = info.file.signature;
        if (!sg.present) return refuse("it is not signed, and this host requires signed packages (--require-signed)");
        if (!keyIsTrusted(&interp.pluginPolicy, sg.keyHex()))
            return refuse("it is signed by the key " + sg.keyHex().substr(0, 16) + "..., which is not trusted (embr trust, or trusted_keys in embr.json)");
    }
    if (interp.packages.count(meta.name)) { pp.ready = false; return ""; }          // already imported here
    if (!meta.module.empty() && !runModules) return "";                              // the caller only wants plugin signatures
    if (std::string why = checkRequires(interp, meta); !why.empty()) return refuse(why);

    std::string target = hostTarget(), entryLabel;
    if (!meta.plugin.empty()) {
        if (info.file.abi != EMBR_PLUGIN_ABI_VERSION)
            return refuse("its plugin was built for plugin interface " + std::to_string(info.file.abi) + ", this embr uses " + std::to_string(EMBR_PLUGIN_ABI_VERSION));
        auto it = meta.plugin.find(target);
        if (it == meta.plugin.end()) {
            std::string have;
            for (const auto& kv : meta.plugin) have += (have.empty() ? "" : ", ") + kv.first;
            return refuse("it has no build for " + target + " (it has " + have + ")");
        }
        // the build of this embr's C++ ABI, else a C build (any host takes one), else one that does not say which it is (the plugin
        // itself is checked when it loads)
        const auto& builds = it->second;
        for (const std::string& want : {std::string(abiTag()), std::string("c"), std::string()}) {
            auto b = builds.find(want);
            if (b != builds.end()) { entryLabel = b->second; break; }
        }
        if (entryLabel.empty()) {
            std::string have;
            for (const auto& b : builds) have += (have.empty() ? "" : ", ") + b.first;
            return refuse("it has no build for " + target + " that fits this embr: its C++ ABI is " + abiTag() + " (see `embr abi`), the package has " + have);
        }
    } else {
        entryLabel = meta.module;
    }

    std::string why;
    if (!unpackPackage(pp.bytes, info.file, meta.name, pp.dir, why)) return refuse(why);
    recordPackageSource(pp.dir, file);

    Interpreter::PackageRecord rec;
    rec.name = meta.name; rec.version = meta.version; rec.file = file.string(); rec.dir = pp.dir.string(); rec.id = info.file.id; rec.caps = meta.caps;
    for (const auto& e : info.file.entries) {
        unsigned char s[32];
        std::memcpy(s, e.sha, 32);
        rec.sha[e.label] = hexOf(s, 32);
    }
    const PackageEntry* entry = info.file.find(entryLabel);
    if (needEntry) {
        if (std::string bad = checkUnpackedFile(pp.dir, *entry); !bad.empty()) return refuse(bad);
        rec.checked.insert(entryLabel);
    }
    pp.entryPath = pp.dir / fs::path(entryLabel);
    interp.packages[meta.name] = std::move(rec);
    pp.ready = true;
    return "";
}

inline std::string importPackageFile(Interpreter& interp, const std::filesystem::path& file, bool runModules) {
    namespace fs = std::filesystem;
    const std::string stem = file.stem().string();
    auto refuse = [&](const std::string& why) { return "package \"" + stem + "\" (" + file.string() + "): " + why; };
    PreparedPackage pp;
    if (std::string err = preparePackage(interp, file, runModules, pp); !err.empty()) return err;
    if (!pp.ready) return "";
    const PackageMeta& meta = pp.info.meta;

    if (!meta.plugin.empty()) {
        PluginClaim claim{meta.name, meta.version, meta.caps};
        // preparePackage registered the package; a failed plugin load must not leave it registered
        std::string err = importPluginFrom(interp, meta.name, {pp.entryPath}, &claim);
        if (!err.empty()) { interp.packages.erase(meta.name); return refuse(err); }
        return "";
    }

    // a module: its imports must not need more than the package declares. then it runs like any module, from the unpacked folder
    {
        std::string missing = moduleNeedsMoreThan(interp, pp.entryPath, meta.caps);
        if (!missing.empty()) { interp.packages.erase(meta.name); return refuse(missing); }
    }
    if (!interp.backend.loadModule) { interp.packages.erase(meta.name); return refuse("no backend is ready to run a module"); }
    interp.backend.loadModule(interp, pp.entryPath.string(), SourceRange{}, /*mutateCallerScope=*/true, /*forceReload=*/false);
    return "";
}

// ---- what the command line and scripts show ----

inline std::string packageKind(const PackageInfo& p) { return !p.meta.module.empty() ? "module" : !p.meta.plugin.empty() ? "plugin" : "?"; }

inline std::string packageJson(const PackageInfo& p) {
    auto arr = [](const std::vector<std::string>& v) {
        std::string s = "[";
        for (size_t i = 0; i < v.size(); ++i) s += (i ? ", " : "") + jsonQuote(v[i]);
        return s + "]";
    };
    std::vector<std::string> caps = capNames(p.meta.caps), labels;
    for (const auto& e : p.file.entries) labels.push_back(e.label);
    std::string s = "{\"name\": " + jsonQuote(p.meta.name) + ", \"version\": " + jsonQuote(p.meta.version) + ", \"description\": " + jsonQuote(p.meta.description);
    s += ", \"path\": " + jsonQuote(p.path) + ", \"parsed\": " + (p.parsed ? "true" : "false") + ", \"kind\": " + jsonQuote(packageKind(p));
    s += ", \"capabilities\": " + arr(caps) + ", \"targets\": " + arr(p.targets) + ", \"builds\": " + arr(p.builds) + ", \"files\": " + arr(labels);
    s += ", \"entry\": " + jsonQuote(!p.meta.module.empty() ? p.meta.module : std::string());
    s += ", \"libraries\": " + arr(p.meta.libraries) + ", \"requires\": {";
    {
        bool first = true;
        for (const auto& kv : p.meta.requires_) { s += std::string(first ? "" : ", ") + jsonQuote(kv.first) + ": " + jsonQuote(kv.second); first = false; }
    }
    s += "}, \"abi\": " + std::to_string(p.file.abi) + ", \"id\": " + jsonQuote(p.file.id);
    s += ", \"signed\": " + std::string(p.file.signature.present ? "true" : "false") + ", \"signature_valid\": " + (p.file.signature.valid ? "true" : "false");
    s += ", \"signer\": " + jsonQuote(p.file.signature.present ? p.file.signature.keyHex() : std::string());
    s += ", \"trusted\": " + std::string(p.file.signature.valid && keyIsTrusted(nullptr, p.file.signature.keyHex()) ? "true" : "false");
    s += ", \"size\": " + std::to_string(p.size) + ", \"problems\": " + arr(p.problems) + "}";
    return s;
}

inline std::string packageText(const PackageInfo& p) {
    std::string s = p.meta.name + " " + (p.meta.version.empty() ? "(no version)" : p.meta.version) + "  " + p.meta.description + "\n";
    if (p.meta.name.empty()) s = p.path + "\n";
    s += "  a " + packageKind(p) + " package, " + std::to_string(p.size) + " bytes, " + std::to_string(p.file.entries.size()) + " files\n";
    s += "  needs: " + capListText(p.meta.caps) + "\n";
    if (!p.targets.empty()) {
        std::string t;
        for (const auto& x : p.targets) t += (t.empty() ? "" : ", ") + x;
        s += "  builds for: " + t + (std::find(p.targets.begin(), p.targets.end(), hostTarget()) == p.targets.end() ? "   (not this platform: " + hostTarget() + ")" : "") + "\n";
    }
    if (!p.builds.empty() && std::any_of(p.builds.begin(), p.builds.end(), [](const std::string& b) { return b.find('/') != std::string::npos; })) {
        std::string b;
        for (const auto& x : p.builds) b += (b.empty() ? "" : ", ") + x;
        s += "  builds: " + b + "   (this embr: " + hostTarget() + "/" + abiTag() + ")\n";
    }
    if (p.file.abi) s += "  plugin interface: " + std::to_string(p.file.abi) + "\n";
    if (!p.meta.module.empty()) s += "  entry point: " + p.meta.module + "\n";
    for (const auto& l : p.meta.libraries) s += "  bundles: " + l + "\n";
    for (const auto& kv : p.meta.requires_) s += "  requires: " + kv.first + " " + kv.second + "\n";
    if (p.file.signature.present)
        s += "  signed by: " + p.file.signature.keyHex() + (p.file.signature.valid ? (keyIsTrusted(nullptr, p.file.signature.keyHex()) ? "  (trusted)" : "  (not in your trust list)") : "  (THE SIGNATURE DOES NOT MATCH)") + "\n";
    else s += "  signed by: nobody\n";
    s += "  file: " + p.path + "\n  id: " + p.file.id + "\n";
    for (const auto& pr : p.problems) s += "  PROBLEM: " + pr + "\n";
    s += "\nfiles\n";
    for (const auto& e : p.file.entries) s += "  " + e.label + "  (" + std::to_string(e.length) + " bytes)\n";
    return s;
}

// the registry entry for a package file served at `url`, as JSON: what `embr pkg` reads from index.json (see modules/pkg.embr)
inline std::string packageRegistryEntry(const std::string& bytes, const PackageInfo& p, const std::string& url) {
    std::string s = "{\"url\": " + jsonQuote(url) + ", \"type\": \"package\", \"sha256\": " + jsonQuote(sha256Hex(bytes));
    s += ", \"description\": " + jsonQuote(p.meta.description) + ", \"capabilities\": [";
    bool first = true;
    for (const auto& n : capNames(p.meta.caps)) { s += std::string(first ? "" : ", ") + jsonQuote(n); first = false; }
    s += "]";
    if (!p.meta.requires_.empty()) {
        s += ", \"deps\": {";
        first = true;
        for (const auto& kv : p.meta.requires_) { s += std::string(first ? "" : ", ") + jsonQuote(kv.first) + ": " + jsonQuote(kv.second); first = false; }
        s += "}";
    }
    return s + "}";
}

// the packages in the places `import` looks, one per name, sorted
inline std::vector<PackageInfo> installedPackages() {
    namespace fs = std::filesystem;
    std::vector<PackageInfo> out;
    std::set<std::string> seen;
    std::vector<fs::path> dirs;
    for (const auto& d : extraDirs()) { dirs.push_back(d); dirs.push_back(d / "packages"); }
    dirs.push_back(fs::current_path() / "packages");
    for (const auto& d : dirs) {
        std::error_code ec;
        if (!fs::is_directory(d, ec)) continue;
        std::vector<fs::path> files;
        for (const auto& e : fs::directory_iterator(d, ec))
            if (e.is_regular_file(ec) && e.path().extension() == ".pmbr") files.push_back(e.path());
        std::sort(files.begin(), files.end());
        for (const auto& f : files) {
            if (!seen.insert(f.filename().string()).second) continue;
            out.push_back(analyzePackageFile(f));
            if (out.back().meta.name.empty()) out.back().meta.name = f.stem().string();
        }
    }
    std::sort(out.begin(), out.end(), [](const PackageInfo& a, const PackageInfo& b) { return a.meta.name < b.meta.name; });
    return out;
}

inline bool findPackageFile(const std::string& nameOrPath, std::filesystem::path& out) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (fs::is_regular_file(nameOrPath, ec)) { out = nameOrPath; return true; }
    fs::path found = findExistingCandidate(fileCandidates(nameOrPath, fs::current_path().string(), ".pmbr", "packages"));
    if (found.empty()) return false;
    out = found;
    return true;
}

// `embr pack`: the files of DIR as a package. returns "" and the file image, or why not. DIR holds meta.embr and everything else
inline std::string buildPackage(const std::filesystem::path& dir, std::string& image, std::vector<std::string>& notes) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return dir.string() + " is not a folder";
    std::vector<std::pair<std::string, std::string>> files;
    for (auto it = fs::recursive_directory_iterator(dir, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) return "cannot read " + dir.string() + ": " + ec.message();
        if (it->is_symlink(ec)) return it->path().string() + " is a link; a package holds files";
        if (!it->is_regular_file(ec)) continue;
        std::string label = fs::relative(it->path(), dir, ec).generic_string();
        std::string lw;
        if (!validLabel(label, &lw)) return "'" + label + "' cannot be a label (" + lw + ")";
        auto b = readPackageBytes(it->path());
        if (!b) return "cannot read " + it->path().string();
        files.emplace_back(label, std::move(*b));
    }
    if (files.size() > kPmbrMaxEntries) return "too many files";
    const std::string* metaText = nullptr;
    for (const auto& f : files) if (f.first == kPackageMetaLabel) metaText = &f.second;
    if (!metaText) return std::string("there is no ") + kPackageMetaLabel + " in " + dir.string();
    PackageMeta meta = parsePackageMeta(*metaText);
    uint32_t abi = meta.plugin.empty() ? 0 : EMBR_PLUGIN_ABI_VERSION;
    std::string image0 = writePackage(abi, meta.version, files);
    PackageInfo info = analyzePackageBytes(image0, dir.string());
    if (!info.parsed || !info.problems.empty()) return info.problems.empty() ? "meta.embr does not parse" : info.problems.front();
    // what the entry point needs must be within what meta declares
    if (!meta.module.empty()) {
        fs::path mp = dir / fs::path(meta.module);
        if (mp.extension() == ".embr") {
            auto src = tryReadFile(mp);
            PluginPolicy policy;
            std::set<std::string> visiting;
            ModuleInfo mi = analyzeModule(*src, mp.string(), policy, visiting, 0);
            if (!mi.parsed) return "the module " + meta.module + " does not parse";
            for (const auto& p : mi.problems) return "the module " + meta.module + ": " + p.text;
            if (uint64_t missing = mi.effectiveCaps() & ~meta.caps)
                return "the module " + meta.module + " needs " + capListText(missing) + ", add it to \"capabilities\" in meta.embr";
            if (mi.dynamicImports) notes.push_back("the module has a computed import: embr cannot follow it, so check \"capabilities\" by hand");
        }
    }
    // the libraries for this platform can be opened here (their description is read, nothing is run but embr_plugin_info): each must be the
    // plugin meta.embr describes, and the C++ ABI it says it has must be the tag meta.embr gives that build. a library of another ABI
    // than this embr can be read too, it just cannot be loaded here
    auto self = meta.plugin.find(hostTarget());
    if (self != meta.plugin.end()) {
        for (const auto& [tag, label] : self->second) {
            PluginFileInfo pi = readPluginFile(dir / fs::path(label));
            if (!pi.declared) return "the library '" + label + "': " + (pi.problem.empty() ? "it does not describe itself" : pi.problem);
            const std::string theirs = pi.cxx ? pi.abiTag : std::string("c");
            if (theirs.empty()) return "the library '" + label + "' is a C++ plugin that does not say which C++ ABI it has, rebuild it with this embr's embr.h";
            if (!tag.empty() && tag != theirs)
                return "the library '" + label + "' has the C++ ABI " + theirs + ", but meta.embr lists it as " + tag;
            if (tag.empty()) notes.push_back("the build for " + hostTarget() + " ('" + label + "') does not say its C++ ABI; meta.embr can list it as \"" + theirs + "\"");
            if (pi.cxx && theirs != abiTag()) notes.push_back("the library '" + label + "' has another C++ ABI than this embr (" + theirs + "), so it was read but could not be loaded here");
            PluginClaim claim{meta.name, meta.version, meta.caps};
            EmbrPluginInfo fake{};
            fake.struct_size = sizeof fake;
            fake.name = pi.name.c_str(); fake.version = pi.version.c_str();
            fake.capabilities = pi.caps; fake.optional_capabilities = pi.optionalCaps;
            std::string why = checkPluginClaim(&claim, &fake);
            if (!why.empty()) return why;
        }
    } else if (!meta.plugin.empty()) {
        notes.push_back("no build for " + hostTarget() + " here, so the libraries were not checked against meta.embr");
    }
    image = std::move(image0);
    return "";
}

} // namespace embr

#endif // EMBR_CORE_PACKAGEIMPORT_H
