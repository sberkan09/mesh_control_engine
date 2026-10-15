# Friction backlog

Collected by mce unknown from 1 project(s).

## blocking (1)

### code-reload (1)

- module_build reports reloaded:true but the server keeps the old module: new components stay unknown (Glowmoth, solo, module_build)
  - expected: new components (Game, Moth, ...) live for the next call, as chapter 14 says
  - actual: each reload copies the build to the same name libglowmoth_loaded_<pid>_0.so (the counter never moves), so dlopen hands back the already-loaded old image; refl_list_components still lists the template Spin and the old Bob; entity_create says unknown component type Game
  - workaround: restart the MCP server (the bridge) after every module change that adds or changes components

## major (3)

### content (1)

- gen_sprite cannot be steered toward a described shape (moth, bat, lantern) (Glowmoth, solo, gen_sprite)
  - expected: a way to give a silhouette/mask or ASCII pixel grid, or a tool that writes a PNG from palette-indexed rows
  - actual: only random mirrored invaders by seed; 40+ seeds tried, none reads as a moth or a bat; no MCP tool writes a PNG from given pixels
  - workaround: draw the sprite sheet as palette-indexed ASCII rows in a small Python script, write the PNG under assets/textures, adopt it with cook_run {adopt:true}, add the CREDITS.md row by hand

### mcp-server (1)

- asset_schema {type: Tilemap} killed the MCP stdio stream (a non-JSON second line); the bridge exited and had to be restarted (Glowmoth, solo, asset_schema)
  - expected: one JSON-RPC line per response
  - actual: JSONDecodeError Expecting value: line 2 column 1 (char 1646) in the client after asset_schema Tilemap + SpriteSheet; server lost its open scene on restart
  - workaround: restarted the bridge; read the formats from mce://docs/guide/05-assets.md instead of asset_schema

### scenes (1)

- No way for game code or data to switch to another scene (title -> level 1 -> level 2 -> win) (Glowmoth, solo)
  - expected: a resource/event like RequestScene{guid} the player runtime honours, or a documented API in gameplay.h
  - actual: docs_search for change/load scene finds only architecture notes; SDK headers have ecs::load_scene(SceneView) but a module has no way to get a cooked SceneView from the pack or to unload the current scene
  - workaround: put title, both levels, win and lose screens in one scene at different world positions and drive them with a game-state machine in the module

## minor (4)

### docs (1)

- engine_capabilities feature list contradicts its own component list (Glowmoth, solo, engine_capabilities)
  - expected: features to reflect the shipped components
  - actual: features say no AnimatedSprite yet, camera follow/shake are code, SfxTrigger coming in step 4, while components lists AnimatedSprite, CameraFollow2D, CameraShake2D, SfxTrigger
  - workaround: trust refl_describe_component over the features table

### project_check (1)

- project_check unused check flags a tileset texture that .mctilemap assets reference (Glowmoth, solo, project_check)
  - expected: a texture used as a tilemap tileset counts as referenced
  - actual: info: textures/tiles_meadow.png - no scene, prefab, material, model or project source references this asset, while tilemaps/level1.mctilemap and level2 name it as tileset
  - workaround: ignore the info

### scenes (1)

- A scene that still uses a component the module no longer has cannot be opened to repair it (Glowmoth, solo, scene_open)
  - expected: open with the unknown component dropped (with a warning), or a force flag that does so
  - actual: scene_open refuses: unknown component type Spin (the template component I removed); force does not help
  - workaround: scene_new, rebuild every entity, scene_save over the old path with force (keeps the GUID)

### testing (1)

- module_test finds no tests in an SDK project although the guide promises a <game>_headless_300 smoke test (Glowmoth, solo, module_test)
  - expected: the template project registers glowmoth_headless_300 (chapter 14.3 step 6)
  - actual: ok:false, tests:[] - ctest has nothing in build/
  - workaround: rely on .mctest scenarios via test_run and ship_build smoke run
