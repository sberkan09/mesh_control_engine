#!/usr/bin/env python3
"""The M-AGENT demo's "no edit is lost" gate, over a finished team run (setup_run.sh layout).

It reads every agent's transcript (agents/<name>/transcript.jsonl, one MCP request and answer per
line) and fails when an edit could have been dropped:
  - a scene_save that succeeded with force: true (it overwrites without merging),
  - an asset_unlock with force: true (it breaks another agent's lock),
  - a scene an agent edited after its last successful save of it, or never saved (edits left in
    memory when the run ended),
  - a save refused for conflicts that the same agent never followed with a successful save.
It also prints how many saves merged another agent's work and how many conflicts were settled.

    check_team_run.py <run dir>        exit 0 when the gate holds
"""

import json
import pathlib
import sys

EDIT_TOOLS = {"entity_create", "entity_delete", "entity_duplicate", "component_add", "component_remove",
              "component_set", "scene_clear", "scene_undo", "scene_redo", "prefab_create_from", "prefab_revert"}


def tool_result(entry):
    r = entry.get("response", {}).get("result", {})
    text = r.get("content", [{}])[0].get("text", "") if r.get("content") else ""
    return (not r.get("isError", True)), text


def check(run):
    problems, merged, resolved = [], 0, 0
    for transcript in sorted(run.glob("agents/*/transcript.jsonl")):
        agent = transcript.parent.name
        current = None              # the scene this agent's server has open
        dirty = {}                  # scene -> the call that edited it after its last save
        refused = {}                # scene -> a conflict refusal not yet followed by a save
        for n, line in enumerate(transcript.read_text(encoding="utf-8").splitlines(), 1):
            e = json.loads(line)
            if e.get("method") != "tools/call":
                continue
            tool = e["params"].get("name")
            args = e["params"].get("arguments", {}) or {}
            ok, text = tool_result(e)
            where = f"{agent} call {n} ({tool})"
            if tool in ("scene_open", "scene_new", "scene_switch") and ok:
                current = json.loads(text).get("path") if text.startswith("{") else args.get("path")
                if tool == "scene_new":
                    current = None
            elif tool in EDIT_TOOLS and ok:
                dirty[current] = where
            elif tool == "scene_save":
                target = args.get("path") or current
                if ok:
                    if args.get("force"):
                        problems.append(f"{where}: saved {target} with force (no merge)")
                    if args.get("resolve"):
                        resolved += 1
                    dirty.pop(current, None)
                    dirty.pop(target, None)
                    refused.pop(target, None)
                    current = target
                elif "conflict" in text:
                    refused[target] = where
            elif tool == "asset_unlock" and ok and args.get("force"):
                problems.append(f"{where}: broke a lock with force")
        for scene, where in dirty.items():
            problems.append(f"{where}: edited {scene or 'an unsaved scene'} and never saved it afterwards")
        for scene, where in refused.items():
            problems.append(f"{where}: a save of {scene} was refused for conflicts and never retried")
    events = run.glob("*/.mce/events.jsonl")
    for f in events:
        for line in f.read_text(encoding="utf-8").splitlines():
            if '"tool":"scene_merge"' in line.replace(" ", ""):
                merged += 1
    return problems, merged, resolved


def main(argv):
    if len(argv) != 1:
        print(__doc__)
        return 2
    problems, merged, resolved = check(pathlib.Path(argv[0]))
    print(f"saves that merged another agent's work: {merged}; conflicts settled with resolve: {resolved}")
    for p in problems:
        print("LOST? " + p)
    print("gate: " + ("FAIL" if problems else "pass (no edit lost)"))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
