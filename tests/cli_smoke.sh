#!/usr/bin/env bash
# runs the real `embr` binary on malformed and edge-case scripts and checks it never dies from a signal or
# an uncaught C++ exception (exit >= 128 / "terminate called") and reports a readable error instead.
# the unit tests call the library directly, so a bug in the CLI layer (src/cli.cpp) -- like lexer errors
# escaping main() and killing the process -- is invisible to them.
#
#   cli_smoke.sh <embr binary> [--backend=NAME]
set -u
embr="$1"; shift
flags=("$@")   # (empty array under set -u needs the ${flags[@]+...} form, bash 3.2 on macos)
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
fail=0

check() {   # name, source, expected substring on stderr+stdout (or "" for "anything, but no crash")
    local name="$1" src="$2" want="$3"
    printf '%s\n' "$src" > "$work/t.embr"
    local out rc
    out="$("$embr" ${flags[@]+"${flags[@]}"} "$work/t.embr" 2>&1)"; rc=$?
    if (( rc >= 128 )) || [[ "$out" == *"terminate called"* ]]; then
        echo "FAIL [$name]: crashed (exit $rc): ${out:0:200}"; fail=1; return
    fi
    if [[ -n "$want" && "$out" != *"$want"* ]]; then
        echo "FAIL [$name]: expected output containing '$want', got: ${out:0:200}"; fail=1; return
    fi
}

check "element type on a non-container" 'fn f(a: int<str>) end'  "only arr and map take an element type"
check "element type left open"  'local arr<int xs = []'        "to close the element type"
check "const assigned again"       'const N = 1
N = 2' "which is a const"
check "const that is not constant" 'x = 3
const N = x + 1' "not a constant"
check "const inside a function"    'fn g()
  const N = 1
end
g()' "top level of a file"
check "const fn with a try"        'const fn f(a) try
a = 1
catch e
end
return a end' "cannot contain a try block"
check "const fn with a closure"    'const fn f(a) return fn(y) return y end end' "cannot contain a function value"
check "const fn changing a global" 'const fn f(a)
  zzz = a
  return a
end' "can only change its own parameters and locals"
check "const fn with a rest param" 'const fn f(...r) return 1 end' "plain parameters"
check "const without a value"      'const N' "expected '='"
check "unknown escape"        'print("\q")'                  "unknown escape"
check "unterminated string"   'print("abc'                   "unterminated string"
check "multiple decimal pts"  'x = 1.2.3'                    "multiple decimal points"
check "trailing decimal pt"   'x = 5.'                       "trailing decimal point"
check "bad \x escape"         'print("\x4")'                 "hex digits"
check "bad \u escape"         'print("\uD800")'              "Unicode scalar"
check "number out of range"   'print(1e999)'                 "out of range"
check "int literal overflow"  'print(99999999999999999999999)' ""
check "deep nesting"          "x = $(printf '(%.0s' $(seq 1 20000))1$(printf ')%.0s' $(seq 1 20000))" "nesting too deep"
check "long operator chain"   "x = 1$(printf '+1%.0s' $(seq 1 20000))" "too deeply nested"
check "bare return at EOF"    'return'                       "unexpected token"
check "top-level return"      'print("a")
return 1
print("b")'                                                  ""
check "stray break"           'break'                        ""
check "runtime error + trace" 'fn f()
  error("boom")
end
f()'                                                          "stack trace"
check "auto was removed"      'local auto x = 5'             "'auto' was removed"
check "undefined variable"    'print(nope_not_defined)'      "undefined variable"
check "empty file"            ''                             ""
check "only comments"         '# hi
#[ block ]#'                                                 ""

