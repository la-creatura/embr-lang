#!/usr/bin/env bash
# makes the release files on this machine, the same ones .github/workflows/release.yml makes, without github actions.
# handy for trying a fix on another machine before tagging a release.
#
#   tools/release/pack.sh                  # linux and windows
#   tools/release/pack.sh windows          # only the windows zip
#   TEST=1 tools/release/pack.sh linux     # run ctest before packing, like the release job
#   OUT=dir JOBS=8 tools/release/pack.sh
#
# the files land in $OUT/dist (default /tmp/embr-release/dist): the tarball or zip, its .sha256 and embr.h.
# on windows install the zip with: .\install.ps1 -From .\embr-<version>-windows-x86_64.zip
#
# good to know
#   - it builds a COPY of the tree (working-tree changes included), because a build in the repo writes bin/linux/, which every preset shares
#   - the copy and its build folders are kept, so a second run only rebuilds what changed
#   - windows is a mingw-w64 cross build. TEST=1 runs its tests under wine, where docs_examples is known to fail
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
out="${OUT:-${TMPDIR:-/tmp}/embr-release}"
jobs="${JOBS:-4}"
targets=("$@")
[ ${#targets[@]} -gt 0 ] || targets=(linux windows)

for t in "${targets[@]}"; do
    case "$t" in
        linux|windows) ;;
        *) echo "pack: unknown target '$t' (linux or windows)"; exit 2 ;;
    esac
done

mkdir -p "$out/dist"
echo "pack: copying the tree to $out/src"
rsync -a --delete --exclude /build --exclude /bin --exclude /.git "$root/" "$out/src/"

for t in "${targets[@]}"; do
    build="$out/build-$t"
    log="$out/$t.log"
    : > "$log"
    if [ "$t" = linux ]; then
        flags=(-DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=cmake/linux-gcc.cmake)
        kind=tar.gz
    else
        command -v x86_64-w64-mingw32-g++ > /dev/null || { echo "pack: windows needs the mingw-w64 cross compiler"; exit 2; }
        flags=(-DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake)
        if command -v wine > /dev/null; then flags+=(-DCMAKE_CROSSCOMPILING_EMULATOR=wine); fi
        kind=zip
    fi

    echo "pack: $t: building (log in $log)"
    cmake -S "$out/src" -B "$build" "${flags[@]}" -DCMAKE_CXX_FLAGS=-Werror >> "$log" 2>&1 || { tail -40 "$log"; exit 1; }
    cmake --build "$build" -j"$jobs" >> "$log" 2>&1 || { tail -40 "$log"; exit 1; }

    # the single header is the same for every platform, the release takes the linux build's copy
    [ "$t" = linux ] && cp "$build/generated/embr/embr.h" "$out/dist/embr.h"

    if [ "${TEST:-0}" = 1 ]; then
        echo "pack: $t: running the tests"
        [ "$t" = windows ] && wineboot --init > /dev/null 2>&1 || true
        ctest --test-dir "$build" -j"$jobs" --output-on-failure --timeout 120 >> "$log" 2>&1 || { tail -40 "$log"; exit 1; }
    fi

    echo "pack: $t: packing"
    rm -f "$build"/embr-*."$kind"
    cmake --build "$build" --target package >> "$log" 2>&1 || { tail -40 "$log"; exit 1; }
    for f in "$build"/embr-*."$kind"; do
        name="$(basename "$f")"
        cp "$f" "$out/dist/$name"
        (cd "$out/dist" && sha256sum "$name" > "$name.sha256")
        echo "pack: $out/dist/$name"
    done
done
