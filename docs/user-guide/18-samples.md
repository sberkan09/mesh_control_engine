# 18. Samples and example projects

Each sample is one milestone's running demo and a worked example of a feature set. Projects under
`projects/` are complete games made with the template. Use them as copy sources. With the
installed SDK (chapter 1) Kindle (a 2D platformer), Undelivered (a light novel) and Room3D (a 3D
room on the kit) are in `share/mce/examples/`: `mce example Kindle` copies one out and `mce build Kindle` builds it against
the SDK; the other projects and the samples are in the engine checkout only.

## 18.1 Projects (template-based games)

| Project | What it shows | Look at |
|---|---|---|
| `projects/Kindle` | a 2D platformer level with colour switches; the reference for "a game on the kit": player controller, coins, hazards, goal, crumbling blocks, moving platforms, follow camera, HUD labels, sounds, a gameplay ctest; the level was authored through MCP | `games/kindle/module/components.h`, `kindle.cpp`, `tools/author_level_mcp.py`, `tests/test_kindle.cpp` |
| `projects/PrismBreak` | a brick breaker authored entirely through MCP: title and pause as scene state, ball physics via velocity, prefab-free spawning with `Commands` | `games/prism_break/module/` |
| `projects/Undertow` | a lit-2D cave diver: `Light2D` with normal maps, particles, a rope, currents, hazards, energy; the second MCP dogfood game | `games/undertow/module/`, `assets/effects/*.mcpart` |
| `projects/Room3D` | the small 3D reference on the kit: a room authored with kit3d components, a third-person hero on the kit's `Physics3D`, a follow camera with mouse look, rocks with cooked LODs in `StaticInstances3D`, saves, scripted-input GPU tests and the LOD gate | `games/room3d/module/room3d.cpp`, `tools/gen_assets.py`, `tests/test_room3d.cpp`, `tests/lod_gate.cmake` |
| `projects/SolsticeReach` | the 3D showcase on the kit: a procedural island, eight times of day written into `Environment3D`, PBR with cascaded shadows and Forward+ lights, scattered grass and trees in `StaticInstances3D`, beams, gulls, a `TextLabel` HUD, sounds, a 3D character, saves, a bot that plays the route; the host is the stock player | `games/solstice_reach/module/components.h`, `present.cpp`, `tools/author_island_mcp.py`, `tests/test_solstice_reach.cpp` |
| `projects/Undelivered` | a light novel on the novel kit (chapter 21): a script compiled by `mce story compile` and run over one saved component, lit paper-layer sprites, LUTs, music beds, its own drawer screen through the kit's hooks, five endings reached by a bot in tests | `story/story.json`, `story/*.story`, `games/undelivered/module/undelivered.cpp`, `tests/test_undelivered.cpp` |

The research notes for the MCP-authored games (`docs/research/mcp-dogfood.md`,
`mcp-dogfood-undertow.md`, `mcp-blind-test.md`) record the friction each hit; chapter 14's
pitfalls come from them.

## 18.2 The sample ladder