# ---- `embr check`: parse + compile without running (must not execute anything, must report every error)
printf 'print("SHOULD NOT RUN")\nx = 1\n' > "$work/good.embr"
out="$("$embr" check "$work/good.embr" 2>&1)"; rc=$?
[[ $rc -eq 0 && -z "$out" ]] || { echo "FAIL [check good file]: rc=$rc out=$out"; fail=1; }
printf 'fn f(a,\n  return a\nend\nprint("x")\nbreak\n' > "$work/bad.embr"
out="$("$embr" check "$work/bad.embr" 2>&1)"; rc=$?
[[ $rc -eq 1 ]] || { echo "FAIL [check bad file]: expected exit 1, got $rc"; fail=1; }
[[ "$out" == *"bad.embr:2:"* && "$out" == *"bad.embr:5:"* ]] || { echo "FAIL [check bad file]: expected two located errors, got: $out"; fail=1; }
printf 'print("a\\q")\n' > "$work/lex.embr"
out="$("$embr" check "$work/lex.embr" 2>&1)"; rc=$?
[[ $rc -eq 1 && "$out" == *"lex.embr:1:"*"unknown escape"* ]] || { echo "FAIL [check lexer error]: rc=$rc out=$out"; fail=1; }
out="$("$embr" check "$work/good.embr" "$work/bad.embr" "$work/none.embr" 2>&1)"; rc=$?
[[ $rc -eq 1 && "$out" == *"none.embr: error: cannot open"* ]] || { echo "FAIL [check several files]: rc=$rc out=$out"; fail=1; }
"$embr" check >/dev/null 2>&1; [[ $? -eq 2 ]] || { echo "FAIL [check with no file]: expected exit 2"; fail=1; }

# ---- `embr test`: pass/fail accounting, exit status, filter, discovery, setup/teardown
mkdir -p "$work/tests"
cat > "$work/tests/test_a.embr" <<'EMBR'
import "testing.embr"
fn test_ok() assert_eq(1, 1) end
fn test_bad() assert_eq(1, 2) end
fn helper_not_a_test() error("must not run") end
EMBR
cat > "$work/tests/b_test.embr" <<'EMBR'
import "testing.embr"
fn test_fine() assert_true(1) end
EMBR
printf 'x = = 1\n' > "$work/tests/test_syntax.embr"
out="$("$embr" ${flags[@]+"${flags[@]}"} test "$work/tests" 2>&1)"; rc=$?
[[ $rc -eq 1 ]] || { echo "FAIL [embr test with failures]: expected exit 1, got $rc"; fail=1; }
[[ "$out" == *"PASS test_ok"* && "$out" == *"FAIL test_bad"* && "$out" == *"PASS test_fine"* ]] || { echo "FAIL [embr test results]: $out"; fail=1; }
[[ "$out" == *"2 passed"*"3 failed"* || "$out" == *"2 passed, 2 failed"* ]] || { echo "FAIL [embr test summary]: ${out: -120}"; fail=1; }
[[ "$out" != *"must not run"* ]] || { echo "FAIL [embr test ran a non-test function]"; fail=1; }
[[ "$out" == *"while loading"* ]] || { echo "FAIL [embr test syntax error file]: $out"; fail=1; }
out="$("$embr" ${flags[@]+"${flags[@]}"} test --filter=fine "$work/tests/b_test.embr" 2>&1)"; rc=$?
[[ $rc -eq 0 && "$out" == *"1 passed, 0 failed"* ]] || { echo "FAIL [embr test --filter / single file]: rc=$rc $out"; fail=1; }
"$embr" ${flags[@]+"${flags[@]}"} test "$work/nothing_here" >/dev/null 2>&1; [[ $? -eq 2 ]] || { echo "FAIL [embr test missing path]: expected exit 2"; fail=1; }
mkdir -p "$work/empty"; "$embr" ${flags[@]+"${flags[@]}"} test "$work/empty" >/dev/null 2>&1; [[ $? -eq 2 ]] || { echo "FAIL [embr test no test files]: expected exit 2"; fail=1; }

# piped REPL: bare expressions echo their value, statements and blocks run quietly, no prompt noise
out="$(printf 'x = 5\nx * 2\nprint("hi")\nif x > 1\nprint("blk")\nend\n' | "$embr" ${flags[@]+"${flags[@]}"} 2>&1)"
[[ "$out" == $'10\nhi\nblk' ]] || { echo "FAIL [piped repl echo]: $out"; fail=1; }

# a missing script file and --version must also behave
"$embr" ${flags[@]+"${flags[@]}"} "$work/does_not_exist.embr" >/dev/null 2>&1; (( $? >= 128 )) && { echo "FAIL: missing file crashed"; fail=1; }
"$embr" --version | grep -q "backends:" || { echo "FAIL: --version"; fail=1; }

