// packaging.cpp
// aembr package, info and verify. see packaging.h.
//
// usage
//   aembr package FILE [-o OUT.pmbr] [--sign NAME.key]      compile FILE as a plugin and pack it with its source and its build record
//   aembr info FILE                                          what made a program, a plugin or a package, read without running it
//   aembr verify PKG.pmbr                                    check a package against its own source (the claim, then the bytes)
//
// good to know
//   - a package holds: meta.embr, lib/<platform>/<name>.so (.dylib on macOS), src/<the source file> and aembr/build.json (the record, record.h)
//   - verify has two levels. the claim: the capabilities worked out again from the bundled source must be inside what the package declares.
//     the bytes: when this machine has the same aembr, C compiler and runtime as the record, the library is built again and must have the same
//     sha256. a different toolchain is said, and is exit code 2, not 1

#include "packaging.h"

#include "pipeline.h"

#include <embr/embr.h>

#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iostream>

namespace fs = std::filesystem;

namespace aembr {

namespace {

std::string tempDir(const char* prefix) {
    const char* t = std::getenv("TMPDIR");
    std::string tpl = std::string(t && *t ? t : "/tmp") + "/" + prefix + "-XXXXXX";
    std::vector<char> buf(tpl.begin(), tpl.end());
    buf.push_back(0);
    return ::mkdtemp(buf.data()) ? std::string(buf.data()) : std::string();
}

bool writeFile(const fs::path& p, const std::string& bytes) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), (std::streamsize)bytes.size());
    f.close();
    return (bool)f;
}

std::string quoteEmbr(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (c == '\n') o += "\\n";
        else if (c == '\t') o += "\\t";
        else o += c;
    }
    return o + "\"";
}

struct TempGuard {
    std::string path;
    ~TempGuard() { if (!path.empty()) { std::error_code ec; fs::remove_all(path, ec); } }
};

std::string readAll(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), {});
}

std::string capsText(const std::vector<std::string>& caps) {
    if (caps.empty()) return "none";
    std::string s;
    for (size_t i = 0; i < caps.size(); ++i) s += (i ? ", " : "") + caps[i];
    return s;
}

void printRecord(const BuildRecord& r) {
    std::cout << "  made by aembr " << r.aembr << " for " << r.target << " (" << r.kind << (r.name.empty() ? "" : " " + r.name) << ")\n"
              << "  source: " << r.source << " (sha256 " << r.sourceSha256 << ")\n"
              << "  needs: " << capsText(r.capabilities) << "\n"
              << "  compiler: " << (r.cc.empty() ? "unknown" : r.cc) << "\n"
              << "  flags: " << r.flags << "\n"
              << "  runtime sha256: " << r.runtimeSha256 << "\n";
}

// the label of the library for this platform in a package, "" when there is none
std::string libraryLabel(const embr::PackageMeta& meta) {
    auto it = meta.plugin.find(embr::hostTarget());
    if (it == meta.plugin.end()) return "";
    auto c = it->second.find("c");
    if (c != it->second.end()) return c->second;
    auto plain = it->second.find("");
    return plain != it->second.end() ? plain->second : it->second.begin()->second;
}

}  // namespace

