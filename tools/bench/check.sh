#!/usr/bin/env bash
# guards against "someone made embr slower". runs the benchmarks (tools/bench/run.sh) with BASE, an embr built from
# an older commit on the same machine, and fails when a benchmark takes more than TOL longer than it did there.
# a ratio between two builds on one machine, so a slow or noisy runner doesn't matter.
#
#   BASE=path/to/old/embr tools/bench/check.sh     # RUNS=5, TOL=0.35
#   BASE=... TOL=0.5 RUNS=9 tools/bench/check.sh fib loop
#   EMBR=path/to/new/embr BASE=... tools/bench/check.sh
#
# both binaries should be Release builds. without BASE there is nothing to compare with, so it says so and exits 0.
# exit 1 on a regression. when GITHUB_STEP_SUMMARY is set, the table is written there too.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
tol="${TOL:-0.35}"
export RUNS="${RUNS:-5}"

if [[ -z "${BASE:-}" ]]; then echo "check.sh: BASE is not set, nothing to compare with. skipping"; exit 0; fi

table="$("$here/run.sh" "$@")"
echo "$table"

# lines look like: name   1.234s   0.456s   0.37x
rows="$(echo "$table" | awk '$4 ~ /x$/ { r = $4; sub(/x$/, "", r); print $1, r }')"
[[ -n "$rows" ]] || { echo "check.sh: no benchmark results found" >&2; exit 2; }

fail=0
limit="$(echo "1 + $tol" | bc -l)"
report="| benchmark | new/base | limit | |
|---|---|---|---|
"
while read -r name now; do
    if (( $(echo "$now > $limit" | bc -l) )); then mark="SLOWER"; fail=1; else mark="ok"; fi
    report+="$(printf '| %s | %.2fx | %.2fx | %s |' "$name" "$now" "$limit" "$mark")
"
done <<< "$rows"

echo; echo "new/base time (tolerance $tol)"; echo "$report"
[[ -n "${GITHUB_STEP_SUMMARY:-}" ]] && { echo "### embr benchmarks, new/base time"; echo "$report"; } >> "$GITHUB_STEP_SUMMARY"
if (( fail )); then echo "check.sh: a benchmark got slower than the base build (see SLOWER above)" >&2; exit 1; fi
echo "check.sh: ok"
