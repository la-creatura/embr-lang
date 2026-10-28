#!/usr/bin/env bash
# `embr pack`, `embr package`, `embr packages`, `embr unpack` and `import` of a .pmbr package (include/embr/core/package.h) against the
# real binary: a plugin package, a module package, a module that wraps a bundled C library, and what is refused (a sandbox that denies
# a capability, a damaged or changed file, a package that claims less than its plugin needs, a name that does not match).
#
#   package_smoke.sh <embr binary>
set -u
embr="$1"
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
fail=0

# sed -i differs between GNU and BSD (macos) sed, so edit through a second file
sedi() { sed "$1" "$2" > "$2.sedi" && mv "$2.sedi" "$2"; }
export EMBR_PATH="$(dirname "$embr")"
export EMBR_PACKAGE_DIR="$work/cache"
export XDG_CONFIG_HOME="$work/config"

has() { [[ "$2" == *"$3"* ]] || { echo "FAIL [$1]: expected '$3' in: ${2:0:600}"; fail=1; }; }
lacks() { [[ "$2" != *"$3"* ]] || { echo "FAIL [$1]: '$3' should not be in: ${2:0:600}"; fail=1; }; }

case "$(uname -s)" in Linux) os=linux ;; Darwin) os=macos ;; *) echo "package smoke: skipped on this platform"; exit 0 ;; esac
case "$(uname -m)" in x86_64|amd64) cpu=x86_64 ;; aarch64|arm64) cpu=arm64 ;; *) echo "package smoke: skipped on this cpu"; exit 0 ;; esac
target="$os-$cpu"
plugdir="$(dirname "$embr")/plugins"
ext=so; [[ "$os" == macos ]] && ext=dylib

mkdir -p "$work/packages"
cd "$work"

# ---- a plugin package: the real path plugin
mkdir -p "src_path/lib/$target"
cp "$plugdir/path.$ext" "src_path/lib/$target/path.$ext" 2>/dev/null || cp "$plugdir/path.so" "src_path/lib/$target/path.$ext"
cat > src_path/meta.embr <<EOF
__package__ = {
    "name": "path",
    "version": "1.0.0",
    "description": "pure path-string utilities",
    "capabilities": [],
    "plugin": {"$target": "lib/$target/path.$ext"},
}
EOF
out="$("$embr" pack src_path -o packages/path.pmbr 2>&1)"; rc=$?
(( rc == 0 )) || { echo "FAIL [pack a plugin]: exit $rc: $out"; fail=1; }
has "pack output" "$out" "wrote packages/path.pmbr (path 1.0.0, 2 files"

# ---- a module package
mkdir -p src_greeter
cat > src_greeter/greeter.embr <<'EOF'
__module__ = {"version": "1.0.0", "description": "says hello", "capabilities": []}
print("GREETER-RAN")

# a greeting for a name
fn greet(name)
    return "hello " + name
end
EOF
cat > src_greeter/meta.embr <<'EOF'
__package__ = {"name": "greeter", "version": "1.0.0", "description": "says hello", "capabilities": [], "module": "greeter.embr"}
EOF
"$embr" pack src_greeter -o packages/greeter.pmbr >/dev/null 2>&1 || { echo "FAIL [pack a module]"; fail=1; }

# ---- a module that wraps a bundled library
have_cc=0
if command -v cc >/dev/null 2>&1; then
    mkdir -p "src_adder/lib/$target"
    echo 'int adder_add(int a, int b) { return a + b; }' > libadder.c
    if cc -shared -fPIC -O2 libadder.c -o "src_adder/lib/$target/libadder.$ext" 2>/dev/null; then
        have_cc=1
        cat > src_adder/adder.embr <<EOF
__module__ = {"version": "1.0.0", "description": "adds through a bundled C library", "capabilities": ["native"]}
import "ffi.embr"
import "package"
__lib = ffi_open(package_path("adder", "lib/{target}/libadder.$ext"))
ffi_cdef(__lib, "int adder_add(int a, int b);")
EOF
        cat > src_adder/meta.embr <<EOF
__package__ = {
    "name": "adder", "version": "1.0.0", "description": "adds through a bundled C library",
    "capabilities": ["native"], "module": "adder.embr", "libraries": ["lib/{target}/libadder.$ext"],
}
EOF
        "$embr" pack src_adder -o packages/adder.pmbr >/dev/null 2>&1 || { echo "FAIL [pack a module with a library]"; fail=1; }
    fi
fi

run() { printf '%s\n' "$1" > t.embr; out="$("$embr" "${@:2}" t.embr 2>&1)"; rc=$?; }

# ---- importing them
run 'import "path"
print(path_join("a", "b"))'
has "a plugin package works" "$out" "a/b"
has "the plugin ran from the unpacked folder" "$out" "$work/cache/path-"
run 'import "greeter"
print(greet("bob"))'
has "a module package works" "$out" "hello bob"
has "its top-level code ran" "$out" "GREETER-RAN"
run 'import "greeter"
import "greeter"
print(greet("x"))'
(( $(grep -c GREETER-RAN <<<"$out") == 1 )) || { echo "FAIL [a package imported twice runs once]: $out"; fail=1; }
if (( have_cc )); then
    run 'import "adder"
print(adder_add(2, 40))'
    has "a module with a bundled library works" "$out" "42"
fi

# ---- package_path in a worker for a package only the parent imported
cat > pp_worker.embr <<'EOF'
import "package"
fn where(x)
    return package_path("greeter", "greeter.embr")
end
EOF
run 'import "greeter"
import "parallel"
import "async"
i = channel()
o = channel()
w = worker_spawn("pp_worker.embr", "where", i, o)
channel_send(i, 1)
channel_close(i)
print(channel_recv(o)["value"])
worker_join(w)
p = async_spawn("pp_worker.embr", "where", 1)
print(async_await(p))'
has "package_path in a worker" "$out" "$work/cache/greeter-"
(( $(grep -c "/greeter.embr" <<<"$out") == 2 )) || { echo "FAIL [package_path in a parallel worker and an async task]: $out"; fail=1; }

# ---- reading them
list="$("$embr" packages 2>&1)"
has "packages: header" "$list" "NAME"
has "packages: a plugin package" "$list" "pure path-string utilities"
has "packages: a module package" "$list" "says hello"
info="$("$embr" package packages/greeter.pmbr 2>&1)"; rc=$?
(( rc == 0 )) || { echo "FAIL [package info]: exit $rc"; fail=1; }
has "package: kind" "$info" "a module package"
has "package: entry point" "$info" "entry point: greeter.embr"
has "package: files" "$info" "greeter.embr  ("
pj="$("$embr" package packages/path.pmbr --json 2>&1)"
has "package json: targets" "$pj" "\"targets\": [\"$target\"]"
has "package json: kind" "$pj" '"kind": "plugin"'
byname="$("$embr" package greeter 2>&1)"
has "package by name" "$byname" "greeter 1.0.0"
if (( have_cc )); then
    aj="$("$embr" package packages/adder.pmbr --json 2>&1)"
    has "a bundled library is shown" "$aj" "libadder"
    has "a bundled library means native" "$aj" '"capabilities": ["native"]'
fi

# the same inputs give the same bytes
"$embr" pack src_greeter -o again.pmbr >/dev/null 2>&1
cmp -s again.pmbr packages/greeter.pmbr || { echo "FAIL [same inputs, same bytes]"; fail=1; }

# unpack gives back the files
"$embr" unpack packages/greeter.pmbr out_greeter >/dev/null 2>&1 || { echo "FAIL [unpack]"; fail=1; }
cmp -s out_greeter/greeter.embr src_greeter/greeter.embr || { echo "FAIL [unpack: a file differs]"; fail=1; }
[[ -f out_greeter/.header ]] || { echo "FAIL [unpack: no .header]"; fail=1; }

# ---- what is refused
if (( have_cc )); then
    rm -rf "$work/cache"
    # a host that denies capabilities only looks in the places it controls (the folders of embr and EMBR_PATH), never next to the script
    printf '%s\n' 'try
  import "adder"
  print("LOADED-adder")
