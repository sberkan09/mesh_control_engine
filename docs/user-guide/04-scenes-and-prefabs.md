# 4. Scenes, entities and prefabs

A scene is a list of entities, each a named bag of components. The editor, an MCP agent and your
text editor all write the same file format, and the cook turns it into the binary the game loads.

## 4.1 The scene file

`assets/scenes/main.mcscene` from the template:

```json
{
  "format": "mce-scene",
  "version": 1,
  "guid": "0d4f8a7e-...",
  "name": "main",
  "entities": [
    {
      "name": "Camera",
      "components": {
        "Camera2DComponent": { "pixels_per_meter": 48 },
        "LocalTransform": { "position": [0, 2, 0] }
      }
    },
    {
      "name": "Ground",
      "components": {
        "Collider2D": { "shape": { "half": [10, 0.5] }, "density": 0 },
        "LocalTransform": { "position": [0, -3, 0] },
        "RigidBody2D": {},
        "Sprite": { "texture": "guid:5b1c...", "size": [20, 1] }
      }
    },
    {
      "name": "Spinner",
      "components": {
        "LocalTransform": { "position": [-3, 1.5, 0] },
        "Sprite": { "texture": "guid:9e02...", "size": [1.5, 1.5] },
        "Spin": { "degrees_per_second": 120 }
      }
    }
  ]
}
```

How values are spelled, by field kind:

| Field kind | Spelling |
|---|---|
| bool | `true` / `false` |
| integer, float | a number (integers must be whole and in range) |
| string (`FixedString<N>`) | `"text"` within the capacity |
| enum | the enumerator name, `"Dynamic"` |
| vec2 / vec3 / vec4 / quat / colour | a number array, `[x, y]`, `[r, g, b, a]` |
| asset reference | `"guid:<uuid>"` or `null` |
| entity reference | `"$<index>"` into this file's `entities` array, or `null` |
| nested struct | an object, `{"type": "Circle", "radius": 0.3}` |
| fixed array | an array of exactly `count` elements |

Omitted fields keep their C++ defaults, which is why `"RigidBody2D": {}` is a valid static body.
`Transient` fields must not appear. Component names are the unqualified C++ names
(`Sprite`, not `mce::kit::Sprite`), engine and game alike.

The editor saves in this exact shape (sorted components, canonical formatting), so a scene
written by hand or by an agent and then saved by the editor diffs cleanly.

## 4.2 Entities and components

An entity is nothing but an id; what it is comes from its components. The engine's components you
will use most:

| Component | Chapter | Purpose |
|---|---|---|
| `LocalTransform` | here | position, rotation (quaternion), scale: the pose of record |
| `ChildOf` | here | parent entity; the engine composes `WorldTransform` |
| `Sprite`, `TextLabel`, `Camera2DComponent`, `Parallax`, `Light2D`, `Post2DComponent`, `TilemapLayer`, `ParticleEmitter` | 7 | the 2D drawing kit |
| `RigidBody2D`, `Collider2D`, `CharacterController2D` | 9 | 2D physics |
| `AudioSource`, `AudioSpatial`, `AudioListener` | 10 | sound |
| `PathAgent` | 7 | walks to a target around walls (grid pathfinding) |
| `SequencePlayer` | 13 | scripted action lists |
| `SaveKey` | 13 | stable identity for save games |

Your game adds its own (chapter 6). Every component is a plain struct the inspector, the cook, the
scene loader and the MCP server all understand through reflection, so a new component needs no
editor or cook change.

`LocalTransform` places an entity; 2D scenes use `position.x`, `position.y` and the rotation about
Z. Rotation is a quaternion in the file (`[x, y, z, w]`); the inspector shows it as three Euler
angles in degrees, so for 2D you edit the Z lane.

## 4.3 Hierarchy

