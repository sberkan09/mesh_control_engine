# Mesh Control Engine — Roadmap

**Project:** Mesh Control Engine (MCE) · **Document class:** Roadmap / plan
**Date:** 2026-10-06 · **Status:** AUTHORITATIVE sequencing for the tiers defined in `REQUIREMENTS.md` §6. Dependency-ordered; **every milestone ends in a running, demonstrable build** (the invariant from REQUIREMENTS §7.4). Sizes are relative (S ≈ 2–4 wk, M ≈ 4–8 wk, L ≈ 8–14 wk, XL ≈ 14–22 wk at the 10–15 h/wk baseline; velocity re-checked per NFR-B-07 — **cut feature scope, never the gate, never the demo**). Every milestone also carries an **effort estimate in hours** (engineering + content production, published next to the size); the tier bands in §2's tier map are the *sums* of those estimates — if a milestone's actual hours exceed its estimate by > 50 %, the named overrun-absorber milestone cuts per the ladder (see the tier map note).

Milestone naming note: the FR tables in REQUIREMENTS carry indicative M0–M5 tags; this document is the sequencing authority and maps to *tiers* (MVP / v0.5 / v1.0), not to those tags.

---

## 1. Milestone DAG (dependency order)

```mermaid
flowchart LR
    M0["M0 skeleton+core<br/>editor shell, 01_triangle"] --> M1["M1 ECS<br/>02_boids, determinism"]
    M1 --> M2["M2 asset+cook pipeline<br/>mmap .mce, 03_platformer starts"]
    M2 --> M3["M3 2D gameplay stack<br/>physics2d+input+audio<br/>03_platformer playable"]
    M3 --> M35["M3.5 solver hardening<br/>+ Box2D oracle suite<br/>(stretch, slip absorber)"]
    M35 --> M4["M4 3D diorama + ship<br/>Artifact A+B, MVP GATE"]
    M4 --> EC["Editor catch-up<br/>M2–M4 editor items"]
    EC --> M45["M4.5 editor MCP server<br/>AI tooling over refl +<br/>headless runs (slip absorber)"]
    M45 --> M5a["M5a 3D rendering real<br/>PBR/Forward+/CSM +<br/>frame-budget tables"]
    M5a --> M5b["M5b Jolt + play-in-editor<br/>+ hot-reload matrix"]
    M4 --> M6["M6 depth+editor respect<br/>hierarchy, prefabs, undo,<br/>audio buses — full Ember Step<br/>v0.5 GATE (part 1)"]
    M5b --> M7["M7 Ashen Court build-out<br/>v0.5 GATE (part 2)"]
    M6 --> M7
    M7 --> M8["M8 mce_ui + editor maturity<br/>project template — v1.0 (part 1)"]
    M8 --> M9["M9 UC-D proof game<br/>v1.0 GATE (part 2)"]
    M35 -.slip whole into v0.5.-> M5b
    M4 -.cut rung 7.-> M5a
    M45 -.slip whole past.-> M5b
    M4 --> MMAC["M-MAC macOS port<br/>mce_rhi_metal + macOS platform<br/>(parallel track, built on the Mac)"]
    MMAC -.parity backlog.-> M5a
```

Sequencing rationale (binding): 2D gameplay precedes 3D rendering polish because Artifact A is the earliest shippable artifact and the cut ladder's final rung reduces the MVE to Artifact A alone — Artifact B is structured as an **addition, not a dependency** (REQUIREMENTS cut-ladder rung 7). The cook pipeline precedes everything that loads content; the determinism harness precedes everything that could break it.

---

## 2. Milestones

### M0 — Skeleton that runs and is observed · **size M (≈ 70–110 h engineering)** · tier: MVP