catch e
  print(e["message"])
end' > t.embr
    out="$(EMBR_PATH="$EMBR_PATH:$work/packages" "$embr" --deny-caps=native t.embr 2>&1)"; rc=$?
    has "a package that needs a denied capability" "$out" 'package "adder" is not allowed in this sandbox: it needs native'
    lacks "refused before it ran" "$out" "LOADED-adder"
    [[ -z "$(ls "$work/cache" 2>/dev/null)" ]] || { echo "FAIL [refused before unpacking]: $(ls "$work/cache")"; fail=1; }
fi
run 'try
  import "greeter"
catch e
  print(e["message"])
end' --sandbox
has "a sandbox does not look next to the script" "$out" 'cannot load plugin "greeter"'
printf '%s\n' 'try
  import "greeter"
catch e
  print(e["message"])
end' > t.embr
out="$(EMBR_PATH="$EMBR_PATH:$work/packages" "$embr" --sandbox t.embr 2>&1)"
has "a sandbox loads a package from a folder the host named" "$out" "GREETER-RAN"

# damaged files
python3 - "$work" <<'PY'
import sys, shutil
w = sys.argv[1]
d = open(w + "/packages/greeter.pmbr", "rb").read()
def save(name, b): open(w + "/bad/" + name + ".pmbr", "wb").write(b)
import os; os.makedirs(w + "/bad", exist_ok=True)
save("greeter", d[:-1] + bytes([d[-1] ^ 1]))                       # a byte of the last entry (the data is after the table)
save("cut", d[:60])                                                 # cut short inside the table
h = bytearray(d); h[5] ^= 1; save("format", bytes(h))              # the file format number
m = bytearray(d); m[0] = ord('X'); save("magic", bytes(m))
t = bytearray(d); t[20] ^= 1; save("table", bytes(t))              # inside the table of contents
PY
mkdir -p dmg
for f in greeter cut format magic table; do
    cp "bad/$f.pmbr" dmg/greeter.pmbr
    run 'try
  import "greeter"
  print("LOADED-greeter")
catch e
  print(e["message"])
end'
    out="$(cd dmg && printf 'try\n  import "greeter"\n  print("LOADED-greeter")\ncatch e\n  print(e["message"])\nend\n' > t.embr && "$embr" t.embr 2>&1)"
    lacks "damaged ($f) is not loaded" "$out" "LOADED-greeter"
    lacks "damaged ($f) does not run" "$out" "GREETER-RAN"
    has "damaged ($f) says so" "$out" 'package "greeter"'
done
has_edge="$(cd dmg && cp ../bad/greeter.pmbr greeter.pmbr && printf 'import "greeter"\n' > t.embr && "$embr" t.embr 2>&1)"
has "a changed byte is found by its sha256" "$has_edge" "does not match its sha256"

# a file name that is not the package name
cp packages/greeter.pmbr dmg/other.pmbr
out="$(cd dmg && printf 'import "other"\n' > t.embr && "$embr" t.embr 2>&1)"
has "the file name must be the package name" "$out" 'the file is called other.pmbr but the package calls itself "greeter"'

# the plugin is not what the package says
mkdir -p src_liar/lib/$target
cp "src_path/lib/$target/path.$ext" "src_liar/lib/$target/path.$ext"
cat > src_liar/meta.embr <<EOF
__package__ = {"name": "str", "version": "1.0.0", "description": "claims to be str", "capabilities": [], "plugin": {"$target": "lib/$target/path.$ext"}}
EOF
out="$("$embr" pack src_liar -o liar.pmbr 2>&1)"; rc=$?
(( rc != 0 )) || { echo "FAIL [pack refuses a library that is not the package]"; fail=1; }
has "pack: the library says another name" "$out" 'the package is called "str" but its library says it is "path"'
# a package made by hand (the packer would refuse it): the loader refuses it too
python3 - "$work" "$target" "$ext" <<'PY'
import sys, hashlib, struct
w, target, ext = sys.argv[1:4]
def pack(abi, version, files):
    files = sorted(files.items())
    head = b"PMBR" + struct.pack("<IIII", 1, abi, len(files), len(version)) + version.encode()
    toc = b""
    off = len(head) + sum(2 + len(l) + 8 + 8 + 32 for l, _ in files) + 32
    for l, d in files:
        toc += struct.pack("<H", len(l)) + l.encode() + struct.pack("<QQ", off, len(d)) + hashlib.sha256(d).digest()
        off += len(d)
    h = head + toc
    return h + hashlib.sha256(h).digest() + b"".join(d for _, d in files)
lib = open(f"{w}/src_path/lib/{target}/path.{ext}", "rb").read()
def meta(name, version="1.0.0", caps="[]"):
    return (f'__package__ = {{"name": "{name}", "version": "{version}", "description": "x", "capabilities": {caps}, "plugin": {{"{target}": "lib/{target}/path.{ext}"}}}}\n').encode()
import os; os.makedirs(w + "/hand", exist_ok=True)
open(w + "/hand/path.pmbr", "wb").write(pack(1, "1.0.0", {"meta.embr": meta("path", "9.9.9"), f"lib/{target}/path.{ext}": lib}))   # the library says 1.0.0
os.makedirs(w + "/hand2", exist_ok=True)
open(w + "/hand2/path.pmbr", "wb").write(pack(77, "1.0.0", {"meta.embr": meta("path"), f"lib/{target}/path.{ext}": lib}))             # built for another plugin interface
os.makedirs(w + "/hand3", exist_ok=True)
m3 = (f'__package__ = {{"name": "path", "version": "1.0.0", "description": "x", "capabilities": [], "plugin": {{"windows-x86_64": "lib/{target}/path.{ext}"}}}}\n').encode()
open(w + "/hand3/path.pmbr", "wb").write(pack(1, "1.0.0", {"meta.embr": m3, f"lib/{target}/path.{ext}": lib}))   # no build for this platform
PY
out="$(cd hand && printf 'import "path"\n' > t.embr && "$embr" t.embr 2>&1)"
has "the library's version must be the package's" "$out" "the package says version 9.9.9 but its library says 1.0.0"
out="$(cd hand2 && printf 'import "path"\n' > t.embr && "$embr" t.embr 2>&1)"
has "another plugin interface" "$out" "built for plugin interface 77"
out="$(cd hand3 && printf 'import "path"\n' > t.embr && "$embr" t.embr 2>&1)"
has "no build for this platform" "$out" "has no build for $target"

# a file changed or removed after unpacking is not run
run 'import "path"'
path_id="$("$embr" package packages/path.pmbr --json | sed -n 's/.*"id": "\([0-9a-f]\{32\}\).*/\1/p')"
cache_lib="$work/cache/path-$path_id/lib/$target/path.$ext"
[[ -f "$cache_lib" ]] || cache_lib=""
if [[ -n "$cache_lib" ]]; then
    cp "$cache_lib" "$work/saved_lib"
    echo "junk" >> "$cache_lib"
    run 'import "path"'
    has "a library changed after unpacking" "$out" "was changed after it was unpacked"
    rm -f "$cache_lib"
    run 'import "path"'
    has "a library removed after unpacking" "$out" "is gone"
    cp "$work/saved_lib" "$cache_lib"
    run 'import "path"
print(path_join("a", "b"))'
    has "put back, it loads again" "$out" "a/b"
else
    echo "FAIL [cache]: no unpacked library found"; fail=1
fi

