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
| 1 | `mce/kit/procgen.h`: streams, BSP rooms, caves, drunkard's walk, noise threshold; connectivity | SBA-171 | Sonnet | done |
| 2 | Wave function collapse: adjacency from an example tilemap or rules, pinned cells, bounded restarts | SBA-172 | Sonnet | done |
| 3 | `ProcLevel`: a kit system builds colliders, sprites and markers from a seed; also from Luau | SBA-173 | Sonnet | done |
| 4 | MCP `procgen_run`, the `reachable` assertion in `.mctest`, user guide | SBA-174 | Sonnet | done |
| 5 | `samples/10_dungeon`, a script-only variant, the perf budget and the gates | SBA-175 | Sonnet | not started |

**Gates:**
- Every generator gives the same bytes twice and in Debug and Release.
- Connectivity holds on every tested seed (a thousand per generator in the unit test).
- A project without `ProcLevel` builds and hashes as today.
- `local_ci.sh` green on every step.

## Step 1 as built

- `mce/kit/procgen.h` and `src/procgen.cpp` in `mce_gameplay_kit`: `Rng` with named child streams, `ProcGrid`,
  `generate_rooms`, `generate_caves`, `generate_walk`, `generate_noise` (`docs/engine/kit.md`). No float and no
  unordered container anywhere: noise is fixed point, the region search is a breadth-first queue.
- Every generator ends in the same `finalize`: wall border, keep the largest 4-connected floor region (the
  first found on a tie), check `min_floor_percent`, start from the `"start"` stream, exit = the cell farthest
  from the start by steps (smallest index on a tie). A missed target retries on the next attempt's stream,
  up to 16 times, then fails with a named stage.
- `tests/kit/test_procgen.cpp`: same bytes twice, a thousand seeds per generator all connected, sizes 8 to 256,
  the exit is the farthest cell, parameters shape the level, stream independence, ASCII, bad parameters, and one
  pinned hash per generator.
- Known limit: caves below about 16 x 16 can fail with the named error (too little floor survives smoothing).

## Step 2 as built

- `WfcRules`, `WfcPin`, `WfcGrid` and `generate_wfc` in `mce/kit/procgen.h` (docs/engine/kit.md). Integer only: the
  state is a `u64` option mask per cell, the pick is the fewest options (a seeded draw among ties) and a weighted
  draw, propagation is a stack. Adjacency is east/south pairs stored both ways; `from_example` learns pairs and
  weights and sorts tiles by id, so renaming or re-ordering the example does not change the rules.
- A contradiction restarts on the next attempt's stream up to `max_restarts` times; the error names the cause
  (`procgen.wfc: contradiction ...`, `... pinned cells contradict ...`, more than 64 tiles).
- Tests (`tests/kit/test_procgen.cpp`, "wfc: ..."): every neighbour pair allowed over 200 seeds, same seed same
  grid with a pinned hash, learned rules and id order, weights, pins, errors, the 64-tile cap.
- Not done: tiles with symmetry/rotation variants, overlapping-model WFC, and rules from a `.mctilemap` file (the
  example is passed as ids; step 3's `ProcLevel` reads the tilemap).

## Step 3 design note (`ProcLevel`) — for review before code

**Question.** The renderer draws a `TilemapLayer` from a baked asset by GUID (`SpriteExtract::draw_tilemap` →
`load_sync<Tilemap>`). A level made at run time from a seed has no asset. Options:

| Option | Cost | Risk |
|---|---|---|
| A. Expand into entities: wall rectangles (rows of walls merged greedily) with `Collider2D` + `Sprite`, one floor `Sprite`, marker entities | kit only; a 128x128 cave is a few hundred rectangles, not 16k tiles | per-tile art variants (autotile) are not possible; fine for the first version |
| B. Register a runtime tilemap through `AssetSystem::commit_reload` on a placeholder `.mctilemap` | tile-accurate art | `commit_reload` is editor-only today and needs a `Tilemap` container writer outside the cook library: touches the asset system, hence core |
| C. Bake only (`procgen_run bake` writes a `.mctilemap`) | no runtime change | no reroll in a running game, which is the point of a seed |

**Recommendation.** Step 3 = option A, step 4's `bake` = option C (authors who want tile art bake a result and paint
over it). Option B stays an open item and needs an Opus decision because it changes the asset system.

