#!/usr/bin/env bash
# runs every tools/bench/*.embr and prints the best time of each. with BASE set (another embr binary, say one built
# from an older commit) it runs both and adds a column with new/base, which is what check.sh uses.
#
#   tools/bench/run.sh                 # 3 runs each, best-of (default)
#   RUNS=10 tools/bench/run.sh         # more runs
#   EMBR=path/to/embr tools/bench/run.sh
#   BASE=path/to/old/embr tools/bench/run.sh
#   tools/bench/run.sh fib loop        # only these benchmarks
#
# for real numbers use a Release build, a Debug build measures -O0 code. `embr --version` is printed first so the
# table says what it measured. with BASE, it refuses to report a benchmark when the two binaries print different output.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
embr="${EMBR:-$root/bin/linux/embr}"
base="${BASE:-}"
runs="${RUNS:-3}"

[[ -x "$embr" ]] || { echo "embr not found at $embr (set EMBR=...)" >&2; exit 1; }
[[ -z "$base" || -x "$base" ]] || { echo "BASE=$base is not an executable" >&2; exit 1; }
"$embr" --version
[[ -z "$base" ]] || { echo "base: $("$base" --version)"; }

if (( $# )); then names=("$@"); else names=(); for f in "$here"/*.embr; do names+=("$(basename "$f" .embr)"); done; fi

# best-of-N wall time in seconds for one command
best() {
    local b="" t s e
    for ((k = 0; k < runs; k++)); do
        s=$(date +%s.%N); "$@" >/dev/null; e=$(date +%s.%N)
        t=$(echo "$e - $s" | bc -l)
        if [[ -z "$b" ]] || (( $(echo "$t < $b" | bc -l) )); then b=$t; fi
    done
    echo "$b"
}

if [[ -n "$base" ]]; then
    printf '\n%-18s %12s %12s %10s\n' benchmark base new "new/base"
    printf '%-18s %12s %12s %10s\n' ------------------ ------------ ------------ ----------
else
    printf '\n%-18s %12s\n' benchmark time
    printf '%-18s %12s\n' ------------------ ------------
fi
for n in "${names[@]}"; do
    f="$here/$n.embr"; [[ -f "$f" ]] || { echo "no such benchmark: $n" >&2; continue; }
    # a benchmark that fails has no time worth printing (it stops early, so it would look fast)
    "$embr" "$f" >/dev/null 2>&1 || { echo "BENCHMARK FAILED: $n (run $embr $f to see why)" >&2; exit 1; }
    tn=$(best "$embr" "$f")
    if [[ -n "$base" ]]; then
        # a benchmark for something the base build doesn't have yet (it fails there) has nothing to compare with
        if ! "$base" "$f" >/dev/null 2>&1; then printf '%-18s %12s %11.3fs %10s\n' "$n" "-" "$tn" "new"; continue; fi
        a="$("$base" "$f" 2>&1 | grep -v '^\[runtime\]')"; b="$("$embr" "$f" 2>&1 | grep -v '^\[runtime\]')"
        [[ "$a" == "$b" ]] || { echo "OUTPUT MISMATCH in $n: base='$a' new='$b'" >&2; exit 1; }
        tb=$(best "$base" "$f")
        printf '%-18s %11.3fs %11.3fs %9.2fx\n' "$n" "$tb" "$tn" "$(echo "$tn / $tb" | bc -l)"
    else
        printf '%-18s %11.3fs\n' "$n" "$tn"
    fi
done
echo
