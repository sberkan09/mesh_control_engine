#!/usr/bin/env bash
# Local CI: the checks the old GitHub Actions workflow ran, for running on a Linux box before merging.
#
#   scripts/ci/local_ci.sh            # everything: walls, 3 Linux presets, clang-tidy, determinism
#   scripts/ci/local_ci.sh --quick    # walls + linux-gcc-debug (with GPU tests)
#   scripts/ci/local_ci.sh --tidy-all # everything, with clang-tidy over the whole tree
#   scripts/ci/local_ci.sh --task     # after each task: walls + linux-gcc-debug + tidy (+ determinism, below)
#   scripts/ci/local_ci.sh --premerge # right before merging, if dev_claude moved: walls + the PR's own tests (below)
#   scripts/ci/local_ci.sh --batch    # the full matrix; the integration runner's job, threads do not run it
#   scripts/ci/local_ci.sh walls tidy linux-asan   # pick stages
#   scripts/ci/local_ci.sh regen      # integration runner only: regenerate the generated files (below)
#   scripts/ci/local_ci.sh --save-cache-only   # integration runner only: refresh the ccache snapshot
#
# regen builds mce_mcp in the gcc debug tree and runs scripts/ci/regen_generated.py: capabilities.md, the sample
# coverage allow list and the transcript golden. scripts/ci/integration_run.sh commits the result; threads do not.
#
# --premerge (the M7 gate, ~1-3 min) is for after dev_claude was merged into the branch: a PR goes green with ONE
# --task on its own base and is not re-run. It runs walls (capabilities --check included), then builds mce_mcp
# and the tests defined next to the files the PR changed (scripts/ci/premerge.py tests) and runs those plus the
# MCP transcript golden (`ctest -R`). It also prints whether dev_claude changed files the PR also changes since
# the last green --task (the base is recorded in build/local-ci-logs/last-task.env; MCE_PREMERGE_SINCE=<sha>
# overrides): only then is a --task re-run needed. Generated files that went stale after the
# merge (capabilities.md, transcript goldens, the sample coverage allow list) are the integration runner's to
# regenerate; a failure in the golden check on a file the PR did not touch is not a reason for --task.
#
# Stages: walls, linux-gcc-debug, slow, linux-clang-release, linux-asan, tidy, determinism, release-build, web.
# release-build compiles the whole gcc Release tree (the SDK's) without running it: determinism builds only
# the targets it runs. linux-gcc-release runs only when named: it is that tree plus its tests, and
# linux-clang-release runs the tests in Release.
# GPU tests run under Xvfb + Mesa llvmpipe (GL) and lavapipe (Vulkan, for mce_rhi_sdlgpu, ADR-0038), in
# linux-gcc-debug only; the other presets build them without running them.
#
# --task adds determinism when the change touches simulation code: src/engine/{core,ecs,refl,scene,kit,
# kit_novel,physics2d,physics3d,audio,input,net,monetize}, or a changed file with MCE_COMPONENT or state_hash.
#
# --task also adds release-build when the change touches C++ code: -O2 enables warnings (format-truncation,
# maybe-uninitialized) the Debug tree cannot raise.
#
# tidy checks the translation units changed since the merge base with $MCE_TIDY_BASE (default
# origin/dev_claude), committed or not, plus the ones that include a changed header. It covers the whole
# tree with --tidy-all, when .clang-tidy changed, or when the base ref is missing.
# web (M-WEB) builds the samples for the browser and runs them in headless Chromium (scripts/web/web_ci.py:
# the smokes, the web replay hash, the itch.io zip and the cold-load budget). It is part of the full run
# and --task adds it when the change touches the web code; without the Emscripten SDK (EMSDK, or
# ~/emsdk/emsdk_env.sh) or Playwright it reports a skip and passes.
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
# Stages always run in a fixed order (stages that share a build tree together). When the disk runs short
# (a project container's allowance is about 40 GB; a tree is 8-15 GB) the trees no later stage needs are
# deleted before the next stage; MCE_CI_MIN_FREE_GB (22) and MCE_CI_HARD_MIN_GB (3) tune it.
#
# Exit code 0 only if every selected stage passed. A summary table is printed at the end.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