Add `ChildOf {"parent": "$<index>"}` (or drag in the Hierarchy panel) to attach an entity to
another. The child's `LocalTransform` is then relative to the parent, and the engine writes a
read-only `WorldTransform` every tick after the fixed update. Rules:

- Destroying a parent destroys its subtree.
- A `ChildOf` that would form a cycle is rejected; the entity stays a root.
- In 2D, a bodied child (one with `RigidBody2D`) is placed by physics, not by its parent, during
  play. Keep physics bodies as roots or make them kinematic and move them yourself.
- Systems that read `WorldTransform` must run after `transform_propagate` in `FixedUpdate_Post`
  (chapter 6).

## 4.4 Entity references

A component field of type `Entity` is an entity reference. In the file it is `"$<index>"`, the
position of the target in the same file. The editor's picker and MCP's `component_set` take the
target's entity id and write the index for you. At load the reference becomes the spawned entity;
an index outside the scene becomes null.

Prefab instances can reference entities inside the prefab; references from an instance to the
containing scene are resolved after flattening (section 4.6).

## 4.5 Several scenes

A project can hold any number of `.mcscene` files. `start_scene` in the `.mceproj` names the one the
game starts with; `-scene scenes/other.mcscene` (or its GUID) plays another. The game moves between
them at run time with one call from any system (section 6.10):

```cpp
scene::request_change(f.world(), "scenes/level2.mcscene");   // or "guid:<uuid>", or a Guid
```

After that tick the host (`mce_player`, the editor's play mode and every MCP run) destroys every
entity, loads the target scene and runs the Startup phase again. World resources stay, so a score
or a lives count kept in a resource carries over. A title screen, levels and win and lose screens
are simply separate scenes. Scenes may name each other in a loop (title, level, win, title): a
reference to another scene, in a string field or an `AssetRef<asset::Scene>`, is a link and never
a cook dependency. A `.mctest` checks where the run ended up with `{"scene": "scenes/win.mcscene"}`
(section 14). The editor opens one document at a time. A large world can instead stream its areas
in and out around the player (section 4.8).

## 4.6 Prefabs

A prefab is a reusable entity tree in a `.mcprefab` file. The format is the scene format with
`"format": "mce-prefab"`, no `settings`, and entity 0 as the root:

```json
{
  "format": "mce-prefab", "version": 1, "guid": "a1b2...", "name": "turret",
  "entities": [
    { "name": "Turret", "components": {
        "LocalTransform": { "position": [0, 1, 0] },
        "Sprite": { "texture": "guid:..." },
        "Health": { "hp": 50 } } },
    { "name": "Barrel", "components": {
        "LocalTransform": { "position": [1, 0, 0] },
        "ChildOf": { "parent": "$0" } } }
  ]
}
```

**Instancing.** In a scene (or in another prefab), an entity may be an instance instead of a
plain entity:

```json
{ "name": "Turret A", "prefab": "guid:a1b2...", "components": {
    "LocalTransform": { "position": [5, 0, 0] },
    "Health": { "hp": 99 } } }
```

`components` on an instance are **overrides** of the root entity's fields (and may add components
the root lacks). `LocalTransform` is always the instance's own placement. Overrides apply to the
root only; per-child overrides are not supported yet.

**Flattening.** The cook expands instances into plain entities (nested instances included), so the
runtime never sees a prefab, only rows. Editing a prefab asset re-cooks every scene that uses it.

**In the editor.**

