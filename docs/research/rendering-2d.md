# 2D Rendering Survey — Research for MCE

**Domain:** rendering-2d | **Date:** 2026-10-06 | **Status:** research input to architecture phase
**Scope:** sprite batching, texture atlases, tilemaps, 2D lighting (normal-map lit sprites, SDF shadows), 2D particles, text rendering (FreeType vs msdf-atlas-gen), sprite animation, camera/projection conventions — and the critical question of how 2D coexists with a 3D renderer sharing one RHI.

Committed stack context (from the technology decision): C++20 / MSVC v143, OpenGL 4.5 Core (DSA + persistent-mapped staging + MultiDrawIndirect) behind a thin RHI, SDL3, archetype ECS, cook pipeline to `.mce`, fixed-timestep sim at 60 Hz with interpolated rendering, frame-linear allocator, generational handles, editor-owns-scene. Companion docs: `docs/architecture/rhi-renderer.md` (the pass/submission design this refines), `docs/REQUIREMENTS.md` §3.3 (FR-R2D-*).

---

## 1. State of the art in 2D rendering (2026)

2D rendering is a solved *shape* with a settled consensus, unlike the 3D stack where Nanite/Lumen-class systems still move. Every serious 2D path today shares five traits:

1. **Everything is a batched quad.** Unity's URP 2D Renderer, Godot's canvas renderer, Bevy's `bevy_sprite`, libGDX's `SpriteBatch`, MonoGame's `SpriteBatch`, PixiJS's batch renderer, and SDL_Renderer's batch semantics all reduce to: order sprites CPU-side, emit quads into a large vertex buffer, draw many quads per call, sorted to keep texture/pipeline changes rare. The differences are only *where the sort key lives* and *how quads reach the GPU*.
2. **Atlas or texture-array lookup, never per-sprite texture binds.** Unity's `SpriteAtlas`, Godot's atlas-packed import defaults, and every JS renderer (PixiJS packs to a 2048² page) converge on "one texture object serves thousands of sprites."
3. **2D lighting as a shader feature, not a second pass system.** Unity URP 2D (Light2D + normal-map sprites, "2D Renderer" data) and Godot 4 (`Light2D`, `PointLight2D`, normal maps via `Sprite2D.texture_normal`) both light sprites inside the sprite fragment shader from a small per-frame light list. Neither runs a deferred 2D pipeline for 2D games.
4. **Text is SDF or MSDF at any non-baked scale.** Chris Green's Valve paper ("Improved Alpha-Tested Magnification for Valves", SIGGRAPH 2007) started it; Viktor Chlumský's msdfgen/msdf-atlas-gen (and 2019 master thesis "Multi-channel signed distance field atlases") made multi-channel SDF the quality standard. Unity ships TextMeshPro (SDF) as the default; Godot 4's font rendering is MSDF or baked-bitmap, per font.
5. **2D and 3D live in one renderer with different cameras.** Godot: one `RenderingServer`, `CanvasItem` (2D) and Node3D (3D) both submit into it, 2D is an orthographic canvas pass. Unity: URP renders either the 2D Renderer or the forward 3D renderer *per camera* (camera stacking composites them). Bevy: `Camera2d` and `Camera3d` are just marker components; 2D is a render-graph pass with an orthographic projection over the same render device. Nobody builds a separate 2D engine anymore.

