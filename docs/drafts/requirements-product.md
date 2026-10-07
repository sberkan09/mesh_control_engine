# MCE Product Requirements — Product Lens

**Project:** Mesh Control Engine (MCE) · **Domain:** product requirements · **Date:** 2026-10-06
**Status:** DRAFT (product lens of the requirements set; architecture and plan lenses are separate documents)
**Inputs:** stack decision (docs/tech/, Candidate 2 binding), docs/research/existing-engines.md, ecs-gameplay.md, audio-input-ui.md, platform-engineering.md. Binding constraints from the stack decision are treated as givens here and are restated only where they constrain product scope.

---

## 1. What this document decides

This is the **product lens**: *what MCE must do*, for whom, at what quality bar, and — just as importantly — what it must **not** attempt. It deliberately does not decide *how* anything is built; the architecture and plan documents own that. Where a requirement could be read as an implementation instruction, the requirement states the observable capability and leaves the mechanism to architecture.

Prioritization is **MoSCoW**, applied with one rule of thumb: **a requirement is Must only if one of the three anchor use cases (§3) is unshippable without it.** Everything else is Should (a shipped small game visibly suffers without it), Could (delight), or Won't (explicit non-goal, §6).

---

## 2. Product definition

**Mesh Control Engine is a from-scratch 2D/3D game engine with an integrated editor, built by one developer on Windows 10 x64, whose measure of success is: two small complete games ship from it, and the developer understands every subsystem in it.**

The product has two equally real user roles, both played by the same person:

1. **The engine author** — the learner. Every subsystem on the binding from-scratch list (math, archetype ECS, 2D physics, audio mixer, glTF importer, cook pipeline, RHI, reflection, renderer) exists to be *built*, not consumed. Product requirements must never quietly outsource one of these to a library.
2. **The game developer using the engine** — the customer surrogate. For this persona the engine is judged by a brutal standard: *can I make a small game in it without fighting it, and would a non-programmer be able to launch the result?* (Binding milestone gates already demand launchable artifacts.)

### 2.1 Product pillars

These four sentences are the tie-breakers for every prioritization argument in this document:

- **P1 — Shippable beats impressive.** A finished 2D platformer in year 1 and a finished 3D third-person game in year 2 outrank any technically impressive subsystem that doesn't serve them.
- **P2 — The engine is the curriculum.** Hand-written per the binding list; a library may only replace *boilerplate*, never *architecture* (the RHI seam and the Jolt/RmlUi ADR exceptions are the only sanctioned outsourcings).
- **P3 — Editor-first iteration.** Content changes — levels, tuning, prefabs, scenes — must never require recompiling C++. If a content tweak needs a rebuild, that is a product bug.
- **P4 — Deterministic and observable by default.** Fixed 60 Hz sim, deterministic iteration, Tracy zones from M0, RenderDoc-verified rendering, frame budget tables per milestone. Performance and debuggability are features, not chores.

### 2.2 Scale envelope ("enough" numbers)

MCE targets **small complete games**. These numbers are the product's performance envelope — requirements are sized to them, and anything far beyond them is a Won't (§6):

| Dimension | Envelope |
|---|---|
| Entities per scene | ~500–5,000 typical; 10,000 hard design ceiling |
| 2D sprites per frame | 10,000 batched @ 60 fps |
| 3D draw calls per frame | ~1,000 pre-batching; MultiDrawIndirect keeps submit cost flat |
| Scene triangle budget | ~1–2 M triangles @ 1080p60 on the dev machine |
| Simultaneous audio voices | 48 hardware + unlimited virtual (priority stealing) |
| Cook time, full 2D game content | < 60 s |
| Play-mode enter (editor) | < 1 s |
| Editor startup to usable | < 5 s |
| Frame budget | 16.6 ms total (binding); per-stage table tracked per milestone |

---

## 3. Anchor use cases

Every Must requirement below traces to one of these. Each use case ends with an **acceptance checklist** — these are the concrete gate criteria the milestone documents should adopt verbatim.

### 3.1 UC-A — "Ember Step": a 2D precision platformer (M1 target)

A precision platformer in the Celeste/Hollow Knight-controls lineage, ~5 hand-authored levels, 60 fps, keyboard + gamepad. The player runs, jumps (with coyote time and jump buffering), wall-slides, and dashes across crumbling platforms and moving platforms, avoiding patrolling and chasing enemies, collecting embers, and reaching a goal gate. A timer tracks best times, persisted between sessions. Parallax ruin backgrounds, dust/impact particles, a looping music track and ~20 SFX. Main menu, level select, pause, results screen with best time.

**What it demands of the engine** (this is the requirements seed for subsystems FR-R2D, FR-PHY, FR-AUD, FR-INP, FR-EDT, and the M1 slice of everything else):

| Demand | Traces to |
|---|---|
| Batched sprites, sprite-sheet animation with playback events, parallax layers, tinting, sort layers | FR-R2D-010..070 |
| Swept character controller: ground/wall/ceiling flags, coyote time, moving-platform carry, one-way platforms | FR-PHY-010..070 |
| Enemies as FSM data (idle/patrol/chase) inspectable in the editor; pickups as prefabs | FR-ENT-090, FR-ENT-070 |
| Music streaming + SFX; volume sliders in options | FR-AUD-010..060 |
| Keyboard + gamepad action mapping with deadzones; deterministic per-tick sampling | FR-INP-010..040 |
| Level authoring in the editor: place tiles/platforms/prefabs, set collisions, gizmo-move, play-in-editor, save | FR-EDT-010..120, FR-R2D-075 |
| Runtime menu/pause/HUD built on the runtime UI system (never ImGui) | FR-UI-010 |
| Best-time persistence | FR-RUN-065 |

