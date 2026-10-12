#!/usr/bin/env bash
# builds and runs the embr fuzzers in a scratch directory. uses the generated single header from an existing
# build dir (default build/linux-debug; configure it first), compiled with
# ASan + UBSan so memory errors and UB are findings too.
#
#   tools/fuzz/run.sh                 # 3000 iterations of every target
#   N=20000 SEED=1 tools/fuzz/run.sh frontend
#   tools/fuzz/run.sh programs json vmbr pmbr
#   tools/fuzz/run.sh plugins          # csv encoding path regex str unicode time vec (or name one: tools/fuzz/run.sh regex)
#
# plugin targets start from tools/fuzz/corpus/<plugin>/ (hand-made seeds plus fixed regressions; a file
# is "a<0x01>b", the two string arguments). to keep a new finding, copy the saved file there as regress-*.bin.

# failures are saved under $OUT (crash-*.bin, hang-*.bin, programs-*.embr). exit status is nonzero
# if any target found something.
set -uo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
gen="${GEN:-$root/build/linux-debug/generated}"
out="${OUT:-${TMPDIR:-/tmp}/embr-fuzz}"
n="${N:-3000}"
seed_arg=(); [[ -n "${SEED:-}" ]] && seed_arg=(-seed "$SEED")
plugin_targets=(csv encoding path regex str unicode time vec)
targets=()
for t in "$@"; do if [[ "$t" == plugins ]]; then targets+=("${plugin_targets[@]}"); else targets+=("$t"); fi; done
(( ${#targets[@]} )) || targets=(frontend fmt vmbr pmbr json programs)
[[ -f "$gen/embr/embr.h" ]] || { echo "no $gen/embr/embr.h -- run: cmake --preset linux-debug && cmake --build build/linux-debug" >&2; exit 2; }
mkdir -p "$out"; cd "$out"
cxx=(g++ -std=gnu++20 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
     -I "$gen" -I "$root/include" -w)
status=0
need() { [[ "$1" == *"$2"* ]]; }
if need " ${targets[*]} " " frontend " || need " ${targets[*]} " " fmt " || need " ${targets[*]} " " vmbr " || need " ${targets[*]} " " pmbr "; then
    "${cxx[@]}" "$here/fuzz.cpp" -o embr_fuzz -ldl || exit 2
fi
# programs: embrlib is compiled in, so push / append / insert exist (see FUZZ_PROGRAMS_EMBRLIB_SRC in fuzz.cpp)
if need " ${targets[*]} " " programs "; then
    "${cxx[@]}" -DFUZZ_PROGRAMS_EMBRLIB_SRC="\"$root/plugins/embrlib/embrlib.cpp\"" "$here/fuzz.cpp" -o embr_fuzz_programs -ldl || exit 2
fi
if need " ${targets[*]} " " json "; then
    # json is written in C: compiled on its own (with the sanitizers), then linked in
    gcc -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -I "$root/include" -c "$root/plugins/json/json.c" -o embr_fuzz_json.o || exit 2
    "${cxx[@]}" -DFUZZ_JSON -DFUZZ_C_PLUGIN "$here/fuzz.cpp" embr_fuzz_json.o -o embr_fuzz_json -ldl || exit 2
fi
for t in "${targets[@]}"; do
    [[ " ${plugin_targets[*]} " == *" $t "* ]] || continue
    if [[ -f "$root/plugins/$t/$t.c" ]]; then   # a plugin written in C: compiled on its own (with the sanitizers), then linked in
        gcc -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -I "$root/include" -c "$root/plugins/$t/$t.c" -o "embr_fuzz_$t.o" || exit 2
        "${cxx[@]}" -DFUZZ_C_PLUGIN -DFUZZ_PLUGIN_SRC="\"$root/include/embr_plugin.h\"" -DFUZZ_PLUGIN_NAME="\"$t\"" "$here/fuzz.cpp" "embr_fuzz_$t.o" -o "embr_fuzz_$t" -ldl || exit 2
    else
        "${cxx[@]}" -DFUZZ_PLUGIN_SRC="\"$root/plugins/$t/$t.cpp\"" -DFUZZ_PLUGIN_NAME="\"$t\"" "$here/fuzz.cpp" -o "embr_fuzz_$t" -ldl || exit 2
    fi
done
# ASan's frames are several times larger than a normal build's, so embr's call-depth limit (1000) can
# exhaust the default 8 MB stack before the clean "max call depth" error fires. give the fuzzers room.
ulimit -s 524288 2>/dev/null || true
export ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 UBSAN_OPTIONS=print_stacktrace=1:abort_on_error=1
for t in "${targets[@]}"; do
    case "$t" in
        frontend) ./embr_fuzz frontend -n "$n" "${seed_arg[@]}" "$root"/modules/*.embr "$root"/tools/bench/*.embr ;;
        fmt)      ./embr_fuzz fmt -n "$n" "${seed_arg[@]}" "$root"/modules/*.embr "$root"/tools/bench/*.embr "$root"/tests/fmt/messy.embr ;;
        vmbr)     ./embr_fuzz vmbr -n "$n" "${seed_arg[@]}" "$root"/modules/*.embr "$root"/tools/bench/*.embr "$root"/examples/*.embr ;;
        pmbr)     ./embr_fuzz pmbr -n "$n" "${seed_arg[@]}" ;;
        programs) ./embr_fuzz_programs programs -n "$n" "${seed_arg[@]}" ;;
        json)     ./embr_fuzz_json json -n "$n" "${seed_arg[@]}" ;;
        csv|encoding|path|regex|str|unicode|time|vec)
                  ./embr_fuzz_$t "$t" -n "$n" "${seed_arg[@]}" "$here/corpus/$t"/* ;;
        *) echo "unknown target $t" >&2; status=2; continue ;;
    esac || status=1
done
echo "artifacts (if any) in $out"
exit $status
