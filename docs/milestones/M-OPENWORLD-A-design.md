# M-OPENWORLD track A (A2–A4): design

**Status:** design, written 2026-10-17 before the code (the milestone asks for an Opus plan first). Owner: the
"open-world render scale" thread. A1 (the city stress scene and its GPU budgets) runs in the RunPod GPU thread; the
steps here stay inside the budgets A1 states once it lands.

Steps covered: **A2** floating origin, **A3** HLOD, impostors and building batching, **A4** render LOD per streamed
chunk. Every feature is opt-in: a scene that adds none of the new components draws, simulates and hashes exactly as
before.

## A2. Floating origin (world rebase)

### Problem

Positions are `f32` everywhere (ECS transforms, Jolt bodies, Detour, the GPU). The spacing of floats at distance `d`
is about `d × 1.2e-7`: 0.24 mm at 2 km, 1 mm at 8 km, 4 mm at 32 km. Rendering wobbles first (the vertex shader
composes large world positions and subtracts a large camera position), physics contacts degrade next. A 1–2 km map
does not need a rebase; a GTA-sized one (10+ km) does, and the clone project after M-OPENWORLD may grow past 4 km.

### Approach: rebase the simulation, not only the render

Camera-relative rendering alone fixes the wobble but leaves physics and navigation at large coordinates, and it needs
every world-space shader path (lighting, clusters, decals, terrain splats, GI, particles) to change. A rebase moves
the whole simulated world by a whole number of grid cells when the focus gets far from the origin, so every system
keeps working near zero with no shader change. This is what Unreal ("world origin rebasing") and Unity games do for
large worlds.

- The world keeps an **integer origin**: `cell[3]` in units of `snap` metres (`i32`, so ±2^31 × 256 m). Absolute
  position = `local + cell × snap`, computed in `f64` by the helpers below. Snapping the shift to `snap` (default 256 m,
  a power of two) keeps world-anchored textures (triplanar mapping, terrain UVs that repeat every 2^k metres) seamless
  across a shift, and makes `p - delta` exact for every `|p| ≤ 2^31 × ulp`-sized value on the near side.
- A **rebase** happens between ticks (never inside a phase), when the first enabled `StreamingFocus` is more than
  `threshold` metres (default 1024) from the origin on x or z (y too when `vertical` is set). The shift is the focus
  position rounded to the nearest `snap` multiple. It is a function of the tick's state, so it is deterministic.

### Interfaces

`mce/ecs/origin.h` (new, mce_ecs; not a wide header):

```cpp
struct WorldOrigin {              // World resource, present once a rebase was configured
    i32 cell[3] = {0, 0, 0};      // the origin, in `snap` units
    f32 snap = 256.0f;
    u64 serial = 0;               // bumped by every rebase
    math::vec3 last_delta{};      // the last shift (local positions moved by -last_delta)
};
using OriginShiftFn = void (*)(World&, const math::vec3& delta, void* user);
void on_origin_shift(World& w, OriginShiftFn fn, void* user);   // modules register at install
void rebase_world(World& w, const math::vec3& delta);          // structural-free; outside phases only
struct WorldPoint64 { f64 x = 0, y = 0, z = 0; };              // an absolute position (the math lib has no f64 vector)
WorldPoint64 to_absolute(const World& w, const math::vec3& local);
math::vec3 to_local(const World& w, const WorldPoint64& absolute);
```

`rebase_world` subtracts `delta` from:

1. `LocalTransform::position` of every root (an entity without `ChildOf`); children are relative to their parent.
2. `WorldTransform::position` and the matrix translation of every entity that has one (so this tick's systems see the
   shifted pose before propagation runs again; no re-propagation and no "changed" seeds are created).
3. Every reflected component field flagged **`WorldPoint`** (a new `refl::FieldFlags` bit, `attr::WorldPoint`): a
   `Vec3` (or a fixed array of them) that holds a world-space point, such as `NavAgent3D::target`, a camera rig's
   smoothed position or a perception memory's last seen position. Directions, velocities and local offsets are not
   flagged.

It then calls every registered listener with `delta`, bumps `serial` and stores `last_delta`. Change detection is not
triggered for the shifted columns (writes go through the raw column accessors), so a rebase does not look like a
teleport to systems that compare against their own cache; the listeners shift those caches instead.

`mce/scene/floating_origin.h` (mce_scene): the policy.

```cpp
struct FloatingOrigin3D {        // component on one base-scene entity; none = no rebase ever
    f32 threshold = 1024.0f;     // rebase when the focus is farther than this from the origin
    f32 snap = 256.0f;           // shifts are multiples of this (power of two recommended)
    u32 vertical = 0;            // 1: rebase on y too (space, deep caves)
    u32 enabled = 1;
};
```

`pump_streaming` (already called by `mce_player`, play-in-editor and the MCP headless run before every tick) checks
the policy first, so every host gets the rebase with no new call. A world without `FloatingOrigin3D` is untouched:
the check returns before reading anything else, as streaming does for a world without chunks.