**Acceptance checklist (M1 gate, binding):** a non-programmer can double-click the shipped exe, play all 5 levels to the goal with keyboard or gamepad, see their best time persist across sessions, and the game runs at 60 fps on the dev machine with the frame budget documented. All content (levels, prefabs, tuning values) was authored in the MCE editor, not in code.

### 3.2 UC-B — "Ashen Court": a small 3D third-person game (M2–M3 target)

A compact third-person combat arena: one authored glTF level (a ruined courtyard, ~300k triangles), a skinned player character (idle/run/attack/hit/death clips, cross-faded), three enemy types driven by FSMs (idle/patrol/chase/attack), melee combat with hit reactions, health pickups, one directional sun + 4–6 point lights, PBR materials, cascaded shadows from the sun, bloom + tonemap, third-person orbit camera with wall collision, spatialized combat and footsteps audio, HUD + main menu + pause.

**What it demands of the engine** (the M2/M3 slice):

| Demand | Traces to |
|---|---|
| glTF 2.0 import → cooked meshes/materials; PBR metallic-roughness; Forward+ clustered lights; CSM shadows | FR-R3D-010..060 |
| Skinned meshes + animation blending; footstep/hit events driving audio | FR-R3D-060, FR-AUD-070 |
| 3D character controller: capsule, slopes, step-up; orbit camera with collision | FR-PHY-080 |
| 3D gizmo workflow, prefab placement, light placement with viewport preview | FR-EDT-030, FR-EDT-080 |
| Golden-image tests + RenderDoc verification for every new render feature | FR-DBG-020 |
| 60 fps at 1080p on the dev machine with the frame budget table filled in | §2.2, FR-DBG-010 |

**Acceptance checklist (M2/M3 gate):** the game is launchable by a non-programmer, holds 60 fps at 1080p on the dev machine, the camera never clips through level geometry in the shipped build, and every render feature in it has a RenderDoc capture and a golden image in the repo.

### 3.3 UC-C — The editor experience that binds them (M0 shell → M4 maturity)

The editor is the product surface that makes UC-A and UC-B buildable, and it is a first-class use case, not a tool afterthought. The anchor is the **30-second loop**: launch editor → open scene → drag a prefab into the viewport → gizmo-nudge it → press Play → watch it behave → press Stop (scene reverts) → tweak a component in the inspector → Save → Cook → launch the game. If any step in that loop takes minutes instead of seconds, the engine fails UC-C regardless of its runtime merits.

**Demands:** docking shell (hierarchy/inspector/asset browser/viewport/console), reflection-driven inspector for *any* component with zero per-type editor code, gizmos with snapping, undo/redo, play-in-editor with pause and single-step, prefab instantiate/override, cook from the editor with surfaced errors, asset hot-reload, debug overlays (colliders/lights/bounds/event flow), Tracy and RenderDoc launch points. Editor owns the canonical scene; the runtime never mutates it (binding).

**Acceptance checklist (M4 gate):** authoring a new UC-A level end-to-end — empty scene to cooked, launchable game content — takes under 30 minutes using only editor features documented in the engine's own docs, with no code changes and no editor crashes (0 crashes in a 2-hour authoring session).

---

## 4. Functional requirements by subsystem

Conventions: **Priority** = MoSCoW. **Anchor** = A (UC-A), B (UC-B), C (UC-C), ★ = shared engine capability. IDs are stable and gapped by 10 for later insertions. Indicative milestone tags refer to the M0–M5 gates from the stack decision; the plan document owns actual sequencing.

### 4.1 FR-RUN — Runtime core, frame loop, application

| ID | Requirement | Priority | Anchor | Notes |
|---|---|---|---|---|
| FR-RUN-010 | The runtime is a standalone executable that boots from cooked `.mce` content only: opens a window (SDL3), initializes audio/GL, loads its boot scene, and runs. No dev dependencies, no editor, no tools required on the target machine. | Must | A,B | Binding asset policy; shipping detail in FR-SHP |
| FR-RUN-020 | Fixed-timestep simulation at 60 Hz with interpolated rendering (*Fix Your Timestep*), spiral-of-death clamp (max 5 catch-up steps/frame), `alpha` exposed to render interpolation. | Must | A,B | Binding; Gaffer On Games |
| FR-RUN-025 | Headless simulation mode: the fixed-step sim runs without a window/renderer (used by the determinism test harness and replay). | Must | * | Prerequisite for FR-RUN-040's CI test |
| FR-RUN-030 | Application shell scenes: boot/main-menu/gameplay/pause as runtime scene states with push/pop transitions, shipped in both anchor games. | Must | A,B | |
| FR-RUN-040 | Deterministic fixed-step sim: identical inputs + content ⇒ identical component-state hashes over N=10k ticks; enforced as a CI test from M1. | Must | * | Binding; platform-engineering §7.2(4) |
| FR-RUN-045 | Frame pacing: vsync on by default; uncapped mode for dev; frame time and budget tables visible in dev overlay. | Must | A,B | Binding budget table |
| FR-RUN-050 | Time scale: global sim speed multiplier (pause, slow-mo) usable as debug tool and gameplay feature. | Should | A,B | |
| FR-RUN-060 | Application lifecycle handling: focus loss/minimize pauses sim; clean quit path flushes saves before exit (crash handler ordering per platform-engineering §6.3). | Should | A,B | |
| FR-RUN-065 | Save/persistence: reflection-serialized save data to `%LOCALAPPDATA%/MCE/<game>/saves`; versioned and forward-compatible-read-rejected. | Should | A | UC-A best times; save games for UC-B |
| FR-RUN-070 | Replay: record per-tick input snapshots, replay deterministically in dev builds. | Should | * | Nearly free given FR-RUN-030/040; intended as a debugging superpower |
| FR-RUN-080 | Multiple runtime windows (e.g., a debug second window). | Could | * | |
| FR-RUN-090 | Runtime mod support, live-reloaded native code in shipping, plugins. | Won't | | §6 |

