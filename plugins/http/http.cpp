// http.cpp
// HTTP(S) client plugin for embr, built on the vendored single-header cpp-httplib (include/httplib/httplib.h,
// MIT, v0.58.0). TLS is security-sensitive, so we wrap a well-reviewed library instead of writing sockets by hand
//
// usage
//   import "http"
//   r = http_get("http://localhost:8000/pkgs/index.json")
//   print(r["status"])   # 200
//   print(r["body"])
//
//   r = http_get("https://api.github.com/repos/owner/repo/releases/latest")
//
//   r = http_post("http://localhost:8000/echo", "hello", {"Content-Type": "text/plain"})
//
//   r = http_request("PUT", "http://localhost:8000/thing", {}, "payload")
//
//   r = http_download("https://github.com/owner/repo/releases/download/v1.0.0/asset.so", "cache/asset.so")
//   print(r["bytes"])
//
//   print(http_sha256("hello"))   # hex digest, to check a downloaded file against a published checksum (see modules/pkg.embr)
//
// every call returns a map {"status": num, "ok": num, "headers": map<str,str>, "body": str}
// (http_download returns the same without the body). "ok" is 1 for 2xx/3xx and 0 otherwise, so look at "status"
// for exact handling. connection failures, TLS failures and bad responses raise, like every other plugin
//
// good to know
//   - redirects (3xx + Location) are followed, up to 10 hops. this matters for GitHub release downloads, which
//     302 to a signed S3 url. pass {"follow": 0} in opts to get the redirect response itself (the target is in
//     its "headers"["location"])
//   - https:// needs a build where OpenSSL was found (see plugins/CMakeLists.txt; windows uses the copy in include/openssl). without it, https:// raises a clear error
//   - https:// checks the certificate and hostname against the system CA store. opts for http_request/http_download:
//       {"verify": 0}            accept any certificate (local testing only, the connection can be intercepted)
//       {"ca_file": "path.pem"}  trust this PEM bundle instead (pin a private CA without turning verification off)
//     (http_get/http_post take no opts map, use http_request)
//   - a response bigger than 64 MiB raises instead of filling memory. opts.max_bytes changes the limit (0 = none). http_download streams to the file and has no limit unless you give one
//   - 10 second socket timeout for connect/read/write. override it with {"timeout": seconds} in http_request opts
//   - every call blocks the calling thread until it finishes. plugins don't depend on each other, so this one doesn't use async

#include <embr/embr.h>

#ifdef _WIN32
#  define CPPHTTPLIB_NO_EXCEPTIONS 0
#endif
// the redirect cap this file's header documents ("up to 10 hops"); httplib's own default is 20
#ifndef CPPHTTPLIB_REDIRECT_MAX_COUNT
#  define CPPHTTPLIB_REDIRECT_MAX_COUNT 10
#endif
#include <httplib.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace embr;

static Param pStr(std::string n)                      { return Param::req(std::move(n), TS::Str); }
static Param pOptStr(std::string n)                    { return Param::opt(std::move(n), TS::Str); }
static Param pOptMap(std::string n)                    { return Param::opt(std::move(n), TS::Map); }

static void throwError(const std::string& fn, const std::string& msg) {
    raiseError("[http:" + fn + "]", msg);
}

