#!/usr/bin/env bash
# Local CI: the checks the old GitHub Actions workflow ran, for running on a Linux box before merging.
#
#   scripts/ci/local_ci.sh            # everything: walls, 3 Linux presets, clang-tidy, determinism
#   scripts/ci/local_ci.sh --quick    # walls + linux-gcc-debug (with GPU tests)
#   scripts/ci/local_ci.sh --tidy-all # everything, with clang-tidy over the whole tree
#   scripts/ci/local_ci.sh --task     # before a task's PR: walls + linux-gcc-debug + tidy, scoped to the change (below)
#   scripts/ci/local_ci.sh --premerge # right before merging, if dev_claude moved: walls + the PR's own tests (below)
#   scripts/ci/local_ci.sh --batch    # the full matrix; the integration runner's job, threads do not run it
#   scripts/ci/local_ci.sh walls tidy linux-asan   # pick stages (a stage named here always runs in full)
#   scripts/ci/local_ci.sh regen      # integration runner only: regenerate the generated files (below)
#   scripts/ci/local_ci.sh --save-cache-only   # integration runner only: refresh the ccache snapshot
#   scripts/ci/local_ci.sh --task --resume     # after the container restarted mid-run: skip the stages that passed
#
# --resume: a stage that passed is recorded (build/local-ci-logs/passed/<stage>) with a key of HEAD, the working
# tree diff and the run's arguments; a later run with --resume and the same key skips it. A project container
# restarts every 30-40 min and kills the run (claude_communication, 2026-10-16), so a long --task is run as
# `--task`, then `--task --resume` after each restart; the build trees and the ccache survive on disk.
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
# --task scopes every stage to the files changed since the merge base with $MCE_TIDY_BASE (default
# origin/dev_claude), committed or not (`git fetch origin dev_claude` first: with no base ref everything runs in
# full). A stage named on the command line is never scoped.
#   tests:   the `gpu` label (68 tests, ~80 % of the ctest time under llvmpipe) is skipped only when every
#            changed file is GPU-irrelevant (docs, scripts outside scripts/ci, src/engine/{net,monetize,
#            liveops,loc} and their tests). A change confined to simulation modules (core, ecs, refl, scene,
#            physics*, script, audio, input) runs the GPU smoke subset ($MCE_CI_GPU_SMOKE) instead of all 68.
#            Anything else runs them all. The `sdk` label (sdk_install, ~95 s) runs only when cmake/,
#            install.sh, release_sdk.sh, tests/sdk or a CMakeLists.txt changed. `slow` never runs here.
#   tidy:    the translation units changed, plus the ones that include a changed header, compiled as objects
#            in the clang tree (ninja builds exactly their prerequisites, nothing else), then clang-tidy.
#            The whole tree with --tidy-all, when .clang-tidy changed, or when the base ref is missing.
#   release-build (added when C++ changed): the same translation units compiled as objects in the gcc
#            Release tree: -O2 enables warnings (format-truncation, maybe-uninitialized) the Debug tree
#            cannot raise, and they fire at compile time, so nothing is linked. The whole tree when a
#            CMake file changed or more than $MCE_CI_MAX_TARGETED_TUS (40) units are affected.
#   determinism (added when the change touches simulation code: src/engine/{core,ecs,refl,scene,kit,
#            kit_novel,physics2d,physics3d,audio,input,net,monetize,script,module,render,rhi*,asset}, or a changed file with
#            MCE_COMPONENT or state_hash): only the subjects the changed modules can reach (det_subjects_of
#            below), built in both gcc trees; every subject for core/ecs/refl/audio/input, an unmapped engine
#            module, or a CMake change.
#   web (added when the change touches the web code): scripts/web/web_ci.py (M-WEB: the browser samples in
#            headless Chromium, the web replay hash, the itch.io zip, the cold-load budget); without the
#            Emscripten SDK (EMSDK, or ~/emsdk/emsdk_env.sh) or Playwright it reports a skip and passes.
# Windows/MSVC presets are not covered; build them on a Windows machine when a change touches
# platform-specific code.
#
# Ubuntu 24.04 deps: scripts/ci/setup_env.sh installs them (and ccache, zstd). Run it first.
#
# Speed: compiles go through ccache when it is installed; third-party sources are fetched once into
# build/_fetch/<hash of cmake/Dependencies.cmake> and shared by every preset; the build trees are
# configured in parallel before the first stage; tests run in parallel.
#
# In a project container (/mnt/project-files exists) the ccache directory is restored from a shared
# snapshot before the first stage, so a fresh session starts warm, and saved back after a green
# linux-gcc-debug stage (its full Debug build is what a fresh session needs most) when the snapshot is
# missing or older than $MCE_CCACHE_SNAPSHOT_MAX_AGE_H (3) hours, or with --save-cache.
# MCE_CCACHE_SNAPSHOT overrides the snapshot path; an empty value turns the snapshot off.
#
# Stages always run in a fixed order (stages that share a build tree together). When the disk runs short
# (a project container's allowance is about 40 GB; a tree is 8-16 GB) the trees no later stage needs are
# deleted before the next stage; MCE_CI_MIN_FREE_GB (22) and MCE_CI_HARD_MIN_GB (3) tune it.
#
# Exit code 0 only if every selected stage passed. Each stage's log is build/local-ci-logs/<stage>.log (the
# previous run's is kept as <stage>.prev.log, so a run killed by a restart does not erase the evidence of the
# last one). A summary table is printed at the end, with the
# configure / build / test split of a preset stage and the ccache hit rate of every stage.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

export LIBGL_ALWAYS_SOFTWARE=1  # Mesa llvmpipe: GL 4.5 core without a GPU

ALL_STAGES=(walls linux-gcc-debug slow linux-clang-release linux-asan tidy determinism release-build web)
declare -A GPU_TESTS=([linux-gcc-debug]=ON [linux-clang-release]=OFF [linux-gcc-release]=OFF [linux-asan]=OFF)

