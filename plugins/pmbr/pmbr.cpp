// pmbr.cpp
// reads and unpacks `.pmbr` package files from a script: the header, what the package says about itself, and the folder its
// files are unpacked into. nothing in a package is run.
//
// usage
//   import "pmbr"
//   m = pmbr_meta("mylib.pmbr")                     # name, version, capabilities, ...
//   dir = pmbr_unpack("mylib.pmbr")                 # the folder with its files
//   lib = load_module(dir + "/mylib.embr")          # the module's globals as a map, like any module file
//
// good to know
//   - pmbr_unpack does what `import` does before it runs anything: the table of contents and every file are checked against their
//     sha256, a capability the host denies refuses the package, and the files go to the same cache folder `import` uses. it also
//     registers the package, so package_path() in the `package` plugin finds its files
//   - unlike `import`, the file may have any name, not only <package>.pmbr

#include <embr/embr.h>

using namespace embr;

static Param pStr(std::string n) { return Param::req(std::move(n), TS::Str); }

[[noreturn]] static void fail(const std::string& fn, const std::string& msg) { raiseError(fn, msg); }

static Value strs(const std::vector<std::string>& v) {
    Value::array_type a;
    for (const auto& x : v) a.push_back(Value(x));
    return Value(std::move(a));
}

EMBR_PLUGIN {
    // pmbr_header(path: str) -> map
    interp->bindSpan("pmbr_header", {pStr("path")},
    [](const ArgSpan& args) -> Value {
        const std::string& path = args[0].asString();
        auto bytes = readPackageBytes(path);
        if (!bytes) fail("pmbr_header", "cannot open '" + path + "'");
        PackageFile pf;
        std::string why;
        if (!readPackageFile(*bytes, pf, why)) fail("pmbr_header", "'" + path + "': " + why);
        Value::map_type m;
        m["format"] = Value((int64_t)pf.format);
        m["abi"] = Value((int64_t)pf.abi);
        m["version"] = Value(pf.version);
        m["id"] = Value(pf.id);
        m["size"] = Value((int64_t)bytes->size());
        Value::array_type entries;
        for (const auto& e : pf.entries) {
            Value::map_type em;
            em["label"] = Value(e.label);
            em["offset"] = Value((int64_t)e.offset);
            em["length"] = Value((int64_t)e.length);
            em["sha256"] = Value(hexOf(e.sha, 32));
            entries.push_back(Value(std::move(em)));
        }
        m["entries"] = Value(std::move(entries));
        return Value(std::move(m));
    });

    // pmbr_meta(path: str) -> map
    interp->bindSpan("pmbr_meta", {pStr("path")},
    [interp](const ArgSpan& args) -> Value {
        const std::string& path = args[0].asString();
        PackageInfo p = analyzePackageFile(path);
        if (!p.parsed && p.problems.empty()) fail("pmbr_meta", "cannot read '" + path + "'");
        std::vector<std::string> files;
        for (const auto& e : p.file.entries) files.push_back(e.label);
        Value::map_type req;
        for (const auto& kv : p.meta.requires_) req[kv.first] = Value(kv.second);
        Value::map_type m;
        m["name"] = Value(p.meta.name);
        m["version"] = Value(p.meta.version);
        m["description"] = Value(p.meta.description);
        m["kind"] = Value(packageKind(p));
        m["capabilities"] = strs(capNames(p.meta.caps));
        m["targets"] = strs(p.targets);
        m["builds"] = strs(p.builds);
        m["files"] = strs(files);
        m["libraries"] = strs(p.meta.libraries);
        m["requires"] = Value(std::move(req));
        m["abi"] = Value((int64_t)p.file.abi);
        m["id"] = Value(p.file.id);
        m["size"] = Value((int64_t)p.size);
        m["signed"] = Value(p.file.signature.present ? (int64_t)1 : (int64_t)0);
        m["signer"] = Value(p.file.signature.present ? p.file.signature.keyHex() : std::string());
        m["trusted"] = Value(p.file.signature.valid && keyIsTrusted(&interp->pluginPolicy, p.file.signature.keyHex()) ? (int64_t)1 : (int64_t)0);
        m["problems"] = strs(p.problems);
        return Value(std::move(m));
    });

    // pmbr_unpack(path: str) -> str
    interp->bindSpan("pmbr_unpack", {pStr("path")},
    [interp](const ArgSpan& args) -> Value {
        const std::string& path = args[0].asString();
        PreparedPackage pp;
        std::string err = preparePackage(*interp, path, /*runModules=*/true, pp, /*needEntry=*/false, /*nameMustMatchFile=*/false);
        if (!err.empty()) fail("pmbr_unpack", err);
        // already imported (or unpacked) here: the folder it went to
        auto it = interp->packages.find(pp.info.meta.name);
        if (it == interp->packages.end()) fail("pmbr_unpack", "the package '" + pp.info.meta.name + "' could not be unpacked");
        return Value(it->second.dir);
    });

    interp->setResultTypes({{"pmbr_header", TS::Map}, {"pmbr_meta", TS::Map}, {"pmbr_unpack", TS::Str}});
}

EMBR_PLUGIN_INFO("pmbr", "1.0.0", "read and unpack .pmbr package files", EMBR_CAP_FILESYSTEM, EMBR_CAP_NONE,
    {"pmbr_header", "the table of contents of a package file: format, abi, version, id, size and entries (label, offset, length, sha256). raises if the file is not a package or is damaged"},
    {"pmbr_meta", "what a package says about itself: name, version, description, kind (module or plugin), capabilities, targets, builds (platform/C++ ABI tag), files, libraries, requires, abi, id, size, signed, signer, trusted and problems. nothing is unpacked or run, safe on a downloaded file"},
    {"pmbr_unpack", "checks the package like import does, writes its files to the cache folder and gives that folder. nothing in it is run. raises if the package is damaged or needs a capability the host denies"});