# embr dis: shows the compiled code, runs nothing, loads no plugin, and fails cleanly on a bad script
printf 'x = 1 + 2\nfn f(a)\n  return a\nend\nprint(x)\n' > "$work/d.embr"
out="$("$embr" ${flags[@]+"${flags[@]}"} dis "$work/d.embr" 2>&1)"; rc=$?
[[ $rc -eq 0 && "$out" == *'== chunk 0 "<top>"'* && "$out" == *'PUSH_INT'* && "$out" == *'== chunk 1 "f" (a)'* && "$out" == *'RETURN'* ]] || { echo "FAIL [embr dis]: rc=$rc ${out:0:300}"; fail=1; }
[[ "$out" != *"print"*"hello"* ]] || { echo "FAIL [embr dis ran the script]"; fail=1; }
out="$("$embr" ${flags[@]+"${flags[@]}"} dis --chunk=1 "$work/d.embr" 2>&1)"
[[ "$out" == '== chunk 1'* && "$out" != *'== chunk 0'* ]] || { echo "FAIL [embr dis --chunk]: ${out:0:200}"; fail=1; }
printf 'x = = 1\n' > "$work/bad.embr"
"$embr" ${flags[@]+"${flags[@]}"} dis "$work/bad.embr" >/dev/null 2>&1; (( $? == 0 || $? >= 128 )) && { echo "FAIL [embr dis bad script]: expected a clean error exit"; fail=1; }
"$embr" ${flags[@]+"${flags[@]}"} dis >/dev/null 2>&1; [[ $? -eq 2 ]] || { echo "FAIL [embr dis no file]: expected exit 2"; fail=1; }

# exit status: 0 when the script runs, 1 when it raises an error or can't be read, for source and compiled scripts alike
printf 'print("fine")\n' > "$work/ok.embr"
printf 'print("before")\nerror("boom")\n' > "$work/err.embr"
"$embr" ${flags[@]+"${flags[@]}"} "$work/ok.embr" >/dev/null 2>&1; [[ $? -eq 0 ]] || { echo "FAIL [exit status of a good script]"; fail=1; }
out="$("$embr" ${flags[@]+"${flags[@]}"} "$work/err.embr" 2>&1)"; rc=$?
[[ $rc -eq 1 && "$out" == *before* && "$out" == *"undefined variable"* ]] || { echo "FAIL [exit status of a script that errors]: rc=$rc ${out:0:150}"; fail=1; }
"$embr" ${flags[@]+"${flags[@]}"} "$work/does_not_exist.embr" >/dev/null 2>&1; [[ $? -eq 1 ]] || { echo "FAIL [exit status of a missing script]"; fail=1; }
"$embr" ${flags[@]+"${flags[@]}"} compile "$work/err.embr" >/dev/null 2>&1; "$embr" ${flags[@]+"${flags[@]}"} compile "$work/ok.embr" >/dev/null 2>&1
out="$("$embr" ${flags[@]+"${flags[@]}"} "$work/err.vmbr" 2>&1)"; rc=$?
[[ $rc -eq 1 && "$out" == *"undefined variable"* ]] || { echo "FAIL [exit status of a compiled script that errors]: rc=$rc ${out:0:150}"; fail=1; }
"$embr" ${flags[@]+"${flags[@]}"} "$work/ok.vmbr" >/dev/null 2>&1; [[ $? -eq 0 ]] || { echo "FAIL [exit status of a good compiled script]"; fail=1; }
printf 'junk' > "$work/bad.vmbr"; "$embr" ${flags[@]+"${flags[@]}"} "$work/bad.vmbr" >/dev/null 2>&1; [[ $? -eq 1 ]] || { echo "FAIL [exit status of a damaged compiled script]"; fail=1; }
# every file still runs after one fails, and the status says one did
out="$("$embr" ${flags[@]+"${flags[@]}"} "$work/err.embr" "$work/ok.embr" 2>&1)"; rc=$?
[[ $rc -eq 1 && "$out" == *fine* ]] || { echo "FAIL [a file after a failing one]: rc=$rc ${out:0:150}"; fail=1; }

