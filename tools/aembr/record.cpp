// record.cpp
// see record.h.

#include "record.h"

#include <cstdio>

namespace aembr {

namespace {

std::string esc(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c < 32) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o;
}

// the string value of "key" in a flat json object written by recordJson
bool getString(const std::string& j, const std::string& key, std::string& out) {
    const std::string pat = "\"" + key + "\":\"";
    size_t at = j.find(pat);
    if (at == std::string::npos) return false;
    at += pat.size();
    out.clear();
    while (at < j.size() && j[at] != '"') {
        if (j[at] == '\\' && at + 1 < j.size()) {
            ++at;
            if (j[at] == 'u' && at + 4 < j.size()) { out += (char)std::stoi(j.substr(at + 1, 4), nullptr, 16); at += 4; }
            else out += j[at];
        } else out += j[at];
        ++at;
    }
    return at < j.size();
}

}  // namespace

std::string recordJson(const BuildRecord& r) {
    std::string caps;
    for (size_t i = 0; i < r.capabilities.size(); ++i) caps += std::string(i ? "," : "") + "\"" + esc(r.capabilities[i]) + "\"";
    return "{\"aembr\":\"" + esc(r.aembr) + "\",\"kind\":\"" + esc(r.kind) + "\",\"name\":\"" + esc(r.name) + "\",\"source\":\"" + esc(r.source) +
           "\",\"source_sha256\":\"" + esc(r.sourceSha256) + "\",\"capabilities\":[" + caps + "],\"cc\":\"" + esc(r.cc) + "\",\"flags\":\"" + esc(r.flags) +
           "\",\"runtime_sha256\":\"" + esc(r.runtimeSha256) + "\",\"target\":\"" + esc(r.target) + "\"}";
}

bool parseRecord(const std::string& j, BuildRecord& out) {
    bool ok = getString(j, "aembr", out.aembr) && getString(j, "kind", out.kind) && getString(j, "name", out.name) &&
              getString(j, "source", out.source) && getString(j, "source_sha256", out.sourceSha256) && getString(j, "cc", out.cc) &&
              getString(j, "flags", out.flags) && getString(j, "runtime_sha256", out.runtimeSha256) && getString(j, "target", out.target);
    if (!ok) return false;
    out.capabilities.clear();
    size_t a = j.find("\"capabilities\":[");
    if (a == std::string::npos) return false;
    a += 16;
    const size_t b = j.find(']', a);
    if (b == std::string::npos) return false;
    std::string list = j.substr(a, b - a), cur;
    bool in = false;
    for (char c : list) {
        if (c == '"') { if (in) { out.capabilities.push_back(cur); cur.clear(); } in = !in; }
        else if (in) cur += c;
    }
    return true;
}

// built from pieces, so this file's own text does not hold the whole marker (`aembr info aembr` would find it)
std::string recordBegin() { return std::string("@@aembr-") + "record-v1@@\n"; }
std::string recordEnd() { return std::string("\n@@end-") + "record@@"; }

bool findRecord(const std::string& bytes, std::string& json) {
    const std::string b = recordBegin(), e = recordEnd();
    size_t at = bytes.find(b);
    if (at == std::string::npos) return false;
    at += b.size();
    const size_t end = bytes.find(e, at);
    if (end == std::string::npos) return false;
    json = bytes.substr(at, end - at);
    return true;
}

}  // namespace aembr