# pack refuses a mistake
mkdir -p src_bad1; cp src_greeter/greeter.embr src_bad1/; cat > src_bad1/meta.embr <<'EOF'
__package__ = {"name": "bad1", "version": "1.0.0", "description": "x", "capabilities": [], "module": "nothere.embr"}
EOF
out="$("$embr" pack src_bad1 -o bad1.pmbr 2>&1)"; rc=$?
(( rc != 0 )) || { echo "FAIL [pack: a missing entry point]"; fail=1; }
has "pack: a missing entry point" "$out" "not in the package"
mkdir -p src_bad2; printf 'import "fs"\nfn f() return 1 end\n' > src_bad2/m.embr; cat > src_bad2/meta.embr <<'EOF'
__package__ = {"name": "bad2", "version": "1.0.0", "description": "x", "capabilities": [], "module": "m.embr"}
EOF
out="$("$embr" pack src_bad2 -o bad2.pmbr 2>&1)"
has "pack: a module needs more than the package declares" "$out" "needs filesystem"
mkdir -p src_bad3; echo 'fn f() return 1 end' > src_bad3/m.embr; echo x > src_bad3/lib.so; cat > src_bad3/meta.embr <<'EOF'
__package__ = {"name": "bad3", "version": "1.0.0", "description": "x", "capabilities": [], "module": "m.embr", "libraries": ["lib.so"]}
EOF
out="$("$embr" pack src_bad3 -o bad3.pmbr 2>&1)"
has "pack: a bundled library means native" "$out" "capabilities\" must include native"
mkdir -p src_bad4; echo 'fn f() return 1 end' > src_bad4/m.embr; cat > src_bad4/meta.embr <<'EOF'
__package__ = {"name": "bad4", "version": "1.0.0", "description": "x", "capabilities": [], "module": "m.embr", "plugin": {"linux-x86_64": "m.embr"}}
EOF
out="$("$embr" pack src_bad4 -o bad4.pmbr 2>&1)"
has "pack: a module and a plugin" "$out" "exactly one entry point"
mkdir -p src_bad5; echo 'fn f() return 1 end' > src_bad5/m.embr; echo 'x' > src_bad5/meta.embr
out="$("$embr" pack src_bad5 -o bad5.pmbr 2>&1)"
has "pack: a meta that does not parse" "$out" "meta.embr"
out="$("$embr" pack no_such_dir -o x.pmbr 2>&1)"; rc=$?
(( rc != 0 )) || { echo "FAIL [pack: no folder]"; fail=1; }

# package_path will not leave the package
if (( have_cc )); then
    run 'import "adder"
import "package"
try
  package_path("adder", "../escape")
catch e
  print(e["message"])
end
try
  package_path("adder", "nothing")
catch e
  print(e["message"])
end
try
  package_dir("nothere")
catch e
  print(e["message"])
end
print(len(package_files("adder")))'
    has "package_path: a label that leaves the folder" "$out" "is not a file name inside a package"
    has "package_path: a file that is not there" "$out" "has no file 'nothing'"
    has "package_dir: a package not imported" "$out" "has not been imported"
    has "package_files" "$out" "3"
fi

# ---- embr pkg: a registry that serves packages
mkdir -p reg/a proj
cp packages/greeter.pmbr reg/a/
sum_greeter="$(sha256sum reg/a/greeter.pmbr 2>/dev/null | cut -d' ' -f1)"
[[ -n "$sum_greeter" ]] || sum_greeter="$(shasum -a 256 reg/a/greeter.pmbr | cut -d' ' -f1)"
idx_adder=""
if (( have_cc )); then
    cp packages/adder.pmbr reg/a/
    idx_adder=', "adder": {"versions": {"1.0.0": {"url": "/a/adder.pmbr", "type": "package", "capabilities": ["native"]}}}'
fi
cat > reg/index.json <<IDX
{"greeter": {"versions": {"1.0.0": {"url": "/a/greeter.pmbr", "type": "package", "sha256": "$sum_greeter", "capabilities": []}}}$idx_adder}
IDX
cd proj
out="$("$embr" pkg --registry "file://$work/reg" install greeter 2>&1)"
has "pkg installs a package" "$out" "installed greeter@1.0.0"
[[ -f packages/greeter.pmbr ]] || { echo "FAIL [pkg: the package is in ./packages]"; fail=1; }
printf 'import "greeter"\nprint(greet("pkg"))\n' > t.embr
out="$("$embr" t.embr 2>&1)"
has "an installed package imports" "$out" "hello pkg"
out="$("$embr" pkg --registry "file://$work/reg" info --verify greeter 2>&1)"
has "pkg info --verify reads a package" "$out" "the downloaded file needs: nothing"
if (( have_cc )); then
    out="$("$embr" pkg --registry "file://$work/reg" install adder --deny-caps=native 2>&1)"
    has "pkg refuses a package that needs a denied capability" "$out" "needs native, which is denied"
    [[ ! -f packages/adder.pmbr ]] || { echo "FAIL [pkg: a refused package is not installed]"; fail=1; }
    cat > "$work/reg/index.json" <<IDX
{"greeter": {"versions": {"1.0.0": {"url": "/a/greeter.pmbr", "type": "package", "sha256": "$sum_greeter", "capabilities": []}}}, "adder": {"versions": {"1.0.0": {"url": "/a/adder.pmbr", "type": "package", "capabilities": []}}}}
IDX
    rm -f packages/adder.pmbr
    out="$("$embr" pkg --registry "file://$work/reg" install adder 2>&1)"
    has "pkg: the registry claims less than the package declares" "$out" "the registry says adder@1.0.0 needs [] but the file also needs native"
fi
cd "$work"

# ---- requires: what a package needs must be here, at the exact version
mkdir -p src_needy
cat > src_needy/needy.embr <<'EOF'
import "greeter"
fn hello_twice(n)
    return greet(n) + " and " + greet(n)
