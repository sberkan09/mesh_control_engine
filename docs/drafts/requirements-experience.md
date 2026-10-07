# MCE Requirements — Developer Experience & Learning

**Project:** Mesh Control Engine (MCE) · **Lens:** developer experience (DX) and pedagogy
**Date:** 2026-10-06 · **Status:** DRAFT (design-only; no implementation yet)
**Binding context this doc must obey (from the technology decision):** C++20 / MSVC v143, OpenGL 4.5 Core behind a thin RHI, SDL3, Dear ImGui (docking), hand-written archetype ECS / math / 2D physics / mixer, glTF → cooked `.mce` assets, fixed 60 Hz sim with interpolated rendering, editor owns the canonical scene, runtime never parses JSON/PNG, no exceptions/RTTI in the runtime core, ImGui is editor/debug-only.

---

## 0. Purpose and the two hats

MCE has exactly one user, who wears two hats:

1. **The game-programmer hat** — spawns entities, writes systems, draws sprites, loads scenes, iterates in the editor. This hat wants *velocity*: the shortest possible distance between an idea and seeing it move.
2. **The engine-builder hat** — writes the renderer, ECS, physics, mixer, cook pipeline. This hat wants *understanding*: every subsystem must be learnable, explainable, and re-derivable.

Every requirement below is written so that it can be **verified** (by a test, a CI check, a stopwatch, or a written self-exam). "Ergonomics stated as testable statements" is the contract of this document: if a requirement cannot be verified, it does not belong here. Where a requirement conflicts with a binding constraint, the binding wins and the conflict is recorded in §11.

The design stance throughout: MCE's only user is a C++ developer (the author). There is no designer persona, no scripting-language persona, no team persona. Ergonomics therefore means **C++ API ergonomics + editor ergonomics**, not "no-code authoring." Visual scripting, C# interop, and hot-reloadable scripting VMs are non-goals in v1 (§10) — this is a deliberate rejection of the Unity/Godot broad-audience model in favor of the Hazel/Handmade Hero shape (docs/research/existing-engines.md).

---

## 1. The two loops that define the product

A game engine *is* its iteration loops. Two loops must be engineered explicitly, each with a measured budget:

### 1.1 The code loop (edit → compile → run)

Budget table (measured on the dev machine, Windows 10 x64, MSVC v143, Ninja):

| REQ | Requirement | Budget | Verification |
|---|---|---|---|
| **DX-C1** | Clean-clone to running editor: `git clone` → CMake preset configure → build → editor opens with sample project loaded. | ≤ 3 commands total (presets make this `cmake --preset msvc-debug && cmake --build --preset msvc-debug && run`), clean build ≤ 15 min | Stopwatch test at M0; documented in README; re-run at every milestone |
| **DX-C2** | Incremental build after touching a **gameplay module** (systems/components only). | ≤ 10 s to a running editor | Stopwatch in CI log at M1 |
| **DX-C3** | Incremental build after touching a **core engine header** (e.g. `math.h`, `ecs.h`). | ≤ 90 s to a running editor (PCH + unity builds absorb it) | Stopwatch in CI log at M1 |
| **DX-C4** | One-keystroke run: F5 in Visual Studio (VS-generator preset for debugging) launches the editor with the last-opened project and scene. No arguments, no environment setup. | Zero manual steps | Manual gate at M0; CMake presets `msvc-debug-editor`, `msvc-debug-devhost` |
| **DX-C5** | Cook loop is incremental: changing one `.gltf` re-cooks only assets whose source hash changed; a full cook of the sample project | ≤ 5 s incremental, ≤ 60 s full at M2 sample-project scale | Cook tool reports per-asset timings; CI assertion on the sample project |

Rationale: the stack decision names velocity as the top-weighted criterion; DX-C1..C5 are how velocity is *measured* rather than asserted. The Bitsquid/Our Machinery posts and *Handmade Hero* both treat build latency as a first-class engineering target ("a 3-minute build steals a whole evening per week" — docs/research/platform-engineering.md §1.2). Unity's greatest DX asset is not the editor, it is that pressing Play is instant; MCE's equivalent is DX-C2/C4.

