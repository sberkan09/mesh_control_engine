#!/usr/bin/env bash
# The integration run (M1): the full matrix on the dev_claude head, once, instead of per thread.
#
#   scripts/ci/integration_run.sh [--no-push] [--branch NAME]
#
# 1. checks out origin/dev_claude (detached; refuses a dirty tree) and regenerates the generated files
#    (capabilities.md, the sample coverage allow list, the transcript golden: `local_ci.sh regen`);
#    a change is committed on a branch ci/regen-<sha> and pushed, for the coordinator to merge. The stages
#    below run on that commit, so a stale golden never turns the run red.
# 2. runs the stages group by group, one local_ci.sh call each, so the disk strategy (finished build trees are
#    deleted when space runs short, the ccache never is) works per group and one red group does not hide the
#    rest: walls + gcc debug (+ GPU tests) + determinism + release build, clang release, ASan, tidy + web, then
#    the `slow` tests when local_ci.sh has that stage.
# 3. refreshes the ccache snapshot (/mnt/project-files/.cache) after the run, green or not.
# 4. writes build/local-ci-logs/integration-report.md and prints it: result per group, and on red the
#    suspect PRs, i.e. the first-parent merges on dev_claude since the last green integration run
#    (MCE_INTEGRATION_STATE, default /mnt/project-files/.cache/integration-last-green when that folder exists,
#    else build/local-ci-logs/integration-last-green; without it, the last 10 merges).
#
# It fixes nothing: on red the report names the failing groups, the log files and the suspects, and the exit
# code is 1. Run it detached (nohup scripts/ci/integration_run.sh > build/local-ci-logs/integration.log &) and
# poll for "=== Integration summary".

set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

push=1
branch=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --no-push) push=0 ;;
        --branch) branch="$2"; shift ;;
        -h|--help) sed -n '2,24p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1"; exit 2 ;;
    esac
    shift
done

LOG_DIR="$ROOT/build/local-ci-logs"
mkdir -p "$LOG_DIR"
REPORT="$LOG_DIR/integration-report.md"
if [[ -d /mnt/project-files/.cache ]]; then
    STATE="${MCE_INTEGRATION_STATE:-/mnt/project-files/.cache/integration-last-green}"
else
    STATE="${MCE_INTEGRATION_STATE:-$LOG_DIR/integration-last-green}"
fi

if [[ -n "$(git status --porcelain --untracked-files=no)" ]]; then
    echo "integration: the working tree has uncommitted changes; run it on a clean checkout"; exit 2
fi
git fetch -q origin dev_claude || { echo "integration: cannot fetch origin/dev_claude"; exit 2; }
git checkout -q --detach origin/dev_claude || exit 2
head_sha="$(git rev-parse --short HEAD)"
echo "=== integration run on dev_claude $head_sha"

bash scripts/ci/setup_env.sh > "$LOG_DIR/setup_env.log" 2>&1 || echo "integration: setup_env.sh failed (see $LOG_DIR/setup_env.log), going on"

# --- 1. regenerate the generated files -------------------------------------------------------------
regen_note="no generated file was stale"
regen_branch=""
if scripts/ci/local_ci.sh regen > "$LOG_DIR/regen.run.log" 2>&1; then
    :
else
    regen_note="regen FAILED (a gap a human has to close), see build/local-ci-logs/regen.log"
fi
if [[ -n "$(git status --porcelain)" ]]; then
    regen_branch="${branch:-ci/regen-$head_sha}"
    git checkout -q -b "$regen_branch" &&
        git add -A docs/engine/capabilities.md scripts/ci/sample_coverage.allow tests/mcp/transcripts &&
        git -c user.name="integration-runner" -c user.email="noreply@anthropic.com" commit -q \
            -m "CI: regenerate generated files on dev_claude $head_sha" \
            -m "Written by scripts/ci/integration_run.sh (capabilities.md, sample coverage allow list, transcript golden)." &&
        regen_note="regenerated: $(git show --name-only --format= HEAD | paste -sd, -) on branch $regen_branch"
    if [[ $push -eq 1 ]]; then
        git push -q -u origin "$regen_branch" 2>&1 && regen_note="$regen_note (pushed; merge it into dev_claude)" ||
            regen_note="$regen_note (NOT pushed: push failed)"
    fi
fi
echo "integration: $regen_note"

# --- 2. the stages, group by group -----------------------------------------------------------------
GROUPS_RUN=("walls linux-gcc-debug determinism release-build" "linux-clang-release" "linux-asan" "tidy web")
grep -q '^ALL_STAGES=.*\bslow\b' scripts/ci/local_ci.sh && GROUPS_RUN+=("slow")
failed=()
declare -A RESULT
i=0
for g in "${GROUPS_RUN[@]}"; do
    i=$((i + 1))
    echo "=== group $i: $g"
    start=$SECONDS
    # shellcheck disable=SC2086
    if scripts/ci/local_ci.sh $g > "$LOG_DIR/integration-group-$i.log" 2>&1; then
        RESULT[$i]="PASS ($((SECONDS - start))s)"
    else
        RESULT[$i]="FAIL ($((SECONDS - start))s)"
        failed+=("$i")
    fi
    echo "    ${RESULT[$i]}"
done

# --- 3. ccache snapshot -----------------------------------------------------------------------------
scripts/ci/local_ci.sh --save-cache-only > "$LOG_DIR/integration-snapshot.log" 2>&1 || true

# --- 4. report ----------------------------------------------------------------------------------------
last_green=""
[[ -f "$STATE" ]] && last_green="$(head -n1 "$STATE")"
if [[ -n "$last_green" ]] && git cat-file -e "$last_green^{commit}" 2>/dev/null; then
    range="$last_green..origin/dev_claude"
else
    range="-10 origin/dev_claude"
fi
{
    echo "# Integration run on dev_claude $head_sha"
    echo
    echo "Generated files: $regen_note"
    echo
    i=0
    for g in "${GROUPS_RUN[@]}"; do
        i=$((i + 1))
        echo "- group $i ($g): ${RESULT[$i]}"
        [[ "${RESULT[$i]}" == FAIL* ]] && grep -E '^  [a-z0-9-]+ +(PASS|FAIL)' "$LOG_DIR/integration-group-$i.log" | sed 's/^/    /'
    done
    if [[ ${#failed[@]} -gt 0 ]]; then
        echo
        echo "## Suspect PRs (first-parent merges on dev_claude since ${last_green:-the last 10 merges})"
        # shellcheck disable=SC2086
        git log --first-parent --format='- %h %s' $range
        echo
        echo "Logs: build/local-ci-logs/integration-group-<n>.log and <stage>.log of the failing stage."
    fi
} > "$REPORT"

echo
echo "=== Integration summary ($head_sha)"
cat "$REPORT"
if [[ ${#failed[@]} -eq 0 ]]; then
    # the state records dev_claude's head, not the regen commit
    git rev-parse origin/dev_claude > "$STATE" 2> /dev/null || true
    exit 0
fi
exit 1
