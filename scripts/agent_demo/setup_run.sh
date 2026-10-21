#!/usr/bin/env bash
# Prepares one M-AGENT demo run (docs/research/m-agent-demo/): a fresh project from the SDK's
# template, built once, and one MCP bridge per agent (scripts/agent_demo/mcp_bridge.py), each
# with its own server, transcript, image folder and work/ folder for the agent's own scripts (a
# scratchpad shared between agents let one agent's helper edit another's project).
#
#   setup_run.sh <sdk prefix> <run dir> <ProjectName> <agent>...     e.g. ... /tmp/run Glowmoth solo
#   setup_run.sh --stop <run dir>
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ "${1:-}" == "--stop" ]]; then
    for s in "$2"/agents/*/bridge.sock; do
        [[ -S "$s" ]] && python3 "$HERE/mcp_bridge.py" stop --sock "$s" || true
    done
    exit 0
fi

SDK="$1" RUN="$2" NAME="$3"
shift 3
MCE=(python3 "$SDK/bin/mce")
mkdir -p "$RUN/agents"
if [[ ! -d "$RUN/$NAME" ]]; then
    "${MCE[@]}" new "$NAME" --dest "$RUN"
    "${MCE[@]}" build "$RUN/$NAME" --config Release
fi
for agent in "$@"; do
    d="$RUN/agents/$agent"
    mkdir -p "$d/work"
    nohup python3 "$HERE/mcp_bridge.py" serve --sock "$d/bridge.sock" --log "$d/transcript.jsonl" --images "$d/images" \
        --name "$agent" -- "${MCE[@]}" mcp "$RUN/$NAME" --agent "$agent" > "$d/bridge.log" 2>&1 &
done
for agent in "$@"; do
    for _ in $(seq 100); do [[ -S "$RUN/agents/$agent/bridge.sock" ]] && break; sleep 0.1; done
    [[ -S "$RUN/agents/$agent/bridge.sock" ]] || { echo "bridge for $agent did not start"; cat "$RUN/agents/$agent/bridge.log"; exit 1; }
    echo "$agent: python3 $HERE/mcp_bridge.py call --sock $RUN/agents/$agent/bridge.sock <tool> '<json>'"
    echo "$agent: scripts in $RUN/agents/$agent/work"
done
