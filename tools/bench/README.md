# embr benchmarks

```bash
# use a Release build for real numbers (a Debug build measures -O0 code):
cmake --preset linux-release
cmake --build build/linux-release --target embr embrlib_plugin embrmath_plugin vec_plugin json_plugin str_plugin -j"$(nproc)"
tools/bench/run.sh                 # best of RUNS=3, all benchmarks
tools/bench/run.sh fib closure     # only these
```

`tools/bench/check.sh` is the regression guard CI runs. it needs a second `embr` built from an older commit
(`BASE=path/to/old/embr`), runs both on the same machine and fails if a benchmark takes more than 35% longer than on the
base (`TOL=0.5` to loosen). comparing two builds on one runner is what keeps a slow or noisy machine from failing it. CI
builds the pull request's base commit in a second checkout for this. locally, copy the old `embr` and its `plugins/`
folder somewhere before you rebuild, since `bin/linux/` is shared by every build directory.

the runner refuses to report a timing if the two binaries print different output, and marks a benchmark the base build
can't run at all as `new`.

this machine is a hybrid cpu: pin to a performance core (`taskset -c 6`) and trust instruction counts
(`perf stat -e instructions:u`) more than seconds, see code layout below.

## the benchmarks

Release, gcc 13, best of 7, pinned to one performance core.

| benchmark | what it does | time |
|---|---|---|
| array | 200k push, 1M reads, 600k writes, 200k append | 0.079s |
| calls | 600k small script calls | 0.055s |
| closure | 500k calls of a closure that changes a captured counter | 0.028s |
| closure_deopt | a plain loop with one unused closure made first | 0.053s |
| cow | 20k calls with a 20k array argument, 60k `for` steps | 0.008s |
| depth | 100k `len` calls, 50 frames deep | 0.005s |
| fib | fib(27) | 0.021s |
| jsonrt | 20k records out to JSON text and back | 0.053s |
| loop | 3M iterations of `i = i + 1` | 0.054s |
| mandelbrot | a float loop with function calls | 0.048s |
| map | 100k string-key inserts, lookups and overwrites | 0.061s |
| nbody | five bodies, 40k steps, floats on array elements | 0.140s |
| overload_dispatch | 600k calls of math overload groups, member picked while running | 0.054s |
| overload_script | 600k calls of script overload groups, a third picked while running | 0.051s |
| overload_static | 1.5M calls of math overload groups, member picked before the run | 0.033s |
| sieve | a 2M entry array, nested loops | 0.216s |
| sort | 200k ints through the library sort | 0.032s |
| strings | 100k `s = s + "ab"` | 0.004s |
| vec | 200k push and 600k set/get on a `vec` | 0.065s |
| wordfreq | join, split and a map of counts over 200k words | 0.060s |

## where the hot loop gives up (`EMBR_STATS=1`)

`EMBR_STATS=1 embr x.embr` prints, when the program ends, how often the hot loop (`fastRun`) stopped at each kind of instruction
and left it to the slower path (`execOne`). that is the list of what the hot loop should learn next. what is left in the
benchmarks: `BUILD_ARRAY` / `BUILD_MAP`, `push(&a, x)` (`CALL`, `REF_PATH`), a fused add-and-store into a local that is not
defined yet (`F_ARITH_ST` in nbody), and calls of a native that takes a plain argument vector.

## other languages (`tools/bench/cross.sh`)

the same programs in embr and in other scripting languages, so a number means something. it runs every language it finds
(`lua`, `luajit`, `qjs`, `python3`, `node`) and skips the rest, takes each language's start-up time off, and stops if a
language prints a different answer than embr. the programs are in `tools/bench/cross/`, one file per language.
interpreters are compared with interpreters (`node --jitless`, `luajit -joff`), and the JIT columns are there to show the
distance to a JIT, not as a target.

measured 2026-10-07: Release, gcc 13, best of 5, seconds. Lua 5.4.6, LuaJIT 2.1 (`-joff` is its interpreter), QuickJS
2021-03-27, Python 3.12.3, node 18.19.1 (`--jitless` is its interpreter). the first six columns are interpreters, the last
two are JITs. embr has got faster since (see the table above); the other columns have not been measured again.

