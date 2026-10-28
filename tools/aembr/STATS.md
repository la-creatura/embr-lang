# measuring aembr output

## the counter build

`aembr build --stats FILE`, `aembr run --stats FILE` and `aembr plugin --stats FILE` link `libaembr_rt_stats.a`, the runtime with counters. when the program ends it prints what it did to stderr if `AEMBR_STATS` is set
(`run --stats` sets it). a normal build has no counters: `RT_STAT` in `rt/rt.h` is nothing there.

counters: `calls` (compiled function calls), `chk` (error flag checks after a call or a native), `param_retain` (the retain a parameter that can change gets on entry), `retain_*` / `release_*` / `free_*`
and `alloc_*` per kind (str, arr, map, any, closure, cell), `alloc_bytes`, `str_concat`, `str_append_inplace` / `_copy`, `arr_get` / `set` / `push` / `grow` / `cow` (copy because the array was shared),
`map_put` / `find` / `grow` / `sort`, `any_op` / `any_conv`, `sort_call`, and `in_node` / `out_node` (values read from or written to the host by a plugin).

to read one operation, run the script with its loop at 0 and at 10 and divide the difference by 10. counts do not depend on the machine, so `tests/aembr_stats_smoke.sh` keeps budgets on a few of them.

for time, use a release `embr` (a debug host hides the cost of a plugin), `taskset -c 6`, best of several, and `perf stat -e instructions:u` for compiled programs. the shared `bin/linux` makes a release
build awkward: copy the tree somewhere (`rsync -a --exclude build --exclude bin --exclude .git ./ /tmp/x/`) and build `linux-release` there.

## what the generated code costs

- the test of `a[i]`, `a[i] = x`, `/`, `%` and a shift count is written in the operation with the jump to the handler, so one branch does the work of the test and the flag check.
- a parameter that is only read takes no count of its own (`markBorrowedParams`, `ir.cpp`): it is never assigned, changed in place, given as `&name`, captured, or changed by C text, and every call gives something that stays alive
  and unchanged during the call (a temporary, or a local that is not captured, not in-out and not in the same call as `&name`; never a global). a function used as a value or written inside another is left alone.
- `rt_ctx_init` reads `EMBR_STACK_KB` once.
- instructions of the compiled benchmarks (`tools/bench`, `--infer`): nbody 97.4M, sieve 77.4M, array 25.1M, fib 21.7M, and with `--fatal-overflow` fib 6.2M.
- `--fatal-overflow` removes the error check after a call whose only way to fail is the stack running out. without it an overflow has to return through every frame as an error. with it the program stops on the spot, nothing can
  catch it and there is no trace, so it is a choice of the person who builds, not a default. it is for programs; a plugin refuses it. it also passes `-fno-optimize-sibling-calls`, since without the checks the C compiler
  could turn an endless recursion into an endless loop.

## the plugin boundary

a value that is not a flat int, float or str is copied at the boundary. an `any` or a nested container is read into the compiled form (a box for every node) and written back the same way, which is most of what a call costs
when the value is big. the `host` type (`value: host`, `-> host`, see DESIGN.md) reads and writes the host's own value where it is. the json and csv ports use it.

release `embr`, one core, milliseconds:

| | C plugin | aembr port |
|---|---:|---:|
| json: parse 1.5 MB | 42 | 52 |
| json: stringify 1.5 MB | 21 | 31 |
| json: stringify indent 2 | 25 | 43 |
| json: valid 1.5 MB | 7.0 | 7.6 |
| json: 50k small parse / stringify | 30 / 23 | 27 / 28 |
| csv: parse 1 MB | 14.4 | 11.4 |
| csv: parse_dicts 1 MB | 28.5 | 31.7 |
| csv: stringify 20000 rows | 4.1 | 3.6 |
| encoding: 1 MB hex, base64, sha256, crc32 | 0.6 to 3.4 | 0.5 to 3.5 |
| encoding: 200k small hex_encode / crc32 | 14.7 / 11.8 | 20.9 / 17.8 |
| path: 100k basename / dirname / normalize / relative | 8.0 / 11.6 / 9.7 / 12.9 | 14.8 / 25 / 24 / 38.9 |

what is left of the gap, measured by running the same `apath` code three ways (100k calls, ms): as the C plugin, as the plugin through the host, and as a program with no host at all.

| | C plugin | apath through the host | apath in a program |
|---|---:|---:|---:|
| basename | 8.0 | 15.1 | 4.4 |
| dirname | 11.6 | 24.9 | 16.7 |
| normalize | 9.7 | 23.9 | 16.5 |
| join (3 parts) | 14.7 | 35.6 | 20.6 |
| relative | 12.9 | 38.9 | 25.3 |

two costs. the call itself: about 60 ns to get into any plugin, which the C plugin pays too, and about 50 ns more for a compiled one (the string argument is copied into a counted string and the result is copied out). copying the argument onto the stack instead of the heap saves only about 2 ms in 15 (tried, not kept). the bigger cost is inside: `dirname`, `normalize`, `join` and `relative` build strings out of slices. `s = s + slice(t, a, b)` now appends straight from `t` (`rt_str_append_slice`, no string made for the slice), and a string that starts an append chain gets room to grow, which took dirname from 27.5 to 16.7 ms in a program. `relative` used to make an array of small strings for the elements; it now keeps start and end positions like `normalize` (54.8 to 38.9 ms), compared with the C plugin on 4000 generated path pairs. a first array of ints grows to 8 slots at once.

## tried and not kept

- free lists for the boxes (any, array and map headers): slower (json parse 138 to 171 ms), the tree is spread out and the C library already caches small blocks.
- `GLIBC_TUNABLES` for malloc: mixed, json stringify got slower.
- a box-free `any` for small numbers (tagged pointers): would remove about half of the boxes of a json parse that goes through an `any`, but every use of `EmbrAny*` changes. not worth it while `host` covers the plugin boundary.