# The GPU tests a simulation-only change still runs under --task: one 2D and three 3D samples plus the game
# route that drives physics3d + scene + script through the player. The owner of a module can widen it.
GPU_SMOKE="${MCE_CI_GPU_SMOKE:-^(sample_03_platformer_gpu|sample_04_diorama_gpu|sample_04_cubes_gpu|sample_05_ashen_court_gpu|marble_garden_win_route_gpu)\$}"
MAX_TARGETED_TUS="${MCE_CI_MAX_TARGETED_TUS:-40}"

stages=()
save_cache=0
save_only=0
tidy_all=0
task_run=0
resume=0   # --resume: skip the stages that already passed for this exact change
declare -A named=()  # stages the command line named: never scoped
for arg in "$@"; do
    case "$arg" in
        --quick) stages+=(walls linux-gcc-debug) ;;
        --task) stages+=(walls linux-gcc-debug tidy); task_run=1 ;;
        --premerge) stages+=(walls premerge) ;;
        --batch) ;;
        --save-cache) save_cache=1 ;;
        --save-cache-only) save_cache=1; save_only=1 ;;
        --tidy-all) tidy_all=1 ;;
        --resume) resume=1 ;;
        -h|--help) sed -n '2,84p' "$0"; exit 0 ;;
        *) stages+=("$arg"); named[$arg]=1 ;;
    esac