| benchmark | embr | lua 5.4 | luajit -joff | quickjs | python | node --jitless | luajit jit | node jit |
|---|---|---|---|---|---|---|---|---|
| fib(27) | 0.022 | 0.010 | 0.008 | 0.014 | 0.015 | 0.021 | 0.002 | 0.009 |
| loop (3M) | 0.061 | 0.033 | 0.020 | 0.069 | 0.209 | 0.044 | 0.010 | 0.011 |
| calls (600k) | 0.060 | 0.027 | 0.013 | 0.051 | 0.076 | 0.037 | 0.000 | 0.011 |
| closure (500k) | 0.039 | 0.011 | 0.005 | 0.019 | 0.034 | 0.019 | 0.000 | 0.008 |
| strings (100k concat) | 0.005 | 0.769 | 0.808 | 0.487 | 0.495 | 0.012 | 0.675 | 0.013 |
| map (100k) | 0.069 | 0.068 | 0.033 | 0.145 | 0.060 | 0.068 | 0.031 | 0.064 |
| array (200k) | 0.082 | 0.025 | 0.017 | 0.075 | 0.157 | 0.060 | 0.004 | 0.030 |
| sieve | 0.238 | 0.090 | 0.059 | 0.218 | 0.414 | 0.112 | 0.018 | 0.029 |
| nbody | 0.147 | 0.081 | 0.057 | 0.135 | 0.133 | 0.172 | 0.007 | 0.014 |
| mandelbrot | 0.051 | 0.019 | 0.012 | 0.053 | 0.044 | 0.044 | 0.003 | 0.012 |
| wordfreq | 0.069 | 0.025 | wrong | 0.095 | 0.065 | 0.095 | wrong | 0.061 |
| sort | 0.030 | 0.059 | wrong | 0.120 | 0.069 | 0.135 | wrong | 0.101 |
| jsonrt | 0.051 | - | - | 0.105 | 0.050 | 0.046 | - | 0.047 |

"wrong" is luajit: its numbers are doubles, so the 31-bit random number generator in `wordfreq` and `sort` loses digits and
it prints different answers (the harness refuses to time it). a `-` is a language without a JSON library in its standard
install.

what it says: the library calls (`sort`, `jsonrt`, string building) are as fast as or faster than the others, because they run
native code. loop-bound programs are about 2 times behind lua, level with QuickJS, and ahead of CPython on `loop` and `sieve`.
calls are 2 to 3.5 times behind lua. `strings` is a trick benchmark: the others copy the whole string on every `s = s + x` and
embr appends in place (in Lua `table.concat` and in CPython a function-local `+=` avoid the copy too).

## how embr is made fast

### values
* a `Value` is 16 bytes: an 8 byte payload, a nesting depth and a one byte type. a string, array, map, function or pointer is a
  counted box. copying or destroying a number is a plain copy, with no type dispatch (`core/value.h`).
* arrays, maps and strings are shared when copied and copied again only on a write another holder could see, so passing an
  array to a function or `for x in arr` is O(1).
* an `Instruction` is 16 bytes: number constants and source lines live in the chunk (`nums`, `ints`, `lines`).

### the dispatch loop
* `VmRunner::fastRun` runs the common instructions on plain pointers that stay in registers: numbers, compares, jumps, locals
  and globals, captured variables, the fused ops, array and map reads and writes, string `+` and `s = s + x`, `for` loop heads,
  `push` appends, script calls and returns (typed ones too) and typed or span natives. at anything it does not expect it stops
  with nothing done and `execOne` runs that instruction. `EMBR_NO_FAST=1` turns it off, and the fuzzer compares both ways.
* the work of a rarer case lives in a small out-of-line helper (`frUpval`, `frAppend`, `frMapGet`, `frMapSet`,
  `frDefineLocal`, `argsFitTyped`) so the common cases keep their registers.
* `execOne` is inlined into the two loops that run whole programs; `step()` and `runLimited()` share one out-of-line copy.
* small things on every instruction are forced inline (`Value::asInt()`, the `Value` move and destructor, the value stack);
  their error paths are out of line. the source line is only looked up for an error.
* release builds use `-falign-labels=16`, which keeps the loop's cases on 16 byte lines (see code layout).

### fused instructions
* when a program starts, short runs of instructions get one fused instruction (`quickenChunk` in `vm.h`):
  `LOAD n; PUSH_INT 2; LT; JUMP_IF_FALSE` is one op, and so are `i = i + 1`, `t = t + i % 7` and the same on globals. a fused
  op does the job when the values are numbers and otherwise runs the instructions it replaced, so nothing is trusted from the
  type checker. `EMBR_NO_FUSE=1` turns it off, and the fuzzer compares both ways.