What did **not** converge: tilemap architecture (Unity's `Tilemap` component grid vs Godot's `TileMap` quadrants — refactored to `TileMapLayer` in 4.3 — vs shader-based tilemaps), 2D shadow technique (Godot occluder extrusion vs Unity's shadow-caster geometry vs stylistic SDF approaches), and whether particles are CPU or GPU simulated. Those are the forks MCE must pick from below.

The canonical 2D-specific literature is thin but real: Gregory, *Game Engine Architecture* (2D rendering and sprite-batching fundamentals in the rendering chapter); Nystrom, *Game Programming Patterns* ("Object Pool", "Update Method", "Dirty Flag" — the particle-system and animation-state chapters); Gaffer On Games, "Fix Your Timestep" (the interpolation contract the renderer consumes); Sondre Lieng's Sprite Lamp work (2014) for normal-map-lit 2D; Green (2007) and Chlumský (2019) for text; the Red Blob Games "2D Visibility" article for the shadow-algorithm landscape; Inigo Quilez's "2D distance functions" article for the SDF toolbox.

---

## 2. Sprite batching

### 2.1 The surveyed options

**A. Legacy immediate/one-draw-per-sprite** (early SDL_Renderer, XNA's worst case): one draw call per sprite. Dead on arrival — 10k sprites means 10k draws, which is CPU-submit-bound at any API. Rejected.

**B. The classic dynamic batcher** (libGDX `SpriteBatch`, MonoGame `SpriteBatch`, Love2D `SpriteBatch`, PixiJS): CPU transforms each sprite into 4 interleaved vertices, appends to one big buffer, flushes when texture/blend changes. Simple, correct, still the right CPU-side *shape* — the flush-on-state-change rule is exactly MCE's sort-key grouping.

**C. Instanced quads:** one unit quad, per-instance attributes (position/rotation/scale/uv/tint) via instanced arrays; the vertex shader expands. Saves vertex bandwidth (20 B/instance vs 80 B/quad), adds a per-instance fetch. Unity's URP sprites and Bevy's quad pipeline are instanced variants of this.

**D. CPU-expanded quads + MultiDrawElementsIndirect** (what `rhi-renderer.md` §3.2 committed): quads fully pre-transformed on the CPU into a sorted vertex stream; draw calls grouped per state change and issued as MDI. Zero per-draw uniforms, zero instance indirection, trivially debuggable in RenderDoc (the vertex dump *is* the sprite list). This is the AZDO-era shape that fits the binding GL discipline (persistent-mapped staging rings + MDI), and it keeps the vertex shader a single viewProj multiply.

**E. GPU-driven 2D** (compute compaction/culling of sprite instances, like Nanite-style two-phase submission): the right answer at 100k+ sprites, a distraction at MCE's scale. The extract/cull/sort CPU path at 10k sprites is sub-millisecond; a compute detour would cost weeks for nothing. Rejected (recorded as a named non-goal).

### 2.2 Tradeoffs that actually matter

- **B vs D:** B's flush-per-texture forces draw-call counts equal to (texture switches); D's sort key puts pipeline first, so blend changes are the only forced breaks. With an atlas *array* (§3) even the page switch disappears — D degrades to B only if you insist on separate page textures.
- **C vs D:** C wins above ~50k sprites; D wins in debuggability and simplicity. At MCE's budget (NFR-T-02: 10k sprites ≈ 800 KiB of vertex data per frame against a 4 MiB staging ring) D is comfortably inside budget. Commit to D; C is a documented upgrade path that changes only the batch builder, not the pass structure.
- **Sorting is the real cost.** Painter's order for translucent premultiplied-alpha quads is non-negotiable in 2D. A 64-bit sort key with `sortLayer` above material above `sortOrder` (already designed in `rhi-renderer.md` §3.4 step 3) is the correct shape; the radix sort over ~10k records is microseconds. The lesson from Godot's canvas item sorting bugs and Unity's transparency-sorting Axis settings: *authored layer/order must dominate everything else*, or batch locality will visibly reorder art.

### 2.3 Decision

Sprite batching is **CPU-expanded premultiplied-alpha quads into a sorted persistent-mapped vertex stream, one MDI per (pipeline variant × texture array)**, exactly as sketched in `rhi-renderer.md` §3.2/§3.4. The vertex stream order is authoritative; GPU-side ordering does not exist. Quads carry a packed RGBA8 tint, flip/pivot pre-applied, and — at v0.5 — an octahedral-packed normal for lit sprites.

---

## 3. Texture atlases

### 3.1 The surveyed options

**A. Grid atlas (power-of-two cells, fixed grid):** trivial math, but wastes memory on size variance and — the classic killer — mipmap bleeding: at higher mip levels, neighboring cells bleed across cell borders. The standard fixes (NVIDIA's texture-atlas guidance: thick padding, half-texel insets, no-mips-for-pixel-art) work but cost memory and never fully fix large mip levels.

**B. Packed atlas with Skyline/MaxRects** (stb_rect_pack, Jukka Jylänki's rectpack2D — the algorithm inside Godot's atlas packer and Unity's SpriteAtlas tight packing): near-optimal occupancy; same mip-bleeding problem, worse because regions are arbitrary.

**C. Texture 2D array (`GL_TEXTURE_2D_ARRAY`)**: each "page" is a layer of one texture object; sampling with an explicit layer index. Mip levels are per-layer, so bleeding between pages is *impossible*; mips stay clean; and because all pages live in one GL object, **a sprite stream can span pages without a draw-call break** — the layer index is just vertex data. This is the same trick 3D engines use for bindless-lite (texture arrays indexed in-shader), and the same reason cascade shadow maps go into a 2D array in `rhi-renderer.md` §3.5.

**D. Virtual textures / megatextures**: a streaming subsystem for open worlds. Massively out of scope. Rejected.

The binding dependency policy closes the door on new runtime libs, and the cook tool list too — so the packer question matters: stb_rect_pack is *not* on the approved list, and everything not listed requires an ADR before the first `#include`.

### 3.2 Decision

**Cook to a 2D array texture.** Each sprite set cooks to one `GL_TEXTURE_2D_ARRAY` of 2048×2048 RGBA8_SRGB layers (BC7 at v0.5 per FR-AST-050); regions are packed per layer with a hand-written ~150-line Skyline packer in `mce_cook` (small, well-understood, deterministic — satisfies FR-AST-030; avoids an ADR for stb_rect_pack, though the ADR route stays open if packing quality ever matters). Mip generation: **off by default** (pixel-art default is point sampling; NEAREST/NEAREST with no mips), opt-in per texture for high-res art, in which case the array's per-layer mips make bleeding moot. Runtime sampling needs no gutter, no inset, no padding tricks.

Cooked shape: a `sprite-set` `.mce` chunk = { array texture payload (or BC payload at v0.5), packed region table: `name → {layer, uvRect, worldSize}` }, referenced by GUID from sprite components. `uvRect` is stored in texels and normalized at load; sprites reference a region by handle, never by raw rect (FR-R2D-020's "texture region" becomes a packed-region handle + flip/tint — stable under re-packing, which a raw uvRect would not be).

Consequence for batching: "one MDI per atlas page" (FR-R2D-010's acceptance criterion) is refined to **one MDI per (blend/pipeline state × sprite-set)** — usually *one* MDI for an entire 2D frame, strictly better than one per page, because the page is a vertex-carried layer index. This is a letter-change to FR-R2D-010's RenderDoc check, not a weakening: the expected capture shows fewer draws, never more. A doc note should record it.

---

## 4. Tilemaps

### 4.1 The surveyed options

**A. Entity-per-tile** (the naive ECS reflex): 32×32 chunks → 1024 entities × 30 chunks = 30k static entities for one screen-sized level. Archetype blowup, sort-key noise, useless inspector rows. Rejected — this is the "OOP object per thing" trap wearing ECS clothing (the same lesson as `existing-engines.md` §2.1 on legacy GameObjects).

**B. Per-frame instanced tiles**: rebuild a culled instance list each frame from a tile grid component. Flexible (runtime-modifiable tiles), pays CPU every frame for tiles that never move. Unity's `Tilemap` renderer effectively does a batched variant of this internally.

**C. Cook-baked chunk meshes** (Unity's Tilemap chunk meshing; Godot's quadrant meshing — the `TileMapLayer` refactor exists precisely because quadrant rebuilding was too slow): bake each chunk's static tiles into one vertex/index buffer at cook (or editor-save), draw visible chunks as ordinary geometry, rebuild a chunk's buffer only when its tiles change. CPU cost at runtime ≈ zero; edits are chunk-local re-bakes.

**D. Shader tilemap**: one fullscreen quad; the fragment shader converts screen→world→tile coords and samples tile-ID textures (the approach popularized by various "single-draw-call tilemap" articles). Zero draw calls and free infinite scrolling, but collision, picking, and editing get *harder* (everything lives in a lookup), and it diverges from the sprite pipeline — a second visual system to maintain. Rejected as the main path; noted as a possible parallax-background trick only.

### 4.2 Decision

**Option C, fully integrated with the sprite pipeline.** A tilemap is authored in the editor as a grid component over a palette of packed sprites (FR-EDT-120's rectangle-fill painting is the M1 authoring path; the full grid tool is its evolution). Cook:

1. Splits the grid into 32×32-tile chunks.
2. Bakes each chunk into a static quad list in **SpriteVertex format, referencing the same sprite-set array texture** — so chunks flow through the *same* batch, same uber shader, same MDI as sprites. There is no tilemap renderer.
3. Runs meshoptimizer's vertex-cache optimization over each chunk buffer at cook (already a cook-time dependency), deterministic by construction.
4. Exports per-tile collision strips (static AABB list per chunk) to the physics `.mce` chunk — the tilemap is a rendering *view* of data, and physics reads its own cooked copy; the runtime never asks the renderer about tiles.

Animated tiles (water, torches): a small per-chunk override list of tiles whose region is driven by a flipbook clip (§8); at extract, their quads are emitted into the normal dynamic sprite stream instead of the baked buffer. Static tiles never touch the CPU after cook.

Entity-per-chunk only for editor interaction (one entity per chunk for painting/picking hit-tests), never per tile. Orthogonal grids only for M1/v0.5 (Ember Step is a platformer); isometric/hexagonal tiles are a recorded non-goal until a committed game needs them — the data format keeps a `projection` enum so it is an add, not a rewrite.

---

## 5. 2D lighting

### 5.1 Normal-map lit sprites

The Sprite Lamp technique (Sondre Lieng, 2014) and its industrial children (Unity URP 2D lights, Godot `PointLight2D` with `Sprite2D` normal maps) established the standard: each sprite optionally carries a tangent-space normal map; the sprite shader loops over the frame's light list, computes N·L with per-light radial falloff, and adds the result. Ambient is a constant (or gradient) term. It is cheap: a handful of ALU ops per light per pixel, no extra passes, and it degrades gracefully (no normal map → N = up, lighting becomes distance falloff).

The alternatives — deferred 2D light-add passes (render light quads additively into a light buffer, then multiply) — buy per-sprite shadow blending but cost an extra full-screen pass plus an extra render target, and break single-pass batching. Unity's 2D default and Godot's default both light in the sprite shader. At MCE's scale (≤64 lights in `PassU` bank 1, per `rhi-renderer.md` §3.6) the in-shader loop is the right call; a light-buffer texture is the escape hatch if light counts ever grow, which the budget says they will not.

### 5.2 Shadows — the fork MCE is allowed to skip (for now)

FR-R2D-120/130 mark skeletal 2D animation and shadow-casting 2D lights **Won't**. The survey still owes a decision for re-entry:

- **Occluder extrusion** (Godot `LightOccluder2D`, Unity URP's ShadowCaster2D): author closed occluder polygons; per light, CPU-extrudes silhouette edges away from the light and renders them into an R8/stencil mask texture that the sprite shader samples (or tests). Proven, fast, author-friendly (occluders double as collision edges). Unity's and Godot's choice. The catch: per-light geometry work and mask passes.
- **SDF shadows**: rasterize occluders into a global 2D SDF texture (recomputed on occluder change); each light's shadow is a fragment-shader ray-march over the SDF — the 1D soft-shadow trick from Inigo Quilez's "2D distance functions". Gorgeous soft penumbras, *one* texture serves *all* lights (compare: one mask per light), trivially resolution-independent. The catch: global-SDF updates cost on occluder change, light leaks through thin geometry if the SDF is under-resolved, and debugging is less obvious than polygons.
- **Visibility polygons** (Red Blob Games, "2D Visibility"): exact CPU-computed light polygons per light. Beautiful for a handful of lights, O(occluders × lights) CPU per light per frame; wrong default for many lights.

**If re-entered via slip note:** occluder-extrusion masks are the default (they match the editor's polygon authoring and the physics-adjacent occluder data MCE would already have), with the scene SDF as the stylistic option for a dedicated atmospheric game. Do not start 2D lighting with shadows — start unlit (M1), add normal-map lighting (v0.5, FR-R2D-090), and treat all shadow-casting as stretch.

### 5.3 Decision

- **M1:** unlit + tint only. `LIT_2D` does not exist yet.
- **v0.5 (FR-R2D-090):** single-pass lit sprites — the sprite uber-shader's `LIT_2D` variant loops over ≤64 point lights from bank 1, sampling the sprite's octahedral-packed normal (already reserved in `SpriteVertex`), radial intensity falloff, additive ambient. No light pass, no light buffer, no second pipeline (same rule as 3D particles). Sketch:

```glsl
// sprite.frag — LIT_2D variant sketch
vec3 n = octDecode(v.normalOct);               // or vec3(0,1,0) when absent
vec3 albedo = texture(atlas, vec3(v.uv, v.layer)).rgb * v.tint;
vec3 light = u_ambient.rgb;
for (int i = 0; i < u_lightCount; ++i) {
    vec2  d   = u_lights[i].pos.xy - v.worldPos;
    float att = clamp(1.0 - length(d) / u_lights[i].radius, 0.0, 1.0);
    att *= att;                                  // quadratic falloff
    light += u_lights[i].color.rgb * (att * max(dot(n, normalize(vec3(d, 0.35))), 0.0));
}
outColor = vec4(albedo * light, albedo.a);       // premultiplied-alpha aware
```

- **2D post (FR-R2D-100)** composites the same way the 3D post chain does — one HDR RGBA16F target for the 2D pass when post is enabled, LDR otherwise. No split framebuffer policy: it is the *same* transient-target machinery from `rhi-renderer.md` §3.7.
- **Shadow casting: deferred per §5.2.** Any re-entry goes through an ADR + slip note per the milestone-gate rule.

---

## 6. 2D particles

Surveyed: CPU-simulated (Nystrom's *Game Programming Patterns* particle chapter — the classic object-pool emitter), GPU-compute-simulated (modern default for 100k+ particles), and hybrid. The decider is the binding simulation discipline: **determinism of the fixed-step sim is a test target** (FR-R2D-070's "determinism test covers particle sim"). GPU simulation makes determinism a GPU-replay problem; CPU SoA simulation at 2k–10k particles is microseconds and testable in the headless harness.

**Decision:** CPU simulation in gameplay-side systems over SoA arrays (`pos[]`, `vel[]`, `life[]`, `color[]`, `size[]` — SIMD-friendly, allocation-free via the frame/persistent arenas), emitted into the renderer as `SpriteInstance`s during extract — the renderer keeps *zero* particle code (the `rhi-renderer.md` §3.4 contract, FR-R2D-070). Emitters are data: point/area shapes, spread, gravity/drag curves, color/size-over-life, burst schedule — serialized through reflection like everything else (binding reflection rule). Additive particles select the additive-blend pipeline variant; the batch builder groups by pipeline first, so additive and alpha-blended particles cost one extra MDI, nothing more.

Numbers for the budget table: 2,000-particle burst (FR-R2D-070 gate) is ≈ 2k × 80 B = 160 KiB of quads/frame — trivial; 10k sustained particles still fit the 4 MiB ring with headroom. Tracy zones at sim step and emit.

---

## 7. Text rendering: FreeType vs msdf-atlas-gen

### 7.1 Survey

- **Runtime FreeType glyph caching** (freetype-gl-style): rasterize glyphs on demand into a runtime atlas, evicting as needed. Highest fidelity *and* the heaviest runtime: FreeType is a real C library with real attack surface, and it violates the closed runtime dependency list outright. Rejected for MCE (runtime never rasterizes).
- **stb_truetype coverage atlas baked at cook** (current FR-R2D-060 commitment): 8-bit anti-aliased glyph bitmaps, point-size-baked. Dead simple, zero new cook tooling, perfectly fine for a fixed HUD size. Blurs when scaled away from the baked size, and multi-weight/multi-size needs re-baking.
- **(M)SDF at cook** (Green, SIGGRAPH 2007; Chlumský's msdfgen/msdf-atlas-gen): bake per-glyph signed distance fields once; the runtime shader threshold-samples (`median(r,g,b)` for MSDF) and stays crisp at any scale and rotation, with effects (outlines, glow) nearly free in-shader. msdf-atlas-gen is a cook-time-only binary (it uses FreeType *internally*, at cook, which keeps the runtime list clean and matches the "cook-time-only" dependency class). MSDF's multi-channel trick fixes single-channel SDF's corner rounding on diagonals.
- **Shaping**: per-glyph advance + a baked kerning table covers ASCII/Latin/Cyrillic BMP text — every game in MCE's scope (REQUIREMENTS §5 product shape). Complex scripts (Arabic, Devanagari, Indic) genuinely need HarfBuzz; that would be a runtime or editor-side ADR *only if* a committed game requires it. Recorded as an escape hatch, not a dependency.

### 7.2 Decision

Text is a **cook-time font pipeline feeding the same sprite batch**, in two stages:

- **M1:** stb_truetype-baked coverage atlas (already committed by FR-R2D-060; zero new tooling, zero timebox risk).
- **v0.5:** switch the font chunk to **MSDF baked by msdf-atlas-gen** (cook-time-only dep — the written ADR names it under the approved cook-only class). The `.mce` font chunk is format-tagged from day one (`coverage8` | `msdf`), so the swap is a cook + shader change, with *no* caller-visible API change: layout code asks for a `TextLayout` (measured quads), the renderer emits quads through the batch, and the only difference is the fragment path (median-of-3 threshold vs plain alpha sample).

Runtime layout API (hand-rolled, ~300 lines): UTF-8 decode → glyph lookup → advance/kern accumulation → line breaking on whitespace (word wrap) → alignment. `DebugDraw::text2d` (rhi-renderer §2.6), HUD text (FR-R2D-060), and the deferred `mce::ui` [v1.0] all consume the same layout function — one text path, three callers, no parallel systems (the two-UI-systems lesson from `existing-engines.md` §2.1).

---

## 8. Sprite animation

**Committed: flipbook clips, events on the fixed tick, no interpolation of frames.** A clip is a `.mce` table (`frame list → {layer, uvRect}`, fps, loop/once/ping-pong, authored event indices — FR-R2D-030's "footstep event fires on the authored frame index ±1 tick"). Playback state lives in a gameplay component advanced by the fixed-step update; the renderer reads the *current frame index* during extract. Frame changes are stepped, never lerped — pixel art reads *wrong* when cross-faded, and stepping keeps animation render-deterministic at any display refresh.

What was considered and rejected: skeletal/mesh-deformed 2D animation (Spine-style) — explicitly Won't per FR-R2D-120, and correctly so: it imports an entire skinning/attachment/deformation stack that no committed MCE game needs, and Unity/Godot's own 2D skeletal systems exist for a market segment MCE is not shipping. If a future game concept needs it, the 3D skinning variant machinery (rhi-renderer §8, FR-R3D-060 sketch) is the re-entry point, not a bespoke 2D skeleton system.

Editor consequence: the inspector previews clips with frame scrubbing through the same clip tables the runtime reads (editor-owns-scene; the editor serializes clip components through reflection like every other component).

---

## 9. Camera and projection conventions

Binding math conventions decide most of this: **right-handed, Y-up, meters, radians, column vectors, column-major, GL NDC z ∈ [-1, 1]** (math-vs-DirectXMath oracle-tested). The 2D-specific commitments:

- **World units are meters.** Pixel art maps to meters at a per-art-pack pixels-per-meter scale (default `PPM = 48`: a 48 px tile is 1 m; 1080p at zoom 1 shows 22.5 m vertically). PPM lives in the sprite-set chunk, not in the engine — the renderer never sees a pixel count that isn't already meters or texels.
- **The ortho projection, right-handed Y-up, sketch:**

```cpp
// Camera2D → viewProj. View: translate(-p) · rotate(-θ); flip nothing.
// Y-up world maps directly to Y-up NDC; +X right. Depth unused (see below).
Mat4 orthoRH(float wMeters, float hMeters) {
    float l = -wMeters*0.5f, r = -l, b = -hMeters*0.5f, t = -b;
    return { {2/(r-l), 0, 0, 0},  {0, 2/(t-b), 0, 0},
             {0, 0, -2/(f-n), 0}, {-(r+l)/(r-l), -(t+b)/(t-b), -(f+n)/(f-n), 1} };
}
```

- **Depth is disabled for the 2D world pass.** Painter's order via the sort key is authoritative; z-fighting between authored layers is designed out, not mitigated (rhi-renderer risk R5's authoring rule). Depth stays *off* even in mixed 2D/3D scenes — layer ordering composites, depth would fight.
- **Pixel snap** (FR-R2D-040): snap the *view translation* so world texel boundaries land on device pixels post-projection (zoom-quantize the camera scale to integer texel ratios when `pixelSnap` is set), then apply screen shake *after* snapping (shake is allowed to shimmer; game-critical art is not). Golden-image scroll pair verifies it.
- **Layer model:** `sortLayer` (authored, 16 bits in the sort key) is the 2D stacking authority; `sortOrder` refines inside a layer; z is reserved for 2.5D compositing with 3D views. Parallax (FR-R2D-050) is per-layer camera scroll factors — data on the layer, zero renderer code.
- **Fixed logical resolution option** [v0.5]: pixel-art games render to a fixed logical size (e.g. 640×360) upscaled with integer scaling + letterbox (SDL3 gives the window rect; the renderer letterboxes the viewport). This is a `Camera2D`/view flag, not a separate render path — the target is just smaller.
- **Screen shake helper** (FR-R2D-040): a small procedural noise/decay utility writing post-snap view offsets; lives beside the camera components in ecs-gameplay's starter kit.

---

## 10. How 2D coexists with 3D in one RHI — the critical question

### 10.1 The question, precisely

Given one `RenderDevice`/`CommandBuffer` RHI and one renderer (`mce_render`), is 2D (a) an orthographic *pass* inside the same frame, (b) a separate *layer/subrenderer* with its own submission path, or (c) built on a *shared sprite/material system* with 3D? And what, exactly, is shared?

### 10.2 Options and what they cost

| Option | Shape | Cost / risk |
|---|---|---|
| **A. 2D as an orthographic pass in one frame** | One renderer, one extract→cull→sort→batch spine; 2D is a camera interpretation + a sprite material family; pass list orders 2D vs 3D | None beyond the sprite batch itself; needs a layer/blend discipline (below). This is Godot's and Bevy's shape. |
| **B. Separate 2D subrenderer** | A `Renderer2D` class beside `Renderer3D`, each with its own extract/cull/sort/batch | Doubles the machinery, forks the sort key, forks golden tests, invites the two-paths-forever tax (`existing-engines.md` §2.1). Rejected. |
| **C. Fully shared sprite/material system with 3D** | Sprites are just billboards of one material system; one uber-material | Couples 2D's premultiplied painter's semantics to PBR's opaque/transparent passes; per-sprite material uniforms contradict the packed-vertex sprite design. Rejected as *full* sharing; partial sharing via common infrastructure is free and automatic (same RHI, same sort key, same batching machinery). |

### 10.3 Decision

**2D is an orthographic camera interpretation plus a sprite material family inside the single renderer — not a second renderer, not a special-case path.** Concretely, in the terms already committed by `rhi-renderer.md`:

- **Shared, and shared at the right altitude:** one `RenderDevice`/`CommandBuffer` (the RHI is 2D-agnostic by construction — it has no concept of sprites), one extract→cull→sort→batch→record pipeline (§3.4), one 64-bit sort key with `passId`/`sortLayer`/material/depth fields, one staging-ring and frame-slot discipline, one transient-target pool, one golden-image and RenderDoc regime. `SpriteInstance`s, `MeshInstance`s, lights, and particles all come out of the same extract phase; the only 2D-specific machinery is the quad batch builder and the camera interpretation.
- **2D world-space layer:** sprites/tiles render in their own pass (or pass group) with an ortho viewProj, depth test off, premultiplied-alpha blend, one MDI per (pipeline variant × sprite-set). **Screen-space layers** (HUD, debug text, editor overlays) are the same batch machinery with a pixel-space ortho camera — no second batching system.
- **Mixed 2D/3D scenes** (editor preview, 2.5D moments): compositing is by *pass order + layer*, driven by the sort key's pass field (the v0.5 Ashen Court pass list already renders transparent 2D sprites into the 3D view). The rule: 2D/3D interleaving is authored through layers, never through depth buffering.
- **What is deliberately NOT shared:** material instances (sprites carry everything in vertices; 3D uses `MaterialU`+`ObjectData`), pipelines (sprite uber-shader family vs PBR family), and culling details (sprite AABB reject vs frustum planes). Sharing the *submission spine* while keeping the material models separate is exactly what lets both paths stay simple.
- **Proof the seam holds:** none of the above required a single new RHI concept. The 2D path is expressible entirely in existing `CommandBuffer` calls (`beginPass`, `bindPipeline`, `setVertexData`, `setDrawArgs`, `drawIndirect`). That is the test — if 2D ever needs an RHI change that isn't also a 3D need, the design has leaked.
- **Anti-goals** (so the shape doesn't erode): no retained 2D scene-object model (no `CanvasItem`-style node tree — the ECS + submission model replaces it); no `Renderer2D` class; no ImGui-family 2D UI path for shipped games (`mce::ui` goes through the sprite batch when it lands, per the binding architecture authority).

---

## 11. Frame-loop integration (the binding disciplines, applied to 2D)

- **Fixed timestep + interpolation** (Gaffer On Games, "Fix Your Timestep"): sim advances at 60 Hz; extract lerps `prev → curr` transforms with `alpha` (double-buffered transforms per FR-ENT-040). Sprite animation frame index steps on ticks, not render frames (§8) — so an uncapped 144 Hz display shows identical animation timing with smoother movement.
- **Allocation:** every per-frame 2D structure (sprite instance array, sort scratch, quad vertex scratch, chunk culled list) is a frame-linear-allocator bump (16 MiB/slot budget, NFR-F-03). Zero heap allocation in extract/cull/sort/batch — asserted in debug, verified by ASan + Tracy.
- **Uploads:** quads/indirect args go through the slot's persistent-mapped staging rings via `CommandBuffer::setVertexData`/`setDrawArgs`; the slot fence (`glFenceSync`/`glClientWaitSync`) is never touched by 2D code — the device owns it (binding GL discipline; seam discipline keeps every `gl*` call inside `mce_rhi_gl`).
- **Profiling from day one:** Tracy zones at sprite extract, radix sort, quad batch build, and per-pass GPU (`glQueryCounter`); RenderDoc checklist for the 2D path reads: one MDI per (blend state × sprite-set), no per-sprite binds, expected vertex byte count ≈ sprites × 80 B.
- **Budget anchors (NFR-T-02):** 10k sprites ≈ 800 KiB vertices/frame (< 20% of the 4 MiB ring), batch build well under 1 ms CPU, GPU submit p99 < 4 ms. These numbers are re-verified per milestone in the budget table, not assumed.

---

## 12. Recommendation for MCE — committed decisions

| # | Decision | Rationale in one line |
|---|---|---|
| D1 | **2D is a pass in ONE renderer** — ortho camera + sprite material family over the shared extract→cull→sort→batch spine; no `Renderer2D`, no second command path | Forked 2D/3D renderers double test surface and fork determinism (§10) |
| D2 | **CPU-expanded premultiplied-alpha quads + MDI**, sorted CPU-side by the 64-bit key; instancing only if >50k sprites ever shows in Tracy | Debuggable (vertex dump = sprite list), inside budget, zero per-sprite uniforms (§2) |
| D3 | **Texture 2D array atlases** (2048² layers, cook-time Skyline packing, hand-written packer); region handles in cooked chunks; mips off by default | Kills mip-bleeding *and* page-switch draw breaks; deterministic cook (§3) |
| D4 | **Refine FR-R2D-010's acceptance** from "one draw per atlas page" to "one MDI per (blend state × sprite-set)" — strictly fewer draws | The array texture makes per-page binds obsolete (§3.2) |
| D5 | **Tilemaps = cook-baked chunk quads** (32×32 tiles) flowing through the sprite batch + chunk-local re-bake on edit + collision strips exported to physics; entity-per-chunk for editor hit-testing only | Zero runtime tile cost; one pipeline; no entity-per-tile blowup (§4) |
| D6 | **2D lighting: single-pass N·L in the sprite uber-shader**, ≤64 lights in bank 1, octahedral normals in `SpriteVertex`; no light pass, no light buffer | Matches URP/Godot practice; no second pipeline; budget-sized (§5) |
| D7 | **2D shadows stay Won't** (FR-R2D-120/130); on re-entry, occluder-extrusion masks are default, scene-SDF is the stylistic option | Proven-by-Unity/Godot default; SDF is a stylistic add, not the foundation (§5.2) |
| D8 | **Particles: CPU SoA sim → `SpriteInstance`s at extract**; additive via a second pipeline variant | Determinism-testable per binding sim discipline; renderer has zero particle code (§6) |
| D9 | **Text: cook-time only; stb_truetype coverage atlas at M1 → MSDF via msdf-atlas-gen at v0.5**, format-tagged font chunk, layout through one shared `TextLayout`; HarfBuzz only via ADR if a committed game needs complex scripts | No runtime font dependency; the swap is data + shader, never callers (§7) |
| D10 | **Animation: flipbook clips with authored events on the fixed tick**; frames step, never lerp; skeletal 2D stays Won't | Matches FR-R2D-030/120; cross-fading pixel art reads wrong (§8) |
| D11 | **Conventions: meters + Y-up + RH; depth test off in 2D; sortLayer authoritative; pixel-snap view translation with shake applied post-snap; PPM lives in sprite-set data** | Deterministic ordering, no z-fights, stable art scale (§9) |
| D12 | **Particles/lit-sprites/post all reuse the transient-target, staging-ring, fence, and golden-image machinery unchanged** | The 2D path must never fork the frame infrastructure (§11) |

**FR traceability:** D1–D4 → FR-R2D-010/020; D5 → FR-EDT-120 + Artifact A tile content; D6 → FR-R2D-090; D8 → FR-R2D-070; D9 → FR-R2D-060 (refinement, not contradiction: "SDF-optional" becomes the planned v0.5 path); D10 → FR-R2D-020/030; D11 → FR-R2D-040/050; FR-R2D-080/100 unchanged (editor viewport/post are renderer-owned, not 2D-specific).

## 13. Risks

| # | Risk | Mitigation |
|---|---|---|
| R1 | Atlas re-packing churn breaks tuned layouts | Regions are handles, not rects (D3); re-pack changes only the sprite-set chunk; pixel tests are per-region |
| R2 | One-vertex-stream sorting regresses if sprites exceed budget | Radix sort is O(n) and Tracy-gated; the instanced variant (D1's upgrade path) is a batch-builder-only change |
| R3 | MSDF swap at v0.5 regresses HUD legibility at small sizes | MSDF atlas bakes with 4 px range + fallback coverage path per font; golden images for both formats |
| R4 | 64-light shader loop costs more than FR-R2D-090's 0.5 ms budget | Light loop compiles per-variant with `LIT_2D` light-count specialization; Tracy GPU zone + golden at 64 lights; drop to 16-light variant is a cook enum, not a redesign |
| R5 | Tile chunk re-bake cost during editor painting | Re-bake is one 32×32 chunk (≤ ~4 KiB quads) at editor-save cadence; Tracy zone + budget line |
| R6 | Layer-order vs material-order confusion in sort key | Authoring rule (editor warning) that layer/order dominates; golden scene with overlapping translucent sprites from different pages |

## 14. References

- Gregory, *Game Engine Architecture* — 2D rendering/sprite fundamentals, layering.
- Nystrom, *Game Programming Patterns* — "Object Pool", "Update Method", "Dirty Flag".
- Gaffer On Games, "Fix Your Timestep" — fixed-timestep + interpolation (binding frame discipline).
- Sondre Lieng, Sprite Lamp (2014) — normal-map lighting for 2D sprites.
- Chris Green (Valve), "Improved Alpha-Tested Magnification for Valves", SIGGRAPH 2007 — SDF text.
- Viktor Chlumský, msdfgen / msdf-atlas-gen; "Multi-channel signed distance field atlases" (2019) — MSDF atlases.
- Inigo Quilez, "2D distance functions" — SDF toolbox incl. analytic soft shadows.
- Red Blob Games, "2D Visibility" — visibility-polygon shadow algorithms.
- Unity URP 2D Renderer docs (Light2D, SpriteAtlas, sprite normal maps); Godot 4 docs (CanvasItem, PointLight2D, LightOccluder2D, TileMapLayer); Bevy `bevy_sprite`/`Camera2d`; libGDX/MonoGame `SpriteBatch`; PixiJS batch renderer — convergence evidence in §1.
- meshoptimizer (Zeal optimized vertex caches at cook, per binding asset policy).
- Companion MCE docs: `docs/architecture/rhi-renderer.md`, `docs/REQUIREMENTS.md` §3.3, `docs/research/existing-engines.md`.
