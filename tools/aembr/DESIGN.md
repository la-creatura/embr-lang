# aembr

aembr is a separate compiler for a typed part of embr. it reads an embr file and writes C11, which the system C compiler (`cc`) builds into a native
program or a C plugin. there is no LLVM and no new dependency. `embr` (the VM) does not change and does not use aembr.

a program is either accepted and means what it means in the VM, or refused with `file:line:col: outside the compiled subset: reason`. the places where compiled
code differs on purpose are listed under "differences".

```
aembr check FILE [--plugin]                      say what cannot be compiled
aembr build FILE [-o OUT] [--emit-c] [--emit-ir] [--infer] [--stats] [--fatal-overflow] [-O0..3] [--cc=CC] [--deny-caps=LIST] [-lX -LX -IX -DX]
aembr run FILE [-- args]                         build into a temp folder, run, remove
aembr plugin FILE [-o OUT.so] [flags as build]   the functions of FILE as a C plugin that `import "name"` loads
aembr package FILE [-o OUT.pmbr] [--sign KEY]    a plugin with its source and build record, as a package
aembr info FILE | aembr verify PKG.pmbr          what made a file, and whether a package is what its source makes
```

## pipeline and files

```
source ─ lexer ─ parser ─ checker ─ lowering ─ IR ─ C text ─ cc ─ program or .so
         (include/embr/core, shared with the VM)   lower.cpp  ir.h  emit_c.cpp  build.cpp
```

| file | what it does |
|---|---|
| `front.cpp` | reads and parses the file, brings in the script modules it imports and the library, runs the VM's type checker (`core/typecheck.h`), keeps the types it found |
| `lower.cpp` | the compiler proper: names, types, coercions, templates, closures, natives. makes the IR. refuses what is outside the subset |
| `ir.h`, `ir.cpp` | the typed IR (`Fn`, `St`, `Ex`), which functions can raise (`markRaisers`), which parameters need no count (`markBorrowedParams`), `--emit-ir` |
| `emit_c.cpp` | IR to C text, and the plugin entry points |
| `natives.h` | the table of built-in natives of embrlib and the C functions in `rt/rt_nat.c` that do them |
| `std/` | the natives of `str` and `embrmath` written in aembr (`std/str.embr`, `std/embrmath.embr`), see "the library" below |
| `plugins.cpp` | reads the description of a C plugin the file imports |
| `build.cpp`, `pipeline.cpp`, `main.cpp` | the flags passed to `cc`, the steps, the command line |
| `record.cpp`, `packaging.cpp` | the build record inside every output, `package`, `info`, `verify` |
| `rt/` | the runtime library (C): `rt.h` (inline accessors and counts), `rt.c`, `rt_any.c`, `rt_nat.c`, `rt_host.c` (calling C plugins), `rt_plugin.h` (the plugin boundary) |
| `ports/` | the C plugins encoding, path, csv and json written again in aembr, checked against the C ones (`ports/README.md`) |
| `STATS.md` | the counter build and what it found |

the runtime is built once, next to the binary, in `<bin>/aembr-rt/` (`libaembr_rt.a`, `libaembr_rt_stats.a` with counters, and the headers).

## the compiled subset

types: `int` (64 bit), `float`, `str`, `arr<T>`, `map<T>` (string keys), nested containers of those, function values `fn(int, str) -> int`, `any` (a value with no fixed type:
a counted box with a tag), the error a `catch` binds, and `host` (below). every parameter and result type is written, or with `--infer` worked out from the first call and
the first `return value`. a variable keeps the type of its first value; an empty `[]` or `{}` takes its element type from the first use.

in: `local`, assignment, element stores at any depth, `if`, `while`, `for` over arrays, ranges (with a step) and strings, `break`, `continue`, `return`, `try`/`catch`, functions at the top
level and inside others, lambdas, unpacking, `const NAME`, `const fn` (one version per argument types), `const import`, all operators, `import "embrlib" | "str" | "embrmath" | <C plugin>`, and the natives
of those three plugins that the table has (including the ones that take a function: `filter reduce sort_cmp ...`).