### 4.2 FR-ENT — Entities, scenes, behavior model

| ID | Requirement | Priority | Anchor | Notes |
|---|---|---|---|---|
| FR-ENT-010 | Archetype ECS: entities as generational-index handles (the only cross-frame reference gameplay code may hold), components as reflected plain structs, queries, explicit named phases with explicit intra-phase order. | Must | * | Binding; ecs-gameplay §13 |
| FR-ENT-020 | Structural changes deferred through per-phase command buffers; user queries never invalidated mid-iteration. | Must | * | ecs-gameplay §5 |
| FR-ENT-030 | Typed, double-buffered event queues for gameplay semantics (`EnemyDied`, `LevelLoaded`, …); each event has an explicit producer and consumers, visible in the editor's event-flow debug view. | Must | A,B | ecs-gameplay §6 |
| FR-ENT-040 | Change detection: per-row changed ticks with `is_added()/is_changed()` filters; drives dirty transform propagation and render interpolation. | Must | * | ecs-gameplay §7, M2 |
| FR-ENT-050 | Transform hierarchy: single parent (`ChildOf`), engine-maintained children, depth-ordered dirty propagation of world transforms; editor shows the tree. | Must | A,B | |
| FR-ENT-060 | Scenes: editor-authored scene documents are canonical; cooking produces `.mce` scene chunks (component columns + id remap); the runtime loads and never mutates the canonical scene. | Must | * | Binding architecture authority |
| FR-ENT-070 | Prefabs: authored entity sub-trees cooked to template chunks; instantiate = bulk row copy + handle remap + instance overrides; nested prefabs flattened at cook. | Must | A,B | ecs-gameplay §9; enemies/pickups/props are prefabs in both anchor games |
| FR-ENT-080 | Gameplay starter kit: camera, sprite, animated sprite, mesh renderer, light, `RigidBody2D`/colliders, audio source, text label, and common singleton resources (Time, Input, ActiveCamera). | Must | A,B | The kit both anchor games share |
| FR-ENT-090 | FSM support: state-as-data components + per-state systems + small `fsm.h` helper (timed states, entry/exit actions); states are inspectable and save-game-able. | Must | A,B | Enemy AI in both anchor games |
| FR-ENT-100 | Scripted gameplay beats: data-driven action-list sequences (wait/move/play_anim/emit/call) advanced by a runner system. | Should | B | Boss/intro beats in UC-B |
| FR-ENT-110 | Timelines: cooked track/clip assets (transform keys, clips, audio, camera cuts, event triggers) with an editor timeline view. | Should | B | M4; doubles as the animation-authoring surface |
| FR-ENT-120 | Sub-scene streaming (multiple scene chunks per world, async load). | Could | | Trigger: a game with more content than one scene chunk comfortably holds |
| FR-ENT-130 | Observers/trigger cascades, runtime prefab inheritance (flecs `IsA`), behavior trees/GOAP, stackful coroutines, C++20 coroutine gameplay scripting. | Won't | | ecs-gameplay §6/§9/§11.2; re-entry triggers recorded there |

### 4.3 FR-R2D — 2D rendering

