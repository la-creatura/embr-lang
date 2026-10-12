#!/usr/bin/env python3
"""drives `embr lsp` over pipes: initialize, open a broken file, fix it, ask about names, close it, shut down.

  lsp_smoke.py <embr binary>
"""
import json, subprocess, sys

proc = subprocess.Popen([sys.argv[1], "lsp"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                        stderr=subprocess.DEVNULL)

def send(obj):
    body = json.dumps(obj).encode()
    proc.stdin.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
    proc.stdin.flush()

def recv():
    length = None
    while True:
        line = proc.stdout.readline()
        if not line:
            raise SystemExit("FAIL: server closed its output")
        line = line.strip()
        if not line:
            break
        if line.lower().startswith(b"content-length:"):
            length = int(line.split(b":")[1])
    return json.loads(proc.stdout.read(length))

def check(cond, what):
    if not cond:
        proc.kill()
        raise SystemExit("FAIL: " + what)

uri = "file:///tmp/t.embr"

send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"capabilities": {}}})
r = recv()
check(r["id"] == 1 and r["result"]["capabilities"]["textDocumentSync"]["change"] == 1, "initialize result")
send({"jsonrpc": "2.0", "method": "initialized", "params": {}})

# a lexer error after a two-byte character: the byte column 10 must become UTF-16 column 8
send({"jsonrpc": "2.0", "method": "textDocument/didOpen",
      "params": {"textDocument": {"uri": uri, "languageId": "embr", "version": 1, "text": 's = "é" @\nprint(1)\n'}}})
r = recv()
check(r["method"] == "textDocument/publishDiagnostics" and r["params"]["uri"] == uri, "publishDiagnostics after open")
d = r["params"]["diagnostics"]
check(len(d) == 1, "one diagnostic, got %r" % d)
check(d[0]["range"]["start"] == {"line": 0, "character": 8}, "range start %r" % d[0]["range"])
check("unexpected character" in d[0]["message"], "message %r" % d[0]["message"])

# a parse error on a later line, with the earlier lines fine
send({"jsonrpc": "2.0", "method": "textDocument/didChange",
      "params": {"textDocument": {"uri": uri, "version": 2},
                 "contentChanges": [{"text": "x = 1\nif x\nprint(x)\n"}]}})
r = recv()
d = r["params"]["diagnostics"]
check(len(d) >= 1 and d[0]["range"]["start"]["line"] >= 1, "parse error on a later line: %r" % d)

# fixed: no diagnostics
send({"jsonrpc": "2.0", "method": "textDocument/didChange",
      "params": {"textDocument": {"uri": uri, "version": 3},
                 "contentChanges": [{"text": "x = 1\nprint(x)\n"}]}})
check(recv()["params"]["diagnostics"] == [], "clean file has no diagnostics")

# names: go-to-definition, hover, completion and the outline
src = """import "json"
fn add(a: int, b) -> int
    local int s = a + b
    return s
end
total = add(1, 2)
fn counter()
    n = 0
    total = 9
    return fn()
        n = n + 1
        return n
    end
end
print(json_parse("[1]"), total)
fn later()
    return add(early(), 1)
end
early() = 2
"""
send({"jsonrpc": "2.0", "method": "textDocument/didChange",
      "params": {"textDocument": {"uri": uri, "version": 4}, "contentChanges": [{"text": src}]}})
check(recv()["params"]["diagnostics"] == [], "names file has no diagnostics")

next_id = [100]
def request(method, line, char):
    next_id[0] += 1
    send({"jsonrpc": "2.0", "id": next_id[0], "method": method,
          "params": {"textDocument": {"uri": uri}, "position": {"line": line, "character": char}}})
    r = recv()
    check(r.get("id") == next_id[0] and "result" in r, "%s answer %r" % (method, r))
    return r["result"]

def defined_at(line, char):
    r = request("textDocument/definition", line, char)
    return r and (r["range"]["start"]["line"], r["range"]["start"]["character"])

