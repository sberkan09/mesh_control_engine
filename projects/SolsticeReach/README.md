# Solstice Reach

The engine's 3D graphics showcase: one small island, eight lighting regimes, one evening. The
Keeper walks the island and aligns seven heliostats; every mirror aligned moves the time of day,
from a gold afternoon through dusk, blue hour, night, deep night and false dawn to the finale in
the Vault and the sunrise after it. Design: [docs/design/solstice-reach.md](../../docs/design/solstice-reach.md).

Everything on the island is procedural (`tools/gen_assets.py`): the terrain, the sea, the limestone
kit, the trees, the Keeper, the star field. No downloaded content.

## Play

Inside the engine tree:

    cmake --preset linux-gcc-debug
    cmake --build --preset linux-gcc-debug --target solstice_reach
    build/linux-gcc-debug/projects/SolsticeReach/solstice_reach

With the installed SDK (user guide chapter 1), which ships only Kindle as an example: copy `projects/SolsticeReach`
out of an engine checkout first, then `mce build SolsticeReach`, `mce play SolsticeReach`,
`mce editor SolsticeReach`, `mce mcp SolsticeReach`, `mce ship SolsticeReach --version 1.0`.

| Action | Keys | Gamepad |
|---|---|---|
| Move (camera-relative), look | WASD / arrows, mouse (Q / E turn) | sticks |
| Sprint, jump | Shift, Space | left stick click, A |
| Interact: hold on a mirror's plate to align it | F / J / left mouse | X |
| Lantern on/off | L | Y |
| Begin, new game (on the title) | Enter, N | A, Back |
| Pause (Q quits while paused), console | Esc, ` or F1 | |

Left on the title for 30 seconds, the bot walks the whole route (attract mode); any key returns to
the title. The host is the stock player, so its flags all work: `-capture_tick N -capture shot.ppm`
(quit after sim tick N and write the frame), `-input tests/walk.input` (scripted input), `-save name`
(a save slot, loaded at start and written at every clean quit), `-headless 1 -ticks N` (no window,
the bot plays; prints `N ticks, E entities, state_hash H`), `-r.stats 1`, `-r.lod 0`, `-r.msaa 1`
(user guide chapter 8).

## Layout

| Path | What |
|---|---|
| `SolsticeReach.mceproj` | project identity: name, engine version, games, start scene |
| `assets/scenes/main.mcscene` | the island, authored through the editor MCP server by `tools/author_island_mcp.py` (437 entities) |
| `assets/models`, `assets/textures` | the procedural kit, written by `tools/gen_assets.py` (72 cooked assets, 92 k triangles) |
| `assets/sfx`, `assets/input` | the three sounds (`tools/gen_sounds.py`) and the action map `controls.mcinput` |
| `games/solstice_reach/module/` | the game module: `components.h` (`Scatter`, `Glow`, `KeeperModel`, `Beam`, `HudText`, `ReachSounds`, `Heliostat`, `Sunshard`, `KeeperSpawn`, `Keeper`, `Waypoint`, `GullPath`, `Progress`, next to the kit's `MeshRef`, `PointLight3D`, `Animator`, `BoxCollider3D`), `terrain.h` (the height field, the same formula as `tools/island.py`), `solstice_reach.cpp` (the sim), `present.cpp` (the sim's state as kit components) |
| `games/solstice_reach/host/` | a `PlayerDesc`: the stock player with this game's module and pack |
| `tools/gen_terrain.py` | writes the island as a painted `.mcterrain` and its four layer materials from `tools/island.py` |
| `tools/island.py` | the island's shape: plateaus and ramps over fBm; `python3 tools/island.py` prints the steepest grade of every ramp |
| `tools/gen_assets.py` | regenerates `assets/models` and `assets/textures` (about a minute; deterministic) |
| `tools/author_island_mcp.py` | re-authors the scene through `solstice_reach_mcp` (`--mcp build/.../solstice_reach_mcp`) |
| `tests/test_solstice_reach.cpp` | the terrain port against the generator, the drawn terrain against the collider's field, the authored island, the bot's full run, determinism |

## How it is built

- The game runs on the engine's 3D kit (`mce/kit/kit3d.h`, user guide chapter 8): the scene places
  kit `MeshRef`, `PointLight3D`, `Animator`, `BoxCollider3D` and `Camera3DComponent` entities, and the
  stock player draws them. The module owns no renderer: `present.cpp` writes the camera (the boom,
  the title and finale orbits), the `Environment3D` resource from the hour (eight sky keys, the
  moon, the star map), the lantern / sconce / firefly glows, the beams, the Keeper's body and
  lantern, the gulls, the HUD's `TextLabel`s and the sounds, every tick.
- The terrain is a height field evaluated twice from the same formula: `tools/gen_terrain.py` writes
  it as `assets/terrain/island.mcterrain` (a `Terrain3D`, 257 samples over 200 m, painted sand, grass,
  dry grass and rock by the rules the old mesh chunks used; its grass is a `TerrainFoliage3D` on the
  grass layer), `module/terrain.h` builds the collider (a 1 m mesh in the kit's `Physics3D`
  world, next to the kit's box colliders) and places scatter; the test checks both agree to 1e-9 m
  on 400 points.
- `Scatter` volumes place grass, flowers, trees and stones by a seeded hash into the kit's
  `StaticInstances3D` (about 6.7 k copies in 16 m cells, culled per cell; small ones thin out past
  60 m and are gone past 120 m).
- The seven heliostats form a chain (`Heliostat::next`); each alignment lerps the hour to the next
  key and shows a journal line. The bot (`Waypoint` entities) walks the whole route in about two
  minutes of sim time, which is what the tests, headless runs and the attract mode run.
- Saves: `-save <slot>` restores `Progress`, the mirrors, the shards and the Keeper (`SaveKey`s
  9001, 9002, 9100+, 9200+); the title then offers CONTINUE.

Regenerate after changing the island's shape: `python3 tools/gen_assets.py` (it runs
`tools/gen_terrain.py` too), update
`tools/terrain_ref.txt` (the test's reference heights; see the test), then re-author the scene with
`tools/author_island_mcp.py` so every prop sits on the new ground.

## Measured on Apple M4 (Metal), before the kit

With the game's own host (replaced by the stock player in M-KIT3D; the frame-budget table and photo
mode went with it).

`solstice_reach -frames 600 -autoplay 1`, macOS release build, Metal backend (MSL 2.3 via SPIRV-Cross),
1280x720 window, vsync on; averaged over 590 frames (129,992 triangles drawn, 3,220 instances).

| stage | CPU ms |
|---|---|
| gameplay tick (ECS + World3D) | 0.095 |
| scene build (scatter cells + cull + sort + cascades + clusters) | 0.891 |
| command recording (3D + HUD) | 0.936 |
| RHI translate + submit | 6.448 |

Wall clock: 600 frames in 5.19 s, about 8.7 ms a frame (115 fps). Most of the submit time is probably the wait
for the next drawable under vsync (not measured separately). The GPU column is empty because the Metal device does not record
timestamp queries yet (`caps().timestamp_queries == false`), so the HUD shows `GPU -1.00 MS`.