| ID | Requirement | Priority | Anchor | Notes |
|---|---|---|---|---|
| FR-R2D-010 | Batched sprite renderer: one MultiDrawIndirect batch per material/atlas; 10,000 sprites @ 60 fps on the dev machine (§2.2). | Must | A | Binding GL discipline |
| FR-R2D-020 | Sprite component: texture region, pivot, flip, color tint, sort layer + order, visible flag. | Must | A | |
| FR-R2D-030 | Sprite-sheet animation: frame grid or atlas-coordinate frames, fps, loop/pingpong/once, frame-indexed playback events (used for footstep SFX). | Must | A,B | |
| FR-R2D-040 | 2D camera: position/zoom/rotation, primary-camera semantics, pixel-snap option, screen-shake helper. | Must | A | |
| FR-R2D-050 | Parallax background layers (factor-per-layer scrolling). | Must | A | |
| FR-R2D-060 | Text rendering: stb_truetype-baked font atlases at cook, SDF-optional, used for HUD, debug overlay, and runtime UI. | Must | A | |
| FR-R2D-070 | CPU-simulated particle systems: point/area emitters, initial velocity/spread, gravity/drag, color-over-life, size-over-life, bursts; drawn through the sprite batch. | Must | A | Dust, impacts, pickups |
| FR-R2D-075 | Tile/level-block chunks: tile grids cooked to chunk quads (32×32 tiles) that flow through the same sprite batch as ordinary sprites — no separate tilemap renderer — with per-chunk collision geometry exported to physics and chunk-local re-bake on edit. Authoring path is FR-EDT-120's placement tools. | Must | A | rendering-2d.md D5; UC-A's 5 levels are tile-based content |
| FR-R2D-080 | Editor game view renders into a render target widget (2D and 3D) with correct DPI scaling. | Must | C | |
| FR-R2D-090 | Lit 2D sprites: normal-map lighting from a small set of 2D lights (ambient + point). | Should | A | Hollow-Knight-style mood lighting |
| FR-R2D-100 | Fullscreen post effects on the 2D path: vignette, color-grade LUT, cheap chromatic aberration. | Should | A | |
| FR-R2D-110 | Render-to-texture: render a camera to a texture usable as a sprite (in-game monitors/portals). | Could | | |
| FR-R2D-120 | Skeletal/mesh-deformed 2D animation (Spine/Live2D-style). | Won't | | Re-entry trigger: a committed game concept needs it |
| FR-R2D-130 | Shadow-casting 2D lights (occluder casting). | Won't | | Same trigger discipline |

### 4.4 FR-R3D — 3D rendering

| ID | Requirement | Priority | Anchor | Notes |
|---|---|---|---|---|
| FR-R3D-010 | glTF 2.0 import at cook: meshes → meshoptimizer vertex-cache/vertex-fetch optimized, quantized vertex streams in `.mce`; materials + textures carried through. | Must | B | Binding asset policy |
| FR-R3D-020 | PBR metallic-roughness (Cook-Torrance GGX) with basecolor/normal/ORM texture set; albedo-only fallback material. | Must | B | |
| FR-R3D-030 | Forward+ clustered lighting: directional + point + spot lights, thousands of lights survivable via clustering, light counts beyond 8 not a special case. | Must | B | Stack decision |
| FR-R3D-040 | Cascaded shadow maps for the sun (2–4 cascades, stable fit). | Must | B | |
| FR-R3D-050 | Frustum culling per entity before MDI submission. | Must | B | |
| FR-R3D-060 | Skinned meshes: glTF skin import, joint-palette skinning, animation clip sampling (lerp/slerp), cross-fade blending between clips, playback events. | Must | B | |
| FR-R3D-070 | 3D cameras: perspective + ortho; primary-camera semantics shared with 2D. | Must | B | |
| FR-R3D-080 | Sky: gradient skydome and equirect skybox texture path. | Must | B | |
| FR-R3D-090 | Post chain (3D): tonemap, bloom, FXAA. | Should | B | Timebox valve: per binding, cut bloom/FXAA before extending the M2 timebox |
| FR-R3D-100 | IBL: environment probe with cook-time convoluted specular cubemap; materials pick it up by proximity. | Should | B | First on the cut list if M2 slips (binding timebox rule) |
| FR-R3D-110 | 3D particles (billboards through the same MDI batch). | Should | B | |
| FR-R3D-120 | Simple screen-space AO (half-res, e.g., GTAO-lite or SSAO). | Could | B | |
| FR-R3D-130 | Decals (projected, limited budget). | Could | B | |
| FR-R3D-140 | Mesh LOD system (screen-space selection; editor-authored or cook-computed). | Could | B | UC-B fits in budget without LOD |
| FR-R3D-150 | Ray tracing, virtualized geometry, volumetric fog/clouds, global illumination beyond probes, terrain systems, SSR. | Won't | | §6; GL 4.5 ceiling is honestly inside "small complete games" |

### 4.5 FR-AST — Assets and the cook pipeline

| ID | Requirement | Priority | Anchor | Notes |
|---|---|---|---|---|
| FR-AST-010 | Interchange formats are exactly: glTF 2.0 (`.gltf`/`.glb`), PNG/JPG, WAV. The shipped runtime never parses JSON/XML/PNG/WAV (binding). | Must | * | |
| FR-AST-020 | Cook step producing versioned chunked `.mce` containers (magic + version + CRC + 16-byte-aligned payload), memory-mapped at load; handles are generational. | Must | * | Binding |
| FR-AST-030 | Deterministic cook: cooking the same source tree twice yields byte-identical outputs; CI-checked. | Must | * | Binding |
| FR-AST-040 | Stable asset GUIDs surviving moves/renames; cook fails with a *diagnosable* error listing broken references. | Must | * | |
| FR-AST-050 | Texture compression at cook: BC7 for color, BC5 for normals; BC1/BC5/BC7 selection per texture class. | Must | B | VRAM sanity for 3D |
| FR-AST-060 | Shader pipeline: GLSL validated offline by glslangValidator at cook/CI; variants enumerated and precompiled at cook; shader hot-reload in the editor in milliseconds (GL 4.5 advantage, must be preserved as a feature). | Must | * | Binding GL discipline |
| FR-AST-070 | Cook CLI (`mce_cook`): scriptable, exit codes for CI, cooks a game content tree to a shippable pack. | Must | * | |
| FR-AST-080 | Incremental cook with content-hash cache (Unity-style import cache at solo scale). | Should | * | Cook < 60 s for UC-A regardless |
| FR-AST-090 | Asset hot-reload in the editor: texture/mesh/scene/prefab changes on disk re-cook and refresh the viewport without restart. | Should | C | Core of the UC-C loop |
| FR-AST-100 | Audio cook: WAV → normalized 48 kHz float32 `.mce` chunks; IMA-ADPCM fallback for size. | Must | A | audio-input-ui |
| FR-AST-110 | Asset dependency graph visualization; "find references" from the browser. | Could | C | |
| FR-AST-120 | Runtime JSON/XML/PNG parsing, asset encryption, on-the-fly asset import in shipped games, network streaming install. | Won't | | Binding + §6 |

