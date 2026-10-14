#!/usr/bin/env bash
# Golden-image suite (FR-DBG-020), dev-machine-only: builds test_golden_gl and runs it three times per
# backend (0 flakes over 3 runs). Missing goldens are written on the first run; MCE_UPDATE_GOLDENS=1
# rebaselines all of them. Mismatches leave <name>.actual.png / .diff.png under the build's golden_out/.
#   scripts/golden.sh [build dir]          (default build/linux-gcc-debug)
#   MCE_GOLDEN_BACKENDS="gl sdlgpu" ...    (default: gl, plus sdlgpu when the build has it)
set -euo pipefail
cd "$(dirname "$0")/.."
BUILD="${1:-build/linux-gcc-debug}"
cmake --build "$BUILD" --target test_golden_gl
BIN="$BUILD/tests/render_gl/test_golden_gl"
BACKENDS="${MCE_GOLDEN_BACKENDS:-gl sdlgpu}"
RUN=()
if [[ "$(uname)" == "Linux" && -z "${DISPLAY:-}" ]] && command -v xvfb-run >/dev/null; then
    RUN=(xvfb-run -a -s "-screen 0 1280x1024x24 -noreset")
fi
UPDATE="${MCE_UPDATE_GOLDENS:-0}"
unset MCE_UPDATE_GOLDENS
for backend in $BACKENDS; do
    for i in 1 2 3; do
        echo "=== golden: $backend, run $i"
        # A rebaseline writes on run 1; runs 2 and 3 compare against it.
        update=0
        [[ "$i" == 1 ]] && update="$UPDATE"
        MCE_UPDATE_GOLDENS="$update" MCE_RHI="$backend" "${RUN[@]}" "$BIN"
    done
done
