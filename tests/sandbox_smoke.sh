#!/usr/bin/env bash
# `--sandbox`, `--allow-plugins=` and `--no-plugins` (an import allow-list; see PluginPolicy in
# include/embr/core/registry.h) tested against the real binary, INCLUDING attempts to get around it:
# path tricks, a planted same-named library, a module that imports a denied plugin, a worker thread,
# reading a file through embrlib. a test "passes" only if the denied thing is really not reachable.
#
#   sandbox_smoke.sh <embr binary> [--backend=NAME]
set -u
embr="$1"; shift
flags=("$@")
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
fail=0

run() {   # extra embr flags (space separated, may be empty), source  -> sets $out $rc
    local extra="$1" src="$2"
    printf '%s\n' "$src" > "$work/t.embr"
    # shellcheck disable=SC2086
    out="$(cd "$work" && "$embr" ${flags[@]+"${flags[@]}"} $extra "$work/t.embr" 2>&1)"; rc=$?
}
expect() {   # name, substring that must appear in the output
    [[ "$out" == *"$2"* ]] || { echo "FAIL [$1]: expected '$2' in: ${out:0:300}"; fail=1; }
}
refuse() {   # name, substring that must NOT appear
    [[ "$out" != *"$2"* ]] || { echo "FAIL [$1]: '$2' leaked: ${out:0:300}"; fail=1; }
}
nocrash() { (( rc < 128 )) || { echo "FAIL [$1]: crashed rc=$rc: ${out:0:200}"; fail=1; }; }

# ---- --sandbox: allowed plugins work, dangerous ones are refused with a catchable error
run "--sandbox" 'import "embrlib"
import "json"
import "str"
print(json_stringify([1, 2]))
print(str_upper("ok"))'
expect "sandbox allows pure plugins" "[1,2]"; expect "sandbox allows str" "OK"

