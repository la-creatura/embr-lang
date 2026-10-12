#!/usr/bin/env bash
# `embr module`, `embr modules` and `embr check` against the real binary, for script modules: the declaration (__module__), the
# capabilities inferred from imports, the functions with their doc comments, the JSON, a wrong declaration reported at its line,
# and the editor's hover showing a module function's comment. nothing in a module is run.
#
#   module_info_smoke.sh <embr binary>
set -u
embr="$1"
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
fail=0
export EMBR_PATH="$(dirname "$embr")"

has() { [[ "$2" == *"$3"* ]] || { echo "FAIL [$1]: expected '$3' in: ${2:0:500}"; fail=1; }; }
lacks() { [[ "$2" != *"$3"* ]] || { echo "FAIL [$1]: '$3' should not be in: ${2:0:500}"; fail=1; }; }

mkdir -p "$work/modules"
cat > "$work/modules/demo.embr" <<'EMBR'
print("MUST-NOT-RUN")
__module__ = {
    "version": "0.3.0",
    "description": "reads a config file and talks to a server",
    "capabilities": ["filesystem", "network"],
}
import "http"
import "fs"
import "str"

# fetches a url and gives back the body.
# raises on a network error.
fn fetch(url: str) -> str
    return http_get(url)["body"]
end

# squares a number
sq(x) = x * x

fn no_doc(a, b)
    return a
end

fn __private_helper()
    return 1
end

fn with_lambda(items)
    return for_each_do(items, fn(x) return x end)
end
EMBR
printf 'import "fs"\nfn read_it(p)\n    return fs_exists(p)\nend\n' > "$work/modules/inferred.embr"
printf 'which = "str"\nimport which\nfn f()\n    return 1\nend\n' > "$work/modules/dynamic.embr"
printf '__module__ = {"capabilities": ["network"]}\nimport "fs"\nfn f()\n    return 1\nend\n' > "$work/modules/liar.embr"

out="$("$embr" module "$work/modules/demo.embr" 2>&1)"; rc=$?
(( rc == 0 )) || { echo "FAIL [module demo]: exit $rc"; fail=1; }
has "module: header" "$out" "demo 0.3.0  reads a config file and talks to a server"
has "module: needs" "$out" "needs: filesystem, network"
has "module: an import and what it needs" "$out" "http (plugin)  needs filesystem, network"
has "module: signature" "$out" "fetch(url: str) -> str"
has "module: doc from the comment block above" "$out" "fetches a url and gives back the body. raises on a network error."
has "module: expression form function" "$out" "sq(x)"
has "module: a function with no comment is listed" "$out" "no_doc(a, b)"
has "module: a function with a lambda does not crash the reader" "$out" "with_lambda(items)"
lacks "module: private helpers are not listed" "$out" "__private_helper"
lacks "module: nothing was run" "$out" "MUST-NOT-RUN"

inf="$("$embr" module "$work/modules/inferred.embr" 2>&1)"
has "inferred: no declaration" "$inf" "no __module__ (everything below is inferred)"
has "inferred: capabilities come from the import" "$inf" "needs: filesystem"
dyn="$("$embr" module "$work/modules/dynamic.embr" 2>&1)"
has "dynamic: a computed import is reported" "$dyn" "computed path, which cannot be followed"
liar="$("$embr" module "$work/modules/liar.embr" 2>&1)"; rc=$?
(( rc == 1 )) || { echo "FAIL [module with a problem]: exit $rc, want 1"; fail=1; }
has "problem: a declaration that leaves out an import" "$liar" 'declares network but imports "fs", which needs filesystem'

# a module by name, found like import finds it (in ./modules)
byname="$(cd "$work" && "$embr" module demo 2>&1)"
has "module by name" "$byname" "demo 0.3.0"
none="$("$embr" module no_such_module_xyz 2>&1)"; rc=$?
(( rc == 1 )) || { echo "FAIL [unknown module]: exit $rc, want 1"; fail=1; }
has "unknown module" "$none" "no_such_module_xyz"

# the list: the modules shipped with embr, and the ones in ./modules
list="$(cd "$work" && "$embr" modules 2>&1)"
has "modules: header" "$list" "NAME"
has "modules: a shipped module" "$list" "pkg"
has "modules: a project module" "$list" "reads a config file and talks to a server"
has "modules: a problem is shown" "$list" "problem: declares network"

