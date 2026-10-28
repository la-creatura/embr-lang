#!/usr/bin/env bash
# checks the aembr ports of the C plugins (tools/aembr/ports): each is built with `aembr plugin`, and the same script is run with the C plugin and
# with the port. the two have to print exactly the same, error messages and error kinds included.
#
#   aembr_ports_smoke.sh <aembr binary> <embr binary> <tools/aembr/ports folder>
set -u
ext=.so; [[ "$(uname)" == Darwin ]] && ext=.dylib   # what embr looks for when it imports a plugin
aembr="$(realpath "$1")"; embr="$(realpath "$2")"; ports="$(realpath "$3")"
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
fail=0
bad() { echo "FAIL [$1]: $2"; fail=1; }

for n in encoding path csv json; do
    "$aembr" plugin "$ports/a$n.embr" -o "$work/a$n$ext" || { bad "$n" "aembr plugin failed"; continue; }
    for lib in "$n" "a$n"; do
        sed "s/@LIB@/$lib/" "$ports/compare_$n.embr" > "$work/c_$lib.embr"
        (cd "$work" && EMBR_PATH="$work" "$embr" "c_$lib.embr" 2>&1 | grep -a -v '^\[runtime\]') > "$work/out_$lib.txt"
    done
    [[ -s "$work/out_$n.txt" ]] || { bad "$n" "the C plugin printed nothing"; continue; }
    if ! cmp -s "$work/out_$n.txt" "$work/out_a$n.txt"; then
        bad "$n" "the port prints something else than the C plugin: $(diff "$work/out_$n.txt" "$work/out_a$n.txt" | head -6)"
    fi
done

(( fail )) && exit 1
echo "aembr_ports_smoke: ok"