int packageCommand(const Cli& c) {
    if (c.in.empty()) { std::cerr << "usage: aembr package FILE [-o OUT.pmbr] [--sign NAME.key] [--cc=CC] [--deny-caps=LIST]\n"; return 2; }
    fs::path inPath(c.in);
    const std::string out = c.out.empty() ? (inPath.parent_path() / (inPath.stem().string() + ".pmbr")).string() : c.out;
    const std::string name = fs::path(out).stem().string();
    unsigned char seed[32];
    std::string why;
    if (!c.signKey.empty() && !embr::readKeyFile(c.signKey, /*secret=*/true, seed, why)) { std::cerr << "aembr: " << why << "\n"; return 1; }

    CompileRequest rq;
    rq.srcPath = c.in; rq.plugin = true; rq.pluginName = name; rq.bo = c.bo; rq.denyCaps = c.denyCaps; rq.exeDir = c.exeDir;
    Compiled comp;
    std::string err;
    if (int rc = compileSource(rq, comp, err)) { std::cerr << err; return rc; }

    TempGuard stage{tempDir("aembr-pkg")};
    if (stage.path.empty()) { std::cerr << "aembr: cannot make a temp folder\n"; return 2; }
    const std::string target = embr::hostTarget();
    const std::string libLabel = "lib/" + target + "/" + name + embr::PLUGIN_EXT;
    BuildOptions bo = c.bo;
    bo.shared = true;
    std::error_code mk;
    fs::create_directories(fs::path(stage.path + "/" + libLabel).parent_path(), mk);
    if (!buildExecutable(comp.c, stage.path + "/" + libLabel, bo, c.exeDir, err)) { std::cerr << err; return 2; }

    std::string caps;
    for (const std::string& n : embr::capNames(comp.meta.caps)) caps += std::string(caps.empty() ? "" : ", ") + quoteEmbr(n);
    const std::string meta = "__package__ = {\n    \"name\": " + quoteEmbr(name) + ",\n    \"version\": " + quoteEmbr(comp.meta.version) +
                             ",\n    \"description\": " + quoteEmbr(comp.meta.description) + ",\n    \"capabilities\": [" + caps +
                             "],\n    \"plugin\": {" + quoteEmbr(target) + ": {\"c\": " + quoteEmbr(libLabel) + "}},\n}\n";
    const std::string srcLabel = "src/" + comp.rec.source;
    for (const auto& m : comp.modules) {
        if (m.first.rfind("..", 0) == 0 || (!m.first.empty() && m.first[0] == '/')) {
            std::cerr << "aembr package: the module " << m.second << " is not in the folder of " << c.in << " or below it, so it cannot be packed with it\n";
            return 1;
        }
        if (!writeFile(stage.path + "/src/" + m.first, readAll(m.second))) { std::cerr << "aembr: cannot write the package folder\n"; return 2; }
    }
    if (!writeFile(stage.path + "/meta.embr", meta) || !writeFile(stage.path + "/" + srcLabel, readAll(c.in)) ||
        !writeFile(stage.path + "/aembr/build.json", recordJson(comp.rec) + "\n")) {
        std::cerr << "aembr: cannot write the package folder\n";
        return 2;
    }

    std::string image;
    std::vector<std::string> notes;
    why = embr::buildPackage(stage.path, image, notes);
    if (!why.empty()) { std::cerr << "aembr package: " << why << "\n"; return 1; }
    if (!c.signKey.empty()) {
        std::string signedImage;
        if (!embr::signPackageBytes(image, seed, signedImage, why)) { std::cerr << "aembr package: " << why << "\n"; return 1; }
        image = std::move(signedImage);
    }
    const std::string tmp = out + ".tmp";
    if (!writeFile(tmp, image)) { std::cerr << "aembr: cannot write " << out << "\n"; return 2; }
    std::error_code ec;
    fs::rename(tmp, out, ec);
    if (ec) { std::cerr << "aembr: cannot write " << out << ": " << ec.message() << "\n"; return 2; }
    for (const std::string& n : notes) std::cerr << "aembr package: note: " << n << "\n";
    embr::PackageInfo info = embr::analyzePackageBytes(image, out);
    std::cout << "wrote " << out << " (" << info.meta.name << " " << info.meta.version << ", " << info.file.entries.size() << " files, " << image.size() << " bytes)\n";
    return 0;
}