aembr-only syntax (the VM refuses it): `fn f(a, b: int = 5)` (a default is a number or a string written out), `fn f(&n: int)` (in-out, called as `f(&x)`), `c """..."""` (C text), `c file """..."""`,
`const c import`, and the `host` type.

out: coroutines, `eval`, computed imports, `embrmeta`, `for k, v in map`, `str_format`, `items`, the random numbers, file natives, a function value inside an `any`, C++ plugins (they need the interpreter).

### differences from the VM

- `int + - *` and unary `-` wrap (two's complement; the VM turns an overflow into a float). `/` is always a float. `<<` wraps, a shift count outside 0 to 63 is an error, as in the VM.
- a program may assign a top level name from a function; it is a global and the program raises if the top level has not run it yet. in a plugin, top level names are read only to the functions.
- `math_floor`, `math_ceil`, `math_round` give an int and raise when the result does not fit; `math_min`/`max`/`clamp` of mixed int and float are refused; `sort` takes ints or floats.
- a stack budget (1 MB, `EMBR_STACK_KB`) replaces the call depth limit of 1000, and there is no step or time limit. `--fatal-overflow` (programs only): an overflow prints its error and stops at once, with no trace and
  nothing to catch; a recursive function that raises nothing else then needs no error check after its calls.
- `e["trace"]` and `print(e)` work; the source snippet of an uncaught error is not printed.

## several files

`import "util.embr"` (a literal ending in `.embr` or `.aembr`, as in the VM) is replaced, in `front.cpp` (`expandModules`), by the statements of that file, the first time the file is seen. the file is found next to the importing file, then in the `EMBR_PATH` folders, then in the current folder; its own imports are expanded first, relative to its own folder. the module runs where the import was, its names are the program's names, and a statement keeps its own line (`Front::stmtFile` says which file it came from). consequences:

- one translation unit, one checker run over the whole program, so types cross files with no rule of their own.
- a function of a module is `Fn::internal`: a plugin does not export it. `Fn` and `St::file` carry the file index (`Module::files`) so a stack trace names the file a call is written in, and a refusal names the module.
- the build record's source hash covers the file and its modules (by their names relative to the file's folder), and `aembr package` carries them under `src/` (a module outside the folder is refused).
- a function of a module assigns only the top level names of its own file; any other plain assignment makes a local (`Lowerer::topName`). this is the VM's rule too. each module is type checked on its own, with `isModule` set, before the pieces are put together.
- the capabilities of a module count for the importing file (`embr::analyzeModule` reads modules recursively).
- difference from the VM: a module's top level variables are the program's variables. in the VM the importer gets a copy when it imports, so a module function that changes its variable is not seen by the importer. a program that relies on that is not the same compiled.
- not done: an `import "x.embr"` inside a function (refused), a module name without `.embr` that is a script (a bare name is a plugin).

## errors

an error has a kind (`vm` for the runtime's own, `error` for `error("text")`, or a plugin's own) and a message, and carries its line. every function that can raise takes the error context as a hidden first
parameter and returns normally; the caller looks at `cx->err` after a call that can raise. a function that cannot raise has no context and no checks. an operation that can fail (`a[i]`, `a[i] = x`, `/`, `%`, a shift) tests and jumps to
the handler in place. a frame an error leaves adds its function name and the line of the call to the trace; `try` clears the flag and binds `e`. an uncaught error ends the program with exit code 1 after printing the message, the line and the
trace in the VM's shape.

## memory

counted, with plain counts. a heap value starts with its count (a count at or above `EMBR_IMMORTAL` is never changed: literals, constants of a plugin).

| object | layout |
|---|---|
| str | `{rc, len, cap, data[cap+1]}` |
| arr | `{rc, len, cap, data*, kind}` 8 byte elements; kind 0 int, 1 float, 2 str, 3 arr, 4 map, 5 any |
| map | `{rc, len, cap, kind, keys*, vals*}` open addressing, str keys |
| any | `{rc, tag, value}`; tag 0 int, 1 float, 2 str, 3 arr, 4 map, 6 handle |
| cell, closure, error | a cell holds a variable a closure shares; a closure holds its function and cells |

