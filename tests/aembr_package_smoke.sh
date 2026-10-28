#!/usr/bin/env bash
# checks `aembr package`, `aembr info` and `aembr verify`: a package of a compiled plugin imports like any package and gives the same answers as the source as a
# script module, it holds the source and a build record, building it twice gives the same bytes, and verify tells a package that is what its source makes
# from one that is not.
#
#   aembr_package_smoke.sh <aembr binary> <embr binary> <tests/aembr folder>
set -u
ext=.so; [[ "$(uname)" == Darwin ]] && ext=.dylib   # what embr looks for when it imports a plugin
aembr="$(realpath "$1")"; embr="$(realpath "$2")"; dir="$(realpath "$3")"
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
fail=0
bad() { echo "FAIL [$1]: $2"; fail=1; }
mkdir -p "$work/a" "$work/b" "$work/pk"

# 1. a package: it imports like a plugin and answers like the module
"$aembr" package "$dir/plugin/mathlib.embr" -o "$work/pk/mathlib.pmbr" > "$work/pack.out" 2>&1 || bad package "failed: $(cat "$work/pack.out")"
cp "$dir/plugin/mathlib.embr" "$work/pk/mathlib.embr"
sed "s/@LIB@/mathlib/" "$dir/plugin/use_mathlib.embr" > "$work/pk/p.embr"
sed "s/@LIB@/mathlib.embr/" "$dir/plugin/use_mathlib.embr" > "$work/pk/v.embr"
rm -f "$work/pk/mathlib$ext"
got="$(cd "$work/pk" && "$embr" p.embr 2>&1 | grep -v '^\[runtime\]')"
want="$(cd "$work/pk" && "$embr" v.embr 2>&1 | grep -v '^\[runtime\]')"
[[ -n "$got" && "$got" == "$want" ]] || bad import "the package and the module disagree: $(diff <(echo "$got") <(echo "$want") | head -5)"
out="$("$embr" package "$work/pk/mathlib.pmbr" 2>&1)"
[[ "$out" == *"mathlib 1.2.0"* && "$out" == *"aembr/build.json"* && "$out" == *"src/mathlib.embr"* && "$out" == *"lib/"*"/mathlib$ext"* ]] || bad contents "$out"

# 1b. a package of a plugin that imports a module: the module is in the package, and verify rebuilds the same bytes from it
"$aembr" package "$dir/plugin/withmod.embr" -o "$work/pk/withmod.pmbr" > "$work/pack2.out" 2>&1 || bad package-modules "failed: $(cat "$work/pack2.out")"
out="$("$embr" package "$work/pk/withmod.pmbr" 2>&1)"
[[ "$out" == *"src/lib/helpers.embr"* ]] || bad package-modules "the module is not in the package: $out"
out="$("$aembr" verify "$work/pk/withmod.pmbr" 2>&1)"; rc=$?
[[ $rc -eq 0 && "$out" == *"claim: ok"* && "$out" == *"bytes: ok"* ]] || bad verify-modules "exit $rc: $out"
#     a module outside the folder cannot be packed
mkdir -p "$work/up/in" && printf 'fn up(x: int) -> int\n    return x\nend\n' > "$work/up/up.embr"
printf '__module__ = {"name": "outer", "version": "1.0.0", "description": "x", "capabilities": []}\nimport "../up.embr"\nfn f(x: int) -> int\n    return up(x)\nend\n' > "$work/up/in/outer.embr"
out="$("$aembr" package "$work/up/in/outer.embr" -o "$work/up/outer.pmbr" 2>&1)"; rc=$?
[[ $rc -ne 0 && "$out" == *"is not in the folder"* ]] || bad package-outside "a module outside the folder should be refused: $out"

# 2. what the package says about how it was made
out="$("$aembr" info "$work/pk/mathlib.pmbr" 2>&1)"
[[ "$out" == *"package mathlib 1.2.0"* && "$out" == *"made by aembr"* && "$out" == *"source: mathlib.embr"* && "$out" == *"compiler:"* && "$out" == *"flags: -std=gnu11"* ]] || bad info "$out"

# 3. the same input gives the same package
"$aembr" package "$dir/plugin/mathlib.embr" -o "$work/a/mathlib.pmbr" >/dev/null 2>&1
"$aembr" package "$dir/plugin/mathlib.embr" -o "$work/b/mathlib.pmbr" >/dev/null 2>&1
cmp -s "$work/a/mathlib.pmbr" "$work/b/mathlib.pmbr" || bad repro "two packages of the same source differ"

# 4. verify: the claim and the bytes
out="$("$aembr" verify "$work/pk/mathlib.pmbr" 2>&1)"; rc=$?
[[ $rc -eq 0 && "$out" == *"claim: ok"* && "$out" == *"bytes: ok"* ]] || bad verify "exit $rc: $out"

