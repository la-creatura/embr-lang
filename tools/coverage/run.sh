#!/usr/bin/env bash
# coverage report: which lines of embr the tests run, and which they never reach.
# builds a copy of the tree with --coverage in a scratch folder, runs ctest there, and writes a summary plus an
# html page per file with the lines no test reached (tools/coverage/report.py). no lcov or other tool needed, just gcov.
#
#   tools/coverage/run.sh                    # report in /tmp/embr-coverage/report/index.html
#   OUT=dir JOBS=8 tools/coverage/run.sh
#
# good to know
#   - it builds a COPY of the tree (working-tree changes included), because a build in the repo writes bin/linux/, which every preset shares
#   - plugins stay loaded until exit (EMBR_NO_DLCLOSE=1), so their counts are written like everything else's
#   - exit status: the tests' status. the report is written either way
set -uo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
out="${OUT:-${TMPDIR:-/tmp}/embr-coverage}"
jobs="${JOBS:-4}"
mkdir -p "$out"

echo "coverage: copying the tree to $out/src"
rsync -a --delete --exclude /build --exclude /bin --exclude /.git "$root/" "$out/src/" || exit 2

echo "coverage: building"
cmake -S "$out/src" -B "$out/build" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_TOOLCHAIN_FILE=cmake/linux-gcc.cmake \
      -DCMAKE_CXX_FLAGS="--coverage" -DCMAKE_EXE_LINKER_FLAGS="--coverage" -DCMAKE_SHARED_LINKER_FLAGS="--coverage" \
      > "$out/configure.log" || { cat "$out/configure.log"; exit 2; }
cmake --build "$out/build" -j"$jobs" > "$out/build.log" 2>&1 || { tail -40 "$out/build.log"; exit 2; }

echo "coverage: running the tests"
find "$out/build" -name '*.gcda' -delete
EMBR_NO_DLCLOSE=1 ctest --test-dir "$out/build" -j"$jobs" --timeout 600 --output-on-failure > "$out/ctest.log" 2>&1
status=$?
tail -3 "$out/ctest.log"

echo "coverage: writing the report"
python3 "$here/report.py" --build "$out/build" --src "$out/src" --out "$out/report" || exit 2
echo "coverage: open $out/report/index.html"
exit $status