### 4.6 FR-PHY — Physics and character control

| ID | Requirement | Priority | Anchor | Notes |
|---|---|---|---|---|
| FR-PHY-010 | 2D rigid bodies: static / kinematic / dynamic; AABB, circle, and (convex) polygon colliders; contact manifolds. | Must | A | Hand-written per binding (Catto) |
| FR-PHY-020 | Sequential-impulse 2D solver with friction + restitution; stable stacking of small piles at 60 Hz. | Must | A | Box2D GDC slides as the reference |
| FR-PHY-030 | Swept 2D character controller: move-and-collide with ground/wall/ceiling flags, slope limits, **moving-platform carry**, and one-way platforms. | Must | A | The heart of UC-A |
| FR-PHY-035 | Sensors/triggers: overlap events with no physical response (pickups, kill zones, checkpoints). | Must | A,B | |
| FR-PHY-040 | Queries: raycasts, shape casts, overlap tests — usable from gameplay systems deterministically. | Must | A,B | |
| FR-PHY-050 | Broadphase (AABB tree or sort-and-sweep) handling thousands of dynamic bodies within the frame budget. | Must | A | |
| FR-PHY-060 | Joints: distance/spring, revolute, prismatic (enough for platforming contraptions). | Should | A | |
| FR-PHY-070 | Simd-friendly broadphase hotspots (SoA sweeps) behind scalar-first math per platform decision. | Should | * | |
| FR-PHY-080 | 3D character controller for UC-B: capsule with slope/step handling and camera- and character-vs-mesh sweeps. Implementation is hand-written SAT/GJK **or** Jolt 5 via the pre-approved ADR — product demands the capability, architecture picks the vehicle and records the ADR either way. | Must | B | Binding exception valve |
| FR-PHY-090 | 3D dynamic rigid bodies (knockable props, thrown objects) via Jolt-via-ADR. | Could | B | UC-B must be shippable without it |
| FR-PHY-100 | Ragdolls, vehicles, cloth, soft bodies, destruction, GPU physics, networked physics rollback. | Won't | | §6 |

### 4.7 FR-AUD — Audio

| ID | Requirement | Priority | Anchor | Notes |
|---|---|---|---|---|
| FR-AUD-010 | Mixer hand-written on the SDL3 device callback: 48 kHz stereo float32 internal format, real-time audio thread with lock-free command FIFO (no allocations/locks/ECS access in the callback). | Must | * | Binding from-scratch list |
| FR-AUD-020 | Voice pool: 48 hardware voices with priority stealing, unlimited virtual voices; one-shot and looped playback with sample-accurate start. | Must | A | |
| FR-AUD-030 | Bus graph Master/Music/SFX/UI with per-bus gain; user volume options (options screen) wired to buses. | Must | A,B | |
| FR-AUD-040 | Streaming music/dialog from memory-mapped `.mce` via ~100 ms ring buffer; SFX fully in-memory. | Must | A | |
| FR-AUD-050 | 2D spatialization (pan/power) and 3D spatialization (distance attenuation + pan, listener = camera). | Must | B | 2D-only pan acceptable for UC-A |
| FR-AUD-055 | Gameplay→audio via events: `AudioSource` component + event-driven one-shots (footsteps from animation events, hits from combat). | Should | A,B | |
| FR-AUD-060 | DSP chain: RBJ-cookbook biquad EQ per bus, Freeverb-style reverb send, look-ahead master limiter. | Should | A,B | M3 |
| FR-AUD-070 | Sweeteners: random pitch/variance, crossfades, bus ducking (music under SFX). | Could | A | |
| FR-AUD-080 | HRTF/convolution reverb, procedural/scored music systems, external codec libraries at runtime, FMOD/Wwise-style authoring integration. | Won't | | audio-input-ui §; §6 |

### 4.8 FR-INP — Input

| ID | Requirement | Priority | Anchor | Notes |
|---|---|---|---|---|
| FR-INP-010 | Device layer via SDL3: keyboard, mouse, XInput-class gamepads (SDL GameController mapping), text input; gameplay code never sees `SDL_Event`. | Must | A,B | |
| FR-INP-020 | Action mapping layer: named actions, bindings (key/button/axis), action maps, per-device deadzones and response curves; authored as data, editable in the editor. | Must | A,B | Unity Input System-shaped |
| FR-INP-030 | Input sampled per 60 Hz sim tick with edges derived from device-state snapshots — deterministic under varying render rates; unit-tested with synthetic event streams. | Must | * | Binding-adjacent; enables FR-RUN-070 |
| FR-INP-040 | Runtime rebinding + persisted bindings (options UI). | Should | A | |
| FR-INP-050 | Rumble/haptics via SDL gamepad rumble. | Could | A,B | |
| FR-INP-060 | Touch, multiple mice, VR controllers, raw device access beyond SDL's abstractions, more than 4 gamepads as a supported scenario. | Won't | | §6 |