export LIBGL_ALWAYS_SOFTWARE=1  # Mesa llvmpipe: GL 4.5 core without a GPU

ALL_STAGES=(walls linux-gcc-debug slow linux-clang-release linux-asan tidy determinism release-build web)
declare -A GPU_TESTS=([linux-gcc-debug]=ON [linux-clang-release]=OFF [linux-gcc-release]=OFF [linux-asan]=OFF)

stages=()
save_cache=0
save_only=0
tidy_all=0
task_run=0
for arg in "$@"; do
    case "$arg" in
        --quick) stages+=(walls linux-gcc-debug) ;;
        --task) stages+=(walls linux-gcc-debug tidy); task_run=1 ;;
        --premerge) stages+=(walls premerge) ;;
        --batch) ;;
        --save-cache) save_cache=1 ;;
        --save-cache-only) save_cache=1; save_only=1 ;;
        --tidy-all) tidy_all=1 ;;
        -h|--help) sed -n '2,52p' "$0"; exit 0 ;;
        *) stages+=("$arg") ;;
    esac
done
full_run=0
[[ ${#stages[@]} -eq 0 && $save_only -eq 0 ]] && { stages=("${ALL_STAGES[@]}"); full_run=1; }

# Files changed since the merge base with $MCE_TIDY_BASE (default origin/dev_claude), committed or
# not; prints nothing when the base ref is missing.
changed_files() {
    local mb
    mb="$(git merge-base HEAD "${MCE_TIDY_BASE:-origin/dev_claude}" 2>/dev/null)" || return 0
    { git diff --name-only "$mb"; git ls-files --others --exclude-standard; } | sort -u
}

# --task: a change to simulation code (anything that can reach World::state_hash) also runs determinism.
touches_sim() {
    local f
    while IFS= read -r f; do
        case "$f" in
            src/engine/core/*|src/engine/ecs/*|src/engine/refl/*|src/engine/scene/*|src/engine/kit/*|\
                src/engine/kit_novel/*|src/engine/physics2d/*|src/engine/physics3d/*|src/engine/audio/*|\
                src/engine/input/*|src/engine/net/*|src/engine/monetize/*) return 0 ;;
        esac
        [[ -f "$f" && "$f" =~ \.(h|hpp|cpp|cc)$ ]] && grep -qE 'MCE_COMPONENT|state_hash' "$f" && return 0
    done < <(changed_files)
    return 1
}
if [[ $task_run -eq 1 ]] && touches_sim; then
    echo "=== --task: the change touches simulation code, determinism runs too"
    stages+=(determinism)
fi

# --task: a change to C++ code also builds the gcc Release tree, where -O2 raises warnings Debug does not.
touches_cpp() {
    local f
    while IFS= read -r f; do
        [[ "$f" =~ ^(src|samples|tests|projects|tools)/.*\.(h|hpp|cpp|cc|c)$ ]] && return 0
    done < <(changed_files)
    return 1
}
if [[ $task_run -eq 1 ]] && touches_cpp; then
    echo "=== --task: the change touches C++ code, the gcc Release build runs too"
    stages+=(release-build)
fi

# --task: a change to the web build (the WebGPU backend, the web glue, the shell and its scripts) also runs web.
touches_web() {
    local f
    while IFS= read -r f; do
        case "$f" in
            src/engine/rhi_webgpu/*|scripts/web/*|tests/web/*|*/web_probe/*|src/engine/core/src/web.cpp|\
                src/engine/core/include/mce/web.h|cmake/MceWeb.cmake|cmake/web/*|CMakePresets.json) return 0 ;;
        esac
    done < <(changed_files)
    return 1
}
if [[ $task_run -eq 1 ]] && touches_web; then
    echo "=== --task: the change touches the web build, web runs too"
    stages+=(web)
fi

LOG_DIR="$ROOT/build/local-ci-logs"
mkdir -p "$LOG_DIR"

# Stages run in this order whatever order they were named or added in: the ones that share a build tree
# sit together (the gcc trees: tests, determinism, the Release build), so a tree is finished with before
# the next one starts and can be dropped when the disk is short (reclaim_space). Unknown names go last
# and fail in run_stage with the list of known ones.
CANON_ORDER=(walls regen premerge linux-gcc-debug slow determinism release-build linux-gcc-release linux-clang-release linux-asan tidy web)
order_stages() {
    local -A seen=()
    local c s sorted=()
    for c in "${CANON_ORDER[@]}"; do
        for s in "${stages[@]}"; do
            [[ "$s" == "$c" && -z "${seen[$s]:-}" ]] && { sorted+=("$s"); seen[$s]=1; }
        done
    done
    for s in "${stages[@]}"; do [[ -z "${seen[$s]:-}" ]] && { sorted+=("$s"); seen[$s]=1; }; done
    stages=("${sorted[@]}")
}
order_stages

# The build trees (build/<preset>) a stage uses.
stage_trees() {
    case "$1" in
        linux-*) echo "$1" ;;
        premerge|regen|slow) echo linux-gcc-debug ;;
        tidy) echo linux-clang-debug ;;
        determinism) echo "linux-gcc-debug linux-gcc-release" ;;
        release-build) echo linux-gcc-release ;;
    esac
}

# Disk: a project container's writable space is a fixed allowance (about 40 GB), and one tree is 8-15 GB,
# so a full run cannot keep them all. Before each stage, when less than MCE_CI_MIN_FREE_GB (default 22: the ASan tree is 18 GB and its sdk_install test copies binaries, a
# Debug or Release tree) is free, the build trees no later stage needs are deleted, largest first, until
# there is room, then, if still short, the ones only later stages need (the ccache survives, so they are
# rebuilt from cache). With less than
# MCE_CI_HARD_MIN_GB (default 3) free after that the stage is not started and says so. A run with plenty
# of room deletes nothing.
MIN_FREE_GB="${MCE_CI_MIN_FREE_GB:-22}"
HARD_MIN_GB="${MCE_CI_HARD_MIN_GB:-3}"
free_gb() { df -BG --output=avail "$ROOT/build" 2>/dev/null | tail -1 | tr -dc '0-9'; }
reclaim_space() {  # $1: index in stages of the stage about to run; returns 1 when there is no room
    local free i t d
    free="$(free_gb)"
    [[ -z "$free" || "$free" -ge "$MIN_FREE_GB" ]] && return 0
    local -A needed=()
    for ((i = $1; i < ${#stages[@]}; i++)); do
        for t in $(stage_trees "${stages[$i]}"); do needed[$t]=1; done
    done
    while IFS= read -r d; do
        t="$(basename "$d")"
        [[ -n "${needed[$t]:-}" ]] && continue
        echo "=== disk: ${free} GB free (want ${MIN_FREE_GB}); removing the finished build tree build/$t ($(du -sh "$d" | cut -f1))"
        rm -rf "$d"
        free="$(free_gb)"
        [[ "$free" -ge "$MIN_FREE_GB" ]] && return 0
    done < <(du -s "$ROOT"/build/linux-*/ 2>/dev/null | sort -rn | cut -f2-)
    # Still short: a tree only a later stage needs, left over from an earlier run, costs less to rebuild
    # (the ccache has it) than the stage about to run costs to fail.
    local -A now=()
    for t in $(stage_trees "${stages[$1]}"); do now[$t]=1; done
    while IFS= read -r d; do
        t="$(basename "$d")"
        [[ -n "${now[$t]:-}" ]] && continue
        echo "=== disk: ${free} GB free (want ${MIN_FREE_GB}); removing build/$t, a later stage rebuilds it ($(du -sh "$d" | cut -f1))"
        rm -rf "$d"
        free="$(free_gb)"
        [[ "$free" -ge "$MIN_FREE_GB" ]] && return 0
    done < <(du -s "$ROOT"/build/linux-*/ 2>/dev/null | sort -rn | cut -f2-)
    if [[ "$free" -lt "$HARD_MIN_GB" ]]; then
        echo "=== disk: only ${free} GB free and no finished build tree is left to delete; free space or run fewer stages at once"
        return 1
    fi
    echo "=== disk: ${free} GB free, less than the ${MIN_FREE_GB} GB wanted; going on"
}

JOBS="$(nproc 2>/dev/null || echo 4)"

# Extra configure arguments shared by every preset. The linux presets set -gz themselves (CMakePresets.json
# linux-base: debug sections compressed, a gcc-debug tree 13 GB -> 8.2 GB, claude_communication R2), so a tree
# configured by hand with `cmake --preset` and one configured here agree. Changing the flags invalidates every
# cached object once: the first run after this lands is cold and the next snapshot is rebuilt.
COMMON_ARGS=()
SNAPSHOT=""
HAVE_CCACHE=0
if command -v ccache > /dev/null; then
    HAVE_CCACHE=1
    export CCACHE_BASEDIR="$ROOT" CCACHE_NOHASHDIR=1  # hits survive a different checkout path
    export CCACHE_DIR="${CCACHE_DIR:-$HOME/.cache/ccache}" CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-3G}"
    # Without these ccache cannot cache a TU that uses a precompiled header (SDL3's): 1308 of 4665 calls were
    # uncacheable, a gcc-debug build with a full cache took 700 s; with them it hits 100 % and takes 110 s
    # (claude_communication R1).
    export CCACHE_SLOPPINESS="${CCACHE_SLOPPINESS:-pch_defines,time_macros,include_file_mtime,include_file_ctime}"
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
            premerge|regen|slow) want[linux-gcc-debug]=1 ;;
            tidy) want[linux-clang-debug]=1 ;;
            determinism) want[linux-gcc-debug]=1; want[linux-gcc-release]=1 ;;
            release-build) want[linux-gcc-release]=1 ;;
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

