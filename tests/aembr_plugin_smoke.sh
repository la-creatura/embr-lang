#!/usr/bin/env bash
# checks `aembr plugin`: a file built as a C plugin loads in embr and gives the same answers as the same file imported as a script module,
# its description and capabilities reach the host, what cannot be exported is refused, and the same input gives the same bytes.
#
#   aembr_plugin_smoke.sh <aembr binary> <embr binary> <tests/aembr folder>
set -u
aembr="$(realpath "$1")"; embr="$(realpath "$2")"; dir="$(realpath "$3")"     # it changes folder below
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
fail=0
bad() { echo "FAIL [$1]: $2"; fail=1; }

# 1. the same file as a compiled plugin and as a script module: same output
for n in mathlib strlib fslib anylib; do
    "$aembr" plugin "$dir/plugin/$n.embr" -o "$work/$n.so" || { bad "$n" "aembr plugin failed"; continue; }
    cp "$dir/plugin/$n.embr" "$work/$n.embr"
    sed "s/@LIB@/$n/" "$dir/plugin/use_$n.embr" > "$work/p_$n.embr"
    sed "s/@LIB@/$n.embr/" "$dir/plugin/use_$n.embr" > "$work/v_$n.embr"
    got="$(cd "$work" && EMBR_PATH="$work" "$embr" "p_$n.embr" 2>&1 | grep -v '^\[runtime\]')"
    want="$(cd "$work" && EMBR_PATH="$work" "$embr" "v_$n.embr" 2>&1 | grep -v '^\[runtime\]')"
    [[ -n "$got" && "$got" == "$want" ]] || bad "$n" "the plugin and the module disagree: $(diff <(echo "$got") <(echo "$want") | head -5)"
done

# 1b. a plugin written with C text: C for the whole file, C in functions, a flag for the C compiler. embr and aembr programs both use it
"$aembr" plugin "$dir/plugin/clib.embr" -o "$work/clib.so" -DCLIB_SCALE=7 || bad clib "aembr plugin failed"
cp "$dir/plugin/use_clib.embr" "$work/use_clib.embr"
got="$(cd "$work" && EMBR_PATH="$work" "$embr" use_clib.embr 2>&1 | grep -v '^\[runtime\]')"
[[ "$got" == "$(cat "$dir/plugin/use_clib.out")" ]] || bad clib-embr "embr gave: ${got:0:200}"
got="$(cd "$work" && "$aembr" run use_clib.embr 2>&1)"
[[ "$got" == "$(cat "$dir/plugin/use_clib.out")" ]] || bad clib-aembr "aembr gave: ${got:0:200}"
"$aembr" info "$work/clib.so" | grep -q -- "-DCLIB_SCALE=7" || bad clib-info "the build record should name the flag"

# 1b2. a plugin made of two files: the module's functions are used, not exported
"$aembr" plugin "$dir/plugin/withmod.embr" -o "$work/withmod.so" || bad withmod "aembr plugin failed"
cp "$dir/plugin/use_withmod.embr" "$work/use_withmod.embr"
got="$(cd "$work" && EMBR_PATH="$work" "$embr" use_withmod.embr 2>&1 | grep -v '^\[runtime\]')"
[[ "$got" == "$(cat "$dir/plugin/use_withmod.out")" ]] || bad withmod-embr "embr gave: ${got:0:200}"
out="$(cd "$work" && EMBR_PATH="$work" "$embr" plugin withmod 2>&1)"
[[ "$out" == *"triple("* && "$out" == *"hey("* && "$out" != *"scale("* && "$out" != *"shout("* ]] || bad withmod-exports "only the file's own functions are exported: ${out:0:300}"

# 1b3. a plugin that imports another plugin (found by name when it is built and when it runs)
EMBR_PATH="$work" "$aembr" plugin "$dir/plugin/chain.embr" -o "$work/chain.so" || bad chain "aembr plugin failed"
cp "$dir/plugin/use_chain.embr" "$work/use_chain.embr"
got="$(cd "$work" && EMBR_PATH="$work" "$embr" use_chain.embr 2>&1 | grep -v '^\[runtime\]')"
[[ "$got" == "$(cat "$dir/plugin/use_chain.out")" ]] || bad chain-embr "embr gave: ${got:0:200}"

# 1c. host values: a parameter read where the host keeps it, a result written into the host's slot
"$aembr" plugin "$dir/plugin/hostlib.embr" -o "$work/hostlib.so" || bad hostlib "aembr plugin failed"
cp "$dir/plugin/use_hostlib.embr" "$work/use_hostlib.embr"
got="$(cd "$work" && EMBR_PATH="$work" "$embr" use_hostlib.embr 2>&1 | grep -v '^\[runtime\]')"
[[ "$got" == "$(cat "$dir/plugin/use_hostlib.out")" ]] || bad hostlib-embr "embr gave: ${got:0:300}"
info="$(EMBR_PATH="$work" "$embr" plugin clib 2>&1)"
[[ "$info" == *"needs: native"* ]] || bad clib-caps "$info"