check(defined_at(5, 9) == (1, 3), "add() goes to its fn")                       # total = add(1, 2)
check(defined_at(3, 11) == (2, 14), "s goes to its local")                      # return s
check(defined_at(3, 12) == (2, 14), "the column just past a name counts")
check(defined_at(2, 18) == (1, 7), "a goes to the parameter")                   # s = a + b
check(defined_at(11, 15) == (7, 4), "a closure's n goes to the enclosing function's n")
check(defined_at(14, 30) == (5, 0), "top-level total")                          # print(..., total)
check(defined_at(8, 4) == (5, 0), "total inside a function assigns the top-level total that already exists")
check(defined_at(16, 15) == (18, 0), "a call of a function defined further down")
check(request("textDocument/definition", 14, 2) is None, "a builtin has no definition in the file")

h = request("textDocument/hover", 5, 9)["contents"]["value"]
check("fn add(a: int, b) -> int" in h, "hover on add: %r" % h)
h = request("textDocument/hover", 14, 8)["contents"]["value"]
check("json_parse(" in h and "plugin json" in h, "hover on a plugin function: %r" % h)
h = request("textDocument/hover", 14, 2)["contents"]["value"]
check("fn print(" in h and "builtin" in h, "hover on a builtin: %r" % h)
h = request("textDocument/hover", 11, 15)["contents"]["value"]
check("n = 0" in h, "hover on a variable shows the line that makes it: %r" % h)
check(request("textDocument/hover", 4, 1) is None, "hover on a keyword is empty")

items = request("textDocument/completion", 3, 4)                                # inside add
labels = {i["label"] for i in items}
jp = [i for i in items if i["label"] == "json_parse"]
check(jp and jp[0].get("documentation"), "a plugin function's completion carries the doc line its plugin gives: %r" % jp)
check(not [i for i in items if i["label"] == "print" and i.get("documentation")], "a builtin has no doc line")
for want in ("a", "b", "s", "add", "total", "counter", "print", "json_parse", "while"):
    check(want in labels, "completion has %s" % want)
check("n" not in labels, "completion leaves out another function's local")

send({"jsonrpc": "2.0", "id": 300, "method": "textDocument/documentSymbol", "params": {"textDocument": {"uri": uri}}})
syms = recv()["result"]
check([s["name"] for s in syms] == ["add", "total", "counter", "later", "early"], "outline %r" % [s["name"] for s in syms])
check(syms[0]["range"]["end"]["line"] == 4 and syms[0]["selectionRange"]["start"] == {"line": 1, "character": 3}, "outline ranges")

# references and rename
def lines_of(locs):
    return sorted(l["range"]["start"]["line"] for l in locs)
def refs(line, char, decl=True):
    next_id[0] += 1
    send({"jsonrpc": "2.0", "id": next_id[0], "method": "textDocument/references",
          "params": {"textDocument": {"uri": uri}, "position": {"line": line, "character": char},
                     "context": {"includeDeclaration": decl}}})
    return recv()["result"]
check(lines_of(refs(5, 9)) == [1, 5, 16], "references of add")
check(lines_of(refs(5, 9, False)) == [5, 16], "references of add without its definition")
check(lines_of(refs(11, 15)) == [7, 10, 10, 11], "references of a closure's n")
check(lines_of(refs(5, 0)) == [5, 8, 14], "references of top-level total include the assignment inside counter")

def rename(line, char, to):
    next_id[0] += 1
    send({"jsonrpc": "2.0", "id": next_id[0], "method": "textDocument/rename",
          "params": {"textDocument": {"uri": uri}, "position": {"line": line, "character": char}, "newName": to}})
    return recv()
r = rename(3, 11, "sum")["result"]["changes"][uri]
check(lines_of(r) == [2, 3] and all(e["newText"] == "sum" for e in r), "rename a local: %r" % r)
check(rename(5, 0, "grand")["result"]["changes"][uri] and lines_of(rename(5, 0, "grand")["result"]["changes"][uri]) == [5, 8, 14],
      "rename a top-level name")
check("already used" in rename(3, 11, "b")["error"]["message"], "rename onto a name already seen there is refused")
check("already used" in rename(2, 18, "total")["error"]["message"], "rename that would capture a use is refused")
check("not a valid name" in rename(3, 11, "end")["error"]["message"], "rename to a keyword is refused")
check("not made in this file" in rename(14, 2, "show")["error"]["message"], "renaming a builtin is refused")
next_id[0] += 1
send({"jsonrpc": "2.0", "id": next_id[0], "method": "textDocument/prepareRename",
      "params": {"textDocument": {"uri": uri}, "position": {"line": 3, "character": 11}}})
check(recv()["result"]["start"] == {"line": 3, "character": 11}, "prepareRename gives the name's range")