# slow (M4): the tests labelled `slow` (over 60 s: the full versions of tests that ship a smoke variant for --task),
# in the gcc debug tree. Part of the full run, i.e. the integration run; threads' --task skips them.
stage_slow() {
    configure linux-gcc-debug &&
        cmake --build --preset linux-gcc-debug &&
        xvfb-run -a -s "-screen 0 1280x1024x24 -noreset" ctest --preset linux-gcc-debug-slow -j "$JOBS"
}

# regen: the generated files, rebuilt from the code (integration runner). mce_mcp is the transcript golden's server.
stage_regen() {
    configure linux-gcc-debug &&
        cmake --build --preset linux-gcc-debug --target mce_mcp &&
        python3 scripts/ci/regen_generated.py --exe build/linux-gcc-debug/src/editor/mcp/mce_mcp
}

# --premerge: report whether a --task re-run is needed, build the PR's own tests and run them with the
# transcript golden. An overlap makes the stage fail so the summary says so.
stage_premerge() {
    local since="${MCE_PREMERGE_SINCE:-}" rc=0 out regex targets full
    python3 scripts/ci/premerge.py overlap ${since:+--since "$since"} || rc=$?
    out="$(python3 scripts/ci/premerge.py tests)"
    regex="$(sed -n 's/^REGEX=//p' <<< "$out")"
    targets="$(sed -n 's/^TARGETS=//p' <<< "$out")"
    full="$(sed -n 's/^FULL_BUILD=//p' <<< "$out")"
    echo "premerge: tests $regex"
    configure linux-gcc-debug || return 1
    if [[ "$full" == 1 ]]; then
        cmake --build --preset linux-gcc-debug || return 1
    else
        # shellcheck disable=SC2086
        cmake --build --preset linux-gcc-debug --target $targets || return 1
    fi
    xvfb-run -a -s "-screen 0 1280x1024x24 -noreset" ctest --preset linux-gcc-debug -j "$JOBS" -R "$regex" || return 1
    [[ $rc -eq 0 ]] && return 0
    echo "premerge: tests pass, but a --task re-run is needed (see the overlap above, or no --task base is recorded)"
    return 1
}

