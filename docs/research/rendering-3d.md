# Research: 3D Rendering for MCE

**Domain:** 3D rendering — RHI design patterns, forward vs deferred vs Forward+, PBR, lighting/shadows/IBL, sky/fog, post-processing, culling, and a solo-developer feature ladder.
**Stack context (binding):** C++20 / MSVC v143; OpenGL 4.5 Core (DSA, persistent-mapped staging, MultiDrawIndirect) behind a thin hand-written RHI with a hard seam — no GL above the backend; SDL3; glTF 2.0 → cooked `.mce`; solo dev, Windows 10 x64, 2–3 year horizon, 16.6 ms frame budget.
**Method:** survey → tradeoffs → committed decision, per topic. Sources are named inline: *Real-Time Rendering* 4th ed. (Akenine-Möller, Haines, Hoffman), *Game Engine Architecture* 3rd ed. (Gregory), the GPU Gems / ShaderX lineage, SIGGRAPH course notes (Karis, Lagarde, Hillaire, Wronski), AMD's Forward+ paper, Olsson & Assarsson's clustering papers, engine sources and blogs (id Tech, Frostbite, Filament, bgfx, The Forge, Diligent, wgpu, Our Machinery, meshoptimizer). No web search was available; every claim is from canonical material already in my knowledge and is cited by name so it can be verified.

---

## 1. State of the art, and what "good" means for MCE

Commercial 3D rendering in 2026 is shaped by three generations of API design:

1. **Global-state era (GL 1–3, D3D9):** one state machine, bind-to-modify, FFP evolved into shaders. Dead as a target; its *habits* still haunt tutorials (learnopengl-style global binds are explicitly forbidden by the binding guidelines).
2. **Modern-explicit era (D3D11/12, Vulkan, Metal, GL 4.5):** explicit pipeline state objects, descriptor/binding models, explicit or semi-explicit synchronization. GL 4.5 Core sits *inside* this era: it has PSO-shaped programs, persistent-mapped buffers, MDI, compute, SSBOs, and texture views — it simply lets the driver do barrier bookkeeping for you.
3. **GPU-driven era (2015→now):** indirect everything, per-cluster triangle culling, compute-based LOD selection, virtualized geometry (Nanite), deferred materials, hardware/software ray tracing (UE5 Lumen, id Tech 7's scaled-down variants). Described canonically in the SIGGRAPH "GPU-Driven Rendering Pipelines" course (Ubisoft, 2015) and its 2021 update.

**Calibration of ambition.** The product goal is "small complete games" shipped by one developer. That is the regime of *Team Fortress 2*-to-*Inside*-era visuals, not Nanite-era. Inside (Playdead, GDC 2016) shipped a full modern look — cluster forward lighting, volumetrics, temporal AA — on hardware weaker than a mid-range 2018 GPU. MCE's realistic ceiling ("good render") is: metallic-roughness PBR, ~64–128 punctual lights in a scene via Forward+, 3-cascade shadows, prefiltered IBL, procedural sky, height fog, ACES tonemap + bloom + SSAO. Every one of those techniques has a well-documented, GL-4.5-expressible formulation. Nothing on the list requires mesh shaders, bindless-only designs, or ray tracing. That honesty drives every commitment below.

---

## 2. RHI design patterns — survey

The RHI seam discipline is binding: everything above `mce::rhi` must be GL-free, and the RHI must be *modern-explicit-shaped* so a D3D12/Vulkan backend is a migration, not a rewrite. The question is which published abstraction to model. Survey of the four abstractions worth knowing, plus the two that set the reference pattern:

| Abstraction | Model | Strengths as a reference | Weaknesses / why not adopted |
|---|---|---|---|
| **wgpu (Rust)** | WebGPU spec: automatic barriers, resource-state tracking, bind-group descriptors, Naga shader translation | Cleanest *specification* of a modern-explicit surface; its state-tracking algorithm is worth reading | Automatic state tracking removes exactly the synchronization lesson the project exists to teach; Rust; breaking-change cadence |
| **bgfx** | View-centric multi-backend (GL/D3D/Vulkan/Metal); per-view state, embedded shaderc/texturec tools | Proves a thin seam can survive 10+ backends; its "views" idea ≈ MCE render passes; shader cross-compilation discipline | View model is coarser than a pass graph; API predates PSO-era shapes in places; opaque internals — you consume, you don't learn backend design |
| **The Forge (ConfettiFX)** | Modern-explicit, PSOs, explicit barriers, render graphs, shader interpreter | Best-published example of *transient resource management* and frame-graph structure in C++; used in shipped games | Very large, team-scale, requires its own shader pipeline; heavy to read; tool-shaped rather than engine-shaped |
| **Diligent Engine** | PSO-based `IPipelineState`/`IDeviceObject`, documented GL + Vulkan + D3D backends | The most readable *interface taxonomy* for a PSO-shaped RHI in C++; its GL backend shows exactly which Vulkan concepts are no-ops on GL | It is an API library, not an engine renderer; adopting it would import its resource-lifetime opinions wholesale |
| **Filament (Google)** | `Handle<HwXxx>` backend abstraction + frame graph, superb written docs (*Filament docs*, the PBR white paper) | The `Handle<Hw*>` + backend-struct pattern is the cleanest published version of exactly what MCE needs; frame-graph docs are the best introduction to transient passes | Its backend is richer than MCE needs (Metal/Vulkan/WebGL); the frame graph is more machinery than one developer should write |
| **Our Machinery / Bitsquid blogs (Gray)** | Frame-scoped allocations, explicit pass lists, no hidden state | The philosophy MCE's core already follows; "simplest thing that works" pass management | No public code; ideas only |

Two pattern families emerge. **Descriptor/PSO-heavy** (Diligent, The Forge, wgpu): model Vulkan's binding model explicitly even on GL — future-proof, heavier to build. **Command-buffer + handle** (bgfx, Filament): record commands into a buffer, translate to backend calls, reference everything by handle. MCE already committed to the second shape in `docs/architecture/rhi-renderer.md` (recorded `CommandBuffer`, generational handles, barrier calls that are no-ops on GL).

### Committed for MCE — RHI patterns

**Committed: hand-rolled command-buffer + handle RHI, Filament's `Handle<HwXxx>` pattern, Diligent's interface taxonomy, The Forge's transient-resource idea, and an explicit pass list instead of a render graph.** Specifically:

1. **Interface taxonomy** (stolen from Diligent's `IDeviceObject` set, simplified): `RenderDevice` (creation/destruction, capabilities), `CommandBuffer` (the only way to submit), and resource types `Buffer, Texture, Sampler, PipelineState, RenderPass`. All creation returns `Handle<T>`; the GL backend stores a dense array of `Hw*` structs (Filament's pattern). Nothing else is public.
2. **PipelineState is a first-class object from day one**, even though GL has no PSOs: `{shader program, vertex layout, depth/stencil state, blend state, raster state, primitive type}` hashed and cached (GL: set once at bind; future D3D12: `CreatePipelineState`). This is the single most important GL-era decision — retrofitting PSOs onto a global-state renderer is a rewrite, and the binding forbids global-state habits anyway.
3. **Barriers in the command stream, no-ops on GL.** `cmd.bufferBarrier(...)`, `cmd.textureBarrier(...)` exist in the interface from M0 with a comment "GL: implicit". Cost today: zero. Migration later: real.
4. **Transient render targets** allocated from a per-frame GPU heap (GL: a pool of `glCreateTextures` targets reused by key `size×format`; future backend: aliased placement in a placed-resource heap — The Forge's `TransientBufferAllocator` is the reference). Never create/destroy a render target per pass.
5. **No render graph.** MCE's pass list is hand-written and explicit (shadow → opaque → sky → transparent → post), like Our Machinery's approach. Render graphs (Hans-Kristian Arntzen's series; Frostbite's "FrameGraph", GDC 2017) buy automatic pass fusion and resource aliasing — machinery that pays off at team scale with many disjoint render features. One developer with a known, short pass list gets the same cache coherency from ordering the list by hand. The graph remains the documented upgrade path if the pass count explodes.
6. **Shader authoring:** GLSL 450 core compiled by `glslangValidator` at cook (binding). The RHI owns a `shaderc`-free reflection pass via SPIRV-Cross *at cook only* (UBO/SSBO names → C-side binding indices), so the runtime never guesses binding locations. This is bgfx's discipline with half the machinery.

Sketch of the committed shape (full design in `docs/architecture/rhi-renderer.md`):

```cpp
// All of mce_rhi's public surface, conceptually:
RenderDevice* device_create(const DeviceDesc&);              // GL backend behind this
Handle<PipelineState> device_create_pipeline(PipelineDesc&&); // {program, layout, states}
void cmd_begin_pass(CmdBuffer&, RenderPassDesc&&);           // transient RTs resolved here
void cmd_bind_pipeline(CmdBuffer&, Handle<PipelineState>);
void cmd_bind_ubo(CmdBuffer&, u32 set, u32 slot, Handle<Buffer>, range);
void cmd_draw_indirect(CmdBuffer&, Handle<Buffer> args, u32 count, u32 stride);
void cmd_barrier(CmdBuffer&, BarrierDesc&&);                 // GL: no-op. Future: real.
```

---

## 3. Forward vs deferred vs Forward+

The canonical comparison lives in AMD's **"Forward+: Bringing Deferred Lighting to the Next Level"** (Harada, McKee, Yang, 2012) and Olsson, Assarsson & Aila's **"Tiled and Cluster Forward Shading"** (HPG 2012); *Real-Time Rendering* ch. 20 covers the taxonomy. What shipped games chose is the real evidence:

- **Deferred** (G-buffer): UE4/UE5, most AAA of the 2010s. Pros: O(lights) shading cost independent of geometry, per-pixel light lists free. Cons: G-buffer bandwidth (3–5 render targets), material ID gymnastics, **forward-only transparency**, MSAA broken by design, and it *forces* a second pass with distinct data layouts.
- **Forward:** one pass, cheapest per-pixel, MSAA-friendly, transparency natural. Cons: naive forward re-shades per light — O(lights × fragments).
- **Forward+ / clustered forward:** forward shading + a compute pass that bins lights into a 3D grid (2D tiles × depth slices, e.g. 32×32×16); the fragment shader looks up its cell's light list. O(1) light cost per fragment, single geometry pass, transparency and MSAA free. Shipped in **AMD's Forward+ demos**, **Playdead's Inside** (GDC 2016: cluster forward, praised for material simplicity), **DOOM 2016** (id Tech 6 — cluster forward with per-cluster lists, presented at SIGGRAPH 2016), and later id Tech 7. It has become the default choice for *stylized and mid-scale* engines (Godot 4's default path is also forward+, behind its Vulkan renderer).

**Bandwidth reality on GL-era hardware.** Deferred's G-buffer costs ~4–8 bytes/px × overdraw per pass; clustered forward costs one light-grid SSBO read. On the 2012–2018 iGPUs MCE's "small games" audience will actually have, clustered forward is the *lower-bandwidth* option for ≤128 lights. The classic pro-deferred argument — "hundreds of dynamic lights" — doesn't apply: no MCE game needs them.

**GL 4.5 specifics.** Clustered forward needs only compute + SSBOs (GL 4.3), both solid. Deferred on GL needs MRT (fine) but its performance hinges on early-z and bandwidth behavior the GL driver manages less predictably than D3D12. Forward+ is simply a better fit for the backend and for MDI-based submission: opaque pass = one `MultiDrawElementsIndirect` per material/atlas bucket, exactly like the 2D sprite path.

### Committed for MCE — shading path

**Committed: forward shading for MVP, Forward+ clustered lighting at v0.5, never deferred.**

- **MVP (M2 gate):** plain forward. Lambert diffuse + Blinn-Phong specular, 1 directional light + 1 shadow map, single material system that already uses metallic-roughness *parameters* (so the asset pipeline never changes). No light count support beyond 4 punctual lights in a UBO.
- **v0.5:** cluster build (32×32 px tiles × 24 depth slices, one `GL_SHADER_STORAGE_BUFFER` per cell with light indices, rebuilt each frame in a compute pass). Lights capped at 256/cluster-list; overflow is a cook-time warning, not a runtime error. This is a ~1.5–2 week feature, well-trodden (AMD's paper has the full algorithm; the GL implementation is ~400 lines).
- **Deferred is rejected permanently for MCE, not merely deferred:** its G-buffer layout, decal story, and transparency special-cases are exactly the team-scale complexity the from-scratch philosophy says to skip, and the literature above shows cluster forward reaches the same *visual* result for this light-count regime.
- The 2D sprite path and the 3D path share one submission model (pipeline + MDI + transient passes) — a deliberate architectural unification, not a coincidence, and the main reason Forward+ is cheap here: the opaque 3D pass is structurally "a big sprite batch with lighting."

---

## 4. PBR (metallic-roughness)

The standard is settled: **Cook-Torrance microfacet BRDF** with GGX distribution (Walter et al. 2007), Smith geometry term, Schlick Fresnel; diffuse from Lambert or a Disney-style Burley term (Burley, "Physically-Based Shading at Disney", SIGGRAPH 2012). The industry parameterization is **metallic-roughness**, established by Disney's parameterization, Unreal's (*Real Shading in Unreal Engine 4*, Karis 2013), Frostbite (*Moving Frostbite to PBR*, Lagarde & de Rousiers 2014), and codified for interchange by **glTF 2.0** and the **Filament white paper** (Aubry et al.) — which is the single best self-contained reference for implementing it, including the split-sum IBL approximation (§5).

Parameter details that matter (all from Filament/Frostbite/Karis):

- **Perceptual roughness → α = roughness²** (theDisney/GGX remap). Never square in the asset, always in the shader.
- **F0 = mix(0.04, baseColor, metallic)** — 4% dielectric reflectance baked in, standard since UE4.
- **Specular k for analytic lights = α/2** (UE4's pre-2018 form; keep it — simpler than the 2018–2020 Smith-height-correlated revision, visually indistinguishable at MCE's budgets, and it's what every tutorial reference implements, which matters for debugging against RenderDoc captures).
- **Diffuse:** start with Lambert (MVP); upgrade to Burley diffuse at v0.5 (~10 lines). Skip the multi-scatter GGX energy compensation (Frostbite 2018/Filament do it) until a visible energy-loss complaint exists — at small-game scene densities it is not perceivable.
- **Color space discipline:** textures authored sRGB are sampled with `GL_SRGB8_ALPHA8` internal format (GL does the decode); all lighting in linear; ACES at the end. Getting this wrong is the #1 cause of "my PBR looks washed out" — enforce at cook (asset policy already rejects non-cooked textures).

### Committed for MCE — PBR

**Committed: glTF 2.0 metallic-roughness is the material model, Cook-Torrance GGX with UE4-era Schlick/Smith terms, Lambert→Burley diffuse, linear-lighting with sRGB decode in hardware, ACES fitted tonemap.** No material system beyond glTF's set (baseColor, metallic, roughness, occlusion, emissive, normal) until a game demands one. Clearcoat/sheen/transmission are documented non-goals (glTF extensions `KHR_materials_*` accepted at cook time only if trivial to map; otherwise ignored with a warning).

Shader sketch (the whole BRDF, minus IBL):

```glsl
// GGX/Trowbridge-Reitz
float D_GGX(float NoH, float a) {
    float a2 = a*a;
    float d = NoH*NoH*(a2-1.0)+1.0;
    return a2 / (PI * d*d);
}
vec3 F_Schlick(float VoH, vec3 f0) {
    return f0 + (1.0-f0)*pow(1.0-VoH, 5.0);
}
float V_SmithGGX(float NoV, float NoL, float a) {   // UE4-style visibility
    float k = a*0.5;
    float gv = NoL*sqrt(NoV*NoV*(1.0-k*k)+k*k);
    float gl = NoV*sqrt(NoL*NoL*(1.0-k*k)+k*k);
    return 0.25/(gv+gl);
}
```

---

## 5. Lighting: shadows, CSM, IBL

### 5.1 Direct lighting

One directional light (sun/moon) + up to 256 punctual lights (point/spot) via the Forward+ grid. Light *count* beyond that is a non-goal; light *quality* (attenuation curves, angular falloff, shadowing) is the goal. IES profiles: deferred to v1.0+, they are a texture lookup, not an algorithm risk.

### 5.2 Shadows — CSM

The technique is **Cascaded Shadow Maps** (Zhang/Eng, "Cascaded Shadow Maps", ShaderX5; Microsoft's CSM sample remains the best implementation writeup) — one depth pass per cascade, split the view frustum along Z, render the sun-lit scene into each split. Everything else is stabilization details:

- **3 cascades at MVP, 4 at v1.0.** 2048² per cascade is plenty for a 30 m draw distance game.
- **Split scheme:** practical logarithmic split (λ ≈ 0.75–0.85 between logarithmic and uniform, per the CSM literature and Microsoft's sample).
- **Texel snapping** (round the cascade's ortho projection to texel boundaries) is mandatory — without it, shadows shimmer on every camera move. This is the single most common shadow-map bug; bake it into `ShadowPass` from day one.
- **Filtering:** 3×3 percentage-closer filtering (Reeves et al. 1983) with a Poisson disk (9–13 taps) as the v0.5 upgrade. Skip variance shadow maps (Lauritzen) — light-bleeding artifacts need bias hacks that cost more debugging time than PCF taps cost GPU time.
- **Peter-panning/bias:** normal-offset bias (offset the shading position along the surface normal before projecting) — the fix recommended by the Microsoft CSM sample and most engines; depth-bias slope-scaled as a secondary.

### 5.3 IBL

The split-sum approximation (Karis 2013, "Real Shading in Unreal Engine 4") is the industry standard and fits GL 4.5 exactly:

1. **Diffuse IBL:** irradiance from the environment — prefiltered irradiance cubemap (9×9 convolution at cook) or, better, 3rd-order spherical harmonics coefficients computed at cook and stored in the material environment UBO (9 vec4s vs 6 mip levels of cube — SH is the engine-preferred form; Frostbite and Godot both use SH for ambient diffuse).
2. **Specular IBL:** prefiltered environment cubemap with roughness mapped to mip levels (PMREM, prefiltered mipmapped radiance environment map) + the 2D **environment BRDF LUT** (Karis's split-sum LUT; a 256² RG16F texture generated once at init by a tiny compute or fullscreen pass — it is scene-independent).
3. Both computed **at cook** from an HDR equirectangular source (the binding asset policy: runtime never touches image formats) — the cook step is a ~200-line cubemap convolution, well-documented in the Filament paper's IBL chapter.

**v1.0, not v0.5.** IBL requires an HDR pipeline end-to-end and adds a full asset type (environment maps, per-zone ambient). It is the last big visual jump and should land after the rest of the ladder is stable. Ambient fallback before IBL: a flat ambient term + hemisphere gradient, which is honest about being a placeholder.

### Committed for MCE — lighting

**Committed: sun + 256 punctual lights (Forward+), 3-cascade CSM with texel snapping and 3×3 PCF (4 cascades + Poisson at v1.0), split-sum IBL deferred to v1.0 with SH ambient diffuse, flat/hemisphere ambient until then.** No real-time GI, no RT, ever, on this product horizon — Lumen-style techniques need async compute or ray query hardware MCE does not target.

---

## 6. Sky and fog

- **Sky, MVP:** a full-screen (or inverted-hull cube) skybox with a baked cubemap from the environment source. Zero risk, ships with the 2D game's first 3D test scene.
- **Sky, v0.5:** procedural analytic sky. The two credible formulas are **Preetham** (1999, cheap, dated look) and **Hosek-Wilkie** (2012, better sun halo, more constants). Both have public coefficient tables; a GLSL port is ~150 lines. **Commit to Hosek-Wilkie fit with Preetham as fallback**, plus an analytic sun disk. Bruneton-style precomputed atmospheric scattering ("Precomputed Atmospheric Scattering", EGSR 2008) and Hillaire's 2020 Frostbite LUT method ("A Scalable and Production Ready Sky and Atmosphere Rendering Technique") are the v1.0 stretch if a game actually needs aerial perspective — they are true-to-physical but need a 4D LUT pipeline that is not worth building for small games.
- **Fog, MVP:** exponential-squared distance fog (the classic `exp(-d²·density²)`), one UBO, ~5 lines.
- **Fog, v0.5:** height fog (analytic integration of exp-density over the view ray; the form used since Frostbite's "Physically-Based and Unified Volumetric Rendering", SIGGRAPH 2015). Volumetric fog/light shafts (3D froxel volumes, Frostbite's method) are a v1.0 stretch — they need a 3D texture + compute scatter pass, which is a real feature with real debugging cost, not a shader tweak.

### Committed for MCE — sky/fog

**Committed: baked-cubemap skybox at MVP; Hosek-Wilkie analytic sky + sun disk at v0.5; exponential-squared fog at MVP, analytic height fog at v0.5; volumetrics and Bruneton/Hillaire atmospheres explicitly out of scope unless a shipped game demands them (ADR).**

---

## 7. Post-processing stack

Order matters more than effects. The canonical reference is Jimenez's **"Next Generation Post Processing in Call of Duty: Advanced Warfare"** (SIGGRAPH 2014) — the dual-filter bloom and the overall chain discipline come from there; ACES fitting from Narkowicz ("ACES Filmic Tone Mapping Curve", 2015) and Hill's ACES implementation notes (SIGGRAPH 2014 "AgX/ACES" lineage; Narkowicz's fit is the one-everyone-ships approximation).

**Committed chain (each stage one fullscreen pass on a transient RT):**

1. **HDR render** to `R11G11B10F` (no alpha needed; 11-bit HDR is enough and halves bandwidth vs RGBA16F) — from MVP, because retrofitting HDR into a LDR pipeline is a rewrite.
2. **[v0.5] Bloom:** dual-filter down/up-sampling (Kawase lineage, Jimenez's variant), half-res, 5 mip levels.
3. **[v0.5] Tonemap:** ACES fitted (Narkowicz), with exposure from a UBO; auto-exposure (average-luminance histogram) at v1.0 only.
4. **[v0.5] Color grading:** 3D LUT (16³ baked into a 2D atlas), loaded from an asset; identity LUT by default.
5. **[v1.0] FXAA or SMAA:** FXAA 3.11 console variant — one pass, no history, no judder; chosen over TAA deliberately (below).
6. **[v1.0] SSAO:** GTAO (Jimenez et al., Intel's "Practical Real-Time Strategies for Accurate Indirect Occlusion", 2016) over the HBAO/SSAO family; half-res, denoised by bilateral upsample. Deferred because SSAO quality is entirely in the noise/tuning and it needs depth+normal access that the forward path already has (forward does not preclude SSAO — depth-only prepass gives it).
7. Vignette/film grain: trivial, ride along with grading at v0.5.

**TAA: rejected for the foreseeable future.** Temporal AA needs robust history management, velocity buffers, and disocclusion handling — a multi-week tuning pit that fights the "deterministic draw order" goal and shadows' texel snapping. FXAA + 1080p+ target resolution is the honest solo-dev trade (this is also why CSM shimmer stabilization above is mandatory: without TAA you see every shimmer). Revisit only if a game's art direction demands smooth gradients over crisp edges.

### Committed for MCE — post

**Committed: HDR (`R11G11B10F`) from MVP; chain = bloom → ACES → LUT grade → FXAA → vignette; SSAO (GTAO) at v1.0; TAA rejected.** Every stage is a fullscreen pass on a transient target from the RHI heap — the post chain is also the *proof* that the transient-RT allocator works.

---

## 8. Culling

- **Frustum culling (MVP, CPU):** per-object sphere-vs-6-plane test (the standard from *Real-Time Collision Detection*, Ericson; plane extraction from the view-proj matrix per Gribb & Hartmann's "Fast Extraction of Viewing Frustum Planes from the World-View-Projection Matrix"). Runs during extract over a flattened scene array; SIMD-friendly by construction (6 dot products per object, SoA layout). At small-game object counts (<10k), this is enough — do not build a BVH before measurement says otherwise.
- **Draw sorting (MVP):** 64-bit key `[(material/shader 16) | (pipeline 16) | (depth 32)]` — depth sort front-to-back within pipeline buckets for early-z; the RHI's MDI argument buffer is filled in sorted order. Deterministic order is a test target (binding frame discipline).
- **Occlusion culling: deferred, with a committed plan.** GL hardware occlusion queries (`glGenQueries`/`GL_ANY_SAMPLES_PASSED`) are stall-prone when used naively and not worth it at small object counts. The credible v1.0 upgrade is **masked software occlusion culling** (Hill & Collin, "Masked Software Occlusion Culling", Intel; variant of the Hi-Z approach in "Software Occlusion Culling", Intel 2013) on the job system — ~1k lines, SIMD, no GPU stalls. **Commit: frustum + sorted MDI at MVP; reevaluate occlusion only when a RenderDoc/Tracy capture proves draw-bound on real content; then masked software occlusion, not GL queries.**
- **GPU-driven (v1.0, optional):** per-cluster triangle culling in the vertex shader + `glMultiDrawElementsIndirectCount` (GL 4.6; on 4.5 emulate the count path with a CPU-computed draw count, which is fine because culling is still CPU-side at that tier) + meshoptimizer's vertex-cache/cluster-cone optimization at cook (binding asset policy already mandates meshoptimizer at cook). Full Nanite-style virtual geometry is explicitly a non-goal.

---

## 9. The feature ladder — one developer, MVP → good

The ladder is calibrated against the binding milestones (M2 = 3D PBR + Forward+) and against what a solo developer can *debug*: every feature below has a RenderDoc-verifiable intermediate state. "Good render" is defined as: lit metallic-roughness scene with cascaded shadows, sky, fog, bloom, ACES — i.e., roughly the visual register of a *Team Fortress 2*-to-*Inside* stylized scene.

| Tier | Features | Rough effort (10–15 h/wk) | Gate criterion |
|---|---|---|---|
| **MVP (M2)** | RHI: PSOs, transient RTs, MDI; forward pass, Lambert+Phong, 1 dir light + 1 shadow map (single cascade, texel-snapped), exp² fog, skybox cubemap, ACES tonemap, frustum cull + sorted MDI submit | 4–6 weeks after 2D path works | Lit 3D scene in editor viewport; RenderDoc capture reviewed; golden image green |
| **v0.5** | PBR Cook-Torrance (Burley diffuse), Forward+ clusters (256 lights), 3-cascade CSM + PCF, Hosek-Wilkie sky + sun disk, analytic height fog, bloom + LUT grade + vignette, depth prepass | 6–8 weeks | Any glTF scene from an art pack lights believably; frame ≤16.6 ms at 1080p on dev GPU |
| **v1.0** | Split-sum IBL (PMREM + BRDF LUT + SH ambient), 4 cascades + Poisson PCF, GTAO, FXAA, auto-exposure, volumetric fog (stretch), masked occlusion culling (only if capture says so), GPU-driven cluster culling (stretch) | 8–12 weeks, cuttable | "Good render" bar above; all features captured in RenderDoc and goldened |

**Explicit cut ladder** (if a timebox slips — binding milestone rules): cut IBL → cut GTAO → cut volumetric fog → drop to 3 cascades → keep ACES + PBR + shadows (the irreducible "good" core). The single most valuable feature on this list is **the depth prepass**, because it enables GTAO, Hi-Z later, and overdraw insurance for free — it is the one v0.5 item I'd refuse to cut.

---

## 10. Risks and validation

- **GL driver variance:** the big three (NVIDIA/AMD/Intel) all implement 4.5 Core credibly; the classic hazards (mutable global state, sync stalls) are neutralized by the binding's DSA-only + fence-ring rules. Remaining risk: Intel iGPU perf on persistent-mapped rings — mitigated by the frame-slot design (3 slots, small staging) and Tracy GPU zones from M0.
- **"Looks wrong" debugging:** the two failure modes that eat solo weeks are (a) color-space mistakes and (b) matrix/convention bugs (row/column, handedness). Mitigations: the DirectXMath-as-oracle unit tests for math (binding), sRGB enforcement at cook, and a `DebugDraw` wireframe pass so normals/tangents/cascade splits are always visualizable. Every render feature merges only with a RenderDoc capture (binding profiling policy) — capture-first debugging is the habit that keeps a solo renderer alive.
- **Golden images:** dev-machine-only with a perceptual threshold (the architecture doc already excludes cross-machine pixel determinism). One canonical scene per feature tier, re-captured on CI-adjacent manual runs, not per-commit.

---

## 11. Recommendation for MCE — consolidated commitments

1. **RHI:** hand-rolled command-buffer + generational-handle abstraction; PSO-shaped from day one; barriers-as-no-ops; transient RT heap; explicit pass list, no render graph. References to steal from: Diligent (taxonomy), Filament (handles, docs), The Forge (transient resources), bgfx (tool discipline).
2. **Shading path:** forward → **Forward+ clustered at v0.5**; deferred rejected permanently for MCE's goals.
3. **Materials:** glTF 2.0 metallic-roughness *is* the material model; Cook-Torrance GGX + UE4-era terms; Lambert→Burley; linear lighting; ACES fitted tonemap.
4. **Lighting:** sun + 256 punctual; **3-cascade CSM** with texel snapping + 3×3 PCF; **split-sum IBL at v1.0** (SH ambient diffuse, PMREM specular, BRDF LUT at init).
5. **Sky/fog:** cubemap skybox → Hosek-Wilkie analytic sky at v0.5; exp² fog → analytic height fog at v0.5; volumetrics out of scope without an ADR.
6. **Post:** HDR `R11G11B10F` from MVP; bloom → ACES → LUT → FXAA chain at v0.5/v1.0; **GTAO at v1.0; TAA rejected**; depth prepass is non-negotiable from v0.5.
7. **Culling:** CPU frustum (sphere-vs-planes, Gribb/Hartmann extraction) + 64-bit-key sorted MDI at MVP; masked software occlusion only on measured need; GL occlusion queries never.
8. **Feature ladder:** MVP forward (4–6 wk) → v0.5 "PBR + Forward+ + shadows + sky + post" (6–8 wk) → v1.0 "IBL + GTAO + FXAA" (8–12 wk, cuttable per the ladder in §9). Every tier gated by a runnable artifact + RenderDoc capture + golden image.

## 12. Sources

- *Real-Time Rendering*, 4th ed., Akenine-Möller, Haines & Hoffman — shading-path taxonomy, shadows, post, culling overview.
- *Real-Time Collision Detection*, Ericson — frustum culling primitives.
- *Game Engine Architecture*, 3rd ed., Gregory — renderer architecture layers (ch. 10–12).
- "Forward+: Bringing Deferred Lighting to the Next Level", Harada, McKee & Yang (AMD/Eurographics 2012); Olsson, Assarsson & Aila, "Tiled and Cluster Forward Shading" (HPG 2012).
- Playdead, "The Rendering of Inside" (GDC 2016); id Software, "Delivering Doom's 60 fps" / id Tech 6 presentations (SIGGRAPH 2016) — shipped cluster-forward evidence.
- "Real Shading in Unreal Engine 4", Karis (SIGGRAPH 2013) — split-sum IBL, Schlick/Smith parameter choices.
- *Moving Frostbite to PBR*, Lagarde & de Rousiers (SIGGRAPH 2014); *Filament* white paper + docs (Google) — metallic-roughness implementation details, IBL pipeline.
- Burley, "Physically-Based Shading at Disney" (SIGGRAPH 2012); Walter et al., "Microfacet Models for Refraction" (EGSR 2007, GGX).
- Zhang/Eng et al., "Cascaded Shadow Maps" (ShaderX5); Microsoft Direct3D CSM sample; Reeves et al. 1983 (PCF); Lauritzen (VSM tradeoffs).
- Narkowicz, "ACES Filmic Tone Mapping Curve" (2015); Jimenez, "Next Generation Post Processing in Call of Duty: Advanced Warfare" (SIGGRAPH 2014); Jimenez et al., GTAO (Intel, 2016).
- Preetham (1999) and Hosek-Wilkie (2012) analytic skies; Bruneton, "Precomputed Atmospheric Scattering" (EGSR 2008); Hillaire, "A Scalable and Production Ready Sky and Atmosphere Rendering Technique" (EGSR 2020); Frostbite volumetric fog (SIGGRAPH 2015).
- Gribb & Hartmann, "Fast Extraction of Viewing Frustum Planes"; Hill & Collin, "Masked Software Occlusion Culling" (Intel); meshoptimizer docs (Kapoulkine).
- Arntzen, "Frame Graph / Render Graph" blog series; Our Machinery/Bitsquid blogs (Gray) — pass-management philosophy.
- Engine sources surveyed for interface shape: bgfx, Diligent Engine, The Forge, Filament, wgpu (read as references, not dependencies).