### 1.2 The content loop (edit → see it, in the editor)

| REQ | Requirement | Budget | Verification |
|---|---|---|---|
| **DX-E1** | **Play mode** (editor): press Play → the editor loads the cooked scene into a fresh runtime world and simulates. Press Stop → world is discarded, editor scene state unchanged. Enter/exit is idempotent: Play→Stop→Play twice produces identical first-tick state (guaranteed by the determinism test's harness, reused). | Play entry ≤ 1 s at M1 sample-project scale | Determinism harness drives enter/exit cycles; editor logs the play-mode timing |
| **DX-E2** | Editor frame rate with the M1 sample project (≈ 5k entities, all systems live) | ≥ 60 fps while idle and while inspecting | Tracy capture attached to the M1 milestone doc |
| **DX-E3** | Property edits in the inspector are visible in the viewport **the same frame** (editor mutates the live preview world; the canonical scene document is written on save — editor-owns-scene binding). | Same-frame | Manual gate; covered by the "inspector highlights changed components" feature (change ticks) |
| **DX-E4** | Transform gizmo (translate/rotate/scale, ImGuizmo pending ADR) round-trips: drag in viewport → `LocalTransform` updated → `WorldTransform` re-propagated → child entities follow, all visible immediately. | Same-frame | Manual gate at M2 (3D), M1 (2D drag-move) |
| **DX-E5** | Undo/redo works for inspector edits, gizmo drags, entity create/delete — implemented as reflection-driven commands on an undo stack, never as snapshots. | Any editor session action sequence is undoable | Unit test: perform command sequence, undo all, byte-compare scene document against before |

Play-mode design note: MCE adopts **Unity's domain-reload-on-Play discipline** (rebuild the world from the scene document at each Play) rather than Godot's "the editor *is* the game" live-world model. Rationale: it makes leak-across-plays bugs impossible, it makes determinism testing trivial (same harness as DX-E1), and it sidesteps the editor/runtime aliasing hazards that the existing-engines research flagged in Hazel's ad-hoc shared mutable scene state. Cost: play-mode entry pays a scene load; the 1 s budget of DX-E1 keeps that honest.

---

## 2. Gameplay authoring ergonomics (the API the user actually touches)

The gameplay API surface is a **budget, not an accident**. Requirement:

| REQ | Requirement | Verification |
|---|---|---|
| **DX-A1** | Making a 2D game requires knowing **at most 12 named types** from `mce::`: `World`, `Entity`, `Frame`, `Commands`, `EventQueue<T>`, `Sprite`, `LocalTransform`, `InputState`, `AudioHandle`, `Material2D`, `Camera2D`, plus the `MCE_COMPONENT`/`MCE_SYSTEM` macros. The census is enforced by review at M1; anything else is engine-internal and lives behind `mce::detail::`. | API-surface census doc; grep for `mce::` usage in the sample projects |
| **DX-A2** | Components are **reflected plain structs** — no registration ceremony beyond one line, no virtual functions, no base class. Inspection, serialization, and cook all come from the reflection traits (binding: reflection-driven, no ad-hoc field lists). | Adding a new component to a sample project requires **zero** editor-code and **zero** serializer changes; the inspector shows it; CI compiles the sample |
| **DX-A3** | Systems are plain functions registered into named phases; intra-phase order is explicit and visible. A user can answer "what runs before my system?" by reading one pipeline listing — in code and in the editor's pipeline debug view. | The editor's pipeline view renders the phase/order graph at runtime; test that declared order == execution order for a sample project |

The sketches below are the *quality bar* for authoring ergonomics — if the real API needs more ceremony than these sketches, the requirement is failed. They are consistent with docs/research/ecs-gameplay.md §13.2, restated here from the user's seat.

### 2.1 Spawning an entity

```cpp
// In a system or tool code; structural change always goes through Commands
// (deferred to the phase sync point — you can never invalidate your own query).
void spawn_coin(Commands& cmd, const AssetRef& gfx, Vec2 pos) {
    Entity e = cmd.spawn("coin")            // name shows in the editor hierarchy
        .set(LocalTransform{ .pos = {pos, 0.0f} })
        .set(Sprite{ .material = gfx.load<Material2D>("art/coin"),
                     .size = {0.6f, 0.6f} })   // meters (binding: meters, Y-up)
        .add<CoinTag>()
        .id();                                 // generational handle, 8 bytes
    cmd.set<AudioSource>(e, { .clip = "sfx/coin.wav", .on_spawn = true });
}
```

Ergonomic properties required: designated initializers (binding) make every call site read like a config file; `.load<T>("path")` returns a cook-time-validated asset reference — a typo is a **cook error with the file and line named**, not a runtime surprise (DX-D2 below); no raw pointers appear anywhere in the call.

### 2.2 Authoring a behavior (system)

The full sketch lives in docs/research/ecs-gameplay.md §13.2 (`EnemyThink`/`EnemyMove`). The DX requirements extracted from it:

- **DX-A4** — A new behavior is **one file**: components at top, one or two `MCE_SYSTEM` functions, one registration line in the module's `register_*`. The file compiles with no includes beyond `mce/mce.hpp`. Verification: the sample projects follow the one-file-per-feature rule; count of includes in gameplay files ≤ 3.
- **DX-A5** — Reading a system tells you its schedule: the `MCE_SYSTEM(Name, Phase, after(...))` declaration *is* the ordering documentation. No hidden scheduler inference (Bevy-style auto-parallelism is an off-by-default M3+ experiment per the ECS research).
- **DX-A6** — Gameplay-visible failure is data: a failed action pushes an event or a component (`FailedToPath{}`), never an exception and never a silent error code. Verify: no `throw` compiles in gameplay code (exceptions off — binding), `Result<T,E>` appears only at asset/IO boundaries.

### 2.3 Loading a scene (the two consumers)

```cpp
// ---- Editor (canonical scene document, text): owns and mutates.
SceneDocument doc = SceneDocument::load_or_create("scenes/level01.mcscene");
//   * editor format is text, readable, diffable, mergeable
//   * entity refs are stable GUID paths; prefab instances are (template + overrides)

// ---- Runtime / Play mode (cooked chunk): read-only, no parsing.
mce::Result<mce::World> w = mce::load_scene("cooked/level01.mce");
if (!w) log_error("level01 failed: {} at {}", w.error().code, w.error().context);
//   * mmap + bulk row attach; sub-millisecond at sample-project scale
//   * the runtime never sees text — cook owns the translation (binding)
```

- **DX-A7** — Scene load in the editor is **text and human-diffable** (the editor document); runtime load is **binary `.mce`** (the cooked chunk). A user can read a scene file in a text editor and understand it. Verify: the M1 sample scene is reviewable as prose; diffing two versions of a scene in git shows a meaningful change, not noise (stable ordering, stable GUIDs).
- **DX-A8** — Every load path returns `Result<T,Error>` with a code, a human string, and the asset path; a corrupt `.mce` file produces a diagnosable error, never a crash (matches the platform-engineering negative-test suite).

### 2.4 Drawing a 2D sprite (the "hello world" of the renderer)

```cpp
// User-facing: sprites are components; drawing is automatic.
// A Sprite component in the world is batched by the renderer — there is no
// per-entity draw call in user code, and no render-list to populate by hand.

// The explicit path, for debug/overlay drawing from a Render-phase system:
void draw_cursor(DebugDraw& dd, const InputState& in) {
    dd.sprite(in.mouse_world(), "art/cursor", /*layer*/ 10);
    dd.line(a, b, color_red);          // lines/circles for game feel + debugging
}
```

- **DX-A9** — The 2D path's default is **component-driven batching** (sprites are data; the renderer batches them via MultiDrawElementsIndirect — binding GL discipline). The imperative `DebugDraw` path exists for overlays and game-feel work. A user never chooses batch sizes, sorts, or vertex formats. Verify: a sample project renders 5k sprites with zero draw-related user code and one MDI submit per layer (Tracy + RenderDoc capture as evidence).
- **DX-A10** — Z/layer semantics for 2D are one documented rule (integer layer + sortable offset), not a rendering-nerd discussion. The rule is stated in the sample project's README and enforced by a validation warning at cook time when a material breaks it.

---

## 3. Editor experience — what a work session feels like

The editor is the product's face. Its layout follows the Unity/Godot muscle-memory layout because the single user already has it (Dear ImGui docking branch per the stack decision):

- **Hierarchy** (scene tree, entity search) · **Viewport** (2D/3D, gizmos, debug draw overlays) · **Inspector** (reflection-driven, field traits render range sliders/tooltips per the O3DE inspector-first lesson in docs/research/existing-engines.md) · **Asset browser** (cooked-source split visible: source file → cooked artifact, with re-import status) · **Console** (spdlog sink; errors clickable to source) · **Pipeline view** (phases/systems, live event-flow counts — from the ECS research's debug-UI plan) · **Tracy and RenderDoc launch from buttons**, not terminals.

| REQ | Requirement | Verification |
|---|---|---|
| **DX-ED1** | The editor shell (docking layout, console, hierarchy, inspector, asset browser) exists at **M0**, before the renderer is finished — the editor is a client of the runtime API from day one (Godot's self-hosting stance; the Hazel layer-stack shape). | M0 milestone artifact: an editor window with a dockable layout that spawns/selects entities and shows components in an inspector, even if the viewport draws a placeholder |
| **DX-ED2** | Inspector coverage is total: **every** component in any loaded scene is inspectable with zero per-type editor code (reflection traits only). A component with no traits renders as a raw field dump, still editable. | Test: register a component with three fields and no editor code; assert inspector renders and round-trips edits through the undo stack |
| **DX-ED3** | All destructive editor actions flow through the undo command stack (reflection-driven property commands — per the audio/input/UI research decision). No editor action can mutate the scene document outside it. | Code-review rule + test: perform N random inspector/gizmo operations; undo to start; scene document byte-identical |
| **DX-ED4** | The editor *launches* the shipped-game target: a "Run in player" button launches `mce_runtime` with the cooked project — the same artifact a non-programmer milestone gate requires. | M1 gate: the runnable artifact is produced by a button, not by a developer's command line |
| **DX-ED5** | Gameplay code never appears in the editor and editor code never appears in gameplay: enforced by the CMake target wall (binding; `mce_runtime` cannot see ImGui) and CI include-grep. The editor's "gameplay hooks" are the same public API the shipped game uses — there is exactly one way to talk to the engine. | CI symbol scan (ImGui symbols absent from `mce_runtime`); the sample games compile and run through `mce_runtime` without the editor target |

Editor anti-goal (v1): **no** scene-diffing merge tools, no multi-scene simultaneous editing, no asset thumbnails beyond icons (M4), no animation curve editor until timelines land (M4). Each is named now so the temptation has an address — the editor grows by milestone gates, not by accretion (binding milestone-gate discipline).

---

## 4. Hot-reload matrix

Hot reload is scoped by a simple contract: **data reloads live; code recompiles fast; DLL reload is a designed-for option, not a v1 promise** (the Handmade Hero platform/game split is the prerequisite, kept cheap now — docs/research/existing-engines.md).

| Asset/code kind | Behavior | Target | REQ |
|---|---|---|---|
| Shaders | File change → glslangValidator re-cook → pipeline swap, no restart. Compile errors surface in the editor console with line numbers; the old program keeps rendering until the new one compiles. | ≤ 1 s from save to pixels | **DX-H1** |
| Textures / materials | Re-import → GPU upload, handles stable, entities keep rendering (magenta placeholder if the new source fails, Unity's convention). | ≤ 1 s | **DX-H2** |
| Scenes (editor document) | Live-reloaded when not in Play mode; dirty-state prompt if the editor has unsaved edits. | ≤ 1 s | **DX-H3** |
| Prefab definitions | Re-cook → instances diff-apply overrides (no runtime inheritance — cook-time templates, per ECS research decision 11). | ≤ 1 s | **DX-H4** |
| Audio clips | Re-import on next use; playing voices finish on the old data. | — | **DX-H5** |
| Gameplay C++ | Recompile (DX-C2 budget). In-editor restart of Play mode, or run relaunch. | ≤ 10 s to next Play | **DX-H6** |
| Game DLL (optional, M3+) | Handmade Hero-style platform/game split makes a hot-reloadable `game.dll` possible; if built, sim state is carried across reload via the save-state blob (raw-pointer-free by binding). | Design-for only; explicit ADR to build | **DX-H7** |

Verification: H1–H5 are one file-watcher + reimport pipeline (asset hashes already exist for cook incrementality); each is a manual stopwatch gate recorded in the milestone doc. H7 is explicitly **not** a requirement to ship — it exists so nobody designs pointer-identity into sim state and forecloses it later.

Shader hot reload (DX-H1) is the single highest-frequency loop in graphics development and the reason the GL 4.5 stack was chosen over PSO-compile pipelines; it is a load-bearing requirement, not a nicety.

---

## 5. Diagnostics: when things go wrong, the engine explains itself

| REQ | Requirement | Verification |
|---|---|---|
| **DX-D1** | Every error a user can hit is either (a) a `Result<T,Error>` with code + asset path + stage, rendered in the console panel and double-clickable to the source, or (b) an `MCE_ASSERT` crash with a minidump path printed (crash pipeline per platform-engineering §6.3). There is no third category of silent weirdness. | Audit each milestone: every failure path found in review maps to one of the two |
| **DX-D2** | Missing/failed assets degrade visibly: missing texture → magenta checker; missing audio → silence + one console error; missing scene → error screen with the path. Never a crash, never a white void. | Negative-asset unit test per kind |
| **DX-D3** | The debug overlay (F1) shows, live: frame time + sim/render split, entity count + archetype count, event counts by type, draw calls/instances per pass, audio voice count, input state. All from the same instrumentation Tracy uses — one source of truth, two consumers. | M1 gate; screenshot in the milestone doc |
| **DX-D4** | Determinism tooling is a user feature: record-and-replay of a session from the fixed-step input stream, plus a "rewind N seconds" debug command (save-state ring). Powered by the determinism test harness. | M2 gate: replay a 60 s sample-gameplay session; final component hashes match the live run |
| **DX-D5** | Tracy is one button away in the editor; a RenderDoc capture launches pre-framed at the current view. Every render feature doc includes its capture (binding profiling discipline). | M0 gate; every milestone doc carries a Tracy screenshot |

The pedagogical point of D3–D5: the engine teaches its own internals by *showing* them. This is the Handmade Hero/Bitsquid ethos — visibility is a feature.

---

## 6. Documentation strategy

Documentation is a **milestone deliverable**, written by the engine-builder as the subsystem is completed — the "explain it to prove you learned it" discipline (docs/research/existing-engines.md, process-over-technology lesson).

| REQ | Requirement | Verification |
|---|---|---|
| **DX-B1** | Every engine subsystem ships, at its milestone, with a one-page `docs/engine/<subsystem>.md`: what it does, its key types, its invariants, and a 10-line usage sketch. Written from memory as a self-test (the LRN gates below). | Milestone checklist item; the doc exists and compiles its own sketch against the headers |
| **DX-B2** | API documentation is **hand-written headers**: every public type/function carries a doc comment stating intent and ownership; no generated reference is attempted (no Doxygen target — for a single reader, generated HTML is noise). | clang-tidy/format check for doc comments on new public API |
| **DX-B3** | Every committed decision in the engine has an ADR or a research doc behind it (`docs/adr/`, `docs/research/`, `docs/tech/`); a "why is it like this?" question must always have a file to point at. | ADR index kept current at each milestone |
| **DX-B4** | The docs live in-repo, in Markdown, versioned with the code they describe. Stale-doc fixes ride the same commit as the API change. | Review habit; enforced by the milestone checklist |

Rejected: a wiki (diverges from code), a generated API site (tooling cost, zero audience beyond one), video documentation (unsearchable). Committed: repo Markdown + living sample projects (next section).

---

## 7. Sample projects — the living documentation

Samples are the primary documentation surface (the Hazel series proved the pedagogy; its stall proves samples must be **kept compiling in CI**, which turns them into tests).

| REQ | Requirement | Verification |
|---|---|---|
| **DX-S1** | A graded sample ladder, each gated at its milestone and kept green in CI forever: `01_triangle` (M0) → `02_boids` (2D ECS + sprites + input, M1) → `03_platformer` (2D physics, audio, prefabs, M1–M3) → `04_cubes` (3D PBR, Forward+, M2) → `05_showcase` (everything; the milestone benchmark scene, M4). | Each sample: builds in CI, cooks in CI, runs headless 300 fixed steps in `mce_devhost` without error |
| **DX-S2** | Every sample's README states, in ≤ 2 pages: what it demonstrates, the 5–12 engine types it touches, and what to open in the editor to see it. A sample that needs a longer README is an API-design bug — file it. | Read-through at milestone gate |
| **DX-S3** | Samples are the golden-image and determinism test subjects (they render the fixed scenes and replay the fixed inputs). One artifact, three jobs: documentation, test, benchmark. | CI: golden-image + determinism jobs run against sample scenes |
| **DX-S4** | The "first hour" experience: a new user (i.e., future-you returning after a month) goes clone → `02_boids` running → modifies one system's behavior → sees the change in the editor, following only the repo README. | Manual gate at M1, re-run at M4 |

---

## 8. Learning milestones (pedagogy gates for the engine-builder)

The project's stated purpose is deep learning through building. These gates are testable the way an exam is: **the builder writes the subsystem's doc page from memory, then answers the listed questions, before the milestone closes.** This is the Feynman-test discipline; it is what distinguishes "used OpenGL" from "learned graphics programming."

| Gate | At | The builder can, from memory, explain... |
|---|---|---|
| **LRN-1** | M0 | The DSA vs. legacy-bind API distinction and why the binding forbids `glBindTexture`-style calls; how the persistent-mapped staging ring + `glFenceSync` slots work and why per-frame-slot sync prevents readback hazards; why glslangValidator runs at cook. *(Evidence: RHI doc page + RenderDoc capture annotated by hand.)* |
| **LRN-2** | M0 | The fixed-timestep accumulator + interpolation math (*Fix Your Timestep*) and why `/fp:fast` is banned in the determinism build; the frame-linear allocator's reset contract and what "zero heap allocation in hot paths" is protecting. |
| **LRN-3** | M1 | Archetype storage mechanics — why `{Transform,Velocity}` iteration is a linear scan, what an archetype edge is, why structural change defers to command buffers; the generational-handle staleness contract; how change ticks drive dirty-transform propagation. *(Study references: flecs/EnTT/Bevy internals, Skypjack series, Bitsquid series — studied, never linked.)* |
| **LRN-4** | M1 | The reflection system end-to-end: macro registration → constexpr traits → inspector rendering → scene serialization → cook. Can add a new component type and predict exactly which machinery engages without looking. |
| **LRN-5** | M2 | The RHI seam: what a GL call may and may not do inside the backend; how the command buffer translates to MDI submits; how a future D3D12 backend would slot in (barriers become real, transient resources become real). Can sketch the same scene submission against the modern-explicit shape from memory. |
| **LRN-6** | M3 | Sequential-impulse solving per Catto's Box2D slides (why warm starting, why Baumgarte vs. positional correction); the audio callback's real-time contract (Doumler's rules) and why the mixer never takes a lock against the main thread. |
| **LRN-7** | M4 | The cook pipeline: why the runtime never parses interchange formats, what vertex-cache optimization/quantization bought (meshoptimizer), and how `.mce` chunk versioning migrates. |

Verification per gate: the subsystem doc page (DX-B1) written from memory + a Q&A self-exam recorded in the milestone doc + the milestone's Tracy/RenderDoc artifacts annotated by hand. A gate that cannot be passed means the milestone does not close (binding milestone-gate rule applies to learning, not just shipping).

**Timebox honesty (binding discipline applied to pedagogy):** if a learning gate cannot close in its milestone's timebox, the correct response is to *cut renderer/feature scope*, never to skip the gate. An unlearned subsystem is unowned; a cut feature can return in the next milestone.

---

## 9. Non-goals for the experience (v1)

Named rejections so they don't return as scope creep, each with its reason:

- **Visual scripting, node graphs, live-tuning C# — rejected.** The user is a C++ programmer; every scripting surface is a second runtime to build and debug. Data-driven behavior (FSM components, sequences, timelines) covers the need per the ECS research §11.
- **In-engine hot reload of *engine* code.** Only gameplay-module rebuilds (DX-C2) and the optional M3+ DLL path (DX-H7). Engine rebuild is a 10 s problem (DX-C3); reload machinery for the engine itself would violate the runtime/editor split.
- **Multiplayer-aware authoring, localization tooling, analytics — out of scope** for small complete games; the scene/prefab formats leave room but nothing is designed now.
- **Cross-platform authoring UX (macOS/Linux editor).** Windows 10 x64 is the dev machine (binding); SDL3 keeps the *runtime* portable-shaped, but editor ergonomics are tuned for the one platform that exists.
- **A shipped-game UI built on ImGui** — rejected twice over (binding + audio/input/UI research); `mce::ui` is its own retained system, phase-2.

---

## 10. Requirement-to-source traceability

| This doc | Grounded in |
|---|---|
| DX-C1..C5 budgets | platform-engineering.md §1 (build latency economics, unity/PCH data); stack decision (velocity criterion) |
| DX-E1 play-mode model | existing-engines.md (Unity editor authority; Hazel's ad-hoc mutation as the failure case) |
| DX-A1..A10 API sketches | ecs-gameplay.md §13.2 (authoritative sketches; this doc restates the user-facing subset) |
| DX-H1 shader reload ≤ 1 s | Stack decision (GL 4.5 / no PSO stalls — the reason the stack won) |
| DX-H7 DLL reload | Handmade Hero platform/game split (existing-engines.md §2.7) |
| DX-D1..D5 diagnostics | platform-engineering.md §5–7 (Tracy/RenderDoc/logging/crash pipeline) |
| DX-B1..B4 docs | existing-engines.md process-over-technology lesson; ADR binding |
| DX-S1..S4 samples | Hazel (pedagogy + the stall as the cautionary tale); golden-image binding |
| LRN-1..7 | Binding from-scratch list; the project's learning mandate |

## 11. Open questions carried forward

1. **ImGuizmo ADR** (DX-E4) — needed before M2's 3D gizmo work; one-page ADR required since it is off the closed dependency list.
2. **Play-mode granularity** — whether Play mode reuses the OS window/swapchain across plays or rebuilds the world in-place under a fresh state root affects DX-E1's 1 s budget; decide during M1 ECS scene-load work.
3. **"Rewind N seconds" (DX-D4)** depends on save-state serialization size; revisit if the ring's memory budget (Our Machinery practice: budget tables alongside frame-time budgets) exceeds ~64 MiB at sample scale.
