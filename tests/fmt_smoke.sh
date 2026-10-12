#!/usr/bin/env bash
# checks `embr fmt` (src/fmt.h): a golden file, idempotence on every .embr file in the repo,
# --check / --stdout / --indent, and that a file that doesn't lex is left alone.
#
#   fmt_smoke.sh <embr binary> <repo root>
set -u
embr="$1"; root="$2"
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
fail=0
bad() { echo "FAIL [$1]"; fail=1; }

# golden: messy input -> expected output, and the expected output is already formatted
"$embr" fmt --stdout "$root/tests/fmt/messy.embr" > "$work/out.embr" 2>"$work/err" || bad "messy.embr did not format: $(cat "$work/err")"
cmp -s "$work/out.embr" "$root/tests/fmt/messy.expected.embr" || { bad "golden output differs"; diff "$work/out.embr" "$root/tests/fmt/messy.expected.embr" | head -10; }
"$embr" fmt --check "$root/tests/fmt/messy.expected.embr" >/dev/null || bad "--check on formatted file should exit 0"
out="$("$embr" fmt --check "$root/tests/fmt/messy.embr")"; rc=$?
[[ $rc -eq 1 && "$out" == *messy.embr* ]] || bad "--check on unformatted file should list it and exit 1 (rc=$rc)"

# in place, then again: the second run changes nothing
cp "$root/tests/fmt/messy.embr" "$work/a.embr"
"$embr" fmt "$work/a.embr" || bad "in-place format failed"
cmp -s "$work/a.embr" "$root/tests/fmt/messy.expected.embr" || bad "in-place result differs from golden"
"$embr" fmt --check "$work/a.embr" >/dev/null || bad "second format changed the file"

# --indent
"$embr" fmt --stdout --indent=2 "$root/tests/fmt/messy.embr" | grep -q '^  return a + b$' || bad "--indent=2"

# every .embr file in the repo formats, and formatting twice equals formatting once
n=0
while IFS= read -r f; do
    n=$((n+1))
    "$embr" fmt --stdout "$f" > "$work/1.embr" 2>"$work/err" || { bad "$f: $(cat "$work/err")"; continue; }
    "$embr" fmt --stdout "$work/1.embr" > "$work/2.embr" 2>/dev/null
    cmp -s "$work/1.embr" "$work/2.embr" || bad "$f: not idempotent"
done < <(find "$root/examples" "$root/modules" "$root/tests" "$root/tools" -name '*.embr' -not -path '*/test-work/*')
(( n > 5 )) || bad "found only $n .embr files"

# element types are written together, and a comparison with a variable called arr is not one
printf 'local arr< int >xs=[]\nfn f(a:arr<int>,b : map<arr<str>>)->arr<arr<str>>\nreturn [[\"a\"]]\nend\narr=3\nif arr<4 and arr<5\nprint(1)\nend\n' > "$work/types.embr"
"$embr" fmt --stdout "$work/types.embr" > "$work/types.out" 2>"$work/err" || bad "types.embr did not format: $(cat "$work/err")"
grep -q '^fn f(a: arr<int>, b: map<arr<str>>) -> arr<arr<str>>$' "$work/types.out" || bad "element types are written together: $(cat "$work/types.out")"
grep -q '^if arr < 4 and arr < 5$' "$work/types.out" || bad "a comparison with a variable called arr: $(cat "$work/types.out")"
"$embr" fmt --stdout "$work/types.out" > "$work/types2.out" 2>/dev/null
cmp -s "$work/types.out" "$work/types2.out" || bad "element types: not idempotent"

# const and const fn keep their keyword
printf 'const   N=5*2\nconst   fn  cl( v,lo )\nif v<lo\nreturn lo\nend\nreturn v\nend\n' > "$work/const.embr"
"$embr" fmt --stdout "$work/const.embr" > "$work/const.out" 2>"$work/err" || bad "const did not format: $(cat "$work/err")"
grep -q '^const N = 5 \* 2$' "$work/const.out" && grep -q '^const fn cl(v, lo)$' "$work/const.out" || bad "const formatting: $(cat "$work/const.out")"
"$embr" fmt --stdout "$work/const.out" > "$work/const2.out" 2>/dev/null
cmp -s "$work/const.out" "$work/const2.out" || bad "const: not idempotent"

# tokens that only look like one operator when written close together keep their space (`! =` must not become `!=`)
for src in '! =' '! ==' '- >' '- =' 'x = - >= y'; do
    printf '%s\n' "$src" > "$work/glue.embr"
    "$embr" fmt --stdout "$work/glue.embr" > "$work/glue.out" 2>/dev/null
    [[ "$(cat "$work/glue.out")" == "$src" ]] || bad "formatting '$src' changed its tokens: $(cat "$work/glue.out")"
done

# a file that doesn't lex is an error and stays as it was
printf 'x = "unterminated\n' > "$work/bad.embr"; cp "$work/bad.embr" "$work/bad.orig"
"$embr" fmt "$work/bad.embr" >/dev/null 2>&1; [[ $? -eq 2 ]] || bad "unlexable file should exit 2"
cmp -s "$work/bad.embr" "$work/bad.orig" || bad "unlexable file was modified"
"$embr" fmt "$work/missing.embr" >/dev/null 2>&1; [[ $? -eq 2 ]] || bad "missing file should exit 2"

# very deep nesting must not blow up the output (indent is capped)
for i in $(seq 1 20000); do echo "if 1"; done > "$work/deep.embr"
# macos has no `timeout` (coreutils calls it gtimeout), without either this runs with no time limit
if command -v timeout >/dev/null; then limit="timeout 60"; elif command -v gtimeout >/dev/null; then limit="gtimeout 60"; else limit=""; fi
size="$($limit "$embr" fmt --stdout "$work/deep.embr" | wc -c)"
(( size > 0 && size < 20000000 )) || bad "deep nesting output size $size"

(( fail )) && exit 1
echo "fmt smoke OK ($n files)"
