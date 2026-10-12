#!/bin/sh
# install.sh
# installs an embr release: the program, an `embr` command, EMBR_PATH, a menu entry, and the icons and file types
# for .embr, .vmbr and .pmbr. uninstall.sh (it is copied next to the install) takes all of it out again.
#
# usage
#   curl -fsSL https://raw.githubusercontent.com/la-creatura/embr-lang/main/install.sh | sh
#   sh install.sh --system        # for every user, run as root (sudo sh install.sh --system)
#
# options
#   --version X.Y.Z    install this release instead of the latest
#   --from FILE        install from a release tarball you already have, nothing is downloaded
#   --system           install under /usr/local instead of your home folder
#   --no-modify-path   leave your shell config alone, you set PATH and EMBR_PATH yourself
#   --no-desktop       skip the menu entry, the icons and the file types
#
# good to know
#   - linux only for now. on other systems build from source (see the readme).
#   - everything it creates outside its own folder is listed in <data>/embr/manifest, which uninstall.sh reads.
#   - EMBR_PATH points at <data>/embr/extra, a folder for your own plugins/ and modules/. uninstall keeps it
#     unless you pass --purge.
#   - EMBR_INSTALL_BASE, EMBR_SYSTEM_PREFIX and EMBR_PROFILE_D replace the release url, /usr/local and
#     /etc/profile.d, and EMBR_NO_CURL=1 makes it use wget. they are there for the tests.

set -eu

repo=la-creatura/embr-lang
base=${EMBR_INSTALL_BASE:-https://github.com/$repo/releases}

say() { printf 'embr: %s\n' "$*"; }
die() { printf 'embr: %s\n' "$*" >&2; exit 1; }

usage() {
    cat <<'EOF'
usage: install.sh [--version X.Y.Z] [--from FILE] [--system] [--no-modify-path] [--no-desktop]
see the top of this file for what each one does
EOF
}

want=
from=
system=0
modify_path=1
desktop=1
while [ $# -gt 0 ]; do
    case $1 in
        --version) [ $# -ge 2 ] || die "--version needs a number"; want=${2#v}; shift ;;
        --from) [ $# -ge 2 ] || die "--from needs a file"; from=$2; shift ;;
        --system) system=1 ;;
        --no-modify-path) modify_path=0 ;;
        --no-desktop) desktop=0 ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; die "unknown option: $1" ;;
    esac
    shift
done

[ "$(uname -s)" = Linux ] || die "the installer only knows linux so far, build from source on this system (see the readme)"
arch=$(uname -m | tr 'A-Z' 'a-z')

# where things go
if [ "$system" = 1 ]; then
    prefix=${EMBR_SYSTEM_PREFIX:-/usr/local}
    data=$prefix/share
    bin=$prefix/bin
    profile_d=${EMBR_PROFILE_D:-/etc/profile.d}
    mkdir -p "$data" "$bin" 2>/dev/null || true
    [ -w "$data" ] && [ -w "$bin" ] || die "no write access to $prefix, try: sudo sh install.sh --system"
else
    [ -n "${HOME:-}" ] || die "HOME is not set"
    data=${XDG_DATA_HOME:-$HOME/.local/share}
    bin=$HOME/.local/bin
fi
ehome=$data/embr

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT INT TERM
: > "$tmp/manifest"
note() { printf '%s %s\n' "$1" "$2" >> "$tmp/manifest"; }

have() { command -v "$1" >/dev/null 2>&1; }
have_curl() { [ -z "${EMBR_NO_CURL:-}" ] && have curl; }

fetch() {
    if have_curl; then curl -fsSL "$1" -o "$2"
    elif have wget; then wget -q "$1" -O "$2"
    else die "need curl or wget to download embr"; fi
}

sha_check() { # dir, file with the checksum in it
    if have sha256sum; then (cd "$1" && sha256sum -c "$2" >/dev/null 2>&1)
    elif have shasum; then (cd "$1" && shasum -a 256 -c "$2" >/dev/null 2>&1)
    else die "need sha256sum or shasum to check the download"; fi
}

latest_tag() {
    if have_curl; then
        where=$(curl -fsSLI -o /dev/null -w '%{url_effective}' "$base/latest") || return 1
    elif have wget; then
        where=$(wget -q --spider --max-redirect=0 -S "$base/latest" 2>&1 | sed -n 's/^ *[Ll]ocation: *//p' | head -n 1 | tr -d '\r')
    else
        die "need curl or wget to download embr"
    fi
    printf '%s\n' "${where##*/}"
}