namespace {

constexpr int64_t kDefaultMaxBody = 64LL * 1024 * 1024;

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

struct UrlParts {
    std::string scheme;
    std::string schemeHostPort; // e.g. "https://api.github.com" or "http://localhost:8931"
    std::string target;         // path + query, always starts with '/'
};

// splits "scheme://host[:port][/path[?query]]" into what httplib::Client wants (scheme + authority) and what
// Get/Post/send want (path + query). httplib validates the authority itself, so this only finds the boundary
UrlParts splitUrl(const std::string& fn, const std::string& raw) {
    auto schemeEnd = raw.find("://");
    if (schemeEnd == std::string::npos)
        throwError(fn, "invalid URL (missing scheme): " + raw);

    UrlParts u;
    u.scheme = toLower(raw.substr(0, schemeEnd));

    std::string rest = raw.substr(schemeEnd + 3);
    auto pathStart = rest.find('/');
    std::string authority = (pathStart == std::string::npos) ? rest : rest.substr(0, pathStart);
    u.target = (pathStart == std::string::npos) ? "/" : rest.substr(pathStart);
    if (u.target.empty()) u.target = "/";

    if (authority.empty())
        throwError(fn, "invalid URL (missing host): " + raw);

    u.schemeHostPort = u.scheme + "://" + authority;
    return u;
}

double timeoutFromOpts(const Value* opts) {
    if (opts && opts->isMap()) {
        auto it = opts->asMap().find("timeout");
        if (it != opts->asMap().end()) return it->second.asNumber();
    }
    return 10.0;
}

struct TlsOpts {
    bool        verify = true;
    std::string caFile;
    int64_t     maxBytes = -1;   // opts.max_bytes. -1 = not given: 64 MiB in memory, no limit for a download. 0 = no limit
};

TlsOpts tlsFromOpts(const std::string& fn, const Value* opts) {
    TlsOpts t;
    if (opts && opts->isMap()) {
        const auto& m = opts->asMap();
        auto v = m.find("verify");
        if (v != m.end()) t.verify = v->second.truthy();
        auto mb = m.find("max_bytes");
        if (mb != m.end()) {
            if (!mb->second.isNumeric()) throwError(fn, "opts.max_bytes must be a number");
            t.maxBytes = numToInt64(mb->second, fn, "opts.max_bytes");
            if (t.maxBytes < 0) throwError(fn, "opts.max_bytes must not be negative");
        }
        auto c = m.find("ca_file");
        if (c != m.end()) {
            if (!c->second.isString()) throwError(fn, "opts.ca_file must be a string path");
            t.caFile = c->second.asString();
        }
    }
    return t;
}

bool followFromOpts(const Value* opts) {
    if (opts && opts->isMap()) {
        auto it = opts->asMap().find("follow");
        if (it != opts->asMap().end()) return it->second.truthy();
    }
    return true;
}

std::filesystem::path resolvePath(Interpreter& interp, const std::string& raw) {
    namespace fs = std::filesystem;
    fs::path p(raw);
    if (p.is_absolute()) return p;
    return interp.scriptDir.empty() ? (fs::current_path() / p) : (fs::path(interp.scriptDir) / p);
}

// does one request/response round trip, following redirects unless disabled. headers/body are optional (nullptr
// if not needed). if downloadTo is set, the body is streamed to that file instead of kept in memory
Value performRequest(const std::string& fn, const std::string& method, const std::string& rawUrl,
                     const Value::map_type* headerArgs, const std::string* body,
                     double timeoutSec, bool follow, const std::filesystem::path* downloadTo,
                     const TlsOpts& tls = {}) {
    UrlParts u = splitUrl(fn, rawUrl);

    if (u.scheme != "http" && u.scheme != "https")
        throwError(fn, "unsupported URL scheme '" + u.scheme + "' (only http:// and https:// are supported): " + rawUrl);
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
    if (u.scheme == "https")
        throwError(fn, "https:// is not supported -- this build has no TLS library linked in (OpenSSL was not found at configure time); use plain http://");
#endif

    httplib::Client cli(u.schemeHostPort);
    if (!cli.is_valid())
        throwError(fn, "invalid URL: " + rawUrl);

    long sec  = (long)timeoutSec;
    long usec = (long)((timeoutSec - (double)sec) * 1e6);
    cli.set_connection_timeout(sec, usec);
    cli.set_read_timeout(sec, usec);
    cli.set_write_timeout(sec, usec);
    cli.set_follow_location(follow);
    cli.set_keep_alive(false);
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    if (u.scheme == "https") {
        if (!tls.caFile.empty()) {
            if (!std::filesystem::exists(tls.caFile))
                throwError(fn, "opts.ca_file does not exist: " + tls.caFile);
            cli.set_ca_cert_path(tls.caFile);
        }
        cli.enable_server_certificate_verification(tls.verify);
        cli.enable_server_hostname_verification(tls.verify);
    }
#else
    (void)tls;
#endif

    httplib::Request req;
    req.method = method;
    req.path   = u.target;

    bool hasContentType = false;
    if (headerArgs) {
        for (auto& [k, v] : *headerArgs) {
            if (toLower(k) == "content-type") hasContentType = true;
            req.headers.emplace(k, v.asString());
        }
    }
    if (body) {
        req.body = *body;
        if (!hasContentType) req.headers.emplace("Content-Type", "application/octet-stream");
    }

    // the body is read in pieces so a huge response can be refused (or, for a download, written to disk) without
    // ever holding it all in memory
    const int64_t limit = tls.maxBytes >= 0 ? tls.maxBytes : (downloadTo ? 0 : kDefaultMaxBody);
    std::string bodyBuf;
    std::ofstream file;
    if (downloadTo) {
        file.open(*downloadTo, std::ios::binary | std::ios::trunc);
        if (!file) throwError(fn, "cannot open '" + downloadTo->string() + "' for writing");
    }
    int64_t total = 0;
    bool tooBig = false, skip = false;
    req.response_handler = [&](const httplib::Response& r) {
        // a redirect that will be followed has a body nobody wants
        skip = follow && r.status >= 300 && r.status < 400 && r.has_header("Location");
        return true;
    };
    req.content_receiver = [&](const char* data, size_t n, uint64_t, uint64_t) {
        if (skip) return true;
        total += (int64_t)n;
        if (limit > 0 && total > limit) { tooBig = true; return false; }
        if (downloadTo) file.write(data, (std::streamsize)n);
        else bodyBuf.append(data, n);
        return true;
    };

    httplib::Response res;
    httplib::Error err = httplib::Error::Success;
    bool sent = cli.send(req, res, err);
    if (tooBig)
        throwError(fn, "response is larger than " + std::to_string(limit) + " bytes (raise it with opts.max_bytes, 0 for no limit)");
    if (!sent || err != httplib::Error::Success)
        throwError(fn, "request to " + rawUrl + " failed: " + httplib::to_string(err));

    Value::map_type out;
    out["status"] = Value((int64_t)res.status);
    out["ok"]     = Value((res.status >= 200 && res.status < 400) ? (int64_t)1 : (int64_t)0);

    Value::map_type headersOut;
    for (auto& [k, v] : res.headers) headersOut[toLower(k)] = Value(v);
    out["headers"] = Value(std::move(headersOut));

    if (downloadTo) {
        file.flush();
        if (!file) throwError(fn, "failed writing response body to '" + downloadTo->string() + "'");
        out["bytes"] = Value(total);
    } else {
        out["body"] = Value(std::move(bodyBuf));
    }
    return Value(std::move(out));
}

} // namespace

