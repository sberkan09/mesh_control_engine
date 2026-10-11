#!/usr/bin/env bash
# Local CI: the checks the old GitHub Actions workflow ran, for running on a Linux box before merging.
#
#   scripts/ci/local_ci.sh            # everything: walls, 4 Linux presets, clang-tidy, determinism
#   scripts/ci/local_ci.sh --quick    # walls + linux-gcc-debug (with GPU tests)
#   scripts/ci/local_ci.sh walls tidy linux-asan   # pick stages
#
# Stages: walls, linux-gcc-debug, linux-clang-release, linux-gcc-release, linux-asan, tidy, determinism.
# GPU tests run under Xvfb + Mesa llvmpipe (GL) and lavapipe (Vulkan, for mce_rhi_sdlgpu, ADR-0038).
# Windows/MSVC presets are not covered; build them on a Windows
# machine when a change touches platform-specific code.
#
# Ubuntu 24.04 deps: scripts/ci/setup_env.sh installs them (and ccache).
#
# Speed: compiles go through ccache when it is installed; third-party sources are fetched once into
# build/_fetch/<hash of cmake/Dependencies.cmake> and shared by every preset; the build trees are
# configured in parallel before the first stage; tests run in parallel.
#
# In a project container (/mnt/project-files exists) the ccache directory is restored from a shared
# snapshot before the first stage, so a fresh session starts warm, and saved back after a green full
# run when the snapshot is missing or older than 12 h (after any green run with --save-cache).
# MCE_CCACHE_SNAPSHOT overrides the snapshot path; an empty value turns the snapshot off.
#
# Exit code 0 only if every selected stage passed. A summary table is printed at the end.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

export LIBGL_ALWAYS_SOFTWARE=1  # Mesa llvmpipe: GL 4.5 core without a GPU

ALL_STAGES=(walls linux-gcc-debug linux-clang-release linux-gcc-release linux-asan tidy determinism)
declare -A GPU_TESTS=([linux-gcc-debug]=ON [linux-clang-release]=ON [linux-gcc-release]=OFF [linux-asan]=OFF)

stages=()
save_cache=0
for arg in "$@"; do
    case "$arg" in
        --quick) stages+=(walls linux-gcc-debug) ;;
        --save-cache) save_cache=1 ;;
        -h|--help) sed -n '2,24p' "$0"; exit 0 ;;
        *) stages+=("$arg") ;;
    esac
