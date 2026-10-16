# M-PROCGEN — Deterministic procedural levels, as data (milestone record)

**Status:** active (added 2026-10-16, not started). Plan: ROADMAP §M-PROCGEN; Linear SBA-171 … SBA-175.

**Why:** a level made from a seed is the cheapest way to a game that stays fresh, and the engine's determinism
contract (same inputs, same `state_hash`) makes it testable: the seed is the level. Today the engine has
`.mctilemap`, `NavGrid`, `Collider2D` and `terrain_generate` (heightfields) but nothing that builds a 2D level from
a seed, so a roguelike needs its own C++ or a hand-written Luau generator.

**Design.** Generators are pure functions of (kind, parameters, seed) in `mce/kit/procgen.h`: no clock, no global
random, no allocation order that depends on the host. Random numbers come from named child streams of the seed, so
adding a step to one generator never shifts the others. A `ProcLevel` component names a generator and its seed; the
stock hosts expand it at scene load (the tilemap in memory, wall colliders, marker entities), so a saved scene
holds the seed and a replay rebuilds the same level. Unsupported combinations are named warnings, never a hang:
WFC gives up after a bounded number of restarts.

**Not in scope:** 3D level generation, runtime-streamed infinite worlds, learned generators, a graph-based
dungeon editor.

## Steps

| # | Step | Linear | Model | State |
|---|---|---|---|---|
| 1 | `mce/kit/procgen.h`: streams, BSP rooms, caves, drunkard's walk, noise threshold; connectivity | SBA-171 | Sonnet | not started |
| 2 | Wave function collapse: adjacency from an example tilemap or rules, pinned cells, bounded restarts | SBA-172 | Sonnet | not started |
| 3 | `ProcLevel`: the stock hosts build tilemap, colliders and markers from a seed; also from Luau | SBA-173 | Sonnet | not started |
| 4 | MCP `procgen_run`, the `reachable` assertion in `.mctest`, user guide | SBA-174 | Sonnet | not started |
| 5 | `samples/10_dungeon`, a script-only variant, the perf budget and the gates | SBA-175 | Sonnet | not started |

**Gates:**
- Every generator gives the same bytes twice and in Debug and Release.
- Connectivity holds on every tested seed (a thousand per generator in the unit test).
- A project without `ProcLevel` builds and hashes as today.
- `local_ci.sh` green on every step.