### 4.9 FR-EDT — The editor (UC-C)

| ID | Requirement | Priority | Anchor | Notes |
|---|---|---|---|---|
| FR-EDT-010 | Docking shell (ImGui docking branch): hierarchy/entity tree, inspector, asset browser, viewport (2D+3D), console, main toolbar (play/pause/step, cook, save). Multi-viewport off. | Must | C | |
| FR-EDT-020 | Reflection-driven inspector: every registered component inspectable with zero per-type editor code; field types: numeric drag, bool, enum dropdown, color, string, asset reference picker, entity reference picker, read-only computed. | Must | C | Binding reflection authority |
| FR-EDT-030 | Gizmos: translate/rotate/scale (ImGuizmo via one-page ADR), world/local toggle, grid, configurable snapping; entity picking in both 2D and 3D viewports. | Must | C | |
| FR-EDT-040 | Undo/redo as a reflection-driven command stack covering scene/prefab edits; bounded history. | Must | C | |
| FR-EDT-050 | Entity operations: create/delete/duplicate, drag-drop from asset browser, copy/paste, multi-select with grouped gizmo transform. | Must | C | |
| FR-EDT-060 | Play-in-editor: enter/exit play builds a runtime instance from the scene document in < 1 s; play-mode edits are discarded on stop; pause + single-step + slow-mo; live inspection of entities/events. | Must | C | The heart of the 30-second loop |
| FR-EDT-070 | Cook integration: cook button + error list panel surfacing cook/shader/import errors with jump-to-asset. | Must | C | |
| FR-EDT-080 | Prefab workflow: instantiate, open-prefab-asset editing, override display on instances, apply/revert overrides. | Should | A,B | |
| FR-EDT-090 | Debug overlays: colliders, bounds, lights, camera frustums, navigation-free scene stats, live event-flow monitor, ECS archetype/table metrics, Tracy launch button. | Should | C | |
| FR-EDT-100 | Scene document safety: text serialization (editor-side), versioned format, autosave + crash-recovery backup. | Should | C | |
| FR-EDT-110 | Asset browser maturity: thumbnails, type filtering, import status, rename-with-GUID-stability. | Should | C | |
| FR-EDT-120 | 2D authoring aids: grid/snap placement for level blocks, rectangle-fill placement tool (proto-tile painting without a full tilemap system). | Should | A | Fallback for FR-EDT-130's full tool |
| FR-EDT-130 | Timeline/curve editor for FR-ENT-110 timelines and animation authoring. | Should | B | M4 |
| FR-EDT-140 | Multi-scene tabs, scene-diff view, in-editor profiling dashboards beyond the overlay. | Could | C | |
| FR-EDT-150 | Visual scripting, node-graph material editors, collaborative/multi-user editing, plugin/extension API, editor theming/localization. | Won't | | §6 |

### 4.10 FR-UI — Runtime game UI (deferred, but designed now)

Binding context: ImGui is editor/debug-only; shipped-game UI is explicitly deferred debt that must be a separate system (`mce::ui`), never folded into ImGui code paths.

| ID | Requirement | Priority | Anchor | Notes |
|---|---|---|---|---|
| FR-UI-010 | Shipped games have main menu, pause, options (volume/keybinds/resolution), and HUD built on the **runtime UI system** — never ImGui. Minimal primitives (text, 9-slice panels, buttons) land by M1 for UC-A; the full system matures M4–M5. | Must | A,B | |
| FR-UI-020 | `mce::ui` retained scene graph: Unity-uGUI-style anchored-rect layout, hierarchy, input routing; rendered through the shared 2D MDI sprite batch path. | Should | A,B | Design doc M4 (audio-input-ui) |
| FR-UI-030 | Data binding: UI observes gameplay state read-only via reflection + events; gameplay never reaches into UI. | Should | A,B | |
| FR-UI-040 | Extra widgets: sliders, checkboxes, scroll view; typewriter text effects; safe-area scaling. | Could | A | |
| FR-UI-045 | Shipped debug console (convars/confuncs on `mce::ui`) for post-ship support. | Should | * | audio-input-ui |
| FR-UI-048 | Immediate-mode UI in shipped games; ImGui anywhere in runtime builds (CI-enforced target wall). | Won't | | Binding |
| FR-UI-060 | Rich-markup/HTML-ish UI, IME-heavy text entry, localization pipeline. | Won't | | RmlUi remains the pre-approved ADR fallback if a game genuinely needs markup — trigger documented, not exercised by default |

### 4.11 FR-DBG — Diagnostics, profiling, debuggability (product-visible)

