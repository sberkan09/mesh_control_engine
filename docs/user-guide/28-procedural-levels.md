# 28. Procedural levels

A level can come from a number: give the engine a seed and a size and it draws the same dungeon or cave every
time, on every machine. Because the engine is deterministic, the seed is the level: a bug report or a test says
"seed 41" and everyone sees the same map. The generators are in `mce/kit/procgen.h` (ROADMAP M-PROCGEN); this
chapter covers looking at a level, checking a run of seeds, baking one into a tilemap, and the `ProcLevel`
component that builds one in a running game.

## 28.1 The generators

Each generator takes a width and a height (8 to 512 cells) and a seed, and returns a grid of walls and floor with
a start cell and an exit cell. The exit can always be reached from the start; a generator that cannot manage it
for a size reports a named error rather than returning a broken level.

| Kind | Looks like | Main knobs |
|---|---|---|
| `rooms` | rooms joined by corridors (binary space partition) | `leaf_min`, `room_min`, `corridor` (1 or 2) |
| `caves` | organic caverns (cellular automaton) | `fill_percent`, `iterations` |
| `walk` | a winding passage (drunkard's walk) | `coverage_percent`, `turn_percent` |
| `noise` | smooth blobs (threshold of value noise) | `scale`, `octaves`, `floor_percent` |

`min_floor_percent` (rooms, caves, noise) makes a generator retry until the level has at least that share of floor.
Grids smaller than 16 cells a side can fail for `caves`; use `rooms` or `walk` for tiny maps.

Wave function collapse (`generate_wfc`) fills a grid with tiles that obey neighbour rules, from rules you add by
hand or learn from an example grid; it is C++ only for now.

## 28.2 Looking at a level: `procgen_run`

```json
{"kind": "rooms", "width": 32, "height": 16, "seed": 7}
```

returns the level as text rows (`#` wall, `.` floor, `S` start, `E` exit), its hash, the floor count and whether
`E` can be reached from `S` (checked again by a separate search, not taken from the generator). The same arguments
give the same level. Pass `"ascii": false` to leave the rows out.

`"count": 100` tries 100 seeds in a row (`seed` to `seed + 99`) and lists only the ones that fail, so
"does this size work on every seed?" is one call.

## 28.3 Baking a level into a tilemap

`"bake": "maps/dungeon.mctilemap"` with `"floor_tile"` and `"wall_tile"` paints the level into an existing tilemap
(make one with `asset_create`, its width and height at least the grid's). The top row of the grid becomes the top
row of the map. Tile numbers are tileset cells as in `tilemap_paint`: 0 is empty, n draws cell n-1. Paint over the
result by hand or with `tilemap_paint`; a baked map is an ordinary `.mctilemap` and does not change when the
generator does.

## 28.4 Testing seeds in a `.mctest`

A level generator is only worth shipping when every seed works. A `.mctest` (chapter 14) can say so:

```json
{
  "name": "caves are always connected",
  "ticks": 1,
  "assert": [
    { "reachable": { "kind": "caves", "width": 64, "height": 48, "seed": 1, "count": 200 } }
  ]
}
```

The check passes when all 200 seeds generate and have their exit reachable from their start. When it fails, the
note names the first failing seeds (`seed 17: exit not reachable from start`), which you can feed straight to
`procgen_run`. It takes the same arguments as `procgen_run` (kind, width, height, seed, count and the knobs) and no
comparison.

## 28.5 A level in the game: `ProcLevel`

Put a `ProcLevel` on an entity (with a `LocalTransform`: its position is the level's bottom-left corner) and the game
builds the level beside it when the scene loads:

```json
{ "components": {
    "LocalTransform": { "position": [0, 0, 0] },
    "ProcLevel": { "kind": "Caves", "width": 64, "height": 48, "seed": 41, "tile_size": 1 },
    "SaveKey": { "key": 1 } } }
```

- Walls become static bodies with box colliders (rows of wall cells merge into rectangles, and equal rectangles in
  neighbouring rows stack, so a 128x128 cave is a few hundred entities, not 16384). A solid sprite per rectangle and one
  floor sprite draw it (`wall_color`, `floor_color`, `layer`).
- Two `ProcMarker` entities, `Start` and `Exit`, stand at the centres of the generator's start and exit cells.
- `path_markers: true` also stands a `Path` marker (`index` 1 to n) on each cell of a shortest walk from Start to Exit, n being the
  Exit cell: a guide for hints, bots and tests.
- `param` overrides one knob of the generator (0 = its default): caves wall fill %, walk coverage %, noise floor %.
- Everything built carries `ProcLevelOwned`. Only the `ProcLevel` entity is saved (give it a `SaveKey`): loading a scene
  or a save rebuilds the level from the seed, so a replay gets the same level. Destroying the `ProcLevel` entity
  destroys what it built.
- Change `seed`, `kind`, the size or the position and the level is rebuilt on the next tick. `reroll: true` adds one to the
  seed and clears itself on the next tick.
- A grid the generator cannot make (caves smaller than 16 cells a side) logs a warning once and builds nothing.

From Luau no new API is needed (a script moves a 2D body to the Start marker with `mce.teleport2d`; writing
`LocalTransform.position` alone does not move a body). A script finds the markers and asks for the next level:

```lua
function update()
    local level, exit_at
    mce.each("ProcLevel", function(e) level = e end)
    mce.each("ProcMarker", function(m)
        if mce.get(m, "ProcMarker", "kind") == "Exit" then
            exit_at = mce.get(m, "LocalTransform", "position")
        end
    end)
    -- when the player is at exit_at: mce.set(level, "ProcLevel", "reroll", true)
end
```

The built entities appear at the end of the tick that rebuilds the level (the engine's usual spawn timing), so a script
sees the new level on the next tick. For pathfinding, `proclevel_nav_grid` (C++) returns a `NavGrid` with the level's
cell size and origin, so `mark_static_colliders` lines up with the walls.

`projects/Dungeon` is a whole game built this way: three scenes (rooms, caves, walk), one script, and a
`.mctest` per generator in which an autopilot following the Path markers clears seeds 1 to 10.

## 28.6 Things to know

- Changing a generator's code changes every seed's level. The engine pins sample hashes in its tests; a game that
  saves seeds should save the engine version with them.
- Random numbers come from named streams of the seed, so adding a step to one generator does not shift the others.
- Nothing here uses the clock, global random state or floating point, so Debug and Release agree.
