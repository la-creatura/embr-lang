// package.cpp
// lets the entry point of a package (core/package.h) find the other files that came in it: a library it wraps, data it reads.
//
// usage
//   import "package"
//   lib = package_path("whatever", "lib/{target}/libwhatever.so")     # in the module of the package "whatever"
//
// good to know
//   - a package is unpacked once into a folder named by its id, each file where its label says. package_path checks the file
//     against the sha256 in the package before it gives the path, so a file changed after unpacking is not handed out
//   - `{target}` in a label is the platform this embr runs on, like "linux-x86_64"
//   - the package must have been imported already (its own entry point runs after that, so a module can ask in its first line)

#include <embr/embr.h>
#include <fstream>

using namespace embr;

static Param pStr(std::string n) { return Param::req(std::move(n), TS::Str); }

[[noreturn]] static void fail(const std::string& fn, const std::string& msg) { raiseError(fn, msg); }

static Interpreter::PackageRecord& loaded(Interpreter& interp, const std::string& fn, const std::string& name) {
    auto it = interp.packages.find(name);
    if (it == interp.packages.end()) fail(fn, "the package '" + name + "' has not been imported (import \"" + name + "\" first)");
    return it->second;
}

EMBR_PLUGIN {
    // package_dir(name: str) -> str
    interp->bindSpan("package_dir", {pStr("name")},
    [interp](const ArgSpan& args) -> Value {
        return Value(loaded(*interp, "package_dir", args[0].asString()).dir);
    });

    // package_path(name: str, label: str) -> str
    interp->bindSpan("package_path", {pStr("name"), pStr("label")},
    [interp](const ArgSpan& args) -> Value {
        auto& rec = loaded(*interp, "package_path", args[0].asString());
        const std::string label = expandTarget(args[1].asString(), hostTarget());
        std::string why;
        if (!validLabel(label, &why)) fail("package_path", "'" + label + "' is not a file name inside a package (" + why + ")");
        auto it = rec.sha.find(label);
        if (it == rec.sha.end()) fail("package_path", "the package '" + rec.name + "' has no file '" + label + "'");
        std::filesystem::path file = std::filesystem::path(rec.dir) / std::filesystem::path(label);
        if (!rec.checked.count(label)) {
            auto bytes = readPackageBytes(file);
            if (!bytes) fail("package_path", "the file " + file.string() + " is gone");
            unsigned char d[32];
            sha256(bytes->data(), bytes->size(), d);
            if (hexOf(d, 32) != it->second) fail("package_path", "the file " + file.string() + " was changed after it was unpacked");
            rec.checked.insert(label);
        }
        return Value(file.string());
    });

    // package_files(name: str) -> arr of str
    interp->bindSpan("package_files", {pStr("name")},
    [interp](const ArgSpan& args) -> Value {
        auto& rec = loaded(*interp, "package_files", args[0].asString());
        Value::array_type out;
        for (const auto& kv : rec.sha) out.push_back(Value(kv.first));
        return Value(std::move(out));
    });

    interp->setResultTypes({{"package_dir", TS::Str}, {"package_path", TS::Str}, {"package_files", TS::Arr}});
    interp->setResultElems({{"package_files", TS::Str}});
}

EMBR_PLUGIN_INFO("package", "1.0.0", "the files that came in a package: where they were unpacked", EMBR_CAP_NONE, EMBR_CAP_NONE,
    {"package_dir", "the folder the package was unpacked into. raises if the package was not imported"},
    {"package_files", "the labels of every file in the package, sorted"},
    {"package_path", "where one file of the package is, after checking it against the package's sha256. {target} in the label is this platform. raises if there is no such file or it changed"});
