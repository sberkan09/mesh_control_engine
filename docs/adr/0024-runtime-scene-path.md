# ADR-0024 — The runtime scene path: reflection-addressed ECS writes, `mce_scene`, the starter kit and `mce_player`

**Status:** Accepted (2026-10-08).

**Context.** M2 shipped the cook half of scenes (`.mcscene` → `CHNK_SCENE`, reflection-driven) but nothing turned a
cooked scene back into entities: `LocalTransform` was not reflected, the ECS could only be written through C++
types, there was no sprite component an authored scene could carry, and no executable played a cooked scene. The
editor catch-up (M2/M3 editor items) needs all of it: Save → Cook → load in the game (ROADMAP M2 demo) and the
run-in-player button (Appendix A4) both end in "a process loads the cooked scene".

**Decisions.**

1. **ECS identity stays `kTypeHash<T>`; reflection records carry it.** `refl::TypeRecord::type_hash` is filled by
   `MCE_COMPONENT`/`MCE_REFL_STRUCT` with `kTypeHash<T>`, so code that knows a component only by its record (scene
   loader, editor preview world) registers and addresses exactly the component `World::component_id<T>()` does.
   No hash changes, so existing state hashes are untouched. `World` gains a small type-erased surface
   (`find_component`, `has/get_raw/set_raw/remove_raw`) with the same structural rules as the typed calls.
2. **`mce_ecs` depends on `mce_refl`** (planned since M1): `Entity` is `FieldKind::EntityRef` and `LocalTransform` is
   an `MCE_COMPONENT`.
3. **`mce_scene` (L2)** instantiates a `SceneView`: every column is matched to the process registry by type name and
   checked for the cooked size/alignment before anything is spawned (all-or-nothing); `EntityRef` fields are
   stamped with the spawned entities; `AssetRef` fields keep their GUID and resolve lazily through the asset system.
4. **`mce_gameplay_kit` (L3)** holds the starter-kit components an authored 2D scene is made of — `Sprite`,
   `TextLabel`, `Camera2DComponent` — and their render extract (`SpriteExtract`), as `renderer.h` already promised.
   A sprite shows its whole texture stretched to `size`; atlas regions and animation wait for the sprite-sheet
   importer.
5. **`mce_cook` registers the engine components** (`tools/cook/engine_components.cpp`), so scenes using them cook.
   Game-module components in the cook arrive with the project template (M8).
6. **`mce_player`** (`src/player`) is the generic host for an authored scene: mount a pack, pick a scene, install
   physics2d + the audio bridge, draw the kit components, Esc pause overlay (the A9 model). It is the child process
   of the editor's Play button and has a headless determinism mode. `samples/scene_demo` is its CI content.

**Consequences.** A reflected component needs no loader, cook or player change to appear in an authored scene. The
player runs engine components only; a game whose systems live in a module keeps its own host until the editor's
Play learns to pass `-module` (play-in-editor, M5b, embeds the host instead).