# get the tarball
if [ -n "$from" ]; then
    [ -f "$from" ] || die "no such file: $from"
    tarball=$from
    if [ -f "$from.sha256" ]; then
        cp "$from" "$tmp/"; cp "$from.sha256" "$tmp/"
        sha_check "$tmp" "$(basename "$from").sha256" || die "the checksum of $from does not match"
    fi
else
    if [ -z "$want" ]; then
        tag=$(latest_tag) || die "could not reach $base"
        case $tag in v[0-9]*) ;; *) die "no release found at $base yet" ;; esac
        want=${tag#v}
    fi
    name=embr-$want-linux-$arch.tar.gz
    say "downloading $name"
    fetch "$base/download/v$want/$name" "$tmp/$name" || die "could not download $base/download/v$want/$name (is there a build for $arch?)"
    fetch "$base/download/v$want/$name.sha256" "$tmp/$name.sha256" || die "could not download the checksum file for $name"
    sha_check "$tmp" "$name.sha256" || die "the checksum of $name does not match, not installing it"
    tarball=$tmp/$name
fi

mkdir "$tmp/x"
tar -xzf "$tarball" -C "$tmp/x" || die "could not unpack $tarball"
src=
for d in "$tmp"/x/*/; do src=${d%/}; done
[ -n "$src" ] && [ -x "$src/bin/embr" ] || die "$tarball does not look like an embr release (no bin/embr)"
[ -f "$src/share/embr/uninstall.sh" ] || die "$tarball is a release without the installer files, use a newer one"

# the new build has to start here, or the install would leave a broken command behind
ver=$("$src/bin/embr" --version 2>/dev/null | sed -n 's/^embr \([^ ]*\).*/\1/p') ||:
[ -n "$ver" ] || die "this build of embr does not start here. it needs libffi and libssl installed (look at the line the loader printed above)"
say "installing embr $ver into $ehome"

# the program
mkdir -p "$ehome" "$bin"
old=
if [ -L "$ehome/current" ]; then old=$(readlink "$ehome/current"); fi
rm -rf "$ehome/$ver.new"
mv "$src" "$ehome/$ver.new"
rm -rf "$ehome/$ver"
mv "$ehome/$ver.new" "$ehome/$ver"
ln -sfn "$ver" "$ehome/current"
case $old in ''|"$ver"|*/*) ;; *) rm -rf "${ehome:?}/$old" ;; esac
note dir "$ehome/$ver"

# the command. never replace one that is not ours (a link you made to your own build, a distro package)
if [ -e "$bin/embr" ] || [ -L "$bin/embr" ]; then
    [ "$(readlink "$bin/embr" 2>/dev/null || true)" = "$ehome/current/bin/embr" ] ||
        die "$bin/embr already exists and is not from this installer. remove it and run this again"
fi
ln -sfn "$ehome/current/bin/embr" "$bin/embr"
note file "$bin/embr"

# the launcher the menu entry uses, and the uninstaller
cp "$ehome/current/share/embr/open.sh" "$ehome/open"
cp "$ehome/current/share/embr/uninstall.sh" "$ehome/uninstall.sh"
chmod +x "$ehome/open" "$ehome/uninstall.sh"

# EMBR_PATH, and PATH if the command folder is not on it. one small file that the shell config (or profile.d) loads
if [ "$system" = 1 ]; then
    extra='${XDG_DATA_HOME:-$HOME/.local/share}/embr/extra'
else
    extra=$ehome/extra
    mkdir -p "$extra/plugins" "$extra/modules"
fi
{
    echo '# set up by the embr installer, uninstall.sh removes it'
    if [ "$system" = 0 ]; then
        printf 'case ":$PATH:" in *":%s:"*) ;; *) PATH="%s:$PATH"; export PATH ;; esac\n' "$bin" "$bin"
    fi
    printf 'embr_extra="%s"\n' "$extra"
    echo 'case ":${EMBR_PATH:-}:" in *":$embr_extra:"*) ;; *) EMBR_PATH="$embr_extra${EMBR_PATH:+:$EMBR_PATH}"; export EMBR_PATH ;; esac'
    echo 'unset embr_extra'
} > "$ehome/env"

hook() { # add the loader of env to a shell config file, once. a second install keeps the block and the notes about it
    note rc "$1"
    if [ -f "$1" ] && grep -q '^# >>> embr >>>$' "$1"; then
        if grep -qx "rcnl $1" "$ehome/manifest" 2>/dev/null; then note rcnl "$1"; fi
        return 0
    fi
    mkdir -p "$(dirname "$1")"
    if [ -s "$1" ] && [ "$(tail -c 1 "$1" | wc -l)" = 0 ]; then echo >> "$1"; note rcnl "$1"; fi
    printf '# >>> embr >>>\n. "%s/env"\n# <<< embr <<<\n' "$ehome" >> "$1"
}

path_help=0
if [ "$system" = 1 ]; then
    if [ "$modify_path" = 1 ]; then
        mkdir -p "$profile_d"
        printf '# set up by the embr installer, uninstall.sh removes it\n[ -r "%s/env" ] && . "%s/env"\n' "$ehome" "$ehome" > "$profile_d/embr.sh"
        note file "$profile_d/embr.sh"
    else
        path_help=1
    fi
elif [ "$modify_path" = 1 ]; then
    shell=${SHELL:-}; shell=${shell##*/}
    done_any=0
    if [ -f "$HOME/.bashrc" ] || [ "$shell" = bash ]; then hook "$HOME/.bashrc"; done_any=1; fi
    if [ -f "$HOME/.zshrc" ] || [ "$shell" = zsh ]; then hook "$HOME/.zshrc"; done_any=1; fi
    if [ -d "$HOME/.config/fish" ] || [ "$shell" = fish ]; then
        mkdir -p "$HOME/.config/fish/conf.d"
        {
            echo '# set up by the embr installer, uninstall.sh removes it'
            printf 'fish_add_path -g "%s"\n' "$bin"
            printf 'contains -- "%s" $EMBR_PATH; or set -gx EMBR_PATH "%s" $EMBR_PATH\n' "$extra" "$extra"
        } > "$HOME/.config/fish/conf.d/embr.fish"
        note file "$HOME/.config/fish/conf.d/embr.fish"
        done_any=1
    fi
    if [ "$done_any" = 0 ]; then hook "$HOME/.profile"; fi
