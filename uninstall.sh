#!/bin/sh
# uninstall.sh
# takes out everything install.sh put on the system, using the manifest it wrote next to this file.
# the install keeps a copy of this script at <data>/embr/uninstall.sh.
#
# usage
#   ~/.local/share/embr/uninstall.sh
#   ~/.local/share/embr/uninstall.sh --purge      # also delete the extra folder (your own plugins and modules)
#
# good to know
#   - your own files in <data>/embr/extra are kept unless you pass --purge.
#   - shell config files only lose the block the installer added, the rest of the file is not touched.

set -eu

say() { printf 'embr: %s\n' "$*"; }
die() { printf 'embr: %s\n' "$*" >&2; exit 1; }

purge=0
for a in "$@"; do
    case $a in
        --purge) purge=1 ;;
        -h|--help) echo "usage: uninstall.sh [--purge]"; exit 0 ;;
        *) die "unknown option: $a" ;;
    esac
done

ehome=$(cd "$(dirname "$0")" && pwd)
manifest=$ehome/manifest
[ -f "$manifest" ] || die "no manifest next to this script, so there is nothing it knows how to remove. run the copy that install.sh made in <data>/embr"

data=$(sed -n 's/^data //p' "$manifest" | head -n 1)
[ -n "$data" ] || die "the manifest is damaged (no data folder in it)"

have() { command -v "$1" >/dev/null 2>&1; }

# the files and links the installer made
sed -n 's/^file //p' "$manifest" | while read -r f; do
    rm -f "$f" 2>/dev/null || say "could not remove $f"
done

# the block in each shell config file. when the file had no newline at the end the installer added one, take it out too
sed -n 's/^rc //p' "$manifest" | while read -r f; do
    [ -f "$f" ] || continue
    awk '
        /^# >>> embr >>>$/ { skip = 1; next }
        /^# <<< embr <<<$/ { skip = 0; next }
        !skip { print }
    ' "$f" > "$f.embr-tmp" && cat "$f.embr-tmp" > "$f"
    rm -f "$f.embr-tmp"
    if [ ! -s "$f" ]; then rm -f "$f"; continue; fi
    if grep -qx "rcnl $f" "$manifest" && [ -s "$f" ]; then
        head -c -1 "$f" > "$f.embr-tmp" && cat "$f.embr-tmp" > "$f"
        rm -f "$f.embr-tmp"
    fi
done

# the databases are rebuilt while their folders are still there
if have update-mime-database && [ -d "$data/mime/packages" ]; then update-mime-database "$data/mime" >/dev/null 2>&1 || true; fi
if have update-desktop-database && [ -d "$data/applications" ]; then update-desktop-database "$data/applications" >/dev/null 2>&1 || true; fi
if have gtk-update-icon-cache && [ -d "$data/icons/hicolor" ]; then gtk-update-icon-cache -q -t -f "$data/icons/hicolor" >/dev/null 2>&1 || true; fi

# the empty folders that leaves under the data folder. a mime or applications folder with nothing of anyone's left
# in it only holds what the tools above generated, so that goes too
sed -n 's/^file //p' "$manifest" | while read -r f; do
    d=$(dirname "$f")
    case $d in
        "$data"/*) while [ "$d" != "$data" ] && rmdir "$d" 2>/dev/null; do d=$(dirname "$d"); done ;;
    esac
done
if [ -d "$data/mime" ] && [ ! -d "$data/mime/packages" ]; then rm -rf "$data/mime"; fi
if [ -f "$data/applications/mimeinfo.cache" ] && [ "$(ls -A "$data/applications" | wc -l)" = 1 ]; then
    rm -f "$data/applications/mimeinfo.cache"
    rmdir "$data/applications" 2>/dev/null || true
fi
if grep -qx 'mode user' "$manifest"; then
    rmdir "$(dirname "$(sed -n 's/^file \(.*\/bin\/embr\)$/\1/p' "$manifest" | head -n 1)")" 2>/dev/null || true
fi

# the embr folder itself: the versions and the files next to them. the extra folder stays if you put something in it
for v in "$ehome"/[0-9]*; do
    [ -d "$v" ] && rm -rf "$v"
done
rm -f "$ehome/current" "$ehome/env" "$ehome/open" "$ehome/manifest"
if [ "$purge" = 1 ]; then
    rm -rf "$ehome/extra"
else
    rmdir "$ehome/extra/plugins" "$ehome/extra/modules" "$ehome/extra" 2>/dev/null || true
fi
kept=
[ -d "$ehome/extra" ] && kept=1

say "removed embr"
if [ -n "$kept" ]; then
    say "kept $ehome/extra (your own plugins and modules). delete it yourself, or run this with --purge next time"
fi
# last, because it is the running script. the shell has it open, so this is fine
rm -f "$ehome/uninstall.sh"
rmdir "$ehome" 2>/dev/null || true
