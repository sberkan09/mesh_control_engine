# ADR-0031: M6 2D extras (particles, parallax, lit 2D, 2D post)

**Status:** Accepted (2026-10-09). **Milestone:** M6 (2D extras). **Requirements:** FR-R2D-070 (particles),
FR-R2D-050 (parallax), FR-R2D-090 (lit 2D), FR-R2D-100 (2D post).
**Design:** `docs/architecture/ecs-gameplay.md` §2.9, `docs/architecture/rhi-renderer.md` §3.2–§3.4,
`docs/architecture/assets.md` §2, `docs/research/rendering-2d.md`.

**Context.** The renderer draws everything 2D through one sprite batch (sprites, tilemap chunks, text). M6 adds the
extras Ember Step needs. The design already fixed the shape of each: particles are simulated by a gameplay-side system
and emitted as `SpriteInstance`s ("the renderer has no particle code"), parallax is "data on the layer, zero renderer
code", lighting and post are renderer features gated to v0.5.

## 1. Particles (FR-R2D-070)

1. **Preset asset.** `.mcpart` (JSON, MCE-native) cooks to `CHNK_PART` (`ChunkType::ParticlePreset`, reserved since
   M2): a fixed 144-byte `ParticlePresetChunk` with the texture GUID, rate / burst / duration / cap, `[min, max]`
   ranges for lifetime, speed and spin, direction ± spread, an emission box, gravity, drag, start/end size and color,
   and an additive flag. Angles are authored in degrees and cooked to radians. The texture is a cook dependency.
2. **Component + resource.** `kit::ParticleEmitter` (preset ref, seed, layer/order, `playing`, `destroy_when_done`)
   is the authored part. The live particles are not component data: they sit in the `kit::ParticleSystem` world
   resource, one structure-of-arrays pool per emitter entity, so the archetype tables (and `World::state_hash`) never
   see thousands of rows churn. `ParticleSystem::hash()` covers every live particle for determinism checks.
3. **Simulation.** `install_particles` adds `particles_advance` to `FixedUpdate_Post` (after physics; after
   `transform_propagate` when the hierarchy is installed first). Per emitter, in ECS order: age, retire past lifetime
   (swap-remove), integrate (semi-implicit Euler, gravity then `1 / (1 + drag·dt)` damping), then emit the start burst,
   requested bursts (`ParticleSystem::burst`), and `rate·dt` with a fractional carry. Particles live in world space.
   Random numbers come from a PCG32 stream per emitter seeded by (`seed`, entity index): the same inputs give the
   same particles on every platform and in replays. `rate 0, duration 0` is a one-shot burst; with
   `destroy_when_done` the entity is destroyed (through Commands inside a phase) once emission ended and the last
   particle died, which is how gameplay spawns dust and impact puffs.
4. **Drawing.** `SpriteExtract::extract` asks the resource to draw: one `SpriteInstance` per particle (size and color
   interpolated over its life), textured through the extract's texture cache. They join the dynamic stream with
   everything else, so all particles of one blend state and texture are one MDI group; additive presets use the
   existing additive pipeline. Culling is the batch's.
5. **Presets resolve** from presets registered by GUID (tests, code-built effects) or the mounted assets. The asset is
   loaded, copied into the pool and released; a moved asset generation (hot reload) re-reads it.

**Budget.** A 2,000-particle burst, advanced and batched every frame: 1.27 ms in Debug on the CI container; the
optimized-build check is < 2 ms (`test_kit`).

## 2. Parallax layers (FR-R2D-050)

1. **Data on the layer.** `kit::Parallax { factor, repeat_x }` sits next to a `Sprite`, `TilemapLayer` or `TextLabel`.
   The extract draws the entity at `position + camera × (1 − factor)`, per axis, with the camera being the batch view's
   (snapped, shaken) center. 1 is the world, 0.5 half speed, 0 fixed to the camera, above 1 a foreground. A layer is
   authored where it appears with the camera at the origin, so the editor viewport (camera at the scene's camera) and
   the game agree.
2. **Draw order is unchanged** for scenes without parallax: the extract runs the old two queries with
   `without<Parallax>`, then the parallax entities. Layers are separated by sort layer anyway.
3. **Tilemaps are not re-baked** when they scroll. `SpriteBatch::add_tilemap` takes a draw-time `offset`: chunks cull
   against the shifted bounds and the group carries the offset; `Renderer::record_2d` re-sets the frame uniform with
   `view_proj × translate(offset)` only when the offset changes between groups (one extra uniform update per parallax
   tile layer). Dynamic sprites always draw at offset zero.
4. **`repeat_x`** turns one sprite into an endless background: the extract emits copies one width apart covering the
   view's horizontal extent (at most 256), each culled normally.

## 3. Lit 2D sprites (FR-R2D-090)

1. **Per sprite set.** `SpriteSetDesc::lit` + `normal_key` give a set a tangent-space normal map laid out like its
   atlas (same layers and regions; +X right, +Y up, +Z out of the screen, imported with `srgb` off). Quads of a lit set
   carry `vertex_flags::kLit`; unlit sets, text and particles never pay for lighting.
2. **Orientation in the vertex.** The 16 bits of `SpriteVertex::packed` reserved since M3 now hold the quad's rotation
   (14 bits over a turn) and its flips, so the fragment shader turns the sampled normal into world space without a
   per-sprite uniform (rendering-2d D1 stays: zero per-sprite uniforms). The vertex format is unchanged (24 bytes).
3. **Lights per batch.** `Lights2D` (ambient + up to 16 `PointLight2D`: position, height above the plane, radius,
   color, intensity) rides on the `SpriteBatch` (`set_lights`, reset by `begin`) and is uploaded once per
   `record_2d` into the pass uniform bank (`LightsU`, bank 1); the normal array binds to bank 7 (`kNormal`). Shading
   is Lambert with a `(1 − d²/r²)²` falloff toward a light `height` metres above the plane, in linear space before the
   sRGB encode. The default (white ambient, no points) draws a lit sprite exactly like an unlit one.
4. **Kit.** `Sprite::normal_map` (a texture ref) makes the sprite's set lit; `Light2D { color, intensity, radius,
   height, kind }` entities are collected by `SpriteExtract` each frame: ambient lights sum (white when there are
   none), point lights whose radius touches the view are taken in ECS order up to 16.
5. **Scene re-cook on layout change.** Adding the field showed a gap: the cook's cache key did not cover component
   layouts, so a dev build kept serving scenes cooked with the old `Sprite` size (refused at load with "re-cook").
   `IImporter::schema_hash()` now joins the key of native assets; the scene and prefab importers hash every reflected
   type's size and fields.

**Budget.** FR-R2D-090 asks ≤ 0.5 ms added GPU at 10k sprites on the dev GPU; llvmpipe cannot measure that, so the
number is open until a run on real hardware (the shader adds one texture fetch and ≤ 16 light iterations, only on lit
quads).
