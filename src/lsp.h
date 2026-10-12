// lsp.h
// the language server behind embr lsp.
// it talks json rpc over stdin/stdout. it tells the editor about lex, parse and compile errors while you type,
// and answers go-to-definition, find-references, rename, hover, completion and the outline (document symbols) from lsp_index.h.
//
// good to know
//   - the editor sends the whole file on every change (sync mode 1), so there is no incremental state
//   - embr columns count bytes, the protocol counts UTF-16 units. toUtf16Col() and fromUtf16Col() convert, nothing else should
//   - the json here is just enough for the protocol. the json plugin can't be used: plugins are separate .so files

#ifndef EMBR_LSP_H
#define EMBR_LSP_H

#include <embr/embr.h>
#include "lsp_index.h"

#include <cstdio>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace embr_lsp {

// a small json value

struct Json {
    enum Kind { Null, Bool, Num, Str, Arr, Obj } kind = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> a;
    std::map<std::string, Json> o;

    const Json& at(const std::string& key) const {
        static const Json none;
        auto it = o.find(key);
        return it == o.end() ? none : it->second;
    }
    bool isObject() const { return kind == Obj; }
    bool isString() const { return kind == Str; }
};

// parses one json value. returns false on any error. depth and size are bounded so a hostile client can't overflow the stack.
class JsonParser {
public:
    explicit JsonParser(const std::string& t) : t_(t) {}
    bool parse(Json& out) {
        if (!value(out, 0)) return false;
        ws();
        return i_ == t_.size();
    }

private:
    const std::string& t_;
    size_t i_ = 0;