Streaming: chunk scenes stay authored in absolute world coordinates. `SceneStreaming::instantiate` subtracts the
current origin (`cell × snap`) from the roots it spawns, so a chunk loaded after a rebase lands in place. `SceneChunk`
entities are ordinary base-scene entities, so their boxes move with the rebase.

### Listeners (who owns which cache)

| Module | Cache | What the listener does |
|---|---|---|
| physics3d | Jolt bodies, characters, vehicles | `World3D::shift_origin(delta)`: every body `SetPosition(p - delta, DontActivate)`, every `CharacterVirtual` the same, contact cache kept (Jolt stores it body-local) |
| kit (bodies3d) | `Bodies3DState::poses` | subtract `delta`, so the shifted transforms are not read as outside moves (no collider rebuild, no character velocity reset) |
| kit (nav3d) | Detour mesh and crowd | a per-world nav offset added at the Detour boundary (queries and crowd positions); the mesh is not rewritten |
| audio bridge | last listener and source positions (Doppler) | no listener: the bridge already treats a move faster than sound as a teleport and holds the Doppler shift for that frame (as built) |
| kit (joints3d, cinematic, sequence) | world-anchored joint ends, root camera rails, absolute `MoveTo` targets | joints move their fixed-to-world side (`Constraint::NotifyShapeChanged` with an invalid body id) and the cached anchor; rails shift their points; an absolute `MoveTo` subtracts `origin_offset` |
| kit (extract) | interpolation history, TAA previous worlds, terrain and merge caches | the extract is not world-bound: it compares `WorldOrigin::serial` each frame and shifts its own caches by the cell difference |
| render | Forward3D history (previous view-projection, SSR reprojection), reflection-probe and GI-volume positions, particle rings | `Scene3D::set_origin_shift(delta)` for the frame. As built: Forward3D treats that frame as a camera cut (TAA, velocity and SSR history reset, one frame), and keys probes and GI volumes by absolute f64 positions, so no probe re-capture and no GI refill. Merged static meshes draw with a translation by the accumulated shift instead of re-merging |

GPU particles are the one cache that is not moved in v1: a shift restarts the particle rings (live particles vanish,
emitters keep emitting). The shader change needed to move them is listed as an open item.

### Not supported in v1 (documented, not silently wrong)

- Networked worlds (server and clients would need a shared origin): do not add `FloatingOrigin3D` to a networked
  scene. Open item: ignore it with one warning while a net session is active (the scene module cannot see net).
- Save games store local positions. Open item: record `WorldOrigin` in the save and restore it before the entities;
  until then a save written after a rebase loads offset by the origin.
- Luau scripts: open item `world.to_absolute` / `world.to_local`; a script that keeps raw positions across ticks in
  its own tables sees them jump by the shift.
- Kit `Blackboard` vectors mix points and offsets, so they are not flagged `WorldPoint` and do not move.
- The editor does not rebase in edit mode; play-in-editor does, and stopping play restores the edit scene as today.

### Tests (A2)

- `test_origin`: a root, a child, a `WorldPoint` field and a fixed array shift; a listener sees the delta once; an
  entity without transforms is untouched; `to_absolute(to_local(p)) == p` for points up to 100 km.
- `test_scene` floating-origin cases: no component, no rebase, hash unchanged; focus at 1100 m rebases by 1024 m once
  (threshold hysteresis: the focus at 1100 m after the shift is at 76 m); a chunk loaded after a rebase lands at
  `absolute - origin`; determinism of two runs.
- `test_physics3d`: a stack of boxes and a character keep their relative poses and velocities across a shift; a
  height-field collider is not rebuilt.
- Headless drive test: a vehicle driven 10 km along x with and without the rebase; with it, the distance between wheel
  and chassis positions stays within 0.1 mm of the start; the world hash is stable over 3 runs.

## A3. HLOD, impostors, building batching

### HLOD proxy (a chunk drawn as one cheap mesh)

A chunk far from the focus is unloaded; today it then draws nothing (holes on the skyline). An HLOD proxy is one mesh
per chunk that stands in for everything static in it.

- **Bake:** MCP tool `hlod_bake` (and later an editor command): loads the chunk scene into a scratch world, gathers
  every visible rigid `MeshRef` with an opaque or masked material (the `StaticMerge3D` rules), takes each source at
  its coarsest LOD level, merges them with `render::build_merged_mesh` (one submesh per material, positions relative
  to the chunk entity), then simplifies each submesh with meshoptimizer to `ratio` (border vertices locked so material
  seams stay closed). Objects smaller than `min_size` metres (bounding diameter) are dropped: they are invisible at
  HLOD distance.
- **As built (#534 and #540):** the bake keeps each source's own LOD levels instead of taking the coarsest,
  and the simplification moved to the cook: the `hlod` importer's `lod_levels` setting (written as 2 into new
  sidecars) adds meshopt levels to the submeshes without any, so level 0 stays the exact merge and far levels are picked
  by screen error like any mesh's.