EMBR_PLUGIN {

    // http_request(method: str, url: str, headers?: map, body?: str, opts?: map) -> map
    // opts: {"timeout": num seconds (default 10), "follow": num (default 1,
    // truthy = follow redirects, falsy = return the 3xx response itself),
    //        "verify": num (default 1; 0 = accept any TLS certificate),
    //        "ca_file": str (PEM bundle to trust instead of the system CA store)}
    interp->bindSig("http_request",
        {pStr("method"), pStr("url"), pOptMap("headers"), pOptStr("body"), pOptMap("opts")},
    [](const std::vector<Value>& args) -> Value {
        const Value::map_type* headers = nullptr;
        std::string bodyStorage;
        const std::string* body = nullptr;
        const Value* opts = nullptr;

        if (args.size() >= 3 && args[2].isMap()) headers = &args[2].asMap();
        if (args.size() >= 4) { bodyStorage = args[3].asString(); body = &bodyStorage; }
        if (args.size() >= 5) opts = &args[4];

        return performRequest("http_request", args[0].asString(), args[1].asString(),
                              headers, body, timeoutFromOpts(opts), followFromOpts(opts), nullptr,
                              tlsFromOpts("http_request", opts));
    });

    // http_get(url: str, headers?: map) -> map
    interp->bindSig("http_get", {pStr("url"), pOptMap("headers")},
    [](const std::vector<Value>& args) -> Value {
        const Value::map_type* headers = (args.size() >= 2 && args[1].isMap()) ? &args[1].asMap() : nullptr;
        return performRequest("http_get", "GET", args[0].asString(), headers, nullptr, 10.0, true, nullptr);
    });

    // http_post(url: str, body: str, headers?: map) -> map
    interp->bindSig("http_post", {pStr("url"), pStr("body"), pOptMap("headers")},
    [](const std::vector<Value>& args) -> Value {
        const Value::map_type* headers = (args.size() >= 3 && args[2].isMap()) ? &args[2].asMap() : nullptr;
        std::string body = args[1].asString();
        return performRequest("http_post", "POST", args[0].asString(), headers, &body, 10.0, true, nullptr);
    });

    // http_download(url: str, path: str, headers?: map, opts?: map) -> map
    // {"status", "ok", "headers", "bytes"}. writes the body straight to path (a relative path resolves against the
    // running script's directory, or cwd if there isn't one) instead of returning it as a string
    interp->bindSig("http_download", {pStr("url"), pStr("path"), pOptMap("headers"), pOptMap("opts")},
    [interp](const std::vector<Value>& args) -> Value {
        const Value::map_type* headers = (args.size() >= 3 && args[2].isMap()) ? &args[2].asMap() : nullptr;
        const Value* opts = (args.size() >= 4) ? &args[3] : nullptr;
        std::filesystem::path dest = resolvePath(*interp, args[1].asString());
        std::error_code ec;
        if (dest.has_parent_path()) std::filesystem::create_directories(dest.parent_path(), ec);
        return performRequest("http_download", "GET", args[0].asString(), headers, nullptr,
                              timeoutFromOpts(opts), followFromOpts(opts), &dest,
                              tlsFromOpts("http_download", opts));
    });

    // http_url_encode(s: str) -> str  (percent-encodes everything outside [A-Za-z0-9-_.~])
    interp->bindSig("http_url_encode", {pStr("s")},
    [](const std::vector<Value>& args) -> Value {
        static const char* hex = "0123456789ABCDEF";
        const std::string& s = args[0].asString();
        std::string out;
        for (unsigned char c : s) {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                out += (char)c;
            } else {
                out += '%';
                out += hex[c >> 4];
                out += hex[c & 0xF];
            }
        }
        return Value(std::move(out));
    });

    // http_sha256(data: str) -> str  (lowercase hex digest)
    interp->bindSig("http_sha256", {pStr("data")},
    [](const std::vector<Value>& args) -> Value {
        return Value(sha256Hex(args[0].asString()));
    });

    // http_tls_available() -> num  (1 if this build was compiled with TLS
    // support, i.e. https:// works; 0 otherwise). lets script code (e.g.
    // pkg.embr) give a clear "rebuild with OpenSSL available" message
    // itself instead of only surfacing http_get's raised error.
    interp->bind("http_tls_available",
    [](const std::vector<Value>&) -> Value {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
        return Value((int64_t)1);
#else
        return Value((int64_t)0);
#endif
    });

    // what each always returns (see Value::Callable::returns)
    interp->setResultTypes({
        {"http_request", TS::Map}, {"http_get", TS::Map}, {"http_post", TS::Map}, {"http_download", TS::Map},
        {"http_url_encode", TS::Str}, {"http_sha256", TS::Str}, {"http_tls_available", TS::Int},
    });
}

