#!/bin/sh
# open.sh
# what a double click on an embr file runs (the menu entry points at the copy of this file that install.sh makes).
# it runs the file in the terminal the desktop opened for us, and waits at the end so the output can be read.
# a .pmbr shows what the package holds, no file at all opens the repl.

here=$(cd "$(dirname "$0")" && pwd)
if [ -r "$here/env" ]; then . "$here/env"; fi
embr="$here/current/bin/embr"

if [ $# -eq 0 ]; then exec "$embr"; fi

cd "$(dirname "$1")" 2>/dev/null || true
file=./$(basename "$1")
case $file in
    *.pmbr) "$embr" package "$file" ;;
    *) "$embr" "$file" ;;
esac
status=$?

printf '\n[embr exited with %s, press enter to close] ' "$status"
read -r _ || true
exit "$status"