| Sample | Shows | Run |
|---|---|---|
| `01_triangle` | the platform window and the render device: one triangle | `samples/01_triangle/01_triangle` |
| `02_boids` | 100k entities through the ECS, headless, the determinism harness | `sample_02_boids -ticks 600 -boids 100000` |
| `sprites_2d` | the 2D batch at 10k sprites, a tilemap arena, pixel-snapped camera, screen shake, HUD text | `sprites_2d -frames 300` |
| `03_platformer` (Ember Step) | the full 2D game: five levels, menus, best times through the save system, a game module loaded by a host, particles, music; the content shipped as Artifact A | `sample_03_platformer`, `-level N`, `-autoplay 1` |
| `scene_demo` | an editor-authored scene with no game code, played by the stock player | `mce_player -pack build/<preset>/samples/scene_demo/project/assets.mce` |
| `stream_demo` | scene streaming with no game code: a traveler patrols four areas, each its own scene, loaded and unloaded around it (chapter 4.8) | `mce_player -pack build/<preset>/samples/stream_demo/project/assets.mce -scene scenes/world.mcscene` |
| `audio_rooms` | the mixer with no game code: a listener walks through three rooms on a timeline, footsteps from a sound set with pitch variation, a reverb room, a cart that drives past with Doppler, music that ducks under a voice line, a snapshot that muffles the mix, a machine heard through two walls (occlusion) until the listener reaches it; the music, voice and hum ship as Ogg Vorbis (chapter 10) | `mce_player -pack build/<preset>/samples/audio_rooms/project/assets.mce`; `-headless 1 -ticks 720 -audio_record walk.wav` |
| `extras_2d` | parallax layers, lit sprites, 2D post effects, particles, a save round trip; assets only, played by the stock player | `mce_player -pack build/<preset>/samples/extras_2d/project/assets.mce` |
| `ui_gallery` | runtime UI: a title menu, options bound to convars, a level grid, the developer console | `ui_gallery -page options` |
| `04_diorama` | glTF kit meshes, one directional light with a shadow map, fog, node animation; Artifact B | `sample_04_diorama` |
| `04_cubes` | the M5 renderer: PBR, Forward+ point and spot lights, cascaded shadows, sky, bloom, FXAA, GPU timings; a capsule walk on 3D physics; skinned reeds (optional skinning, `G` or `-skin 0` turns the skeletons off) | `sample_04_cubes`, `-walk 1`, `-skin 0` |
| `05_ashen_court` (Ashen Court) | a third-person melee slice: `World3D` characters, a swept camera boom, FSM enemies, 3D one-shots, a triangle budget | `sample_05_ashen_court`, `-autoplay 1` |
| `07_gameplay3d` | the 3D gameplay systems with no code, built over MCP alone: grunts (a `gen_mesh` humanoid) chase a drivable cart across a baked navmesh and keep apart, the cart pushes a hinged door open, a drawbridge lowers on its motor, and the cart's bumper sensor sends the grunts it hits into ragdolls | `mce_player -pack build/<preset>/samples/07_gameplay3d/project/assets.mce`, WASD drives; `-headless 1 -input samples/07_gameplay3d/drive.input` |
| `08_movement` | 3D movement feel with no code, built over MCP alone: a humanoid runs a course (a hurdle that needs a jump, a low lintel that needs a crouch, a lift up to a ledge) with acceleration, coyote time and `Locomotion3D` animation under a spring-arm camera, while three karts race two laps on `VehicleDriver3D` | `mce_player -pack build/<preset>/samples/08_movement/project/assets.mce`, WASD moves, Space jumps, C crouches; `-headless 1 -input samples/08_movement/drive.input` |
| `physics2d_headless`, `physics3d_headless` | physics-only determinism scenes | `-ticks 1200` |
| `net_headless` | the optional networking module: a server and a client in one process over loopback, snapshot replication, a client-driven player ([Multiplayer](20-multiplayer.md)) | `sample_net_headless -ticks 300` |
| `shop_demo` | purchases and ads against the fake store and ad network: gems and "remove ads" bought, a rewarded continue, interstitials between levels, restore, a relaunch that keeps the entitlement, record and replay ([Purchases and ads](22-purchases-and-ads.md)) | `sample_shop_demo -ticks 1850` |
| `liveops_demo` | live services against local stand-ins: achievements and a leaderboard on a folder "platform", a cloud restore after reinstall, a remote config value that changes the game after relaunch (and survives airplane mode), analytics that consent gates, record and replay ([Live services](23-live-services.md)) | `sample_liveops_demo -ticks 1850` |
| `steam_demo` | the stock player on Steam against a stand-in `steam_api`: Steam Input, the overlay holding the game, rich presence, achievements, a leaderboard, a Steam Cloud save, the depot started the way Steam starts it ([Steam](26-steam.md)) | `ctest -R sample_steam_demo` |

Windowed samples accept `-frames N` and `-capture out.ppm`; most accept `-headless 1 -ticks N` and
print a state hash.

## 18.3 Where to look for a recipe

| I want to... | Read |
|---|---|
| write a platformer controller | `projects/Kindle/games/kindle/module/kindle.cpp` (`kindle_player`) |
| make pickups, hazards and a goal with sensors | the same file (`kindle_triggers`) |
| follow the player with the camera | `kindle_camera` |
| show HUD text | `kindle_present` and the `HudText` component |
| animate a sprite from frames | `kindle_present` (`set_texture`) |
| switch something on and off in physics | `kindle_blocks` (`World2D::set_enabled`) |
| drive enemies with a state machine | `samples/05_ashen_court/game/court.cpp` |
| build a menu with widgets | `samples/ui_gallery/main.cpp` |
| render 3D | `samples/04_cubes/main.cpp` |
| write a gameplay test | `projects/Kindle/tests/test_kindle.cpp` |
| author a level from a script through MCP | `projects/Kindle/tools/author_level_mcp.py` |
| build a custom input map | `samples/03_platformer/main.cpp` |
| sell a product or show an ad | `samples/shop_demo/main.cpp` (`shop_game`), [Purchases and ads](22-purchases-and-ads.md) |
| unlock an achievement, sync a cloud save or read remote config | `samples/liveops_demo/main.cpp` (`liveops_game`), [Live services](23-live-services.md) |
| save best times or progress | `samples/03_platformer/main.cpp` (the save system) and section 13.3 |
