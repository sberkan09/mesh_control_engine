#!/usr/bin/env bash
# Local CI for the macOS port (docs/architecture/macos-metal.md §4): what a Mac PR must pass before it
# merges. Runs on the Mac itself (Apple clang, arm64, a logged-in window server for the GPU stages).
#
#   scripts/ci/local_ci_macos.sh              # everything
#   scripts/ci/local_ci_macos.sh --quick      # walls + macos-debug (with GPU tests)
#   scripts/ci/local_ci_macos.sh walls samples   # pick stages
#
# Stages: walls, macos-debug, macos-release, macos-asan, validation, samples, determinism, ship.
#   macos-debug/-release  build + ctest, GPU tests (test_rhi_metal, test_render_gl on Metal) included
#   macos-asan            UBSan build + ctest, no GPU tests (ASan's runtime hangs on macOS 26.6: M-MAC.md)
#   validation            the GPU tests again under MTL_DEBUG_LAYER + MTL_SHADER_VALIDATION
#   samples               01_triangle, sprites_2d, 03_platformer, 04_diorama, 04_cubes: 120 frames each under
#                         Metal validation, last frame captured to build/local-ci-logs/shots/*.png
#   determinism           scripts/ci/determinism.py, Debug vs Release hashes on this Mac (ADR-0007)
#   ship                  scripts/ship.py platformer on macos-release: the signed .app bundle
#                         (cmake/macos/macos_bundle.py), codesign --verify, a 60-frame smoke run
#   android               (on request only, M-MOBILE) scripts/ship.py platformer + diorama --target android:
#                         the APKs, and a 300-frame smoke run on the emulator/device adb sees, if any
#                         (start the emulator with -gpu host: SwiftShader hangs in present)
#   ios                   (on request only, M-MOBILE) scripts/ship.py platformer + diorama --target ios: the
#                         Simulator apps, each played 300 frames in an iPhone Simulator (booted if none is)
#
# Deps: Xcode (Metal framework), cmake >= 3.28, ninja, python3 (android: the Android SDK + NDK, JDK 17).
# Bash 3.2 compatible (macOS /bin/bash).
# Exit code 0 only if every selected stage passed. A summary table is printed at the end.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

ALL_STAGES="walls macos-debug macos-release macos-asan validation samples determinism ship"

stages=""
for arg in "$@"; do
    case "$arg" in
        --quick) stages="$stages walls macos-debug" ;;
        -h|--help) sed -n '2,26p' "$0"; exit 0 ;;
        *) stages="$stages $arg" ;;
    esac
done
[[ -z "$stages" ]] && stages="$ALL_STAGES"

LOG_DIR="$ROOT/build/local-ci-logs"
SHOTS="$LOG_DIR/shots"
mkdir -p "$LOG_DIR" "$SHOTS"

gpu_tests_for() {
    case "$1" in
        macos-debug|macos-release) echo ON ;;
        *) echo OFF ;;
    esac
}

# Runs "$@" and kills it after $1 seconds (macOS has no coreutils `timeout`).
with_timeout() {
    local secs="$1"; shift
    "$@" &
    local pid=$! i=0
    while kill -0 "$pid" 2>/dev/null; do
        if (( i >= secs )); then
            echo "timeout after ${secs}s: $*"
            kill -9 "$pid" 2>/dev/null
            wait "$pid" 2>/dev/null
            return 124
        fi
        sleep 1
        i=$((i + 1))
    done
    wait "$pid"
}

configure_build() {
    cmake --preset "$1" -DMCE_GPU_TESTS="$(gpu_tests_for "$1")" && cmake --build --preset "$1"
}

stage_preset() {
    # UBSan findings fail the test that hit them instead of scrolling past. Known findings in
    # cloud-owned code are suppressed (cmake/macos/ubsan.supp, each one reported). sdk_install is
    # skipped under UBSan: its consumer project builds without -fsanitize and cannot link the
    # instrumented SDK libraries; the debug and release stages run it.
    local exclude=""
    [[ "$1" == macos-asan ]] && exclude="-LE sdk"
    configure_build "$1" &&
        UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1:suppressions=$ROOT/cmake/macos/ubsan.supp" \
            ctest --preset "$1" --timeout 600 $exclude
}

stage_validation() {
    configure_build macos-debug &&
        MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 ctest --preset macos-debug -L gpu --timeout 600
}

stage_samples() {
    configure_build macos-debug || return 1
    local b=build/macos-debug/samples
    local rc=0 name bin
    for spec in "01_triangle:$b/01_triangle/01_triangle" "sprites_2d:$b/sprites_2d/sprites_2d" \
                "03_platformer:$b/03_platformer/sample_03_platformer" "04_diorama:$b/04_diorama/sample_04_diorama" \
                "04_cubes:$b/04_cubes/sample_04_cubes"; do
        name="${spec%%:*}"
        bin="${spec#*:}"
        rm -f "$SHOTS/$name.ppm" "$SHOTS/$name.png"
        echo "--- $name"
        if MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 with_timeout 120 "$bin" -frames 120 -capture "$SHOTS/$name.ppm" &&
            [[ -s "$SHOTS/$name.ppm" ]]; then
            sips -s format png "$SHOTS/$name.ppm" --out "$SHOTS/$name.png" >/dev/null && rm -f "$SHOTS/$name.ppm"
            echo "$name: ok ($SHOTS/$name.png)"
        else
            echo "$name: FAILED"
            rc=1
        fi
    done
    return $rc
}