# static type errors: found before the script runs, so nothing is printed first, and the exit status is 1
printf 'print("ran")\nx = 1\nx = "a"\n' > "$work/te.embr"
out="$("$embr" ${flags[@]+"${flags[@]}"} "$work/te.embr" 2>&1)"; rc=$?
[[ $rc -eq 1 && "$out" == *"cannot assign str to 'x'"* && "$out" != *ran* ]] || { echo "FAIL [static type error stops the script]: rc=$rc ${out:0:150}"; fail=1; }
out="$("$embr" ${flags[@]+"${flags[@]}"} check "$work/te.embr" 2>&1)"; rc=$?
[[ $rc -eq 1 && "$out" == *"te.embr:3:5: type: cannot assign str to 'x'"* ]] || { echo "FAIL [embr check shows the type error with its position]: rc=$rc ${out:0:150}"; fail=1; }
# check knows the functions of the plugins the file imports, and keeps their "loaded plugin" lines out of its output
printf 'import "json"\nx = json_parse(5)\n' > "$work/tn.embr"
out="$("$embr" ${flags[@]+"${flags[@]}"} check "$work/tn.embr" 2>&1)"; rc=$?
[[ $rc -eq 1 && "$out" == *"tn.embr:2:16: type: argument 'text' to 'json_parse'"* && "$out" != *"loaded plugin"* ]] || { echo "FAIL [embr check type checks a plugin call]: rc=$rc ${out:0:200}"; fail=1; }
# the script you run is the whole program, so its own functions get parameter types from their calls
printf 'fn f(a)\n  a = "s"\nend\nf(1)\n' > "$work/te2.embr"
"$embr" ${flags[@]+"${flags[@]}"} "$work/te2.embr" >/dev/null 2>&1; [[ $? -eq 1 ]] || { echo "FAIL [parameter type from the call]"; fail=1; }
printf 'print("fine")\nx = len([1])\nx = "a"\n' > "$work/te3.embr"
out="$("$embr" ${flags[@]+"${flags[@]}"} "$work/te3.embr" 2>&1)"; [[ "$out" == *fine* ]] || { echo "FAIL [a name the checker can't see must not be an error]: ${out:0:150}"; fail=1; }

# the plugins a script imports at its top level are known to the checker before it runs: a wrong argument to one of their
# functions is found up front, and a plugin that is not allowed is simply not known (the import reports it when it runs)
export EMBR_PATH="${EMBR_PATH:-$(dirname "$embr")}"
printf 'import "str"\nprint("ran")\nprint(str_upper(5))\n' > "$work/pl.embr"
out="$("$embr" ${flags[@]+"${flags[@]}"} "$work/pl.embr" 2>&1)"; rc=$?
[[ $rc -eq 1 && "$out" == *"argument 'string' to 'str_upper': expected str but got int"* && "$out" != *ran* ]] || { echo "FAIL [plugin signature checked before the script runs]: rc=$rc ${out:0:200}"; fail=1; }
printf 'import "str"\nprint("ran")\nlocal any x = 5\nprint(str_upper(x))\n' > "$work/pl2.embr"
out="$("$embr" ${flags[@]+"${flags[@]}"} "$work/pl2.embr" 2>&1)"; rc=$?
[[ "$out" == *ran* && "$out" == *"expected string but got int"* ]] || { echo "FAIL [the run-time check still works for a value the checker can't see]: ${out:0:200}"; fail=1; }
out="$("$embr" ${flags[@]+"${flags[@]}"} --no-plugins "$work/pl.embr" 2>&1)"
[[ "$out" == *"not allowed in this sandbox"* && "$out" != *"[type]"* ]] || { echo "FAIL [a denied plugin is not preloaded]: ${out:0:200}"; fail=1; }
# a variable keeps its type from file to file on one command line
printf 'shared_v = 1\n' > "$work/f1.embr"; printf 'print("second")\nshared_v = "x"\n' > "$work/f2.embr"
out="$("$embr" ${flags[@]+"${flags[@]}"} "$work/f1.embr" "$work/f2.embr" 2>&1)"; rc=$?
[[ $rc -eq 1 && "$out" == *"cannot assign str to 'shared_v'"* && "$out" != *second* ]] || { echo "FAIL [a global keeps its type in the next file]: rc=$rc ${out:0:200}"; fail=1; }