# while typing, the function being edited does not parse. completion still knows its names
send({"jsonrpc": "2.0", "method": "textDocument/didChange",
      "params": {"textDocument": {"uri": uri, "version": 5},
                 "contentChanges": [{"text": src.replace("    return s\n", "    return s +\n")}]}})
recv()
labels = {i["label"] for i in request("textDocument/completion", 3, 4)}
check("s" in labels and "a" in labels, "completion falls back to the last clean parse")

r = rename(14, 30, "grand"); check("fix them" in r.get("error", {}).get("message", ""), "no rename while the file has errors: %r" % r)

# a function from an imported script module shows its parameters
import os, tempfile
mdir = tempfile.mkdtemp()
with open(os.path.join(mdir, "util.embr"), "w") as f:
    f.write("fn twice(x: int) -> int\n    return x * 2\nend\n")
muri = "file://" + os.path.join(mdir, "main.embr")
send({"jsonrpc": "2.0", "method": "textDocument/didOpen",
      "params": {"textDocument": {"uri": muri, "languageId": "embr", "version": 1, "text": 'import "util.embr"\nprint(twice(2))\n'}}})
recv()
next_id[0] += 1
send({"jsonrpc": "2.0", "id": next_id[0], "method": "textDocument/hover",
      "params": {"textDocument": {"uri": muri}, "position": {"line": 1, "character": 8}}})
h = recv()["result"]["contents"]["value"]
check("fn twice(x: int) -> int" in h and "util.embr" in h, "hover on a module function: %r" % h)
os.remove(os.path.join(mdir, "util.embr")); os.rmdir(mdir)

# every kind of statement and expression the index walks: while, if/else, a function value, arrays, unary minus, break
suri = "file:///tmp/shapes.embr"
shapes = """square = fn(n: int) -> int
    return n * n
end
xs = [1, -2, 3]
i = 0
while i < 3
    local v = square(xs[i])
    if v > 3
        print(-v)
        break
    else
        print({"v": v})
    end
    i = i + 1
end
"""
send({"jsonrpc": "2.0", "method": "textDocument/didOpen",
      "params": {"textDocument": {"uri": suri, "languageId": "embr", "version": 1, "text": shapes}}})
check(recv()["params"]["diagnostics"] == [], "shapes file has no diagnostics")
def at(method, line, char, u=suri):
    next_id[0] += 1
    send({"jsonrpc": "2.0", "id": next_id[0], "method": method,
          "params": {"textDocument": {"uri": u}, "position": {"line": line, "character": char}}})
    return recv()["result"]
check(at("textDocument/definition", 6, 14)["range"]["start"] == {"line": 0, "character": 0}, "a function value's name")
check("fn square(n: int) -> int" in at("textDocument/hover", 6, 14)["contents"]["value"], "hover on a function value")
check(at("textDocument/definition", 8, 15)["range"]["start"] == {"line": 6, "character": 10}, "a local inside while and if")
check(at("textDocument/definition", 6, 21)["range"]["start"] == {"line": 3, "character": 0}, "an array read inside a call")
check(at("textDocument/definition", 6, 24)["range"]["start"] == {"line": 4, "character": 0}, "an index inside an index")
check(at("textDocument/definition", 11, 20)["range"]["start"] == {"line": 6, "character": 10}, "a local inside else")
check("v" not in {i["label"] for i in at("textDocument/completion", 15, 0)}, "a local is gone after its block")

# unknown requests get an error, unknown notifications are ignored
send({"jsonrpc": "2.0", "id": 7, "method": "textDocument/typeDefinition", "params": {}})
r = recv()
check(r["id"] == 7 and r["error"]["code"] == -32601, "unknown request answers method-not-found")
send({"jsonrpc": "2.0", "method": "$/cancelRequest", "params": {"id": 7}})

send({"jsonrpc": "2.0", "method": "textDocument/didClose", "params": {"textDocument": {"uri": uri}}})
check(recv()["params"]["diagnostics"] == [], "close clears diagnostics")

send({"jsonrpc": "2.0", "id": 2, "method": "shutdown"})
check(recv()["id"] == 2, "shutdown reply")
send({"jsonrpc": "2.0", "method": "exit"})
check(proc.wait(timeout=10) == 0, "exit code 0 after shutdown")
print("lsp smoke OK")
