#!/bin/sh
# One-step install of the MCE SDK (docs/user-guide/01-getting-started.md, section 1.9).
#
#   ./install.sh                         # in an engine checkout: build the release preset and install it
#   ./install.sh mce-sdk-<ver>-macos-arm64.tar.gz   # unpack a prebuilt SDK archive (scripts/release_sdk.sh)
#   ./install.sh --release [tag]         # download the prebuilt archive for this machine from the
#                                        # repository's GitHub Releases (needs `gh`, signed in: the repo is private)
#
# Options: --prefix <dir> (default ~/mce-sdk, or $MCE_SDK), --no-path (leave the shell profile alone),
# --yes (install missing build tools without asking, through Homebrew on macOS or apt on Ubuntu).
#
# Every mode checks the tools a project build needs (CMake >= 3.28, Ninja, a C++20 compiler, Python 3),
# puts <prefix>/bin on PATH through ~/.zshrc or ~/.bashrc, and ends with `mce version`.

set -eu

REPO="sberkan09/mesh_control_engine"
PREFIX="${MCE_SDK:-$HOME/mce-sdk}"
MODE=source
ARCHIVE=""
TAG=""
EDIT_PATH=1
YES=0

say() { printf 'mce-install: %s\n' "$*"; }
die() { printf 'mce-install: error: %s\n' "$*" >&2; exit 1; }

while [ $# -gt 0 ]; do
    case "$1" in
        --prefix) [ $# -ge 2 ] || die "--prefix needs a directory"; PREFIX="$2"; shift 2 ;;
        --no-path) EDIT_PATH=0; shift ;;
        --yes|-y) YES=1; shift ;;
        --release) MODE=release; shift
                   if [ $# -gt 0 ] && [ "${1#-}" = "$1" ]; then TAG="$1"; shift; fi ;;
        -h|--help) sed -n '2,14p' "$0"; exit 0 ;;
        -*) die "unknown option $1 (--help lists them)" ;;
        *) MODE=archive; ARCHIVE="$1"; shift ;;
    esac
done

case "$(uname -s)" in
    Darwin) OS=macos ;;
    Linux) OS=linux ;;
    *) die "the SDK exists for macOS and Linux only; on Windows build from the checkout (chapter 1)" ;;
esac
ARCH="$(uname -m)"
case "$ARCH" in aarch64) ARCH=arm64 ;; amd64) ARCH=x86_64 ;; esac
if [ "$OS" = macos ]; then
    [ "$ARCH" = arm64 ] || die "macOS needs an Apple silicon Mac (this one is $ARCH)"
    major="$(sw_vers -productVersion | cut -d. -f1)"
    [ "$major" -ge 26 ] || die "macOS 26.0 or later is required (this Mac runs $(sw_vers -productVersion))"
fi

# --- build tools -------------------------------------------------------------------------------
have() { command -v "$1" >/dev/null 2>&1; }

ask() {  # ask "<question>": 0 on yes
    [ "$YES" -eq 1 ] && return 0
    [ -t 0 ] || return 1
    printf 'mce-install: %s [y/N] ' "$1"
    read -r answer
    case "$answer" in y|Y|yes|YES) return 0 ;; *) return 1 ;; esac
}

cmake_ok() {
    have cmake || return 1
    v="$(cmake --version | sed -n 's/^cmake version \([0-9]*\)\.\([0-9]*\).*/\1 \2/p')"
    set -- $v
    [ "${1:-0}" -gt 3 ] || { [ "${1:-0}" -eq 3 ] && [ "${2:-0}" -ge 28 ]; }
}

check_tools() {
    missing=""
    if [ "$OS" = macos ]; then
        if ! xcode-select -p >/dev/null 2>&1; then
            say "Apple's Command Line Tools are missing (the C++ compiler and the Metal framework)."
            xcode-select --install >/dev/null 2>&1 || true
            die "finish the Command Line Tools installer that just opened, then run this script again"
        fi
        cmake_ok || missing="$missing cmake"
        have ninja || missing="$missing ninja"
        have python3 || missing="$missing python3"
        [ -z "$missing" ] && return 0
        if have brew && ask "install$missing with Homebrew?"; then
            # shellcheck disable=SC2086
            brew install $missing
        else
            die "missing:$missing. Install them (for example: brew install$missing) and run this script again"
        fi
    else
        cmake_ok || missing="$missing cmake"
        have ninja || missing="$missing ninja-build"
        have python3 || missing="$missing python3"
        have g++ || have clang++ || missing="$missing g++"
        [ -z "$missing" ] && return 0
        if have apt-get && ask "install$missing with apt-get (sudo)?"; then
            # shellcheck disable=SC2086
            sudo apt-get install -y $missing
        else
            die "missing:$missing (CMake must be 3.28 or newer). On Ubuntu 24.04, scripts/ci/setup_env.sh installs everything"
        fi
    fi
    cmake_ok || die "CMake 3.28 or newer is required (found: $(cmake --version 2>/dev/null | head -n1))"
}

