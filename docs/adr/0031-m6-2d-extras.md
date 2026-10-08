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
