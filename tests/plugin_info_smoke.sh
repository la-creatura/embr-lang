#!/usr/bin/env bash
# `embr plugins` and `embr plugin NAME` against the real binary: the list shows every installed plugin with its version, kind and
# capabilities (and why one cannot be loaded), the reference shows every function with its signature and doc, --json is valid
# JSON with the same facts, and the language server shows the doc on hover.
#
#   plugin_info_smoke.sh <embr binary>
set -u
embr="$1"
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
fail=0
export EMBR_PATH="$(dirname "$embr")"

has() {   # name, text, substring that must appear
    [[ "$2" == *"$3"* ]] || { echo "FAIL [$1]: expected '$3' in: ${2:0:400}"; fail=1; }
}
lacks() {
    [[ "$2" != *"$3"* ]] || { echo "FAIL [$1]: '$3' should not be in: ${2:0:400}"; fail=1; }
}

list="$("$embr" plugins 2>&1)"
has "list header" "$list" "NAME"
has "list shows a C++ plugin" "$list" "table"
has "list shows a C plugin" "$list" "json"
has "list shows capabilities" "$list" "process, env"
has "list says why a plugin cannot be loaded" "$list" "built for plugin ABI 2"
has "list says why a C++ plugin cannot be loaded" "$list" "built with a different compiler"

one="$("$embr" plugin json 2>&1)"
has "plugin json: name and version" "$one" "json 1.0.0"
has "plugin json: kind" "$one" "written in C"
has "plugin json: needs" "$one" "needs: none"
has "plugin json: a function with its signature" "$one" "json_parse(text: str, opts?: map) -> any"
has "plugin json: a doc line" "$one" "raises with a position on malformed input"

cxx="$("$embr" plugin table 2>&1)"
has "plugin table: kind" "$cxx" "written in C++"
has "plugin table: build id" "$cxx" "built with: cxx-abi"
has "plugin table: signature read from the bound function" "$cxx" "table_get(t: ptr, key)"
has "plugin table: doc" "$cxx" "raises \"key not found"

caps="$("$embr" plugin http 2>&1)"
has "plugin http: capabilities" "$caps" "needs: filesystem, network"
opt="$("$embr" plugin embrlib 2>&1)"
has "plugin embrlib: optional capability" "$opt" "uses when allowed: filesystem"

bad="$("$embr" plugin cp_badbuild 2>&1)"; rc=$?
(( rc == 1 )) || { echo "FAIL [plugin with a wrong build id]: exit $rc, want 1"; fail=1; }
has "plugin with a wrong build id" "$bad" "different compiler, standard library or embr C++ interface"
# the same C++ ABI from another toolchain (clang for gcc on libstdc++): loaded, with a warning that names both
abi_script="$(mktemp --suffix=.embr 2>/dev/null || mktemp)"; printf 'import "cp_sameabi"\nprint(cp_sameabi_ok())\n' > "$abi_script"
same="$("$embr" "$abi_script" 2>&1)"; rc=$?
(( rc == 0 )) || { echo "FAIL [a plugin of the same ABI from another toolchain]: exit $rc: $same"; fail=1; }
has "same ABI: it loads and runs" "$same" "$(printf '\n1')"
has "same ABI: the warning names the exact toolchains" "$same" "some other compiler 9.9"
has "same ABI: and the shared ABI" "$same" "both have the C++ ABI $("$embr" abi | head -1)"
quiet="$(EMBR_NO_ABI_WARNINGS=1 "$embr" "$abi_script" 2>&1)"
lacks "same ABI: the warning can be switched off" "$quiet" "warning"
rm -f "$abi_script"
quiet="$("$embr" plugin cp_sameabi 2>&1)"
has "same ABI: the plugin reference shows the ABI" "$quiet" "C++ ABI: $("$embr" abi | head -1)"
list="$("$embr" plugins 2>&1)"
has "same ABI: it is not listed as broken" "$list" "cp_sameabi"
lacks "same ABI: no problem line for it" "$(echo "$list" | grep -A1 cp_sameabi)" "different compiler"
abi="$("$embr" abi)"
has "embr abi: the tag" "$abi" "cxx2-"
has "embr abi: the exact toolchain" "$abi" "cxx-abi 2;"
has "the other ABI is refused with both tags" "$bad" "the plugin: cxx1-some-other-abi"
none="$("$embr" plugin no_such_plugin_xyz 2>&1)"; rc=$?
(( rc == 1 )) || { echo "FAIL [unknown plugin]: exit $rc, want 1"; fail=1; }
has "unknown plugin" "$none" "no_such_plugin_xyz"

# the JSON is valid and says the same
if command -v python3 >/dev/null 2>&1; then
    "$embr" plugins --json > "$work/list.json" 2>&1
    "$embr" plugin table --json > "$work/regex.json" 2>&1
    python3 - "$work/list.json" "$work/regex.json" <<'PY' || fail=1
import json, sys
lst = json.load(open(sys.argv[1]))
names = {p["name"]: p for p in lst}
ok = True
def check(c, msg):
    global ok
    if not c: print("FAIL [json]:", msg); ok = False
check("table" in names and names["table"]["kind"] == "cxx", "table is a cxx plugin")
check(names["json"]["kind"] == "c" and names["json"]["functions"] == 3, "json is a c plugin with 3 functions")
check(names["os"]["capabilities"] == ["process", "env"], "os needs process and env")
check(names["embrlib"]["optional_capabilities"] == ["filesystem"], "embrlib uses the filesystem when allowed")
check(names["cp_badbuild"]["problem"] != "", "cp_badbuild has a problem")
r = json.load(open(sys.argv[2]))
fns = {f["name"]: f for f in r["functions"]}
check(r["name"] == "table" and r["abi"] == 1 and r["build_id"].startswith("cxx-abi"), "table facts")
check(fns["table_get"]["signature"].startswith("fn table_get("), "table_get signature")
check(fns["table_get"]["doc"] != "", "table_get doc")
check(all(f["doc"] != "" for f in r["functions"]), "every table function is documented")
sys.exit(0 if ok else 1)
PY
fi

# the language server shows the doc next to the signature on hover
if command -v python3 >/dev/null 2>&1; then
    printf 'import "regex"\nre_test("a", "a")\n' > "$work/h.embr"
    python3 - "$embr" "$work/h.embr" <<'PY' || fail=1
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
text = open(path).read()
send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}}); read()
send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {"textDocument": {"uri": uri, "languageId": "embr", "version": 1, "text": text}}})
send({"jsonrpc": "2.0", "id": 2, "method": "textDocument/hover", "params": {"textDocument": {"uri": uri}, "position": {"line": 1, "character": 2}}})
ans = None
for _ in range(10):
    m = read()
    if m.get("id") == 2: ans = m; break
p.terminate()
v = json.dumps(ans)
if "1 if it matches anywhere, else 0" not in v: print("FAIL [lsp hover doc]: no doc line in", v[:300]); sys.exit(1)
PY
fi

(( fail )) && exit 1
echo "plugin info smoke OK"