done
full_run=0
[[ ${#stages[@]} -eq 0 && $save_only -eq 0 ]] && { stages=("${ALL_STAGES[@]}"); full_run=1; }

# ---------------------------------------------------------------------------------------------------
# The change: files changed since the merge base with $MCE_TIDY_BASE, committed or not.

have_base() { git merge-base HEAD "${MCE_TIDY_BASE:-origin/dev_claude}" > /dev/null 2>&1; }

# Prints nothing when the base ref is missing (callers then fall back to "everything").
changed_files() {
    local mb
    mb="$(git merge-base HEAD "${MCE_TIDY_BASE:-origin/dev_claude}" 2>/dev/null)" || return 0
    { git diff --name-only "$mb"; git ls-files --others --exclude-standard; } | sort -u
}
CHANGED="$(changed_files)"

# A changed C++ file (headers included) that still exists.
changed_cpp_files() {
    local f
    while IFS= read -r f; do
        [[ -n "$f" && -f "$f" && "$f" =~ \.(h|hpp|cpp|cc|c)$ ]] && echo "$f"
    done <<< "$CHANGED"
}

# A file with MCE_COMPONENT or state_hash can reach World::state_hash wherever it lives.
hashes_state() { [[ -f "$1" && "$1" =~ \.(h|hpp|cpp|cc)$ ]] && grep -qE 'MCE_COMPONENT|state_hash' "$1"; }

# --task: a change to simulation code (anything that can reach World::state_hash) also runs determinism.
touches_sim() {
    local f
    while IFS= read -r f; do
        case "$f" in
            src/engine/core/*|src/engine/ecs/*|src/engine/refl/*|src/engine/scene/*|src/engine/kit/*|\
                src/engine/kit_novel/*|src/engine/physics2d/*|src/engine/physics3d/*|src/engine/audio/*|\
                src/engine/input/*|src/engine/net/*|src/engine/monetize/*|src/engine/script/*|src/engine/module/*|\
                src/engine/render/*|src/engine/rhi*/*|src/engine/asset/*) return 0 ;;
        esac
        hashes_state "$f" && return 0
    done <<< "$CHANGED"
    return 1
}

# --task: a change to C++ code also compiles in the gcc Release tree, where -O2 raises warnings Debug does not.
touches_cpp() { grep -qE '^(src|samples|tests|projects|tools)/.*\.(h|hpp|cpp|cc|c)$' <<< "$CHANGED"; }

touches_cmake() { grep -qE '(^|/)CMakeLists\.txt$|^cmake/|^CMakePresets\.json$' <<< "$CHANGED"; }

# --task: a change to the web build (the WebGPU backend, the web glue, the shell and its scripts) also runs web.
touches_web() {
    grep -qE '^(src/engine/rhi_webgpu/|scripts/web/|tests/web/|.*/web_probe/|src/engine/core/src/web\.cpp$|src/engine/core/include/mce/web\.h$|cmake/MceWeb\.cmake$|cmake/web/|CMakePresets\.json$)' <<< "$CHANGED"
}

# Which tests a scoped linux-gcc-debug stage runs. The `gpu` label is set far beyond render code (every sample
# and game, the editor, mcp, asset, kit, ui tests), so the rule is inverted: GPU tests are skipped only when
# EVERY changed file is in the allow-list of GPU-irrelevant paths; a change confined to the simulation modules
# runs the non-GPU tests plus GPU_SMOKE; anything else runs them all.
gpu_irrelevant() {
    case "$1" in
        docs/*|*.md|claude_communication/*|changelog.d/*|.clang-format|.clang-tidy|.gitignore|.mcp.json|.claude/*) return 0 ;;
        scripts/ci/*) return 1 ;;
        scripts/*) return 0 ;;
        src/engine/net/*|src/engine/monetize/*|src/engine/liveops/*|src/engine/loc/*|\
            tests/net/*|tests/monetize/*|tests/liveops/*|tests/loc/*|samples/net_*/*) return 0 ;;
    esac
    return 1
}
sim_only() {
    case "$1" in
        src/engine/core/*|src/engine/ecs/*|src/engine/refl/*|src/engine/scene/*|src/engine/physics2d/*|\
            src/engine/physics3d/*|src/engine/script/*|src/engine/audio/*|src/engine/input/*|\
            tests/core/*|tests/ecs/*|tests/refl/*|tests/scene/*|tests/physics2d/*|tests/physics3d/*|\
            tests/script/*|tests/audio/*|tests/input/*|samples/*_headless/*) return 0 ;;
    esac
    gpu_irrelevant "$1"
}
test_scope() {  # full | nongpu | smoke
    [[ $task_run -eq 1 ]] && have_base || { echo full; return; }
    [[ -z "$CHANGED" ]] && { echo nongpu; return; }
    local f scope=nongpu
    while IFS= read -r f; do
        if gpu_irrelevant "$f"; then continue
        elif sim_only "$f"; then scope=smoke
        else echo full; return
        fi
    done <<< "$CHANGED"
    echo "$scope"
}
needs_sdk_tests() {
    [[ $task_run -eq 1 ]] && have_base || return 0
    grep -qE '(^|/)CMakeLists\.txt$|^cmake/|^install\.sh$|^scripts/release_sdk\.sh$|^tests/sdk/' <<< "$CHANGED"
}

# The translation units a scoped tidy / release-build compiles: the changed ones, plus the ones that
# include a changed header (by basename, anywhere under src, samples, tests, projects, tools).
changed_tus() {
    local f h t
    local -A tus=()
    while IFS= read -r f; do
        case "$f" in
            *.cpp|*.cc|*.c) tus[$f]=1 ;;
            *.h|*.hpp)
                h="$(basename "$f")"
                while IFS= read -r t; do tus[$t]=1; done < <(
                    grep -rlE --include='*.cpp' --include='*.cc' --include='*.c' \
                        "#include [<\"]([^<>\"]*/)?${h//./\\.}[>\"]" src samples tests projects tools 2>/dev/null) ;;
        esac
    done < <(changed_cpp_files)
    [[ ${#tus[@]} -gt 0 ]] && printf '%s\n' "${!tus[@]}" | grep -E '^(src|samples|tests|projects|tools)/' | sort
}

# Whether a stage compiles objects instead of the tree: prints "full", or the TU list (possibly empty).
tu_scope() {
    if [[ $task_run -eq 0 || -n "${named[$1]:-}" ]] || ! have_base || touches_cmake; then echo full; return; fi
    local tus n
    tus="$(changed_tus)"
    n=$(grep -c . <<< "$tus")
    if [[ "$n" -gt "$MAX_TARGETED_TUS" ]]; then echo full; return; fi
    printf '%s' "$tus"
}

# Resolves TUs (stdin, repo-relative) to their object files in build/<preset>, from ninja's target list:
# an object is <target dir>/CMakeFiles/<target>.dir/<source relative to that dir>.o. TUs no target
# compiles in this tree (a platform's file, a tool not built here) are reported on stderr and skipped.
tu_objects() {
    local preset="$1" tus
    tus="$(cat)"
    [[ -n "$tus" ]] || return 0
    ninja -C "build/$preset" -t targets all 2>/dev/null |
        awk -F': ' -v tus="$tus" '
            BEGIN { n = split(tus, a, "\n"); for (i = 1; i <= n; i++) if (a[i] != "") want[a[i]] = 1 }
            /\.(cpp|cc|c)\.o: / { o = $1; k = o; sub(/CMakeFiles\/[^\/]+\.dir\//, "", k); sub(/\.o$/, "", k)
                                  if (k in want) { print o; found[k] = 1 } }
            END { for (k in want) if (!(k in found)) print "    (no object for " k " in this tree)" > "/dev/stderr" }'
}

if [[ $task_run -eq 1 ]] && touches_sim; then
    echo "=== --task: the change touches simulation code, determinism runs too"
    stages+=(determinism)
fi
if [[ $task_run -eq 1 ]] && touches_cpp; then
    echo "=== --task: the change touches C++ code, the gcc Release objects compile too"
    stages+=(release-build)
fi
if [[ $task_run -eq 1 ]] && touches_web; then
    echo "=== --task: the change touches the web build, web runs too"
    stages+=(web)
fi
if [[ $task_run -eq 1 ]] && ! have_base; then
    echo "=== --task: no merge base with ${MCE_TIDY_BASE:-origin/dev_claude} (git fetch origin dev_claude); every stage runs in full"
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

# Disk: a project container's writable space is a fixed allowance (about 40 GB), and one tree is 8-18 GB,
# so a full run cannot keep them all. Before each stage, when less is free than the stage wants
# (stage_want_gb: nothing for a stage without a build tree, a few GB when its tree already exists, the size
# of a fresh tree otherwise: MCE_CI_TREE_GB (12) for a Debug/Release tree, MCE_CI_MIN_FREE_GB (22) for the
# ASan tree, whose sdk_install test copies binaries), the build trees no later stage needs are deleted,
# largest first, until there is room, then, if still short, the ones only later stages need (the ccache
# survives, so they are rebuilt from cache). With less than MCE_CI_HARD_MIN_GB (default 3) free after that
# the stage is not started and says so. A run with plenty of room deletes nothing, and a stage that needs
# no tree (walls) never deletes one: the earlier rule deleted a finished Debug tree before walls to make
# room it did not need, so every second run started cold (claude_communication C3).
MIN_FREE_GB="${MCE_CI_MIN_FREE_GB:-22}"
TREE_GB="${MCE_CI_TREE_GB:-12}"
HARD_MIN_GB="${MCE_CI_HARD_MIN_GB:-3}"
free_gb() { df -BG --output=avail "$ROOT/build" 2>/dev/null | tail -1 | tr -dc '0-9'; }
stage_want_gb() {  # $1 stage
    local t n want=0
    for t in $(stage_trees "$1"); do
        if [[ -d "$ROOT/build/$t" ]]; then n=4
        elif [[ "$t" == linux-asan ]]; then n="$MIN_FREE_GB"
        elif [[ "$1" == tidy || "$1" == release-build ]] && [[ "$(tu_scope "$1")" != full ]]; then n=4  # objects, not a tree
        else n="$TREE_GB"
        fi
        (( n > want )) && want=$n
    done
    echo "$want"
}
reclaim_space() {  # $1: index in stages of the stage about to run; returns 1 when there is no room
    local free want i t d
    want="$(stage_want_gb "${stages[$1]}")"
    free="$(free_gb)"
    [[ -z "$free" || "$free" -ge "$want" ]] && return 0
    local -A needed=()
    for ((i = $1; i < ${#stages[@]}; i++)); do
        for t in $(stage_trees "${stages[$i]}"); do needed[$t]=1; done
    done
    while IFS= read -r d; do
        t="$(basename "$d")"
        [[ -n "${needed[$t]:-}" ]] && continue
        echo "=== disk: ${free} GB free (${stages[$1]} wants ${want}); removing the finished build tree build/$t ($(du -sh "$d" | cut -f1))"
        rm -rf "$d"
        free="$(free_gb)"
        [[ "$free" -ge "$want" ]] && return 0
    done < <(du -s "$ROOT"/build/linux-*/ 2>/dev/null | sort -rn | cut -f2-)
    # Still short: a tree only a later stage needs, left over from an earlier run, costs less to rebuild
    # (the ccache has it) than the stage about to run costs to fail.
    local -A now=()
    for t in $(stage_trees "${stages[$1]}"); do now[$t]=1; done
    while IFS= read -r d; do
        t="$(basename "$d")"
        [[ -n "${now[$t]:-}" ]] && continue
        echo "=== disk: ${free} GB free (${stages[$1]} wants ${want}); removing build/$t, a later stage rebuilds it ($(du -sh "$d" | cut -f1))"
        rm -rf "$d"
        free="$(free_gb)"
        [[ "$free" -ge "$want" ]] && return 0
    done < <(du -s "$ROOT"/build/linux-*/ 2>/dev/null | sort -rn | cut -f2-)
    if [[ "$free" -lt "$HARD_MIN_GB" ]]; then
        echo "=== disk: only ${free} GB free and no finished build tree is left to delete; free space or run fewer stages at once"
        return 1
    fi
    echo "=== disk: ${free} GB free, less than the ${want} GB wanted; going on"
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
    if [[ -d /mnt/project-files/ ]]; then
        if command -v zstd > /dev/null; then
            SNAPSHOT="${MCE_CCACHE_SNAPSHOT-/mnt/project-files/.cache/ccache-linux.tar.zst}"
        else
            echo "=== WARNING: zstd is missing, the shared ccache snapshot cannot be used: run scripts/ci/setup_env.sh first"
        fi
    fi
else
    echo "=== WARNING: ccache is missing, every compile runs cold (a Debug tree is ~12 min instead of ~2): run scripts/ci/setup_env.sh first"
fi

# "hits misses" so far, for the per-stage hit rate in the summary.
ccache_counts() {
    [[ $HAVE_CCACHE -eq 1 ]] || { echo "0 0"; return; }
    ccache --print-stats 2>/dev/null | awk -F'\t' '
        $1 == "direct_cache_hit" || $1 == "preprocessed_cache_hit" { h += $2 }
        $1 == "cache_miss" { m += $2 }
        END { print h + 0, m + 0 }'
}

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

# Saved after a green linux-gcc-debug stage when the snapshot is missing or older than
# MCE_CCACHE_SNAPSHOT_MAX_AGE_H hours, or with --save-cache (--save-cache-only: always). With 6-8 sessions
# running --task all day that is one save every few hours; last writer wins is fine for a tarball that is
# written locally and moved into place. (It used to wait for a green full run, which never happened: the
# snapshot was 8 days old and hit 10 %.)
save_cache_snapshot() {
    [[ -n "$SNAPSHOT" && -d "$CCACHE_DIR" ]] || return 0
    if [[ $save_only -eq 0 ]]; then
        [[ "${RESULT[linux-gcc-debug]:-}" == PASS* ]] || return 0
        local max_age=$(( ${MCE_CCACHE_SNAPSHOT_MAX_AGE_H:-3} * 3600 ))
        if [[ $save_cache -eq 0 && -f "$SNAPSHOT" && $(( $(date +%s) - $(stat -c %Y "$SNAPSHOT") )) -lt $max_age ]]; then
            return 0
        fi
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

# CMake's configure step is mostly single-threaded (~20-120 s per tree, SDL's feature checks), so
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

# ctest under Xvfb; extra arguments select tests. A -LE on the command line replaces the preset's
# `exclude slow` filter, so every exclusion here carries `slow` (mce_mark_slow, cmake/MceTarget.cmake).
ctest_xvfb() {
    xvfb-run -a -s "-screen 0 1280x1024x24 -noreset" ctest "$@"
}

# The selection runs in chunks of MCE_CI_TEST_CHUNK tests (default 60), each recorded in passed/ when it passes;
# `--resume` after a container restart skips the chunks that passed for this exact change, so a kill costs at most
# one chunk. Every chunk runs even after a failure, as one ctest would, and one failed chunk fails the call.
run_tests() {
    local preset="$1"; shift
    local nums=() n chunk=${MCE_CI_TEST_CHUNK:-60} rc=0 i ck
    mapfile -t nums < <(ctest --preset "$preset" -N "$@" 2> /dev/null | sed -n 's/^ *Test *#\([0-9]*\):.*/\1/p')
    if [[ ${#nums[@]} -le $chunk || -z "${run_key:-}" ]]; then
        ctest_xvfb --preset "$preset" -j "$JOBS" "$@"; return
    fi
    for ((i = 0; i < ${#nums[@]}; i += chunk)); do
        local sel
        sel="$(IFS=,; echo "${nums[*]:i:chunk}")"
        ck="tests-$(echo "$preset $* $sel" | sha1sum | cut -c1-12)"
        if [[ $resume -eq 1 && -f "$PASSED_DIR/$ck" && "$(cat "$PASSED_DIR/$ck")" == "$run_key" ]]; then
            echo "tests: chunk $((i / chunk + 1)) (${#nums[@]} selected) passed in an earlier run, skipped"
            continue
        fi
        if ctest_xvfb --preset "$preset" -j "$JOBS" "$@" -I "0,0,1,$sel"; then
            echo "$run_key" > "$PASSED_DIR/$ck"
        else
            rc=1
        fi
    done
    return $rc
}

# The tests of a preset: all of them for a named stage or a full run, else scoped (test_scope, needs_sdk_tests).
run_scoped_tests() {
    local preset="$1" scope ex="slow"
    if [[ -n "${named[$preset]:-}" || $task_run -eq 0 ]]; then
        run_tests "$preset"; return
    fi
    scope="$(test_scope)"
    needs_sdk_tests || ex="$ex|sdk"
    case "$scope" in
        full)
            echo "tests: all (the change can reach the GPU tests), without the ${ex//|/, } label(s)"
            run_tests "$preset" -LE "$ex" ;;
        nongpu)
            echo "tests: without the gpu, ${ex//|/, } label(s) (every changed file is GPU-irrelevant)"
            run_tests "$preset" -LE "gpu|$ex" ;;
        smoke)
            echo "tests: without the gpu, ${ex//|/, } label(s), then the GPU smoke subset $GPU_SMOKE (simulation-only change)"
            run_tests "$preset" -LE "gpu|$ex" && run_tests "$preset" -L gpu -LE "$ex" -R "$GPU_SMOKE" ;;
    esac
}

# Configure, build, test, with the three times on a "=== timing" line the summary picks up.
stage_preset() {
    local preset="$1" t0 t1 t2 rc=0
    t0=$SECONDS
    configure "$preset" || return 1
    t1=$SECONDS
    cmake --build --preset "$preset" || return 1
    t2=$SECONDS
    run_scoped_tests "$preset" || rc=1
    echo "=== timing: configure $((t1 - t0))s, build $((t2 - t1))s, tests $((SECONDS - t2))s"
    return $rc
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
        # A target premerge.py guessed wrong (a test command that is not a target) falls back to the full build.
        # shellcheck disable=SC2086
        cmake --build --preset linux-gcc-debug --target $targets || cmake --build --preset linux-gcc-debug || return 1
    fi
    xvfb-run -a -s "-screen 0 1280x1024x24 -noreset" ctest --preset linux-gcc-debug -j "$JOBS" -R "$regex" || return 1
    [[ $rc -eq 0 ]] && return 0
    echo "premerge: tests pass, but a --task re-run is needed (see the overlap above, or no --task base is recorded)"
    return 1
}

# Prints the run-clang-tidy file regexes: one per changed translation unit, or the whole tree.
tidy_targets() {
    local f
    if [[ $tidy_all -eq 1 ]] || ! have_base; then echo "^$ROOT/(src|samples|tests)/"; return; fi
    if grep -qx '.clang-tidy' <<< "$CHANGED"; then echo "^$ROOT/(src|samples|tests)/"; return; fi
    while IFS= read -r f; do
        [[ -n "$f" && "$f" =~ ^(src|samples|tests)/ ]] && echo "^$ROOT/${f//./\\.}\$"
    done < <(changed_tus)
}

stage_tidy() {
    local targets objs
    mapfile -t targets < <(tidy_targets)
    if [[ ${#targets[@]} -eq 0 ]]; then
        echo "tidy: no C++ translation unit changed since the merge base, nothing to check"
        return 0
    fi
    configure linux-clang-debug || return 1
    if [[ "${targets[0]}" == "^$ROOT/(src|samples|tests)/" ]]; then
        echo "tidy: the whole tree"
        cmake --build --preset linux-clang-debug || return 1
    else
        # Only the changed units' objects: ninja builds their prerequisites (generated headers, the
        # third-party libraries they include) and nothing else, and clang compiles the units once here too.
        echo "tidy: ${#targets[@]} target(s): ${targets[*]}"
        mapfile -t objs < <(changed_tus | grep -E '^(src|samples|tests)/' | tu_objects linux-clang-debug)
        if [[ ${#objs[@]} -gt 0 ]]; then
            echo "tidy: compiling ${#objs[@]} object(s) in build/linux-clang-debug"
            ninja -C build/linux-clang-debug "${objs[@]}" || return 1
        fi
    fi
    run-clang-tidy-18 -p build/linux-clang-debug -quiet "${targets[@]}"
}

# Every target of the gcc Release tree, tests included, compiled but not run: a -Werror that fires in the
# editor, the MCP server or a sample in Release (gcc 13's -Wformat-truncation did, #359) passed every
# earlier stage; the SDK build (release_sdk.sh) is this tree. Under --task only the affected units'
# objects: the warnings fire at compile time, and linking the hundreds of binaries is what the rest costs
# (claude_communication A3/F: 504 s cold, 66 s warm, versus 3 s per object).
stage_release_build() {
    local scope objs
    configure linux-gcc-release || return 1
    scope="$(tu_scope release-build)"
    if [[ "$scope" == full ]]; then
        echo "release-build: the whole tree"
        cmake --build --preset linux-gcc-release
        return
    fi
    mapfile -t objs < <(tu_objects linux-gcc-release <<< "$scope")
    if [[ ${#objs[@]} -eq 0 ]]; then
        echo "release-build: no translation unit of this tree is affected, nothing to compile"
        return 0
    fi
    echo "release-build: compiling ${#objs[@]} object(s) in build/linux-gcc-release"
    ninja -C build/linux-gcc-release "${objs[@]}"
}

# ---------------------------------------------------------------------------------------------------
# Determinism (FR-RUN-040, ADR-0007): the same World state hash across 3 runs and Debug vs Release, per
# subject. Each subject names the targets it needs in the two gcc trees (det_targets) and the checks it runs
# (det_run); det_subjects_of says which subjects a changed file can reach. The stage's time is almost all
# building subjects in the Release tree (claude_communication A4: the runs are 1-3 s each), so --task builds
# only the reachable ones. Adding a subject: a line in DET_ALL, det_targets, det_run and det_subjects_of.
DET_ALL=(boids physics2d platformer diorama scene stream gameplay3d shop liveops net module physics3d cubes court demo_games script)

det_targets() {  # $1 subject, $2 tree (debug|release); the content packs are cooked once, in the debug tree
    case "$1" in
        boids) echo sample_02_boids ;;
        physics2d) echo sample_physics2d_headless ;;
        platformer) echo sample_03_platformer ;;
        diorama) echo sample_04_diorama ;;
        scene) echo mce_player; [[ $2 == debug ]] && echo scene_demo_content ;;
        stream) echo mce_player; [[ $2 == debug ]] && echo stream_demo_content ;;
        gameplay3d) echo mce_player; [[ $2 == debug ]] && echo gameplay3d_content ;;
        shop) echo sample_shop_demo ;;
        liveops) echo sample_liveops_demo ;;
        net) echo sample_net_headless sample_net_rollback sample_net_arena ;;
        module) echo mce_devhost modtest_good ;;
        physics3d) echo sample_physics3d_headless ;;
        cubes) echo sample_04_cubes ;;
        court) echo sample_05_ashen_court ;;
        demo_games) echo glowmoth marble_garden ;;
        script) echo sample_script_headless ember_script dungeon pursuit ;;
    esac
    return 0
}

# The subjects a changed file can reach; "ALL" when it can reach every hash (core, ecs, refl, audio, input,
# an unmapped engine module, the CMake files, this script or the harness). render/kit/asset are in: 04_cubes,
# 04_diorama and 05_ashen_court hash World3D, camera and animation state (claude_communication R3).
det_subjects_of() {
    case "$1" in
        src/engine/core/*|src/engine/ecs/*|src/engine/refl/*|src/engine/audio/*|src/engine/input/*|\
            cmake/*|CMakeLists.txt|CMakePresets.json|scripts/ci/determinism.py|scripts/ci/local_ci.sh) echo ALL ;;
        src/engine/physics2d/*) echo physics2d boids platformer demo_games ;;
        src/engine/physics3d/*) echo physics3d cubes court diorama gameplay3d demo_games ;;
        src/engine/scene/*) echo scene stream gameplay3d cubes diorama court demo_games ;;
        src/engine/script/*) echo script demo_games gameplay3d ;;
        src/engine/render/*|src/engine/rhi*/*|src/engine/kit/*|src/engine/kit_novel/*|src/engine/asset/*)
            echo cubes diorama court gameplay3d demo_games platformer ;;
        src/engine/net/*) echo net ;;
        src/engine/monetize/*|src/engine/liveops/*) echo shop liveops ;;
        src/engine/module/*|src/module/*|tests/module/*|tests/devhost/*) echo module ;;
        src/engine/*) echo ALL ;;
        src/player/*) echo scene stream gameplay3d ;;
        samples/02_boids/*) echo boids ;;
        samples/physics2d_headless/*) echo physics2d ;;
        samples/03_platformer/*) echo platformer ;;
        samples/04_diorama/*) echo diorama ;;
        samples/scene_demo/*) echo scene ;;
        samples/stream_demo/*) echo stream ;;
        samples/07_gameplay3d/*) echo gameplay3d ;;
        samples/shop_demo/*) echo shop ;;
        samples/liveops_demo/*) echo liveops ;;
        samples/net_*/*) echo net ;;
        samples/physics3d_headless/*) echo physics3d ;;
        samples/04_cubes/*) echo cubes ;;
        samples/05_ashen_court/*) echo court ;;
        samples/script_headless/*) echo script ;;
        projects/Glowmoth/*|projects/MarbleGarden/*) echo demo_games ;;
        projects/EmberScript/*) echo script ;;
        *) hashes_state "$1" && echo ALL ;;
    esac
    return 0
}

# The subjects this run checks: all of them, or under --task the union over the changed files.
det_subjects() {
    if [[ $task_run -eq 0 || -n "${named[determinism]:-}" ]] || ! have_base; then echo "${DET_ALL[@]}"; return; fi
    local f s
    local -A sel=()
    while IFS= read -r f; do
        [[ -n "$f" ]] || continue
        for s in $(det_subjects_of "$f"); do
            [[ "$s" == ALL ]] && { echo "${DET_ALL[@]}"; return; }
            sel[$s]=1
        done
    done <<< "$CHANGED"
    for s in "${DET_ALL[@]}"; do [[ -n "${sel[$s]:-}" ]] && printf '%s ' "$s"; done
    echo
}

det() { python3 scripts/ci/determinism.py --runs 3 "$@"; }
det_pair() {  # $1 relative binary path, $2 args (--args= form: a value starting with "-" needs it)
    det --args="$2" "build/linux-gcc-debug/$1" "build/linux-gcc-release/$1"
}

det_run() {
    case "$1" in
        boids) det_pair samples/02_boids/sample_02_boids "-ticks 1000 -boids 20000" ;;
        physics2d) det_pair samples/physics2d_headless/sample_physics2d_headless "-ticks 2000" ;;
        platformer) det_pair samples/03_platformer/sample_03_platformer "-headless 1 -ticks 1800" ;;
        diorama) det_pair samples/04_diorama/sample_04_diorama "-headless 1 -ticks 600" ;;
        scene) det_pair src/player/mce_player "-headless 1 -ticks 600 -pack build/linux-gcc-debug/samples/scene_demo/project/assets.mce" ;;
        stream) det_pair src/player/mce_player "-headless 1 -ticks 1500 -pack build/linux-gcc-debug/samples/stream_demo/project/assets.mce -scene scenes/world.mcscene" ;;
        gameplay3d) det_pair src/player/mce_player "-headless 1 -ticks 600 -pack build/linux-gcc-debug/samples/07_gameplay3d/project/assets.mce -input samples/07_gameplay3d/drive.input" ;;
        shop) det_pair samples/shop_demo/sample_shop_demo "-ticks 1850" ;;
        liveops) det_pair samples/liveops_demo/sample_liveops_demo "-ticks 1850" ;;
        net) stage_net_determinism ;;
        module) stage_module_determinism ;;
        physics3d) stage_physics3d_determinism ;;
        cubes) stage_cubes_determinism ;;
        court) stage_court_determinism ;;
        demo_games) stage_demo_games_determinism ;;
        script) stage_script_determinism ;;
        *) echo "unknown determinism subject: $1"; return 2 ;;
    esac
}

# A game module ticked by the devhost: same World state hash across runs and Debug/Release.
# The module path differs per tree, so each tree is compared against itself and the two hashes
# are then compared.
stage_module_determinism() {
    local h=()
    for t in linux-gcc-debug linux-gcc-release; do
        det --args="-module build/$t/tests/module/modules/libmodtest_good.so -ticks 2000" \
            "build/$t/tests/devhost/mce_devhost" || return 1
        h+=("$("build/$t/tests/devhost/mce_devhost" -module "build/$t/tests/module/modules/libmodtest_good.so" \
            -ticks 2000 | grep -o 'state_hash [0-9a-f]*')")
    done
    echo "module state hash: debug ${h[0]}, release ${h[1]}"
    [[ "${h[0]}" == "${h[1]}" ]]
}

# M5b (ADR-0003 §8): the Jolt-backed 3D scene hashes the same across runs and Debug/Release.
stage_physics3d_determinism() {
    det_pair samples/physics3d_headless/sample_physics3d_headless "-ticks 2000"
}

# The M-AGENT demo games (docs/research/m-agent-demo/): Glowmoth's second level idle (bats and
# lanterns) and Marble Garden's scripted win route (3D physics, five pickups, a scene change).
stage_demo_games_determinism() {
    det_pair projects/Glowmoth/glowmoth "-headless 1 -ticks 1200 -scene 0c5b255e-286f-4293-a352-48dcb9c80653" &&
        det_pair projects/MarbleGarden/marble_garden \
            "-headless 1 -ticks 1800 -scene 5ebadc24-96f4-438c-a342-d6f37fb5ca8c -input projects/MarbleGarden/tests/win_route.input"
}

# ADR-0041 spike: the Luau system hashes the same across runs and Debug/Release, and the same as
# its C++ twin (`-mode cpp`), so the VM adds no float drift of its own.
stage_script_determinism() {
    det_pair projects/EmberScript/ember_script "-headless 1 -ticks 1800 -input projects/EmberScript/tests/run_right.input" &&
        det_pair projects/Dungeon/dungeon "-headless 1 -ticks 3000 -input projects/Dungeon/tests/autopilot.input" &&
        det_pair projects/Pursuit/pursuit "-headless 1 -ticks 1500 -input projects/Pursuit/tests/autopilot.input" &&
        det_pair samples/script_headless/sample_script_headless "-ticks 1200" &&
        python3 scripts/ci/determinism.py --runs 1 --args="-ticks 1200 -mode cpp" \
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
    det_pair samples/net_headless/sample_net_headless "-sim 1 -clients 4 -latency 75 -jitter 20 -loss 0.05 -ticks 600" &&
        det_pair samples/net_headless/sample_net_headless "-sim 1 -clients 8 -latency 150 -jitter 40 -loss 0.15 -seed 3 -ticks 900" &&
        det_pair samples/net_headless/sample_net_headless "-sim 1 -relay 1 -clients 3 -latency 40 -jitter 10 -loss 0.05 -ticks 600" &&
        det_pair samples/net_rollback/sample_net_rollback "-peers 4 -latency 100 -jitter 20 -loss 0.05 -frames 900" &&
        det_pair samples/net_rollback/sample_net_rollback "-peers 2 -prediction 0 -frames 300" &&
        det_pair samples/net_arena/sample_net_arena "-headless 1 -clients 4 -latency 150 -jitter 20 -loss 0.03 -ticks 900" &&
        det_pair samples/net_arena/sample_net_arena "-headless 1 -clients 3 -latency 60 -jitter 10 -loss 0.1 -seed 9 -ticks 600"
}

# M5a: 04_cubes (cascade fit, Forward+ clustering, node animation) across runs and Debug/Release;
# M5b: the same with the capsule walk (World3D state + the swept camera boom in the hash).
stage_cubes_determinism() {
    det_pair samples/04_cubes/sample_04_cubes "-headless 1 -ticks 600" &&
        det_pair samples/04_cubes/sample_04_cubes "-headless 1 -walk 1 -ticks 600"
}

# M7: Ashen Court with the bot playing (enemy FSMs, combat, World3D characters, the swept boom,
# 3D audio commands) across runs and Debug/Release; 3000 ticks covers a cleared court and a restart.
stage_court_determinism() {
    det_pair samples/05_ashen_court/sample_05_ashen_court "-headless 1 -ticks 3000"
}

stage_determinism() {
    # Reuses the gcc debug/release trees (tests stay ON so it doesn't thrash the test stages' caches).
    local subjects s t targets
    read -ra subjects <<< "$(det_subjects)"
    if [[ ${#subjects[@]} -eq 0 ]]; then
        echo "determinism: the change reaches no subject, nothing to check"
        return 0
    fi
    echo "determinism: ${subjects[*]}"
    for t in debug release; do
        targets=()
        for s in "${subjects[@]}"; do read -ra _t <<< "$(det_targets "$s" "$t" | tr '\n' ' ')"; targets+=("${_t[@]}"); done
        configure "linux-gcc-$t" && cmake --build --preset "linux-gcc-$t" --target "${targets[@]}" || return 1
    done
    for s in "${subjects[@]}"; do
        echo "--- $s"
        det_run "$s" || return 1
    done
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
        walls) python3 scripts/ci/check_walls.py && python3 scripts/bump_version.py --check && python3 scripts/ci/capabilities.py --check && python3 scripts/ci/check_docs.py && python3 scripts/changelog.py --check ;;
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
declare -A RESULT
if [[ $save_only -eq 1 ]]; then save_cache_snapshot; exit 0; fi
[[ $HAVE_CCACHE -eq 1 ]] && ccache --zero-stats > /dev/null
preconfigure

# The key of this run for --resume: the commit, the uncommitted changes and the arguments.
PASSED_DIR="$LOG_DIR/passed"
run_key="$({ git rev-parse HEAD; git diff HEAD; git ls-files --others --exclude-standard | xargs -r sha1sum; echo "${stages[*]} task=$task_run tidy_all=$tidy_all"; } 2>/dev/null | sha1sum | cut -c1-16)"
[[ $resume -eq 1 ]] || rm -rf "$PASSED_DIR"
mkdir -p "$PASSED_DIR"

failed=0
for ((si = 0; si < ${#stages[@]}; si++)); do
    s="${stages[$si]}"
    echo "=== $s"
    start=$SECONDS
    read -r h0 m0 <<< "$(ccache_counts)"
    if [[ $resume -eq 1 && -f "$PASSED_DIR/$s" && "$(cat "$PASSED_DIR/$s")" == "$run_key" ]]; then
        RESULT[$s]="PASS (resumed: passed in an earlier run of this change)"
    elif ! reclaim_space "$si"; then
        RESULT[$s]="FAIL (0s) — no disk space, see above"
        failed=1
    elif { [[ -f "$LOG_DIR/$s.log" ]] && mv -f "$LOG_DIR/$s.log" "$LOG_DIR/$s.prev.log"; run_stage "$s" > "$LOG_DIR/$s.log" 2>&1; }; then
        RESULT[$s]="PASS ($((SECONDS - start))s)"
        echo "$run_key" > "$PASSED_DIR/$s"
    else
        RESULT[$s]="FAIL ($((SECONDS - start))s) — see build/local-ci-logs/$s.log"
        failed=1
        tail -n 40 "$LOG_DIR/$s.log"
    fi
    timing="$(grep -o '=== timing: .*' "$LOG_DIR/$s.log" 2>/dev/null | tail -1 | sed 's/=== timing: //')"
    [[ -n "$timing" ]] && RESULT[$s]="${RESULT[$s]} [$timing]"
    read -r h1 m1 <<< "$(ccache_counts)"
    if (( h1 + m1 - h0 - m0 > 0 )); then
        RESULT[$s]="${RESULT[$s]} [ccache $(( (h1 - h0) * 100 / (h1 + m1 - h0 - m0) ))% of $(( h1 + m1 - h0 - m0 ))]"
    fi
    echo "    ${RESULT[$s]}"
done

echo
echo "=== Summary ($(git rev-parse --short HEAD 2>/dev/null))"
for s in "${stages[@]}"; do printf '  %-22s %s\n' "$s" "${RESULT[$s]}"; done
if [[ $HAVE_CCACHE -eq 1 ]]; then
    ccache --show-stats 2> /dev/null | grep -E '^(Cacheable calls|  Hits|  Misses|Uncacheable)' | awk '!seen[$0]++' |
        sed 's/^/  ccache: /'
    save_cache_snapshot
fi
# A --task whose walls and gcc-debug tests passed records the base it ran on (a tidy-only fix needs no re-run), so --premerge can tell later whether dev_claude moved onto the
# PR's files (scripts/ci/premerge.py overlap).
if [[ $task_run -eq 1 && "${RESULT[walls]:-}" == PASS* && "${RESULT[linux-gcc-debug]:-}" == PASS* ]]; then
    printf 'BASE=%s\nHEAD=%s\n' "$(git merge-base HEAD "${MCE_TIDY_BASE:-origin/dev_claude}" 2>/dev/null)" \
        "$(git rev-parse HEAD)" > "$LOG_DIR/last-task.env"
fi
# One line per run in a log shared by every thread, so the pre-PR CI time is measurable:
# <utc> branch=<b> mode=<task|premerge|full|stages> result=<PASS|FAIL> total=<s> boot=<container boot> restarts=<n> <stage>=<s>...
# `restarts` counts stages skipped as "resumed" (--resume); `boot` lets a reader count the container restarts a
# branch's runs lived through. MCE_CI_TIMINGS overrides the path; no shared folder, no record.
record_timing() {
    local f="${MCE_CI_TIMINGS:-/mnt/project-files/.cache/ci-timings.log}" mode line s secs resumed=0
    [[ -d "$(dirname "$f")" && -w "$(dirname "$f")" ]] || return 0
    if [[ $task_run -eq 1 ]]; then mode=task
    elif [[ " ${stages[*]} " == *" premerge "* ]]; then mode=premerge
    elif [[ $full_run -eq 1 ]]; then mode=full
    else mode=stages; fi
    for s in "${stages[@]}"; do [[ "${RESULT[$s]:-}" == *resumed* ]] && resumed=$((resumed + 1)); done
    line="$(date -u +%FT%TZ) branch=$(git rev-parse --abbrev-ref HEAD 2> /dev/null) mode=$mode"
    line+=" result=$([[ $failed -eq 0 ]] && echo PASS || echo FAIL) total=${SECONDS}s boot=$(uptime -s 2> /dev/null | tr ' ' _) restarts=$resumed"
    for s in "${stages[@]}"; do
        secs="$(sed -n 's/^[A-Z]* (\(resumed[^)]*\|\([0-9]*\)s\)).*/\2/p' <<< "${RESULT[$s]:-}")"
        line+=" $s=${secs:-0}"
    done
    echo "$line" >> "$f" 2> /dev/null || true
}
record_timing

exit $failed
