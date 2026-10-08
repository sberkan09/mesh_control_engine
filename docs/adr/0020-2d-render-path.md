# ADR-0020 — The M3 2D render path as built: where it deviates from the sketches

**Status:** Accepted (2026-10-07).

**Context.** `rhi-renderer.md` §3.2–§3.5 and `research/rendering-2d.md` D1–D11 commit the 2D path: CPU-expanded
quads over 2D-array atlases, a 64-bit sort key with a radix sort, one MultiDrawElementsIndirect per
(blend × sprite set), cook-baked tilemap chunks through the same batch, cook-baked font atlases, a pixel-snapped
Camera2D. Building it surfaced a few places where the sketches are under-specified or contradict themselves.

**Decisions.**

1. **Extract lives with the starter kit, not in `mce_render`.** The `Sprite`/`Camera2D`/`TextLabel` components are
   kit components (L4, ecs-gameplay §2.9) and `mce_render` is L3, so the renderer cannot query them. `mce_render`
   exposes the post-extract vocabulary (`SpriteInstance`, `View2D`, `SpriteBatch`, `TextPlacement`); the kit's
   `RenderExtract` systems fill a `SpriteBatch` in ECS iteration order. Submission order breaks sort-key ties, which
   keeps draw order deterministic without an entity index in the key.
2. **`SpriteVertex` is 24 bytes**, not the "20 B" in §3.2 — the listed fields (`f32x2 pos, f32x2 uv, u32 color,
   u32 packed`) add up to 24. 10k sprites are ≈ 960 KiB/frame, still < 25 % of the 4 MiB geometry ring.
3. **MDI shape.** Dynamic quads of a frame are uploaded per *segment* (a run of dynamic groups between tilemap
   groups) through the staging ring; a shared persistent u32 quad index buffer (16384 quads) plus per-command
   `base_vertex` addresses them. A group longer than 16384 quads becomes several commands of the same MDI.
   Tilemap layers keep their baked quads in a persistent vertex buffer; each visible 32×32 chunk is one command,
   the layer one MDI.
4. **sRGB output is encoded in the sprite shader** (`FrameU.params.x`), not via `GL_FRAMEBUFFER_SRGB`: the
   backbuffer and the editor targets are plain RGBA8. Cooked colour textures upload as `RGBA8_SRGB` (the cook's
   `Srgb` flag), tints are packed sRGB-encoded, and blending happens in display space, as in every 2D engine.
5. **Cooked textures upload as 2D arrays**, even with one layer, so the sprite path samples one `sampler2DArray`
   type. The 3D path (M4) can sample layer 0 or ask for `TextureUpload::array = false`.
6. **Font atlas at bank slot 11, sprite atlas at slot 6** (§3.3 table); a vertex flag picks the coverage path, so
   text and sprites share one pipeline per blend state.
7. **Sort key within a layer is material-then-order** exactly as §3.4 lays the bits out. Sprites of *different*
   sprite sets in the same layer therefore group by set before `sortOrder`; authors who need interleaving use
   layers (rhi-renderer R5's authoring rule).
8. **Tilemaps bake at runtime today** with the same chunking the cook will use for `CHNK_TILE` (`bake_tilemap`),
   so the editor's paint loop and the future cooked chunk share one format.

**Consequences.** The 2D path needs no RHI concept beyond 2D-array textures, `update_texture` and MDI
(`set_draw_args`/`draw_indirect`), which the 3D path needs too (rendering-2d §10.3's seam test holds).
