# 28. Procedural levels

A level can come from a number: give the engine a seed and a size and it draws the same dungeon or cave every
time, on every machine. Because the engine is deterministic, the seed is the level: a bug report or a test says
"seed 41" and everyone sees the same map. The generators are in `mce/kit/procgen.h` (ROADMAP M-PROCGEN); this
chapter covers what you can do today: look at a level, check a run of seeds, and bake one into a tilemap.

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

## 28.5 Things to know

- Changing a generator's code changes every seed's level. The engine pins sample hashes in its tests; a game that
  saves seeds should save the engine version with them.
- Random numbers come from named streams of the seed, so adding a step to one generator does not shift the others.
- Nothing here uses the clock, global random state or floating point, so Debug and Release agree.