# Prints the run-clang-tidy file regexes: one per changed translation unit, or the whole tree.
tidy_targets() {
    local f h
    local -A tus=()
    if [[ $tidy_all -eq 1 ]] || ! git merge-base HEAD "${MCE_TIDY_BASE:-origin/dev_claude}" > /dev/null 2>&1; then
        echo "^$ROOT/(src|samples|tests)/"; return
    fi
    local changed
    changed="$(changed_files)"
    if grep -qx '.clang-tidy' <<< "$changed"; then echo "^$ROOT/(src|samples|tests)/"; return; fi
    while IFS= read -r f; do
        [[ -f "$f" ]] || continue
        case "$f" in
            src/*.cpp|samples/*.cpp|tests/*.cpp|src/*.cc|samples/*.cc|tests/*.cc) tus[$f]=1 ;;
            src/*.h|samples/*.h|tests/*.h|src/*.hpp|samples/*.hpp|tests/*.hpp)
                h="$(basename "$f")"
                while IFS= read -r t; do tus[$t]=1; done < <(
                    grep -rlE --include='*.cpp' --include='*.cc' "#include [<\"]([^<>\"]*/)?${h//./\\.}[>\"]" src samples tests) ;;
        esac
    done <<< "$changed"
    for f in "${!tus[@]}"; do echo "^$ROOT/${f//./\\.}\$"; done
}