| ID | Requirement | Priority | Anchor | Notes |
|---|---|---|---|---|
| FR-DBG-010 | Tracy zones from M0: frame mark, sim tick, ECS iteration, render submit, cook steps; GPU zones; memory hooks. Frame-budget table filled per milestone and kept next to a Tracy capture. | Must | * | Binding |
| FR-DBG-020 | Every new render feature verified in a RenderDoc capture before merge; golden-image tests at cook for renderer correctness (perceptual threshold, not byte equality). | Must | * | Binding |
| FR-DBG-030 | Crash handling: SEH filter + minidump to `%LOCALAPPDATA%/MCE/crashes` in editor and runtime; PDBs archived per CI build; WER suppressed; no silent corrupt-save paths. | Must | * | platform-engineering §6.3 |
| FR-DBG-040 | Debug console (editor): convars/confuncs, log panel with levels, no hot-path logging. | Must | C | |
| FR-DBG-050 | Sim inspection: pause, single-step, scrub FR-RUN-070 replays; inspect any entity's component state live. | Should | * | The determinism payoff, made usable |
| FR-DBG-060 | Dev overlay in shipped-game dev builds: fps, frame breakdown, entity counts, audio voices. | Should | * | |
| FR-DBG-070 | Capture-to-GIF/video from the dev overlay (dev-log/itch page material). | Could | * | |
| FR-DBG-080 | Remote profiling, fleet crash aggregation/symbol servers, telemetry in shipped games. | Won't | | §6 |

### 4.12 FR-SHP — Shipping & platform

| ID | Requirement | Priority | Anchor | Notes |
|---|---|---|---|---|
| FR-SHP-010 | Shipped game = game `.exe` + cooked `.mce` pack (+ data dir for saves/logs/crashes); launches on a clean Windows 10 x64 machine with at most the MSVC redistributable as an external requirement. | Must | A,B | |
| FR-SHP-020 | Shipped builds: exceptions/RTTI off, Tracy compiled out except FrameMark, asserts per policy (`MCE_ASSERT` debug-only, `MCE_VERIFY` live), `/LTCG` release. | Must | * | Binding idioms |
| FR-SHP-030 | One-command ship: cook + package + zip ready for itch.io direct upload; icon and version metadata embedded. | Should | A,B | |
| FR-SHP-040 | Build: CMake 3.28+ presets only (Ninja dev, VS debug), vcpkg manifest, unity + PCH, GitHub Actions (windows-latest) with ASan per PR, clang-tidy, green-pipeline-before-milestone. | Must | * | Binding; platform-owned detail |
| FR-SHP-050 | Steam/itch API integration, achievements, cloud saves. | Won't | | §6 — a shipped small game may wrap manually later |
| FR-SHP-060 | macOS/Linux/console/web/mobile targets; 32-bit; Windows 7/8. | Won't | | §6 — SDL3 keeps portability *theoretical* at zero maintenance cost |

---

## 5. Quality requirements (product-level, measurable)

Architecture owns the mechanisms; these are the numbers the product commits to. They feed the milestone budget tables (binding).

| ID | Quality requirement | Target |
|---|---|---|
| QR-010 | Frame rate | Both anchor games hold 60 fps at 1080p on the dev machine in shipped builds; frame budget 16.6 ms with ≥ 2 ms headroom at ship. |
| QR-020 | Load time | Boot scene to interactive main menu < 5 s cold; scene-to-scene transition < 1 s (memory-mapped cook pays for this). |
| QR-030 | Determinism | FR-RUN-040 hash test green on every PR from M1; cook byte-determinism (FR-AST-030) on every milestone. |
| QR-040 | Editor iteration | Play-mode enter < 1 s; asset hot-reload < 2 s; full 2D-game cook < 60 s (incremental: < 5 s for a single-asset touch). |
| QR-050 | Stability | 2-hour authoring session in the editor with zero crashes; any crash produces a minidump + matching-PDB path, never a silent corrupt state. |
| QR-060 | Memory | Frame-time budget table **and** a memory budget table per milestone (Our Machinery practice); zero per-frame heap allocation in hot paths (binding), verified by ASan build + Tracy memory track. |
| QR-070 | Shipped overhead | Shipped game carries no profiler overhead (Tracy off), no editor code (CMake target wall), no debug consoles unless the FR-UI-045 dev console is enabled. |

---

## 6. Non-goals — the aggressive Won't list (with re-entry triggers)

A solo engine dies of scope. Every item below was considered and **rejected**, with the one condition that would reopen it. Reopening any of these requires a written slip note (binding milestone-gate discipline) or an ADR, never a silent slide.