int infoCommand(const Cli& c) {
    if (c.in.empty()) { std::cerr << "usage: aembr info FILE      (a program, a plugin or a package made by aembr)\n"; return 2; }
    const std::string bytes = readAll(c.in);
    if (bytes.empty()) { std::cerr << "aembr: cannot read " << c.in << "\n"; return 2; }
    if (bytes.compare(0, 4, "PMBR") == 0) {
        embr::PackageInfo info = embr::analyzePackageBytes(bytes, c.in);
        std::cout << c.in << ": package " << info.meta.name << " " << info.meta.version << "\n  " << info.meta.description << "\n  needs: "
                  << capsText(embr::capNames(info.meta.caps)) << "\n  platforms: ";
        for (size_t i = 0; i < info.targets.size(); ++i) std::cout << (i ? ", " : "") << info.targets[i];
        std::cout << (info.targets.empty() ? "none" : "") << "\n  signed by: ";
        if (!info.file.signature.present) std::cout << "nobody\n";
        else std::cout << info.file.signature.keyHex() << (info.file.signature.valid ? "" : " (the signature does not match)") << "\n";
        for (const std::string& p : info.problems) std::cout << "  problem: " << p << "\n";
        const embr::PackageEntry* e = info.file.find("aembr/build.json");
        std::string json;
        BuildRecord r;
        if (e && embr::packageEntryBytes(bytes, *e, json) && parseRecord(json, r)) { std::cout << "build record (aembr/build.json):\n"; printRecord(r); }
        else std::cout << "  no aembr build record: this package was not made by aembr\n";
        return info.problems.empty() ? 0 : 1;
    }
    std::string json;
    BuildRecord r;
    if (!findRecord(bytes, json) || !parseRecord(json, r)) { std::cerr << c.in << ": no aembr record (it was not made by aembr, or by an older one)\n"; return 1; }
    std::cout << c.in << ":\n";
    printRecord(r);
    return 0;
}