stage_tidy() {
    local targets
    mapfile -t targets < <(tidy_targets)
    if [[ ${#targets[@]} -eq 0 ]]; then
        echo "tidy: no C++ translation unit changed since the merge base, nothing to check"
        return 0
    fi
    echo "tidy: ${#targets[@]} target(s): ${targets[*]}"
    configure linux-clang-debug &&
        cmake --build --preset linux-clang-debug &&
        run-clang-tidy-18 -p build/linux-clang-debug -quiet "${targets[@]}"
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

# The M-AGENT demo games (docs/research/m-agent-demo/): Glowmoth's second level idle (bats and
# lanterns) and Marble Garden's scripted win route (3D physics, five pickups, a scene change).
stage_demo_games_determinism() {
    cmake --build --preset linux-gcc-debug --target glowmoth marble_garden &&
        cmake --build --preset linux-gcc-release --target glowmoth marble_garden &&
        python3 scripts/ci/determinism.py --runs 3 --args="-headless 1 -ticks 1200 -scene 0c5b255e-286f-4293-a352-48dcb9c80653" \
            build/linux-gcc-debug/projects/Glowmoth/glowmoth build/linux-gcc-release/projects/Glowmoth/glowmoth &&
        python3 scripts/ci/determinism.py --runs 3 \
            --args="-headless 1 -ticks 1800 -scene 5ebadc24-96f4-438c-a342-d6f37fb5ca8c -input projects/MarbleGarden/tests/win_route.input" \
            build/linux-gcc-debug/projects/MarbleGarden/marble_garden build/linux-gcc-release/projects/MarbleGarden/marble_garden
}

# Every target of the gcc Release tree, tests included, compiled but not run. The determinism stage builds
# only the targets it runs, so a -Werror that fires in the editor, the MCP server or a sample in Release
# (gcc 13's -Wformat-truncation did, #359) passed every earlier stage; the SDK build (release_sdk.sh) is
# this tree.
stage_release_build() {
    configure linux-gcc-release &&
        cmake --build --preset linux-gcc-release
}

stage_determinism() {
    # Reuses the gcc debug/release trees (tests stay ON so it doesn't thrash the test stages' caches).
    configure linux-gcc-debug &&
        cmake --build --preset linux-gcc-debug --target sample_02_boids sample_physics2d_headless mce_devhost modtest_good sample_03_platformer sample_04_diorama mce_player scene_demo_content stream_demo_content gameplay3d_content sample_shop_demo sample_liveops_demo sample_net_headless sample_net_rollback sample_net_arena &&
        configure linux-gcc-release &&
        cmake --build --preset linux-gcc-release --target sample_02_boids sample_physics2d_headless mce_devhost modtest_good sample_03_platformer sample_04_diorama mce_player sample_shop_demo sample_liveops_demo sample_net_headless sample_net_rollback sample_net_arena &&
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
        python3 scripts/ci/determinism.py --runs 3 \
            --args="-headless 1 -ticks 600 -pack build/linux-gcc-debug/samples/07_gameplay3d/project/assets.mce -input samples/07_gameplay3d/drive.input" \
            build/linux-gcc-debug/src/player/mce_player build/linux-gcc-release/src/player/mce_player &&
        python3 scripts/ci/determinism.py --runs 3 --args "-ticks 1850" \
            build/linux-gcc-debug/samples/shop_demo/sample_shop_demo \
            build/linux-gcc-release/samples/shop_demo/sample_shop_demo &&
        python3 scripts/ci/determinism.py --runs 3 --args "-ticks 1850" \
            build/linux-gcc-debug/samples/liveops_demo/sample_liveops_demo \
            build/linux-gcc-release/samples/liveops_demo/sample_liveops_demo &&
        stage_net_determinism &&
        stage_module_determinism &&
        stage_physics3d_determinism &&
        stage_cubes_determinism &&
        stage_court_determinism &&
        stage_demo_games_determinism &&
        stage_script_determinism
}

# ADR-0041 spike: the Luau system hashes the same across runs and Debug/Release, and the same as
# its C++ twin (`-mode cpp`), so the VM adds no float drift of its own.
stage_script_determinism() {
    cmake --build --preset linux-gcc-debug --target sample_script_headless ember_script &&
        cmake --build --preset linux-gcc-release --target sample_script_headless ember_script &&
        python3 scripts/ci/determinism.py --runs 3 \
            --args="-headless 1 -ticks 1800 -input projects/EmberScript/tests/run_right.input" \
            build/linux-gcc-debug/projects/EmberScript/ember_script \
            build/linux-gcc-release/projects/EmberScript/ember_script &&
        python3 scripts/ci/determinism.py --runs 3 --args "-ticks 1200" \
            build/linux-gcc-debug/samples/script_headless/sample_script_headless \
            build/linux-gcc-release/samples/script_headless/sample_script_headless &&
        python3 scripts/ci/determinism.py --runs 1 --args "-ticks 1200 -mode cpp" \
            build/linux-gcc-debug/samples/script_headless/sample_script_headless \
            build/linux-gcc-release/samples/script_headless/sample_script_headless &&
        [[ "$(build/linux-gcc-release/samples/script_headless/sample_script_headless -ticks 1200 | grep -o 'hash [0-9a-f]*')" == \
           "$(build/linux-gcc-release/samples/script_headless/sample_script_headless -ticks 1200 -mode cpp | grep -o 'hash [0-9a-f]*')" ]]
}

# M-NET2 step 1: a server and its clients over SimNetwork (simulated latency, jitter and loss on a
# virtual clock): every client mirrors the server, and the hash is the same across runs and builds.
# M-NET2 step 5: rollback peers (and lockstep) over SimNetwork agree on every confirmed frame, and
# rollbacks, stalls and the confirmed state hash are the same across runs and builds.
# M-NET2 step 6: the same session through a relay room (net::RelayServer / RelayClient).
stage_net_determinism() {
    python3 scripts/ci/determinism.py --runs 3 --args "-sim 1 -clients 4 -latency 75 -jitter 20 -loss 0.05 -ticks 600" \
        build/linux-gcc-debug/samples/net_headless/sample_net_headless \
        build/linux-gcc-release/samples/net_headless/sample_net_headless &&
        python3 scripts/ci/determinism.py --runs 3 --args "-sim 1 -clients 8 -latency 150 -jitter 40 -loss 0.15 -seed 3 -ticks 900" \
            build/linux-gcc-debug/samples/net_headless/sample_net_headless \
            build/linux-gcc-release/samples/net_headless/sample_net_headless &&
        python3 scripts/ci/determinism.py --runs 3 --args "-sim 1 -relay 1 -clients 3 -latency 40 -jitter 10 -loss 0.05 -ticks 600" \
            build/linux-gcc-debug/samples/net_headless/sample_net_headless \
            build/linux-gcc-release/samples/net_headless/sample_net_headless &&
        python3 scripts/ci/determinism.py --runs 3 --args "-peers 4 -latency 100 -jitter 20 -loss 0.05 -frames 900" \
            build/linux-gcc-debug/samples/net_rollback/sample_net_rollback \
            build/linux-gcc-release/samples/net_rollback/sample_net_rollback &&
        python3 scripts/ci/determinism.py --runs 3 --args "-peers 2 -prediction 0 -frames 300" \
            build/linux-gcc-debug/samples/net_rollback/sample_net_rollback \
            build/linux-gcc-release/samples/net_rollback/sample_net_rollback &&
        python3 scripts/ci/determinism.py --runs 3 --args="-headless 1 -clients 4 -latency 150 -jitter 20 -loss 0.03 -ticks 900" \
            build/linux-gcc-debug/samples/net_arena/sample_net_arena \
            build/linux-gcc-release/samples/net_arena/sample_net_arena &&
        python3 scripts/ci/determinism.py --runs 3 --args="-headless 1 -clients 3 -latency 60 -jitter 10 -loss 0.1 -seed 9 -ticks 600" \
            build/linux-gcc-debug/samples/net_arena/sample_net_arena \
            build/linux-gcc-release/samples/net_arena/sample_net_arena
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

# M-WEB step 7: scripts/web/web_ci.py, when this box has the Emscripten SDK and Playwright.
stage_web() {
    if [[ -z "${EMSDK:-}" && -f "$HOME/emsdk/emsdk_env.sh" ]]; then
        # shellcheck disable=SC1091
        source "$HOME/emsdk/emsdk_env.sh" > /dev/null 2>&1
    fi
    if [[ -z "${EMSDK:-}" ]] || ! command -v emcc > /dev/null; then
        echo "web: SKIPPED, no Emscripten SDK (source <emsdk>/emsdk_env.sh)"; return 0
    fi
    [[ -n "${NODE_PATH:-}" ]] || export NODE_PATH="$(npm root -g 2> /dev/null)"
    if ! node -e 'require("playwright")' > /dev/null 2>&1; then
        echo "web: SKIPPED, no Playwright for node (npm i -g playwright)"; return 0
    fi
    python3 scripts/web/web_ci.py
}

run_stage() {
    case "$1" in
        walls) python3 scripts/ci/check_walls.py && python3 scripts/bump_version.py --check && python3 scripts/ci/capabilities.py --check && python3 scripts/ci/check_docs.py ;;
        linux-*) [[ -n "${GPU_TESTS[$1]:-}" ]] || { echo "unknown preset stage: $1"; return 2; }; stage_preset "$1" ;;
        tidy) stage_tidy ;;
        determinism) stage_determinism ;;
        release-build) stage_release_build ;;
        web) stage_web ;;
        premerge) stage_premerge ;;
        regen) stage_regen ;;
        slow) stage_slow ;;
        *) echo "unknown stage: $1 (known: ${ALL_STAGES[*]} premerge regen)"; return 2 ;;
    esac
}