# The Linux determinism stage, on the two macOS trees (macos-debug vs macos-release).
stage_determinism() {
    local targets="sample_02_boids sample_physics2d_headless mce_devhost modtest_good sample_03_platformer sample_04_diorama mce_player scene_demo_content sample_physics3d_headless"
    local d=build/macos-debug r=build/macos-release
    # shellcheck disable=SC2086
    cmake --preset macos-debug -DMCE_GPU_TESTS=ON && cmake --build --preset macos-debug --target $targets &&
        cmake --preset macos-release -DMCE_GPU_TESTS=ON && cmake --build --preset macos-release --target $targets || return 1
    python3 scripts/ci/determinism.py --runs 3 --args "-ticks 1000 -boids 20000" \
        $d/samples/02_boids/sample_02_boids $r/samples/02_boids/sample_02_boids &&
    python3 scripts/ci/determinism.py --runs 3 --args "-ticks 2000" \
        $d/samples/physics2d_headless/sample_physics2d_headless $r/samples/physics2d_headless/sample_physics2d_headless &&
    python3 scripts/ci/determinism.py --runs 3 --args="-headless 1 -ticks 1800" \
        $d/samples/03_platformer/sample_03_platformer $r/samples/03_platformer/sample_03_platformer &&
    python3 scripts/ci/determinism.py --runs 3 --args="-headless 1 -ticks 600" \
        $d/samples/04_diorama/sample_04_diorama $r/samples/04_diorama/sample_04_diorama &&
    python3 scripts/ci/determinism.py --runs 3 \
        --args="-headless 1 -ticks 600 -pack $d/samples/scene_demo/project/assets.mce" \
        $d/src/player/mce_player $r/src/player/mce_player &&
    python3 scripts/ci/determinism.py --runs 3 --args "-ticks 2000" \
        $d/samples/physics3d_headless/sample_physics3d_headless $r/samples/physics3d_headless/sample_physics3d_headless || return 1
    # A game module ticked by the devhost: each tree against itself, then the two hashes compared.
    local hd hr t
    for t in $d $r; do
        python3 scripts/ci/determinism.py --runs 3 \
            --args "-module $t/tests/module/modules/libmodtest_good.so -ticks 2000" "$t/tests/devhost/mce_devhost" || return 1
    done
    hd="$("$d/tests/devhost/mce_devhost" -module "$d/tests/module/modules/libmodtest_good.so" -ticks 2000 | grep -o 'state_hash [0-9a-f]*')"
    hr="$("$r/tests/devhost/mce_devhost" -module "$r/tests/module/modules/libmodtest_good.so" -ticks 2000 | grep -o 'state_hash [0-9a-f]*')"
    echo "module state hash: debug $hd, release $hr"
    [[ "$hd" == "$hr" ]]
}

stage_android() {
    local out="$LOG_DIR/android" smoke="" adb="${ANDROID_HOME:-$HOME/Library/Android/sdk}/platform-tools/adb"
    rm -rf "$out"
    if [[ -x "$adb" ]] && "$adb" devices | sed 1d | grep -q 'device$'; then
        smoke="--smoke"
    else
        echo "android: no emulator or device attached, building the APKs only"
    fi
    python3 scripts/ship.py platformer --target android --out "$out" $smoke &&
        python3 scripts/ship.py diorama --target android --out "$out" $smoke
}

stage_ios() {
    local out="$LOG_DIR/ios"
    rm -rf "$out"
    python3 scripts/ship.py platformer --target ios --out "$out" --smoke &&
        python3 scripts/ship.py diorama --target ios --out "$out" --smoke
}

stage_ship() {
    local out="$LOG_DIR/dist"
    rm -rf "$out"
    python3 scripts/ship.py platformer --preset macos-release --out "$out" --smoke || return 1
    local app
    app="$(ls -d "$out"/*/*.app | head -1)"
    [[ -n "$app" ]] && codesign --verify --strict --deep --verbose=1 "$app"
}

run_stage() {
    case "$1" in
        walls) python3 scripts/ci/check_walls.py ;;
        macos-debug|macos-release|macos-asan) stage_preset "$1" ;;
        validation) stage_validation ;;
        samples) stage_samples ;;
        determinism) stage_determinism ;;
        ship) stage_ship ;;
        android) stage_android ;;
        ios) stage_ios ;;
        *) echo "unknown stage: $1 (known: $ALL_STAGES android ios)"; return 2 ;;
    esac
}

summary=""
failed=0
for s in $stages; do
    echo "=== $s"
    start=$SECONDS
    if run_stage "$s" > "$LOG_DIR/$s.log" 2>&1; then
        result="PASS ($((SECONDS - start))s)"
    else
        result="FAIL ($((SECONDS - start))s) — see build/local-ci-logs/$s.log"
        failed=1
        tail -n 40 "$LOG_DIR/$s.log"
    fi
    echo "    $result"
    summary="$summary$(printf '  %-16s %s' "$s" "$result")"$'\n'
done

echo
echo "=== Summary ($(git rev-parse --short HEAD 2>/dev/null), $(sw_vers -productVersion) $(uname -m))"
printf '%s' "$summary"
exit $failed
