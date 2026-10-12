# embr

<img src="assets/embr-256.png" alt="embr" width="128">

embr (Embeddable/Extensible Meta-Binding Runtime) & (EMBR Makes Binding Reliable)
was created solely because i wanted a single header scripting language and the lua vm pissed me off. with this, you can include one file and be able to use it for configs, scripting and dynamic plugin loading, or download a release (a tarball on the github releases page, unpack it anywhere, `bin/embr` finds its plugins by itself) and use the cli interpreter to run scripts.

![license](https://img.shields.io/badge/license-MIT-blue.svg) ![version](https://img.shields.io/badge/version-1.0.0-green.svg) [![ci](https://github.com/la-creatura/embr-lang/actions/workflows/ci.yml/badge.svg)](https://github.com/la-creatura/embr-lang/actions/workflows/ci.yml)

## table of contents

- [features](#features)
- [using as lib](#lib)
- [building from source](#building)
- [release (cli interpreter) usage](#cli)
- [documentation](#documentation)
- [contributing](#contributing)
- [faq](#faq)
- [license](#license)

## features

- minimalistic syntax with 18 reserved keywords
- a variable has the type of its first value, checked before the script runs (`embr check`), and arrays and maps can say what they hold: `local arr<int> xs = []`
- optional function arg and return typechecking
- python-esque module system (`import` works for native plugins and for `.embr` files)
- C ABI plugin support, and a good pile of plugins already: strings, json, csv, regex, files, http(s), threads, coroutines, ...
- built-in FFI plugin for calling C libraries (via libffi)
- first class functions and closures with full upvalue capture
- number, int, string, array, map, callable, pointer data types (no bool or nil type on purpose, see the [type system](https://github.com/la-creatura/embr-lang/wiki/type-system) page)
- runs on a bytecode vm. the code is split so another backend (a jit, say) can be added next to it
- scripts can be compiled to `.vmbr` files (`embr compile`) and cached, so imports don't get recompiled
- a plain C interface (`embr_c.h`) so other languages can embed it, see [from c](#from-c-or-python-rust-go)
- editor support: a language server plus vs code and sublime grammars (`tools/editor`)
- release tarballs for linux, and a windows build cross-compiled with mingw-w64
- `--sandbox` mode to run a script that shouldn't be touching your files, processes or network, and `--deny-caps` to deny just some of those
- every plugin describes itself (version, capabilities, a doc line per function): `embr plugins`, `embr plugin NAME`, the `embrmeta` plugin and editor hover all show it
- `.pmbr` packages: one file with a module or a plugin (a build per platform), the libraries it wraps and its files, that says what it needs without being opened (`embr pack`, `embr package`)
- `embr test`, `embr check`, `embr fmt` (formatter), `embr lsp` (editor errors, go-to-definition, references, rename, hover, completion) and a small package manager (`embr pkg`)

## lib

the headers under `include/embr/` are glued into one file at build time, `embr.h`. it ends up at `build/<preset>/generated/embr/embr.h`, copy that into your project and `#include "embr.h"` is all you need to access everything. run embr code with
```cpp
embr::Interpreter interp;  // interpreter context with all the variables and stuff
embr::runSource("print(\"hewwo wowwd!\")", interp);
```
`embr::runSource` runs on the default backend, which is the vm. `embr::vm::runSource(src, interp)` is the same thing with the backend named

plugins (`import "json"` and friends) are separate `.so` files, they aren't inside `embr.h`. to use them from your own program, keep them next to your executable or point `EMBR_PATH` at them

#### from c (or python, rust, go...)

`include/embr_c.h` is a small plain C interface, built into `bin/<platform>/lib/libembr_c.a` and `libembr_c_shared.so`. nothing c++ crosses it, so anything with a C FFI can use it
```c
embr_state* e = embr_new();
embr_set_int(e, "width", 80);
if (embr_run_string(e, "area = width * 2", "config") != 0) puts(embr_last_error(e));
int64_t area; embr_get_int(e, "area", &area);   // 160
embr_free(e);
```
you can also give scripts your own functions (`embr_register`) and limit which plugins a script may import. `examples/embed/host.c` shows all of it and runs as a test. link the static one with a c++ linker (or add `-lstdc++`), it has embr's c++ code inside

#### sophisticated route

under the hood, `vm::runSource()` is just a convenience wrapper for this
```cpp
inline void runSource(const std::string& src, Interpreter& interp,
                      const std::string& filename = "<input>") {
    auto prog = compileSource(src, interp, filename);   // lex, parse, type check, compile
    VmRunner runner(interp);
    try {
        runner.run(prog);
    } catch (const EmbrError& e) {
        std::cerr << e.what() << formatTrace(e);
    }
}
```
you can stop after `compileSource` to look at the compiled program, or catch errors yourself. the [architecture](https://github.com/la-creatura/embr-lang/wiki/architecture) page explains how the pieces connect

## building
```bash
git clone --recurse-submodules https://github.com/la-creatura/embr-lang.git
cd embr-lang

# installing dev libffi package on linux
sudo apt install libffi-dev      # debian/ubuntu
sudo dnf install libffi-devel    # fedora

# optional: OpenSSL dev package, needed for https:// in the http plugin
# (without it the plugin still builds and plain http:// works; https:// raises a clear error,
#  and http_tls_available() returns 0). cmake prints a status line when it isn't found.
sudo apt install libssl-dev      # debian/ubuntu
sudo dnf install openssl-devel   # fedora

# building for linux
cmake --preset linux-release
cmake --build --preset linux-release

# debug, for working on embr
cmake --preset linux-debug
cmake --build --preset linux-debug

# install it (bin/embr, lib/embr/plugins, lib/embr/modules, include/embr/embr.h) or make the tarball
cmake --install build/linux-release --prefix ~/.local
cmake --build build/linux-release --target package    # embr-<version>-linux-<arch>.tar.gz in the build dir

# building for windows (libffi bundled in source), cross-compiled with mingw-w64 on linux
cmake --preset windows-release
cmake --build --preset windows-release

# and running its tests on linux under wine, the way ci does
cmake --preset windows-release -DCMAKE_CROSSCOMPILING_EMULATOR=wine
ctest --test-dir build/windows-release
```
#### macos
**untested** - i dont have a mac. the build files are there (`macos-*` presets, `bin/macos/`, brew keg-only libffi/openssl are looked up for you, an `environ`/executable-path fix for macOS) and ci has an experimental macos job, but nobody has run it yet ¯\\_(ツ)_/¯
```bash
brew install libffi pkg-config openssl@3
cmake --preset macos-release
cmake --build --preset macos-release
```
if it fails, the error will probably be one line in a plugin, and a pr or issue with that line would be very welcome. known gaps: the ffi tests open `libSystem.B.dylib`, `os_exec` is posix-only (fine on mac), https certificate lookup uses openssl's default paths rather than the keychain.

#### the shared bin/ folder
every preset writes `embr`, the plugins and the test binaries into the same `bin/<platform>/` folder. building one preset after another overwrites the first one's output, and neither build notices. a typical symptom: you built `linux-release` last and your debug build seems to ignore your changes.

- `./embr --version` shows which build directory made the binary and which backends it has. check it first when a run looks like the wrong build.
- the ctest test `bin_dir_stamp` fails with the fix when `bin/` came from a different build directory.
- to fix it, delete the old output and rebuild the preset you want:
```bash
rm -f bin/linux/embr bin/linux/tests/test_* bin/linux/plugins/*.so
cmake --build --preset linux-debug
```
- for a throwaway config, build in a directory outside the repo so it doesn't touch `bin/`:
```bash
cmake -S . -B /tmp/scratch -DCMAKE_BUILD_TYPE=Debug && cmake --build /tmp/scratch
```

#### tests
```bash
ctest --test-dir build/linux-debug --output-on-failure
```
- every suite runs once per backend, `<name>_<backend>` (today just `<name>_vm`). each test runs in its own folder, `bin/<platform>/test-work/<name>`, with `EMBR_PATH` set, so don't assume the working directory.
- a new `tests/*_test.cpp` needs a re-configure (`cmake --preset ...`), a plain build won't find it.
- if a test crashes with no output, the output may just be unflushed. rerun with `stdbuf -oL -eL`.
- sanitizers (asan, ubsan) run in ci. fuzzing: `tools/fuzz/run.sh`. benchmarks: `tools/bench/run.sh` (needs a release build, which overwrites `bin/`). coverage: `tools/coverage/run.sh` (which lines the tests never reach, as an html page; builds in a copy, so `bin/` is left alone).

#### debugging
- shrink the problem to a tiny script and run it with a timeout: `timeout 5 ./embr repro.embr; echo $?` (124 means it hung).
- the build is expected to be warning free (`-Werror` in ci). gcc's `-Wdangling-reference` gives false positives on helpers that return a reference, so those helpers return a pointer instead.

## cli

on linux, one line installs the latest release for you (no sudo, it goes in your home folder):
```bash
curl -fsSL https://raw.githubusercontent.com/la-creatura/embr-lang/main/install.sh | sh
```
you get an `embr` command, `EMBR_PATH` set up (it points at `~/.local/share/embr/extra`, put your own plugins and modules there), a menu entry, and icons and double-click for `.embr`, `.vmbr` and `.pmbr` files. `~/.local/share/embr/uninstall.sh` takes all of it out again. `--system` installs for everyone, `--version X.Y.Z` picks a release, `--no-modify-path` and `--no-desktop` skip those parts. the [installing](https://github.com/la-creatura/embr-lang/wiki/installing) page has the details.

or by hand, from the tarball or a build:
```bash
./embr myscript.embr
```
or
```bash
./embr
```
for repl

```bash
./embr --backend=vm myscript.embr  # pick a backend by name (`--version` lists them, today only vm, which is the default)
./embr myscript.embr -- a b      # arguments after -- are read in the script with os_args()
./embr --version                 # which build this is, and which backends it has
./embr check a.embr b.embr       # parse + type check + compile, nothing runs; errors as file:line:col: message
./embr dis a.embr                # show the vm code a script compiles to (nothing runs)
./embr --cache-dir=cache a.embr  # keep the compiled script (and the modules it imports) in cache/ as .vmbr files, reuse them while unchanged
./embr --max-steps=1000000 --timeout=5 a.embr  # stop a script that runs too long (an error the script can't catch)
./embr compile a.embr            # write a.vmbr, then ./embr a.vmbr runs it. ./embr cache clean cache removes stale cache files
./embr fmt a.embr                # format in place (--check just lists files that would change, --stdout prints)
./embr lsp                       # language server on stdin/stdout: errors as you type, go-to-definition, hover, completion
./embr test tests/               # run test_*.embr files (test_* functions, assertions in testing.embr)
./embr --sandbox untrusted.embr  # imports limited to pure plugins, no file access (see the [syntax](https://github.com/la-creatura/embr-lang/wiki/syntax) page, sandbox mode)
./embr --deny-caps=network a.embr # refuse plugins that need the network (or filesystem, process, env, threads, native)
./embr plugins                   # the installed plugins: version, kind, what capabilities they need
./embr plugin regex              # one plugin's reference: every function with its signature and doc
./embr modules                  # the installed script modules, and ./embr module NAME for one: __module__, what it imports and needs, its functions
./embr pack mylib -o mylib.pmbr   # one file for a module or plugin (+ libraries and files), see the package-format page in the wiki
./embr package mylib.pmbr        # what a package says about itself and holds, without unpacking it
./embr keygen me                 # a signing key pair; embr pack --sign me.key and embr sign sign packages, embr trust me.pub trusts a key
./embr --require-signed a.embr   # import only packages signed by a trusted key
./embr pkg install <name>        # package manager: adds it to embr.json and embr.lock, see the pkg.embr page in the wiki
./embr pkg install               # everything embr.json lists, the same versions as embr.lock
```

## documentation

all the docs live in the [wiki](https://github.com/la-creatura/embr-lang/wiki):

- [tutorial](https://github.com/la-creatura/embr-lang/wiki/tutorial) - learn embr in 15 minutes, start here if you want to write scripts
- [architecture](https://github.com/la-creatura/embr-lang/wiki/architecture) - how the pieces fit together, start here if you're new to the code
- [reference](https://github.com/la-creatura/embr-lang/wiki/home) - the language and runtime reference (grammar, types, scoping, errors, modules, backends)
- [plugin reference](https://github.com/la-creatura/embr-lang/wiki/plugin-reference) - one page per plugin/module (str, json, fs, os, path, csv, encoding, random, unicode, regex, time, table, vec, http, async, parallel, coroutine, gc, ffi, ...)
- [writing plugins](https://github.com/la-creatura/embr-lang/wiki/writing-plugins) - tutorial for writing a native plugin
- [examples/](examples/) - small runnable programs; they are run by the test suite so they stay correct

## contributing

contributions are welcome. follow these steps:

1. fork the repo
2. create your feature branch (`git checkout -b feature/pawsome-feature`)
3. commit your changes (`git commit -m 'add pawsome feature'`)
4. push to the branch (`git push origin feature/pawsome-feature`)
5. open a pull request

## faq

#### does it support XYZ

¯\\_(ツ)_/¯
only tested on linux mint 22.3 64 bit and windows 10 64 bit

#### how do i ask for a new feature

contact me on discord @alice_was_taken or like open an issue or something

#### im having XYZ issue

contact me on discord or open an issue

## license

this project is licensed under the gpl-3.0. see [LICENSE](LICENSE) for details

---

made with ❤ by [alice](https://github.com/la-creatura)