end
EOF
cat > src_needy/meta.embr <<'EOF'
__package__ = {"name": "needy", "version": "1.0.0", "description": "needs greeter", "capabilities": [], "module": "needy.embr", "requires": {"greeter": "1.0.0"}}
EOF
"$embr" pack src_needy -o packages/needy.pmbr >/dev/null 2>&1 || { echo "FAIL [pack a package with requires]"; fail=1; }
info="$("$embr" package packages/needy.pmbr 2>&1)"
has "package: requires are shown" "$info" "requires: greeter 1.0.0"
mkdir -p lone; cp packages/needy.pmbr lone/
printf 'import "needy"\n' > lone/t.embr
out="$(cd lone && "$embr" t.embr 2>&1)"
has "requires: a package that is not installed" "$out" "it requires greeter 1.0.0, which is not installed"
mkdir -p both; cp packages/needy.pmbr packages/greeter.pmbr both/
printf 'import "needy"\nprint(hello_twice("x"))\n' > both/t.embr
out="$(cd both && "$embr" t.embr 2>&1)"
has "requires: it is there" "$out" "hello x and hello x"
mkdir -p src_greeter2; sed 's/"1.0.0"/"2.0.0"/' src_greeter/meta.embr > src_greeter2/meta.embr; cp src_greeter/greeter.embr src_greeter2/
"$embr" pack src_greeter2 -o other.pmbr >/dev/null 2>&1; mkdir -p wrongver; cp packages/needy.pmbr wrongver/; cp other.pmbr wrongver/greeter.pmbr
printf 'import "needy"\n' > wrongver/t.embr
out="$(cd wrongver && "$embr" t.embr 2>&1)"
has "requires: another version" "$out" "it requires greeter 1.0.0 but"
has "requires: another version, which" "$out" "is version 2.0.0"
# a loose plugin counts too
mkdir -p src_needs_plugin; echo 'fn f() return 1 end' > src_needs_plugin/m.embr
json_version="$("$embr" plugin json --json | sed -n 's/.*"version": "\([^"]*\)".*/\1/p' | head -1)"
cat > src_needs_plugin/meta.embr <<EOF
__package__ = {"name": "np", "version": "1.0.0", "description": "x", "capabilities": [], "module": "m.embr", "requires": {"json": "$json_version"}}
EOF
"$embr" pack src_needs_plugin -o np.pmbr >/dev/null 2>&1 || { echo "FAIL [pack: requires a plugin]"; fail=1; }
mkdir -p np_ok; cp np.pmbr np_ok/; printf 'import "np"\nprint("NP-LOADED")\n' > np_ok/t.embr
out="$(cd np_ok && "$embr" t.embr 2>&1)"
has "requires: a loose plugin at the right version" "$out" "NP-LOADED"
sedi "s/\"json\": \"$json_version\"/\"json\": \"99.0.0\"/" src_needs_plugin/meta.embr
"$embr" pack src_needs_plugin -o np2.pmbr >/dev/null 2>&1; mkdir -p np_bad; cp np2.pmbr np_bad/np.pmbr; printf 'import "np"\n' > np_bad/t.embr
out="$(cd np_bad && "$embr" t.embr 2>&1)"
has "requires: a loose plugin at another version" "$out" "it requires json 99.0.0 but"
for bad in '"greeter": "^1.0"' '"needy2": "1.0.0 || 2.0.0"' '"npx": ""'; do
    mkdir -p src_badreq; echo 'fn f() return 1 end' > src_badreq/m.embr
    printf '__package__ = {"name": "npx", "version": "1.0.0", "description": "x", "capabilities": [], "module": "m.embr", "requires": {%s}}\n' "$bad" > src_badreq/meta.embr
    out="$("$embr" pack src_badreq -o badreq.pmbr 2>&1)"; rc=$?
    (( rc != 0 )) || { echo "FAIL [pack refuses requires $bad]"; fail=1; }
done
printf '__package__ = {"name": "npx", "version": "1.0.0", "description": "x", "capabilities": [], "module": "m.embr", "requires": {"npx": "1.0.0"}}\n' > src_badreq/meta.embr
out="$("$embr" pack src_badreq -o badreq.pmbr 2>&1)"
has "pack: a package cannot require itself" "$out" "cannot require itself"

# a registry entry made from the file
entry="$("$embr" package packages/needy.pmbr --registry-entry /a/needy.pmbr 2>&1)"
want_sum="$(sha256sum packages/needy.pmbr 2>/dev/null | cut -d' ' -f1)"; [[ -n "$want_sum" ]] || want_sum="$(shasum -a 256 packages/needy.pmbr | cut -d' ' -f1)"
has "registry entry: the sha256 of the whole file" "$entry" "\"sha256\": \"$want_sum\""
has "registry entry: the dependencies" "$entry" '"deps": {"greeter": "1.0.0"}'
has "registry entry: the url and type" "$entry" '"url": "/a/needy.pmbr", "type": "package"'

# embr pkg: the registry must list what the file requires
mkdir -p reg2/a proj2; cp packages/needy.pmbr packages/greeter.pmbr reg2/a/
sum_g="$(sha256sum packages/greeter.pmbr 2>/dev/null | cut -d' ' -f1)"; [[ -n "$sum_g" ]] || sum_g="$(shasum -a 256 packages/greeter.pmbr | cut -d' ' -f1)"
cat > reg2/index.json <<IDX
{"greeter": {"versions": {"1.0.0": {"url": "/a/greeter.pmbr", "type": "package", "sha256": "$sum_g"}}},
 "needy": {"versions": {"1.0.0": {"url": "/a/needy.pmbr", "type": "package", "sha256": "$want_sum"}}}}
IDX
out="$(cd proj2 && "$embr" pkg --registry "file://$work/reg2" install needy 2>&1)"
has "pkg: the registry does not list what the package requires" "$out" "requires greeter 1.0.0 but the registry does not list that as a dependency"
cat > reg2/index.json <<IDX
{"greeter": {"versions": {"1.0.0": {"url": "/a/greeter.pmbr", "type": "package", "sha256": "$sum_g"}}},
 "needy": {"versions": {"1.0.0": {"url": "/a/needy.pmbr", "type": "package", "sha256": "$want_sum", "deps": {"greeter": "1.0.0"}}}}}
IDX
out="$(cd proj2 && "$embr" pkg --registry "file://$work/reg2" install needy 2>&1)"
has "pkg: with the dependency listed, both are installed" "$out" "installed needy@1.0.0"
[[ -f proj2/packages/greeter.pmbr && -f proj2/packages/needy.pmbr ]] || { echo "FAIL [pkg installs the dependency too]"; fail=1; }
out="$(cd proj2 && printf 'import "needy"\nprint(hello_twice("k"))\n' > t.embr && "$embr" t.embr 2>&1)"
has "pkg: the installed pair works" "$out" "hello k and hello k"

# ---- merge: the same package built on two platforms
mkdir -p src_m1/lib/$target src_m2/lib/windows-x86_64
cp "src_path/lib/$target/path.$ext" "src_m1/lib/$target/path.$ext"
echo "pretend windows library" > src_m2/lib/windows-x86_64/path.dll
cat > src_m1/meta.embr <<EOF
__package__ = {"name": "path", "version": "1.0.0", "description": "pure path-string utilities", "capabilities": [], "plugin": {"$target": "lib/$target/path.$ext"}}
EOF
cat > src_m2/meta.embr <<'EOF'
__package__ = {"name": "path", "version": "1.0.0", "description": "pure path-string utilities", "capabilities": [], "plugin": {"windows-x86_64": "lib/windows-x86_64/path.dll"}}
EOF
"$embr" pack src_m1 -o m1.pmbr >/dev/null 2>&1; "$embr" pack src_m2 -o m2.pmbr >/dev/null 2>&1 || { echo "FAIL [pack the other platform]"; fail=1; }
out="$("$embr" merge m1.pmbr m2.pmbr -o merged.pmbr 2>&1)"; rc=$?
(( rc == 0 )) || { echo "FAIL [merge]: $out"; fail=1; }
has "merge: output" "$out" "builds for $target, windows-x86_64"
mj="$("$embr" package merged.pmbr --json)"
has "merged: both targets" "$mj" "\"targets\": [\"$target\", \"windows-x86_64\"]"
has "merged: both libraries" "$mj" 'lib/windows-x86_64/path.dll'
"$embr" merge m2.pmbr m1.pmbr -o merged_b.pmbr >/dev/null 2>&1
cmp -s merged.pmbr merged_b.pmbr || { echo "FAIL [merge: the order of the inputs does not matter]"; fail=1; }
mkdir -p mm; cp merged.pmbr mm/path.pmbr; printf 'import "path"\nprint(path_join("m", "n"))\n' > mm/t.embr
out="$(cd mm && "$embr" t.embr 2>&1)"
has "merged package imports" "$out" "m/n"
# what cannot be merged
sed 's/"1.0.0"/"1.1.0"/' src_m2/meta.embr > src_m2b_meta; mkdir -p src_m2b/lib/windows-x86_64; cp src_m2b_meta src_m2b/meta.embr; cp src_m2/lib/windows-x86_64/path.dll src_m2b/lib/windows-x86_64/
"$embr" pack src_m2b -o m2b.pmbr >/dev/null 2>&1
out="$("$embr" merge m1.pmbr m2b.pmbr -o x.pmbr 2>&1)"; rc=$?
(( rc != 0 )) || { echo "FAIL [merge refuses another version]"; fail=1; }
has "merge: another version" "$out" "differ in version"
mkdir -p src_m3/lib/windows-x86_64; echo "a different windows library" > src_m3/lib/windows-x86_64/path.dll; cp src_m2/meta.embr src_m3/
"$embr" pack src_m3 -o m3.pmbr >/dev/null 2>&1
out="$("$embr" merge m2.pmbr m3.pmbr -o x.pmbr 2>&1)"; rc=$?
(( rc != 0 )) || { echo "FAIL [merge refuses a different file under one label]"; fail=1; }
has "merge: one label, two files" "$out" "is a different file in two of the packages"
out="$("$embr" merge m1.pmbr -o x.pmbr 2>&1)"; rc=$?
(( rc != 0 )) || { echo "FAIL [merge needs two]"; fail=1; }
out="$("$embr" merge greeter.pmbr m1.pmbr -o x.pmbr 2>&1)"
[[ -z "$out" || "$out" == *differ* || "$out" == *"cannot open"* ]] || true

# ---- the cache
rm -rf "$work/cache"
mkdir -p cachetest; cp packages/greeter.pmbr cachetest/; printf 'import "greeter"\n' > cachetest/t.embr
(cd cachetest && "$embr" t.embr >/dev/null 2>&1)
n_before=$(ls "$work/cache" | wc -l)
(( n_before == 1 )) || { echo "FAIL [cache: one folder after one import, got $n_before]"; fail=1; }
out="$("$embr" cache clean --packages 2>&1)"
has "cache clean: a package still in use is kept" "$out" "removed 0 folders"
(( $(ls "$work/cache" | wc -l) == 1 )) || { echo "FAIL [cache clean kept the folder]"; fail=1; }
# the package file is replaced by another version: the old folder is no use any more
cp other.pmbr cachetest/greeter.pmbr
(cd cachetest && "$embr" t.embr >/dev/null 2>&1)
(( $(ls "$work/cache" | wc -l) == 2 )) || { echo "FAIL [cache: both versions are unpacked]"; fail=1; }
out="$("$embr" cache clean --packages 2>&1)"
has "cache clean: an old version is removed" "$out" "removed 1 folder"
(( $(ls "$work/cache" | wc -l) == 1 )) || { echo "FAIL [cache clean removed the old version only]"; fail=1; }
# a half-written folder, and an old and a fresh temporary one
mkdir -p "$work/cache/halfway-00000000000000000000000000000000"
mkdir -p "$work/cache/old-00000000000000000000000000000000.tmp123" "$work/cache/new-00000000000000000000000000000000.tmp456"
touch -d '2 hours ago' "$work/cache/old-00000000000000000000000000000000.tmp123" 2>/dev/null || touch -t 202001010000 "$work/cache/old-00000000000000000000000000000000.tmp123"
out="$("$embr" cache clean --packages 2>&1)"
has "cache clean: a half-written folder" "$out" "removed halfway-"
has "cache clean: an old temporary folder" "$out" "removed old-"
[[ -d "$work/cache/new-00000000000000000000000000000000.tmp456" ]] || { echo "FAIL [cache clean leaves a fresh temporary folder (another process may be writing it)]"; fail=1; }
# the package file is gone
rm cachetest/greeter.pmbr
out="$("$embr" cache clean --packages 2>&1)"
has "cache clean: nobody lists it any more" "$out" "removed greeter-"
rm -rf "$work/cache/new-00000000000000000000000000000000.tmp456"
cp packages/greeter.pmbr cachetest/; (cd cachetest && "$embr" t.embr >/dev/null 2>&1)
out="$("$embr" cache clean --packages --all 2>&1)"
has "cache clean --all" "$out" "removed 1 folder"
[[ -z "$(ls "$work/cache" 2>/dev/null)" ]] || { echo "FAIL [cache clean --all empties the cache]"; fail=1; }

# ---- from a script: pmbr_header, pmbr_meta, pmbr_unpack, and load_module on what was unpacked
cat > t.embr <<'EOF'
import "embrlib"
import "pmbr"
h = pmbr_header("packages/greeter.pmbr")
print(h["format"], h["abi"], h["version"], len(h["id"]), len(h["entries"]))
m = pmbr_meta("packages/needy.pmbr")
print(m["name"], m["kind"], m["requires"])
dir = pmbr_unpack("packages/greeter.pmbr")
g = load_module(dir + "/greeter.embr")
print(g["greet"]("map"))
try
  pmbr_header("t.embr")
catch e
  print(e["message"])
end
EOF
out="$("$embr" t.embr 2>&1)"
has "pmbr_header" "$out" "1 0 1.0.0 64 2"
has "pmbr_meta" "$out" '{greeter: "1.0.0"}'
has "pmbr_unpack, then load_module for the globals as a map" "$out" "hello map"
has "pmbr_header on a file that is not a package" "$out" "not a package file"
if (( have_cc )); then
    cat > t.embr <<'EOF'
import "pmbr"
import "package"
dir = pmbr_unpack("packages/adder.pmbr")
print(package_path("adder", "lib/{target}/libadder.so") != "")
EOF
    sedi "s/libadder.so/libadder.$ext/" t.embr
    out="$("$embr" t.embr 2>&1)"
    has "pmbr_unpack registers the package for package_path" "$out" "1"
fi
out="$("$embr" --deny-caps=filesystem t.embr 2>&1)"
has "the pmbr plugin needs the filesystem" "$out" "not allowed in this sandbox"

# ---- a package's files in a worker thread: package_path works there too
mkdir -p src_wk/data
echo "worker data" > src_wk/data/x.txt
cat > src_wk/wk.embr <<'EOF'
import "package"
fn where(n)
    return package_path("wk", "data/x.txt")
end
EOF
cat > src_wk/meta.embr <<'EOF'
__package__ = {"name": "wk", "version": "1.0.0", "description": "a file in a worker", "capabilities": [], "module": "wk.embr"}
EOF
"$embr" pack src_wk -o packages/wk.pmbr >/dev/null 2>&1 || { echo "FAIL [pack wk]"; fail=1; }
run 'import "wk"
import "package"
import "async"
import "embrlib"
f = async_spawn(package_dir("wk") + "/wk.embr", "where", 1)
r = async_await(f)
p = r["value"]
print(r["ok"], p == package_path("wk", "data/x.txt"), len(load_file(p)))'
has "package_path in an async worker" "$out" "1 1 12"

# ---- two packages that share one library: the second requires the first and asks for its file
if (( have_cc )); then
    mkdir -p src_user
    cat > src_user/user.embr <<EOF
import "adder"
import "package"
fn lib_of_adder()
    return package_path("adder", "lib/{target}/libadder.$ext")
end
fn twice(x)
    return adder_add(x, x)
end
EOF
    cat > src_user/meta.embr <<'EOF'
__package__ = {"name": "user", "version": "1.0.0", "description": "uses the library bundled in adder", "capabilities": ["native"], "module": "user.embr", "requires": {"adder": "1.0.0"}}
EOF
    "$embr" pack src_user -o packages/user.pmbr >/dev/null 2>&1 || { echo "FAIL [pack user]"; fail=1; }
    run 'import "user"
print(twice(21))
print(lib_of_adder() == package_path("adder", "lib/{target}/libadder.'"$ext"'"))'
    has "a package uses the library another one bundles" "$out" "42"
    has "and finds the same file" "$out" "1"
    mkdir -p lonely; cp packages/user.pmbr lonely/; printf 'import "user"\n' > lonely/t.embr
    out="$(cd lonely && "$embr" t.embr 2>&1)"
    has "without the package that holds the library" "$out" "it requires adder 1.0.0, which is not installed"
fi

# ---- signing
export EMBR_TRUST_FILE="$work/trusted_keys"
rm -f "$EMBR_TRUST_FILE"
out="$("$embr" keygen alice 2>&1)"; rc=$?
(( rc == 0 )) || { echo "FAIL [keygen]: $out"; fail=1; }
[[ -f alice.key && -f alice.pub ]] || { echo "FAIL [keygen writes both files]"; fail=1; }
has "keygen prints the public key" "$out" "public key: "
pubhex="$(sed -n 2p alice.pub)"
[[ ${#pubhex} -eq 64 ]] || { echo "FAIL [the public key file holds 64 hex digits]: $pubhex"; fail=1; }
out="$("$embr" keygen alice 2>&1)"; rc=$?
(( rc != 0 )) || { echo "FAIL [keygen does not overwrite]"; fail=1; }
has "keygen: not overwriting" "$out" "not overwriting"
if [[ "$(uname -s)" == Linux ]]; then
    [[ "$(stat -c %a alice.key)" == 600 ]] || { echo "FAIL [the secret key file is private]: $(stat -c %a alice.key)"; fail=1; }
fi
"$embr" keygen bob >/dev/null 2>&1
bobhex="$(sed -n 2p bob.pub)"

out="$("$embr" pack src_greeter --sign alice.key -o signed.pmbr 2>&1)"
has "pack --sign" "$out" "wrote signed.pmbr"
info="$("$embr" package signed.pmbr 2>&1)"
has "package shows the signer" "$info" "signed by: $pubhex  (not in your trust list)"
has "an unsigned package says so" "$("$embr" package packages/greeter.pmbr 2>&1)" "signed by: nobody"
out="$("$embr" trust alice.pub "alice's key" 2>&1)"
has "trust" "$out" "trusted $pubhex"
has "trust: twice" "$("$embr" trust alice.pub 2>&1)" "already trusted"
has "trusted lists it" "$("$embr" trusted 2>&1)" "$pubhex  yours  alice's key"
info="$("$embr" package signed.pmbr 2>&1)"
has "package: a trusted signer" "$info" "signed by: $pubhex  (trusted)"
pj="$("$embr" package signed.pmbr --json)"
has "package json: signed" "$pj" '"signed": true, "signature_valid": true'
has "package json: signer and trust" "$pj" "\"signer\": \"$pubhex\", \"trusted\": true"

# importing: a signature is not needed unless the host asks for it
mkdir -p sg; cp signed.pmbr sg/greeter.pmbr; cp packages/greeter.pmbr sg/unsigned_src.pmbr
printf 'import "greeter"\nprint(greet("sig"))\n' > sg/t.embr
out="$(cd sg && "$embr" t.embr 2>&1)"
has "a signed package imports" "$out" "hello sig"
out="$(cd sg && "$embr" --require-signed t.embr 2>&1)"
has "required, signed by a trusted key" "$out" "hello sig"
mkdir -p us; cp packages/greeter.pmbr us/; cp sg/t.embr us/
out="$(cd us && "$embr" --require-signed t.embr 2>&1)"
has "required, not signed" "$out" "it is not signed, and this host requires signed packages"
lacks "required, not signed: it did not run" "$out" "hello sig"
"$embr" untrust alice.pub >/dev/null 2>&1
out="$(cd sg && "$embr" --require-signed t.embr 2>&1)"
has "required, signed by a key nobody trusts" "$out" "it is signed by the key ${pubhex:0:16}..., which is not trusted"
out="$(cd sg && "$embr" t.embr 2>&1)"
has "not required: an untrusted signature is no problem" "$out" "hello sig"
# a project can trust a key: trusted_keys in embr.json of the working folder
mkdir -p proj_trust; cp signed.pmbr proj_trust/greeter.pmbr; cp sg/t.embr proj_trust/
printf '{"dependencies": {}, "trusted_keys": ["%s"]}\n' "$pubhex" > proj_trust/embr.json
out="$(cd proj_trust && "$embr" --require-signed t.embr 2>&1)"
has "a project trusts a key" "$out" "hello sig"
printf '{"dependencies": {}, "trusted_keys": ["%s"]}\n' "$bobhex" > proj_trust/embr.json
out="$(cd proj_trust && "$embr" --require-signed t.embr 2>&1)"
has "a project that trusts another key" "$out" "which is not trusted"
# a sandbox does not read the working folder's embr.json (a script could have planted it)
printf '{"dependencies": {}, "trusted_keys": ["%s"]}\n' "$pubhex" > proj_trust/embr.json
out="$(cd proj_trust && EMBR_PATH="$EMBR_PATH:$work/proj_trust" "$embr" --sandbox --require-signed t.embr 2>&1)"
has "a sandbox ignores the project's trusted keys" "$out" "which is not trusted"

# a signature that does not match is refused whatever the host asks
python3 - "$work" <<'PY'
import sys, hashlib, struct
w = sys.argv[1]
d = open(w + "/signed.pmbr", "rb").read()
# read the table of contents
n = struct.unpack_from("<I", d, 12)[0]; vlen = struct.unpack_from("<I", d, 16)[0]
pos = 20 + vlen; ents = []
for _ in range(n):
    ll = struct.unpack_from("<H", d, pos)[0]; pos += 2
    label = d[pos:pos+ll].decode(); pos += ll
    off, ln = struct.unpack_from("<QQ", d, pos); pos += 16 + 32
    ents.append((label, d[off:off+ln]))
trailer = d[-100:]
assert trailer[:4] == b"PSIG"
def pack(abi, version, files):
    files = sorted(files.items())
    head = b"PMBR" + struct.pack("<IIII", 1, abi, len(files), len(version)) + version.encode()
    toc = b""
    off = len(head) + sum(2 + len(l) + 8 + 8 + 32 for l, _ in files) + 32
    for l, c in files:
        toc += struct.pack("<H", len(l)) + l.encode() + struct.pack("<QQ", off, len(c)) + hashlib.sha256(c).digest()
        off += len(c)
    h = head + toc
    return h + hashlib.sha256(h).digest() + b"".join(c for _, c in files)
files = dict(ents)
files["greeter.embr"] = files["greeter.embr"].replace(b"hello", b"HELLO")
import os; os.makedirs(w + "/forged", exist_ok=True)
open(w + "/forged/greeter.pmbr", "wb").write(pack(0, "1.0.0", files) + trailer)   # the files changed, the old signature put back
open(w + "/forged/t.embr", "w").write('try\n  import "greeter"\n  print("LOADED")\ncatch e\n  print(e["message"])\nend\n')
open(w + "/forged/cut.pmbr", "wb").write(d + b"x")                                  # a stray byte after the signature
PY
out="$(cd forged && "$embr" t.embr 2>&1)"
has "a signature that does not match the package" "$out" "the signature does not match the package"
lacks "and nothing ran" "$out" "LOADED"
out="$("$embr" package forged/greeter.pmbr 2>&1)"
has "package: a bad signature is shown" "$out" "THE SIGNATURE DOES NOT MATCH"
out="$("$embr" package forged/cut.pmbr 2>&1)"
has "bytes after the signature" "$out" "bytes after the last file that are not a signature"

# sign replaces a signature; merge drops it; sign a finished file
cp signed.pmbr resigned.pmbr
"$embr" sign resigned.pmbr --key bob.key >/dev/null 2>&1 || { echo "FAIL [sign]"; fail=1; }
has "sign: the new signer" "$("$embr" package resigned.pmbr 2>&1)" "signed by: $bobhex"
(( $(stat -c %s resigned.pmbr 2>/dev/null || stat -f %z resigned.pmbr) == $(stat -c %s signed.pmbr 2>/dev/null || stat -f %z signed.pmbr) )) || { echo "FAIL [sign: a second signature replaces the first]"; fail=1; }
"$embr" pack src_m1 --sign alice.key -o sm1.pmbr >/dev/null 2>&1; "$embr" pack src_m2 --sign alice.key -o sm2.pmbr >/dev/null 2>&1
"$embr" merge sm1.pmbr sm2.pmbr -o smerged.pmbr >/dev/null 2>&1 || { echo "FAIL [merge signed packages]"; fail=1; }
has "merge: the result is unsigned, sign it again" "$("$embr" package smerged.pmbr 2>&1)" "signed by: nobody"
"$embr" sign smerged.pmbr --key alice.key >/dev/null 2>&1
has "sign a merged package" "$("$embr" package smerged.pmbr 2>&1)" "signed by: $pubhex"
out="$("$embr" sign no_such.pmbr --key alice.key 2>&1)"; rc=$?
(( rc != 0 )) || { echo "FAIL [sign: no file]"; fail=1; }
out="$("$embr" sign signed.pmbr --key alice.pub 2>&1)"; rc=$?
(( rc != 0 )) || { echo "FAIL [sign with a public key file]"; fail=1; }
has "sign: not a secret key" "$out" "is not an embr secret key file"

# from a script
cat > t.embr <<'EOF'
import "embrlib"
import "pmbr"
m = pmbr_meta("signed.pmbr")
print(m["signed"], len(m["signer"]), m["trusted"])
u = pmbr_meta("packages/greeter.pmbr")
print(u["signed"], u["signer"] == "", u["trusted"])
EOF
"$embr" trust alice.pub >/dev/null 2>&1
out="$("$embr" t.embr 2>&1)"
has "pmbr_meta: signed, signer, trusted" "$out" "1 64 1"
has "pmbr_meta: unsigned" "$out" "0 1 0"

# embr pkg --require-signed
mkdir -p reg3/a proj3 src_plain; cp signed.pmbr reg3/a/greeter.pmbr
cp src_greeter/greeter.embr src_plain/greeter.embr; sed 's/"name": "greeter"/"name": "plain"/' src_greeter/meta.embr > src_plain/meta.embr
"$embr" pack src_plain -o reg3/a/unsigned.pmbr >/dev/null 2>&1 || { echo "FAIL [pack plain]"; fail=1; }
sum_s="$(sha256sum signed.pmbr 2>/dev/null | cut -d' ' -f1)"; [[ -n "$sum_s" ]] || sum_s="$(shasum -a 256 signed.pmbr | cut -d' ' -f1)"
sum_u="$(sha256sum reg3/a/unsigned.pmbr 2>/dev/null | cut -d' ' -f1)"; [[ -n "$sum_u" ]] || sum_u="$(shasum -a 256 reg3/a/unsigned.pmbr | cut -d' ' -f1)"
cat > reg3/index.json <<IDX
{"greeter": {"versions": {"1.0.0": {"url": "/a/greeter.pmbr", "type": "package", "sha256": "$sum_s"}}},
 "plain": {"versions": {"1.0.0": {"url": "/a/unsigned.pmbr", "type": "package", "sha256": "$sum_u"}}}}
IDX
out="$(cd proj3 && "$embr" pkg --registry "file://$work/reg3" install greeter --require-signed 2>&1)"
has "pkg --require-signed: a package signed by a trusted key" "$out" "installed greeter@1.0.0"
out="$(cd proj3 && "$embr" pkg --registry "file://$work/reg3" install plain --require-signed 2>&1)"
has "pkg --require-signed: an unsigned package" "$out" "plain@1.0.0 is not signed, and signed packages are required"
"$embr" untrust alice.pub >/dev/null 2>&1
rm -rf proj3; mkdir proj3
out="$(cd proj3 && "$embr" pkg --registry "file://$work/reg3" install greeter --require-signed 2>&1)"
has "pkg --require-signed: a key nobody trusts" "$out" "which is not trusted"
[[ ! -f proj3/packages/greeter.pmbr ]] || { echo "FAIL [pkg: a refused package is not installed]"; fail=1; }
out="$(cd proj3 && "$embr" pkg --registry "file://$work/reg3" install greeter 2>&1)"
has "pkg: without the option, an untrusted signature is fine" "$out" "installed greeter@1.0.0"

# ---- signer pinning: a package keeps the signer it was installed with
sha_of() { sha256sum "$1" 2>/dev/null | cut -d' ' -f1 || shasum -a 256 "$1" | cut -d' ' -f1; }
mkdir -p src_g11 src_g12; for v in 11 12; do cp src_greeter/greeter.embr src_g$v/; done
sed 's/"version": "1.0.0"/"version": "1.1.0"/' src_greeter/meta.embr > src_g11/meta.embr
sed 's/"version": "1.0.0"/"version": "1.2.0"/' src_greeter/meta.embr > src_g12/meta.embr
"$embr" pack src_greeter --sign alice.key -o g_alice_10.pmbr >/dev/null 2>&1
"$embr" pack src_g11 --sign bob.key -o g_bob_11.pmbr >/dev/null 2>&1
"$embr" pack src_g11 -o g_plain_11.pmbr >/dev/null 2>&1
"$embr" pack src_g12 --sign alice.key -o g_alice_12.pmbr >/dev/null 2>&1
mkdir -p reg4/a proj4
cp g_alice_10.pmbr g_bob_11.pmbr g_plain_11.pmbr g_alice_12.pmbr reg4/a/
index4() {   # greeter 1.0.0 by alice and the given 1.1.0 file
    local f="$1"
    cat > reg4/index.json <<IDX
{"greeter": {"versions": {
  "1.0.0": {"url": "/a/g_alice_10.pmbr", "type": "package", "sha256": "$(sha_of reg4/a/g_alice_10.pmbr)"},
  "1.1.0": {"url": "/a/$f", "type": "package", "sha256": "$(sha_of reg4/a/$f)"},
  "1.2.0": {"url": "/a/g_alice_12.pmbr", "type": "package", "sha256": "$(sha_of reg4/a/g_alice_12.pmbr)"}}}}
IDX
}
pkgp() { (cd proj4 && "$embr" pkg --registry "file://$work/reg4" "$@" 2>&1); }
index4 g_bob_11.pmbr
out="$(pkgp install greeter 1.0.0)"
has "pin: the first install pins the signer" "$out" "installed greeter@1.0.0"
has "pin: the manifest remembers it" "$(pkgp pins)" "greeter 1.0.0  signed by $pubhex"
has "pin: so does the lockfile" "$(cat proj4/embr.lock)" "\"signer\": \"$pubhex\""
out="$(pkgp install greeter 1.2.0)"
has "pin: the same signer updates fine" "$out" "installed greeter@1.2.0"
out="$(pkgp install greeter 1.1.0)"
has "pin: another key is refused" "$out" "greeter was signed by the key ${pubhex:0:16}... when you installed it, and greeter@1.1.0 is signed by the key ${bobhex:0:16}..."
has "pin: and says what to do" "$out" "--allow-signer-change"
has "pin: nothing changed" "$(pkgp pins)" "greeter 1.2.0  signed by $pubhex"
index4 g_plain_11.pmbr
out="$(pkgp install greeter 1.1.0)"
has "pin: signed, then unsigned, is refused" "$out" "greeter@1.1.0 is unsigned"
out="$(pkgp install greeter 1.1.0 --allow-signer-change)"
has "pin: accepted when you say so" "$out" "installed greeter@1.1.0"
has "pin: and the pin is now unsigned" "$(pkgp pins)" "greeter 1.1.0  unsigned"
index4 g_bob_11.pmbr
out="$(pkgp install greeter 1.1.0)"
has "pin: unsigned, then signed, is refused too" "$out" "greeter@1.1.0 is signed by the key ${bobhex:0:16}..."
out="$(pkgp install greeter 1.1.0 --allow-signer-change)"
has "pin: and the new signer is pinned" "$(pkgp pins)" "greeter 1.1.0  signed by $bobhex"
# a fresh checkout installs from embr.lock: the lock's signer has to match what comes down
rm -rf proj4/packages proj4/installed.json proj4/pkg_installed.json proj4/pkg_cache
python3 - "$work/proj4/embr.lock" "$pubhex" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
for p in d["packages"].values():
    p.pop("sha256", None)          # without the sha256 only the signer in the lock can tell a swapped file
    p["signer"] = sys.argv[2]      # the lock says alice signed it; what the lock's url serves is signed by bob
json.dump(d, open(sys.argv[1], "w"))
PY
out="$(cd proj4 && "$embr" pkg --registry "file://$work/reg4" install 2>&1)"
has "pin: embr.lock says who signed it" "$out" "is signed by the key ${bobhex:0:16}... but embr.lock says it was signed by the key ${pubhex:0:16}..."
out="$(pkgp pins)"
has "pins: nothing installed" "$out" "no packages installed"

# ---- several builds of one plugin, named by C++ ABI tag
hosttag="$("$embr" abi | head -1)"
cxxplug="$plugdir/str.$ext"; [[ -f "$cxxplug" ]] || cxxplug="$plugdir/str.so"
mkdir -p src_abi1/lib
cp "$cxxplug" src_abi1/lib/str.$ext
write_abi_meta() {   # dir, plugin map text
    printf '__package__ = {"name": "str", "version": "1.1.0", "description": "x", "capabilities": [], "plugin": %s}\n' "$2" > "$1/meta.embr"
}
write_abi_meta src_abi1 "{\"$target\": {\"$hosttag\": \"lib/str.$ext\"}}"
out="$("$embr" pack src_abi1 -o abi1.pmbr 2>&1)"; rc=$?
(( rc == 0 )) || { echo "FAIL [pack a C++ plugin under its C++ ABI tag]: $out"; fail=1; }
has "package lists the build by tag" "$("$embr" package abi1.pmbr 2>&1)" "builds: $target/$hosttag"
has "package json lists the build" "$("$embr" package abi1.pmbr --json 2>&1)" "\"builds\": [\"$target/$hosttag\"]"
mkdir -p abi_ok; cp abi1.pmbr abi_ok/str.pmbr; printf 'import "str"\nprint(str_upper("abi"))\n' > abi_ok/t.embr
out="$(cd abi_ok && "$embr" t.embr 2>&1)"
has "the build of this ABI is the one that loads" "$out" "ABI"
# a wrong tag for the library is caught by the packer, which can read the library's own
write_abi_meta src_abi1 "{\"$target\": {\"cxx1-itanium-libstdcxx99\": \"lib/str.$ext\"}}"
out="$("$embr" pack src_abi1 -o abi_bad.pmbr 2>&1)"; rc=$?
(( rc != 0 )) || { echo "FAIL [pack: a C++ ABI tag the library does not have]"; fail=1; }
has "pack: the library's own tag" "$out" "has the C++ ABI $hosttag, but meta.embr lists it as cxx1-itanium-libstdcxx99"
write_abi_meta src_abi1 "{\"$target\": {\"c\": \"lib/str.$ext\"}}"
out="$("$embr" pack src_abi1 -o abi_bad.pmbr 2>&1)"
has "pack: a C++ plugin listed as a C build" "$out" "has the C++ ABI $hosttag, but meta.embr lists it as c"
write_abi_meta src_abi1 "{\"$target\": \"lib/str.$ext\"}"
out="$("$embr" pack src_abi1 -o abi_untagged.pmbr 2>&1)"
has "pack: a build that does not say its ABI gets a note" "$out" "does not say its C++ ABI; meta.embr can list it as \"$hosttag\""
mkdir -p abi_untagged; cp abi_untagged.pmbr abi_untagged/str.pmbr; cp abi_ok/t.embr abi_untagged/
has "an untagged build still loads" "$(cd abi_untagged && "$embr" t.embr 2>&1)" "ABI"
# a C plugin is a build any host takes
mkdir -p src_abi2/lib; cp "$plugdir/path.$ext" src_abi2/lib/path.$ext 2>/dev/null || cp "$plugdir/path.so" src_abi2/lib/path.$ext
printf '__package__ = {"name": "path", "version": "1.0.0", "description": "x", "capabilities": [], "plugin": {"%s": {"c": "lib/path.%s"}}}\n' "$target" "$ext" > src_abi2/meta.embr
out="$("$embr" pack src_abi2 -o abi2.pmbr 2>&1)"; rc=$?
(( rc == 0 )) || { echo "FAIL [pack a C plugin as the c build]: $out"; fail=1; }
write_abi_meta src_abi2 "{\"$target\": {\"$hosttag\": \"lib/path.$ext\"}}"
sedi 's/"name": "str"/"name": "path"/' src_abi2/meta.embr
out="$("$embr" pack src_abi2 -o abi2b.pmbr 2>&1)"; rc=$?
(( rc != 0 )) || { echo "FAIL [pack: a C plugin listed under a C++ tag]"; fail=1; }
has "pack: a C plugin is the c build" "$out" "has the C++ ABI c, but meta.embr lists it as $hosttag"

# builds made by hand (the packer would not take a fake library), to try how the loader chooses
python3 - "$work" "$target" "$ext" "$hosttag" "$cxxplug" "$plugdir/path.$ext" <<'PY'
import sys, hashlib, struct, os
w, target, ext, hosttag, cxx, cpl = sys.argv[1:7]
def pack(abi, version, files):
    files = sorted(files.items())
    head = b"PMBR" + struct.pack("<IIII", 1, abi, len(files), len(version)) + version.encode()
    toc = b""
    off = len(head) + sum(2 + len(l) + 8 + 8 + 32 for l, _ in files) + 32
    for l, c in files:
        toc += struct.pack("<H", len(l)) + l.encode() + struct.pack("<QQ", off, len(c)) + hashlib.sha256(c).digest()
        off += len(c)
    h = head + toc
    return h + hashlib.sha256(h).digest() + b"".join(c for _, c in files)
real = open(cxx, "rb").read()
creal = open(cpl, "rb").read() if os.path.exists(cpl) else b""
fake = b"not a library, built for another compiler"
def meta(name, plugin):
    return ('__package__ = {"name": "%s", "version": "%s", "description": "x", "capabilities": [], "plugin": %s}\n' % (name, "1.1.0" if name == "str" else "1.0.0", plugin)).encode()
def save(dirname, name, plugin, files):
    os.makedirs(f"{w}/{dirname}", exist_ok=True)
    open(f"{w}/{dirname}/{name}.pmbr", "wb").write(pack(1, "1.1.0" if name == "str" else "1.0.0", dict(files, **{"meta.embr": meta(name, plugin)})))
    open(f"{w}/{dirname}/t.embr", "w").write('import "%s"\nprint("LOADED")\n' % name)
# two builds on this platform: another compiler's first, ours second
save("abi_two", "str", '{"%s": {"cxx2-msvc-v143": "lib/msvc.dll", "%s": "lib/ours.so"}}' % (target, hosttag), {"lib/msvc.dll": fake, "lib/ours.so": real})
# only builds of other ABIs
save("abi_none", "str", '{"%s": {"cxx2-msvc-v143": "lib/msvc.dll", "cxx2-itanium-libcxx18": "lib/libcxx.so"}}' % target, {"lib/msvc.dll": fake, "lib/libcxx.so": fake})
# other ABIs, and a C build that any host takes
if creal:
    save("abi_c", "path", '{"%s": {"cxx2-msvc-v143": "lib/msvc.dll", "c": "lib/path_c.so"}}' % target, {"lib/msvc.dll": fake, "lib/path_c.so": creal})
# a build for this platform with another compiler (name "str") and one with ours, to merge
save("abi_m1", "str", '{"%s": {"%s": "lib/ours.so"}}' % (target, hosttag), {"lib/ours.so": real})
save("abi_m2", "str", '{"%s": {"cxx2-msvc-v143": "lib/msvc.dll"}}' % target, {"lib/msvc.dll": fake})
PY
out="$(cd abi_two && "$embr" t.embr 2>&1)"
has "two builds: ours is chosen" "$out" "LOADED"
out="$(cd abi_none && "$embr" t.embr 2>&1)"
has "no build fits: the message names both sides" "$out" "it has no build for $target that fits this embr: its C++ ABI is $hosttag"
has "no build fits: and what the package has" "$out" "the package has cxx2-itanium-libcxx18, cxx2-msvc-v143"
lacks "no build fits: nothing was loaded" "$out" "LOADED"
if [[ -d abi_c ]]; then
    out="$(cd abi_c && "$embr" t.embr 2>&1)"
    has "a C build fits any host" "$out" "LOADED"
fi
out="$("$embr" merge abi_m1/str.pmbr abi_m2/str.pmbr -o abi_merged.pmbr 2>&1)"; rc=$?
(( rc == 0 )) || { echo "FAIL [merge builds of different ABIs]: $out"; fail=1; }
mj="$("$embr" package abi_merged.pmbr --json)"
has "merge: both builds" "$mj" "\"builds\": [\"$target/$hosttag\", \"$target/cxx2-msvc-v143\"]"
mkdir -p abi_mm; cp abi_merged.pmbr abi_mm/str.pmbr; cp abi_m1/t.embr abi_mm/
has "merged package loads our build" "$(cd abi_mm && "$embr" t.embr 2>&1)" "LOADED"

(( fail )) && exit 1
echo "package smoke OK"
