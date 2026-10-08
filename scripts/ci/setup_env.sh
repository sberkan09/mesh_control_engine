#!/usr/bin/env bash
# Installs what scripts/ci/local_ci.sh needs on Ubuntu 24.04 (plus ccache). Idempotent and quiet:
# when every package is already present it returns in well under a second without touching apt.
#
#   scripts/ci/setup_env.sh
#
# Used by the cloud environment's setup script and by the SessionStart hook in .claude/settings.json.

set -euo pipefail

PKGS=(cmake ninja-build gcc g++ clang clang-tidy-18 python3 xvfb xauth ccache libgl1-mesa-dri libgl-dev
    libegl-dev libx11-dev libxext-dev libxcursor-dev libxrandr-dev libxi-dev libxss-dev libxtst-dev
    libxfixes-dev libxkbcommon-dev)

missing=()
for p in "${PKGS[@]}"; do
    dpkg-query -W -f='${Status}' "$p" 2>/dev/null | grep -q "install ok installed" || missing+=("$p")
done
[[ ${#missing[@]} -eq 0 ]] && exit 0

SUDO=()
[[ $(id -u) -ne 0 ]] && SUDO=(sudo)
export DEBIAN_FRONTEND=noninteractive
echo "setup_env: installing ${missing[*]}"
"${SUDO[@]}" apt-get install -y -qq --no-install-recommends "${missing[@]}" ||
    { "${SUDO[@]}" apt-get update -qq && "${SUDO[@]}" apt-get install -y -qq --no-install-recommends "${missing[@]}"; }