**Builds:** CMake presets + vcpkg pinned baseline + GitHub Actions (windows-latest, ASan smoke; **clang-tidy joins in M1**) · `mce_core` (allocators, containers, math + DirectXMath oracle **reduced to the ~10 functions the samples exercise** (mat mul, perspective/look-at, TRS, inverse, dot/cross, slerp — the full randomized sweep lands with M1's determinism build), `Result`/`Error`, asserts/logs/crash handler + minidump chain, convars, Tracy) · `mce_platform` (SDL3 window/pump/filesystem/mmap, SDL+GL grep walls) · `mce::app::run` fixed-timestep loop with headless mode · `mce_rhi` interface + `mce_rhi_gl` backend enough for clear + triangle · `mce_editor_layer` docking shell **(viewport + console panels over a stub world only — hierarchy/inspector arrive with reflection in M1/M2)** · sample `01_triangle` · `--crash-test` minidump path.

**Demo (running):** clone → 3 commands → editor opens `01_triangle` rendered in a docking viewport; Tracy shows the frame; the GL-leak, SDL-leak, and ImGui-wall greps are CI jobs; a `--crash-test` dump opens in VS with the archived PDB. Headless mode runs 10k ticks with zero GL symbols.

**Gates:** FR-DX-080 LRN-1 (RHI/DSA) written from memory · NFR-B-01 (clean clone ≤ 3 commands, ≤ 15 min) measured · unit tests: math oracle (reduced set), allocators, containers, Result.
**Budget table:** frame slot + arena behavior, editor fps.

### M1 — The ECS and the determinism harness · **size M (≈ 70–110 h engineering)** · tier: MVP

**Builds:** `mce_ecs` MVP checklist (archetype tables, generational entities, queries with with/without filters, phase pipeline + sync points, deferred `Commands`, double-buffered `EventQueue<T>`) · `World::state_hash` + headless determinism harness in CI (FR-RUN-040, green from this milestone on) · `World::metrics` · fixed-step + tick-quantized input skeleton (keyboard/mouse action snapshot — full action maps in M3) · **game.dll ABI spike (~10–20 h, finding F33): bake the cross-DLL template rules into core/ECS from day one — no function-local statics inside engine header templates (identity routes through exported non-template registry functions), one explicit `__declspec(dllexport)` template instantiation proven with a throwaway DLL** · clang-tidy joins CI · sample `02_boids` (100k entities). *Reflection (`mce_refl`, `MCE_COMPONENT`) and the inspector round-trip moved to M2* — M1's scope is ECS core + determinism only (slip note 2026-10-06; full editor CRUD/gizmos/viewport live in M3, while M2 carries the minimal spawn/select/delete + scene-save path its authoring demo needs).

**Demo (running):** `02_boids` simulating 100k entities < 1.5 ms/iteration (Tracy screenshot in milestone doc), driven headless *and* observed through `World::metrics` in the editor console; the CI determinism job hashes 3 consecutive runs + Debug-vs-Release, identically.
**Gates:** LRN-3 (archetype internals) · FR-ENT-010/020/030 acceptance tests · NFR-T-01 measured.
**Budget table:** ECS iteration, command-buffer application.

### M2 — Assets: cook it, map it, reload it · **size L (≈ 110–170 h engineering)** · tier: MVP

**Builds:** `.mce` container (header/TOC/CRC/version, fuzz + negative tests) · GUID registry + `.mceasset` sidecars · cook library + `mce_cook.exe` (scan, dep graph, topological cook, content-addressed outputs, CI exit codes) · importers: PNG → RGBA8, WAV → 48 kHz float32, TTF → font atlas, `.mcscene` → columnar scene chunk (reflection-driven) · `mce_refl` (field registry, `MCE_COMPONENT`) + inspector round-trip in the editor (moved here from M1) · **minimal entity spawn/select/delete in the editor (the authoring path the M2 demo needs — finding F54; full CRUD + gizmos in M3)** · mmap runtime load path · shader cook — **MVP slice: `glslangValidator -V` validation per stage at cook + the ≤ 1 s full program rebuild-and-swap (DX-H1, the load-bearing DX feature) + the binding-bank conformance checker (via glslangValidator's reflection dump, not a stored blob)**; *variant enumeration and the SPIR-V reflection blob are deferred to v0.5 by slip note (finding F9)* · scene save/load in the editor via `SceneDocument` + reflection serialization · job system learned cook-side (M2 per core R9).

**Demo (running):** author a scene in the editor (sprites + text), Save → Cook → load in `game.exe`; cook twice ⇒ byte-identical (CI); edit a fragment shader → pixels change ≤ 1 s without restart; a broken asset yields a file+line cook error in the editor panel while the old asset keeps rendering. `03_platformer` begins as static content.
**Gates:** LRN-7 part 1 (cook pipeline) · FR-AST-020/030/040/060/070 acceptance (FR-AST-060's variant enumeration re-tiered to v0.5 — REQUIREMENTS slip note) · LRN-4 part 1 (reflection end-to-end: inspector round-trip with zero per-type code, FR-EDT-020).
**Budget table:** full cook < 60 s, incremental < 5 s, mmap load < 5 ms.

### M3 — The 2D game stack · **size L (≈ 130–190 h engineering)** · tier: MVP

*Re-scoped 2026-10-06 (finding F2): M3 ships cut-ladder rung 6's shape by default. Solver hardening (warm starting, island traversal, restitution tuning, platform carry via velocity accumulation) and the 20-hand-computed-case suite move to an explicit M3.5 below; sub-frame sample-accurate audio scheduling moves to v0.5 — 60 Hz command granularity is sufficient for Artifact A. Mid-point review rule: if M3 is still tracking beyond 14 weeks at its half-way gate, golden-image plumbing moves to M2 by slip note (the cook pipeline already hosts glslangValidator and the content-addressed store).*

**Builds:** `mce_render` 2D path (extract → cull → 64-bit radix sort → one MDI per (pipeline/blend state × sprite-set) — ARCHITECTURE CF16/ADR-0006; sprite/animated-sprite/text; Camera2D pixel-snap; DebugDraw MVP) · golden-image plumbing (dev-machine, perceptual threshold, RenderDoc capture as merge gate) · `mce_physics2d` **rung-6 shape**: BVH broadphase, dynamic-vs-static solver, kinematic swept `move_and_collide` controller (sweeps, one-way platforms, ground flags), sensors and queries · `mce_input` action maps (cooked chunk, fixed-field bindings, record/replay; *rebinding persistence is FR-INP-040 v0.5 — M6*) · `mce_audio` v1 (RT callback, SPSC FIFO, 8 voices, streamed music ring, AudioSource bridge; **60 Hz command granularity — sample-accurate `play_at` scheduling deferred to v0.5**) · FSM helper `mce/fsm.h` + **`mce/gameplay.h` umbrella target** + game-module loader (`mce_game_module`, copy-and-load, run-in-player button) + **`SequenceRunner` MVP-lite subset {Wait, PlayAnim, PlaySound, Emit, End}** (scripting.md §8) + the ComponentId-identity CI test (finding F11) + the **game.dll ABI ledger (~15–30 h, finding F33): explicit `__declspec(dllexport)`/import instantiation pairs for the 12-type budget, enumerated and CI-checked** · `mce_gameplay_kit` MVP components · entity CRUD + copy/paste + translate gizmo · **editor grid/snap placement aids (pulled forward from M6 — finding F34; once the inspector exists these are days, not weeks)** · **minimal scene-state menu + pause overlay (A9 model — the MVP half of FR-RUN-030, finding F55)**.

**Demo (running):** sample `03_platformer` — player runs/jumps on AABB physics, rides a moving platform (velocity accumulation, no solver carry), collects a pickup (trigger event → SFX + particle-less flash), enemy FSM chases, music loops gaplessly, all authored in the editor, Play → the cooked `game.exe` child process runs it. Determinism CI covers physics + input + audio command hash.
**Gates:** LRN-6 part 1 (dynamic-vs-static solver + RT audio; LRN-6 closes at M3.5) · LRN-2 (fixed timestep + allocator — passed implicitly at M0, audited here) · FR-PHY-010/020/030/035/040 (sensors/triggers and queries close here — they are Must FRs in the committed M3 `World2D` shape, and Must FRs may not ride a slip-absorbing milestone; M3.5 carries solver hardening only), FR-AUD-010/020(8, 60 Hz granularity)/040, FR-INP-020/030, FR-R2D-010/020/030/040/060, FR-ENT-090.
**Budget table:** 10k sprites p99 < 4 ms GPU (FR-R2D-010), physics step, audio callback µs.

### M3.5 — Solver hardening + the physics case suite · **size S–M (≈ 30–70 h engineering)** · tier: MVP stretch (may slip into v0.5 without gating M4)

**Builds:** warm-started sequential impulse solver, island traversal, restitution tuning, platform carry via velocity accumulation promoted into the solver path, and the 20 hand-computed manifold unit cases (ROADMAP risk 3's early-warning suite) · **Box2D v3 test-time oracle suite (ADR-0016, CF13): identical deterministic drop/slide/restitution/pyramid-lite/platform-ride scenes stepped in `test_physics2d` and Box2D, compared within documented tolerances; runs locally + nightly CI (the only target allowed to link Box2D)** · sample `03_platformer` re-verified against the full suite + determinism CI.

**Demo (running):** `03_platformer` stacking two dynamic crates and riding a rotating-ish kinematic path without jitter; the 20-case suite green in CI.
**Gates:** FR-PHY-010/020 solver-hardening acceptance (sensors/queries already closed at M3 — physics §8) · LRN-6 closed (Catto solver written from memory).
**Overrun rule:** this is the MVP tier's designated shock absorber — if M3 or M4 overruns, M3.5 slips whole into v0.5. It carries **no Must FRs** (the 2026-10-06 re-tier closed FR-PHY-035/040 at M3 precisely so the slip absorber stays Must-free), so the slip triggers no FR re-tier; moving any Must FR onto a slip-absorbing milestone requires a REQUIREMENTS slip note first.

### M4 — The 3D gate + shipping · **size L (≈ 90–140 h engineering + ≈ 50–70 h content production)** · tier: MVP complete (MVE)

**Builds:** glTF 2.0 static import (meshoptimizer vertex-cache/fetch at cook, quantized streams) · forward 3D: diffuse+specular, 1 directional light + 1 shadow map, fog, node-transform animation · frustum culling + radix sort 3D path · `mce_engine.dll` + `game.dll` module packaging · one-command ship v1 (cook + package + zip, icon/version) · clean-VM soak test · **the `mce_ui` design doc (FR-UI-020 / ARCHITECTURE CF21: the design commitment of audio-input-ui §2.5 finalized as the M4 deliverable; the build itself is M8)** · editor gizmo polish, autosave, and the UC-C loop rehearsal.

**Content production (finding F1 — the MVP tier ships real content, not programmer art):** Artifact A's sprite set + tileset + SFX from **Kenney's CC0 2D packs** (platformer-art, particle/atlas packs) plus a freesound or JSFX SFX bundle (≈ 15–25 h: selection, attribution pass, cook integration) · Artifact B's diorama **kit-assembled from Kenney/CC0 3D kit meshes + at most a few Blender-adjusted primitives**, stated triangle budget ≥ 10k total (≈ 20–30 h: kitbashing, baking the orbit/fog scene, one spin animation). No hand-modelled art anywhere in the MVP gate.

**Demo (running, the MVP gate):** **Artifact A** (single-screen Ember Step, fully arted from CC0 packs) and **Artifact B** (lit 3D diorama ≥ 10k tris of kit-assembled CC0 meshes, orbit/zoom, shadow, fog, spinning prop) both zip-shipped, double-clicked on a clean Windows 10 x64 VM by a named non-programmer; Artifact B first frame < 1.0 s (NFR-M-02); 10-minute soaks clean; MVE capability checklist fully green; LRN gates 1–5 closed with docs written from memory.
**Cut-ladder checkpoint:** if the 2D game is not playable by month 12 of real time, rung 7 executes (3D diorama becomes the first v0.5 milestone) — pre-agreed, no re-negotiation.
**Budget table:** NFR-F-01 per-stage frame table; NFR-M-01/02; both artifacts' zips.

### M4.5 — Editor MCP server (AI-native tooling) · **size S (≈ 35–60 h engineering)** · tier: off-tier tooling (between the MVP gate and v0.5)

*Added 2026-10-08. Sequenced after the editor catch-up (the M2–M4 editor items — SceneDocument + EditorScene funnel, inspector, CRUD, cook panel, headless player — finished after the M4 gate) and before M5a. Design: `docs/architecture/editor-mcp.md`.*

**Builds:** `mce_mcp` — a headless Model Context Protocol server (JSON-RPC 2.0 over **stdio only**, no socket — N7 intact) linking `mce_editor_core` + `mce_engine`, never ImGui · `mce_mcp_core` (protocol, tool table, editor-side JSON codec — hand-written by default, ADR-0027 records the pick; the runtime stays JSON-free per FR-AST-010) · MVP tool table: discover (`refl_list_components`/`refl_describe_component` straight from `mce_refl`), read (scene, entities, components, GUID-registry assets), edit (entity create/delete/duplicate, component add/remove/set by refl property path — **every write goes through the `EditorScene` command funnel**, the same funnel as the inspector), persist (`scene_open`/`scene_save`), build (`cook_run`/`cook_errors`), verify (`run_headless`: N fixed ticks → `state_hash` + `World::metrics` + log tail), console (`console_list`/`console_exec`, cheats refused by default) · resources `mce://scene/current`, `mce://log`, `mce://refl/schema` · a checked-in `.mcp.json` so Claude Code can use it from a clone. **Stretch, cut in this order:** attach-to-running-editor (same tool table over a local pipe, edits visible live) first, then `render_frame` (offscreen PNG as MCP image content).

**Demo (running):** from a fresh clone, an MCP client connected to `mce_mcp --project samples/03_platformer` adds a pickup to the level through the tools, saves, cooks, calls `run_headless` for 300 ticks and gets a hash back; the same edit done through C++ `SceneCommand`s gives a byte-identical `.mcscene` and the same hash.
**Gates:** FR-EDT-160 acceptance · scripted MCP transcript test (initialize → tools/list → calls → shutdown, golden responses) · JSON codec fuzz target ASan-clean · walls: no ImGui and no socket symbols in `mce_mcp*`, no JSON codec linked into any runtime target · all of it inside `scripts/ci/local_ci.sh`.
**Overrun rule:** carries **no Must FRs**; like M3.5 it may slip whole (past M5b) without gating anything. In-game AI (LLM NPCs, runtime generation) is explicitly **not** this milestone — it is listed under M10+.

### M-MAC — macOS port: Metal backend + macOS platform · **size L (≈ 100–160 h engineering)** · tier: off-tier platform track (parallel, gates nothing)

*Added 2026-10-08 by user decision. Built by a Claude Code session on the user's Mac (Apple silicon), in parallel with the cloud threads that continue M4.5 → M5a → …; it does not block, and is not blocked by, any other milestone. Design, file ownership and the Mac-only verification list: `docs/architecture/macos-metal.md`. Implements the "second RHI backend" path pre-agreed in REQUIREMENTS §8 Q2 (Mac migration note).*

**Builds:** ADR-0028 (Metal over MoltenVK; GLSL → SPIR-V → MSL via glslang + SPIRV-Cross, at pipeline creation inside `mce_rhi_metal` until shaders are cooked assets, then in the cook) · **seam PRs first**, each small and merged before the backend work leans on it: (1) one backend factory in `mce_rhi` so hosts stop calling `rhi::gl::create_device` directly, plus the Metal wall in `check_walls.py`; (2) a Metal-capable `platform::Window` (`SDL_WINDOW_METAL`, GL attributes only on the GL path); (3) MSL in the shader cook + `ShaderSource` · `mce_rhi_metal` (`src/engine/rhi_metal`): buffers/textures/samplers/pipelines, recorded `CommandBuffer` → Metal encoders, 3 frame slots on shared-storage rings + completion-handler semaphore, indirect draws with `[[base_instance]]` · macOS platform bits (`.dylib` module loading, bundle paths) · `macos-debug` / `macos-release` / `macos-asan` presets · `scripts/ci/local_ci_macos.sh` · `.app` packaging for the player.

**File ownership (binding while M-MAC is open):** the Mac session alone edits `src/engine/rhi_metal/**`, `tests/rhi_metal/**`, `src/engine/platform/src/macos/**`, `cmake/macos/**`, the `macos-*` presets, `scripts/ci/local_ci_macos.sh`, ADR-0028 and `docs/milestones/M-MAC.md`. Cloud threads never touch those. The Mac session never edits `rhi_gl`, the renderer or other engine modules; shared seams (`mce_rhi` headers, `platform/window.*`, host `main.cpp` device creation, CMake roots, `check_walls.py`) change only in dedicated seam PRs that pass the Linux `local_ci.sh` (verified and merged by a cloud thread) and keep `mce_rhi_gl` green. **Parity rule:** new RHI features land on GL first; Metal catch-up is M-MAC's backlog, never a reason for a cloud milestone to wait.

**Demo (running, on the Mac):** `01_triangle`, `sprites_2d`, `03_platformer` and `04_diorama` run natively on Metal on the Mac with screenshots perceptually matching the GL goldens; the 3D diorama keeps its shadow, fog and spinning prop; the packaged `.app` launches from Finder and soaks 10 minutes clean.
**Gates (run on the Mac, cannot run in the cloud):** `local_ci_macos.sh` green — the three macOS presets build and pass `ctest`, walls (GL/SDL/Metal) clean, Metal API + shader validation layers report zero errors, determinism ladder Debug = Release on the Mac (own baselines; never compared with Windows/Linux hashes, ADR-0007) — and an Xcode GPU capture per backend feature (the Metal side of FR-DBG-020). Seam PRs additionally pass the Linux `local_ci.sh`.
**Overrun rule:** carries **no Must FRs**; it can pause at any point without affecting a tier gate. If the cook-time shader translation becomes a sink (> 40 h), fall back to hand-written MSL for the sample shaders and record a slip note.

### M5a — The 3D renderer becomes real · **size L (≈ 90–150 h engineering)** · tier: v0.5 (part 1a)

*Split 2026-10-06 (finding F28): the old M5 bundled four multi-week flagship projects (PBR/Forward+, CSM, the Jolt embed, play-in-editor) into one XL milestone. M5a is the rendering half, gated by its own demo; M5b carries the embed + the stateful editor machinery. The frame-budget tables land in M5a — a budget miss there must not strand the physics/UI half mid-milestone.*

**Builds:** Cook-Torrance GGX PBR + ORM materials · Forward+ CPU-clustered lighting (≥ 256 point lights, cluster build < 0.5 ms) · 3-cascade texel-snapped CSM · sky + post chain (ACES tonemap, bloom, FXAA) · glTF node-animation tracks (multi-node; single-node TRS shipped at M4) · **frame-budget tables for the 3D pipeline (NFR-F-01 per-stage shape, filled and reconciled here)** · sample `04_cubes`.

**Demo (running):** `04_cubes` renders a PBR material sphere row + 256 lights + cascaded shadows at 60 fps with a RenderDoc capture per feature and the filled per-stage budget table in the milestone doc.
**Gates:** FR-R3D-020/030/040/080 · LRN-2 close-out (frame allocator under GPU pressure).
**Cut-ladder coupling:** post FX cuts before cascades (R1); Forward+ degrades on timebox **or schedule** breach (rung 3 — see risk 4's second trigger, binding).
**Budget table:** shadow < 2 ms GPU, cluster < 0.5 ms CPU, post < 1.5 ms.

### M5b — Physics 3D + play-in-editor + hot reload · **size M–L (≈ 80–130 h engineering)** · tier: v0.5 (part 1b)

**Builds:** **ADR-0003 + `mce_physics3d`** (Jolt 5 embed, `World3D::move_character`, `camera_sweep`) · 3D camera-collision gate for the editor orbit camera · play-in-editor (embedded host, pause/step/slow-mo, ≤ 1 s enter, Play→Stop→Play hash) · hot-reload matrix (textures/materials/scenes ≤ 1 s, game.dll recompile ≤ 10 s to Play).

**Demo (running):** play-in-editor enters a scene in < 1 s, pauses, single-steps, and Play→Stop→Play reproduces the tick-0 hash; a capsule character walks `04_cubes`' floor with `camera_sweep` collision; shader + texture hot reload stopwatch recordings in the milestone doc.
**Gates:** ADR-0003 must predate the first `JPH::` include · FR-R3D-070, FR-EDT-060, FR-DX-040 (Should items), FR-PHY-080 scaffolding.
**Budget table:** play enter/exit ≤ 1 s (NFR-M-03), reload stopwatches.

### M6 — Depth, respect, and the full platformer · **size L (≈ 110–170 h engineering + ≈ 60–100 h content production)** · tier: v0.5 (part 2)

**Builds:** ECS v0.5: change detection (`is_added/is_changed`), `ChildOf` hierarchy with depth-ordered dirty propagation, prefab cook + instantiate (no runtime inheritance) · editor v0.5: undo/redo (reflection command stack + fuzz test), asset browser with drag-drop, rotate/scale gizmos, multi-select, **tilemap authoring panel + rectangle-fill over the baked tile chunk + re-bake loop (the FR-EDT-120 remainder, with FR-R2D-115; grid/snap already landed in M3 — finding F34)** · 2D extras: particles through the sprite batch, parallax layers, lit-2D sprites + 2D post · audio v0.5: bus graph + biquad EQ + limiter, 48 voices, 3D spatialization, **sample-accurate `play_at` scheduling (deferred from M3, finding F2)** · input: rebinding persistence (FR-INP-040, moved out of M3) · physics v0.5: joints (distance/revolute), sleeping, 2,000-body budget · gamepad action maps · SequenceRunner (v0.5) · save/persistence + replay (FR-RUN-065/070) · lifecycle polish (focus-loss pause, clean-quit save flush) · shader cook v0.5: variant enumeration + stored SPIR-V reflection blob (deferred from M2, finding F9).

**Content production (finding F1):** Ember Step's 5 levels authored from the M4 CC0 tileset/sprite kit plus **Kenney's CC0 pixel-platformer expansion packs** (≈ 20–30 h level assembly; **preceded by a timed authoring spike — block out one full level, record the hours, and re-estimate from that measurement; if the spike exceeds budget, cut to three levels rather than compressing tool time — finding F34**) · enemy/prop art and UI frames from the same CC0 kits (≈ 10–15 h) · music (one looping CC0/proc-generated track) + ~20 SFX from freesound/JSFX with the attribution pass (≈ 15–20 h). Level design hours are included in the engineering column's upper end.

**Demo (running, v0.5 gate part 1):** **full Ember Step ships** — 5 authored levels, coyote time/jump buffering/wall-slide/dash, crumbling + moving platforms, patrolling/chasing enemies, best-time persistence, particles + parallax + menu/pause/results, keyboard **and gamepad**, 60 fps documented — and a prefab edited in the editor re-cooks and updates all instances on reload.
**Gates:** FR-ENT-040/050/070, FR-R2D-050/070/090, FR-AUD-030/050/060, FR-PHY-050/060, FR-EDT-040/060/110/120 acceptance tests.
**Budget table:** 2,000-body broadphase < 3 ms; 2,000-particle burst; editor 60 fps @ 5k entities (NFR-B-06).

### M7 — Ashen Court build-out · **size L (≈ 110–170 h engineering + ≈ 80–120 h content production)** · tier: v0.5 complete

**Builds:** the Ashen Court level — **kit-assembled from CC0 3D kits (Kenney modular kits, Poly Haven CC0 assets) + Blender kitbashing passes, stated triangle budget ≈ 300k with a per-mesh budget table checked at cook (finding F1: the level is sourced, not hand-modelled)** — plus third-person character + camera with `camera_sweep` collision, three FSM enemy types, melee combat + health pickups (events, not callbacks), spatialized audio for combat, 4–6 point lights + sun, HUD via text sprites + scene states, editor perf dashboards. Skinning per Open Question 3: **default = node-transform/static player proxy**; since 2026-10-10 skinning exists as an opt-in engine feature (M-SKIN below), so a skinned protagonist is the game's choice, not a requirement.

**Content production (finding F1):** environment kitbashing + light authoring from CC0 kits (≈ 40–60 h) · character/enemy meshes from CC0 rig-compatible sources with Blender retouch, no custom rigging (≈ 20–30 h) · combat SFX + music pass from freesound/JSFX bundles (≈ 15–25 h). Any hand-modelled asset is a slip note, not a default.

**Demo (running, v0.5 gate part 2):** Ashen Court playable slice — combat loop works, camera never clips geometry, spatial audio attenuates correctly, 60 fps at 1080p on the dev machine with the budget table and ≥ 2 ms headroom; every render feature in it has a RenderDoc capture + golden image; the level's triangle budget is documented in the milestone doc and verified at cook.
**Gates:** FR-PHY-080 (the A10 capability), FR-R3D budgets, UC-B v0.5 acceptance.
**Budget table:** 1M-tri p99 < 12 ms proxy scene; sim 3.0 / cull+sort 1.5 / translate 1.5 / submit 2.0 / GPU 6.0 / headroom 2.6 ms (NFR-F-02 shape).

### M8 — Ship shape: runtime UI + editor maturity · **size M–L (≈ 100–170 h engineering)** · tier: v1.0 (part 1)

**Builds:** **project template + one-command new-project generator (finding F17: directory scaffold, `<project>.mceproj` + `assets/` + `module/` + `host/` CMake skeleton per ARCHITECTURE CF17/assets.md §4.4, working empty game module — `new_project.bat <name>` produces a compiling, cooking, shipping project)** · `mce_ui` prototype → real (retained anchored-rect tree, focus navigation, reflection data binding, debug console on convars; **held to the pre-approved RmlUi fallback trigger from audio-input-ui R6 — one slip converts this build-out to an ADR'd RmlUi dependency rather than a slipped milestone**) · packaging v2 (symbolicated crash dumps for shipped builds, shipped debug console, version metadata) · editor maturity: prefab override UI, asset thumbnails, timeline/curve editor (FR-ENT-110), multi-scene tabs — **with the explicit cut order: timeline/curve editor first out the door, then asset thumbnails (cut-ladder rung 1, binding); IBL remains first on the v1.0 cut list if anything slips** · docs pass: `docs/engine/<subsystem>.md` written from memory for every subsystem, ADR index current.

**Demo (running):** a menu/pause/options/HUD panel set rendering through `mce::ui` inside a shipped build with **zero ImGui symbols** (CI scan); running the generator produces a second project that cooks and ships through the one-command pipeline; thumbnails and prefab overrides round-trip.
**Gates:** FR-UI-010/020/030/045, FR-EDT-080/130, FR-R3D-100 acceptance.
**Size note:** cut ladder rung 1 targets exactly this milestone's niceties list — timeline editor and thumbnails are its rungs 1a/1b.

### M9 — UC-D: the proof game · **size M (≈ 80–140 h engineering + ≈ 30–50 h content production)** · tier: v1.0 (part 2)

**Builds:** UC-D — **a vertical slice, not a second full game (finding F7): one small level reusing Ember Step's kit components plus one new mechanic, authored exclusively through the project template and editor workflows, shipped through the one-command pipeline. Gate precondition: the M8 project template is the path UC-D itself uses — no hand-rolled project scaffold.** Engine-source changes are capped at ≤ 2 gameplay systems, and gameplay-system code must not require engine-source edits beyond that budget (REQUIREMENTS §2 UC-D, §7.1; the v1.0 gate wording there is binding). Sample `05_showcase` grows alongside as the API-stability specimen (FR-DX-070: compiled headless in CI forever — the de-facto compatibility contract).

**Content production (finding F1):** UC-D's level from the Ember Step/Ashen Court CC0 kits with one new mechanic's assets (≈ 30–50 h).

**Demo (running, the v1.0 gate):** UC-D passes the non-programmer double-click test; UC-C is formally measured — a new UC-A level from empty scene to cooked launchable content in < 30 minutes of editor-only actions, 0 crashes in a 2-hour session; all seven learning gates closed; budgets reconciled within 20% of the Tracy memory track.
**Fallback:** if the v1.0 tier still does not close at M9's mid-point review, UC-D moves to a named **v1.1** milestone and v1.0 closes at M8's gate — pre-agreed, no re-negotiation.

### M-MOBILE — iOS/Android port (optional, parked) · **size L–XL (rough guess, not estimated)** · tier: off-tier platform track, **not scheduled**

*Added 2026-10-10 as an optional note by user request. **Parked:** no thread starts it, and milestone sequencing skips it, until the user explicitly un-parks it. Mobile is still Won't in REQUIREMENTS (FR-SHP-060, FR-INP-060); un-parking it means a REQUIREMENTS slip note first.*

**Already portable:** window, events, audio and module loading go through SDL3, which supports iOS and Android · the game module can already be statically linked (`MCE_PROJECT_STATIC_MODULE`, `PlayerDesc::static_module`), which iOS requires · the Metal backend (M-MAC) is most of the iOS GPU work · ECS, physics, scene, UI and the cook are platform-neutral C++. The editor stays desktop-only; only the player is ported.

**Would need:** (1) an Android RHI backend (Vulkan or GLES 3.x): `mce_rhi_gl` targets desktop GL 4.5 (DSA, persistent mapping, MDI), none of which exists on Android, so this is the biggest item · (2) `mce_rhi_metal` on iOS (UIKit / `CAMetalLayer`, tile-GPU storage modes) · (3) touch input: touch/gesture events in `mce_input`, a virtual stick, touch hit-testing in `mce_ui` · (4) app lifecycle: background/foreground pause, GPU surface loss, orientation, safe areas · (5) packaging: APK/AAB (Gradle + NDK) and IPA (Xcode, signing), mobile CMake presets and toolchains · (6) mobile frame/thermal/battery budgets: the NFR tables assume desktop GPUs, so shadows and post FX need mobile tiers.

**Rough shape if un-parked:** iOS after M-MAC closes (≈ M-size, leaning on the Metal backend) · Android as its own L-size track (new RHI backend). Both inherit the M-MAC rules: GL stays the reference backend, determinism baselines are per-platform (ADR-0007).

### M-SKIN — Optional skinned animation · **size M (≈ 30–50 h engineering)** · tier: v0.5 Should (FR-R3D-060), off the milestone sequence

*Added 2026-10-10 by user request (REQUIREMENTS §8 Open Question 3 update): skinning is supported but never required. A character may be a static mesh, a node-transform hierarchy or a skinned mesh; the developer chooses per asset (sidecar `"skinning"`) and per instance (`MeshInstance::skinning`). Not a promotion to Must: Ashen Court and the cut ladder are unchanged.*

**Built:** glTF skin import (joints, inverse bind matrices, `JOINTS_0`/`WEIGHTS_0` quantized to 4 × unorm8, ≤ 256 joints per skin, per-joint bind bounds) into an optional skin section of `CHNK_MESH` (chunk minor 1; static meshes keep the minor-0 layout) · `skin_palette` / `skin_bounds` over the existing node poses (joints are nodes, so `pose_nodes`, clip blending and `AnimationPlayer` cross-fades drive skeletons unchanged) · `Scene3D` per-instance joint palettes with posed culling and shadow bounds · GPU skinning in the forward and shadow passes (`SkinnedMeshVertex` stream, joint palette SSBO at binding 7) · `ClipEvent` playback markers (`AnimationPlayer::events`) for footsteps.
**Gates:** FR-R3D-060 acceptance: cross-fade between two clips with no pop (the existing `AnimationPlayer` test, now covering skeletons); playback events fire once per pass across loops; skinned draws pass the GL validation test with zero errors. Cook, headless and GL tests in `tests/asset/test_cook_gltf.cpp`, `tests/render/test_render3d_skin.cpp`, `tests/render_gl/test_render3d_gl.cpp`.

### M10+ — Explicitly deferred (named so they cannot sneak in)

Scene streaming (FR-ENT-120, trigger: a game outgrows one chunk), auto-parallel scheduling (**experiment run 2026-10-10 and not merged**: deterministic under FR-RUN-040 but no win for any MCE game, whose fixed-step sims take microseconds per tick in `after`-chained phases; findings and re-entry trigger in `docs/research/auto-parallel-scheduling.md`), engine-level live DLL reload (N13, measured trigger), Vulkan/D3D12 backend (N14, after both anchor games ship; the Metal backend is the sanctioned exception, M-MAC), mod support, networking, localization, **in-game AI (LLM-driven NPCs/dialogue, runtime content generation — latency, per-call cost and sim non-determinism vs FR-RUN-040; would also need the network stack N7 rules out)** — each with its recorded re-entry trigger from REQUIREMENTS §5/§6.3.

**Milestone → tier map (effort-summed, finding F3):**

| Tier | Milestones | Calendar | Effort (sum of per-milestone estimates) |
|---|---|---|---|
| MVP | M0–M4 (+ M3.5 stretch) | months 0–14 | ≈ 500–790 h engineering (M0 70–110 + M1 70–110 + M2 110–170 + M3 130–190 + M3.5 30–70 + M4 90–140) + ≈ 50–70 h content |
| Tooling (off-tier) | M4.5 editor MCP server | after the editor catch-up, before M5a | ≈ 35–60 h engineering (not summed into the tier bands; slips whole past M5b if needed) |
| Platform (off-tier) | M-MAC macOS port | parallel, from now; built on the user's Mac | ≈ 100–160 h engineering (not summed into the tier bands; gates nothing) |
| Platform (optional, parked) | M-MOBILE iOS/Android port | not scheduled; only if the user un-parks it | not estimated (not summed into the tier bands; gates nothing) |
| v0.5 | M5a + M5b + M6 + M7 | months 14–24 | ≈ 390–620 h engineering + ≈ 140–220 h content |
| v1.0 | M8–M9 (UC-D may slip to named v1.1) | months 24–36 | ≈ 180–310 h engineering + ≈ 30–50 h content |

Estimated total ≈ 1,070–1,720 h engineering + ≈ 220–340 h content production (the sum of the tier bands above; the earlier ≈ 1,200–1,500 h single-number band omitted content entirely and under-counted M3; REQUIREMENTS §6.2's tier bands are updated to match). **Standing overhead (finding F39):** every estimate above additionally carries ~10–15 % recurring non-feature work — PR CI maintenance, RenderDoc captures per feature, golden-image rebasing, per-milestone demo docs, LRN-gate writeups, ADRs, budget tables, slip notes, toolset churn. It is not separately ledgered per milestone; when a milestone exceeds budget **+ overhead**, the pre-committed cuts are: slip notes shrink to bullets, ADRs cap at one page, golden images stay dev-machine-only. **Overrun absorber before the month-12 go/no-go:** a 50 % overrun on any pre-M4 milestone is absorbed by M3.5 slipping whole into v0.5 and by M4 cutting content scope per the ladder — the MVP *gate* never moves; a 50 % overrun on M4 itself triggers rung 7 exactly as written above. The v1.0 calendar endpoint is decided by Open Question 1 (honest weekly hours).

---

## 3. Cross-milestone invariants (checked at every gate)

1. **A running demo exists** — every milestone above names it; a milestone without a demo did not close.
2. **Green CI**: include-grep walls (GL/SDL/ImGui/JPH), symbol scans, determinism hash (from M1), cook-determinism (from M2), ASan smoke, sample ladder compiles + cooks + runs 300 headless steps (from M1).
3. **Budget tables** filled: frame (per-stage), memory (per-subsystem, ±20% vs Tracy), velocity (devlog hours ±2 h).
4. **RenderDoc capture per new render feature** merged before the feature is (FR-DBG-020).
5. **Learning gate closed**: the milestone's LRN doc written from memory + self-exam recorded; a gate that cannot close in its timebox cuts feature scope, never the gate (FR-DX-080).
6. **No silent scope change**: any slip enters the milestone doc as a slip note; cuts execute the pre-agreed ladder without re-negotiation.

---

## 4. Risk register (top 10)

| # | Risk | Likelihood | Impact | Mitigation | Early-warning signal |
|---|---|---|---|---|---|
| 1 | **Solo-velocity collapse / scope death** — the Hazel stall pattern: subsystem work outruns shipped artifacts, motivation decays | High | Critical | P5 machinery: MVE checklist, cut ladder rungs 1–7 pre-agreed, formal one-tier downscope after two consecutive <6 h/wk windows (NFR-B-07), devlog hours from day one, every milestone demo-sized | Rolling 4-week hours < 6 h/wk twice; any milestone estimate off by > 50% |
| 2 | **glTF importer scope creep** — extensions, skins, rigs, edge-case files (assets.md ranks this #1) | Medium | High | Importer locked to FR-R3D-010 (static meshes, PBR params, node transforms); unknown extensions = cook warnings, never failures; importer work tracked as its own line in every milestone doc | Importer consuming > 25% of a milestone's hours; > 10 unresolved extension warnings |
| 3 | **2D solver instability** — stacking/joints/tunneling eat weeks | Medium | High | 20 hand-computed manifold unit cases in M3.5 (whole milestone is the designated slip absorber — it can move to v0.5 without gating M4); **Box2D v3 test-time oracle diffs (ADR-0016, nightly from M3/M3.5)**; stacking out of MVP scope (R5); cut rung 6 = dynamic-vs-static only, kinematic player — now M3's default shape, not a degradation | Solver bug weeks > 3; any tunneling repro at Artifact A speeds; oracle-diff tolerance drift beyond documented bands |
| 4 | **3D frame-budget miss** (1M tri < 12 ms, Forward+ < 0.5 ms, shadows < 2 ms) **and 3D schedule overrun** | Medium | High | Per-milestone budget tables with Tracy evidence; cut rung 3 ladder: drop post FX → Forward+ → cascades → ship lit-diffuse. **Second, schedule-based trigger (finding F28): if M5a exceeds ≈ 200 h, demote Forward+ to a ≤ 64-light clustered-forward/C-buffer or light pre-pass (which fully serves the 300k-tri arena target) and sketch true clustered lighting into v1.0 — the demotion fires on schedule, not only on frame-budget miss** | M5a demo p99 > 13 ms at 1080p; cluster build > 0.5 ms in Tracy; M5a hours > 200 |
| 5 | **Play-in-editor / hot-reload instability** — the most stateful code in the engine (partial commits, leaks) | Medium-High | Medium | Single `load_scene` path; all-or-nothing commits; generational handles; reload hammer test (50 assets/5 s under ASan) at v0.5; entity-count + voice-leak asserts on world teardown | Any leak in the hammer test; Play→Stop→Play hash mismatch |
| 6 | **Determinism flakes** — `/fp` leakage, hash-order dependence, container iteration order leaks into sim | Medium | High | **`/fp:precise` in every configuration — `/fp:fast` is banned engine-wide (finding F29), so no per-file pragma discipline is needed and the "determinism preset" is just a named alias of the standard release preset**; `FlatMap` for sim-visible iteration; determinism CI from M1 (M1 is chosen for it); Debug-vs-Release + 3-run identity required | First cross-config hash mismatch; any flaky determinism job |
| 7 | **Jolt embed friction** (units, step semantics, memory hooks, determinism of poses) | Medium | Medium | ADR-0003 written before the first include; JPH confined to `mce_physics3d` by CI grep; `World3D` facade keeps the surface to move_character/queries/sweeps; 3D items stay sketch-tier until M5b | `World3D` facade redesign count > 2; JPH version bump required mid-milestone |
| 8 | **Editor iteration budget overrun** (undo, browser, gizmos are deep water) | Medium | Medium | editor_core headless + CI-tested; cut rung 2 removes undo/browser/rotate-scale without touching the shell; A2/A4 keep MVP editor minimal | Editor work > 125% of its milestone budget (rung 2 trigger, binding) |
| 9 | **Shader/hot-reload GL instability** — program swap, uniform caches, driver quirks | Low-Medium | Medium | Old-program-keeps-rendering fallback by construction; binding-bank convention validated at cook; RenderDoc capture as merge gate; stopwatch log (DX-H1) records every reload | Any crash or > 1 s reload in the DX-H1 log; golden-image drift after reload |
| 10 | **Toolchain/CI drag** — SDL3 vcpkg port breakage, 15-min builds, ASan cost, unity-build collisions | Medium | Medium | Pinned ports + vendored-SDL fallback ADR (core R1); unity + PCH presets; ASan smoke per PR with weekly full job (R8 cut-line); unity batch-size escape hatch | PR CI > 30 min sustained; any unity collision needing > 1 h untangle |

---

## 5. What would change this plan

Recorded triggers, not surprises: Open Question 1 (weekly hours) rescales the calendar, not the scope discipline; Open Question 3 (skinned protagonist) is settled as opt-in skinning (M-SKIN; FR-R3D-060 built, still Should, nothing displaced); a committed commercial release promotes FR-SHP-030 and re-opens N15. Any other change enters by written slip note only — the roadmap bends at gates, never silently between them.