#    a compiler that is not the recorded one: said, and exit 2 (could not verify), not 1
if command -v gcc >/dev/null 2>&1 && [[ "$(gcc --version | head -1)" != "$(cc --version | head -1)" ]]; then
    out="$("$aembr" verify "$work/pk/mathlib.pmbr" --cc=gcc 2>&1)"; rc=$?
    [[ $rc -eq 2 && "$out" == *"claim: ok"* && "$out" == *"bytes: not checked. it needs compiler"* ]] || bad verify-compiler "exit $rc: $out"
fi

#    a bundled source that is not the one the record was made from
"$embr" unpack "$work/pk/mathlib.pmbr" "$work/un" >/dev/null 2>&1 || bad unpack "embr unpack failed"
rm -f "$work/un/.header"      # (unpack keeps the table of contents there; pack does not take it)
printf '\n# changed after the build\n' >> "$work/un/src/mathlib.embr"
"$embr" pack "$work/un" -o "$work/pk/changed.pmbr" >/dev/null 2>&1 || bad repack "embr pack failed"
out="$("$aembr" verify "$work/pk/changed.pmbr" 2>&1)"; rc=$?
[[ $rc -eq 1 && "$out" == *"not the one the record was made from"* ]] || bad verify-source "exit $rc: $out"

#    a source that needs more than the package says
cat > "$work/un/src/mathlib.embr" <<'SRC'
import "fs"
fn probe() -> int
    return fs_exists("/")
end
SRC
"$embr" pack "$work/un" -o "$work/pk/liar.pmbr" >/dev/null 2>&1 || bad repack2 "embr pack failed"
out="$("$aembr" verify "$work/pk/liar.pmbr" 2>&1)"; rc=$?
[[ $rc -eq 1 && "$out" == *"claim: WRONG. the source needs filesystem"* ]] || bad verify-claim "exit $rc: $out"

#    a library changed inside the file
cp "$work/pk/mathlib.pmbr" "$work/pk/damaged.pmbr"
size=$(wc -c < "$work/pk/damaged.pmbr")
printf '\xff' | dd of="$work/pk/damaged.pmbr" bs=1 seek=$((size / 2)) count=1 conv=notrunc 2>/dev/null
out="$("$aembr" verify "$work/pk/damaged.pmbr" 2>&1)"; rc=$?
[[ $rc -ne 0 ]] || bad verify-damaged "a changed package verified: $out"

#    no aembr record
printf '__package__ = {"name": "plain", "version": "1.0.0", "description": "x", "capabilities": [], "module": "plain.embr"}\n' > "$work/plain_meta.embr"
mkdir -p "$work/plain" && cp "$work/plain_meta.embr" "$work/plain/meta.embr" && printf 'fn f() return 1 end\n' > "$work/plain/plain.embr"
"$embr" pack "$work/plain" -o "$work/pk/plain.pmbr" >/dev/null 2>&1
"$aembr" verify "$work/pk/plain.pmbr" >/dev/null 2>&1; [[ $? -eq 2 ]] || bad verify-not-aembr "a package not made by aembr should be exit 2"

# 5. signing is embr's own: sign while packing, the signer is shown, verify still passes
(cd "$work" && "$embr" keygen alice >/dev/null 2>&1) || bad keygen "embr keygen failed"
"$aembr" package "$dir/plugin/mathlib.embr" -o "$work/pk/signed.pmbr" --sign "$work/alice.key" >/dev/null 2>&1 || bad sign "aembr package --sign failed"
out="$("$aembr" info "$work/pk/signed.pmbr" 2>&1)"
[[ "$out" == *"signed by: "* && "$out" != *"signed by: nobody"* ]] || bad signed-info "$out"
out="$("$aembr" verify "$work/pk/signed.pmbr" 2>&1)"; rc=$?
[[ $rc -eq 0 ]] || bad signed-verify "exit $rc: $out"

# 6. a package of a plugin that calls C plugins takes their capabilities, and verify checks them
"$aembr" package "$dir/plugin/fslib.embr" -o "$work/pk/fslib.pmbr" >/dev/null 2>&1 || bad fslib "package failed"
out="$("$aembr" info "$work/pk/fslib.pmbr" 2>&1)"
[[ "$out" == *"needs: filesystem"* ]] || bad fslib-caps "$out"
"$aembr" verify "$work/pk/fslib.pmbr" >/dev/null 2>&1; [[ $? -eq 0 ]] || bad fslib-verify "verify failed"

# 7. a program and a plugin carry the record too, and info reads it without running them
"$aembr" build "$dir/ok/fib.embr" -o "$work/fib" >/dev/null 2>&1 || bad build "aembr build failed"
out="$("$aembr" info "$work/fib" 2>&1)"
[[ "$out" == *"program"* && "$out" == *"source: fib.embr"* ]] || bad info-program "$out"
"$aembr" plugin "$dir/plugin/mathlib.embr" -o "$work/mathlib$ext" >/dev/null 2>&1
out="$("$aembr" info "$work/mathlib$ext" 2>&1)"
[[ "$out" == *"plugin mathlib"* ]] || bad info-plugin "$out"
"$aembr" info "$embr" >/dev/null 2>&1; [[ $? -eq 1 ]] || bad info-other "a file without a record should be exit 1"

(( fail )) && exit 1
echo "aembr_package_smoke: ok"
