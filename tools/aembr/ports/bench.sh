#!/usr/bin/env bash
# builds a C plugin and its aembr port with the same optimization and runs the same benchmark script on both.
#   tools/aembr/ports/bench.sh encoding       (needs bin/linux/embr and bin/linux/aembr; use a Release build of embr for steadier numbers)
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
name="${1:?name of a plugin that has a port here, like encoding}"
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
cc -O2 -shared -fPIC -fvisibility=hidden -I"$root/include" "$root/plugins/$name/$name.c" -o "$work/$name.so"
"$root/bin/linux/aembr" plugin "$here/a$name.embr" -o "$work/a$name.so"
for lib in "$name" "a$name"; do
    sed "s/@LIB@/$lib/" "$here/bench_$name.embr" > "$work/bench_$lib.embr"
    echo "== $lib"
    EMBR_PATH="$work" taskset -c "${CPU:-6}" "$root/bin/linux/embr" "$work/bench_$lib.embr" 2>&1 | grep -v '^\[runtime\]'
done
