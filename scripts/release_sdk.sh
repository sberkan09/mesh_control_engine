#!/bin/sh
# Build the release preset and pack the installed SDK as one archive for ./install.sh:
#
#   scripts/release_sdk.sh [preset]      # default: macos-release on a Mac, linux-gcc-release on Linux
#
# Writes dist/mce-sdk-<version>-<os>-<arch>.tar.gz (one mce-sdk/ folder inside) and its .sha256.
# The SDK is relocatable (relative rpaths, MCEConfig finds its prefix), so the archive unpacks
# anywhere. Publish it as a GitHub release asset, which `./install.sh --release` downloads:
#
#   gh release create v<version> dist/mce-sdk-<version>-<os>-<arch>.tar.gz   # or: gh release upload <tag> ...

set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
case "$(uname -s)" in
    Darwin) OS=macos; preset="${1:-macos-release}" ;;
    Linux) OS=linux; preset="${1:-linux-gcc-release}" ;;
    *) echo "release_sdk.sh: macOS and Linux only" >&2; exit 1 ;;
esac
ARCH="$(uname -m)"
case "$ARCH" in aarch64) ARCH=arm64 ;; amd64) ARCH=x86_64 ;; esac

cd "$ROOT"
cmake --preset "$preset"
cmake --build --preset "$preset"
stage="$ROOT/build/sdk-stage"
rm -rf "$stage"
cmake --install "$ROOT/build/$preset" --prefix "$stage/mce-sdk"

version="$(head -n1 "$stage/mce-sdk/share/mce/VERSION")+$(git -C "$ROOT" rev-parse --short=8 HEAD)"
mkdir -p "$ROOT/dist"
out="$ROOT/dist/mce-sdk-$version-$OS-$ARCH.tar.gz"
(cd "$stage" && tar -czf "$out" mce-sdk)
(cd "$ROOT/dist" && shasum -a 256 "$(basename "$out")" > "$(basename "$out").sha256" 2>/dev/null \
    || sha256sum "$(basename "$out")" > "$(basename "$out").sha256")
rm -rf "$stage"
echo "release_sdk.sh: wrote $out"
