#!/usr/bin/env bash
# runs install.sh and uninstall.sh against a tarball made from `cmake --install`, in a fake home folder.
# checks the command, EMBR_PATH, the shell config block, the menu entry, the file types and the icons, an install
# twice, a command that is not ours, --system, and that an uninstall leaves the home folder as it was.
#
#   installer_smoke.sh <build dir> <version> <source dir>
set -u
build="$1"; version="$2"; src="$3"
[[ "$(uname -s)" == Linux ]] || { echo "skipped: the installer is linux only"; exit 0; }
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
fail=0
bad() { echo "FAIL [$1]"; fail=1; }

arch="$(uname -m | tr 'A-Z' 'a-z')"
name="embr-$version-linux-$arch"
mkdir -p "$work/stage" "$work/dl"
cmake --install "$build" --prefix "$work/stage/$name" >/dev/null 2>"$work/err" || { echo "FAIL: cmake --install: $(tail -3 "$work/err")"; exit 1; }
tar -czf "$work/dl/$name.tar.gz" -C "$work/stage" "$name"
(cd "$work/dl" && sha256sum "$name.tar.gz" > "$name.tar.gz.sha256")

fresh_home() { # a new fake home with a .bashrc that has something in it
    rm -rf "$work/home"; mkdir -p "$work/home"
    printf 'alias ll="ls -l"\nexport FOO=1' > "$work/home/.bashrc"     # no newline at the end on purpose
    cp "$work/home/.bashrc" "$work/bashrc.orig"
    (cd "$work/home" && find . | sort > "$work/tree.orig")
}
tree_now() { (cd "$work/home" && find . | sort); }
inst() { # runs install.sh in the fake home. the first argument is extra options
    env -i PATH="$PATH" HOME="$work/home" SHELL=/bin/bash XDG_DATA_HOME="$work/home/.local/share" "$@"
}

d="$work/home/.local/share"
cat > "$work/t.embr" <<'EMBR'
import "str"
print(str_upper("ok"))
EMBR

# a plain install from a file
fresh_home
inst sh "$src/install.sh" --from "$work/dl/$name.tar.gz" >"$work/out" 2>&1 || { bad "install failed: $(cat "$work/out")"; exit 1; }
b="$work/home/.local/bin/embr"
[[ -L "$b" ]] || bad "no embr link in ~/.local/bin"
out="$(cd "$work" && env -u EMBR_PATH "$b" t.embr 2>/dev/null)"
[[ "$out" == "OK" ]] || bad "the installed embr could not load a plugin: '$out'"
[[ "$("$b" --version)" == "embr $version "* ]] || bad "--version is wrong: $("$b" --version)"
grep -q '^# >>> embr >>>$' "$work/home/.bashrc" || bad "no embr block in .bashrc"
[[ "$(grep -c '>>> embr >>>' "$work/home/.bashrc")" == 1 ]] || bad "more than one block in .bashrc"
head -n 2 "$work/home/.bashrc" | cmp -s - <(printf 'alias ll="ls -l"\nexport FOO=1\n') || bad "the first lines of .bashrc changed"
got="$(env -i HOME="$work/home" PATH=/usr/bin:/bin sh -c ". '$d/embr/env'; printf '%s|%s' \"\$EMBR_PATH\" \"\$PATH\"")"
[[ "$got" == "$d/embr/extra|$work/home/.local/bin:"* ]] || bad "env did not set EMBR_PATH and PATH: $got"
[[ -d "$d/embr/extra/plugins" && -d "$d/embr/extra/modules" ]] || bad "the extra folders are missing"
for f in applications/embr.desktop mime/packages/embr.xml icons/hicolor/16x16/apps/embr.png icons/hicolor/512x512/apps/embr.png \
         icons/hicolor/scalable/apps/embr.svg icons/hicolor/scalable/mimetypes/text-x-embr.svg \
         icons/hicolor/scalable/mimetypes/application-x-embr-bytecode.svg icons/hicolor/scalable/mimetypes/application-x-embr-package.svg; do
    [[ -f "$d/$f" ]] || bad "missing $f"
done
grep -q "^Exec=\"$d/embr/open\" %f$" "$d/applications/embr.desktop" || bad "the menu entry does not point at the launcher"
if command -v desktop-file-validate >/dev/null; then
    desktop-file-validate "$d/applications/embr.desktop" || bad "the menu entry is not valid"
fi
[[ -x "$d/embr/open" && -x "$d/embr/uninstall.sh" && -f "$d/embr/manifest" ]] || bad "launcher, uninstaller or manifest missing"

# the launcher runs a script and shows a package
mkdir -p "$work/pk"
printf 'print("from open")\n' > "$work/pk/hello.embr"
out="$(cd / && echo | env -u EMBR_PATH "$d/embr/open" "$work/pk/hello.embr" 2>&1)"
[[ "$out" == *"from open"* && "$out" == *"press enter"* ]] || bad "the launcher did not run the script: $out"

