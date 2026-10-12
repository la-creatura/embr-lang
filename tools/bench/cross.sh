#!/usr/bin/env bash
# runs the same small programs in embr and in other scripting languages, and prints best-of-N times side by side.
# a language that is not installed is skipped. the programs are in tools/bench/cross/ (one file per language).
#
#   tools/bench/cross.sh                    # every benchmark, every language found
#   RUNS=10 tools/bench/cross.sh            # more runs
#   tools/bench/cross.sh fib loop           # only these
#   EMBR=path/to/embr tools/bench/cross.sh
#
# what it measures: wall time of the whole process, minus the time the same language takes to run an empty file, so
# start-up cost doesn't hide the difference. every language prints the same answer as embr or the run stops, so a
# program that quietly did something else can't win. use a Release build of embr (`embr --version` says which).
# interpreters only: node runs with --jitless, luajit with -joff, so the comparison is bytecode VM against bytecode VM.
# the JIT numbers are printed too, in separate columns, to show how far a JIT is from all of them.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
embr="${EMBR:-$root/bin/linux/embr}"
runs="${RUNS:-5}"
cross="$here/cross"
[[ -x "$embr" ]] || { echo "embr not found at $embr (set EMBR=...)" >&2; exit 1; }

# label | command | file extension. only the ones that exist are used
labels=(); cmds=(); exts=(); versions=()
add() {   # label, extension, version command, command words...
    local label="$1" ext="$2" ver="$3"; shift 3
    command -v "$1" >/dev/null 2>&1 || return 0
    labels+=("$label"); exts+=("$ext"); cmds+=("$*"); versions+=("$( (bash -c "$ver" 2>&1 || true) | head -1)")
}
export EMBR_BIN="$embr"   # the path can have a space in it, so the commands below name it through the environment
labels+=("embr"); exts+=(embr); cmds+=('"$EMBR_BIN"'); versions+=("$("$embr" --version 2>&1 | head -1)")
add "lua"           lua  "lua5.4 -v"  lua5.4
command -v lua5.4 >/dev/null 2>&1 || add "lua" lua "lua -v" lua
add "luajit -joff"  lua  "luajit -v"  luajit -joff
add "quickjs"       js   "qjs --help"  qjs
add "python"        py   "python3 --version" python3
add "node nojit"    js   "node --version" node --jitless
add "luajit jit"    lua  "luajit -v"  luajit
add "node jit"      js   "node --version" node

if (( $# )); then names=("$@"); else names=(fib loop calls closure strings map array sieve nbody mandelbrot wordfreq sort jsonrt); fi

# best-of-N wall time in seconds
best() {
    local b="" t s e
    for ((k = 0; k < runs; k++)); do
        s=$(date +%s.%N); "$@" >/dev/null 2>&1; e=$(date +%s.%N)
        t=$(echo "$e - $s" | bc -l)
        if [[ -z "$b" ]] || (( $(echo "$t < $b" | bc -l) )); then b=$t; fi
    done
    echo "$b"
}
clean() { grep -v '^\[runtime\]' || true; }

echo "languages:"
for i in "${!labels[@]}"; do printf '  %-13s %s\n' "${labels[$i]}" "${versions[$i]}"; done
echo "best of $runs, seconds, start-up time taken off"

# the answer embr gives is the one everybody has to give
printf '\n%-9s' benchmark; for l in "${labels[@]}"; do printf ' %13s' "$l"; done; echo
printf '%-9s' ---------; for l in "${labels[@]}"; do printf ' %13s' -------------; done; echo
startup=()
for i in "${!labels[@]}"; do
    startup+=("$(best bash -c "${cmds[$i]} '$cross/empty.${exts[$i]}'")")
done
for n in "${names[@]}"; do
    want="$("$embr" "$here/$n.embr" 2>/dev/null | clean)"
    printf '%-9s' "$n"
    for i in "${!labels[@]}"; do
        f="$cross/$n.${exts[$i]}"
        if [[ "${labels[$i]}" == embr ]]; then f="$here/$n.embr"; fi
        if [[ ! -f "$f" ]]; then printf ' %13s' "-"; continue; fi
        got="$(bash -c "${cmds[$i]} '$f'" 2>/dev/null | clean | tr -d '\r')"
        if [[ "$got" != "$want" ]]; then printf ' %13s' "WRONG"; continue; fi
        t=$(best bash -c "${cmds[$i]} '$f'")
        d=$(echo "$t - ${startup[$i]}" | bc -l)
        printf ' %13.3f' "$(echo "if ($d < 0) 0 else $d" | bc -l)"
    done
    echo
done
echo
printf 'start-up  '; for i in "${!labels[@]}"; do printf ' %13.3f' "${startup[$i]}"; done; echo " (already taken off above)"