- Drag a `.mcprefab` from the Assets panel into the viewport to create an instance.
- Instance fields show an override mark in the Inspector when they differ from the prefab.
  **Revert** drops the override; **Apply to prefab** writes the value into the prefab file for
  every instance (entity references are not applied, since they name this scene's entities).
- Open a prefab asset (double-click or "Open prefab") to edit it like a scene; Save re-cooks its
  users and the instances in your scene follow on hot reload.
- To make a **new** prefab, right-click an entity in the Hierarchy and pick **Save as Prefab**: the
  entity and its children are written to `prefabs/<name>.mcprefab` (the root first, with an
  identity transform; references that leave the subtree are cleared), cooked, and replaced in the
  scene by an instance at the same place. One undo step brings the entities back (the file stays).

**Over MCP** (chapter 14): `prefab_create_from {"entity": "Grunt", "path": "prefabs/grunt.mcprefab"}`
does the same; `entity_create {"name": "Grunt B", "prefab": "guid:...", "components":
{"LocalTransform": {"position": [4, 0, 0]}, "Health": {"hp": 5}}}` places an instance whose
components override the prefab root; `prefab_apply` and `prefab_revert` are the Inspector's two
buttons; and `scene_open` / `scene_save` take a `.mcprefab` path to edit the prefab itself.

**At run time.** `ecs::instantiate` spawns a cooked prefab from code with a replica count, a parent
and typed overrides (`PrefabOverride::of("Health.hp", 99.0f)`), which is how a game spawns enemies,
bullets or pickups it did not place in the scene (chapter 6).

## 4.7 Authoring tips

- **Name entities.** Names are how the Hierarchy, `-editor.select`, MCP's listings and your own
  debugging find things; they cost nothing at run time.
- **Author in metres.** The template camera shows 48 pixels per metre, so a 1280 x 720 window sees
  about 26.7 by 15 metres. A one-metre sprite is a sensible tile.
- **Keep runtime state out of the file.** Mark fields that only the game writes (`started`,
  `timer`, `vel`) as `Transient` (and `ReadOnly` for the inspector) so scenes stay small and the
  cook's determinism check is not disturbed by stale values.
- **A scene without a camera** still cooks and plays; the player then renders nothing useful. Put
  exactly one `Camera2DComponent` with `primary: true` in every playable scene.

## 4.8 Scene streaming (optional)

Streaming is opt-in: a scene without `SceneChunk` components plays exactly as before. To stream a
large world, split it into one small base scene (the player, the camera, anything that must always
exist) and one scene per area, then add to the base scene

- one entity per area with `LocalTransform` (the centre of the area's box) and `SceneChunk`:
  `scene` names the area's `.mcscene`, `half_extents` is half the box size, `load_distance` and
  `unload_distance` say how close a focus must come for the area to load and how far it must go for
  it to unload (keep `unload_distance` larger, so walking along a border does not reload the area
  every frame);
- `StreamingFocus` on the player (or the camera; several are allowed).

```json
{ "name": "Forest", "components": {
    "LocalTransform": { "position": [64, 0, 0] },
    "SceneChunk": { "scene": "guid:5ce4e000-2222-4000-8000-00000000000b",
                    "half_extents": [32, 32, 32], "load_distance": 16, "unload_distance": 24 } } }
```

Area scenes are authored in world coordinates; the chunk entity only places the box. When an area
unloads, every entity it spawned is destroyed, so anything that must survive (score, inventory,
opened doors) belongs on base-scene entities or in a resource. `pinned: true` keeps an area loaded
wherever the player is, `disabled: true` keeps it out; game code can flip both at run time, and it
reads `SceneChunkStatus` (`Unloaded`, `Loading`, `Loaded`, `Failed`) to know when an area is in.
These components are not in `mce/gameplay.h`: a game module that touches them adds
`#include <mce/scene/streaming.h>`.
Save games do not track areas yet, so keep `SaveKey` entities in the base scene.

`mce_player`, play-in-editor and the MCP headless run stream with no game code. The windowed
player reads area files on a background thread and instantiates at most one area per tick; headless
runs and play-in-editor load an area in the tick that wants it, so their state hashes stay
deterministic. At start every area the player's start position needs is loaded before the first
tick. A C++ host does the same with `scene::install_streaming`, `scene::pump_streaming(world,
scene::PumpMode::Settle)` after the base scene, and `scene::pump_streaming(world)` before each
`World::tick` (`mce/scene/streaming.h`).
