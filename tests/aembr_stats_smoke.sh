#!/usr/bin/env bash
# the counters of tools/aembr/STATS.md as budgets: a change that makes the generated code do more work (more retains, more error checks, more
# copies at the plugin boundary) fails here, whatever the clock says. counts do not depend on the machine.
#
#   aembr_stats_smoke.sh <aembr binary> <repo root>
set -u
aembr="$1"; root="$(realpath "$2")"
fail=0
bad() { echo "FAIL [$1]: $2"; fail=1; }
count() { grep -c "$1" <<<"$2"; }   # lines of a text that match

# 1. the ports: a parameter that is only read takes no count of its own
for n in encoding path csv json; do
    c="$("$aembr" plugin --emit-c "$root/tools/aembr/ports/a$n.embr" 2>&1)"
    k="$(count 'rt_retain_str(v_\|rt_retain_arr(v_\|rt_retain_any(v_' "$c")"
    [[ "$k" -le 6 ]] || bad "ports a$n" "$k parameters are retained on entry (budget 6): is markBorrowedParams working?"
done

# 2. array loops: the bounds test is in the operation, not a call and a flag check after it
c="$("$aembr" build --infer --emit-c "$root/tools/bench/nbody.embr" 2>&1)"
[[ "$(count 'rt_arr_get_\|rt_arr_set_' "$c")" -eq 0 ]] || bad nbody "an array access is a function call again"
[[ "$(count 'RT_ERR' "$c")" -le 8 ]] || bad nbody "more than 8 error flag checks: $(count 'RT_ERR' "$c")"

# 3. counts of a run: fib calls itself 635622 times and makes no allocation. with --fatal-overflow it checks no flag at all
out="$("$aembr" run --infer --stats "$root/tools/bench/fib.embr" 2>&1 >/dev/null)"
[[ "$out" == *"calls "*"635622"* && "$out" != *"alloc_"* && "$out" != *"retain_"* ]] || bad fib "unexpected counters: ${out:0:300}"
out="$("$aembr" run --infer --stats --fatal-overflow "$root/tools/bench/fib.embr" 2>&1 >/dev/null)"
[[ "$out" != *"chk "* ]] || bad fib-fatal "the call to fib still checks the error flag: ${out:0:300}"

# 4. a json round trip through the port's host values: no allocation per value at the boundary (the port makes no tree of anys)
c="$("$aembr" plugin --emit-c "$root/tools/aembr/ports/ajson.embr" 2>&1)"
[[ "$(count 'rtp_in_any\|rtp_out_any' "$c")" -le 2 ]] || bad ajson "json goes through a tree of anys again"

# 5. s = s + slice(t, a, b) appends from t: no string is made for the slice, and a string built this way grows in a few steps
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
cat > "$tmp/slice.embr" <<'EOS'
import "embrlib"
fn build(p: str, n: int) -> str
    out = ""
    i = 0
    while i < n
        out = out + slice(p, i % 5, i % 5 + 3)
        i += 1
    end
    return out
end
print(len(build("abcdefgh", 1000)))
EOS
f="$tmp/slice.embr"
c="$("$aembr" build --emit-c "$f" 2>&1)"
[[ "$(count 'rt_slice_str' "$c")" -eq 0 && "$(count 'rt_str_append_slice' "$c")" -ge 1 ]] || bad append-slice "the slice is made as a string again"
out="$("$aembr" run --stats "$f" 2>&1 >/dev/null)"
a="$(sed -n 's/.*alloc_str *\([0-9]*\).*/\1/p' <<<"$out" | head -1)"
[[ -n "$a" && "$a" -le 20 ]] || bad append-slice "1000 appends allocated $a strings (budget 20)"

[[ $fail -eq 0 ]] && echo "aembr_stats_smoke: ok"
exit $fail
