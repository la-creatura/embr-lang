#!/usr/bin/env bash
# the .vmbr cache of compiled scripts, through the real `embr` binary: nothing is written unless --cache-dir is
# given, a cached file is reused while the script is unchanged, and an old or damaged file is a cache miss, not a crash.
#
#   bytecode_smoke.sh <embr binary> <examples dir>
set -u
embr="$1"; examples="$2"
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
export EMBR_PATH="$(cd "$(dirname "$embr")" && pwd)"
fail=0
bad() { echo "FAIL [$1]"; fail=1; }

cd "$work"
printf 'total = 0\nfn add(a, b)\n  return a + b\nend\nfor i in [1, 2, 3]\n  total = add(total, i)\nend\nprint(total)\n' > s.embr

# no --cache-dir: no file anywhere
"$embr" s.embr >/dev/null 2>&1
[[ -z "$(find "$work" -name '*.vmbr')" ]] || bad "default run wrote a .vmbr"

# --cache-dir: one file, same output, and it matches what the compiler makes
out0="$("$embr" s.embr 2>&1)"
out1="$("$embr" --cache-dir=cache s.embr 2>&1)"
[[ "$out0" == "$out1" && "$out0" == "6" ]] || bad "output with --cache-dir: $out1"
n=$(find cache -name '*.vmbr' | wc -l); [[ "$n" -eq 1 ]] || bad "expected 1 cached file, got $n"
f="$(find cache -name '*.vmbr')"
[[ "$("$embr" dis s.embr)" == "$("$embr" dis "$f")" ]] || bad "dis of the .vmbr differs from dis of the source"
out2="$("$embr" --cache-dir=cache s.embr 2>&1)"; [[ "$out2" == "6" ]] || bad "second run: $out2"

# a changed script is not served from the old file
printf 'print(41 + 1)\n' > s.embr
out3="$("$embr" --cache-dir=cache s.embr 2>&1)"; [[ "$out3" == "42" ]] || bad "changed script: $out3"
[[ "$("$embr" dis "$f")" == *"PUSH_INT"* && "$("$embr" dis "$f")" != *'"add"'* ]] || bad "file was not rewritten for the changed script"