# running out of memory is an error a script can catch, not a crash. a memory limit keeps the test small. a
# sanitizer build can't start under one (it reserves a huge address range), so those builds skip this part
if ( ulimit -v 400000 2>/dev/null; "$embr" --version >/dev/null 2>&1 ); then
printf 'a = "x"\ntry\n  while 1\n    a = a + a\n  end\ncatch e\n  print("caught")\nend\nprint("after")\n' > "$work/oom.embr"
out="$( ulimit -v 400000 2>/dev/null; "$embr" ${flags[@]+"${flags[@]}"} "$work/oom.embr" 2>&1 )"; rc=$?
[[ $rc -eq 0 && "$out" == *caught* && "$out" == *after* ]] || { echo "FAIL [out of memory is a catchable error]: rc=$rc ${out:0:200}"; fail=1; }
printf 'import "embrlib"\na = [1]\nwhile 1\n  a = concat(a, a)\nend\n' > "$work/oom2.embr"
out="$( ulimit -v 400000 2>/dev/null; "$embr" ${flags[@]+"${flags[@]}"} "$work/oom2.embr" 2>&1 )"; rc=$?
[[ $rc -eq 1 && "$out" == *"out of memory"* ]] || { echo "FAIL [uncaught out of memory is an error with exit 1]: rc=$rc ${out:0:200}"; fail=1; }

fi

# a step or time limit stops a script that never ends, and the script can't catch it
budget() {   # name, source, flag, expected substring
    printf '%s\n' "$2" > "$work/b.embr"
    local out rc
    out="$(timeout 20 "$embr" ${flags[@]+"${flags[@]}"} "$3" "$work/b.embr" 2>&1)"; rc=$?
    if [[ $rc -ne 1 || "$out" != *"$4"* || "$out" == *"swallowed"* ]]; then
        echo "FAIL [$1]: rc=$rc ${out:0:200}"; fail=1
    fi
}
budget "while loop"      'while 1 end'                                         --max-steps=1000 "step limit of 1000"
budget "for loop"        'import "embrlib"
for i in range(0, 5000000)
end'                                                                           --max-steps=1000 "step limit"
budget "recursion"       'fn f(n)
  if n > 0
    f(n - 1)
    f(n - 1)
  end
end
f(40)'                                                                          --max-steps=5000 "step limit"
budget "try cannot catch" 'while 1
  try
    while 1 end
  catch e
    print("swallowed")
  end
end'                                                                           --max-steps=500 "step limit"
budget "time limit"      'while 1 end'                                         --timeout=0.3 "time limit of 0.3 seconds"
out="$("$embr" ${flags[@]+"${flags[@]}"} --max-steps=abc "$work/t.embr" 2>&1)"; rc=$?
[[ $rc -eq 2 && "$out" == *"needs a number above 0"* ]] || { echo "FAIL [a bad --max-steps is refused]: rc=$rc ${out:0:200}"; fail=1; }
printf 'n = 0\nwhile n < 50\n  n = n + 1\nend\nprint(n)\n' > "$work/ok.embr"
out="$("$embr" ${flags[@]+"${flags[@]}"} --max-steps=1000 --timeout=5 "$work/ok.embr" 2>&1)"
[[ "$out" == "50" ]] || { echo "FAIL [a script inside its limits runs normally]: ${out:0:200}"; fail=1; }

# a runtime error points at the part of the line that failed, with a caret
printf 'm = {"a": 1}\nprint(m["zz"])\n' > "$work/c1.embr"
out="$("$embr" ${flags[@]+"${flags[@]}"} "$work/c1.embr" 2>&1)"
[[ "$out" == *"at line 2, col 7"* && "$out" == *$'\n        ^^^^^^^'* ]] || { echo "FAIL [a runtime error underlines the failing expression]: ${out:0:300}"; fail=1; }
printf 'import "str"\nlocal any x = 5\nprint(  str_upper(x))\n' > "$work/c2.embr"
out="$("$embr" ${flags[@]+"${flags[@]}"} "$work/c2.embr" 2>&1)"
[[ "$out" == *"col 9"* && "$out" == *$'\n          ^^^^^^^^^^^^'* ]] || { echo "FAIL [an error from a native points at the call]: ${out:0:300}"; fail=1; }