check_tools

# --- install -----------------------------------------------------------------------------------
unpack() {  # unpack <archive.tar.gz>
    [ -f "$1" ] || die "$1: no such file"
    tmp="$(mktemp -d)"
    tar -xzf "$1" -C "$tmp"
    root="$tmp/mce-sdk"
    [ -x "$root/bin/mce" ] || die "$1 is not an MCE SDK archive (no mce-sdk/bin/mce inside)"
    if [ -e "$PREFIX" ]; then
        [ -f "$PREFIX/share/mce/VERSION" ] || die "$PREFIX exists and is not an MCE SDK; pick another --prefix"
        say "replacing the SDK in $PREFIX"
        rm -rf "$PREFIX"
    fi
    mkdir -p "$(dirname "$PREFIX")"
    mv "$root" "$PREFIX"
    rm -rf "$tmp"
    # A downloaded archive carries the quarantine flag; Gatekeeper would then stop each tool on
    # first launch. The binaries are signed ad hoc by the linker, which Apple silicon accepts.
    [ "$OS" = macos ] && xattr -dr com.apple.quarantine "$PREFIX" 2>/dev/null || true
}

case "$MODE" in
    source)
        here="$(cd "$(dirname "$0")" && pwd)"
        [ -f "$here/CMakePresets.json" ] || die "not in an engine checkout: pass an SDK archive or --release (--help)"
        if [ "$OS" = macos ]; then preset=macos-release; else preset=linux-gcc-release; fi
        # Its own build folder with only what the SDK needs, so the preset's own build keeps its options.
        bdir="$here/build/sdk-$preset"
        say "building the engine ($preset, in $bdir); the first configure downloads the dependencies"
        (cd "$here" && cmake --preset "$preset" -B "$bdir" \
            -DMCE_BUILD_TESTS=OFF -DMCE_BUILD_SAMPLES=OFF -DMCE_TEMPLATE_PROBE=OFF -DMCE_PHYSICS_ORACLE=OFF)
        cmake --build "$bdir"
        stage="$(mktemp -d)"
        cmake --install "$bdir" --prefix "$stage/mce-sdk" >/dev/null
        (cd "$stage" && tar -czf sdk.tar.gz mce-sdk)
        unpack "$stage/sdk.tar.gz"
        rm -rf "$stage"
        ;;
    archive)
        unpack "$ARCHIVE"
        ;;
    release)
        have gh || die "--release needs the GitHub CLI (brew install gh; gh auth login): the repository is private"
        dl="$(mktemp -d)"
        say "downloading mce-sdk-*-$OS-$ARCH.tar.gz from ${TAG:-the latest release} of $REPO"
        # shellcheck disable=SC2086
        gh release download $TAG -R "$REPO" -p "mce-sdk-*-$OS-$ARCH.tar.gz" -D "$dl" \
            || die "no SDK archive for $OS-$ARCH in ${TAG:-the latest release} (scripts/release_sdk.sh makes one)"
        set -- "$dl"/mce-sdk-*.tar.gz
        unpack "$1"
        rm -rf "$dl"
        ;;
esac

# --- PATH --------------------------------------------------------------------------------------
line="export PATH=\"$PREFIX/bin:\$PATH\"  # MCE SDK"
if [ "$EDIT_PATH" -eq 1 ]; then
    case "${SHELL:-}" in
        */zsh) profile="$HOME/.zshrc" ;;
        */bash) if [ "$OS" = macos ]; then profile="$HOME/.bash_profile"; else profile="$HOME/.bashrc"; fi ;;
        *) profile="$HOME/.profile" ;;
    esac
    if ! grep -qsF "$PREFIX/bin" "$profile"; then
        printf '\n%s\n' "$line" >> "$profile"
        say "added $PREFIX/bin to PATH in $profile (open a new terminal, or: source $profile)"
    fi
fi

"$PREFIX/bin/mce" version
say "done. Next: mce new MyGame && mce build MyGame && mce editor MyGame"
