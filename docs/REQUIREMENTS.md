# Mesh Control Engine — Requirements (Authoritative)

**Project:** Mesh Control Engine (MCE) · **Document class:** Requirements · **Date:** 2026-10-06
**Status:** AUTHORITATIVE. This document synthesizes and supersedes the three lens drafts (`docs/drafts/requirements-{product,experience,feasibility}.md`). Where this document and a draft disagree, this document wins; the disagreement is recorded in Appendix A (Deferred debates).
**Binding context (from the technology decision, not re-litigated here):** C++20 / MSVC v143, OpenGL 4.5 Core behind a thin RHI (DSA, persistent-mapped staging rings, MultiDrawIndirect), SDL3, Dear ImGui (docking), hand-written archetype ECS / math / 2D physics / audio mixer / glTF importer / cook pipeline / reflection, glTF 2.0 + PNG/JPG + WAV as interchange only, cooked versioned `.mce` containers, fixed 60 Hz sim with interpolated rendering, editor owns the canonical scene, runtime never parses JSON/XML/PNG, no exceptions/RTTI in the runtime core, ImGui is editor/debug-only, Tracy from M0, RenderDoc-verified render features.

---

## 0. How to read this document

- **Priority** is MoSCoW. The product lens' rule stands: *a requirement is Must only if one of the anchor use cases (§2) is unshippable without it.*
- **Tier** (MVP / v0.5 / v1.0) says when it lands. Where priority and enthusiasm conflict, the tier wins (feasibility constitution). Features enter a tier only by written slip note.
- **Tie-break rule, decided now:** for Must-haves, **feasibility wins ties**. When the product lens wants a feature earlier than one developer can build it, the feasibility tier holds and the conflict is recorded in Appendix A — never resolved by silently slipping scope.
- Every FR carries a **measurable acceptance criterion** (a test, a stopwatch number, a CI check, or a named human's demonstrated action). If a requirement cannot be verified, it does not belong in this document.
- FR IDs are stable and gapped by 10 for later insertion. Indicative milestone tags (M0–M5) refer to the binding milestone gates; the architecture and plan documents own sequencing.

---

## 1. Vision & design pillars

Mesh Control Engine is a from-scratch 2D/3D game engine with an integrated editor, built by one developer on Windows 10 x64, whose measure of success is: **small complete games ship from it, and the developer understands every subsystem in it.** Six pillars are the tie-breakers for every requirement argument below:

- **P1 — Shippable beats impressive.** A finished 2D platformer and a finished 3D game outrank any technically impressive subsystem that does not serve them.
- **P2 — The engine is the curriculum.** Every subsystem on the binding from-scratch list (math, archetype ECS, 2D physics, audio mixer, glTF importer, cook pipeline, RHI, reflection, renderer) is hand-written; a library may replace only *boilerplate*, never *architecture*.
- **P3 — Editor-first iteration.** Content changes — levels, tuning, prefabs, scenes — must never require recompiling C++; if a content tweak needs a rebuild, that is a product bug.
- **P4 — Deterministic and observable by default.** Fixed 60 Hz sim, hash-tested determinism, Tracy zones from M0, RenderDoc-verified rendering, frame and memory budget tables per milestone.
- **P5 — Feasibility is a requirement, not a hope.** ~10–15 focused hours/week is the honest budget; the MVE, the tiers, and the pre-agreed cut ladder are the mechanism by which the project survives its own ambition (a solo engine dies of ungated scope, not technology — the Hazel lesson).
- **P6 — One path, thin seams.** One code path per concern (no parallel old/new systems), one canonical scene owned by the editor, one thin RHI seam confining all GL calls, one closed dependency list.

---

## 2. Target use cases

Every Must requirement in §3 traces to one of these. UC-A and UC-B exist in **two size classes**: the MVE artifacts (MVP) and the full anchor games (v0.5/v1.0). Both sizes exercise the same engine code paths — that is what makes MCE one engine rather than two demos.

### UC-A — "Ember Step": 2D platformer

- **MVP size (Artifact A):** a single-screen 2D platformer — terrain sprites, a player (run/jump, AABB physics), 2–3 enemy behaviors, collectibles, one SFX set + one music loop, start and win/lose states, HUD-free (win condition rendered as on-screen text). Ships as a zip: `game.exe` + `assets.mce` + SDL3 DLLs, double-clickable by a non-programmer.
- **Full size (v0.5):** a precision platformer in the Celeste/Hollow Knight-controls lineage, ~5 hand-authored levels, 60 fps, keyboard + gamepad, coyote time / jump buffering / wall-slide / dash, crumbling and moving platforms, patrolling and chasing enemies, best-time persistence, parallax backgrounds, dust/impact particles, main menu / level select / pause / results.
- **Acceptance (gate):** a non-programmer double-clicks the exe and completes the game (MVP: single screen; v0.5: all 5 levels) with keyboard (v0.5: or gamepad); 60 fps on the dev machine with the frame budget documented; all content authored in the MCE editor, not in code.

### UC-B — 3D game: lit diorama → "Ashen Court"

- **MVP size (Artifact B):** one static glTF scene (≥ 10k triangles, textured) **kit-assembled from CC0 3D kits (e.g., Kenney) plus at most a few Blender-adjusted primitives — no hand-modelled art at the MVP gate**, orbit/zoom camera on input actions, one directional light with one shadow map, fog, a node-transform-animated spinning prop. Same zip shape. *Purpose: force the 3D pipeline (glTF → cook → MDI, lighting, shadow, camera) before the project can drift into "2D engine forever."*
- **Full size (v0.5→v1.0):** a compact third-person combat arena — one glTF level (~300k triangles, **kit-assembled from CC0 kits + Blender kitbashing with a stated per-mesh triangle budget checked at cook**), a player character, three FSM-driven enemy types, melee combat, health pickups, sun + 4–6 point lights, PBR materials, cascaded shadows, bloom + tonemap, third-person orbit camera with collision, spatialized audio, HUD/menu/pause.
- **Fallback (pre-agreed):** if skinned animation is cut (feasibility cut candidate #1 — see FR-R3D-060 and Open Question 3), Ashen Court ships with a node-transform-animated or static-mesh player proxy; the game must be shippable without skinning.
- **Acceptance (gate):** launchable by a non-programmer; 60 fps at 1080p on the dev machine; camera never clips through level geometry in the shipped build; every render feature in it has a RenderDoc capture and a golden image in the repo.

### UC-D — the v1.0 proof game

One additional small complete game (2D or 3D) built **exclusively through the editor workflows and the `docs/engine/*.md` set** — with zero engine-source changes to gameplay systems beyond an at-most-2-system budget — shipped through the one-command packaging pipeline. This is the proof that MCE is an engine rather than two demos hand-wired to one codebase. (UC-D ships as a vertical slice: one small level reusing Ember Step's kit components plus one new mechanic — finding F7; it is gated on the M8 project template being the path UC-D itself uses.)

### UC-C — the editor loop (the surface that binds them)

The anchor is the **30-second loop**: launch editor → open scene → drag a prefab into the viewport → gizmo-nudge it → press Play → watch it behave → press Stop (scene reverts) → tweak a component in the inspector → Save → Cook → launch the game. If any step takes minutes instead of seconds, MCE fails UC-C regardless of runtime merits.

- **Acceptance (gate, v1.0):** authoring a new UC-A level end-to-end — empty scene to cooked, launchable game content — takes under 30 minutes using only editor features documented in the engine's own docs, with no code changes and 0 crashes in a 2-hour authoring session.

---

## 3. Functional requirements by subsystem

Conventions: **Anchor** = A (UC-A), B (UC-B), D (UC-D), C (UC-C), ★ = shared capability. "MVP/v0.5/v1.0" is the tier at which the requirement must be green.

### 3.1 FR-RUN — Runtime core, frame loop, application

| ID | Requirement | MoSCoW | Tier | Acceptance criterion |
|---|---|---|---|---|
| FR-RUN-010 | The runtime is a standalone executable that boots from cooked `.mce` content only: opens a window (SDL3), initializes audio/GL, loads its boot scene, runs. No dev dependencies, editor, or tools on the target machine. | Must | MVP | A clean Windows 10 x64 VM (no dev tools, only MSVC redistributable) runs the shipped artifact from a double-click. |
| FR-RUN-020 | Fixed-timestep simulation at 60 Hz with interpolated rendering (*Fix Your Timestep*, Gaffer On Games), spiral-of-death clamp at max 5 catch-up steps/frame, `alpha` exposed for render interpolation. | Must | MVP | Frame log shows exactly-16.67 ms tick grid under a synthetic 30 fps and 240 fps render load; interpolation alpha used by the render extract step. |
| FR-RUN-025 | Headless simulation mode: the fixed-step sim runs without window/renderer. | Must | MVP | Determinism CI harness runs 10k ticks headless in `mce_devhost` with zero GL calls (verified by GL-leak grep). |
| FR-RUN-030 | Application shell scenes (boot / main menu / gameplay / pause) as runtime scene states with push/pop transitions, used by both anchor games. | Must | MVP (state stack) / v0.5 (full menu flow) | **MVP acceptance:** the state stack exists — boot→gameplay→quit push/pop with no leaked entities, and the pause state suspends the sim (scene-state menu per A9; nothing in M0–M4 builds a real menu screen). **v0.5 acceptance:** Artifact A demonstrates menu → gameplay → pause → menu round-trip (lands with full Ember Step / FR-UI-010, ROADMAP M6). |
| FR-RUN-040 | Deterministic fixed-step sim, **narrowly defined**: same binary + same `.mce` inputs + same input stream ⇒ identical component-state hashes over N=10k ticks, on one machine, in Debug and Release. Cross-machine/cross-driver bit-identical rendering is out of scope forever. | Must | MVP | CI determinism test green on every PR from M1: identical hashes across 3 consecutive runs and Debug-vs-Release. |
| FR-RUN-045 | Frame pacing: vsync on by default; uncapped mode for dev; frame time + budget table visible in the dev overlay. | Must | MVP | Dev overlay shows frame ms, sim/render split, and the milestone budget table; uncapped mode toggled by convar. |
| FR-RUN-050 | Time scale: global sim speed multiplier (pause, slow-mo) usable as debug tool and gameplay feature. | Should | v0.5 | Slow-mo at 0.25× preserves determinism (hash test passes with scaled dt). |
| FR-RUN-060 | Lifecycle: focus loss/minimize pauses sim; clean quit flushes saves before exit; crash handler writes minidump before termination. | Should | v0.5 | Alt-tab during play resumes without input stuck states; task-manager kill during save leaves last-good save intact. |
| FR-RUN-065 | Save/persistence: reflection-serialized save data to `%LOCALAPPDATA%/MCE/<game>/saves`; versioned, forward-compatible reads rejected with a diagnosable error. | Should | v0.5 | UC-A best time persists across sessions; a save from a newer format version fails with code + path, not a crash. |
| FR-RUN-070 | Replay: record per-tick input snapshots; replay deterministically in dev builds. | Should | v0.5 | Replay a 60 s sample session; final component hashes match the live run (reuses FR-RUN-040 harness). |
| FR-RUN-080 | Multiple runtime windows (e.g., debug second window). | Could | — | Optional; demo-able in devhost. |
| FR-RUN-090 | Mod support, live-reloaded native code in shipping, plugins. | Won't | — | §5 N10. |

### 3.2 FR-ENT — Entities, scenes, behavior model

| ID | Requirement | MoSCoW | Tier | Acceptance criterion |
|---|---|---|---|---|
| FR-ENT-010 | Archetype ECS: entities as generational-index handles (the only cross-frame reference gameplay code may hold), reflected plain-struct components, queries, explicit named phases with explicit intra-phase order. | Must | MVP | 100k entities × transform+velocity iterate+write < 1.5 ms single-thread (Tracy-verified); stale handle lookup fails, never aliases (unit test). |
| FR-ENT-020 | Structural changes deferred through per-phase command buffers (frame-linear memory); user queries never invalidated mid-iteration. | Must | MVP | Command-buffer order tests pass; spawn-storm test (10k spawns in one tick) applies in bulk at sync point. |
| FR-ENT-030 | Typed, double-buffered event queues with explicit producer/consumer registration; no god-queue. | Must | MVP | **MVP acceptance:** event registry records per-type counts + producer/consumer sets, verified by unit test and a console command dump; reader never sees a partially-written event list (unit test). **v0.5 acceptance:** the live editor event-flow *view* renders those same per-type counts. |
| FR-ENT-040 | Change detection: per-row changed ticks with `is_added()/is_changed()` filters; drives dirty transform propagation and render interpolation. | Must | v0.5 | Moving 1 of 5k entities re-interpolates exactly that entity (assert count in debug render extract). |
| FR-ENT-050 | Transform hierarchy: `ChildOf{parent}` single-parent component, engine-maintained children, depth-ordered dirty propagation of world transforms; editor shows the tree. | Must | v0.5 | Rotating a parent moves children same-frame; dirty propagation touches only changed subtrees (assert). |
| FR-ENT-060 | Scenes: editor-authored scene document is canonical; cooking produces `.mce` scene chunks (component columns + id remap); runtime loads and never mutates the canonical scene. | Must | MVP | Scene save→cook→load→hash equals the editor-side reference hash; runtime binary contains no scene-mutation code path (review + CI). |
| FR-ENT-070 | Prefabs: authored entity sub-trees cooked to template chunks; instantiate = bulk row copy + handle remap + instance overrides; nested prefabs flattened at cook. No runtime inheritance. | Must | v0.5 | Editing a prefab re-cooks and all instances reflect the change on reload; override round-trip test passes. |
| FR-ENT-080 | Gameplay starter kit: camera, sprite, animated sprite, mesh renderer, light, `RigidBody2D`/colliders, audio source, text label; singleton resources (Time, Input, ActiveCamera). | Must | MVP | Both anchor games are built from kit components with zero engine-source changes (CI compile + grep of sample imports). |
| FR-ENT-090 | FSM support: state-as-data components + per-state systems + small `fsm.h` helper (timed states, entry/exit actions); states inspectable and save-game-able. | Must | MVP | Enemy FSM in Artifact A is inspectable in the editor live; state time resets on transition (unit test). |
| FR-ENT-100 | Scripted beats: data-driven action-list sequences (wait/move/play_anim/emit/call) advanced by a runner system. | Should | v0.5 | A scripted intro sequence in Ashen Court runs from data; serializes and resumes (round-trip). |
| FR-ENT-110 | Timelines: cooked track/clip assets (transform keys, clips, audio, camera cuts, events) with an editor timeline view. | Should | v1.0 | A camera-cut timeline plays back deterministically; editor timeline view scrubs it. |
| FR-ENT-120 | Sub-scene streaming (multiple scene chunks per world, async load). | Could | — | Trigger: a game exceeds one scene chunk comfortably. |
| FR-ENT-130 | Observers/trigger cascades, runtime prefab inheritance (flecs `IsA`), behavior trees/GOAP, coroutines for gameplay scripting. | Won't | — | ecs-gameplay research §6/§9/§11 records re-entry triggers. |

### 3.3 FR-R2D — 2D rendering

| ID | Requirement | MoSCoW | Tier | Acceptance criterion |
|---|---|---|---|---|
| FR-R2D-010 | Batched sprite renderer: one MultiDrawElementsIndirect batch per material/atlas; 10,000 sprites @ 60 fps on the dev machine. | Must | MVP | 10k quads at 60 Hz with GPU submit p99 < 4 ms (Tracy). RenderDoc check (refined per rendering-2d D4): **one MDI per (blend/pipeline state × sprite-set)** — the cooked 2D-array atlas makes the page a vertex-carried layer index, so the expected capture shows *fewer* draws than the per-page bound, never more. |
| FR-R2D-020 | Sprite component: texture region, pivot, flip, color tint, sort layer + order, visible flag. | Must | MVP | Inspector round-trip of every field; render matches (golden image). |
| FR-R2D-030 | Sprite-sheet animation: grid or atlas-coordinate frames, fps, loop/pingpong/once, frame-indexed playback events. | Must | MVP | Footstep event fires on the authored frame index ±1 tick (unit test with synthetic clock). |
| FR-R2D-040 | 2D camera: position/zoom/rotation, primary-camera semantics, pixel-snap option, screen-shake helper. | Must | MVP | Pixel-snap eliminates texel shimmer in a scroll test scene (golden image pair). |
| FR-R2D-050 | Parallax background layers (factor-per-layer scrolling). | Must | v0.5 | Ember Step background renders 3 layers at authored factors; golden image. |
| FR-R2D-060 | Text rendering: cook-baked font atlases used for HUD, debug overlay, and runtime UI. Format-tagged font chunk from day one: stb_truetype coverage atlas at MVP; MSDF via msdf-atlas-gen at v0.5 (cook-time-only ADR class — rendering-2d D9); the swap is a cook + shader change with no caller-visible API change. | Must | MVP | Sample scene renders ASCII text legible at 1080p; atlas built at cook, not runtime. |
| FR-R2D-070 | CPU-simulated particle systems: point/area emitters, velocity/spread, gravity/drag, color/size-over-life, bursts; drawn through the sprite batch. Design home: `ParticleEmitter` starter-kit component + reference `particles` system (ecs-gameplay §2.9), cooked `CHNK_PART` preset chunk (assets §2); the reference system ships in the game module per rhi-renderer §3.4's ownership stance. | Must | v0.5 | 2,000-particle burst at 60 fps within budget (Tracy); determinism test covers particle sim. |
| FR-R2D-080 | Editor game view renders into a render-target widget (2D and 3D) with correct DPI scaling. | Must | MVP | Editor viewport shows the game at window DPI without blur (manual gate). |
| FR-R2D-090 | Lit 2D sprites: normal-map lighting from a small 2D light set (ambient + point). | Should | v0.5 | Golden image with a moving 2D light; ≤ 0.5 ms added GPU at 10k sprites. |
| FR-R2D-100 | 2D post effects: vignette, color-grade LUT, cheap chromatic aberration. | Should | v0.5 | Toggleable at runtime; < 1 ms GPU combined at 1080p. |
| FR-R2D-110 | Render-to-texture (camera to texture usable as sprite). | Could | — | Demo-able in devhost. |
| FR-R2D-115 | Tilemap rendering: cook-baked tile chunk (CHNK_TILE), rendered as one MDI batch; authoring via FR-EDT-120's rectangle-fill over the tilemap document + re-bake loop (editor-tools §2.7). | Must | v0.5 (when Ember Step's levels need it) | An Ember Step level renders from the baked tile chunk in a single MDI batch (RenderDoc evidence); editing a tile re-bakes the chunk without editor restart. |
| FR-R2D-120/130 | Skeletal/mesh-deformed 2D animation; shadow-casting 2D lights. | Won't | — | Re-entry only via committed game concept (slip note). |

### 3.4 FR-R3D — 3D rendering

| ID | Requirement | MoSCoW | Tier | Acceptance criterion |
|---|---|---|---|---|
| FR-R3D-010 | glTF 2.0 import at cook: meshes → meshoptimizer vertex-cache/fetch optimized, quantized vertex streams in `.mce`; materials + textures carried through. Importer scope: static meshes, PBR metallic-roughness params, node transforms, node-transform animation; unknown extensions skipped with a cook warning. | Must | MVP (static meshes + **single-node TRS clip playback** — all the spinning-prop gate needs) / v0.5 (full multi-node animation tracks) | Cook of the 10k-tri reference mesh < 5 s; load < 5 ms (mmap); unsupported extension produces warning, not failure. |
| FR-R3D-020 | PBR metallic-roughness (Cook-Torrance GGX) with basecolor/normal/ORM set; albedo-only fallback material. | Must | v0.5 | Golden image of the material test sphere row stable across 3 runs (perceptual threshold, R9 policy). |
| FR-R3D-030 | Forward+ clustered lighting: directional + point + spot; light counts beyond 8 not a special case. | Must | v0.5 | ≥ 256 point lights in one scene at 60 Hz; cluster build < 0.5 ms CPU (Tracy). |
| FR-R3D-040 | Cascaded shadow maps for the sun (2–4 cascades, stable fit). | Must | v0.5 | No visible seam pop at demo camera distances; shadow pass < 2 ms GPU. *Cut-order: reduce to single cascade before extending the M5a timebox (binding — 3D rendering is M5a work since the M5a/M5b split).* |
| FR-R3D-050 | Frustum culling per entity before MDI submission. | Must | v0.5 | Off-screen 50% of scene ⇒ draw-call count drops accordingly (Tracy/RenderDoc evidence). |
| FR-R3D-060 | Skinned meshes: glTF skin import, joint-palette skinning, clip sampling (lerp/slerp), cross-fade blending, playback events. | Should | v0.5 | *Demoted from Must — feasibility cut candidate #1 (Appendix A).* Acceptance if built: cross-fade between two clips with no pop; footstep events drive audio. Ashen Court ships without it if cut. |
| FR-R3D-070 | 3D cameras: perspective + ortho; primary-camera semantics shared with 2D. | Must | MVP (diorama) | Orbit/zoom camera drives Artifact B via input actions only. |
| FR-R3D-080 | Sky: gradient skydome and equirect skybox texture path. | Must | v0.5 | Golden image; sky renders behind scene with correct depth. |
| FR-R3D-090 | Post chain (3D): tonemap, bloom, FXAA. | Should | v0.5 | Toggleable; < 1.5 ms GPU combined at 1080p. *Pre-agreed cut before cascades (R1 order).* |
| FR-R3D-100 | IBL: environment probe with cook-time convoluted specular cubemap. | Should | v1.0 | Materials respond plausibly to an environment swap (side-by-side golden images). First on the cut list if M5a slips (binding timebox rule). |
| FR-R3D-110 | 3D particles (billboards through the same MDI batch). | Should | v0.5 | Impact burst renders through the shared batch path (RenderDoc: no second pipeline). |
| FR-R3D-120/130/140 | SSAO, decals, mesh LOD. | Could | — | Demo-able only. |
| FR-R3D-150 | Ray tracing, virtualized geometry, volumetrics, GI beyond probes, terrain, SSR. | Won't | — | GL 4.5 ceiling is honestly inside "small complete games." |

### 3.5 FR-AST — Assets and the cook pipeline

| ID | Requirement | MoSCoW | Tier | Acceptance criterion |
|---|---|---|---|---|
| FR-AST-010 | Interchange formats are exactly glTF 2.0 (`.gltf`/`.glb`), PNG/JPG, WAV. The shipped runtime never parses JSON/XML/PNG/WAV. | Must | MVP | CI symbol/dependency scan: no JSON/PNG/WAV decoder linked into `mce_runtime`. |
| FR-AST-020 | Cook step producing versioned chunked `.mce` containers (magic + version + CRC + 16-byte-aligned payload), memory-mapped at load; generational handles throughout. | Must | MVP | `.mce` reader passes fuzz + negative tests (bad magic, truncation, CRC mismatch, version-ahead) with diagnosable `Result` errors, never a crash. |
| FR-AST-030 | Deterministic cook: same source tree twice ⇒ byte-identical outputs. | Must | MVP | CI test at every milestone: cook, hash, cook, hash, equal. |
| FR-AST-040 | Stable asset GUIDs surviving moves/renames; broken references fail cook with a diagnosable error listing them. | Must | MVP | Rename an asset in a sample project; cook succeeds with references intact; delete a referenced asset → cook error names file + line. |
| FR-AST-050 | Texture compression at cook: BC7 color, BC5 normals; per-class selection (BC1/BC5/BC7). | Must | v0.5 | Ashen Court textures fit VRAM budget (memory table); visual diff vs uncompressed within threshold. |
| FR-AST-060 | Shader pipeline: GLSL validated offline by glslangValidator at cook/CI; **MVP = per-stage validation + binding-bank conformance check + full program rebuild-and-swap on change; variant enumeration + precompiled variant table + stored SPIR-V reflection blob are v0.5** (slip note: finding F9); editor shader hot-reload in milliseconds. | Must | MVP | Saving a fragment shader changes on-screen output ≤ 1 s without restart (DX-H1); a shader with a syntax error surfaces line-numbered in the editor console while the old program keeps rendering. |
| FR-AST-070 | Cook CLI (`mce_cook`): scriptable, exit codes for CI, cooks a game content tree to a shippable pack. | Must | MVP | CI green/red path exercised; full sample-project cook < 60 s, incremental < 5 s. |
| FR-AST-080 | Incremental cook with content-hash cache. | Should | v0.5 | Touching one `.gltf` re-cooks only assets whose source hash changed (cook log evidence). |
| FR-AST-090 | Asset hot-reload in the editor: texture/mesh/scene/prefab changes on disk re-cook and refresh the viewport without restart. | Should | v0.5 | Each of texture/scene/prefab reloads ≤ 1 s; failed texture re-import shows magenta placeholder, handles stable. |
| FR-AST-100 | Audio cook: WAV → normalized 48 kHz float32 `.mce` chunks; IMA-ADPCM fallback for size. | Must | MVP | Latency gate (finding F13): **command-enqueue-to-callback-mix ≤ 25 ms, measured on the fake-device harness** (the engine-controllable segment); device loopback stays *informational* with a ≤ 40 ms target at the 480-frame period request, tuned via `audio.device_latency_ms` — driver-granted periods are outside the engine's control; music loop gapless (waveform comparison). |
| FR-AST-110 | Asset dependency graph visualization; find-references from the browser. | Could | — | Demo-able. |
| FR-AST-120 | Runtime JSON/XML/PNG parsing, asset encryption, runtime import, network streaming install. | Won't | — | Binding asset policy. |

### 3.6 FR-PHY — Physics and character control

| ID | Requirement | MoSCoW | Tier | Acceptance criterion |
|---|---|---|---|---|
| FR-PHY-010 | 2D rigid bodies: static/kinematic/dynamic; AABB, circle, convex polygon colliders; contact manifolds. Hand-written (Catto). | Must (MVP: rung-6 dynamic-vs-static bodies with discrete AABB manifolds) / Should (M3.5 stretch: full manifold suite) | MVP (rung 6) / M3.5 stretch (suite) | **MVP acceptance:** dynamic-vs-static contact + response correct on basic unit cases (stacking explicitly out, R5). **M3.5 exit criterion:** the 20 hand-computed contact-manifold unit cases pass — if M3.5 slips whole into v0.5 (its designed function), the suite moves with it, not the MVP gate. |
| FR-PHY-020 | Sequential-impulse 2D solver with friction + restitution. | Must (MVP: rung-6 impulse response, no warm starting) / Should (M3.5 stretch: full sequential-impulse solver) | MVP (rung 6) / M3.5 stretch | **MVP acceptance:** restitution/friction behaviors match hand-computed cases on the rung-6 path; no tunneling at Artifact A player speeds with fixed 60 Hz + swept AABB. **M3.5:** warm starting + the full solver suite. |
| FR-PHY-030 | Swept 2D character controller: move-and-collide with ground/wall/ceiling flags, slope limits, moving-platform carry, one-way platforms. | Must | MVP | Artifact A player rides a moving platform without sliding; one-way platform blocks from above only (unit tests). |
| FR-PHY-035 | Sensors/triggers: overlap events with no physical response. | Must | MVP | Pickup/kill-zone triggers fire events exactly once per entry (unit test). |
| FR-PHY-040 | Queries: raycasts, shape casts, overlap tests — deterministic from gameplay systems. | Must | MVP | Determinism test includes query results (same inputs ⇒ same hit sets). |
| FR-PHY-050 | Broadphase (AABB tree or sort-and-sweep) handling thousands of dynamic bodies within frame budget. | Must | v0.5 | 2,000 dynamic bodies broadphase+integrate < 3 ms (Tracy). |
| FR-PHY-060 | Joints: distance + revolute (spring optional). | Should | v0.5 | Joints unit-tested against hand-computed constraint solutions; 20-body stack settles < 1 s with no jitter (video + Tracy). *Stacking is explicitly out of MVP scope (R5).* |
| FR-PHY-070 | SoA SIMD-friendly broadphase hotspots behind scalar-first math. | Should | v0.5 | Only if Tracy shows the cost; scalar reference stays and oracle-tests the SIMD path. |
| FR-PHY-080 | 3D character controller for Ashen Court: capsule with slope/step handling, camera- and character-vs-mesh sweeps. Vehicle: hand-written SAT/GJK **or** Jolt 5 via the pre-approved ADR — product demands the capability, architecture picks the mechanism and records the ADR either way. | Must | v0.5 | Player traverses the Ashen Court level: slopes, steps, no wall-clipping; camera never enters geometry in the shipped build. |
| FR-PHY-090 | 3D dynamic rigid bodies (knockable props) via Jolt-via-ADR. | Could | — | UC-B must be shippable without it. |
| FR-PHY-100 | Ragdolls, vehicles, cloth, soft bodies, destruction, GPU physics, networked physics. | Won't | — | §5. |

### 3.7 FR-AUD — Audio

| ID | Requirement | MoSCoW | Tier | Acceptance criterion |
|---|---|---|---|---|
| FR-AUD-010 | Mixer hand-written on the SDL3 device callback: 48 kHz stereo float32 internal; real-time audio thread with lock-free SPSC command FIFO; no allocations/locks/ECS access in the callback (Doumler's RT rules). | Must | MVP | Callback does 0 allocations/0 mutex locks — asserted in debug and verified in a Tracy capture. |
| FR-AUD-020 | Voice pool with priority stealing + unlimited virtual voices; one-shot and looped playback with sample-accurate start. MVP ships ≥ 8 hardware voices; 48 by v0.5. | Must | MVP (8) / v0.5 (48) | Playing the 49th concurrent sound by v0.5 steals the lowest-priority voice without a click; sub-frame scheduling honors timestamps to the sample. |
| FR-AUD-030 | Bus graph Master/Music/SFX/UI with per-bus gain; options-screen volume wired to buses. | Should | v0.5 | Bus render matches an offline reference mix within −60 dB (sample-compare test). |
| FR-AUD-040 | Streaming music/dialog from memory-mapped `.mce` via ~100 ms ring buffer; SFX fully in-memory (resident budget < 32 MB). | Must | MVP | Music loop plays gapless from the mapped container; no underruns in a 10-minute soak. |
| FR-AUD-050 | 2D spatialization (pan/power) and 3D spatialization (distance attenuation + pan, listener = camera). | Must | v0.5 | 3D: sound attenuates per authored curve as the camera retreats (measured gain curve within tolerance). |
| FR-AUD-055 | Gameplay→audio via events: `AudioSource` component + event-driven one-shots (footsteps from animation events, hits from combat). | Should | v0.5 | Footstep SFX aligns with the authored animation frame ±1 tick. |
| FR-AUD-060 | DSP chain: RBJ-cookbook biquad EQ per bus, Freeverb-style reverb send, look-ahead master limiter. | Should | v0.5 | Reverb/EQ/limiter each pass an offline impulse/sweep reference test; timebox on reverb quality enforced (R-cut). |
| FR-AUD-070 | Sweeteners: random pitch/variance, crossfades, bus ducking. | Could | — | Demo-able. |
| FR-AUD-080 | HRTF/convolution reverb, procedural music, runtime codecs, FMOD/Wwise-style authoring integration. | Won't | — | audio-input-ui research §. |

### 3.8 FR-INP — Input

| ID | Requirement | MoSCoW | Tier | Acceptance criterion |
|---|---|---|---|---|
| FR-INP-010 | Device layer via SDL3: keyboard, mouse, text input; gameplay code never sees `SDL_Event`. Gamepads (SDL GameController mapping) join at v0.5. | Must | MVP (kb+mouse) / v0.5 (gamepad) | CI grep: no `SDL_Event` outside `mce::platform`; gamepad actions drive Artifact A by v0.5. |
| FR-INP-020 | Action mapping layer: named actions, bindings, action maps, per-device deadzones and response curves; authored as data, editable in the editor. | Must | MVP | Action-mapping unit tests with synthetic event streams; an in-session rebinding applies from the next sim tick (persistence is FR-INP-040, not here). |
| FR-INP-030 | Input sampled per 60 Hz sim tick with edges derived from device-state snapshots — deterministic under varying render rates. | Must | MVP | Synthetic events at odd rates (30/90/240 Hz render loops) produce identical sim-visible sequences (the determinism corollary test). |
| FR-INP-040 | Runtime rebinding + persisted bindings (options UI). | Should | v0.5 | Rebind in options; a rebinding survives an editor/game restart — persisted across sessions via reflection-serialized save. |
| FR-INP-050 | Rumble/haptics via SDL gamepad rumble. | Could | — | One call; demo-able. |
| FR-INP-060 | Touch, multiple mice, VR, raw device access beyond SDL, >4 gamepads as a supported scenario. | Won't | — | §5. |

### 3.9 FR-EDT — The editor (UC-C)

| ID | Requirement | MoSCoW | Tier | Acceptance criterion |
|---|---|---|---|---|
| FR-EDT-010 | Docking shell (ImGui docking branch, multi-viewport off): hierarchy, inspector, asset browser (v0.5), viewport (2D+3D), console, toolbar (play/pause/step, cook, save). Editor exists at M0 as a client of the runtime API. | Must | MVP | **M0 artifact:** dockable shell + console + hierarchy outline against the stub world (placeholder viewport) — the shell portion of this gate (finding F4 re-baseline). **Full acceptance** — spawning/selecting entities and inspecting components — at M3 (inspector round-trip at M2 per the slip note; see ROADMAP M0/M2/M3 and ARCHITECTURE §10.1). |
| FR-EDT-020 | Reflection-driven inspector: every registered component inspectable with zero per-type editor code; field types: numeric drag, bool, enum dropdown, color, string, asset/entity reference pickers, read-only computed. | Must | MVP | Register a 3-field component with no editor code; inspector renders and round-trips edits through the undo stack (test). **MVP asset/entity pickers (finding F6):** the MVP picker is a Guid text field + a type-filtered dropdown populated from the GUID registry scan (the registry already exists for sidecars) — this satisfies the MVP acceptance; the browser-linked drag-drop picker upgrades it at v0.5 (editor-tools §2.6). |
| FR-EDT-030 | Gizmos: translate (MVP), rotate/scale (v0.5) via ImGuizmo (one-page ADR), world/local toggle, grid, snapping; entity picking in 2D and 3D viewports. | Must | MVP (translate) / v0.5 (rotate/scale) | Drag → `LocalTransform` updated → children follow, all same-frame; snap increments honored. |
| FR-EDT-040 | Undo/redo as a reflection-driven command stack covering scene/prefab edits; bounded history. | Should | v0.5 (Must for the v1.0 ship gate) | Perform N random inspector/gizmo ops, undo all; scene document byte-identical to before (test). |
| FR-EDT-050 | Entity ops: create/delete/duplicate, drag-drop, copy/paste, multi-select with grouped gizmo transform. | Must | MVP (CRUD+copy-paste) / v0.5 (multi-select) | CRUD round-trip: edit + save + reload + play yields identical scene hash. |
| FR-EDT-060 | Play-in-editor: enter/exit play builds a runtime world from the scene document; play-mode edits discarded on stop; pause + single-step + slow-mo; live entity/event inspection. | Must | v0.5 | Play entry ≤ 1 s at sample scale; Play→Stop→Play twice yields identical first-tick state (determinism harness). MVP ships the "Run in player" button instead (DX-ED4). |
| FR-EDT-070 | Cook integration: cook button + error list panel surfacing cook/shader/import errors with jump-to-asset. | Must | MVP | A deliberately broken asset surfaces in the panel with path; click navigates to the asset. |
| FR-EDT-080 | Prefab workflow: instantiate, open-prefab-asset editing, override display, apply/revert overrides. | Should | v1.0 | Prefab edit propagates to all instances on reload; override merge round-trips. |
| FR-EDT-090 | Debug overlays: colliders, bounds, lights, camera frustums, live event-flow monitor, ECS archetype/table metrics, Tracy/RenderDoc launch buttons. | Should | v0.5 | Overlay toggles render correct bounds in a golden-image scene; one-button Tracy capture. |
| FR-EDT-100 | Scene document safety: text serialization (editor-side), versioned format, autosave + crash-recovery backup. | Should | v0.5 | Kill -9 the editor mid-session; reopen recovers autosaved state with a prompt. |
| FR-EDT-110 | Asset browser maturity: thumbnails, type filtering, import status, rename-with-GUID-stability. | Should | v0.5 (browser with drag-drop) / v1.0 (thumbnails) | Renaming an asset keeps references resolving (GUID test). |
| FR-EDT-120 | 2D authoring aids: grid/snap placement, rectangle-fill placement tool (grid/snap pulled forward to ROADMAP M3 — once the inspector exists these are days, not weeks; rectangle-fill lands with the tilemap panel at M6). | Should | v0.5 (**Must for M6** — the v0.5 content plan assumes editor-authored levels) | An Ember Step level block is placed in < 1 min of editor actions (manual gate). M6 runs a timed authoring spike (block out one full level, record hours) and re-estimates the content ledger from that measurement before committing to five levels; if the spike exceeds budget, cut to three levels rather than compressing tool time. |
| FR-EDT-130 | Timeline/curve editor for FR-ENT-110. | Should | v1.0 | Timeline view edits and scrubs a cooked timeline. |
| FR-EDT-140 | Multi-scene tabs, scene-diff view, in-editor profiling dashboards. | Could | — | Demo-able. |
| FR-EDT-150 | Visual scripting, node-graph material editors, multi-user editing, plugin API, theming/localization. | Won't | — | §5. |
| FR-EDT-160 | Editor MCP server: a headless, stdio-only Model Context Protocol server exposing reflection discovery, scene read/edit (through the `SceneDocument` mutation funnel), save, cook and headless deterministic runs to an external AI client (editor-mcp.md). Editor-side only; no runtime target links it. | Should | ROADMAP M4.5 (off-tier tooling) | An edit made through the MCP tools saves a `.mcscene` byte-identical to the same edit made via `SceneCommand`s, and `run_headless` returns the same state hash for both (test). |

### 3.10 FR-UI — Runtime game UI (deferred, designed now)

| ID | Requirement | MoSCoW | Tier | Acceptance criterion |
|---|---|---|---|---|
| FR-UI-010 | Shipped games have main menu, pause, options (volume/keybinds/resolution), and HUD built on the **runtime UI system** — never ImGui. MVP/v0.5 games may ship scene-state menus + text-sprite HUD; the full `mce::ui` system powers the v1.0 ship gate. | Must | v1.0 (primitives Should at v0.5) | The v1.0 game's menus/HUD render through `mce::ui` with zero ImGui symbols in the runtime binary (CI symbol scan). |
| FR-UI-020 | `mce::ui` retained scene graph: anchored-rect layout (uGUI model), hierarchy, input routing; rendered through the shared 2D MDI sprite batch path. Design doc at M4; build at v1.0. | Should | v1.0 | Prototype panel+button+text renders through the sprite batcher; focus/navigation works on gamepad. |
| FR-UI-030 | Data binding: UI observes gameplay state read-only via reflection + events; gameplay never reaches into UI. | Should | v1.0 | Health bar binds to a component property path; no gameplay code imports `mce::ui`. |
| FR-UI-040 | Extra widgets: sliders, checkboxes, scroll view, typewriter text, safe-area scaling. | Could | — | Demo-able. |
| FR-UI-045 | Shipped debug console (convars/confuncs on `mce::ui`). | Should | v1.0 | Dev console usable in a shipped build without ImGui present. |
| FR-UI-048 | Immediate-mode UI in shipped games; ImGui anywhere in runtime builds. | Won't | — | CI-enforced target wall (CMake + include-grep + symbol scan). |
| FR-UI-060 | Rich-markup UI, IME-heavy text entry, localization pipeline. | Won't | — | RmlUi is the documented pre-approved ADR fallback if a game genuinely needs markup. |

### 3.11 FR-DBG — Diagnostics, profiling, debuggability

| ID | Requirement | MoSCoW | Tier | Acceptance criterion |
|---|---|---|---|---|
| FR-DBG-010 | Tracy zones from M0: frame mark, sim tick, ECS iteration, render submit, cook steps; GPU zones; memory hooks. Frame **and memory** budget tables filled per milestone, next to a Tracy capture. | Must | MVP | Every milestone doc carries a Tracy screenshot and a filled budget table reconciling within 20% of the memory plot. |
| FR-DBG-020 | Every new render feature verified in a RenderDoc capture before merge; golden-image tests at cook for renderer correctness. Golden images are **dev-machine-only**, perceptual threshold (block-mean luminance diff < 0.5%), auto-rebaselined by convention; never a CI gate — the merge gate is the human-eyeballed capture. | Must | MVP | Milestone checklist: each render feature lists its capture; golden suite runs on the dev machine with 0 flakes over 3 runs. |
| FR-DBG-030 | Crash handling: SEH filter + minidump to `%LOCALAPPDATA%/MCE/crashes` in editor, cook tool, and runtime; PDBs archived per CI build keyed by build ID; WER suppressed; no silent corrupt-save paths. | Must | MVP | An induced crash produces a dump that opens in VS with matching PDBs and locals visible. |
| FR-DBG-040 | Debug console (editor): convars/confuncs, log panel with levels; no hot-path logging (per-frame summaries instead). | Must | MVP | Convar change takes effect same-frame; log panel shows a filtered error with jump-to-source. |
| FR-DBG-050 | Sim inspection: pause, single-step, scrub replays; inspect any entity's component state live. | Should | v0.5 | Replay of a 60 s session scrubs to any tick with matching state (reuses FR-RUN-070). |
| FR-DBG-060 | Dev overlay in shipped-game dev builds: fps, frame breakdown, entity counts, audio voices. | Should | v0.5 | F1 overlay matches Tracy's numbers for the same frame. |
| FR-DBG-070 | Capture-to-GIF/video from the dev overlay. | Could | — | Demo-able. |
| FR-DBG-080 | Remote profiling, crash aggregation/symbol servers, telemetry in shipped games. | Won't | — | §5 N7. |

### 3.12 FR-SHP — Shipping & platform

| ID | Requirement | MoSCoW | Tier | Acceptance criterion |
|---|---|---|---|---|
| FR-SHP-010 | Shipped game = game `.exe` + cooked `.mce` pack (+ data dir for saves/logs/crashes); launches on a clean Windows 10 x64 machine with at most the MSVC redistributable as an external requirement. Boot-failure path per core §2.1: any failure before the error screen exists (missing/corrupt `assets.mce`, mmap failure, engine_id mismatch) → `SDL_ShowSimpleMessageBox` naming the error, expected path, and log location, then a documented nonzero exit code. | Must | MVP | Fresh-VM test: artifact A/B install-free launch, 10-minute clean soak (no crashes, no ASan findings). CI: rename `assets.mce`, run `game.exe`, assert the error message box path + nonzero exit code (exit-code check, no UI automation). |
| FR-SHP-020 | Shipped builds: exceptions/RTTI off, Tracy compiled out except FrameMark, `MCE_ASSERT` debug-only / `MCE_VERIFY` live, `/LTCG` release. | Must | MVP | Release binary contains no Tracy zone symbols (dumpbin scan) and no exception-handling CRT paths (link flags verified in CI). |
| FR-SHP-030 | One-command ship: cook + package + zip ready for direct upload; icon and version metadata embedded. | Should | v1.0 | One command produces the zip; fresh VM runs it (reuses FR-SHP-010 test). |
| FR-SHP-040 | Build: CMake 3.28+ presets only (Ninja dev, VS debug), vcpkg manifest with pinned baseline, unity + PCH, GitHub Actions (windows-latest) with clang-tidy; ASan per PR (weekly if it costs > 30 min/PR — R8 cut-line); clang-cl UBSan weekly; no milestone merge without a green pipeline. | Must | MVP | CI green is a required check on the repo; a deliberately introduced leak fails the ASan job. |
| FR-SHP-050 | Steam/itch API integration, achievements, cloud saves. | Won't | — | A shipped game may wrap manually later. |
| FR-SHP-060 | macOS/Linux/console/web/mobile targets; 32-bit; Windows 7/8. | Won't | — | SDL3 keeps portability theoretical at zero maintenance cost. |

### 3.13 FR-DX — Developer experience & learning (the second user role)

| ID | Requirement | MoSCoW | Tier | Acceptance criterion |
|---|---|---|---|---|
| FR-DX-010 | Gameplay API surface budget: **12 engine-core types** from `mce::` (World, Entity, Frame, Commands, EventQueue<T>, Sprite, LocalTransform, InputState, AudioHandle, Material2D, Camera2D, + the component/system macros) **+ the closed starter-kit component list (ecs-gameplay §2.9) + one facade type per embedded subsystem (physics `World2D`/`World3D`, audio mixer)**; everything else lives behind `mce::detail::`. | Must | MVP | M1 census doc enumerates the real closed set with per-namespace counts (the raw "12" is exceeded by math types and kit components by design); the M3 grep acceptance checks samples against that census, not against the raw number. |
| FR-DX-020 | Components are reflected plain structs: adding a component requires zero editor code and zero serializer changes. | Must | MVP | CI compiles a sample with a new component; inspector shows it without code changes (test). |
| FR-DX-030 | Systems are plain functions in named phases; the declaration is the ordering documentation; declared order == execution order (editor pipeline view + test). | Must | MVP | Pipeline view renders the phase/order graph at runtime; order test passes for the sample project. |
| FR-DX-040 | Hot-reload matrix: shaders ≤ 1 s save-to-pixels; textures/materials ≤ 1 s with stable handles (magenta on failure); scenes ≤ 1 s outside play mode; gameplay C++ recompile ≤ 10 s to next Play. Game-DLL reload is design-for-only (Handmade Hero split), never a v1 promise. | Must (shaders) / Should (rest) | MVP (shader) / v0.5 (rest) | Stopwatch gates recorded in milestone docs; shader reload is load-bearing (the reason GL 4.5 won). |
| FR-DX-050 | Failure is visible: every user-facing error is either a `Result<T,Error>` (code + path + stage, console-rendered, clickable) or an assert with a minidump path. Missing assets degrade visibly (magenta checker / silence + one error / error screen) — never a crash, never a white void. | Must | MVP | Negative-asset unit test per kind; milestone audit maps every failure path to one of the two categories. |
| FR-DX-060 | Docs are a milestone deliverable: every subsystem ships a one-page `docs/engine/<subsystem>.md` (what/key types/invariants/10-line sketch) written from memory; public API carries doc comments; every committed decision has an ADR or research doc; docs ride the same commit as the API change. | Must | MVP onward | Milestone checklist: doc exists and its sketch compiles against the headers; ADR index current. |
| FR-DX-070 | Graded sample ladder kept green in CI forever: `01_triangle` (M0) → `02_boids` (M1) → `03_platformer` (M1–v0.5) → `04_cubes` (v0.5) → `05_showcase` (v1.0). Each README ≤ 2 pages: what it demonstrates, the 5–12 types it touches, what to open in the editor. Samples are the golden-image and determinism test subjects. | Must | MVP, growing | Each sample: builds in CI, cooks in CI, runs 300 headless fixed steps without error. |
| FR-DX-080 | Learning gates LRN-1..LRN-7 (RHI/DSA, fixed-timestep+allocator, archetype internals, reflection end-to-end, RHI seam, Catto solver + RT audio, cook pipeline): at each gate the builder writes the subsystem's doc from memory and answers the recorded self-exam before the milestone closes. If a gate cannot close in its timebox, cut feature scope — never the gate. | Must | per milestone | Gate recorded in the milestone doc; a gate that cannot close blocks the milestone (binding). |

---

## 4. Non-functional requirements

All numbers are measured on the dev machine (Windows 10 x64, Intel HD Graphics 630 @ driver 26.20.100.7262 — pinned in §8.2) and tracked in the per-milestone budget tables (binding). p99, not averages.

### 4.1 Frame budget (16.6 ms @ 60 Hz, shipped builds)

| ID | Requirement | Target |
|---|---|---|
| NFR-F-01 | Total frame budget at 1080p60, both anchor games, shipped builds | 16.6 ms with **≥ 2 ms headroom at ship**; per-stage table (sim / cull+sort / RHI translate / GL submit / GPU / headroom) published per milestone with a Tracy capture. |
| NFR-F-02 | Stage allocations (indicative, filled per milestone) | e.g. v0.5 target: sim 3.0 / cull+sort 1.5 / RHI translate 1.5 / submit 2.0 / GPU 6.0 / headroom 2.6 ms. |
| NFR-F-03 | Zero heap allocation in hot paths | Asserted in debug builds; verified by ASan + Tracy memory track. Per-frame allocations come only from the frame-linear allocator (16 MiB budget per frame slot, reset after the frame fence). |
| NFR-F-04 | Sim determinism | FR-RUN-040 hash test green on every PR from M1; **`/fp:precise` in every configuration — `/fp:fast` is banned engine-wide** (finding F29: fast-math contraction/reassociation in any non-physics float code feeding a hash — interpolation lerp, hierarchy propagation, particle integrators — would break Debug-vs-Release hashing; at MCE's budgets the MSVC float gain is noise). The separate "determinism preset" is retired — it is now a named alias of the standard release preset. |

### 4.2 Throughput budgets (dev machine)

| ID | Requirement | Target |
|---|---|---|
| NFR-T-01 | ECS iteration | 100k entities × transform+velocity iterate+write < 1.5 ms single-thread. |
| NFR-T-02 | 2D sprites | 10,000 batched sprites @ 60 fps; GPU submit p99 < 4 ms; one MDI submit per (blend/pipeline state × sprite-set) — the atlas page is a vertex-carried layer index, not a batch boundary (CF15/CF16, FR-R2D-010). |
| NFR-T-03 | 3D scene | ~1–2 M triangles @ 1080p60; 1M-triangle scene render p99 < 12 ms; Forward+ cluster build < 0.5 ms CPU; shadow pass < 2 ms GPU. |
| NFR-T-04 | Entities | ~500–5,000 typical, 10,000 design ceiling per scene. |
| NFR-T-05 | Audio | 48 hardware voices + unlimited virtual; **one-shot latency: command-enqueue-to-callback-mix ≤ 25 ms on the fake-device harness (the engine-controllable segment); device loopback ≤ 40 ms informational target with the 480-frame period request + `audio.device_latency_ms` convar as the documented tuning lever** (finding F13); resident SFX < 32 MB; callback zero-alloc/zero-lock. |

### 4.3 Load, memory, and stability

| ID | Requirement | Target |
|---|---|---|
| NFR-M-01 | Startup | Editor startup to usable < 5 s; runtime cold boot to interactive main menu < 5 s; scene-to-scene transition < 1 s. |
| NFR-M-02 | MVE 3D diorama load | Double-click to first frame < 1.0 s (memory-mapped, no runtime decode). |
| NFR-M-03 | Play-mode | Enter/exit play ≤ 1 s at sample-project scale; idempotent (Play→Stop→Play identical first-tick state). |
| NFR-M-04 | Memory budgets | Per-subsystem memory budget table per milestone (ECS heap, GPU allocations, audio buffers, cook scratch), reconciling within 20% of the Tracy memory track. Replay/rewind ring ≤ 64 MiB at sample scale. |
| NFR-M-05a | Stability (dev) | 2-hour editor authoring session, 0 crashes; any crash produces minidump + matching-PDB path; 10-minute soak of the dev build under ASan, 0 errors. |
| NFR-M-05b | Stability (shipped) | 10-minute soak of the shipped zip on a clean Windows 10 x64 VM: 0 crashes, no sanitizer instrumentation (matching ROADMAP M4's clean-VM soak — finding F16). |
| NFR-M-06 | Shipped overhead | No profiler overhead (Tracy off), no editor code (CMake target wall: `mce_runtime` cannot see ImGui), no debug console unless FR-UI-045 is enabled. |

### 4.4 Build & iteration time (velocity, measured)

| ID | Requirement | Target |
|---|---|---|
| NFR-B-01 | Clean clone → running editor | ≤ 3 commands (`cmake --preset msvc-debug && cmake --build --preset ... && run`); clean build ≤ 15 min. |
| NFR-B-02 | Incremental build, gameplay module change | ≤ 10 s to a running editor. |
| NFR-B-03 | Incremental build, core engine header change | ≤ 90 s to a running editor (PCH + unity builds absorb it). |
| NFR-B-04 | Cook | Full sample-project cook < 60 s; incremental (single asset) < 5 s; cook twice ⇒ byte-identical. |
| NFR-B-05 | Content iteration | Shader hot-reload ≤ 1 s; asset hot-reload ≤ 1–2 s; play-mode enter ≤ 1 s. |
| NFR-B-06 | Editor frame rate | ≥ 60 fps with the sample project (~5k entities, all systems live) while idle and inspecting. |
| NFR-B-07 | Velocity instrumentation | Devlog hours log from day one; at any gate, invested hours known to ±2 h; 4-week rolling velocity re-checked against estimates; < 6 h/week for two consecutive windows ⇒ formal one-tier downscope (not date-slipping). |

---

## 5. Explicit non-goals

A solo engine dies of scope. Every item below is **rejected**, with the one condition that would reopen it. Reopening requires a written slip note or ADR — never a silent slide.

| # | Won't | Why not | Re-entry trigger |
|---|---|---|---|
| N1 | Multiplayer/netcode of any kind | Doubles the hardest problem class; determinism is kept only because it is nearly free and pays off in testing/replay | A completed shipped game + explicit design doc |
| N2 | Scripting VM (Lua) in v1; visual scripting; blueprint graphs | Native C++20 systems are the committed gameplay model; a VM before the ECS is proven is a second runtime to debug | After Ashen Court ships, if measured recompile-iterations dominate editor time |
| N3 | 3D physics from scratch beyond the character controller | Hand-rolling 3D rigid bodies is a year of edge cases Ashen Court doesn't need; Jolt 5 is the pre-approved exception | FR-PHY-090 Could — only if a third game demands knockable props |
| N4 | Terrain, foliage, ray tracing, GI beyond probes, SSR, virtualized geometry, volumetrics | All optimize beyond the "small complete game" ceiling; GL 4.5 ceiling stated honestly | A committed game concept whose art direction requires it |
| N5 | Asset-creation DCC features (mesh editing, texture painting, rigging, authoring animation from scratch) | Blender exists; MCE imports-cooks-inspects, it does not compete with Blender | Never planned (the v1.0 curve/timeline editor is the only authoring surface) |
| N6 | Mobile/console/web/VR platforms, 32-bit, non-Windows desktops | Windows 10 x64 is the stated machine; porting multiplies every subsystem's cost | A shipped game with commercial demand on another platform |
| N7 | Networking, accounts, telemetry, achievements, cloud saves, DRM | Solo offline games; online services are a second product | Never planned |
| N8 | Engine-as-product: installer, plugin API, third-party docs, API stability guarantees, asset store | MCE ships *games*; the engine is the curriculum; API-stability discipline taxes the architecture for no anchor-UC benefit | A second developer joins |
| N9 | Mega-scale sparse entities (100k+), GPU-driven everything | Targets are thousands; the archetype ECS already gives the headroom that matters | Profiling shows a real game hitting 10k entities |
| N10 | Runtime mod support, UGC, in-game editors | Shipping untrusted content is a security + format-versioning project | Never planned |
| N11 | Cinematic tooling beyond FR-ENT-110 timelines | Ashen Court's beats fit the action-list/timeline scope | A game with FMV-scale narrative ambitions |
| N12 | Localization/i18n pipeline | Both anchor games are English-only. Structural precaution (finding F23): text renders through the `CHNK_STR` string-table chunk (`StringId` → UTF-8 span, assets §2) with inline-text fallback for debug labels — MVP ships English-only tables, so a reversal costs one new `CHNK_STR` per locale + a font-atlas rebuild, not a content rewrite | A committed commercial release in another language |
| N13 | Engine-level live code hot-reload | The Handmade Hero DLL split is adopted as *architecture* (testability, seam), but hot reload is fragile on MSVC and unnecessary once content iteration is editor-first | After Ashen Court, if C++ iteration pain is measured, not felt |
| N14 | A second graphics backend (D3D12/Vulkan) on a schedule | The RHI seam keeps the exit door open; a second backend before two shipped games is architecture tourism | After both anchor games ship, as an explicit learning milestone |
| N15 | Accessibility beyond standard rebinding (full remap UX, colorblind filters, screen readers) | Important, but beyond what two small games require; recorded as a decision, not an oversight | A shipped game published commercially |
| N16 | Cross-machine/cross-driver bit-identical rendering; golden-image tests as a CI gate | Driver variance makes byte equality a false-alarm machine; determinism is defined narrowly (FR-RUN-040) | Never planned |

---

## 6. Feature tiers

Tier definitions: **MVP** = the minimum viable engine (MVE) plus the minimum that makes it an *engine* rather than two demos (the editor can author the 2D game; cook is deterministic; CI green). **v0.5** = the 3D engine is real, physics and audio are respectable, the editor stops fighting the user. **v1.0** = UC-D is shipped by the author working exclusively through editor workflows and the `docs/engine/*.md` set, with zero engine-source changes to gameplay systems beyond the ≤ 2-system budget (finding F10 — note this is deliberately *not* a claim about third-party developers; engine-as-product with API stability stays non-goal N8).

### 6.1 Minimum viable engine (MVE) — called out

The MVE is the smallest set of MCE capabilities producing **two artifacts a non-programmer can launch by double-clicking**, exercising the same engine code paths in both:

- **Artifact A** — the single-screen 2D platformer (UC-A MVP size): tile sprites, player run/jump on AABB physics, 2–3 enemy behaviors, collectibles, one SFX set + music loop, start/win/lose states, text-sprite HUD. Zip: `game.exe` + `assets.mce` + SDL3 DLLs.
- **Artifact B** — the 3D lit diorama (UC-B MVP size): one static glTF scene ≥ 10k triangles textured, orbit/zoom camera, one directional light + one shadow map, fog, a node-transform-animated prop. Same zip shape.

**MVE capability checklist** (everything not listed is out of MVE by definition): SDL3 window/input (keyboard+mouse only), fixed 60 Hz loop + interpolation; math/allocators/handles/`Result`/Tracy; RHI + GL 4.5 backend (DSA, staging ring, MDI, shader hot-reload); ECS subset (tables, queries, command buffers, phase pipeline — no hierarchy edges, observers, parallelism); reflection sufficient for inspector + scene save/load + ECS registration; glTF static import + PNG + WAV → `.mce`, cook CLI, CRC + version, mmap load; 2D sprite batch + atlas + one bitmap font; forward 3D with diffuse+specular, 1 directional light + 1 shadow map, fog; 2D physics (AABB+circle, dynamic-vs-static, gravity, contact response, one-way platforms); audio (one-shot + streamed loop through mixer → master bus); editor (docking shell, hierarchy, reflection inspector, save/load, entity CRUD, translate gizmo); Tracy from M0, RenderDoc per render feature, unit tests for math/ECS/`.mce` reader, ASan weekly.

**MVE acceptance criteria:** FR-RUN/FR-SHP gates above, plus: Artifact A runs ≥ 60 fps (p99 < 16.6 ms in Tracy) with zero console errors from a double-click; Artifact B loads to first frame < 1.0 s; both terminate cleanly in a 10-minute soak; a named non-programmer completes Artifact A with no instructions beyond a README paragraph.

### 6.2 Tier contents

**MVP (target: months 0–14 at baseline velocity; ≈ 500–790 h engineering + ≈ 50–70 h content production = ROADMAP M0–M4 + M3.5 stretch, sums of the per-milestone tables — rebalanced per finding F3)** — M0 + M1 + the 3D diorama, trimmed and named:

Project skeleton (CMake presets, vcpkg, CI, target graph) · core runtime (frame allocator, pools, `Handle<T>`, `Result<T,E>`, crash handler, Tracy) · math library with DirectXMath oracle · RHI + GL backend · archetype ECS core · reflection + scene serialization · asset pipeline v1 (glTF static, `.mce`, cook CLI, determinism) · 2D renderer (MDI batch, atlas, text) · fixed-step sim + input action layer (keyboard/mouse) · editor shell (hierarchy, inspector, save/load, translate gizmo, run-in-player) · 2D physics v1 (contact response, one-way platforms; stacking explicitly out) · audio v1 (callback, mixer, ≥ 8 voices, streaming music) · **Artifact A ships**.

**v0.5 (target: months 14–24; ≈ +390–620 h engineering + ≈ +140–220 h content production):**

3D PBR forward renderer (Cook-Torrance GGX; **Forward+ clustered lighting, ≥ 256 point lights — FR-R3D-030; CSM 2–4 cascades — FR-R3D-040**; fog, tonemap) · glTF full animation tracks (single-node TRS already at MVP) · physics v0.5 (sleeping, distance+revolute joints, player CCD, 3D character controller per FR-PHY-080) · audio v0.5 (buses, biquads, limiter; 48 voices) · editor v0.5 (undo/redo, asset browser, rotate/scale gizmos, play-in-editor ≤ 1 s) · change detection + `ChildOf` hierarchy · hot-reload matrix (textures/scenes/prefabs) · job system learned in cook first · full Ember Step (5 levels, gamepad, particles, parallax) · **Artifact B v0.5 (lit diorama with PBR + shadows) and full Ember Step ship**. *(The "≤ 8 point lights + single shadow map" phrasing that used to live here described only the M4 3D-diorama placeholder — the binding spec is the FR table, not this paragraph.)*

**v1.0 (target: months 24–36; ≈ +180–310 h engineering + ≈ +30–50 h content production — UC-D may slip to a named v1.1 per finding F7) — candidates admitted by slip-note only if earlier tiers are green:**

IBL · cascaded shadow maps (if not already) · editor maturity (prefab instantiate + override UI, multi-scene, thumbnails, timeline/curve editor) · runtime UI `mce::ui` powering shipped menus/HUD (FR-UI-010; the pre-approved RmlUi fallback trigger of audio-input-ui R6 stands if `mce::ui` overruns) · packaging (versioned zip, symbolicated crash dumps, shipped debug console) · post FX (bloom, vignette) · Ashen Court complete · **UC-D: a vertical-slice proof game built exclusively through the editor + project template, as the v1.0 proof of engine.** If M8 overruns, the first cuts are thumbnails and the timeline/curve editor (cut-ladder rung 1) — UC-D does not depend on either.

**Explicitly deferred beyond v1.0 (named so they cannot sneak in):** scripting VM (Lua/sol2), skinned animation (unless promoted per Open Question 3), streaming/world-partition, job-parallel runtime scheduling, hot-reloadable game DLL (the platform/game split is designed *for* it now; building it is deferred), Vulkan/D3D12 backend, networking, GPU particles, terrain, navmesh/AI.

### 6.3 The cut ladder (decided now, executed without re-negotiation)

When time pressure hits, cut in this order; each rung names its trigger. One-directional: rungs are re-adopted only when the triggering pressure resolves **and** the next gate is green.

1. **Drop v1.0 niceties** — IBL, cascades, post FX, prefab/override UI, multi-scene; within v1.0 itself the timeline/curve editor goes first, then asset thumbnails (finding F7). *Trigger: any v0.5 slip note.*
2. **Drop editor polish** — undo/redo, asset browser, rotate/scale gizmos; scenes edited via inspector + save/load. *Trigger: editor work exceeding its budget by 25%.*
3. **Degrade 3D** — drop Forward+ (plain forward, 8-light cap), then shadows; the 3D demo ships lit-diffuse. *Trigger: first-lit-3D-scene timebox breach (binding).*
4. **Degrade audio** — drop buses and DSP; mixer = voices → master limiter → device. *Trigger: audio threatening Artifact A's date.*
5. **Degrade assets** — drop the meshoptimizer cook step (ship raw quantized meshes), keep container + CRC + determinism. *Trigger: pipeline work threatening the MVE.*
6. **Degrade physics** — dynamic-vs-static only, no sleeping/joints; platformer uses a kinematic player. *Trigger: solver instability consuming > 3 weeks. (Since finding F2, this rung is M3's default shipping shape — the rung is now a degradation only relative to the M3.5 hardened solver, which may itself slip whole to v0.5.)*
7. **Final cut line** — the MVE reduces to **Artifact A only**; the 3D diorama becomes the first v0.5 milestone. "2D engine first, 3D later" is an acceptable, honest endgame — which is why Artifact B is structured as an addition, not a dependency.

Go/no-go checkpoint: if the 2D game is not playable by month 12 of real time, the correct move is rung 7, not heroics. **The project remains a success under every rung of the ladder — that is the point of the ladder.**

---

## 7. Success criteria for the whole project

1. **Three shipped artifacts.** Artifact A (2D platformer), Artifact B (3D game), and UC-D (a proof game built exclusively through the editor workflows + project template) all pass the non-programmer double-click test on a clean Windows 10 x64 machine — UC-D built with zero engine-source changes to gameplay systems beyond ≤ 2 gameplay systems.
2. **The MVE landed inside its envelope.** The MVE shipped by roughly month 14 at baseline velocity, with actual-vs-estimated hours tabulated and the next gate rescaled (estimate-calibration rule).
3. **The curriculum closed.** All seven learning gates (LRN-1..7) passed: each subsystem's doc written from memory, self-exam recorded, Tracy/RenderDoc artifacts annotated by hand. Every subsystem on the binding from-scratch list is hand-written and documented.
4. **The invariant held.** Every milestone ended in a runnable, double-clickable artifact; no milestone absorbed slipped scope without a written slip note; the cut ladder, when invoked, was executed without re-negotiation.
5. **The budgets held.** Both anchor games: 60 fps at 1080p shipped on the dev machine with ≥ 2 ms headroom; determinism and cook-determinism tests green at every milestone from M1; memory budget tables published and reconciled per gate.
6. **The loop is real.** The 30-second editor loop (UC-C) and the "first hour" test (clone → sample running → modify a system → see it in the editor, following only the README) both pass at v1.0.
7. **It is stable and diagnosable.** 0 crashes in a 2-hour authoring session; every crash produces a symbolicated minidump; every failure path is a `Result` or an assert — no third category.
8. **Velocity survived.** The devlog exists from day one; hours are known to ±2 h at every gate; no two consecutive windows below 6 h/week without the formal down-tier (which is itself a success path, not a failure).

---

## 8. Open questions — RESOLVED (2026-10-07; the user delegated all five decisions to the implementation agent)

1. **Calendar and velocity.** Implementation is agent-driven: the ROADMAP hour bands are **effort budgets, not calendar promises**, and there is no wall-clock date for any milestone. Cadence = one shippable demonstration per milestone gate (the ROADMAP's exit criteria are the calendar). NFR-B-07's velocity trigger is reinterpreted accordingly: if effort overshoots a milestone band, the cut ladder fires — timeboxes never extend. The MVP gate (Artifact A + Artifact B, ROADMAP M4) is the first hard checkpoint.
2. **Dev machine (pinned).** Windows 10 Enterprise 1809 (build 17763), Intel i7-7700 (4C/8T), 32 GB RAM, **Intel HD Graphics 630** (driver 26.20.100.7262 — GL 4.5 on Gen9.5 silicon). Toolchain verified present: VS 2022 Community (MSVC v143 / 14.39), bundled CMake 3.28.0 + Ninja 1.11, Windows SDK 10.0.22621. **All NFR-T budgets and golden-image baselines anchor to the HD 630 on this driver** — the 1M-triangle/12 ms budget is the tightest risk on GT2-class iGPU silicon; measure early (M0 bring-up) and cut on the ladder rather than on paper. **A MacBook Pro M4 (16 GB) becomes the dev machine later** — see the migration note below.
   - **Mac migration note (binding constraint discovered, plan pre-agreed).** macOS caps OpenGL at **4.1**: no DSA, no `ARB_buffer_storage` (persistent mapping), no MDI, no `ARB_shader_draw_parameters`. The committed GL 4.5 backend cannot run there, and the design will **not** be downgraded to a 4.1 subset to accommodate it. When the M4 becomes primary, the sanctioned path is a **second RHI backend** (MoltenVK/Vulkan, or Metal) behind the existing `mce_rhi` seam (ADR-0001) — recorded as a future ADR, budgeted as a bounded translation, not a rewrite. Until then this Windows machine remains the GL 4.5 reference machine. Cross-machine render determinism was never in scope (ADR-0007 pins determinism to one machine/binary); the 16 GB RAM is sufficient for the committed scope.
3. **Ashen Court's protagonist: stylized.** The default stands — **FR-R3D-060 (skinning) stays Should at v0.5** and remains deferred beyond v1.0 unless promoted by an explicit slip note. Ashen Court ships with a node-transform-animated or static-mesh player proxy (per the pre-agreed fallback in UC-B). Nothing on the cut ladder (IBL, post FX) is displaced.
4. **Public release intent: private artifacts for now.** FR-SHP-030 (one-command ship) stays Should; N15 (accessibility) stays Won't for v1.0. Packaging stays minimal. If this changes, it is a REQUIREMENTS slip note, not a silent scope add.
5. **Working titles: confirmed.** "Ember Step" (2D precision platformer, UC-A) and "Ashen Court" (3D arena, UC-B) stand; all FR traceability references keep these names.

---

## Appendix A — Deferred debates (conflicts between drafts, resolved)

Feasibility wins ties for Must-haves; each resolution below is binding and the losing draft's position is recorded so it is not re-argued silently.

| # | Conflict | Resolution | Rationale |
|---|---|---|---|
| A1 | Product: UC-A is a 5-level, gamepad, best-time-persisting platformer at M1. Feasibility: MVE Artifact A is a single-screen, keyboard-only, HUD-free platformer. | Feasibility. MVP ships Artifact A; the full Ember Step is the v0.5 gate. | One developer cannot build the full game and the engine underneath it in the same window; the MVE keeps a shippable artifact in months 0–14 (Hazel-pacing evidence). |
| A2 | Product: undo/redo is Must for the editor. Feasibility cut-ladder rung 2: no undo until v0.5. | Should at v0.5; Must for the v1.0 ship gate. MVP relies on save/load + copy-paste. | Scenes are small at MVP scale; re-opening a scene is cheaper than a reflection-command undo stack built before the reflection system is proven. |
| A3 | Product: prefabs Must at M1. Feasibility: prefabs at v1.0, copy-paste until then. | Prefab cook/instantiate Must at v0.5 (the full Ember Step's enemies/pickups need them); override UI Should at v1.0. | Cook-time template chunks are the binding-compatible mechanism; the *UI* is the expensive part and is deferred. |
| A4 | Product: play-in-editor (< 1 s, pause/step) Must at M1. Feasibility: not in the MVP editor. | MVP ships the "Run in player" button (launches the cooked runtime); play-in-editor with pause/step is Must at v0.5 with the ≤ 1 s budget. | Play-mode needs a proven, deterministic scene-load path first; the run-in-player button satisfies the non-programmer gate at MVP. |
| A5 | Product: gamepad input Must at M1. Feasibility MVE: keyboard only. | Keyboard+mouse Must at MVP; gamepad Must at v0.5 (UC-A full). | SDL3 gamepad support is cheap but its action-map testing is not free; nothing in Artifact A requires a pad. |
| A6 | Product: skinned meshes Must for Ashen Court. Feasibility: skinned animation is cut candidate #1, deferred beyond v1.0. | FR-R3D-060 demoted to **Should** (v0.5); Ashen Court must be shippable with a node-transform-animated or static player proxy. Promotion is Open Question 3. | Skinning + blending is an L effort (70–160 h) competing directly with the M5a timebox; the binding timebox rule says cut features, not extend. |
| A7 | Product: 48 hardware audio voices Must. Feasibility: 8 real voices at MVP. | ≥ 8 at MVP; 48 by v0.5 (FR-AUD-020). | The voice pool is the same code at either size; the count is a tuning constant, not an architecture decision — tier the number, not the system. |
| A8 | Product: golden-image tests at cook, in CI, Must. Feasibility R9: dev-machine-only, perceptual threshold, never a CI gate (driver variance). | Feasibility scope adopted: golden images run at cook on the dev machine with a perceptual threshold (< 0.5% block-mean luminance diff); the merge gate for render features is the human-eyeballed RenderDoc capture. | GL driver variance across machines makes pixel-diff CI a false-alarm machine; the RenderDoc capture gate delivers the actual safety at zero flake cost. |
| A9 | Product: runtime UI (FR-UI-010) Must by M1 for UC-A's menus. Feasibility: MVP games are HUD-free with text sprites; `mce::ui` is v1.0. | Scene-state menus + text-sprite HUD satisfy MVP and v0.5; the full `mce::ui` system is Must **for the v1.0 ship gate** (a shipped game's menus may never be ImGui). | Minimal text/panel primitives can ship early without committing the retained-UI architecture; the binding (ImGui never in runtime) is CI-enforced from M0 regardless. |
| A10 | Product: 3D character controller Must with "hand-written or Jolt via ADR" left open. Feasibility: Jolt is the pre-approved exception; anything beyond a character controller is out. | Capability Must at v0.5 (FR-PHY-080); the vehicle (hand-written SAT/GJK vs Jolt 5 ADR) is an architecture decision, recorded by ADR either way. | Product demands the observable capability; the from-scratch binding names the exception valve; the architecture document picks the mechanism. |
| A11 | Experience draft: inspector edits visible same-frame (DX-E3) vs editor-owns-scene binding (edits go to the canonical document on save). | Same-frame visibility is against the editor's **live preview world**; the canonical scene document is written on save. Both binding and DX-E3 hold as worded. | The editor mutates its preview and stages commands through the reflection-driven undo stack; the canonical document remains the single serialized truth. |
| A12 | Experience draft DX-E2 editor ≥ 60 fps with 5k entities vs feasibility's smaller MVP scale. | Kept as NFR-B-06 with sample-project scale at v0.5. | The archetype ECS design makes this nearly free; it is a budget line, not a work item. |

---

## Appendix B — Sources relied on (canonical; web search unavailable)

- Jason Gregory, *Game Engine Architecture*, 3rd ed. — subsystem scope, audio ch. 13, object models ch. 14, allocators ch. 6.
- Glenn Fiedler, "Fix Your Timestep," Gaffer On Games — fixed-timestep loop, determinism scope, audio scheduling.
- Erin Catto, Box2D GDC slide series — 2D solver and character-controller shape; solver-tuning realism.
- Casey Muratori, *Handmade Hero* — platform/game split, arenas, incremental shippability, honest hour costs.
- Bob Nystrom, *Game Programming Patterns* — Component, Update Method, Event Queue, Dirty Flag, Object Pool.
- Bitsquid/Our Machinery blog corpus (Niklas Frykholm, Tobias Persson) — data-oriented entity systems, memory budgets, minimal explicit render layers.
- Skypjack, *ECS back to basics*; flecs v3, EnTT, Bevy ECS documentation — archetype/sparse-set designs (studied, never linked).
- Timothy Ford, "Overwatch Gameplay Architecture and Netcode," GDC 2017 — deferred commands, FSM-as-systems.
- Unity documentation (Input System, uGUI, prefabs, import cache, DOTS); Unreal (RHI, cook, UHT cautionary); Godot 4 (servers, editor self-hosting); O3DE (prefab-first, inspector-first warning).
- Fabien Sanglard, *Game Engine Black Books*; Hazel (The Cherno) series — solo-engine pacing and stall modes; Piccolo, Overload, Kohi.
- Timur Doumler, *C++ in the Audio Context*, CppCon 2015 — real-time audio rules; RBJ Audio-EQ-Cookbook; Freeverb (Schroeder-Moorer).
- Tracy and RenderDoc documentation; MSVC `/fsanitize=address`, `MiniDumpWriteDump`, vcpkg manifest, CMake presets documentation.
- Repo-internal inputs: `docs/drafts/requirements-{product,experience,feasibility}.md` (superseded by this document), `docs/research/{existing-engines,ecs-gameplay,platform-engineering,audio-input-ui,physics,rendering-2d,rendering-3d,assets-tools}.md`, `docs/tech/panel-cpp-modern-gpu.md`.