| # | Won't | Why not | Re-entry trigger |
|---|---|---|---|
| N1 | Multiplayer/netcode of any kind | Doubles the hardest problem class; determinism (FR-RUN-040) is kept *only* because it is nearly free and pays off in testing/replay, not as netcode prep | A completed, shipped game + explicit design doc |
| N2 | Scripting language VM (Lua) at v1; visual scripting, blueprint graphs | Native C++20 systems are the committed gameplay model (ecs-gameplay); a VM before the ECS is proven is a second runtime to debug | After UC-B ships, if iteration data shows recompile-iterations dominate editor time |
| N3 | 3D physics written from scratch beyond the character controller | The binding names Jolt 5 the pre-approved exception; hand-rolling rigid-body 3D is a year of edge cases UC-B doesn't need | FR-PHY-090 Could — only if a third game concept demands knockable props |
| N4 | Terrain, foliage/vegetation, ray tracing, GI beyond probes, SSR, virtualized geometry, volumetrics | All optimize beyond the "small complete game" ceiling; GL 4.5 ceiling stated honestly in the stack decision | A committed game concept whose art direction requires it |
| N5 | Asset-creation DCC features (mesh editing, texture painting, rigging, animation authoring from scratch) | Blender exists; MCE's job is import-cook-inspect, not compete with Blender | Never planned; curve/timeline editor (FR-EDT-130) is the only authoring surface |
| N6 | Mobile/console/web/VR platforms, 32-bit, non-Windows desktops | Windows 10 x64 is the stated machine; porting is a force-multiplier on every subsystem | A shipped game with commercial demand on another platform |
| N7 | Networking, accounts, telemetry, achievements, cloud saves, DRM | Solo offline games; online services are a second product | Never planned |
| N8 | Engine-as-product: installer, plugin API, docs-for-third-parties, API stability guarantees, asset store | MCE ships *games*; the engine is the curriculum (P2). API stability discipline would tax the architecture for no anchor-UC benefit | Revisit only if a second developer joins |
| N9 | Sparse-entities-at-mega-scale (100k+ entities, GPU-driven everything) | UC targets are thousands; archetype ECS already gives the headroom that matters | Profiling shows a real game hitting 10k entities |
| N10 | Runtime mod support, user-generated content, in-game editors | Shipping untrusted content is a security + format-versioning project | Never planned |
| N11 | Cinematic tooling beyond FR-ENT-110 timelines (cutscene editor, camera choreography suites) | UC-B's beats fit the action-list/timeline scope | A game concept with FMV-scale narrative ambitions |
| N12 | Localization/i18n pipeline | Both anchor games are English-only | A committed commercial release in another language |
| N13 | Engine-level live code hot-reload | Handmade Hero DLL split is adopted as *architecture* (testability, runtime/editor seam), but hot reload itself is fragile on MSVC and not needed once content iteration is editor-first (P3) | After UC-B, if C++ iteration pain is measured, not felt |
| N14 | A second graphics backend (D3D12/Vulkan) on a schedule | The RHI seam (binding) keeps the exit door open; a second backend before two shipped games is architecture tourism | After both anchor games ship, as an explicit learning milestone |
| N15 | Gamepad-only-or-better accessibility systems (full remap UX, colorblind filters, screen-reader) beyond standard rebinding FR-INP-040 | Important, but beyond what two small games require; noted so it is a *decision*, not an oversight | If a shipped game is published commercially |

---

## 7. MoSCoW summary

| Priority | Count | Reading |
|---|---|---|
| Must | 48 | The contract for UC-A + UC-B + UC-C as specified. These map to M0–M3 gates. |
| Should | 28 | Expected of a polished small-game engine; cut only with a written slip note. Maps to M3–M5. |
| Could | 12 | Delight layer; explicitly first on the chopping block. |
| Won't | 30+ | §4 inline items + the 15 consolidated non-goals of §6. |

Every Must traces to at least one anchor use case; every anchor acceptance checklist (§3) is satisfiable from the Must set alone. That is the invariant this document is built to hold: **if the Musts are done, the two games and the editor loop ship — nothing more is required, and nothing in §6 may creep in to "help."**

---

## 8. Handoffs to the other lenses

- **Architecture lens:** FR-ENT's ECS decisions and FR-RUN's loop discipline are already fixed by research; the architecture doc should treat the FR tables here as the external behavior contract and design internals to it. Note the two deliberate "product demands capability, architecture picks mechanism" cases: FR-PHY-080 (hand-written sweeps vs Jolt ADR) and FR-UI-020 (mce::ui design doc at M4).
- **Plan lens:** the indicative milestone tags above are MoSCoW-derived, not schedule commitments; the binding M0–M5 gates in the stack decision are authoritative. The §3 acceptance checklists are offered as the measurable content of each gate.
- **Risks the product lens wants recorded:** (1) M1 is the heaviest milestone (ECS + editor + 2D stack simultaneously — ecs-gameplay §13.3 flags the same); (2) FR-UI-010 forces *some* runtime-UI primitives at M1 even though the full system is M4 — the plan must not defer menus past UC-A's gate; (3) the M2 timebox valve (cut IBL/bloom per binding) should be echoed in the plan as a pre-agreed cut list.

---

## 9. References (canonical, from knowledge)

- Jason Gregory, *Game Engine Architecture*, 3rd ed. — scope-setting for engine feature sets; audio ch. 13; object models ch. 14.
- Glenn Fiedler, "Fix Your Timestep," Gaffer On Games — FR-RUN-020/030/040.
- Erin Catto, Box2D GDC slide series — FR-PHY-010..070 solver and character-controller shape.
- Unity documentation: Input System (FR-INP-020), uGUI anchored layout (FR-UI-020), prefab/override model (FR-EDT-080), import cache (FR-AST-080).
- flecs v3, EnTT, Bevy ECS documentation — the ECS feature set FR-ENT commits to and, equally, what it rejects.
- Bitsquid/Our Machinery blog corpus — asset cook pipelines, tool philosophy, memory budgets (QR-060).
- *Game Programming Patterns* (Nystrom) — Event Queue, Update Method, Dirty Flag underpinnings of FR-ENT-030/040/050.
- Hazel (The Cherno) series — the closest public reference for the UC-C editor shape, and the cautionary tale behind several Won't entries.
- audio-input-ui.md and platform-engineering.md (this repo) — subsystem-level decisions this document consumes rather than repeats.