    void ws() { while (i_ < t_.size() && (t_[i_] == ' ' || t_[i_] == '\t' || t_[i_] == '\n' || t_[i_] == '\r')) ++i_; }
    bool lit(const char* w) {
        size_t n = std::char_traits<char>::length(w);
        if (t_.compare(i_, n, w) != 0) return false;
        i_ += n;
        return true;
    }
    static void utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
        else { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F));
               out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
    }
    bool hex4(unsigned& v) {
        if (i_ + 4 > t_.size()) return false;
        v = 0;
        for (int k = 0; k < 4; ++k) {
            char c = t_[i_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool str(std::string& out) {
        if (i_ >= t_.size() || t_[i_] != '"') return false;
        ++i_;
        while (i_ < t_.size()) {
            char c = t_[i_++];
            if (c == '"') return true;
            if (c != '\\') { out += c; continue; }
            if (i_ >= t_.size()) return false;
            switch (char e = t_[i_++]) {
                case 'n': out += '\n'; break; case 't': out += '\t'; break; case 'r': out += '\r'; break;
                case 'b': out += '\b'; break; case 'f': out += '\f'; break;
                case '"': case '\\': case '/': out += e; break;
                case 'u': {
                    unsigned cp;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp < 0xDC00 && t_.compare(i_, 2, "\\u") == 0) {   // surrogate pair
                        i_ += 2;
                        unsigned lo;
                        if (!hex4(lo) || lo < 0xDC00 || lo > 0xDFFF) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    utf8(out, cp);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }
    bool value(Json& v, int depth) {
        if (depth > 64) return false;
        ws();
        if (i_ >= t_.size()) return false;
        char c = t_[i_];
        if (c == '{') {
            ++i_; v.kind = Json::Obj; ws();
            if (i_ < t_.size() && t_[i_] == '}') { ++i_; return true; }
            while (true) {
                ws();
                std::string k;
                if (!str(k)) return false;
                ws();
                if (i_ >= t_.size() || t_[i_++] != ':') return false;
                if (!value(v.o[k], depth + 1)) return false;
                ws();
                if (i_ >= t_.size()) return false;
                if (t_[i_] == ',') { ++i_; continue; }
                if (t_[i_] == '}') { ++i_; return true; }
                return false;
            }
        }
        if (c == '[') {
            ++i_; v.kind = Json::Arr; ws();
            if (i_ < t_.size() && t_[i_] == ']') { ++i_; return true; }
            while (true) {
                v.a.emplace_back();
                if (!value(v.a.back(), depth + 1)) return false;
                ws();
                if (i_ >= t_.size()) return false;
                if (t_[i_] == ',') { ++i_; continue; }
                if (t_[i_] == ']') { ++i_; return true; }
                return false;
            }
        }
        if (c == '"') { v.kind = Json::Str; return str(v.s); }
        if (lit("true"))  { v.kind = Json::Bool; v.b = true;  return true; }
        if (lit("false")) { v.kind = Json::Bool; v.b = false; return true; }
        if (lit("null"))  { v.kind = Json::Null; return true; }
        size_t start = i_;
        while (i_ < t_.size() && (std::isdigit((unsigned char)t_[i_]) || t_[i_] == '-' || t_[i_] == '+' ||
                                  t_[i_] == '.' || t_[i_] == 'e' || t_[i_] == 'E')) ++i_;
        if (i_ == start) return false;
        v.kind = Json::Num;
        v.n = std::strtod(t_.substr(start, i_ - start).c_str(), nullptr);
        return true;
    }
};

inline std::string quote(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break; case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break; case '\r': out += "\\r"; break; case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) { char buf[8]; std::snprintf(buf, sizeof buf, "\\u%04x", c); out += buf; }
                else out += (char)c;
        }
    }
    return out + "\"";
}

// the server

// returns every error found in a document, see diagnose() in cli.cpp
using DiagnoseFn = std::function<std::vector<embr::EmbrError>(const std::string& text, const std::string& uri)>;

// a byte column (1-based, what embr reports) to the 0-based UTF-16 column the protocol wants
inline int toUtf16Col(const std::string& line, int byteCol) {
    size_t end = byteCol > 0 ? (size_t)byteCol - 1 : 0;
    if (end > line.size()) end = line.size();
    int units = 0;
    for (size_t i = 0; i < end; ++i) {
        unsigned char c = (unsigned char)line[i];
        if ((c & 0xC0) == 0x80) continue;          // continuation byte: part of the previous character
        units += c >= 0xF0 ? 2 : 1;                // a 4-byte character is a surrogate pair in UTF-16
    }
    return units;
}

// the other way: a 0-based UTF-16 column to embr's 1-based byte column
inline int fromUtf16Col(const std::string& line, int units) {
    size_t i = 0;
    while (i < line.size() && units > 0) {
        unsigned char c = (unsigned char)line[i];
        units -= c >= 0xF0 ? 2 : 1;
        ++i;
        while (i < line.size() && ((unsigned char)line[i] & 0xC0) == 0x80) ++i;
    }
    return (int)i + 1;
}

// file:///home/a/x.embr -> /home/a/x.embr (and file:///C:/x.embr -> C:/x.embr). "" for any other scheme
inline std::string uriToPath(const std::string& uri) {
    if (uri.rfind("file://", 0) != 0) return "";
    std::string out;
    for (size_t i = 7; i < uri.size(); ++i) {
        if (uri[i] == '%' && i + 2 < uri.size() && std::isxdigit((unsigned char)uri[i + 1]) && std::isxdigit((unsigned char)uri[i + 2])) {
            out += (char)std::stoi(uri.substr(i + 1, 2), nullptr, 16);
            i += 2;
        } else {
            out += uri[i];
        }
    }
    if (out.size() > 2 && out[0] == '/' && out[2] == ':') out.erase(0, 1);
    return out;
}

class Server {
public:
    Server(std::istream& in, std::ostream& out, DiagnoseFn diagnose) : in_(in), out_(out), diagnose_(std::move(diagnose)) {}

    // runs until exit or end of input. returns the process exit code the protocol asks for
    int run() {
        std::string body;
        while (readMessage(body)) {
            Json msg;
            if (!JsonParser(body).parse(msg) || !msg.isObject()) { replyError("null", -32700, "parse error"); continue; }
            handle(msg);
            if (exit_) return shutdown_ ? 0 : 1;
        }
        return 1;      // the client went away without `exit`
    }

private:
    std::istream& in_;
    std::ostream& out_;
    DiagnoseFn diagnose_;
    std::map<std::string, std::string> docs_;   // uri -> text
    // uri -> what is known about its names. `last` is from the latest text the lexer could read, `clean` from the latest
    // text with no parse errors (a function you are typing in is dropped by the parser, so completion still looks there)
    struct Indexes { Index last, clean; bool lastIsClean = false; };
    std::map<std::string, Indexes> index_;
    std::unique_ptr<NativeCatalog> natives_;     // made on first use: it loads plugins
    bool shutdown_ = false, exit_ = false;

    static constexpr size_t kMaxMessage = 64u * 1024 * 1024;

    bool readMessage(std::string& body) {
        long length = -1;
        std::string line;
        while (std::getline(in_, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) {
                if (length < 0) continue;
                if ((size_t)length > kMaxMessage) return false;
                body.assign((size_t)length, '\0');
                in_.read(&body[0], length);
                return in_.gcount() == length;
            }
            if (line.rfind("Content-Length:", 0) == 0) length = std::atol(line.c_str() + 15);
        }
        return false;
    }

    void send(const std::string& json) {
        out_ << "Content-Length: " << json.size() << "\r\n\r\n" << json << std::flush;
    }
    void reply(const std::string& id, const std::string& result) {
        send("{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"result\":" + result + "}");
    }
    void replyError(const std::string& id, int code, const std::string& message) {
        send("{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"error\":{\"code\":" + std::to_string(code) +
             ",\"message\":" + quote(message) + "}}");
    }
    // an id is a number or a string; echo it back in the same form
    static std::string idText(const Json& id) {
        if (id.kind == Json::Num) { char b[32]; std::snprintf(b, sizeof b, "%.0f", id.n); return b; }
        if (id.kind == Json::Str) return quote(id.s);
        return "null";
    }

    static std::string lineOf(const std::string& text, int line) {   // 1-based
        size_t start = 0;
        for (int l = 1; l < line; ++l) {
            size_t nl = text.find('\n', start);
            if (nl == std::string::npos) return "";
            start = nl + 1;
        }
        size_t end = text.find('\n', start);
        std::string out = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!out.empty() && out.back() == '\r') out.pop_back();
        return out;
    }

    void publish(const std::string& uri) {
        std::string list;
        auto it = docs_.find(uri);
        if (it != docs_.end()) {
            const std::string& text = it->second;
            for (const embr::EmbrError& e : diagnose_(text, uri)) {
                int sl = 0, sc = 0, el = 0, ec = 0;
                if (e.hasLocation && e.range.valid()) {
                    sl = e.range.startLine - 1; el = (e.range.endLine > 0 ? e.range.endLine : e.range.startLine) - 1;
                    sc = toUtf16Col(lineOf(text, e.range.startLine), e.range.startCol);
                    ec = toUtf16Col(lineOf(text, el + 1), e.range.endCol > 0 ? e.range.endCol : e.range.startCol + 1);
                    if (el == sl && ec <= sc) ec = sc + 1;     // an empty range is invisible in most editors
                }
                if (!list.empty()) list += ",";
                list += "{\"range\":{\"start\":{\"line\":" + std::to_string(sl) + ",\"character\":" + std::to_string(sc) +
                        "},\"end\":{\"line\":" + std::to_string(el) + ",\"character\":" + std::to_string(ec) +
                        "}},\"severity\":1,\"source\":\"embr\",\"message\":" +
                        quote((e.context.empty() ? "" : e.context + ": ") + e.message) + "}";
            }
        }
        send("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":{\"uri\":" + quote(uri) +
             ",\"diagnostics\":[" + list + "]}}");
    }

    void reindex(const std::string& uri) {
        Index ix;
        bool clean = false;
        if (!buildIndex(docs_[uri], ix, &clean)) return;   // the lexer gave up: keep what was known
        auto& slot = index_[uri];
        if (clean) slot.clean = ix;
        slot.last = std::move(ix);
        slot.lastIsClean = clean;
    }

    NativeCatalog& natives() {
        if (!natives_) natives_ = std::make_unique<NativeCatalog>();
        return *natives_;
    }

    // a protocol range for an embr range in `text`
    static std::string rangeJson(const std::string& text, const SourceRange& r) {
        int el = r.endLine > 0 ? r.endLine : r.startLine;
        return "{\"start\":{\"line\":" + std::to_string(r.startLine - 1) + ",\"character\":" +
               std::to_string(toUtf16Col(lineOf(text, r.startLine), r.startCol)) + "},\"end\":{\"line\":" +
               std::to_string(el - 1) + ",\"character\":" + std::to_string(toUtf16Col(lineOf(text, el), r.endCol)) + "}}";
    }

    // the document and the embr position (1-based line, byte column) a request points at. false when the document is not open
    struct At { std::string uri; const std::string* text = nullptr; const Indexes* ix = nullptr; int line = 0, col = 0; };
    bool where(const Json& params, At& at) {
        at.uri = params.at("textDocument").at("uri").s;
        auto d = docs_.find(at.uri);
        auto x = index_.find(at.uri);
        if (d == docs_.end() || x == index_.end()) return false;
        at.text = &d->second;
        at.ix = &x->second;
        at.line = (int)params.at("position").at("line").n + 1;
        at.col = fromUtf16Col(lineOf(d->second, at.line), (int)params.at("position").at("character").n);
        return true;
    }

    // every use of the same thing as the use at `u`: the same symbol, or for a name not made in this file (a builtin) the same name
    static std::vector<const Use*> sameAs(const Index& ix, const Use& u) {
        std::vector<const Use*> out;
        for (const auto& o : ix.uses)
            if (o.symbol == u.symbol && (u.symbol >= 0 || o.name == u.name)) out.push_back(&o);
        return out;
    }

    std::string references(const Json& params) {
        At at;
        if (!where(params, at)) return "null";
        const Index& ix = at.ix->last;
        const Use* u = ix.useAt(at.line, at.col);
        if (!u) return "[]";
        bool withDecl = params.at("context").at("includeDeclaration").b;
        std::string out;
        for (const Use* o : sameAs(ix, *u)) {
            if (!withDecl && o->symbol >= 0 && o->at.startLine == ix.symbols[(size_t)o->symbol].at.startLine &&
                o->at.startCol == ix.symbols[(size_t)o->symbol].at.startCol) continue;
            if (!out.empty()) out += ",";
            out += "{\"uri\":" + quote(at.uri) + ",\"range\":" + rangeJson(*at.text, o->at) + "}";
        }
        return "[" + out + "]";
    }

    // the name a rename would change, or null with `why` set when it can't be renamed
    const Use* renameTarget(const At& at, std::string& why) {
        // with parse errors some statements are missing from the index, so some uses would be missed
        if (!at.ix->lastIsClean) { why = "the file has errors, fix them before renaming"; return nullptr; }
        const Use* u = at.ix->last.useAt(at.line, at.col);
        if (!u) { why = "there is no name here"; return nullptr; }
        if (u->symbol < 0) { why = "'" + u->name + "' is not made in this file"; return nullptr; }
        return u;
    }

    std::string prepareRename(const Json& params, std::string& why) {
        At at;
        if (!where(params, at)) { why = "the document is not open"; return ""; }
        const Use* u = renameTarget(at, why);
        return u ? rangeJson(*at.text, u->at) : "";
    }

    // renames a name in this file only. another file that imports this one keeps the old name
    std::string rename(const Json& params, std::string& why) {
        At at;
        if (!where(params, at)) { why = "the document is not open"; return ""; }
        const Use* u = renameTarget(at, why);
        if (!u) return "";
        const std::string& to = params.at("newName").s;
        bool ident = !to.empty() && !std::isdigit((unsigned char)to[0]);
        for (char c : to) if (!std::isalnum((unsigned char)c) && c != '_') ident = false;
        if (!ident || embr::keywordTable().count(to)) { why = "'" + to + "' is not a valid name"; return ""; }
        const Index& ix = at.ix->last;
        auto uses = sameAs(ix, *u);
        // the new name must not already mean something where the renamed one is written, or those uses would change meaning
        for (const Use* o : uses)
            for (const Symbol* s : ix.visibleAt(o->at.startLine, o->at.startCol))
                if (s->name == to) { why = "'" + to + "' is already used here (line " + std::to_string(o->at.startLine) + ")"; return ""; }
        // and a use of the new name somewhere the renamed one can be seen would start meaning the renamed one
        const Symbol* renamed = &ix.symbols[(size_t)u->symbol];
        for (const auto& o : ix.uses) {
            if (o.name != to) continue;
            for (const Symbol* s : ix.visibleAt(o.at.startLine, o.at.startCol))
                if (s == renamed) { why = "'" + to + "' is already used here (line " + std::to_string(o.at.startLine) + ")"; return ""; }
        }
        for (const auto& n : natives().builtins())
            if (n.name == to) { why = "'" + to + "' is a builtin"; return ""; }
        std::string edits;
        for (const Use* o : uses) {
            if (!edits.empty()) edits += ",";
            edits += "{\"range\":" + rangeJson(*at.text, o->at) + ",\"newText\":" + quote(to) + "}";
        }
        return "{\"changes\":{" + quote(at.uri) + ":[" + edits + "]}}";
    }

    std::string definition(const Json& params) {
        At at;
        if (!where(params, at)) return "null";
        const Use* u = at.ix->last.useAt(at.line, at.col);
        if (!u || u->symbol < 0) return "null";
        const Symbol& s = at.ix->last.symbols[(size_t)u->symbol];
        return "{\"uri\":" + quote(at.uri) + ",\"range\":" + rangeJson(*at.text, s.at) + "}";
    }

    // the doc line of every function of an imported script module, from the comment above it. the file is read, not run, and its
    // plugins are only looked for in trusted places
    std::map<std::string, std::string> moduleDocs(const std::string& dir, const std::string& m) {
        std::map<std::string, std::string> out;
        const bool compiled = std::filesystem::path(m).extension() == ".vmbr";
        std::filesystem::path file = embr::findExistingCandidate(embr::fileCandidates(m, dir, compiled ? ".vmbr" : ".embr", "modules"));
        if (file.empty()) return out;
        embr::PluginPolicy policy;
        policy.restricted = true;
        std::set<std::string> visiting;
        for (const auto& f : embr::analyzeModuleFile(file, policy, visiting).functions) out[f.name] = f.doc;
        return out;
    }

    // what a name not made in this file is: a builtin, a function from an imported plugin, or a name from an imported script module.
    // "" when it is none of those
    std::string outsideDetail(const std::string& uri, const Index& ix, const std::string& name) {
        for (const auto& n : natives().builtins()) if (n.name == name) return n.signature + "\n```\n" + n.from;
        for (const auto& p : ix.plugins)
            for (const auto& n : natives().plugin(p)) if (n.name == name) return n.signature + "\n```\n" + n.from + (n.doc.empty() ? "" : "\n\n" + n.doc);
        embr::vm::ModuleTypeResolver modules(nullptr);
        std::string dir = std::filesystem::path(uriToPath(uri)).parent_path().string();
        for (const auto& m : ix.modules) {
            const embr::typecheck::ModuleExports* ex = modules.resolve(dir, m);
            if (!ex) continue;
            auto it = ex->find(name);
            if (it == ex->end() || it->second.isLocal) continue;
            const auto& e = it->second;
            std::string sig = e.isFn ? signatureText(name, e.params, e.ret, e.retEl) : name + ": " + typeText(e.type, e.el);
            std::string doc = moduleDocs(dir, m)[name];
            return sig + "\n```\nfrom " + m + (doc.empty() ? "" : "\n\n" + doc);
        }
        return "";
    }

    std::string hover(const Json& params) {
        At at;
        if (!where(params, at)) return "null";
        const Index& ix = at.ix->last;
        const Use* u = ix.useAt(at.line, at.col);
        if (!u) return "null";
        std::string body;
        if (u->symbol >= 0) body = ix.symbols[(size_t)u->symbol].detail + "\n```";
        else body = outsideDetail(at.uri, ix, u->name);
        if (body.empty()) return "null";
        return "{\"contents\":{\"kind\":\"markdown\",\"value\":" + quote("```embr\n" + body) +
               "},\"range\":" + rangeJson(*at.text, u->at) + "}";
    }

    std::string completion(const Json& params) {
        At at;
        if (!where(params, at)) return "[]";
        // after `m.` the word is a map key, not a name
        std::string line = lineOf(*at.text, at.line);
        size_t i = (size_t)std::min<int>(at.col - 1, (int)line.size());
        while (i > 0 && (std::isalnum((unsigned char)line[i - 1]) || line[i - 1] == '_')) --i;
        if (i > 0 && line[i - 1] == '.') return "[]";

        // LSP CompletionItemKind: 3 function, 6 variable, 14 keyword, 21 constant
        std::set<std::string> seen;
        std::string items;
        auto add = [&](const std::string& name, int kind, const std::string& detail, const std::string& doc = "") {
            if (name.empty() || !seen.insert(name).second) return;
            if (!items.empty()) items += ",";
            items += "{\"label\":" + quote(name) + ",\"kind\":" + std::to_string(kind);
            if (!detail.empty()) items += ",\"detail\":" + quote(detail);
            if (!doc.empty()) items += ",\"documentation\":" + quote(doc);
            items += "}";
        };
        auto symKind = [](const Symbol& s) {
            return s.kind == Symbol::Kind::Function ? 3 : s.kind == Symbol::Kind::Constant ? 21 : 6;
        };
        for (const Index* ix : {&at.ix->last, &at.ix->clean})
            for (const Symbol* s : ix->visibleAt(at.line, at.col)) add(s->name, symKind(*s), s->detail);
        for (const auto& n : natives().builtins()) add(n.name, 3, n.signature);
        for (const auto& p : at.ix->last.plugins)
            for (const auto& n : natives().plugin(p)) add(n.name, 3, n.signature, n.doc);
        embr::vm::ModuleTypeResolver modules(nullptr);
        std::string dir = std::filesystem::path(uriToPath(at.uri)).parent_path().string();
        for (const auto& m : at.ix->last.modules)
            if (const auto* ex = modules.resolve(dir, m)) {
                auto docs = moduleDocs(dir, m);
                for (const auto& [name, e] : *ex) if (!e.isLocal) add(name, e.isFn ? 3 : 6, "from " + m, docs[name]);
            }
        for (const auto& kv : embr::keywordTable()) add(kv.first, 14, "");
        return "[" + items + "]";
    }

    // the outline: top-level functions (with the functions inside them) and top-level names
    std::string documentSymbols(const Json& params) {
        const std::string uri = params.at("textDocument").at("uri").s;
        auto d = docs_.find(uri);
        auto x = index_.find(uri);
        if (d == docs_.end() || x == index_.end()) return "[]";
        const Index& ix = x->second.last;
        // LSP SymbolKind: 12 function, 13 variable, 14 constant
        std::function<std::string(int)> children = [&](int parent) {
            std::string out;
            for (size_t i = 0; i < ix.symbols.size(); ++i) {
                const Symbol& s = ix.symbols[i];
                if (s.parent != parent || s.kind == Symbol::Kind::Parameter) continue;
                if (parent >= 0 && s.kind != Symbol::Kind::Function) continue;   // a function's own variables are left out
                if (parent < 0 && !s.topLevel) continue;
                int kind = s.kind == Symbol::Kind::Function ? 12 : s.kind == Symbol::Kind::Constant ? 14 : 13;
                if (!out.empty()) out += ",";
                out += "{\"name\":" + quote(s.name) + ",\"kind\":" + std::to_string(kind) +
                       ",\"range\":" + rangeJson(d->second, s.whole) + ",\"selectionRange\":" + rangeJson(d->second, s.at);
                if (s.kind == Symbol::Kind::Function) out += ",\"detail\":" + quote(s.detail) + ",\"children\":[" + children((int)i) + "]";
                out += "}";
            }
            return out;
        };
        return "[" + children(-1) + "]";
    }

    void handle(const Json& msg) {
        const std::string method = msg.at("method").s;
        const Json& params = msg.at("params");
        bool isRequest = msg.o.count("id") > 0;
        std::string id = idText(msg.at("id"));

        if (method == "initialize") {
            reply(id, "{\"capabilities\":{\"textDocumentSync\":{\"openClose\":true,\"change\":1},"
                      "\"definitionProvider\":true,\"hoverProvider\":true,\"documentSymbolProvider\":true,"
                      "\"referencesProvider\":true,\"renameProvider\":{\"prepareProvider\":true},"
                      "\"completionProvider\":{}},"
                      "\"serverInfo\":{\"name\":\"embr\"}}");
        } else if (method == "textDocument/definition") {
            reply(id, definition(params));
        } else if (method == "textDocument/references") {
            reply(id, references(params));
        } else if (method == "textDocument/prepareRename" || method == "textDocument/rename") {
            std::string why;
            std::string result = method == "textDocument/rename" ? rename(params, why) : prepareRename(params, why);
            if (why.empty()) reply(id, result);
            else replyError(id, -32803, why);      // RequestFailed: the editor shows the message
        } else if (method == "textDocument/hover") {
            reply(id, hover(params));
        } else if (method == "textDocument/completion") {
            reply(id, completion(params));
        } else if (method == "textDocument/documentSymbol") {
            reply(id, documentSymbols(params));
        } else if (method == "shutdown") {
            shutdown_ = true;
            reply(id, "null");
        } else if (method == "exit") {
            exit_ = true;
        } else if (method == "textDocument/didOpen") {
            const Json& d = params.at("textDocument");
            if (d.at("uri").isString() && d.at("text").isString()) {
                docs_[d.at("uri").s] = d.at("text").s;
                reindex(d.at("uri").s);
                publish(d.at("uri").s);
            }
        } else if (method == "textDocument/didChange") {
            const std::string& uri = params.at("textDocument").at("uri").s;
            const Json& changes = params.at("contentChanges");
            if (!uri.empty() && !changes.a.empty() && changes.a.back().at("text").isString()) {
                docs_[uri] = changes.a.back().at("text").s;      // full sync: the last change is the whole file
                reindex(uri);
                publish(uri);
            }
        } else if (method == "textDocument/didClose") {
            const std::string& uri = params.at("textDocument").at("uri").s;
            docs_.erase(uri);
            index_.erase(uri);
            publish(uri);                                          // an erased document publishes an empty list
        } else if (isRequest) {
            replyError(id, -32601, "method not found: " + method);
        }
        // any other notification ($/cancelRequest, initialized, ...) is ignored
    }
};

} // namespace embr_lsp

#endif // EMBR_LSP_H
