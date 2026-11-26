#!/usr/bin/env bash
# M-NIGHTSHOT: headless night capture of the headlight retro-reflection (M-HEADLIGHT) on the city's road paint.
# usage: night_shot.sh <out.png> [retro]   (retro: marking paint reflectivity, default 0.8; 0 is the baseline)
# Regenerates the content with MCE_NIGHT_SHOT=1 (22:00, a lit parked car behind the camera), captures, then restores the committed content.
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:?out.png}; retro=${2:-0.8}
build=${BUILD:-build/linux-gcc-debug}
trap 'git checkout -- samples/09_openworld/gen_assets.py samples/09_openworld/assets; (cd samples/09_openworld && python3 gen_assets.py >/dev/null)' EXIT
sed -i "s/PAINT_RETRO = {.*}   #/PAINT_RETRO = {\"marking_white\": $retro, \"marking_yellow\": $retro}   #/" samples/09_openworld/gen_assets.py
(cd samples/09_openworld && MCE_NIGHT_SHOT=1 python3 gen_assets.py >/dev/null)
cmake --build "$build" --target openworld_content mce_player -j4 >/dev/null
tmp=$(mktemp --suffix=.ppm)
"$build/src/player/mce_player" -pack "$build/samples/09_openworld/project/assets.mce" -scene scenes/city.mcscene -input /dev/null \
  -frames 12 -capture "$tmp" -platform.error_box 0 -r.vsync 0
python3 -c "import sys;from PIL import Image;Image.open(sys.argv[1]).save(sys.argv[2])" "$tmp" "$out"; rm -f "$tmp"