else
    path_help=1
fi

# the menu entry, the file types and the icons
if [ "$desktop" = 1 ]; then
    integ=$ehome/current/share/embr/integration
    mkdir -p "$data/applications" "$data/mime/packages"
    sed "s|@OPEN@|$ehome/open|" "$integ/embr.desktop" > "$data/applications/embr.desktop"
    note file "$data/applications/embr.desktop"
    cp "$integ/embr-mime.xml" "$data/mime/packages/embr.xml"
    note file "$data/mime/packages/embr.xml"
    (cd "$integ/icons" && find . -type f) | while read -r rel; do
        rel=${rel#./}
        mkdir -p "$data/icons/$(dirname "$rel")"
        cp "$integ/icons/$rel" "$data/icons/$rel"
        note file "$data/icons/$rel"
    done
    # a pipe runs in its own shell, so the notes it made are in the manifest file already
    if have update-mime-database; then update-mime-database "$data/mime" >/dev/null 2>&1 || true; fi
    if have update-desktop-database; then update-desktop-database "$data/applications" >/dev/null 2>&1 || true; fi
    if have gtk-update-icon-cache; then gtk-update-icon-cache -q -t -f "$data/icons/hicolor" >/dev/null 2>&1 || true; fi
fi

{ echo "mode $([ "$system" = 1 ] && echo system || echo user)"; echo "data $data"; cat "$tmp/manifest"; } > "$ehome/manifest"

say "done. installed embr $ver"
if [ "$path_help" = 1 ]; then
    say "add these to your shell config yourself:"
    [ "$system" = 1 ] || say "  export PATH=\"$bin:\$PATH\""
    say "  export EMBR_PATH=\"$extra\${EMBR_PATH:+:\$EMBR_PATH}\""
elif [ "$system" = 0 ]; then
    say "open a new terminal (or run: . \"$ehome/env\"), then try: embr --version"
fi
say "to remove it again: \"$ehome/uninstall.sh\""