- **Format:** the bake writes `<chunk>.mchlod`, a CHNK_MESH payload (the merged mesh, materials by texture GUID, so the
  source textures are reused, not copied). A new passthrough cook importer validates it with `MeshView::read` and emits
  it as a Mesh asset, the `.mcnav` precedent.

### Impostors (far buildings and trees)

At a few hundred metres a building is a box with a picture on it. The impostor bake produces that box.

- **Bake:** `hlod_bake mode=impostor`. The chunk's sources are grouped by root entity (a building is a root with
  children, or one `MeshRef`). Each group gets a **box impostor**: its bounding box's four sides and its top, each a
  quad textured with an orthographic albedo capture of the group from that side. Groups taller than wide by
  `card_ratio` and smaller than `card_size` (trees, poles) get **cross cards** (two vertical quads) instead. Captures
  are rasterised on the CPU (cooked meshes, cooked textures decoded through the cook's BC decoder, base colour × tint),
  alpha = coverage, so the bake runs in the CI container and is deterministic.
- **Format:** a `.glb` (one mesh, one material with the atlas as base colour, alpha mask) that the existing glTF
  importer cooks like any model. No new shader and no new render path: impostors are ordinary masked meshes and get
  culling, instancing, shadows and the quality tiers for free.

### Building batching

`StaticMerge3D` already merges a building kit's pieces per cell and material. Two gaps for a city:

1. A chunk that streams in merges all its cells in its first extract (a hitch). The merge gets a per-frame budget
   (`Kit3DExtract::set_merge_budget(cells)`, default unlimited so existing scenes are unchanged; the player sets it
   from `r.merge_budget`): cells waiting for their turn draw their children one by one.
   As built (merge budget PR): `r.merge_budget` / `set_merge_budget`, `Kit3DStats::merge_pending`.
2. Many chunks each with their own `StaticMerge3D` root: the per-group hash walk runs every frame for every group.
   Groups whose children carry no change since the last frame skip the hash (a change counter per group fed by the
   ECS change ticks of the children's columns).
   As built (#536): no change counter; the per-frame walk hashes the children's raw inputs word-wise and reads no mesh
   views (24k children: 10 ms to 3 ms), so a counter fed by change ticks was not needed yet.

## A4. Render LOD per streamed chunk

```cpp
struct ChunkLod3D {                       // kit3d component, on a SceneChunk entity of the base scene
    asset::AssetRef<asset::Mesh> proxy;     // the HLOD proxy (.mchlod), relative to this entity
    asset::AssetRef<asset::Mesh> impostor;  // the impostor (.glb), relative to this entity; optional
    f32 impostor_distance = 600.0f;         // beyond this (camera to the chunk box) the impostor replaces the proxy
    f32 max_distance = 0.0f;                // beyond this nothing draws; 0 = no limit
    u32 cast_shadows = 1;
};
```

The extract draws the proxy (or the impostor, by distance from the camera to the chunk box) whenever the chunk's
`SceneChunkStatus` is not `Loaded`; a loaded chunk draws its own entities and hides the proxy. `Loading` keeps the
proxy up, so the swap never leaves a hole. Distances are camera-based (the render), while loading stays focus-based
(the simulation). The chain for a city is therefore: loaded chunk (its meshes with their own LODs, merged by
`StaticMerge3D`) → HLOD proxy → impostor → nothing.

Tests: `test_kit3d` cases for the proxy shown and hidden by chunk state, the impostor swap by distance, no draws for a
scene without `ChunkLod3D`; a headless city sample (procedural blocks) reporting draws and triangles with HLOD on and
off, gated in `local_ci.sh` by counters (not by GPU time, which only A1 measures).

## PR plan

| # | PR | Modules | Notes |
|---|---|---|---|
| 1 | this design | docs | docs only |
| 2 | `WorldPoint` field flag | refl | seam: `refl/type.h` and `refl/macros.h` are wide headers |
| 3+4 | `rebase_world`, `WorldOrigin`, `FloatingOrigin3D`, streaming offset; physics and kit listeners, extract, Forward3D history | ecs, scene, physics3d, kit, render | shipped as one PR (no audio change needed) |
| 5 | `.mchlod` importer, `hlod_bake` (mesh), `ChunkLod3D` | cook, mcp, kit | A3 + A4 |
| 6 | impostor bake | mcp (tools) | CPU rasteriser |
| 7 | merge budget and change skip | kit | building batching |

Each PR: `local_ci.sh --task`, a changelog.d fragment, the user guide page it changes (`08-3d.md`, `25-performance.md`),
and its step row in `M-OPENWORLD.md`.

## Risks

- **Fields that hold world points but are not flagged** shift wrong after a rebase. Mitigation: an audit of every
  reflected `Vec3` field in the kit, listed in PR 4, and a debug check (`r.origin_check`) that runs a scene with a
  forced rebase every N ticks and diffs `to_absolute` positions against a run without.
- **Game modules** that cache positions in their own resources must register a listener; the guide says so.
- **Proxy pop** at the load distance: v1 swaps at once; a dithered cross-fade is an open item once A1 shows it matters.