restore_cache
if [[ $save_only -eq 1 ]]; then save_cache_snapshot; exit 0; fi
[[ $HAVE_CCACHE -eq 1 ]] && ccache --zero-stats > /dev/null
preconfigure

declare -A RESULT
failed=0
for ((si = 0; si < ${#stages[@]}; si++)); do
    s="${stages[$si]}"
    echo "=== $s"
    start=$SECONDS
    if ! reclaim_space "$si"; then
        RESULT[$s]="FAIL (0s) — no disk space, see above"
        failed=1
    elif run_stage "$s" > "$LOG_DIR/$s.log" 2>&1; then
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
if [[ $HAVE_CCACHE -eq 1 ]]; then
    ccache --show-stats 2> /dev/null | grep -E '^(Cacheable calls|  Hits|  Misses)' | awk '!seen[$0]++' |
        sed 's/^/  ccache: /'
    [[ $failed -eq 0 ]] && save_cache_snapshot
fi
# A green --task records the base it ran on, so --premerge can tell later whether dev_claude moved onto the
# PR's files (scripts/ci/premerge.py overlap).
if [[ $task_run -eq 1 && $failed -eq 0 ]]; then
    printf 'BASE=%s\nHEAD=%s\n' "$(git merge-base HEAD "${MCE_TIDY_BASE:-origin/dev_claude}" 2>/dev/null)" \
        "$(git rev-parse HEAD)" > "$LOG_DIR/last-task.env"
fi
exit $failed