// what this plugin says about itself. embr reads it before it runs embr_register (see core/plugininfo.h)
EMBR_PLUGIN_INFO("http", "1.0.0", "HTTP(S) client", EMBR_CAP_NETWORK | EMBR_CAP_FILESYSTEM, EMBR_CAP_NONE,
    {"http_download", "same opts, but no size limit unless you give max_bytes; streams the body to path (relative paths resolve like write_file's, parents created) and returns {\"status\",\"ok\",\"headers\",\"bytes\"} (no \"body\")"},
    {"http_get", "no opts (so the 64 MiB body limit applies), use http_request for timeouts and TLS options. http_post defaults Content-Type: application/octet-stream if none given"},
    {"http_post", "no opts (so the 64 MiB body limit applies), use http_request for timeouts and TLS options. http_post defaults Content-Type: application/octet-stream if none given"},
    {"http_request", "opts: timeout (seconds, default 10; connect/read/write), follow (default 1; 0 returns the 3xx itself, target in headers[\"location\"]), verify (default 1; 0 accepts any certificate, for local testing only), ca_file (PEM bundle to trust instead of the system store, e.g. to pin a private CA or a self-signed server cert), max_bytes (the biggest body it will accept, default 64 MiB, 0 for no limit; a bigger response raises)"},
    {"http_sha256", "hex digest, for checking downloads against a published checksum (modules/pkg.embr uses it)"},
    {"http_tls_available", "1 if this build can do https://"},
    {"http_url_encode", "percent-encodes everything outside [A-Za-z0-9-_.~]"});