# a damaged file (cut short, or one byte changed) is a miss: the script still runs and the file is repaired
head -c 40 "$f" > "$f.cut" && mv "$f.cut" "$f"
out4="$("$embr" --cache-dir=cache s.embr 2>&1)"; [[ "$out4" == "42" ]] || bad "truncated cache: $out4"
"$embr" dis "$f" >/dev/null 2>&1 || bad "truncated cache was not repaired"
python3 - "$f" <<'PY'
import sys
b = bytearray(open(sys.argv[1], 'rb').read()); b[len(b) // 2] ^= 0xFF; open(sys.argv[1], 'wb').write(b)
PY
out5="$("$embr" --cache-dir=cache s.embr 2>&1)"; [[ "$out5" == "42" ]] || bad "flipped byte: $out5"
"$embr" dis "$f" >/dev/null 2>&1 || bad "damaged cache was not repaired"
# reading a damaged file directly is a clean error
python3 - "$f" <<'PY'
import sys
b = bytearray(open(sys.argv[1], 'rb').read()); b[len(b) // 2] ^= 0xFF; open(sys.argv[1], 'wb').write(b)
PY
"$embr" dis "$f" >/dev/null 2>"$work/err"; rc=$?
[[ $rc -eq 1 && -s "$work/err" ]] || bad "dis of a damaged .vmbr: rc=$rc"

# an unwritable cache dir does not stop the script
printf 'x' > notadir
out6="$("$embr" --cache-dir=notadir/sub s.embr 2>&1)"; [[ "$out6" == "42" ]] || bad "unwritable cache dir: $out6"

# same file name in two folders: two cache files
mkdir -p a b; printf 'print(1)\n' > a/t.embr; printf 'print(2)\n' > b/t.embr
rm -rf cache2; "$embr" --cache-dir=cache2 a/t.embr >/dev/null 2>&1; "$embr" --cache-dir=cache2 b/t.embr >/dev/null 2>&1
n=$(find cache2 -name '*.vmbr' | wc -l); [[ "$n" -eq 2 ]] || bad "same name in two folders: $n files"

# embr compile: writes next to the script (or -o), runs nothing, and the .vmbr runs on its own
mkdir -p comp; cd comp
printf 'fn twice(x)\n  return x * 2\nend\nhelper = 21\n' > mod.embr
printf 'import "mod.embr"\nprint(twice(helper))\n' > main.embr
out="$("$embr" compile main.embr 2>&1)"; rc=$?
[[ $rc -eq 0 && "$out" == "wrote main.vmbr" && -f main.vmbr ]] || bad "embr compile: rc=$rc $out"
[[ "$("$embr" main.vmbr 2>&1)" == "42" ]] || bad "running a .vmbr: $("$embr" main.vmbr 2>&1)"
"$embr" compile -o out.vmbr mod.embr >/dev/null 2>&1; [[ -f out.vmbr ]] || bad "embr compile -o"
"$embr" compile mod.embr >/dev/null 2>&1; [[ -f mod.vmbr ]] || bad "embr compile of a module"
"$embr" compile -o x.vmbr main.embr mod.embr >/dev/null 2>&1; [[ $? -eq 2 ]] || bad "embr compile -o with two files should be exit 2"
printf 'x = = 1\n' > bad.embr
"$embr" compile bad.embr >/dev/null 2>&1; rc=$?
[[ $rc -eq 1 && ! -f bad.vmbr ]] || bad "embr compile of a bad script: rc=$rc"
"$embr" compile >/dev/null 2>&1; [[ $? -eq 2 ]] || bad "embr compile without a file should be exit 2"
# a damaged .vmbr is a clean error, not a crash
head -c 30 main.vmbr > cut.vmbr
out="$("$embr" cut.vmbr 2>&1)"; rc=$?
(( rc < 128 )) && [[ "$out" == *"bad compiled file"* ]] || bad "running a damaged .vmbr: rc=$rc ${out:0:200}"
# an error inside a compiled script still reports the line and the call it came from
printf 'fn f()\n  return nope + 1\nend\nf()\n' > boom.embr; "$embr" compile boom.embr >/dev/null 2>&1
out="$("$embr" boom.vmbr 2>&1)"; [[ "$out" == *"undefined variable: nope at line 2"* && "$out" == *"in f()"* ]] || bad "error in a compiled script: ${out:0:200}"

# import: a .vmbr module can be imported by name, and --cache-dir caches the modules a script imports
printf 'import "mod.vmbr"\nprint(twice(5))\n' > viavmbr.embr
[[ "$("$embr" viavmbr.embr 2>&1)" == "10" ]] || bad "import of a .vmbr module: $("$embr" viavmbr.embr 2>&1)"
printf 'import "nothere.vmbr"\n' > missing.embr
[[ "$("$embr" missing.embr 2>&1)" == *'cannot find module "nothere.vmbr"'* ]] || bad "import of a missing .vmbr module"
rm -rf cc; out="$("$embr" --cache-dir=cc main.embr 2>&1)"
n=$(find cc -name '*.vmbr' | wc -l); [[ "$out" == "42" && "$n" -eq 2 ]] || bad "cached import: out=$out files=$n"
printf 'fn twice(x)\n  return x * 3\nend\nhelper = 21\n' > mod.embr
[[ "$("$embr" --cache-dir=cc main.embr 2>&1)" == "63" ]] || bad "a changed module was served from the old cache"

# load_module() takes a .vmbr too, and a script module through the cache
printf 'import "embrlib"\nm = load_module("mod.vmbr")\nprint(m["twice"](4))\n' > lm.embr
"$embr" compile mod.embr >/dev/null 2>&1
[[ "$("$embr" lm.embr 2>&1 | tail -1)" == "12" ]] || bad "load_module of a .vmbr: $("$embr" lm.embr 2>&1 | tail -3)"
printf 'import "embrlib"\nm = load_module("mod.embr")\nprint(m["twice"](4))\n' > lm2.embr
rm -rf cl; "$embr" --cache-dir=cl lm2.embr >/dev/null 2>&1
[[ "$(find cl -name 'mod-*.vmbr' | wc -l)" -eq 1 && "$("$embr" --cache-dir=cl lm2.embr 2>&1 | tail -1)" == "12" ]] || bad "load_module through the cache"

# worker threads (async, parallel) use the same cache folder for the modules they load
printf 'fn dbl(x)\n  return x * 2\nend\n' > wmod.embr
printf 'import "async"\np = async_spawn("wmod.embr", "dbl", 21)\nr = async_await(p)\nprint(r["value"])\n' > wasync.embr
rm -rf cw; out="$("$embr" --cache-dir=cw wasync.embr 2>&1 | tail -1)"
[[ "$out" == "42" && "$(find cw -name 'wmod-*.vmbr' | wc -l)" -eq 1 ]] || bad "async worker did not use the cache folder: out=$out files=$(find cw -name '*.vmbr' | wc -l)"
printf 'import "parallel"\nic = channel()\noc = channel()\nw = worker_spawn("wmod.embr", "dbl", ic, oc)\nchannel_send(ic, 5)\nchannel_close(ic)\nr = channel_recv(oc)\nworker_join(w)\nprint(r["value"])\n' > wpar.embr
rm -rf cw2; out="$("$embr" --cache-dir=cw2 wpar.embr 2>&1 | tail -1)"
[[ "$out" == "10" && "$(find cw2 -name 'wmod-*.vmbr' | wc -l)" -eq 1 ]] || bad "parallel worker did not use the cache folder: out=$out files=$(find cw2 -name '*.vmbr' | wc -l)"

# a cached file was compiled without knowing what other files would do, so the types of globals are checked when it runs
printf 'cv = 1\n' > ca.embr; printf 'print("b")\ncv = "s"\n' > cb.embr
rm -rf ctypes; "$embr" --cache-dir=ctypes ca.embr >/dev/null 2>&1
out="$("$embr" --cache-dir=ctypes cb.embr ca.embr 2>&1)"; rc=$?
[[ $rc -eq 1 && "$out" == *"cannot assign int to 'cv' declared as string"* ]] || bad "a cached file broke a global's type: rc=$rc $out"

# embr cache clean: drops stale and damaged files, keeps good ones and other files, --all drops every .vmbr
printf 'print(1)\n' > gone.embr; "$embr" --cache-dir=cc gone.embr >/dev/null 2>&1; rm gone.embr
printf 'junk' > cc/junk.vmbr; printf 'keep' > cc/notes.txt
out="$("$embr" cache clean cc 2>&1)"; [[ "$out" == "removed 2 files" ]] || bad "cache clean: $out"
[[ -f cc/notes.txt && "$(find cc -name '*.vmbr' | wc -l)" -ge 1 ]] || bad "cache clean removed too much"
[[ "$("$embr" cache clean --all cc 2>&1)" == removed* && -z "$(find cc -name '*.vmbr')" && -f cc/notes.txt ]] || bad "cache clean --all"
"$embr" cache clean >/dev/null 2>&1; [[ $? -eq 2 ]] || bad "cache clean without a folder should be exit 2"
"$embr" cache clean "$work/nothing_here" >/dev/null 2>&1; [[ $? -eq 2 ]] || bad "cache clean of a missing folder should be exit 2"
cd "$work"

# every example: same output cached and not, on a miss and on a hit, and dis agrees
for ex in "$examples"/*.embr; do
    name="$(basename "$ex" .embr)"
    mkdir -p "run_$name"; cp "$ex" "run_$name/"; ( cd "run_$name"
        plain="$("$embr" "$name.embr" 2>&1)"
        miss="$("$embr" --cache-dir=c "$name.embr" 2>&1)"
        hit="$("$embr" --cache-dir=c "$name.embr" 2>&1)"
        [[ "$plain" == "$miss" && "$plain" == "$hit" ]] || { echo "FAIL [example $name: output differs with the cache]"; exit 1; }
        cf="$(find c -name '*.vmbr')"
        [[ -n "$cf" && "$("$embr" dis "$name.embr")" == "$("$embr" dis "$cf")" ]] || { echo "FAIL [example $name: dis differs]"; exit 1; }
    ) || fail=1
done

(( fail )) && exit 1
echo "bytecode smoke OK"
