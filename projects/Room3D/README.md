# Room3D

The smallest 3D game on the engine's 3D kit (`mce/kit/kit3d.h`, M-KIT3D): a walled room on a
meadow, a hero you walk with WASD, turn the camera with the mouse and jump with Space, lamps, a
spinning fan with a spot light and nine hundred scattered rocks. The host is the template's thin
`PlayerDesc`: the stock player draws any scene with a `Camera3DComponent` in 3D.

| Path | What |
|---|---|
| `Room3D.mceproj` | project identity: name, engine version, games, start scene |
| `assets/scenes/main.mcscene` | the scene: `Environment3D`, a camera with `Camera3DComponent` + `FollowCamera`, `MeshRef` models with `BoxCollider3D`, `PointLight3D` / `SpotLight3D`, `Animator` clips, the `RockField` |
| `assets/models/`, `assets/textures/` | glTF models and PNG textures, written by `tools/gen_assets.py` |
| `games/room3d/module/` | the game: `Hero` (a capsule character in the kit's `Physics3D`), `FollowCamera` (a third-person boom pulled in by walls), `RockField` (a seeded scatter into `StaticInstances3D`) |
| `games/room3d/host/` | the thin host executable (the engine's player runtime + this game) |
| `tests/` | `test_room3d.cpp` plays the cooked scene with scripted input; `walk.input` drives the windowed and save tests |

The kit does the rest: `kit3d_colliders` builds the static bodies at Startup, `kit3d_physics` steps
the world, `kit3d_animators` advances and cross-fades the clips, and the player's extract draws
every `MeshRef`, light and static instance between the last two ticks (`-r.stats 1` or F3 shows the
3D line: triangles, draws, instances, lights).

From the engine checkout:

    python3 projects/Room3D/tools/gen_assets.py          # re-make the models, textures and scene
    cmake --preset linux-gcc-debug                      # picks up projects/Room3D
    cmake --build --preset linux-gcc-debug --target room3d
    build/linux-gcc-debug/projects/Room3D/room3d     # play (Esc pauses and frees the mouse, ` opens the console)
    ctest --preset linux-gcc-debug -R room3d

With the installed SDK: `mce build Room3D`, `mce play Room3D`, `mce editor Room3D`.