# the file types (needs the mime tools and gio, both are optional on a bare box)
if command -v update-mime-database >/dev/null && command -v gio >/dev/null; then
    for pair in a.embr:text/x-embr a.vmbr:application/x-embr-bytecode a.pmbr:application/x-embr-package; do
        f="${pair%%:*}"; want="${pair#*:}"; echo x > "$work/$f"
        got="$(XDG_DATA_HOME="$d" gio info -a standard::content-type "$work/$f" 2>/dev/null | sed -n 's/.*content-type: //p')"
        [[ "$got" == "$want" ]] || bad "$f should be $want, gio says '$got'"
    done
    got="$(XDG_DATA_HOME="$d" gio mime text/x-embr 2>/dev/null | head -n 1)"
    [[ "$got" == *embr.desktop* ]] || bad "embr is not the default for .embr: $got"
else
    echo "note: no update-mime-database or gio, the file type checks were skipped"
fi

# installing again changes nothing that matters
inst sh "$src/install.sh" --from "$work/dl/$name.tar.gz" >"$work/out" 2>&1 || bad "a second install failed: $(cat "$work/out")"
[[ "$(grep -c '>>> embr >>>' "$work/home/.bashrc")" == 1 ]] || bad "a second install added a second block"
[[ "$(find "$d/embr" -maxdepth 1 -type d -name '[0-9]*' | wc -l)" == 1 ]] || bad "a second install left two versions"

# uninstall keeps the files the user put in extra, and takes out everything else
echo 'x' > "$d/embr/extra/plugins/mine.so"
"$d/embr/uninstall.sh" >"$work/out" 2>&1 || bad "uninstall failed: $(cat "$work/out")"
[[ "$(cat "$work/home/.bashrc")" == "$(cat "$work/bashrc.orig")" ]] || bad ".bashrc is not back to what it was: $(cat "$work/home/.bashrc")"
[[ ! -e "$b" && ! -L "$b" ]] || bad "the embr link is still there"
[[ -f "$d/embr/extra/plugins/mine.so" ]] || bad "uninstall deleted the user's own plugin"
leftover="$(tree_now | diff "$work/tree.orig" - | grep '^>' | grep -v '/embr/extra' | grep -v '/\.local\(/share\)\?\(/embr\)\?$' || true)"
[[ -z "$leftover" ]] || bad "uninstall left files behind: $leftover"
grep -q "kept" "$work/out" || bad "uninstall did not say it kept the extra folder"

# --purge takes the extra folder too, and the home folder is exactly as before
fresh_home
inst sh "$src/install.sh" --from "$work/dl/$name.tar.gz" >/dev/null 2>&1 || bad "install before purge failed"
echo 'x' > "$d/embr/extra/modules/mine.embr"
"$d/embr/uninstall.sh" --purge >/dev/null 2>&1 || bad "uninstall --purge failed"
leftover="$(tree_now | diff "$work/tree.orig" - | grep '^>' | grep -v '/\.local\(/share\|/bin\)\?$' || true)"
[[ -z "$leftover" ]] || bad "--purge left files behind: $leftover"
cmp -s "$work/home/.bashrc" "$work/bashrc.orig" || bad "--purge: .bashrc changed"

# the options that leave things alone
fresh_home
inst sh "$src/install.sh" --from "$work/dl/$name.tar.gz" --no-modify-path --no-desktop >"$work/out" 2>&1 || bad "install with options failed"
cmp -s "$work/home/.bashrc" "$work/bashrc.orig" || bad "--no-modify-path changed .bashrc"
[[ ! -e "$d/applications" && ! -e "$d/mime" && ! -e "$d/icons" ]] || bad "--no-desktop still wrote desktop files"
grep -q 'export EMBR_PATH' "$work/out" || bad "--no-modify-path did not say what to add"
"$d/embr/uninstall.sh" >/dev/null 2>&1 || bad "uninstall after options failed"

# a command that is not ours is never replaced
fresh_home
mkdir -p "$work/home/.local/bin"; echo 'mine' > "$work/home/.local/bin/embr"
if inst sh "$src/install.sh" --from "$work/dl/$name.tar.gz" >"$work/out" 2>&1; then bad "install replaced an embr that is not ours"; fi
[[ "$(cat "$work/home/.local/bin/embr")" == mine ]] || bad "install touched an embr that is not ours"

# a checksum that does not match stops the install
fresh_home
cp "$work/dl/$name.tar.gz" "$work/bad.tar.gz"; echo 0000 > "$work/bad.tar.gz.sha256"
if inst sh "$src/install.sh" --from "$work/bad.tar.gz" >"$work/out" 2>&1; then bad "install went on with a wrong checksum"; fi
[[ ! -e "$work/home/.local/share/embr" ]] || bad "a refused install still wrote files"