for deny in os fs http ffi parallel async; do
    run "--sandbox" "try
  import \"$deny\"
  print(\"LOADED-$deny\")
catch e
  print(e[\"message\"])
end"
    expect "sandbox denies $deny" "is not allowed in this sandbox"; refuse "sandbox denies $deny" "LOADED-$deny"
done

# ---- path tricks: only bare names are accepted in a restricted sandbox
for trick in './json' '../plugins/json' '/tmp/json' 'plugins/json' 'json.so' '.'; do
    run "--sandbox" "try
  import \"$trick\"
  print(\"LOADED\")
catch e
  print(e[\"message\"])
end"
    expect "path trick $trick" "bare names"; refuse "path trick $trick" "LOADED"
done

# ---- a library planted next to the script (or in the cwd) must NOT be loaded for an allowed name
case "$(uname)" in Darwin) ext=dylib; plat=macos ;; *) ext=so; plat=linux ;; esac   # plugin extension and os_platform() here
cp "$(dirname "$embr")/plugins/json.$ext" "$work/plantedlib.$ext" 2>/dev/null
run "--allow-plugins=plantedlib" 'try
  import "plantedlib"
  print("LOADED-PLANTED")
catch e
  print("not loaded")
end'
expect "planted library" "not loaded"; refuse "planted library" "LOADED-PLANTED"
# ...but the same planted file IS found when unrestricted (proves the test itself can see it)
run "" 'import "plantedlib"
print("LOADED-PLANTED")'
expect "planted library is visible unrestricted (control)" "LOADED-PLANTED"

# ---- file access through embrlib is off in --sandbox, on with only --allow-plugins
printf 'secret' > "$work/secret.txt"
run "--sandbox" 'import "embrlib"
try
  print(load_file("'"$work"'/secret.txt"))
catch e
  print(e["message"])
end
try
  write_file("'"$work"'/pwned.txt", "x")
  print("WROTE")
catch e
  print(e["message"])
end'
expect "sandbox blocks load_file" "file access is disabled"; refuse "sandbox blocks load_file" "secret"; refuse "sandbox blocks write_file" "WROTE"
[[ ! -e "$work/pwned.txt" ]] || { echo "FAIL: write_file created a file in the sandbox"; fail=1; }
run "--allow-plugins=embrlib" 'import "embrlib"
print(load_file("'"$work"'/secret.txt"))'
expect "allow-plugins keeps embrlib file natives" "secret"

# ---- --allow-plugins is an exact list; --no-plugins allows none
run "--allow-plugins=str" 'import "str"
print(str_upper("a"))
try
  import "json"
  print("LOADED-JSON")
catch e
  print("json denied")
end'
expect "allow-plugins allows listed" "A"; expect "allow-plugins denies unlisted" "json denied"; refuse "allow-plugins" "LOADED-JSON"
run "--no-plugins" 'print("still runs")
try
  import "embrlib"
  print("LOADED")
catch e
  print("embrlib denied")
end'
expect "no-plugins: core still works" "still runs"; expect "no-plugins denies embrlib" "embrlib denied"; refuse "no-plugins" "LOADED"

# ---- a script module cannot smuggle in a denied plugin
printf 'import "os"\nmod_value = 1\n' > "$work/evilmod.embr"
run "--sandbox" 'try
  import "evilmod.embr"
  print("MODULE-LOADED")
catch e
  print(e["message"])
end'
expect "module importing os" "is not allowed"; refuse "module importing os" "MODULE-LOADED"
run "--sandbox" 'import "embrlib"
try
  load_module("evilmod")
  print("MODULE-LOADED")
catch e
  print(e["message"])
end'
expect "load_module importing os" "is not allowed"; refuse "load_module importing os" "MODULE-LOADED"

# ---- workers inherit the policy (only reachable if the user explicitly allows parallel/async)
printf 'import "os"\nfn go(x)\n  return os_getenv("HOME")\nend\n' > "$work/worker.embr"
run "--allow-plugins=embrlib,parallel,table" 'import "embrlib"
import "parallel"
inc = channel()
outc = channel()
w = worker_spawn("worker.embr", "go", inc, outc)
channel_send(inc, 1)
channel_close(inc)
r = channel_recv(outc)
print(r["ok"])
print(r["error"]["message"])
worker_join(w)'
expect "worker inherits policy" "is not allowed in this sandbox"; nocrash "worker policy"
printf 'import "os"\nfn go(x)\n  return os_getenv("HOME")\nend\n' > "$work/aworker.embr"
run "--allow-plugins=embrlib,async,table" 'import "embrlib"
import "async"
p = async_spawn("aworker.embr", "go", 1)
r = async_await(p)
print(r["ok"])
print(r["error"]["message"])'
expect "async worker inherits policy" "is not allowed in this sandbox"; nocrash "async worker policy"

# ---- C plugins (include/embr_plugin.h): a wrong ABI number is refused, and so is a capability the sandbox turned off
run "" 'try
  import "cp_badabi"
  print("LOADED-badabi")
catch e
  print(e["message"])
end'
expect "C plugin with another ABI" "plugin ABI"; refuse "C plugin with another ABI" "LOADED-badabi"
run "" 'import "cp_needs_fs"
print(cp_ok())'
expect "C plugin loads when files are allowed" "1"
run "--no-plugins" 'print(1)'   # make sure the flag exists before relying on it below
run "--sandbox --allow-plugins=cp_needs_fs" 'try
  import "cp_needs_fs"
  print("LOADED-needsfs")
catch e
  print(e["message"])
end'
expect "C plugin needing files, in a sandbox" "it needs filesystem"; refuse "C plugin needing files, in a sandbox" "LOADED-needsfs"

# ---- capabilities decide, not names: a plugin is refused when it needs one the host denies, before any of its code runs
# --sandbox denies all of them. the plugins that need none still load, the others are refused whatever their name
allowed="embrlib str unicode json csv encoding regex embrmath table vec time path random embrmeta embrtypes gc coroutine"
refused="os fs http ffi parallel async"
for ok in $allowed; do
    run "--sandbox" "import \"$ok\"
print(\"LOADED-$ok\")"
    expect "sandbox allows $ok (no capability needed)" "LOADED-$ok"
done
for no in $refused; do
    run "--sandbox" "try
  import \"$no\"
  print(\"LOADED-$no\")
catch e
  print(e[\"message\"])
end"
    expect "sandbox refuses $no" "is not allowed in this sandbox: it needs"; refuse "sandbox refuses $no" "LOADED-$no"
done
# --deny-caps denies just some: network keeps files, process keeps the network
run "--deny-caps=network" 'import "fs"
import "os"
print("FS-OS-LOADED")
try
  import "http"
  print("LOADED-http")
catch e
  print(e["message"])
end'
expect "deny network keeps fs and os" "FS-OS-LOADED"; expect "deny network refuses http" "it needs network"; refuse "deny network refuses http" "LOADED-http"
run "--deny-caps=process,env" 'import "http"
print("HTTP-LOADED")
try
  import "os"
  print("LOADED-os")
catch e
  print(e["message"])
end'
expect "deny process keeps http" "HTTP-LOADED"; expect "deny process refuses os" "it needs process, env"; refuse "deny process refuses os" "LOADED-os"
run "--deny-caps=bogus" 'print(1)'
expect "unknown capability name" "'bogus' is not a capability"; (( rc == 2 )) || { echo "FAIL [unknown capability name]: exit $rc, want 2"; fail=1; }
# embrlib loads under every denial and turns its file functions off when the filesystem is denied
run "--deny-caps=filesystem" 'import "embrlib"
try
  load_file("t.embr")
  print("READ-A-FILE")
catch e
  print(e["message"])
end'
expect "deny filesystem switches embrlib's files off" "file access is disabled"; refuse "deny filesystem switches embrlib's files off" "READ-A-FILE"

# a C++ plugin that needs the network: refused BEFORE embr_register runs, loaded (and run) when allowed
run "--deny-caps=network" 'try
  import "cp_caps"
  print("LOADED-caps")
catch e
  print(e["message"])
end'
expect "network plugin refused" "plugin \"cp_caps\" is not allowed in this sandbox: it needs network"; refuse "network plugin refused" "REGISTER-RAN"; refuse "network plugin refused" "LOADED-caps"
run "" 'import "cp_caps"
print(cp_caps_ok())'
expect "network plugin loads when allowed" "REGISTER-RAN"; expect "network plugin loads when allowed" "1"
# a C++ plugin with no description cannot be checked, so a host that denies anything refuses it. without a denial it loads
run "" 'import "cp_undeclared"
print(cp_undeclared_ok())'
expect "undeclared C++ plugin loads by default" "1"
run "--deny-caps=network" 'try
  import "cp_undeclared"
  print("LOADED-undeclared")
catch e
  print(e["message"])
end'
expect "undeclared C++ plugin refused" "does not declare its capabilities"; refuse "undeclared C++ plugin refused" "LOADED-undeclared"
# a C++ plugin built with another toolchain: a message, and nothing of it runs
run "" 'try
  import "cp_badbuild"
  print("LOADED-badbuild")
catch e
  print(e["message"])
end'
expect "plugin built with another toolchain" "different compiler, standard library or embr C++ interface"; refuse "plugin built with another toolchain" "REGISTER-RAN"; refuse "plugin built with another toolchain" "LOADED-badbuild"

# ---- script modules: __module__ and the capabilities of their imports. a module that needs a denied capability is refused
# before the first line of it runs. the real boundary stays the plugin import (a computed import is still caught there)
printf '__module__ = {"version": "1.0.0", "description": "declares what it needs", "capabilities": ["filesystem", "network"]}\nimport "http"\nprint("RAN-okmod")\nfn okf() return 1 end\n' > "$work/okmod.embr"
printf '__module__ = {"capabilities": ["network"]}\nimport "http"\nprint("RAN-badmod")\n' > "$work/badmod.embr"
printf 'import "fs"\nprint("RAN-undeclared")\n' > "$work/undeclared.embr"
printf 'print("RAN-pure")\nfn purefn() return 7 end\n' > "$work/pure.embr"
printf 'which = "str"\nimport which\nprint("RAN-dyn")\n' > "$work/dyn.embr"
printf 'which = "os"\nimport which\nprint("RAN-dynos")\n' > "$work/dynos.embr"
printf 'import "okmod.embr"\nprint("RAN-outer")\n' > "$work/outer.embr"
printf '__module__ = {"capabilities": ["network"], "bogus": "x"}\nprint("RAN-badkey")\n' > "$work/badkey.embr"
printf '__module__ = {"version": 1 + 1}\nprint("RAN-nonliteral")\n' > "$work/nonliteral.embr"
printf 'fn f()\n  __module__ = {"version": "1"}\nend\nprint("RAN-nested")\n' > "$work/nesteddecl.embr"

run "" 'import "embrlib"
import "okmod.embr"
import "pure.embr"
print(okf(), purefn())
print(has(globals(), "__module__"))'
expect "a declared module runs" "RAN-okmod"; expect "its functions are there" "1 7"; expect "__module__ is not given to the importer" $'\n0'
run "" 'try
  import "badmod.embr"
  print("LOADED-badmod")
catch e
  print(e["message"])
end'
expect "a declaration that leaves out an import" 'declares network but imports "http", which needs filesystem'; refuse "a declaration that leaves out an import" "RAN-badmod"
for bad in badkey nonliteral nesteddecl; do
    run "" "try
  import \"$bad.embr\"
  print(\"LOADED-$bad\")
catch e
  print(e[\"message\"])
end"
    refuse "a wrong __module__ ($bad) is refused before it runs" "RAN-$bad"; refuse "a wrong __module__ ($bad)" "LOADED-$bad"
done
run "" 'try
  import "badkey.embr"
catch e
  print(e["message"])
end'
expect "unknown key" 'unknown key "bogus" in __module__'
run "" 'try
  import "nonliteral.embr"
catch e
  print(e["message"])
end'
expect "non-literal value" 'must be a string written out in the file'
run "" 'try
  import "nesteddecl.embr"
catch e
  print(e["message"])
end'
expect "declaration not at the top level" 'must be assigned at the top level'

run "--deny-caps=network" 'try
  import "okmod.embr"
  print("LOADED-okmod")
catch e
  print(e["message"])
end'
expect "deny network refuses a module that imports http" 'module "okmod" is not allowed in this sandbox: it needs network'; refuse "deny network refuses a module that imports http" "RAN-okmod"; refuse "deny network refuses a module that imports http" "LOADED-okmod"
run "--deny-caps=network" 'import "undeclared.embr"
import "pure.embr"'
expect "an undeclared module that needs only files loads when only network is denied" "RAN-undeclared"
run "--sandbox" 'try
  import "undeclared.embr"
  print("LOADED-undeclared")
catch e
  print(e["message"])
end'
expect "sandbox refuses an undeclared module by what it imports" 'module "undeclared" is not allowed in this sandbox: it needs filesystem'; refuse "sandbox refuses an undeclared module by what it imports" "RAN-undeclared"
run "--sandbox" 'import "pure.embr"
import "dyn.embr"
print(purefn())'
expect "sandbox allows a module that needs nothing" "RAN-pure"; expect "sandbox allows a computed import of an allowed plugin" "RAN-dyn"
run "--sandbox" 'try
  import "dynos.embr"
  print("LOADED-dynos")
catch e
  print(e["message"])
end'
expect "a computed import of a denied plugin is caught at the import" 'plugin "os" is not allowed in this sandbox'; refuse "a computed import of a denied plugin is caught at the import" "RAN-dynos"
run "--deny-caps=network" 'try
  import "outer.embr"
  print("LOADED-outer")
catch e
  print(e["message"])
end'
expect "a module that imports a module that needs a denied capability" 'module "outer" is not allowed in this sandbox: it needs network'; refuse "a module that imports a module that needs a denied capability" "RAN-okmod"; refuse "a module that imports a module that needs a denied capability" "RAN-outer"
run "--deny-caps=network" 'import "embrlib"
try
  load_module("okmod.embr")
  print("LOADED-okmod")
catch e
  print(e["message"])
end'
expect "load_module is checked too" 'is not allowed in this sandbox: it needs network'; refuse "load_module is checked too" "RAN-okmod"
run "" 'import "embrlib"
m = load_module("okmod.embr")
print(has(m, "okf"), has(m, "__module__"))'
expect "load_module's map has the functions, not __module__" "1 0"

# ---- compiled modules (.vmbr) carry their description, so they are refused the same way, before they run
for m in okmod undeclared pure outer; do (cd "$work" && "$embr" compile "$m.embr" -o "$m.vmbr" >/dev/null); done
run "--deny-caps=network" 'try
  import "okmod.vmbr"
  print("LOADED-okmod")
catch e
  print(e["message"])
end'
expect "a compiled module that needs a denied capability" 'module "okmod" is not allowed in this sandbox: it needs network'; refuse "a compiled module that needs a denied capability" "RAN-okmod"
run "--sandbox" 'try
  import "undeclared.vmbr"
  print("LOADED-undeclared")
catch e
  print(e["message"])
end'
expect "a compiled module is refused by what it imports" 'module "undeclared" is not allowed in this sandbox: it needs filesystem'; refuse "a compiled module is refused by what it imports" "RAN-undeclared"
run "--sandbox" 'import "pure.vmbr"
print(purefn())'
expect "a compiled module that needs nothing runs" "RAN-pure"
run "--deny-caps=network" 'import "embrlib"
try
  load_module("okmod.vmbr")
  print("LOADED-okmod")
catch e
  print(e["message"])
end'
expect "load_module checks a compiled module too" 'module "okmod" is not allowed in this sandbox: it needs network'; refuse "load_module checks a compiled module too" "RAN-okmod"
run "--deny-caps=network" 'try
  import "outer.vmbr"
  print("LOADED-outer")
catch e
  print(e["message"])
end'
expect "a compiled module that imports a script module that needs a denied capability" 'module "outer" is not allowed in this sandbox: it needs network'; refuse "a compiled module that imports a script module that needs a denied capability" "RAN-outer"
# a compiled script is run directly, too
(cd "$work" && "$embr" compile badmod.embr -o badmod.vmbr >/dev/null 2>&1)
out="$(cd "$work" && "$embr" --sandbox okmod.vmbr 2>&1)"; rc=$?
expect "running a compiled script that declares a need the sandbox denies" 'is not allowed in this sandbox: it needs filesystem, network'; refuse "running a compiled script that declares a need the sandbox denies" "RAN-okmod"
(( rc != 0 )) || { echo "FAIL [running a compiled script that declares a need the sandbox denies]: exit 0"; fail=1; }

# ---- the script you run: refused up front when it declares a need the host denies, or declares wrongly. one that declares nothing
# still runs up to the import (it may import inside a try to see what is allowed)
out="$(cd "$work" && "$embr" --deny-caps=network okmod.embr 2>&1)"; rc=$?
expect "an entry script that declares a denied need" 'is not allowed in this sandbox: it needs network'; refuse "an entry script that declares a denied need" "RAN-okmod"
(( rc != 0 )) || { echo "FAIL [an entry script that declares a denied need]: exit 0"; fail=1; }
out="$(cd "$work" && "$embr" badmod.embr 2>&1)"; rc=$?
expect "an entry script with a wrong declaration" 'declares network but imports "http", which needs filesystem'; refuse "an entry script with a wrong declaration" "RAN-badmod"
run "--sandbox" 'print("BEFORE")
try
  import "os"
catch e
  print("CAUGHT")
end'
expect "an entry script that declares nothing runs up to the import" "BEFORE"; expect "an entry script that declares nothing can catch the refusal" "CAUGHT"

# ---- default (no flag) is unchanged: everything loads
run "" 'import "os"
import "fs"
print(os_platform())'
expect "unrestricted default" "$plat"

(( fail )) && exit 1
echo "sandbox smoke OK"