int verifyCommand(const Cli& c) {
    if (c.in.empty()) { std::cerr << "usage: aembr verify PKG.pmbr\n"; return 2; }
    const std::string bytes = readAll(c.in);
    embr::PackageInfo info = embr::analyzePackageBytes(bytes, c.in);
    if (!info.parsed || !info.problems.empty()) {
        std::cerr << c.in << ": the package is not sound:\n";
        for (const std::string& p : info.problems) std::cerr << "  " << p << "\n";
        return 1;
    }
    const embr::PackageEntry* be = info.file.find("aembr/build.json");
    std::string json;
    BuildRecord rec;
    if (!be || !embr::packageEntryBytes(bytes, *be, json) || !parseRecord(json, rec)) {
        std::cerr << c.in << ": no aembr build record, so there is nothing to check it against (it was not made by aembr)\n";
        return 2;
    }
    const embr::PackageEntry* se = info.file.find("src/" + rec.source);
    std::string source;
    if (!se || !embr::packageEntryBytes(bytes, *se, source)) { std::cerr << c.in << ": the source " << rec.source << " is not in the package\n"; return 1; }

    // 1. the claim: what the source needs has to be inside what the package says it needs
    TempGuard work{tempDir("aembr-verify")};
    if (work.path.empty()) { std::cerr << "aembr: cannot make a temp folder\n"; return 2; }
    const std::string srcPath = work.path + "/src/" + rec.source;
    if (!writeFile(srcPath, source)) { std::cerr << "aembr: cannot write a temp file\n"; return 2; }
    for (const embr::PackageEntry& pe : info.file.entries) {       // the modules the source imports
        if (pe.label.rfind("src/", 0) != 0 || pe.label == "src/" + rec.source) continue;
        std::string text;
        if (!embr::packageEntryBytes(bytes, pe, text) || !writeFile(work.path + "/" + pe.label, text)) { std::cerr << "aembr: cannot write a temp file\n"; return 2; }
    }
    BuildOptions bo = c.bo;
    bo.shared = true;
    const size_t o = rec.flags.find("-O");
    if (o != std::string::npos && o + 2 < rec.flags.size() && rec.flags[o + 2] >= '0' && rec.flags[o + 2] <= '3') bo.opt = rec.flags[o + 2] - '0';
    bo.infer = rec.flags.find("--infer") != std::string::npos;
    {   // the flags the plugin was built with that reach the C compiler
        std::istringstream words(rec.flags);
        for (std::string w; words >> w;) {
            if (w.size() > 2 && (w.rfind("-l", 0) == 0 || w.rfind("-L", 0) == 0)) bo.libs.push_back(w);
            else if (w.size() > 2 && (w.rfind("-I", 0) == 0 || w.rfind("-D", 0) == 0)) bo.cflags.push_back(w);
        }
    }
    CompileRequest rq;
    rq.srcPath = srcPath; rq.plugin = true; rq.pluginName = rec.name; rq.bo = bo; rq.exeDir = c.exeDir;
    Compiled comp;
    std::string err;
    if (int rc = compileSource(rq, comp, err)) {
        std::cerr << "cannot check the claim: the bundled source does not compile here:\n" << err;
        return rc == 1 ? 2 : rc;
    }
    const uint64_t needed = comp.meta.caps, declared = info.meta.caps;
    bool bad = false;
    if (needed & ~declared) {
        std::cout << "claim: WRONG. the source needs " << capsText(embr::capNames(needed & ~declared)) << ", which the package does not declare\n";
        bad = true;
    } else {
        std::cout << "claim: ok. the source needs " << capsText(embr::capNames(needed)) << ", the package declares " << capsText(embr::capNames(declared)) << "\n";
    }
    BuildRecord now = comp.rec;
    if (now.capabilities != rec.capabilities) {
        std::cout << "claim: WRONG. the build record says the program needs " << capsText(rec.capabilities) << ", the source needs " << capsText(now.capabilities) << "\n";
        bad = true;
    }
    if (now.sourceSha256 != rec.sourceSha256) {
        std::cout << "claim: WRONG. the bundled source is not the one the record was made from\n";
        bad = true;
    }

    // 2. the bytes: the same toolchain builds the same library
    const std::string label = libraryLabel(info.meta);
    const embr::PackageEntry* le = label.empty() ? nullptr : info.file.find(label);
    for (const std::string& t : info.targets)
        if (t != embr::hostTarget()) std::cout << "bytes: " << t << " not checked (this machine is " << embr::hostTarget() << ")\n";
    std::string differs;
    if (rec.aembr != kAembrVersion) differs += "aembr " + rec.aembr + " (this is " + kAembrVersion + "), ";
    if (rec.target != embr::hostTarget()) differs += "platform " + rec.target + ", ";
    if (rec.cc != now.cc) differs += "compiler '" + rec.cc + "' (this machine has '" + now.cc + "'), ";
    if (rec.runtimeSha256 != now.runtimeSha256) differs += "another runtime library, ";
    if (!le) {
        std::cout << "bytes: not checked. there is no library for " << embr::hostTarget() << " in the package\n";
        return bad ? 1 : 2;
    }
    if (!differs.empty()) {
        differs.resize(differs.size() - 2);
        std::cout << "bytes: not checked. it needs " << differs << "\n";
        return bad ? 1 : 2;
    }
    std::string packaged;
    if (!embr::packageEntryBytes(bytes, *le, packaged)) {
        std::cout << "bytes: WRONG. the library " << label << " in the file does not match its hash in the table of contents\n";
        return 1;
    }
    // the same file name as in the package: on macOS the name is written into the library
    const std::string rebuilt = work.path + "/" + fs::path(label).filename().string();
    if (!buildExecutable(comp.c, rebuilt, bo, c.exeDir, err)) { std::cerr << err; return 2; }
    unsigned char d[32];
    const std::string lib = readAll(rebuilt);
    embr::sha256(lib.data(), lib.size(), d);
    if (std::memcmp(d, le->sha, 32) != 0) {
        std::cout << "bytes: WRONG. a build of the bundled source with the recorded toolchain gives " << embr::hexOf(d, 32) << ", the package has "
                  << embr::hexOf(le->sha, 32) << "\n";
        return 1;
    }
    std::cout << "bytes: ok. building the bundled source again gives the library in the package (sha256 " << embr::hexOf(d, 32) << ")\n";
    return bad ? 1 : 0;
}

}  // namespace aembr