# 2. what the host reads from the table
info="$(EMBR_PATH="$work" "$embr" plugin mathlib 2>&1)"
[[ "$info" == *"mathlib 1.2.0"* && "$info" == *"written in C"* && "$info" == *"needs: none"* ]] || bad info "$info"
[[ "$info" == *"scaled(n: int) -> int"* && "$info" == *"n times the scale"* ]] || bad info-functions "$info"
[[ "$info" != *"__twice"* ]] || bad info-private "a function starting with __ is exported"

# 3. capabilities are computed from the file and checked by the host before any code runs
sed 's/"capabilities": \[\]/"capabilities": ["network"]/' "$dir/plugin/mathlib.embr" > "$work/netlib.embr"
"$aembr" plugin "$work/netlib.embr" -o "$work/netlib.so" || bad netlib "aembr plugin failed"
printf 'import "netlib"\nprint(scaled(2))\n' > "$work/n.embr"
[[ "$(EMBR_PATH="$work" "$embr" "$work/n.embr" 2>/dev/null)" == "6" ]] || bad netlib-run "wrong answer"
out="$(EMBR_PATH="$work" "$embr" --deny-caps=network "$work/n.embr" 2>&1)"
[[ "$out" == *"needs network"* ]] || bad deny-caps "a denied capability should refuse the import: $out"

# a compiled plugin that calls C plugins takes their capabilities, and its declaration has to cover them
info="$(EMBR_PATH="$work" "$embr" plugin fslib 2>&1)"
[[ "$info" == *"needs: filesystem"* ]] || bad fslib-caps "$info"
sed 's/"capabilities": \["filesystem"\]/"capabilities": []/' "$dir/plugin/fslib.embr" > "$work/fsbad.embr"
out="$("$aembr" plugin "$work/fsbad.embr" -o "$work/fsbad.so" 2>&1)"; rc=$?
[[ $rc -eq 1 && "$out" == *"filesystem"* ]] || bad fslib-undeclared "exit $rc: ${out:0:200}"

# 4. what cannot be exported is refused, with the reason
refuse() {   # name, source, words
    printf '%s\n' "$2" > "$work/r_$1.embr"
    local out rc
    out="$("$aembr" plugin "$work/r_$1.embr" -o "$work/r_$1.so" 2>&1)"; rc=$?
    [[ $rc -eq 1 && "$out" == *"$3"* ]] || bad "refuse $1" "exit $rc, wanted '$3': ${out:0:200}"
}
refuse callback 'fn apply(f: fn(int) -> int, x: int) -> int
    return f(x)
end' "cannot cross the plugin boundary"
refuse export_inout 'fn bump(&n: int)
    n = n + 1
end' "is in-out"
refuse host_keep 'fn f(v: host) -> int
    x = v
    return 1
end' "can only be given to a host parameter"
refuse host_closure 'fn f(v: host) -> int
    g = fn() -> int
        c """
        """
        return 1
    end
    return v
end' "host"
refuse host_value 'fn f(v: host) -> int
    return 1
end
fn g() -> int
    h = f
    return 1
end' "cannot be used as a value"
refuse host_out_plain 'fn f(v: host) -> int
    c """
        (void)$out;
    """
    return 1
end' "result slot"
refuse host_local 'fn f(v: host) -> int
    local host x = 1
    return 1
end' "type of a parameter"
refuse write_global 'X = 1
fn bump() -> int
    X = X + 1
    return X
end' "functions only read"
refuse element_store 'import "embrlib"
LIST = [1, 2]
fn grow() -> int
    append(&LIST, 3)
    return len(LIST)
end' "functions only read"
refuse keep_function 'F = fn(x: int) -> int return x end
fn g() -> int
    return 1
end' "cannot keep"
refuse nothing_to_export 'X = 1
fn __h() -> int
    return X
end' "no function to export"

# 5. the same input gives the same plugin
mkdir -p "$work/a" "$work/b"
"$aembr" plugin "$dir/plugin/strlib.embr" -o "$work/a/strlib.so" || bad repro "build failed"
"$aembr" plugin "$dir/plugin/strlib.embr" -o "$work/b/strlib.so" || bad repro "build failed"
cmp -s "$work/a/strlib.so" "$work/b/strlib.so" || bad repro "two builds gave different plugins"

(( fail )) && exit 1
echo "aembr_plugin_smoke: ok"