**Shape of A.**
- `ProcLevel { kind, width, height, seed, params…, tile_size, reroll }` is a reflected component (all fields `Save`).
  `kit::ProcLevelSystem` runs in the stock hosts, once per (kind, seed, params) change: it destroys the entities it
  made earlier (tagged with a `ProcLevelOwned { owner }` component) and builds new ones. `reroll` bumps the seed by
  one and clears itself, so a script or a button can ask for the next level through `mce.set`.
- Generated entities are ordinary entities, so they are in the `state_hash`; a project without a `ProcLevel`
  creates nothing and hashes as today. Expansion is a pure function of the component, so a replay rebuilds the same
  level.
- Markers: `Start`, `Exit` (from `ProcGrid::start/exit`) as named entities with a `ProcMarker { kind, index }`
  component that scripts find with `mce.find`.
- Luau needs no new API: `mce.set(level, "seed", n)` or `reroll = true`; the system runs in the same tick.

**Decided (coordinator, Opus review, 2026-10-16): option A.** B stays an open item; the core is not touched. Review
corrections that bind step 3:
- Rectangle merging is 2D: rows first, then equal-width neighbours merge vertically. A test pins an upper bound of
  rectangles (and so entities) per grid size.
- The `ProcLevel` entity carries a `SaveKey`; the generated entities do not. The rule is "rebuild on load".
- The (kind, seed, params) hash lives in a non-saved field of the component, not in the system's cache.
- `reroll` is read and cleared on the system's next run. The system runs in the `Pre` phase at a fixed order.
- An `on_remove` hook on `ProcLevel` deletes the `ProcLevelOwned` entities when the owner goes away.
- Walls are `RigidBody2D` Static plus `Collider2D` (`pathfinding.cpp:147` needs the body).
- `tile_size` and the pathfinding cell size are aligned.
- The system is a kit system the game registers itself, not code inside the stock hosts.
- Later, not now: runtime tilemap drawing without an asset could extend `draw_tilemap` (`sprite_extract.cpp:511`).

## Step 4 as built

- `procgen_run` (`tools_procgen.cpp`): kind, width, height, seed and the generators' knobs; returns text rows, the hash, floor
  count, start, exit and `reachable` (an independent flood fill). `count` runs consecutive seeds and lists failures only (at
  most 1000). `bake` paints into an existing `.mctilemap` (grid row 0 = map top row) with `floor_tile` / `wall_tile`.
- `.mctest` check `{"reachable": {kind, width, height, seed, count, ...}}` (`agent_sim.cpp`): passes when no seed fails; the
  note names up to five failing seeds. Shared argument parsing in `procgen_check.{h,cpp}`.
- User guide chapter 28. Tests: `test_mcp` (procgen_run), `test_mcp_playtest` (reachable).
- Not done: the check reads the generators, not a running `ProcLevel` (step 3); `procgen_run` has no WFC.

## Step 3 as built

- `mce/kit/proclevel.h`, `src/proclevel.cpp`: `ProcLevel`, `ProcLevelOwned`, `ProcMarker`, `install_proclevel` (called by
  `install_stock`, so the three hosts and a Luau-only project get it, and the hosts contain no code of it), `kit_proclevel`
  in `FixedUpdate_Pre`.
- Option A as decided: 2-D merged wall rectangles (Static `RigidBody2D` + box `Collider2D` + solid `Sprite`), one floor
  sprite, `Start` / `Exit` markers. `proclevel_max_wall_rects` is the pinned upper bound; the test also holds 128x128 of
  every kind under cells / 6.
- Review corrections: `built` (the hash of kind, size, seed, param, tile size, position and colours) is a plain member, not
  reflected, so it is neither saved nor hashed and a load rebuilds; `reroll` is read and cleared at the start of the next
  run; a destroy hook removes the owned entities; `proclevel_nav_grid` makes the pathfinding grid with the level's cell size
  and origin. The `ProcLevel` entity carries the `SaveKey` (the author adds it); built entities carry none.
- Not done: tile art (option B stays open), spawn points beyond Start/Exit, a Luau-driven sample (step 5).