# a compiled module (.vmbr) carries the same description, read without running it
(cd "$work/modules" && "$embr" compile demo.embr -o demo.vmbr >/dev/null && "$embr" compile inferred.embr -o inferred.vmbr >/dev/null)
cm="$("$embr" module "$work/modules/demo.vmbr" 2>&1)"; rc=$?
(( rc == 0 )) || { echo "FAIL [compiled module demo]: exit $rc"; fail=1; }
has "compiled: header" "$cm" "demo 0.3.0  reads a config file and talks to a server"
has "compiled: says so" "$cm" "compiled module, declares __module__"
has "compiled: needs" "$cm" "needs: filesystem, network"
has "compiled: an import" "$cm" "http (plugin)  needs filesystem, network"
has "compiled: doc" "$cm" "fetches a url and gives back the body. raises on a network error."
lacks "compiled: nothing was run" "$cm" "MUST-NOT-RUN"
ci="$("$embr" module "$work/modules/inferred.vmbr" 2>&1)"
has "compiled: capabilities are worked out again from the imports" "$ci" "needs: filesystem"
cj="$("$embr" module "$work/modules/inferred.vmbr" --json 2>&1)"
has "compiled: json" "$cj" '"compiled": true'
printf 'import "demo.vmbr"\nfn g()\n    return 1\nend\n' > "$work/modules/usesdemo.embr"
nested="$("$embr" module "$work/modules/usesdemo.embr" 2>&1)"
has "a script that imports a compiled module inherits its needs" "$nested" "needs: filesystem, network"
cname="$(cd "$work" && "$embr" module demo.vmbr 2>&1)"
has "compiled module by file name" "$cname" "demo 0.3.0"
clist="$(cd "$work" && "$embr" modules 2>&1)"
has "modules: a compiled module is listed" "$clist" "demo"
# a file cached by --cache-dir has no description, and a damaged one is reported
printf 'fn c()\n    return 1\nend\n' > "$work/cached.embr"
(cd "$work" && "$embr" --cache-dir="$work/cache" cached.embr >/dev/null 2>&1)
cf="$(ls "$work"/cache/cached-*.vmbr 2>/dev/null | head -1)"
if [[ -n "$cf" ]]; then
    nd="$("$embr" module "$cf" 2>&1)"
    has "compiled: a file written without a description" "$nd" "written without a description"
else
    echo "FAIL [cache file]: --cache-dir wrote nothing"; fail=1
fi
printf 'import "embrmeta"\nimport "embrlib"\nm = meta_module_file("%s")\nprint(m["version"], m["compiled"], m["capabilities"], len(m["functions"]))\n' "$work/modules/demo.vmbr" > "$work/mm.embr"
mm="$("$embr" "$work/mm.embr" 2>&1)"
has "meta_module_file reads a compiled module" "$mm" '0.3.0 1 ["filesystem", "network"] 4'
printf 'import "embrmeta"\nm = meta_module_file("%s")\n' "$cf" > "$work/mm2.embr"
mm2="$("$embr" "$work/mm2.embr" 2>&1)"
has "meta_module_file on a file with no description" "$mm2" "compiled module written without a description"
head -c 60 "$work/modules/demo.vmbr" > "$work/modules/torn.vmbr"
torn="$("$embr" module "$work/modules/torn.vmbr" 2>&1)"; rc=$?
(( rc == 1 )) || { echo "FAIL [torn compiled module]: exit $rc, want 1"; fail=1; }

if command -v python3 >/dev/null 2>&1; then
    "$embr" module "$work/modules/demo.embr" --json > "$work/demo.json" 2>&1
    (cd "$work" && "$embr" modules --json > "$work/list.json" 2>&1)
    python3 - "$work/demo.json" "$work/list.json" <<'PY' || fail=1
import json, sys
d = json.load(open(sys.argv[1]))
ok = True
def check(c, msg):
    global ok
    if not c: print("FAIL [json]:", msg); ok = False