# the types a script module makes are known to the file that imports it, with the elements of its arrays and what its functions return
mkdir -p "$work/modt"
printf 'shared = 5\nnames = ["a", "b"]\nlocal hidden = 1\nfn make() -> arr<int>\n    return [1, 2]\nend\n' > "$work/modt/mod.embr"
printf 'import "mod.embr"\nshared = "x"\nnames[0] = 3\nx = make()\nx[0] = "s"\nhidden = "fine"\n' > "$work/modt/main.embr"
out="$("$embr" ${flags[@]+"${flags[@]}"} check "$work/modt/main.embr" 2>&1)"
[[ "$out" == *"cannot assign str to 'shared', which holds int"* && "$out" == *"cannot put int into arr<str>"* && "$out" == *"cannot put str into arr<int>"* && "$out" != *hidden* && "$out" != *"col 1:"* ]] || { echo "FAIL [an import brings in the types of the module's names]: ${out:0:400}"; fail=1; }

# a const may call a pure native with constant arguments; anything else names the reason
printf 'import "embrlib"\nimport "embrmath"\nconst N = len("abc") * 2\nprint(N)\n' > "$work/k1.embr"
out="$("$embr" ${flags[@]+"${flags[@]}"} "$work/k1.embr" 2>&1)"
[[ "$out" == *$'\n6'* || "$out" == "6" ]] || { echo "FAIL [a const calls a pure native]: ${out:0:200}"; fail=1; }
out="$("$embr" ${flags[@]+"${flags[@]}"} dis "$work/k1.embr" 2>&1)"
[[ "$out" == *"PUSH_INT             6"* ]] || { echo "FAIL [the const is stored as the worked-out value]: ${out:0:400}"; fail=1; }
printf 'import "embrlib"\nimport "embrmath"\nconst N = math_random()\n' > "$work/k2.embr"
out="$("$embr" ${flags[@]+"${flags[@]}"} "$work/k2.embr" 2>&1)"
[[ "$out" == *"not a constant: 'math_random' is not a pure native"* ]] || { echo "FAIL [a const can't call an impure native]: ${out:0:200}"; fail=1; }
printf 'const N = unicode_len("a")\n' > "$work/k3.embr"
out="$("$embr" ${flags[@]+"${flags[@]}"} "$work/k3.embr" 2>&1)"
[[ "$out" == *"'unicode_len' is not known yet when the file is compiled"* ]] || { echo "FAIL [a const calling a plugin that isn't imported]: ${out:0:200}"; fail=1; }

# ---- what only the command line reaches: the ast() debug builtin, `embr pkg`, an unknown backend, a write that fails
printf 'ast("x = 1 + 2\\nfn f(a: int) -> int\\n  return -a\\nend\\nfor i in [1]\\n  break\\nend\\n")\n' > "$work/ast.embr"
out="$("$embr" ${flags[@]+"${flags[@]}"} "$work/ast.embr" 2>&1)"
[[ "$out" == *"Program (3 statements)"* && "$out" == *"Fn f(a: int) -> int  (L2)"* && "$out" == *"Unary -  (L3)"* && "$out" == *"Break  (L6)"* ]] || { echo "FAIL [ast() prints the parse tree]: ${out:0:400}"; fail=1; }
out="$("$embr" pkg 2>&1)"; rc=$?
[[ $rc -eq 1 && "$out" == *"usage: embr pkg"* ]] || { echo "FAIL [embr pkg with no command shows its usage]: rc=$rc ${out:0:200}"; fail=1; }
mkdir -p "$work/pk"
embr_abs="$(cd "$(dirname "$embr")" && pwd)/$(basename "$embr")"
out="$(cd "$work/pk" && "$embr_abs" pkg list 2>&1)"; rc=$?
[[ $rc -eq 0 && "$out" == *"no packages installed"* ]] || { echo "FAIL [embr pkg list in an empty project]: rc=$rc ${out:0:200}"; fail=1; }
out="$("$embr" --backend=nosuch "$work/ast.embr" 2>&1)"; rc=$?
[[ $rc -eq 2 && "$out" == *"unknown backend 'nosuch' (this build has: "* ]] || { echo "FAIL [an unknown backend names the ones there are]: rc=$rc ${out:0:200}"; fail=1; }
printf 'print(1)\n' > "$work/ok.embr"
out="$("$embr" compile "$work/ok.embr" -o "$work/ok.embr/inside-a-file.vmbr" 2>&1)"; rc=$?
[[ $rc -eq 1 && "$out" == *"cannot write file"* ]] || { echo "FAIL [embr compile to a path it can't write]: rc=$rc ${out:0:200}"; fail=1; }

(( fail )) && exit 1
echo "cli smoke OK"