### calls
* a call builds its frame where it will live and reuses the buffers of earlier frames (`FrameStack`, `FlatVec`). a frame points
  at its chunk with a plain pointer, and the chunk's and frame's hot fields are copied in one go (`CompiledChunk::Fast`,
  `CallFrame::Fast`).
* the gc asks each `VmRunner` for its roots when it runs (`Interpreter::GcFrameSource`), so a call pushes no gc entry.
* a callee is pushed as a mark, not as a counted copy (`kCallee`), so calling a function costs no reference count.
* a return whose result is the only value above the frame leaves it where it is.
* `name(params) = expr` and `const fn` are written out at their calls (see the wiki's syntax page).

### variables
* a variable with a slot is read and written straight in the frame's slot array, and its type is kept per slot.
* globals are remembered per name and chunk (`CompiledChunk::GlobalCache`), valid while `Interpreter::scopeEpoch()` is unchanged.
* the compiler works out which names can be a function's own locals and which belong to an enclosing function
  (`Compiler::resolveNames`). a captured variable lives in a cell (`Cell`), so it is an index, not a lookup by name.
* the first store into a local is done in the hot loop when the function captured nothing and no global has that name.
* a block that cannot make a name of its own gets no `SCOPE_PUSH` / `SCOPE_POP` (`Compiler::blockNeedsScope`;
  `Compiler::Options::elideScopes`, the fuzzer runs both ways).

### containers and strings
* `arr[i]`, `m[k]`, `arr[i] = x`, `m[k] = x` and `arr = push(arr, x)` change a plain variable where it lives
  (`INDEX_GET_VAR`, `INDEX_SET_VAR`, `APPEND_ARR`), so building an n element array or map is O(n). a key or value that may run
  script code makes the op copy the variable first (see `isPureBuiltin` in `core/ast.h`).
* `s = s + x` grows the string in place (`APPEND_VAR`), and so does `+` on a string nothing else holds.
* `mapSet` hashes a key once.

### natives
* a native bound with `bindTyped` (C++ `double` / `int64_t` / `std::string` parameters) is called straight on the stack values,
  with no `std::function`, signature check or reference count (`EMBR_NO_TYPED=1` turns it off). `bindSpan` natives take a view
  of the stack.
* a native's signature is summarised once when it is bound (`Callable::SigInfo`), so a call that plainly fits is checked by one
  short loop.
* plugin helpers take `const char*` names, so a call builds no `std::string` from a literal.

### overload groups
* a call whose argument types are known before the run calls the member directly.
* the runtime dispatcher keeps each group's members until a name is added (`overloadGroupCached`), builds error text only for an
  error, and puts the chosen member in place of the dispatcher so the VM makes the call itself (`pickOverload`).

### compile time
* a `const`, and any expression of constants, is one literal. a `const` may call pure natives (`setPure`).
* the compiled-script cache (`--cache-dir`, `.vmbr`) skips the compile: a 28,000 line script takes 19 ms instead of 53 ms. a small
  script gains nothing.

## what did not help

* link-time optimization (`-flto`): the whole VM is already one translation unit.
* a profile-guided build (`-fprofile-use`): `loop` and `calls` 20 to 25% slower, `vec` 15% faster.
* a whole numeric expression as one instruction: as many machine instructions per step as the separate fused ops, and short
  expressions got 20% slower.
* every fused op on bare values (`NumBits`): 6% fewer instructions but `loop` and `sieve` 20% slower (front-end bound, the
  layout problem below).
* keeping the loop's state in a struct to free registers, pooling the native argument vector, skipping `checkNativeSig`,
  a lazily made `localTypes` map: no measurable change.
* computed goto: branch misses are already tiny (nbody: 1.7M in 5.8G instructions).
* plain (non-atomic) reference counts would be worth up to 20% on `map` and 4 to 8% on `wordfreq`, `vec` and `jsonrt`, but are
  not safe with threads (see the card on non-atomic reference counts).

## code layout

the hot loop is one huge function, and where each case lands decides a lot. the same instructions run 10 to 20% faster or
slower after an edit somewhere else in it (`mandelbrot` and `loop` have moved that much from unrelated changes), and code
added inline to the loop costs the other cases registers (that is why rarer cases are out-of-line helpers). `-falign-labels=16`
removed most of the swing but not all of it. so compare a change against a build of the same tree, look at instruction counts,
and take a gain under about 5% on an unrelated benchmark as noise.
