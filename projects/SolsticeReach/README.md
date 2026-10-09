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

With the installed SDK (user guide chapter 1): `mce build SolsticeReach`, `mce play SolsticeReach`,
`mce editor SolsticeReach`, `mce mcp SolsticeReach`, `mce ship SolsticeReach --version 1.0`.

| Action | Keys | Gamepad |
|---|---|---|
| Move (camera-relative), look | WASD / arrows, mouse | sticks |
| Sprint, jump | Shift, Space | left trigger, A |
| Interact: hold on a mirror's plate to align it | E / F / J / left mouse | X |
| Lantern on/off | L | Y |
| Pause, confirm, new game (on the title) | Esc, Enter, N | Start, A |
| Photo mode (from the pause menu; always after the finale) | P | Back |

Photo mode: WASD + R/F fly, Shift fast, `1`..`8` pick the hour, B bloom, G fog, T anti-aliasing,
H the HUD, C captures a `.ppm` next to the save.

Useful flags: `-autoplay 1` (the bot plays the whole route: attract mode), `-photo 1 -hour 22
-cam "@x,y,z,yaw,pitch"` (start in photo mode at a given hour and camera; the `@` keeps a negative
`x` from reading as a flag), `-frames N -capture shot.ppm` (quit after N frames and write the last
one), `-headless 1 -ticks N` (no window; prints `N ticks, E entities, state_hash H`), `-trace 600`
(headless: print the Keeper and the bot every 600 ticks), `-newgame 1` (ignore the autosave),
`-save name` (another save slot), `-r.scatter 0` (no grass).

## Layout

| Path | What |
|---|---|
| `SolsticeReach.mceproj` | project identity: name, engine version, games, start scene |
| `assets/scenes/main.mcscene` | the island, authored through the editor MCP server by `tools/author_island_mcp.py` (416 entities) |
| `assets/models`, `assets/textures` | the procedural kit, written by `tools/gen_assets.py` (72 cooked assets, 92 k triangles) |
| `games/solstice_reach/module/` | the game module: `components.h` (the game's own mini 3D kit: `MeshRef`, `Scatter`, `Light3D`, `BoxCollider3D`, `Animated`; and `Heliostat`, `Sunshard`, `KeeperSpawn`, `Keeper`, `Waypoint`, `GullPath`, `Progress`), `terrain.h` (the height field, the same formula as `tools/island.py`), `solstice_reach.cpp` (systems) |
| `games/solstice_reach/host/` | the 3D host: renderer, time-of-day keys, scatter cells, beams, HUD, photo mode, capture, the budget table |
| `tools/island.py` | the island's shape: plateaus and ramps over fBm; `python3 tools/island.py` prints the steepest grade of every ramp |
| `tools/gen_assets.py` | regenerates `assets/models` and `assets/textures` (about a minute; deterministic) |
| `tools/author_island_mcp.py` | re-authors the scene through `solstice_reach_mcp` (`--mcp build/.../solstice_reach_mcp`) |
| `tests/test_solstice_reach.cpp` | the terrain port against the generator, the authored island, the bot's full run, determinism |

## How it is built

- The engine has no 3D kit yet, so the module declares the handful of components a 3D scene needs
  and the host draws them (the pattern of `samples/05_ashen_court`, as a project: editor, MCP, cook
  and ship all work; the editor's viewport shows the 2D kit only).
- The terrain is a height field evaluated twice from the same formula: `tools/island.py` writes the
  16 terrain chunks, `module/terrain.h` builds the collider (a 1 m mesh) and places scatter; the
  test checks both agree to 1e-9 m on 400 points.
- `Scatter` volumes place grass, flowers, trees and stones by a seeded hash (about 6.7 k instances
  in 16 m cells, culled per cell and thinned with distance). They are a resource, not entities.
- The seven heliostats form a chain (`Heliostat::next`); each alignment lerps the hour to the next
  key and shows a journal line. The bot (`Waypoint` entities, `-autoplay 1`) walks the whole route
  in about two minutes of sim time, which is what the tests and the attract mode run.
- Saves: the autosave after every alignment restores `Progress`, the mirrors, the shards and the
  Keeper (`SaveKey`s 9001, 9002, 9100+, 9200+).

Regenerate after changing the island's shape: `python3 tools/gen_assets.py`, update
`tools/terrain_ref.txt` (the test's reference heights; see the test), then re-author the scene with
`tools/author_island_mcp.py` so every prop sits on the new ground.
