#!/usr/bin/env bash
# installs into a temporary prefix and checks the installed embr works on its own: the layout, plugin and module
# lookup from another directory with no EMBR_PATH, --version, and the packaged names.
#
#   install_smoke.sh <build dir> <version>
set -u
build="$1"; version="$2"
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
fail=0
bad() { echo "FAIL [$1]"; fail=1; }

cmake --install "$build" --prefix "$work/prefix" >/dev/null 2>"$work/err" || { echo "FAIL: cmake --install: $(tail -3 "$work/err")"; exit 1; }
p="$work/prefix"
if [[ "$(uname)" == Linux ]]; then
    for f in share/embr/uninstall.sh share/embr/open.sh share/embr/integration/embr.desktop share/embr/integration/embr-mime.xml \
             share/embr/integration/icons/hicolor/scalable/apps/embr.svg share/embr/integration/icons/hicolor/256x256/apps/embr.png; do
        [[ -e "$p/$f" ]] || bad "missing $f in the install"
    done
fi
case "$(uname)" in Darwin) ext=dylib ;; *) ext=so ;; esac   # plugin file extension on this platform
for f in bin/embr include/embr/embr.h lib/embr/plugins/str.$ext lib/embr/plugins/json.$ext lib/embr/modules/testing.embr share/doc/embr/README.md; do
    [[ -e "$p/$f" ]] || bad "missing $f in the install"
done

# run from an unrelated directory, with no EMBR_PATH: plugin and module come from the prefix
mkdir -p "$work/elsewhere"; cd "$work/elsewhere" || exit 1
cat > t.embr <<'EMBR'
import "str"
import "testing.embr"
print(str_upper("ok"))
EMBR
out="$(env -u EMBR_PATH "$p/bin/embr" t.embr 2>/dev/null)"
[[ "$out" == "OK" ]] || bad "installed embr could not load a plugin and a module: '$out'"

# a symlink to the installed binary (like /usr/local/bin/embr -> prefix/bin/embr) finds them too
mkdir -p "$work/link"; ln -s "$p/bin/embr" "$work/link/embr"
out="$(env -u EMBR_PATH "$work/link/embr" t.embr 2>/dev/null)"
[[ "$out" == "OK" ]] || bad "a symlinked embr could not load a plugin and a module: '$out'"

# --sandbox only lets the host's own directories supply plugins, and the prefix counts as the host's
out="$(env -u EMBR_PATH "$p/bin/embr" --sandbox t.embr 2>/dev/null)"
[[ "$out" == "OK" ]] || bad "--sandbox could not load a pure plugin from the prefix: '$out'"

ver="$("$p/bin/embr" --version)"
[[ "$ver" == "embr $version "* ]] || bad "--version should start with 'embr $version ', got: $ver"

(( fail )) && exit 1
echo "install smoke OK"