- a variable owns one count of its value. a function takes its own count of a parameter it can change, so a write inside it copies (copy on write); a parameter that is only read, whose callers all keep the value alive, takes none.
- a function returns an owned value. runtime functions borrow what they are given, retain what they store, and return a new reference.
- before a write to a container the code makes it unique: the same container when the count is 1, else a copy. a nested write goes down one level at a time, each level handing out a pointer to its child made unique in its slot.
- `s = s + x` grows a string nobody else holds in place.
- counts are not atomic. a compiled value never crosses threads (the plugin boundary copies, or hands over the host's own value), a compiled plugin has no mutable top level state, an executable has no threads.
- a cycle through a closure and its own variable is broken when the function drops the variable; other cycles leak.

## plugins

`aembr plugin x.embr` writes a shared library whose only exported symbol is `embr_plugin_info` (`include/embr_plugin.h`, ABI 1). the name is the output file's name; version, description and capabilities come from the file's
`__module__` (capabilities are declared plus inferred from the imports; a file with C text needs `native`). every top level function whose name does not start with `__` is exported, with a signature of int, float, str, `any`, `arr<T>`,
`map<T>`, nested ones, `host`, optional parameters (`EMBR_PARAM_OPTIONAL`, the default filled in when the host leaves it out) and a variadic last parameter. a function value or an in-out parameter is refused with the reason. an error
reaches the script with its own kind.

at the boundary a flat value is copied; an `any` or a nested container is read into the compiled form and written back the same way. a plugin written in aembr can also do its work in C text:

- `c """ ... """` in a function: `$name` is the C name of a variable of the function, `$$` a dollar sign, `cx` the error context (`rt_raise(cx, ...)`), everything in `rt.h`. a heap variable owns one count: give it a value with
  `rt_set_str(&$s, v)` and the like. do not `return`, `goto` or `break` out of the block. a block makes its function able to raise.
- `c file """ ... """` or `const c file import "x.h"`: C for the whole file, put before everything aembr writes.
- `-l -L -I -D` go to the C compiler and are kept in the build record.

### host values

`value: host` is the host's own value, not copied (`const EmbrValue*` in C text, read with the `embrp_*` calls); `-> host` is a result written into the host's slot by C text. in C text `$ctx` is the host handle (`rt_host_ctx`) and `$out` is
the result slot (`rt_host_out`); both are set by the entry point. `RT_HOST(cx, call)` raises when an `embrp_` call fails. the compiler is strict, since the value is only good during the call: a `host` value can be passed to another `host` parameter or
named in C text, nothing else (no variable, array, closure, global, expression or return). `host` is for plugins: a program, a function value, an optional or in-out parameter, a variable or an element refuse it; a `const fn` may take one and use `$out`
when it is called from a function that has `-> host`. the parser marks it with `TypeAnn::isHost` (it is `any` for the checker) and the VM refuses a function that uses it. `ports/ajson.embr` and `ports/acsv.embr` are written this way.

## calling other plugins

- embrlib is built in: the table in `natives.h` and `rt/rt_nat.c`, with the VM's names, messages and error kinds. a native is there only when the file imports its plugin. natives decided at compile time (`type`, `print`,
  `len` of a typed value, the loops of `filter` and `sort`) stay in the compiler. `str` and `embrmath` are the library below.

### the library (`std/*.embr`)

`import "str"` (or `"embrmath"`) brings `std/<plugin>.embr` in before the statements of the file (`front.cpp`, `spliceStd`), so a call of `str_split` is a call of an ordinary aembr function, with the VM's names, messages and error kinds. the files are
copied next to the runtime (`<bin>/aembr-rt/std/`) and their hash is part of the build record. rules:

- the compiler trusts them: C text there needs no `native` capability (not for the file, not under `--deny-caps=native`), and the functions are not exported by `aembr plugin`.
- their locals never mean a top level name of the file that imports them.
- they have no frame in a stack trace, and an error they raise (`rt_raise(cx, kind, 0, ...)`, line 0) gets the line of the call.
- `num` as a parameter type means a float that takes an int argument; a native that is a different function for ints and floats is a `const fn` (one version per type) with `_Generic` in the C text.
- PI and E stay in the compiler. what is not in a file is still the table's or refused.
- the container natives of embrlib stay in the compiler on purpose: they are its container support (an `any` argument is tried against each overload, an empty literal takes its type from its neighbours, `&name` marks the ones that change a container, some results are built from the argument's type). str and embrmath were ordinary functions, so they moved.
- a C plugin: `import "name"` reads its `embr_plugin_info` when the file is compiled and checks every call against it; a call whose result varies is refused unless the result is dropped or the plugin says `any`. the program opens the
  library when it starts and calls through the host side of `embr_plugin.h` that `rt/rt_host.c` implements over its own values. a plugin that makes a handle (other than json's true/false/null) fails. a C++ plugin cannot be called. regex, unicode, os, random and time are C plugins, so a program can use them (`tests/aembr/ok/c_plugins.embr`); `os_args()` gives the words after the program name, which `aembr run FILE -- a b` passes on.
- `--deny-caps=LIST` refuses at compile time a program that imports a plugin needing one of those capabilities (and refuses C text for `native`).

## trust and packages

every output carries a record (aembr version, kind, source name and sha256, capabilities, the C compiler's version line, the flags, the sha256 of the runtime, the platform) as text inside the file, found by scanning, so `aembr info` never loads a
library. output is deterministic (no timestamps or absolute paths), so the same input gives the same bytes. `aembr package` hands a folder (`meta.embr`, `lib/<platform>/<name>.so` (`.dylib` on macOS), `src/`, `aembr/build.json`) to the same `buildPackage` as `embr pack`
and signs with the same key tooling. `aembr verify PKG.pmbr` checks the claim (capabilities recomputed from the bundled source must be inside what the package declares) and, when aembr, the compiler, the runtime and the platform are the recorded ones,
the bytes (the library is built again and its sha256 must match). exit codes: 0 verified, 1 a mismatch, 2 could not verify.

a compiled plugin is native code with the process's rights. its capability list is a claim the host checks before any code runs, the same as for any C plugin. a bug in the compiler is a memory bug in the host process; the guard is the tests under
AddressSanitizer and the fuzz target `programs`.

## tests

| | |
|---|---|
| `tests/aembr_smoke.sh` | `ok/` programs print what the VM prints; `golden/` programs (aembr-only syntax) print their `.out` and the VM refuses them; `refuse/` files are refused with the words on their first line; `error/` ends the way the VM's errors do; the emitted C compiles without a warning and is the same twice |
| `tests/aembr/ok/std_names.embr`, `natives_*.embr` | the library: same output as the VM for every native it replaces |
| `tests/aembr_plugin_smoke.sh` | plugins: capabilities, the same file as plugin and as script module, C text, host values, refusals |
| `tests/aembr_ports_smoke.sh` | each port prints exactly what the C plugin prints, errors included |
| `tests/aembr_package_smoke.sh` | package, info, verify |
| `tests/aembr_stats_smoke.sh` | budgets from the counters: parameters retained, error checks, boundary copies |

## adding things

- a native: a row in `natives.h` and the C function in `rt/rt_nat.c`; a shared example in `tests/aembr/ok/natives_*.embr` that runs on the VM and on aembr.
- an operator or a statement: `lower.cpp` (lowering and its refusal), `ir.h` if the IR needs a new form, `emit_c.cpp`, and a test in `ok/` and one in `refuse/`.
- a change to the layout of a heap object: `rt/rt.h`, the emitter's inline accessors, and the runtime sha256 in the build record changes by itself.

## limits that are known

- a cycle that is not the closure-and-its-variable shape leaks.
- an in-out argument has to be a variable (not `&a[i]`); a `host` value cannot be optional.
- a function used as a value cannot have optional, in-out or host parameters.
- a module's top level variables are shared with the program (the VM copies them at import).
- windows and macOS are untested for this tool.
