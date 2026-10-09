# Undertow

The second MCP dogfood game (`docs/research/mcp-dogfood-undertow.md`): a lit-2D cave-diving drone,
made with the MCE project template and every scene edit done through the project's MCP server
(`undertow_mcp`). You fly a salvage drone through a flooded cave system, find five reactor cores,
tow each one back to the dock on a rope and keep the hull in one piece.

- **Thrust** in four directions; the drone has mass, drag and a little gravity, so it drifts.
- **Boost** (hold Space) is faster but drains energy; energy regenerates when you glide.
- **Tether** (press J near a loose core) hooks the nearest core on a rope. Cores are heavy and pull
  you around in currents; J again drops it. Dock with a core on the rope to deliver it.
- **Hazards:** lasers (closed gates), crushers on a kinematic path, drifting mines (two hull),
  currents that push you (and a towed core) around. Impacts above a speed cost hull; the dock and
  the relay station are checkpoints that repair it. Dying respawns at the last checkpoint.
- **Switches** open the gates for a while; **beacons** mark the cores still out there.
- Four zones: the dock cavern, the crusher gallery, the mine field below and the current shafts.

Controls: WASD / arrows thrust, Space boosts, J tethers / drops, Esc pauses, ` opens the console.
`scenes/attract.mcscene` is the same level with the drone on an `Autopilot` that fetches the
first core (the headless test and the MCP `run_headless` check).

Engine surface used: `Sprite` + `normal_map` with `Light2D` (a headlamp, beacons, pod glow, ambient),
`Post2DComponent` (vignette, LUT, chromatic aberration on impacts), `Parallax`, `ParticleEmitter`
(`.mcpart` presets: thrusters, explosions, sparks, current flow, bubbles), physics2d (dynamic drone,
kinematic crushers and mines, static rock, sensors, a `DistanceJoint` rope), `AudioSource` + `PlaySfx`
on two buses, code-defined `SequenceRegistry` sequences (the delivery fanfare and the win), a
`Text` HUD, and `RunStats` (a ReadOnly/Transient component the MCP `inspect` reads back).

| Path | What |
|---|---|
| `Undertow.mceproj` | project identity: name, engine version, games, start scene |
| `assets/scenes/main.mcscene`, `attract.mcscene` | the level, authored through `undertow_mcp` (`tools/author_level_mcp.py` is the record, 4 248 tool calls) |
| `assets/textures/` | generated art (`tools/gen_art.py`: rock + normal map, drone, pods, hazards, the teal LUT) |
| `assets/sfx/` | generated sounds (`tools/gen_sfx.py`: hum, impacts, chimes, a 16 s music loop) |
| `assets/effects/*.mcpart` | particle presets written by the authoring script |
| `assets/fonts/` | ProggyClean (`CREDITS.md`) |
| `games/undertow/module/` | the game module: `components.h` (19 components), `undertow.cpp` systems + `install`, `module.cpp` entry, `types.cpp` for the cook |
| `games/undertow/host/` | the thin host executable (the engine's player runtime + this game) |
| `tests/test_undertow.cpp` | ctest `undertow_level`: thrust and energy, gates and lasers, mines and checkpoints, death and respawn, the rope and the delivery, the win, a scripted run that docks a core, attract determinism |

From the engine checkout:

    cmake --preset linux-gcc-debug                      # picks up projects/Undertow
    cmake --build --preset linux-gcc-debug --target undertow undertow_editor undertow_mcp undertow_level
    build/linux-gcc-debug/projects/Undertow/undertow                                  # play
    build/linux-gcc-debug/projects/Undertow/undertow_editor -project projects/Undertow  # edit
    python3 projects/Undertow/tools/gen_art.py && python3 projects/Undertow/tools/gen_sfx.py
    python3 projects/Undertow/tools/author_level_mcp.py --mcp build/linux-gcc-debug/projects/Undertow/undertow_mcp
    ctest --test-dir build/linux-gcc-debug -R undertow
    python3 scripts/ship.py --project projects/Undertow --smoke --smoke-headless   # the ship zip in dist/

The authoring script rebuilds both scenes from scratch (it clears them entity by entity), cooks,
runs the attract scene headless for 2 400 ticks and checks through `inspect` that the autopilot
delivered a core, then writes a timing / friction report next to the transcript it kept.