check(d["name"] == "demo" and d["version"] == "0.3.0" and d["declared"], "demo facts")
check(d["capabilities"] == ["filesystem", "network"] and d["declared_capabilities"] == d["inferred_capabilities"] == ["filesystem", "network"], "demo capabilities")
check([i["path"] for i in d["imports"]] == ["http", "fs", "str"] and d["imports"][0]["kind"] == "plugin", "imports")
fns = {f["name"]: f for f in d["functions"]}
check(set(fns) == {"fetch", "sq", "no_doc", "with_lambda"}, "functions: %s" % sorted(fns))
check(fns["fetch"]["signature"] == "fn fetch(url: str) -> str" and fns["fetch"]["line"] > 1, "fetch signature")
check(fns["no_doc"]["doc"] == "", "no_doc has no doc")
check(d["problems"] == [], "no problems")
lst = {m["name"]: m for m in json.load(open(sys.argv[2]))}
check({"pkg", "ffi", "parallel", "testing", "demo", "liar"} <= set(lst), "listed modules: %s" % sorted(lst))
check(lst["pkg"]["capabilities"] == ["filesystem", "network"] and lst["pkg"]["version"] == "1.0.0", "pkg")
check(lst["liar"]["problems"] != [], "liar has a problem")
sys.exit(0 if ok else 1)
PY
fi

# embr check shows a wrong declaration where it is, with the same wording
printf '__module__ = {"version": 1 + 1, "bogus": "x", "capabilities": ["nope"]}\n' > "$work/badmeta.embr"
chk="$("$embr" check "$work/badmeta.embr" 2>&1)"; rc=$?
(( rc != 0 )) || { echo "FAIL [check badmeta]: exit 0, want an error"; fail=1; }
has "check: not a literal" "$chk" 'badmeta.embr:1:26: module: "version" in __module__ must be a string written out in the file'
has "check: unknown key" "$chk" 'unknown key "bogus" in __module__'
has "check: not a capability" "$chk" "'nope' is not a capability"
"$embr" check "$work/modules/demo.embr" >/dev/null 2>&1 || { echo "FAIL [check demo]: a good module did not check"; fail=1; }
chk2="$("$embr" check "$work/modules/liar.embr" 2>&1)"; rc=$?
(( rc != 0 )) || { echo "FAIL [check liar]: exit 0, want an error"; fail=1; }
has "check: a declaration that leaves out an import" "$chk2" 'liar.embr:2:1: module: declares network but imports "fs"'

# the language server shows the comment above an imported module's function on hover
if command -v python3 >/dev/null 2>&1; then
    printf 'import "demo.embr"\nfetch("http://x")\n' > "$work/modules/use.embr"
    python3 - "$embr" "$work/modules/use.embr" <<'PY' || fail=1
import json, subprocess, sys
embr, path = sys.argv[1], sys.argv[2]
p = subprocess.Popen([embr, "lsp"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
def send(m):
    b = json.dumps(m).encode()
    p.stdin.write(b"Content-Length: %d\r\n\r\n" % len(b) + b); p.stdin.flush()
def read():
    n = 0
    while True:
        line = p.stdout.readline()
        if line in (b"\r\n", b""): break
        if line.lower().startswith(b"content-length:"): n = int(line.split(b":")[1])
    return json.loads(p.stdout.read(n))
uri = "file://" + path
send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}}); read()
send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {"textDocument": {"uri": uri, "languageId": "embr", "version": 1, "text": open(path).read()}}})
send({"jsonrpc": "2.0", "id": 2, "method": "textDocument/hover", "params": {"textDocument": {"uri": uri}, "position": {"line": 1, "character": 2}}})
send({"jsonrpc": "2.0", "id": 3, "method": "textDocument/completion", "params": {"textDocument": {"uri": uri}, "position": {"line": 1, "character": 2}}})
ans = {}
for _ in range(20):
    m = read()
    if m.get("id") in (2, 3): ans[m["id"]] = m
    if len(ans) == 2: break
p.terminate()
v = json.dumps(ans.get(2))
if "fetches a url and gives back the body. raises on a network error." not in v:
    print("FAIL [lsp hover on a module function]: no doc line in", v[:400]); sys.exit(1)
items = (ans.get(3) or {}).get("result") or []
fetch = [i for i in items if i["label"] == "fetch"]
if not fetch or "fetches a url" not in fetch[0].get("documentation", ""):
    print("FAIL [lsp completion of a module function]: no documentation in", json.dumps(fetch)[:400]); sys.exit(1)
PY
fi

(( fail )) && exit 1
echo "module info smoke OK"