# the download path: a file:// address laid out like the release page
if command -v curl >/dev/null; then
    fresh_home
    mkdir -p "$work/rel/download/v$version"; cp "$work/dl/"* "$work/rel/download/v$version/"
    inst env EMBR_INSTALL_BASE="file://$work/rel" sh "$src/install.sh" --version "$version" >"$work/out" 2>&1 || bad "download install failed: $(cat "$work/out")"
    [[ "$("$work/home/.local/bin/embr" --version)" == "embr $version "* ]] || bad "the downloaded install does not run"
    "$d/embr/uninstall.sh" >/dev/null 2>&1
    echo 0000 > "$work/rel/download/v$version/$name.tar.gz.sha256"
    if inst env EMBR_INSTALL_BASE="file://$work/rel" sh "$src/install.sh" --version "$version" >"$work/out" 2>&1; then bad "download install ignored a wrong checksum"; fi
fi

# the latest release and the download over http, with curl and with wget. a small server gives /latest the redirect github gives
if command -v python3 >/dev/null && command -v curl >/dev/null && command -v wget >/dev/null; then
    cat > "$work/serve.py" <<'PY'
import http.server, sys
root, version = sys.argv[1], sys.argv[2]
class H(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **k): super().__init__(*a, directory=root, **k)
    def log_message(self, *a): pass
    def go(self, head):
        if self.path == "/latest":
            self.send_response(302); self.send_header("Location", "/tag/v" + version); self.end_headers()
        elif self.path.startswith("/tag/"):
            self.send_response(200); self.send_header("Content-Length", "0"); self.end_headers()
        elif head: super().do_HEAD()
        else: super().do_GET()
    def do_GET(self): self.go(False)
    def do_HEAD(self): self.go(True)
s = http.server.HTTPServer(("127.0.0.1", 0), H)
print(s.server_address[1], flush=True)
s.serve_forever()
PY
    python3 "$work/serve.py" "$work/rel" "$version" > "$work/port" &
    server=$!
    trap 'kill $server 2>/dev/null; rm -rf "$work"' EXIT
    for _ in 1 2 3 4 5 6 7 8 9 10; do [[ -s "$work/port" ]] && break; sleep 0.3; done
    url="http://127.0.0.1:$(cat "$work/port")"
    (cd "$work/dl" && sha256sum "$name.tar.gz" > "$work/rel/download/v$version/$name.tar.gz.sha256")
    for tool in curl wget; do
        fresh_home
        nocurl=; [[ $tool == wget ]] && nocurl=1
        inst env EMBR_INSTALL_BASE="$url" EMBR_NO_CURL="$nocurl" sh "$src/install.sh" >"$work/out" 2>&1 || bad "$tool: install of the latest release failed: $(cat "$work/out")"
        [[ "$("$work/home/.local/bin/embr" --version 2>/dev/null)" == "embr $version "* ]] || bad "$tool: the latest release does not run"
        "$d/embr/uninstall.sh" >/dev/null 2>&1
    done
    kill $server 2>/dev/null
else
    echo "note: no python3, curl and wget together, the http download checks were skipped"
fi

# --system, into a prefix we can write to
fresh_home
sys="$work/usr/local"; prof="$work/etc/profile.d"
inst env EMBR_SYSTEM_PREFIX="$sys" EMBR_PROFILE_D="$prof" sh "$src/install.sh" --system --from "$work/dl/$name.tar.gz" >"$work/out" 2>&1 || bad "system install failed: $(cat "$work/out")"
[[ -L "$sys/bin/embr" && -f "$prof/embr.sh" && -f "$sys/share/applications/embr.desktop" ]] || bad "system install is missing files"
cmp -s "$work/home/.bashrc" "$work/bashrc.orig" || bad "--system changed a user's .bashrc"
got="$(env -i HOME="$work/home" PATH=/usr/bin:/bin sh -c ". '$prof/embr.sh'; printf '%s' \"\$EMBR_PATH\"")"
[[ "$got" == "$work/home/.local/share/embr/extra" ]] || bad "the system EMBR_PATH should be the user's own folder: $got"
out="$(cd "$work" && env -u EMBR_PATH "$sys/bin/embr" t.embr 2>/dev/null)"
[[ "$out" == "OK" ]] || bad "the system embr could not load a plugin: '$out'"
"$sys/share/embr/uninstall.sh" >/dev/null 2>&1 || bad "system uninstall failed"
[[ -z "$(find "$work/usr" "$work/etc" -type f 2>/dev/null)" ]] || bad "system uninstall left files: $(find "$work/usr" "$work/etc" -type f)"

[[ $fail == 0 ]] && echo "installer_smoke: ok"
exit $fail
