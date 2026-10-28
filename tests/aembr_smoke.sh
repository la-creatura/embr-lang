#!/usr/bin/env bash
# checks aembr end to end: programs built by aembr print what the VM prints, what it refuses is refused with the right words,
# a runtime error ends the program the way the VM's does, and the same input gives the same bytes.
#
#   aembr_smoke.sh <aembr binary> <embr binary> <tests/aembr folder>
set -u
aembr="$1"; embr="$2"; dir="$3"
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
fail=0
bad() { echo "FAIL [$1]: $2"; fail=1; }

# in a sanitizer build the shipped plugins need the ASan runtime loaded first, so a program that imports one is built with it too
sanitized=0
plugin="$(ls "$(dirname "$embr")"/plugins/*.so 2>/dev/null | head -1)"
if [[ -n "$plugin" ]] && command -v ldd >/dev/null && ldd "$plugin" | grep -q libasan; then
    printf '#!/bin/sh\nexec cc -fsanitize=address,undefined "$@"\n' > "$work/san-cc"
    chmod +x "$work/san-cc"
    export CC="$work/san-cc"
    sanitized=1
fi

# 1. programs that mean the same in both: same output
for f in "$dir"/ok/*.embr; do
    n="$(basename "$f" .embr)"
    want="$("$embr" "$f" 2>/dev/null | sed -E 's|file: "[^"]*"|file: "F"|g')"      # stdout only: the VM tells on stderr which plugins it loaded. (print(e) names the file, the path is not compared)
    got="$("$aembr" run "$f" 2>&1)"; rc=$?
    got="$(printf '%s' "$got" | sed -E 's|file: "[^"]*"|file: "F"|g')"
    [[ $rc -eq 0 ]] || bad "$n" "aembr run gave exit $rc: ${got:0:200}"
    [[ "$got" == "$want" ]] || bad "$n" "output differs from the VM"
done

# 2. where compiled code differs on purpose (int overflow wraps): a golden file
got="$("$aembr" run "$dir/wrap.embr" 2>&1)"
[[ "$got" == "$(cat "$dir/wrap.out")" ]] || bad wrap "output differs from wrap.out"

# golden programs the VM does not run (optional and in-out parameters are aembr only), and that the VM refuses
for f in "$dir"/golden/*.embr; do
    n="$(basename "$f" .embr)"
    got="$("$aembr" run "$f" 2>&1)"
    [[ "$got" == "$(cat "${f%.embr}.out")" ]] || bad "golden $n" "output differs from $n.out"
    vm="$("$embr" "$f" 2>&1)"; rc=$?
    [[ $rc -ne 0 && "$vm" == *"is for aembr"* ]] || bad "golden $n" "the VM should refuse it (exit $rc): ${vm:0:200}"
done

# 3. refusals: exit 1, file:line:col, and the words the file asks for on its first line
for f in "$dir"/refuse/*.embr; do
    n="$(basename "$f" .embr)"
    want="$(sed -n '1s/^# refuse: //p' "$f")"
    out="$("$aembr" check "$f" 2>&1)"; rc=$?
    [[ $rc -eq 1 ]] || bad "refuse $n" "exit $rc, wanted 1"
    [[ "$out" == *"outside the compiled subset"* && "$out" == *"$want"* ]] || bad "refuse $n" "wanted '$want', got: ${out:0:200}"
    [[ "$out" =~ ^[^:]+:[0-9]+:[0-9]+: ]] || bad "refuse $n" "message has no file:line:col"
done

# 4. a runtime error: message, line, trace, exit 1, and nothing printed after it
out="$("$aembr" run "$dir/error/div_zero.embr" 2>&1)"; rc=$?
[[ $rc -eq 1 ]] || bad div_zero "exit $rc, wanted 1"
[[ "$out" == "1"$'\n'"error: division by zero at line 2"* ]] || bad div_zero "wrong report: ${out:0:200}"
[[ "$out" == *"in half() called at line 5 of "* && "$out" == *"in outer() called at line 8 of "* ]] || bad div_zero "wrong trace: $out"
out="$("$aembr" run "$dir/error/mod_zero.embr" 2>&1)"; [[ "$out" == "error: modulo by zero at line 2" ]] || bad mod_zero "$out"
out="$("$aembr" run "$dir/error/shift.embr" 2>&1)"; [[ "$out" == *"shift count 64 is out of range, it has to be 0 to 63 at line 1" ]] || bad shift "$out"
out="$("$aembr" run "$dir/error/recursion.embr" 2>&1)"; rc=$?
[[ $rc -eq 1 && "$out" == "error: stack overflow"* && "$out" == *"more frames)"* ]] || bad recursion "exit $rc: ${out:0:200}"

out="$("$aembr" run "$dir/error/index_oob.embr" 2>&1)"; [[ "$out" == "error: array index 5 out of bounds at line 2" ]] || bad index_oob "$out"
out="$("$aembr" run "$dir/error/neg_index.embr" 2>&1)"; [[ "$out" == "1"$'\n'"error: array index -1 out of bounds at line 2"* && "$out" == *"in get() called at line 5"* ]] || bad neg_index "$out"
out="$("$aembr" run "$dir/error/str_oob.embr" 2>&1)"; [[ "$out" == "error: string index 3 out of bounds at line 2" ]] || bad str_oob "$out"

out="$("$aembr" run "$dir/error/uncaught.embr" 2>&1)"; rc=$?
[[ $rc -eq 1 && "$out" == "1"$'\n'"error: too big: 5 at line 4"* && "$out" == *"in check() called at line 8 of "* && "$out" == *"in run() called at line 12 of "* ]] || bad uncaught "exit $rc: $out"
out="$("$aembr" run "$dir/error/key_missing.embr" 2>&1)"; [[ "$out" == "error: key not found: zz at line 2" ]] || bad key_missing "$out"

out="$("$aembr" run "$dir/error/closure_trace.embr" 2>&1)"; rc=$?
[[ $rc -eq 1 && "$out" == "1"$'\n'"error: deep at line 4"* && "$out" == *"in <lambda>() called at line 9 of "* && "$out" == *"in call() called at line 12 of "* ]] || bad closure_trace "exit $rc: $out"

# the library written in aembr needs no `native` capability, even under --deny-caps=native
out="$("$aembr" run --deny-caps=native "$dir/ok/std_names.embr" 2>&1)"; rc=$?
[[ $rc -eq 0 ]] || bad std-deny-native "the library's C text should not need native: ${out:0:200}"

# --fatal-overflow: programs mean the same, a stack overflow stops the program with its message and exit 1 (no trace), a plugin refuses the flag
for n in higher_order closure_bench const_fn nested; do
    f="$dir/ok/$n.embr"; [[ -f "$f" ]] || continue
    want="$("$embr" "$f" 2>/dev/null | sed -E 's|file: "[^"]*"|file: "F"|g')"
    got="$("$aembr" run --fatal-overflow "$f" 2>&1 | sed -E 's|file: "[^"]*"|file: "F"|g')"
    [[ "$got" == "$want" ]] || bad "fatal-overflow $n" "output differs from the VM"
done
out="$(timeout 120 "$aembr" run --fatal-overflow "$dir/error/recursion.embr" 2>&1)"; rc=$?
[[ $rc -eq 1 && "$out" == "error: stack overflow: the stack budget is used up at line 1" ]] || bad fatal-overflow "exit $rc: ${out:0:200}"
out="$("$aembr" plugin --fatal-overflow "$dir/plugin/clib.embr" -o "$work/nofatal.so" 2>&1)"; rc=$?
[[ $rc -ne 0 && "$out" == *"--fatal-overflow is for programs"* ]] || bad fatal-overflow "a plugin should refuse the flag: ${out:0:200}"

# a program of several files: the trace names the file each call is written in, and what is not found is said
out="$("$aembr" run "$dir/error/module_trace.embr" 2>&1)"; rc=$?
[[ $rc -eq 1 && "$out" == *"in boom() called at line "*" of "*"ok/mod/util.embr"* && "$out" == *"in outer() called at line 3 of "*"module_trace.embr"* ]] || bad module-trace "exit $rc: $out"
printf 'import "nothere.embr"\n' > "$work/nomod.embr"
out="$("$aembr" check "$work/nomod.embr" 2>&1)"; rc=$?
[[ $rc -ne 0 && "$out" == *'cannot find the module "nothere.embr"'* ]] || bad module-missing "$out"

# the counter build: --stats prints what the program did when it ends (and a normal build prints nothing of the kind)
out="$("$aembr" run --stats "$dir/ok/array.embr" 2>&1 >/dev/null)"
[[ "$out" == "[aembr stats]"* && "$out" == *"calls "* && "$out" == *"arr_get "* ]] || bad stats "no counters from --stats: ${out:0:200}"
out="$("$aembr" run "$dir/ok/array.embr" 2>&1 >/dev/null)"
[[ "$out" != *"aembr stats"* ]] || bad stats "a normal build should not print counters"

# 5. the C text compiles without a warning, and the same input gives the same C and the same program
rt="$(dirname "$aembr")/aembr-rt"
for f in "$dir"/ok/*.embr "$dir"/wrap.embr; do
    n="$(basename "$f" .embr)"
    "$aembr" build --emit-c "$f" > "$work/$n.c" || { bad "emit-c $n" "failed"; continue; }
    cc -std=gnu11 -Wall -Wextra -Werror -fsyntax-only -I"$rt" "$work/$n.c" 2> "$work/cc.err" || bad "emit-c $n" "cc warns: $(head -3 "$work/cc.err")"
    "$aembr" build --emit-c "$f" | cmp -s - "$work/$n.c" || bad "repro $n" "two runs gave different C"
done
# the same file name in two folders: macOS writes the name into the program's signature.
# ASan writes the temp path of the C file into it, so a sanitizer build is never the same twice
mkdir -p "$work/a" "$work/b"
"$aembr" build "$dir/ok/fib.embr" -o "$work/a/fib" && "$aembr" build "$dir/ok/fib.embr" -o "$work/b/fib" || bad repro "build failed"
(( sanitized )) || cmp -s "$work/a/fib" "$work/b/fib" || bad repro "two builds gave different programs"
[[ "$("$work/a/fib")" == "$("$embr" "$dir/ok/fib.embr")" ]] || bad build "the built program prints something else"

# a plugin that needs a capability can be refused when the program is compiled
printf 'import "fs"\nprint(fs_exists("/"))\n' > "$work/needfs.embr"
out="$("$aembr" check "$work/needfs.embr" --deny-caps=filesystem 2>&1)"; rc=$?
[[ $rc -eq 1 && "$out" == *"needs filesystem"* && "$out" == *"--deny-caps"* ]] || bad deny-caps "exit $rc: $out"
"$aembr" check "$work/needfs.embr" >/dev/null 2>&1 || bad deny-caps-off "the same file should compile without --deny-caps"
"$aembr" check "$work/needfs.embr" --deny-caps=bogus >/dev/null 2>&1; [[ $? -eq 2 ]] || bad deny-caps-name "an unknown capability should exit 2"

# 6. the runtime under the address and leak sanitizers: every program, also the ones that end in an error, frees all it made
rt_src="$(dirname "$0")/../tools/aembr/rt"
if [[ -f "$rt_src/rt.c" ]] && printf 'int main(void){return 0;}\n' | cc -x c -fsanitize=address,undefined - -o "$work/san_probe" 2>/dev/null && "$work/san_probe" 2>/dev/null; then
    printf '#include <stdlib.h>\nint main(void){ volatile void* p = malloc(8); p = 0; return 0; }\n' > "$work/leak.c"
    cc -fsanitize=address -O0 "$work/leak.c" -o "$work/leak" 2>/dev/null
    if ! "$work/leak" >/dev/null 2>&1; then     # leak detection works here (it does not under some debuggers and containers)
        for f in "$dir"/ok/*.embr "$dir"/wrap.embr "$dir"/error/*.embr "$dir"/infer/*.embr; do
            n="$(basename "$f" .embr)"
            "$aembr" build --infer --emit-c "$f" > "$work/s_$n.c" || continue
            cc -std=gnu11 -O1 -g -fwrapv -fno-strict-aliasing -fsanitize=address,undefined -fno-sanitize-recover=undefined -I"$rt" \
                "$work/s_$n.c" "$rt_src/rt.c" "$rt_src/rt_nat.c" "$rt_src/rt_host.c" "$rt_src/rt_any.c" -lm -ldl -o "$work/s_$n" 2> "$work/cc.err" || { bad "sanitize $n" "cc failed: $(head -3 "$work/cc.err")"; continue; }
            EMBR_STACK_KB=512 "$work/s_$n" > /dev/null 2> "$work/san.err"
            grep -q 'ERROR: \(AddressSanitizer\|LeakSanitizer\)\|runtime error' "$work/san.err" && bad "sanitize $n" "$(grep -m1 'ERROR\|runtime error' "$work/san.err")"
        done
    else
        echo "aembr_smoke: leak detection is not available here, the sanitizer step is skipped"
    fi
fi

# 6b. --infer: parameter and result types left out are worked out from the calls, and the program means the same as in the VM
for f in "$dir"/infer/*.embr; do
    n="$(basename "$f" .embr)"
    want="$("$embr" "$f" 2>/dev/null)"
    got="$("$aembr" run --infer "$f" 2>&1)"; rc=$?
    [[ $rc -eq 0 ]] || bad "infer $n" "aembr run --infer gave exit $rc: ${got:0:200}"
    [[ "$got" == "$want" ]] || bad "infer $n" "output differs from the VM"
    "$aembr" check "$f" >/dev/null 2>&1 && bad "infer $n" "without --infer the file should be refused"
    "$aembr" build --infer --emit-c "$f" > "$work/i_$n.c" || { bad "infer $n" "emit-c failed"; continue; }
    cc -std=gnu11 -Wall -Wextra -Werror -fsyntax-only -I"$rt" "$work/i_$n.c" 2> "$work/cc.err" || bad "infer $n" "cc warns: $(head -3 "$work/cc.err")"
done
for f in "$dir"/infer_refuse/*.embr; do
    n="$(basename "$f" .embr)"
    want="$(sed -n '1s/^# refuse: //p' "$f")"
    out="$("$aembr" check --infer "$f" 2>&1)"; rc=$?
    [[ $rc -eq 1 && "$out" == *"outside the compiled subset"* && "$out" == *"$want"* ]] || bad "infer refuse $n" "exit $rc, wanted '$want', got: ${out:0:200}"
done
out="$("$aembr" check "$dir/infer/fib.embr" 2>&1)"
[[ "$out" == *"--infer"* ]] || bad infer-hint "the refusal should say that --infer exists: $out"

# 6c. the .aembr extension means the same as .embr: the name of the output comes from the file's name without it
cp "$dir/infer/fib.embr" "$work/fibx.aembr"
out="$("$aembr" run --infer "$work/fibx.aembr" 2>&1)"
[[ "$out" == "$("$embr" "$dir/infer/fib.embr" 2>/dev/null)" ]] || bad aembr-ext "a .aembr file gave: ${out:0:200}"
"$aembr" build --infer "$work/fibx.aembr" || bad aembr-ext "build failed"
[[ -x "$work/fibx" ]] || bad aembr-ext "the program should be called fibx"
"$aembr" info "$work/fibx" | grep -q "source: fibx.aembr" || bad aembr-ext "info should name fibx.aembr"

# 6d. C text (c """ ... """): it is compiled in, it counts as native code, and embr itself refuses it
for f in "$dir"/inline/*.embr; do
    n="$(basename "$f" .embr)"
    got="$("$aembr" run "$f" 2>&1)"; rc=$?
    [[ $rc -eq 0 ]] || bad "inline $n" "exit $rc: ${got:0:200}"
    [[ "$got" == "$(cat "${f%.embr}.out")" ]] || bad "inline $n" "output differs from $(basename "${f%.embr}.out"): ${got:0:200}"
    "$aembr" build --emit-c "$f" > "$work/c_$n.c" || { bad "inline $n" "emit-c failed"; continue; }
    cc -std=gnu11 -Wall -Wextra -Werror -fsyntax-only -I"$rt" "$work/c_$n.c" 2> "$work/cc.err" || bad "inline $n" "cc warns: $(head -3 "$work/cc.err")"
done
out="$("$aembr" check "$dir/inline/basic.embr" --deny-caps=native 2>&1)"; rc=$?
[[ $rc -eq 1 && "$out" == *"native code"* && "$out" == *"--deny-caps"* ]] || bad inline-deny "exit $rc: $out"
"$aembr" build "$dir/inline/basic.embr" -o "$work/inl" && "$aembr" info "$work/inl" | grep -q "needs: native" || bad inline-caps "info should say the program needs native"
out="$("$embr" "$dir/inline/basic.embr" 2>&1)"; rc=$?
[[ $rc -ne 0 && "$out" == *"C text for aembr"* ]] || bad inline-embr "embr should refuse C text: ${out:0:200}"

# 6e. const import and const c import
out="$("$aembr" run "$dir/include/const_import.embr" 2>&1)"
[[ "$out" == "$(cat "$dir/include/const_import.out")" ]] || bad const-import "output differs: ${out:0:200}"
out="$("$aembr" run "$dir/include/file_c_import.embr" 2>&1)"
[[ "$out" == "$(cat "$dir/include/file_c_import.out")" ]] || bad file-c "const c file import: ${out:0:200}"
"$aembr" build --emit-c "$dir/include/const_import.embr" | grep -q '#line 1 "add_one.c"' || bad const-c-import "the C text should point at its own file"

# 6f. os_args: the words after -- reach the program
printf 'import "os"\nimport "embrlib"\nprint(os_args(), len(os_args()))\n' > "$work/args.embr"
out="$("$aembr" run "$work/args.embr" -- x "y z" 2>&1)"
[[ "$out" == '["x", "y z"] 2' ]] || bad os-args "got: ${out:0:200}"

# 7. usage
"$aembr" >/dev/null 2>&1; [[ $? -eq 2 ]] || bad usage "no arguments should exit 2"
"$aembr" check "$work/missing.embr" >/dev/null 2>&1; [[ $? -eq 1 ]] || bad usage "a missing file should exit 1"
"$aembr" build "$dir/ok/fib.embr" --cc=/no/such/cc -o "$work/x" >/dev/null 2>&1; [[ $? -eq 2 ]] || bad usage "a missing C compiler should exit 2"

(( fail )) && exit 1
echo "aembr_smoke: ok"