done
full_run=0
[[ ${#stages[@]} -eq 0 ]] && { stages=("${ALL_STAGES[@]}"); full_run=1; }

LOG_DIR="$ROOT/build/local-ci-logs"
mkdir -p "$LOG_DIR"

JOBS="$(nproc 2>/dev/null || echo 4)"

# Extra configure arguments shared by every preset.
COMMON_ARGS=()
SNAPSHOT=""
if command -v ccache > /dev/null; then
    export CCACHE_BASEDIR="$ROOT" CCACHE_NOHASHDIR=1  # hits survive a different checkout path
    export CCACHE_DIR="${CCACHE_DIR:-$HOME/.cache/ccache}" CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-3G}"
    COMMON_ARGS+=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
    if [[ -d /mnt/project-files/ ]] && command -v zstd > /dev/null; then
        SNAPSHOT="${MCE_CCACHE_SNAPSHOT-/mnt/project-files/.cache/ccache-linux.tar.zst}"
    fi
fi

# The shared snapshot: one zstd tarball of the ccache directory, so the sync folder sees a single file.
restore_cache() {
    [[ -n "$SNAPSHOT" && -f "$SNAPSHOT" ]] || return 0
    # Only into an empty cache: a local cache is at least as fresh as the snapshot it came from.
    [[ -n "$(ls -A "$CCACHE_DIR" 2>/dev/null)" ]] && return 0
    local start=$SECONDS
    mkdir -p "$CCACHE_DIR"
    if tar -I zstd -xf "$SNAPSHOT" -C "$CCACHE_DIR"; then
        echo "=== ccache restored from $SNAPSHOT ($((SECONDS - start))s)"
    else
        echo "=== ccache snapshot unreadable, starting cold"
        rm -rf "${CCACHE_DIR:?}"/*
    fi
}

save_cache_snapshot() {
    [[ -n "$SNAPSHOT" && -d "$CCACHE_DIR" ]] || return 0
    if [[ $save_cache -eq 0 ]]; then
        # Automatic saves only after a full run, so a --quick run never pins a half-warm snapshot.
        [[ $full_run -eq 1 ]] || return 0
        [[ -f "$SNAPSHOT" && $(( $(date +%s) - $(stat -c %Y "$SNAPSHOT") )) -lt 43200 ]] && return 0
    fi
    local start=$SECONDS tmp
    tmp="$(mktemp "${TMPDIR:-/tmp}/ccache-snapshot.XXXXXX")"
    ccache --cleanup > /dev/null 2>&1
    # Written locally first, then moved into place: the shared folder never holds a half-written file.
    if tar -I 'zstd -1 -T0' -cf "$tmp" -C "$CCACHE_DIR" . && mkdir -p "$(dirname "$SNAPSHOT")" &&
        cp "$tmp" "$SNAPSHOT.tmp" && mv "$SNAPSHOT.tmp" "$SNAPSHOT"; then
        echo "=== ccache snapshot saved to $SNAPSHOT ($(du -h "$SNAPSHOT" | cut -f1), $((SECONDS - start))s)"
    else
        echo "=== ccache snapshot not saved (not fatal)"
        rm -f "$SNAPSHOT.tmp"
    fi
    rm -f "$tmp"
}

# Third-party sources (FetchContent) are cloned once and reused by every later configure via
# FETCHCONTENT_SOURCE_DIR_<NAME>. The cache is keyed by the hash of cmake/Dependencies.cmake, so a
# tag bump fetches fresh sources. Only sources are shared; each tree keeps its own dep build dirs.
FETCH_DIR="$ROOT/build/_fetch/$(sha1sum cmake/Dependencies.cmake | cut -c1-12)"
# A tree that fetched a dependency itself keeps its own copy, so its source paths never change.
fetch_args() {
    local src name
    for src in "$FETCH_DIR"/*-src; do
        [[ -d "$src" ]] || continue
        name="$(basename "$src" -src)"
        [[ -d "build/$1/_deps/$name-src" ]] && continue
        printf '%s\n' "-DFETCHCONTENT_SOURCE_DIR_${name^^}=$src"
    done
}

# Hard-links sources a tree had to fetch itself into the shared cache (same filesystem: no copy).
stash_fetched() {
    local src name
    mkdir -p "$FETCH_DIR"
    for src in "build/$1/_deps/"*-src; do
        [[ -d "$src" && ! -L "$src" ]] || continue
        name="$(basename "$src")"
        [[ -e "$FETCH_DIR/$name" ]] && continue
        rm -rf "$FETCH_DIR/.tmp" && cp -al "$src" "$FETCH_DIR/.tmp" && mv "$FETCH_DIR/.tmp" "$FETCH_DIR/$name"
    done
}

# Every stage configures a tree with the same arguments, so a second configure is a no-op.
configure() {
    local preset="$1" args
    mapfile -t args < <(fetch_args "$preset")
    [[ -n "${GPU_TESTS[$preset]:-}" ]] && args+=(-DMCE_GPU_TESTS="${GPU_TESTS[$preset]}")
    cmake --preset "$preset" "${COMMON_ARGS[@]}" "${args[@]}" && stash_fetched "$preset"
}

# CMake's configure step is mostly single-threaded (~20-60 s per tree, SDL's feature checks), so
# the trees the selected stages need are configured up front in parallel: the first alone (it
# fetches the sources), the rest together. Failures are not reported here: the stage that needs
# the tree configures it again and logs the error.
preconfigure() {
    local -A want=()
    local s p first=""
    for s in "${stages[@]}"; do
        case "$s" in
            linux-*) [[ -n "${GPU_TESTS[$s]:-}" ]] && want[$s]=1 ;;
            tidy) want[linux-clang-debug]=1 ;;
            determinism) want[linux-gcc-debug]=1; want[linux-gcc-release]=1 ;;
        esac
    done
    [[ ${#want[@]} -gt 1 ]] || return 0
    echo "=== configure (${!want[*]})"
    local start=$SECONDS
    if ! compgen -G "$FETCH_DIR/*-src" > /dev/null; then
        for p in linux-gcc-debug "${!want[@]}"; do [[ -n "${want[$p]:-}" ]] && { first=$p; break; }; done
        configure "$first" > "$LOG_DIR/configure-$first.log" 2>&1
    fi
    for p in "${!want[@]}"; do
        [[ "$p" == "$first" ]] || configure "$p" > "$LOG_DIR/configure-$p.log" 2>&1 &
    done
    wait
    echo "    done ($((SECONDS - start))s)"
}

run_tests() {
    xvfb-run -a -s "-screen 0 1280x1024x24 -noreset" ctest --preset "$1" -j "$JOBS"
}

stage_preset() {
    local preset="$1"
    configure "$preset" &&
        cmake --build --preset "$preset" &&
        run_tests "$preset"
}

stage_tidy() {
    configure linux-clang-debug &&
        cmake --build --preset linux-clang-debug &&
        run-clang-tidy-18 -p build/linux-clang-debug -quiet "^$ROOT/(src|samples|tests)/"
}

# A game module ticked by the devhost: same World state hash across runs and Debug/Release.
# The module path differs per tree, so each tree is compared against itself and the two hashes
# are then compared.
stage_module_determinism() {
    local h=()
    for t in linux-gcc-debug linux-gcc-release; do
        python3 scripts/ci/determinism.py --runs 3 \
            --args "-module build/$t/tests/module/modules/libmodtest_good.so -ticks 2000" \
            "build/$t/tests/devhost/mce_devhost" || return 1
        h+=("$("build/$t/tests/devhost/mce_devhost" -module "build/$t/tests/module/modules/libmodtest_good.so" \
            -ticks 2000 | grep -o 'state_hash [0-9a-f]*')")
    done
    echo "module state hash: debug ${h[0]}, release ${h[1]}"
    [[ "${h[0]}" == "${h[1]}" ]]
}

# M5b (ADR-0003 §8): the Jolt-backed 3D scene hashes the same across runs and Debug/Release. Runs
# after stage_determinism has configured both gcc trees.
stage_physics3d_determinism() {
    cmake --build --preset linux-gcc-debug --target sample_physics3d_headless &&
        cmake --build --preset linux-gcc-release --target sample_physics3d_headless &&
        python3 scripts/ci/determinism.py --runs 3 --args "-ticks 2000" \
            build/linux-gcc-debug/samples/physics3d_headless/sample_physics3d_headless \
            build/linux-gcc-release/samples/physics3d_headless/sample_physics3d_headless
}

stage_determinism() {
    # Reuses the gcc debug/release trees (tests stay ON so it doesn't thrash the test stages' caches).
    configure linux-gcc-debug &&
        cmake --build --preset linux-gcc-debug --target sample_02_boids sample_physics2d_headless mce_devhost modtest_good sample_03_platformer sample_04_diorama mce_player scene_demo_content stream_demo_content &&
        configure linux-gcc-release &&
        cmake --build --preset linux-gcc-release --target sample_02_boids sample_physics2d_headless mce_devhost modtest_good sample_03_platformer sample_04_diorama mce_player &&
        python3 scripts/ci/determinism.py --runs 3 --args "-ticks 1000 -boids 20000" \
            build/linux-gcc-debug/samples/02_boids/sample_02_boids \
            build/linux-gcc-release/samples/02_boids/sample_02_boids &&
        python3 scripts/ci/determinism.py --runs 3 --args "-ticks 2000" \
            build/linux-gcc-debug/samples/physics2d_headless/sample_physics2d_headless \
            build/linux-gcc-release/samples/physics2d_headless/sample_physics2d_headless &&
        python3 scripts/ci/determinism.py --runs 3 --args="-headless 1 -ticks 1800" \
            build/linux-gcc-debug/samples/03_platformer/sample_03_platformer \
            build/linux-gcc-release/samples/03_platformer/sample_03_platformer &&
        python3 scripts/ci/determinism.py --runs 3 --args="-headless 1 -ticks 600" \
            build/linux-gcc-debug/samples/04_diorama/sample_04_diorama \
            build/linux-gcc-release/samples/04_diorama/sample_04_diorama &&
        python3 scripts/ci/determinism.py --runs 3 \
            --args="-headless 1 -ticks 600 -pack build/linux-gcc-debug/samples/scene_demo/project/assets.mce" \
            build/linux-gcc-debug/src/player/mce_player build/linux-gcc-release/src/player/mce_player &&
        python3 scripts/ci/determinism.py --runs 3 \
            --args="-headless 1 -ticks 1500 -pack build/linux-gcc-debug/samples/stream_demo/project/assets.mce -scene scenes/world.mcscene" \
            build/linux-gcc-debug/src/player/mce_player build/linux-gcc-release/src/player/mce_player &&
        stage_module_determinism &&
        stage_physics3d_determinism &&
        stage_cubes_determinism &&
        stage_court_determinism
}

# M5a: 04_cubes (cascade fit, Forward+ clustering, node animation) across runs and Debug/Release;
# M5b: the same with the capsule walk (World3D state + the swept camera boom in the hash).
stage_cubes_determinism() {
    cmake --build --preset linux-gcc-debug --target sample_04_cubes &&
        cmake --build --preset linux-gcc-release --target sample_04_cubes &&
        python3 scripts/ci/determinism.py --runs 3 --args="-headless 1 -ticks 600" \
            build/linux-gcc-debug/samples/04_cubes/sample_04_cubes \
            build/linux-gcc-release/samples/04_cubes/sample_04_cubes &&
        python3 scripts/ci/determinism.py --runs 3 --args="-headless 1 -walk 1 -ticks 600" \
            build/linux-gcc-debug/samples/04_cubes/sample_04_cubes \
            build/linux-gcc-release/samples/04_cubes/sample_04_cubes
}

# M7: Ashen Court with the bot playing (enemy FSMs, combat, World3D characters, the swept boom,
# 3D audio commands) across runs and Debug/Release; 3000 ticks covers a cleared court and a restart.
stage_court_determinism() {
    cmake --build --preset linux-gcc-debug --target sample_05_ashen_court &&
        cmake --build --preset linux-gcc-release --target sample_05_ashen_court &&
        python3 scripts/ci/determinism.py --runs 3 --args="-headless 1 -ticks 3000" \
            build/linux-gcc-debug/samples/05_ashen_court/sample_05_ashen_court \
            build/linux-gcc-release/samples/05_ashen_court/sample_05_ashen_court
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

restore_cache
[[ ${#COMMON_ARGS[@]} -gt 0 ]] && ccache --zero-stats > /dev/null
preconfigure

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
if [[ ${#COMMON_ARGS[@]} -gt 0 ]]; then
    ccache --show-stats 2> /dev/null | grep -E '^(Cacheable calls|  Hits|  Misses)' | awk '!seen[$0]++' |
        sed 's/^/  ccache: /'
    [[ $failed -eq 0 ]] && save_cache_snapshot
fi
exit $failed
