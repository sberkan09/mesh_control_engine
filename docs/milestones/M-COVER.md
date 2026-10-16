# M-COVER — Docs and sample coverage (milestone record)

**Status:** active (added 2026-10-16; step 1 done). Plan: ROADMAP §M-COVER; Linear SBA-157 … SBA-160.

**Why:** the capability table (M-DATA-REACH) already fails `local_ci.sh` when a component, asset type or MCP
tool is named by no document. Nothing checks that a component is *shown working*. A user who finds
`Decal3D` in the guide has no scene to copy. Of the 101 reflected components, 39 appear in no sample,
project or template, only in code and tests.

It needs no device and no engine code: the work is a gate and data-only samples.

**Not in scope:**
- New engine features.
- Samples for asset types and MCP tools (the docs gate covers those).
- Visual polish beyond what proves the component runs.

## Steps

| # | Step | Linear | State |
|---|---|---|---|
| 1 | The gate: `capabilities.py --check` (in the walls stage) fails for a component that no file under `samples/`, `projects/` or `templates/` names, unless `scripts/ci/sample_coverage.allow` lists it with a reason; it also fails for a listed component a sample now shows, so the list only shrinks. The table gains a "Shown in" column | SBA-157 | done |
| 2 | A 3D tour sample, data only, played by the stock player: bone attachment, IK, locomotion, camera shake and blends, probes, GI volume, foliage, terrain collider, decal, 3D particles, nav areas, links and obstacles, a sensor ray, a waypoint circuit and a vehicle driver. A ctest runs it headless twice for the same hash | SBA-158 | not started |
| 3 | A 2D and gameplay tour: a 2D skeleton with animator, slots, follow and IK, camera shake and shake triggers, particle bursts, physics events, a nav grid source with path agents, sound captions, a token pool | SBA-159 | not started |
| 4 | A systems tour: replicated and owned entities, a script system, dialogue; the allow-list ends with only state the engine writes (`Blackboard`, `SceneChunkStatus`) | SBA-160 | not started |

**Gates:**
- `local_ci.sh` walls fails on an unshown, unlisted component, and on a stale list entry.
- Every sample added runs headless twice with the same state hash.
- The allow-list ends with only runtime-state components, each with its reason.

## Step 1 — The gate (done, SBA-157)

- **What counts as shown.** A file under `samples/`, `projects/` or `templates/` (scenes, prefabs, timelines,
  sequences, behavior trees, UI documents, tests, scripts and C++) that names the component as a whole word.
  Code counts because some components are added from a module; the tours in steps 2–4 are data.
- **The list.** `scripts/ci/sample_coverage.allow`, lines of `Component | reason`. It starts with the 39
  components that no sample names, in four groups: two that are state the engine writes, and 18, 14 and 5
  that steps 2, 3 and 4 will show. A line without a reason, a name that is not a component, or a component a
  sample now shows fails the check.
- **The table.** `docs/engine/capabilities.md` has a "Shown in" column with the first file that names each
  component, or `allowed: <reason>`.
- **Found while adding it.** `M-AUDIO2`'s six audio mix components were in the list until `audio_rooms` landed.
