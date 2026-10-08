#!/usr/bin/env bash
# Local CI: the checks the old GitHub Actions workflow ran, for running on a Linux box before merging.
#
#   scripts/ci/local_ci.sh            # everything: walls, 4 Linux presets, clang-tidy, determinism
#   scripts/ci/local_ci.sh --quick    # walls + linux-gcc-debug (with GPU tests)
#   scripts/ci/local_ci.sh walls tidy linux-asan   # pick stages
#
# Stages: walls, linux-gcc-debug, linux-clang-release, linux-gcc-release, linux-asan, tidy, determinism.
# GPU tests run under Xvfb + Mesa llvmpipe. Windows/MSVC presets are not covered; build them on a Windows
# machine when a change touches platform-specific code.
#
# Ubuntu 24.04 deps: cmake ninja-build gcc g++ clang clang-tidy-18 python3 xvfb libgl1-mesa-dri libgl-dev
#   libegl-dev libx11-dev libxext-dev libxcursor-dev libxrandr-dev libxi-dev libxss-dev libxtst-dev
#   libxfixes-dev libxkbcommon-dev
#
# Exit code 0 only if every selected stage passed. A summary table is printed at the end.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

export LIBGL_ALWAYS_SOFTWARE=1  # Mesa llvmpipe: GL 4.5 core without a GPU

ALL_STAGES=(walls linux-gcc-debug linux-clang-release linux-gcc-release linux-asan tidy determinism)
declare -A GPU_TESTS=([linux-gcc-debug]=ON [linux-clang-release]=ON [linux-gcc-release]=OFF [linux-asan]=OFF)

stages=()
for arg in "$@"; do
    case "$arg" in
        --quick) stages+=(walls linux-gcc-debug) ;;
        -h|--help) sed -n '2,16p' "$0"; exit 0 ;;
        *) stages+=("$arg") ;;
    esac
done
[[ ${#stages[@]} -eq 0 ]] && stages=("${ALL_STAGES[@]}")

LOG_DIR="$ROOT/build/local-ci-logs"
mkdir -p "$LOG_DIR"

run_tests() {
    xvfb-run -a -s "-screen 0 1280x1024x24 -noreset" ctest --preset "$1"
}

stage_preset() {
    local preset="$1"
    cmake --preset "$preset" -DMCE_GPU_TESTS="${GPU_TESTS[$preset]}" &&
        cmake --build --preset "$preset" &&
        run_tests "$preset"
}

stage_tidy() {
    cmake --preset linux-clang-debug &&
        cmake --build --preset linux-clang-debug &&
        run-clang-tidy-18 -p build/linux-clang-debug -quiet "^$ROOT/(src|samples|tests)/"
}

stage_determinism() {
    # Reuses the gcc debug/release trees (tests stay ON so it doesn't thrash the test stages' caches).
    cmake --preset linux-gcc-debug -DMCE_GPU_TESTS="${GPU_TESTS[linux-gcc-debug]}" &&
        cmake --build --preset linux-gcc-debug --target sample_02_boids sample_physics2d_headless sample_03_platformer &&
        cmake --preset linux-gcc-release -DMCE_GPU_TESTS="${GPU_TESTS[linux-gcc-release]}" &&
        cmake --build --preset linux-gcc-release --target sample_02_boids sample_physics2d_headless sample_03_platformer &&
        python3 scripts/ci/determinism.py --runs 3 --args "-ticks 1000 -boids 20000" \
            build/linux-gcc-debug/samples/02_boids/sample_02_boids \
            build/linux-gcc-release/samples/02_boids/sample_02_boids &&
        python3 scripts/ci/determinism.py --runs 3 --args "-ticks 2000" \
            build/linux-gcc-debug/samples/physics2d_headless/sample_physics2d_headless \
            build/linux-gcc-release/samples/physics2d_headless/sample_physics2d_headless &&
        python3 scripts/ci/determinism.py --runs 3 --args "-headless 1 -ticks 1800" \
            build/linux-gcc-debug/samples/03_platformer/sample_03_platformer \
            build/linux-gcc-release/samples/03_platformer/sample_03_platformer
}

run_stage() {
    case "$1" in
        walls) python3 scripts/ci/check_walls.py ;;
        linux-*) [[ -n "${GPU_TESTS[$1]:-}" ]] || { echo "unknown preset stage: $1"; return 2; }; stage_preset "$1" ;;
        tidy) stage_tidy ;;
        determinism) stage_determinism ;;
        *) echo "unknown stage: $1 (known: ${ALL_STAGES[*]})"; return 2 ;;
    esac
}

declare -A RESULT
failed=0
for s in "${stages[@]}"; do
    echo "=== $s"
    start=$SECONDS
    if run_stage "$s" > "$LOG_DIR/$s.log" 2>&1; then
        RESULT[$s]="PASS ($((SECONDS - start))s)"
    else
        RESULT[$s]="FAIL ($((SECONDS - start))s) — see build/local-ci-logs/$s.log"
        failed=1
        tail -n 40 "$LOG_DIR/$s.log"
    fi
    echo "    ${RESULT[$s]}"
done

echo
echo "=== Summary ($(git rev-parse --short HEAD 2>/dev/null))"
for s in "${stages[@]}"; do printf '  %-22s %s\n' "$s" "${RESULT[$s]}"; done
exit $failed
