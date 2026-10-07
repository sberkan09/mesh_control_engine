# Existing Engine Architecture Survey — Research for MCE

**Domain:** existing-engines | **Date:** 2026-10-06 | **Status:** research input to architecture phase
**Scope:** architectural lessons from Unity, Unreal 5, Godot 4, Bevy, O3DE, id Tech, Hazel, Piccolo, Handmade Hero, the Bitsquid/Our Machinery blog corpus, and canonical literature — distilled into what a **solo developer building a from-scratch 2D/3D engine (MCE)** should copy, avoid, and why.

Committed stack context (from the technology decision): C++20 / MSVC v143, OpenGL 4.5 Core behind a thin RHI, SDL3, Dear ImGui (docking), archetype ECS, cook pipeline to `.mce`, fixed-timestep sim, editor-owns-scene.

---

## 1. State of the art: what engines converged on by 2026

Fifteen years of engine development produced a surprising amount of convergence. Almost every serious engine today shares these traits:

1. **A layered, module-based codebase** with a thin platform layer and thick subsystems (Gregory, *Game Engine Architecture*, ch. 1–4 describes the layering that Unity, O3DE, and Unreal all follow in different clothing).
2. **A "renderer abstraction / RHI" seam** between engine code and graphics API. Unreal (RHI), Godot 4 (RenderingServer + RendererRenderDevice over Vulkan/GL), O3DE (Atom: RPI over RHI), Bevy (RenderDevice/render graph), The Forge, Flax, Stride — all have one. Nobody writes engine code against a raw graphics API anymore, because D3D12/Vulkan/Metal fragmentation made it untenable.
3. **Cooked, runtime-native asset formats** behind an import/cook step. The runtime never parses JSON/XML/PNG. id Tech shipped `.pk3` + native bsp; Unreal uses `.pak` + cooked packages; Godot added binary `.res`/imported formats (Godot 4.x imports to a cache of binary resources); O3DE ships AzModel/AzBuffer binary formats. Unity is the extreme version: every asset is imported into a library of native binary forms.
4. **An editor that owns canonical scene data**, with the runtime as a consumer. Unity's editor serializes YAML scenes/asset GUIDs; Unreal's editor saves `.umap`/Blueprints and cooks the result; O3DE made **prefabs** the unit of scene ownership; Godot serializes `.tscn`/`.escn` and the editor *is* a game running the same engine.
5. **Data-oriented organization of hot paths.** Mike Acton's CppCon 2014 talk and Bitsquid's blog posts started it; Unity DOTS, Unity's Burst, EnTT, flecs, Bevy, and Our Machinery industrialized it. Even OOP-first engines (Godot) moved rendering internals to flat arrays.
6. **Explicit GPU synchronization with frame pipelining.** Swapchain image count 2–3, per-frame-slot resources, fences — identical shape in D3D12, Vulkan, Metal.
7. **Immediate-mode or retained-mode debug/editor UI as a separate subsystem** — Dear ImGui became the industry default for tools (used inside Unreal, in virtually every mid-size studio, and in Godot's own editor-adjacent tooling and countless engine editors).
8. **Frame profiling as infrastructure, not an afterthought.** Tracy, ETW/PIX, Optick. Engines that did not invest early (older Unity profiler, pre-4.3 Godot) paid for years.

What did **not** converge: scripting (Lua vs. C# vs. Blueprints vs. GDScript vs. native-only), scene-graph vs. ECS vs. component-entity hybrids, and whether the renderer should be a directed graph of passes (frame graph) or hand-orchestrated. These remain genuine forks in the road — and they are exactly the decisions MCE must make deliberately.

---

## 2. Surveyed engines

### 2.1 Unity

**Architecture in one line:** a component-based scene model (`GameObject` = ID; `Component` = C# object holding data + behavior), an editor that serializes scenes as YAML with GUID-based asset references, a scripting VM (Mono/IL2CPP) above a C++ native core, and — since 2015 — a parallel data-oriented stack (DOTS: ECS + Job System + Burst) coexisting with the legacy model.

**What to copy:**
- **Editor authority + serialized object model.** Everything in Unity is a serializable object with fields and GUID references, and the editor is the only writer. This is the cleanest statement of the MCE binding "editor owns canonical scene representation." MCE copies the *pattern*: scenes serialize through reflection; runtime is a read-only consumer.
- **Asset import as a separate step with cached native forms.** Unity's `Library/` folder of imported artifacts is the same idea as MCE's cook step to `.mce`. The takeaway: source formats are for humans; runtime formats are for the machine; a deterministic import step stands between them.
- **The DOTS lesson about *why* ECS exists** — not as dogma but because iterating a `float3` velocity array over tightly packed entities is 10–50x faster than iterating scattered C# objects (Unity's own transform-access benchmarks; Skypjack's *ECS back to basics* series reaches the same conclusions for archetype storage). EnTT and flecs prove the same layout works in C++ without a JIT.
- **Prefab/instance model** for authoring reuse (nested prefabs shipped in 2018 and immediately became the standard).

**What to avoid, and why:**
- **The legacy `GameObject`/`Component` class model is a memory-layout trap.** Each component is a separate heap object (in C++, a separate allocation per component per entity), iteration is pointer-chasing, and `GetComponent` calls dominate profiles. This is precisely the anti-pattern Mike Acton demos at CppCon 2014. MCE's archetype ECS exists to avoid it; Unity DOTS is the proof that the fix is structural, not a patch.
- **Scripting-VM-first design.** Mono domain reloads became a notorious Unity iteration tax (edit-compile-run stalls of 10+ seconds are a running community complaint); IL2CPP exists because the VM model didn't hold up. MCE's C++-component-first approach with a *deferred* Lua phase avoids building iteration speed on a VM.
- **Feature sprawl without a spine.** Unity's monorepo carries decades of parallel systems (two UI systems, two input systems, old/new physics, legacy + DOTS renderers). The lesson for a solo dev is brutal: **every parallel path you keep alive is a permanent tax.** MCE's closed dependency list and one-code-path-per-feature rule come straight from watching this.

### 2.2 Unreal Engine 5

**Architecture in one line:** a giant C++ actor-component runtime with code-generated reflection (Unreal Header Tool), a mark-sweep GC over `UObject`s, a Blueprint VM, a module system with per-module build scripts, and a renderer split into a platform-independent `Renderer` over an `RHI` (D3D12/Vulkan/Metal) — with Nanite (virtualized geometry), Lumen (real-time GI), and World Partition (streamed open worlds) as the flagship systems.

**What to copy:**
- **The RHI seam, literally.** Unreal's rule — engine/renderer code speaks an abstract `RHI` (buffers, textures, pipelines, command lists), only the `D3D12RHI`/`VulkanRHI` modules touch the API — is exactly MCE's binding RHI discipline. Unreal proves a ~3k-line-equivalent abstraction can carry AAA features.
- **Modules with explicit dependency direction** (gameplay depends on engine; engine never depends on game; renderer depends on RHI; RHI depends on nothing above). Unreal's `.Build.cs` enforcement is the mechanism; MCE gets the same effect with CMake target dependencies plus a GL-symbol-leak CI check.
- **World Partition's core idea** — the world is a grid of cells streamed by bound — matters even at small scale: MCE's scene format should be *chunked* so streaming later is an add, not a rewrite.
- **Nanite/Lumen as evidence of what a data-driven, GPU-driven architecture enables** — clusters, hierarchical culling, visibility buffers. Worth understanding even though MCE will never build them; they justify keeping the renderer submission layer data-oriented from day one.

**What to avoid, and why:**
- **Reflection by external code generation (UHT).** Unreal's C++ is effectively a dialect (macros + a custom parser + generated glue). The build cost, IDE breakage, and learning cliff are real. MCE's macro + `constexpr` trait reflection achieves the same editor/serialization goals with zero tooling — C++26 static reflection is the future escape hatch, not a dependency now.
- **GC-managed everything.** `UObject` mark-sweep GC is a major source of frame hitches and a large fraction of engine complexity, because *every* object lifecycle decision flows through it. MCE's generational-index handles + explicit lifetime (assets owned by an arena/registry) avoid an entire subsystem class of bugs and stalls.
- **Editor and runtime in one binary with gameplay hooks everywhere.** Powerful for a large team, fatal for a solo dev's compile times and conceptual load. MCE keeps the editor a *client* of the engine (the Godot model, see below), not a tumor inside it.
- **Blueprint-as-architecture.** Blueprint is a great gameplay tool; as an architectural load-bearing element it hides costs (VM overhead, refactor brittleness) and bifurcates the codebase. MCE defers scripting to phase 2 (Lua via sol2 recorded decision) and keeps gameplay in native components until profiling proves the need.

### 2.3 Godot 4

**Architecture in one line:** a `Node` tree where every scene is a composition of nodes (2D/3D/control), communicated with via signals and owned data (`Resource`), above a set of **singleton "servers"** (RenderingServer, PhysicsServer2D/3D, AudioServer) that do the actual work in flat, data-oriented C++ — with the editor itself built as a Godot scene tree running the same engine, and a renderer abstracted into RenderingDevice with Vulkan/GL backends.

**What to copy:**
- **The "server" pattern.** Godot's biggest architectural idea: subsystems expose a thin API (create/destroy/configure by handle) and own their data internally, instead of exposing objects. This is *precisely* the shape of MCE's RHI (RenderDevice/CommandBuffer) and audio mixer (voices by handle), and it is why the MCE bindings mandate generational handles everywhere.
- **Editor-as-engine-client (self-hosting).** The Godot editor is a scene using engine widgets; it proves an editor can be "just another app" on top of the runtime, not a fork of it. MCE's ImGui editor adopts the same stance: editor code reads scenes via the same public API gameplay uses.
- **Signals (first-class events) on nodes** — a tiny, cheap observer pattern that removes most "who updates whom" coupling in gameplay glue. MCE's ECS should grow an event/signal queue for cross-system notification (Nystrom, *Game Programming Patterns*, ch. "Event Queue").
- **Renderer backend pluralism done cheaply:** Godot's Clustered (desktop Vulkan), Mobile (Vulkan), and Compatibility (GL 3.3) backends show that a single upper renderer can serve very different backends — support for MCE's belief that a modern-explicit-shaped RHI over GL 4.5 keeps a D3D12/Vulkan future open.

**What to avoid, and why:**
- **The Node tree as the universal object model.** Nodes are fat OOP objects (`Node2D` → `CanvasItem` → `Node`), scene traversal is pointer-chasing, and the engine's own docs concede heavy scenes pay for it. Godot deliberately rejected ECS; the cost shows in transform propagation and physics broadphase. MCE's archetype ECS is the counter-bet, and Godot is the cautionary tale that justifies it.
- **GDScript-everywhere ergonomics leaking into core design.** Nice for onboarding, but it pushed Godot to store variant-typed properties on nodes rather than strict typed data (typed GDScript arrived late and only partially fixed it). MCE: typed components, reflection for tools, no variant soup.
- **Version 3→4 migration chaos.** The language and API break was so large the community split for years. MCE's mitigation is already in the bindings: versioned `.mce` containers, ADRs for breaking changes, and milestone gates that force serialized-scene round-trip tests.

### 2.4 Bevy

**Architecture in one line:** a Rust engine organized as a dependency graph of independent crates (`bevy_ecs`, `bevy_render`, …) where everything is a **Plugin** registering **Systems** into a deterministic **Schedule**, with archetype ECS, change detection, and a **render graph** (directed acyclic graph of pass nodes) over wgpu.

**What to copy:**
- **Schedules with explicit, *inferred* system ordering.** Bevy's insight: most systems don't need manual ordering — declare data access (queries with mutable/immutable access) and derive parallelism and order from conflicts. For MCE, this suggests the job system should at minimum auto-parallelize *declared-conflict-free* ECS system groups rather than requiring hand-chained job graphs everywhere (reference: flecs' paired pipeline phases solve the same problem more simply).
- **Change detection as a first-class ECS feature.** Bevy's `Changed<T>` filters (implemented as per-component tick stamps) make reactive gameplay cheap and kill a whole class of "did I forget to set dirty?" bugs. This is cheap to implement in an archetype ECS (a tick array per component column) and MCE should have it from the start.
- **Plugin architecture as the module system.** Bevy apps are built from `Plugin`s; the engine itself is plugins. MCE can approximate: each engine subsystem is a CMake target plus an explicit `init/step/shutdown` registration, keeping "engine as a bag of systems" honest.
- **Render graph idea (from Frostbite's frame graph, GDC 2017, which Bevy inherited):** declare passes + resource reads/writes; derive barriers, aliasing, and culling of unused passes. MCE's GL backend can *no-op barriers* and still use the graph to order passes and cull work — the graph is architecture, not API ceremony.

**What to avoid, and why:**
- **Breaking-release cadence as a way of life.** Bevy's pre-1.0 churn (v22→v25 across 2024–2025 in wgpu-land, Bevy's own API breaks across 0.10→0.15) is survivable for apps, brutal for an engine you own for years. MCE's pinned-dependency + ADR policy is the direct countermeasure.
- **Compile-time metaprogramming weight.** Bevy's derive-heavy, monomorphizing style gives Rust-style compile times (minutes) that a solo dev feels daily. MCE's C++20 commitments (no heavy TMP at API boundaries, unity builds, PCH) exist to prevent the same tax in MSVC.
- **"ECS for everything" maximalism.** Bevy UI, audio, and editor surfaces all bend around the ECS, sometimes awkwardly. MCE commits to ECS for simulation/scene entities, but the editor (ImGui), the mixer (voice handles), and the renderer (its own resource model) keep their own idioms — the ECS is the *scene* database, not the *engine* database.

### 2.5 Open 3D Engine (O3DE)

**Architecture in one line:** the open-sourced Lumberyard rebuilt as a modular engine: `AZ::Component`-based entity model, **prefab-as-source-of-truth**, **Gem** packages for every subsystem, Atom renderer with a strict RPI-over-RHI split (Vulkan/DX12/Metal), and a code-gen-backed reflection/serialize context.

**What to copy:**
- **Gems = hard module boundaries.** Every capability (physics, audio, UI) is an optional Gem with declared dependencies. For MCE this validates the CMake-per-subsystem + closed-dependency-list approach and, more importantly, the rule that *the engine core must not know subsystem names*.
- **Prefab-first scene ownership.** O3DE went further than Unity/Godot: the prefab (with overrides) is the only saved scene unit; instances are procedural. For MCE this is the right target for M4 editor maturity (instancing + overrides) and validates making `.mce` scene chunks composable from day one.
- **Atom's RPI/RHI split:** a mid-level "RPI" (passes, views, feature processors) over a low "RHI" (API wrappers). It confirms the MCE plan of RHI-now / render-graph-later, and shows where the boundary naturally falls (~feature processors ≈ MCE's future pass features).

**What to avoid, and why:**
- **Governance-by-committee architecture.** O3DE's build system and module ceremony (AzAutoGen, build pipelines, environment tooling) is heavy enough that a solo dev could spend a month before a triangle. The solo lesson: module boundaries yes, module *ceremony* no.
- **Prebuilt-binary engine distribution** (engine as an installed SDK with project templates). Great for teams, unnecessary indirection for a single developer; MCE is a source tree with presets, full stop.
- **Serialized context so abstract the editor can't show it usefully without extra work** — a warning that reflection should be designed *for the inspector first* (name, type, range, UI hint on the field), which is exactly how MCE's reflection traits are specified.

### 2.6 id Tech (historical line, Doom → Quake → Doom 3 → id Tech 7)

**Architecture in one line:** engines written by tiny teams that shipped era-defining games, organized around a simple loop: platform layer → game loop → systems (render/physics/audio) over cooked-native formats (`.wad`, `.pak`, BSP), evolving to unified shader pipelines and bindless/virtual-textured rendering (Megatexture, id Tech 7).

**What to copy:**
- **The discipline of tiny teams with hard format boundaries.** Doom (1993) was ~5 people; the modding explosion happened because the data formats (WAD, lumps, things) were documented and simple. MCE's `.mce` chunked container is the same philosophy: a *simple, documented, versioned* container beats a rich one.
- **Fabien Sanglard's code reviews (Doom 3, Wolfenstein 3D) and the Game Engine Black Books** document the frame model: fixed-size system tick + render decoupled from sim — the same fixed-timestep + interpolation shape MCE binds.
- **id Tech 7's renderer**: bindless materials, cluster-based culling, and almost no runtime shader compilation — evidence that "cook everything, index everything at runtime" scales to the top of the industry. Validates MCE's cook-first asset stance.

**What to avoid, and why:**
- (Honest limit: little. id engines are the best "small team" reference in existence.) The one caution: **C-era global state** (`com_frameTime`, dozens of `cvar`s read from anywhere) made reasoning about the frame hard — visible in the Black Book. MCE's explicit frame context (one struct passed down, frame allocator reset) is the modern replacement.

### 2.7 Hazel (The Cherno)

**Architecture in one line:** an educational C++ engine built on YouTube (2019–2022): layered `Application` with a Layer stack, scene as `entt::registry` (after early hand-rolled component maps), `Entity` = ID + component wrapper, YAML scene serialization, ImGui editor (`HazelEditor`), OpenGL 4.5 renderer with a batched 2D renderer, later stalled before depth (no cook pipeline, no physics, eventual inactivity).

**What to copy:**
- **The Layer stack** (core layers + overlay layers, update in order,ImGui as an overlay). It is a clean, minimal answer to "how do tools/debug UI plug into the frame" and maps directly onto MCE's editor shell.
- **The pedagogy itself**: Hazel proves a solo dev can build and *narrate* a Windows/OpenGL/ImGui engine exactly in MCE's shape (SDL-ish input → GL renderer → editor → serialization). Its early episodes (window abstraction, event bus, batch renderer) are the closest public reference implementation to MCE's M0/M1.
- **Batched 2D renderer** (one dynamic vertex buffer, per-batch flush on texture switch, quad submission by component): essentially the design MCE's 2D path will use over MultiDrawIndirect.

**What to avoid, and why:**
- **`std::shared_ptr` everywhere and raw owning pointers across module boundaries.** Hazel's codebase is a museum of ownership ambiguity; MCE's rules (generational handles for cross-frame references, `std::unique_ptr`/value semantics for ownership, no shared ownership in engine code) exist because watching Hazel struggle with lifetime bugs is the cheapest lesson available.
- **No asset cook step and no versioned containers.** YAML scenes edited by hand at runtime, textures loaded by stb at load-time — fine for a demo, fatal for "ship small games" (load times, no determinism, no streaming). This is the single biggest architectural gap between Hazel and MCE's requirements.
- **Scope without milestone gates.** Hazel grew feature-by-feature (scripting → Vulkan rewrite attempts → pause) with no shippable-artifact gate; the series effectively stalled. MCE's binding milestone gates exist because of this pattern.
- **Editor as a separate app but sharing *mutable* scene state ad hoc.** Without editor-authority discipline, serialization became the editor's own subsystem. MCE's binding (editor owns canonical scene; runtime never mutates; all flows through reflection) is the fix.

### 2.8 Piccolo (open-source, solo-dev C++ editor engine)

**Architecture in one line:** a compact open-source engine (originally "Pikachu") built by one developer with AI assistance: runtime/editor split, a **meta-tool that generates reflection/serialization code** from annotated headers, an editor (ImGui + custom widgets) owning scenes, a render system with a thin abstraction over Vulkan, Jolt physics, and a component-based gameplay layer.

**What to copy:**
- **Proof that a solo dev can ship an editor-capable engine** — Piccolo is the closest public project to MCE's actual shape (one person, editor + runtime, C++, ImGui, modern-ish renderer). Its existence de-risks the MCE plan.
- **Reflection-as-codegen, done lightly.** A small offline tool generating field-walk code from annotations is a viable alternative to macros. MCE sticks with macro+constexpr traits (no build step, MSVC-friendly), but Piccolo shows the design pressure point: reflection must cover *serialization + editor UI + component registration* with one mechanism.
- **RHI-thin-over-one-API.** Piccolo's renderer sits over Vulkan only, proving that a thin seam pays even for a single backend — the same reasoning as MCE's GL-only-but-explicit-shaped RHI.

**What to avoid, and why:**
- **AI-assisted sprawl without tests.** Large parts of Piccolo were AI-generated with thin test coverage; the result has structural inconsistencies (mixed ownership conventions, uneven error handling) that a solo dev *feels* on every refactor. MCE's binding test/CI policy (unit tests for math/ECS/serialization, clang-tidy, ASan/UBSan CI) is the direct countermeasure.
- **Editor widgets hand-rolled per component** rather than driven uniformly by reflection — the inspector drift problem MCE's binding architecture-authority rule forbids.

### 2.9 Handmade Hero (Casey Muratori) and the "simple-first" school

**Architecture in one line:** a complete Windows game built from scratch on stream: a platform layer (Win32) ↔ game layer split across a hot-reloadable DLL boundary, software rendering first, then D3D11; no external libraries in the core; SIMD-optimized inner loops; memory allocated once at startup into arenas.

**What to copy:**
- **The platform/game split with a C-style API boundary.** Platform provides frame input/time/audio buffer; game is a pure update function over game state. This is *the* most valuable single pattern in the corpus for MCE: it makes hot-reload possible later, makes testing trivial (sim without OS), and forces the runtime/editor split MCE already binds.
- **Arena-first memory discipline.** Casey allocates all game memory at startup (permanent + transient arenas). MCE's frame-linear allocator + pool allocators + zero-heap-in-hot-paths binding is the same policy; Handmade Hero is the reference implementation to imitate.
- **Hot-reloadable game code.** Platform loads/unloads `game.dll` with a state struct carried across reloads. MCE should treat this as an *optional* M3+ capability — cheap to design for now (the platform/game split is the prerequisite) and expensive to bolt on later.
- **"Semantic compression" and performance-aware programming** (Casey's talks): write the dumb version, then compress it with real knowledge. Anti-dogma, pro-measurement.

**What to avoid, and why:**
- **Software rendering past its purpose.** Educational gold, but don't carry it into the shipping architecture; MCE goes straight to GPU with GL 4.5.
- **Single-platform Win32 assumptions inside game code.** Casey's lesson is the *boundary*, not the Win32; SDL3 takes that slot in MCE while the boundary discipline stays.

### 2.10 Bitsquid/Our Machinery blog corpus (Frykholm, Persson)

**Architecture in one line:** Bitsquid (→ Stingray) and Our Machinery were small-team, data-oriented engines whose *blogs* became canonical: data-oriented entity systems, Lua everywhere for gameplay glue, explicit memory budgets, Vulkan-first rendering with minimal abstraction layers.

**What to copy (these blogs are MCE's secondary canon):**
- **"Design for data"** (Bitsquid's *Building a Data-Oriented Entity System* series): start from the hot-loop data layout, derive the API. This is the design order for MCE's archetype ECS.
- **Explicit memory budgets per subsystem** (Our Machinery's memory posts): a budget table per milestone (already an MCE binding via frame-time budget) should have a *memory* twin.
- **Minimal-but-explicit render abstraction** (Machinery's Vulkan layer): proof that a small explicit layer can serve AAA-grade rendering when the *shape* matches the modern APIs — the strongest external support for MCE's "modern-explicit-shaped RHI, barriers no-op on GL" plan.

### 2.11 Brief notes on others

- **Flax Engine:** C#/C++ hybrid with a genuinely good editor and a clean renderer abstraction; the best modern example that "C++ core + managed-ish scripting" can coexist — but the interop seam is exactly what MCE's single-language decision avoids.
- **Stride/Xenko:** C# engine with an editor; shows the reflection-driven inspector ceiling (great) and the raw-GPU-from-C# cost (thin docs, interop debugging) that the technology panel already rejected.
- **Overload (educational, French):** C++ + OpenGL + ImGui solo-ish engine with editor, closest open-source "student MCE"; useful as a code reference for editor gizmos and scene serialization done at achievable scale.
- **Kohi (educational):** Vulkan-first educational engine; strong on Vulkan/RHI pedagogy, but its GL-absent, D3D12-absent path confirms the panel's point that single-raw-backend education is slow — MCE's RHI-with-GL-first is the faster route to the same concepts.
- **CryEngine:** feature-complete with a legendary Sandbox editor; cautionary tale of complexity/nurture imbalance (tiny team, huge feature surface, dwindling ecosystem) — the solo-dev death-spiral in commercial form.
- **Raylib:** not an engine, but its philosophy (small, flat, immediate, zero-config) is worth absorbing into MCE's *runtime API surface*: gameplay-facing APIs should be boring, flat, and obvious.

---

## 3. Cross-cutting lessons for a solo from-scratch engine

**L1 — Layering is the one non-negotiable.** Every surveyed engine converges on Gregory's layering: platform layer → core systems (memory, containers, math) → resources → renderer/RHI → gameplay/ECS → tools. Deviations (Hazel's ad-hoc includes, CryEngine's historical tangle) cost more than any single feature. **MCE:** enforce with CMake target graph + include-direction CI rule.

**L2 — The RHI seam is the highest-leverage abstraction.** Unreal (RHI), Godot (RenderingDevice), O3DE (Atom), Bevy (RenderDevice) all built one; all later added backends through it. The seam pays even if never used for a second backend, because it *localizes* graphics debugging, forces explicit resource states into the design, and keeps engine code readable. **MCE:** binding already in place — RenderDevice/CommandBuffer, GL confined to one directory, modern-explicit shape.

**L3 — Cook-first assets are what make an engine shippable.** Unity's import cache, id's native formats, Unreal's cook, Godot's imported binary resources, Piccolo's packed assets — every shipping engine separates human formats from runtime formats. The runtime-never-parses-JSON rule (already bound for MCE) is the single clearest "real engine" tell, and it also makes load code trivial and deterministic.

**L4 — Editor authority: one canonical scene, one serializer.** Unity (serialized objects + GUIDs), Godot (editor is a scene; resources), O3DE (prefabs), Unreal (editor-owned world) — all converge: the editor owns scene state; runtime consumes. Engines that let editor and runtime mutate independently (early Hazel, various hobby engines) end up with divergent save/load bugs. **MCE:** binding already in place; reflection is the enforcement mechanism.

**L5 — ECS: archetype + change-detection, but not maximalism.** Unity DOTS, EnTT, flecs, Bevy all validate archetype (table) storage with sparse-set lookups; Bevy adds cheap change ticks. But Godot's Node tree and Unity's legacy components prove OOP scene graphs *can* ship games — ECS buys iteration speed and determinism, at the cost of design friction for hierarchical/prefab-style content. **MCE:** archetype ECS for the scene/sim, with parent/child via archetype edges (flecs-style) added in a later phase; editor/UI/mixer keep their own idioms.

**L6 — Fixed timestep + interpolation is settled science.** Gaffer On Games' *Fix Your Timestep*, Box2D's default stepping, and every physics-based engine converge: simulate at fixed 60 Hz, render interpolated. MCE binds it; the survey finds no dissent among engines that care about determinism (and validates adding a determinism test from M1).

**L7 — Profiling is an architecture, not a tool.** Tracy-from-day-one (bound), frame budgets (bound), and RenderDoc-verified render features (bound) match how Unreal (Unreal Insights/PIX), id (etw), and Bevy (tracing) operate. The survey adds: give every subsystem a **memory budget** alongside the frame-time budget (Machinery practice).

**L8 — Solo-dev process matters more than solo-dev technology.** Hazel (stalled: no gates), Piccolo (drifted: AI sprawl, thin tests), O3DE (committee weight) vs. id (tiny team, hard formats, ship gates) and Handmade Hero (relentless incremental shippability). The process bindings MCE already carries (milestone gates, ADRs, CI, no-slip rule) are the distilled lesson of the whole survey.

---

## 4. Recommendation for MCE — committed decisions

Each row is a committed decision grounded in the survey. (Bindings from the technology decision are restated as context, not re-litigated.)

| # | Decision | Adopted from | Rejected alternative & why |
|---|---|---|---|
| 1 | **Layering:** platform → core → assets → RHI → renderer → ECS/gameplay → editor/tools, enforced by CMake target dependency direction + include-direction check. | Gregory; Unreal module direction | Godot's "everything sees everything" single lib (compile-time death at scale, unclear seams). |
| 2 | **RHI:** thin RenderDevice/CommandBuffer over GL 4.5 Core (DSA, persistent-mapped staging rings, MDI), modern-explicit shape (pipeline objects, explicit barrier calls that no-op on GL, frame-scoped transients). | Unreal RHI, O3DE Atom, Machinery's Vulkan layer | Godot RenderingDevice's full multi-backend generality (too much abstraction too early for one backend); Bevy/wgpu-style auto-barriers (hides the learning objective). |
| 3 | **Scene model:** archetype ECS (tables + archetype edges for hierarchy later, change-detection ticks from day one), editor-owned canonical scene, runtime read-only. | EnTT/flecs designs (studied, not linked), Bevy change ticks, Unity/Godot editor authority | Unity GameObject model (allocation + pointer-chasing, Acton's CppCon critique); Godot Node tree (fat objects, traversal cost); Bevy-style ECS-everywhere (UI/audio forced through ECS idioms). |
| 4 | **Frame model:** fixed 60 Hz sim + interpolated render + decoupled input sampling; frame context struct passed explicitly; frame-linear allocator + pools; zero heap in hot paths. | Gaffer On Games, id/Box2D stepping, Handmade Hero arenas | Variable-delta sim (nondeterminism, physics instability); Casey's software renderer (educational detour, not architecture). |
| 5 | **Assets:** glTF 2.0 + PNG/JPG + WAV as interchange only; cook to versioned chunked `.mce` (magic+version+CRC+16B-aligned payload), meshoptimizer at cook, memory-mapped load, deterministic outputs. | Unity import cache, id formats, Unreal cook, MCE binding | Godot's runtime import (convenient, slow, nondeterministic); runtime JSON parsing (Godot 3 era, Hazel YAML — rejected for load time and determinism). |
| 6 | **Reflection:** one macro + constexpr trait system serving serializer + inspector + component registration; fields carry UI hints (range, tooltip) — designed inspector-first. | MCE binding; Piccolo's one-mechanism lesson; O3DE inspector-first warning | UHT-style external codegen (build fragility); C++26 reflection (not yet shippable on MSVC); per-component ad-hoc widget code (Hazel/Piccolo drift). |
| 7 | **Editor:** ImGui docking, Layer-stack-style overlay integration, editor as a *client* of the runtime API; prefab + override model deferred to M4 but scene chunks designed composable now. | Godot self-hosting, O3DE prefabs, Hazel's layer stack | Unreal's editor-in-runtime-binary (build time, coupling); custom retained UI now (deferred debt, per binding). |
| 8 | **Cross-system communication:** ECS event queue (ring buffer per frame) + explicit system ordering in a simple declarative pipeline; no global singletons besides a small, documented core-services struct. | Godot signals, Nystrom event queue, Bitsquid pipelines | id-era cvar/global state (frame reasoning becomes impossible); Bevy full schedule inference (over-engineering for solo scale; adopt a simpler declared pipeline instead). |
| 9 | **Handles over pointers:** all cross-frame references (textures, meshes, entities, voices) are generational-index handles; assets owned by registries with explicit acquire/release. | MCE binding; Bevy/generational arena practice; counter to Unreal GC | UObject-style GC (stalls, complexity — Unreal's biggest architectural tax); raw shared_ptr graphs (Hazel's lifetime-bug museum). |
| 10 | **Process:** milestone gates with runnable artifacts, Tracy from M0, RenderDoc-verified render features, golden-image tests at cook, ADRs for every dep/structural change, memory budget table alongside frame budget. | MCE bindings + Machinery budgets + id ship discipline | Hazel's ungated growth (stalled); O3DE's committee weight (solo-incompatible). |

---

## 5. Ten decisions other engines got right or wrong

*(Right = copy; Wrong = avoid. Each with the engine evidence.)*

1. **RIGHT — Cook-time asset processing with a runtime-native container** (id, Unreal, Unity, Godot 4). Load times, determinism, and hot-reload all get simpler. MCE: cook to `.mce`, runtime never sees JSON/PNG.
2. **RIGHT — A thin RHI seam even with one backend** (Unreal RHI, O3DE Atom, Godot RenderingDevice). Localizes API code, keeps engine readable, keeps the second backend a migration. MCE's binding codifies it.
3. **RIGHT — Fixed timestep + interpolated rendering** (Gaffer On Games, Box2D, id). Settled; nondeterministic variable-delta physics is the classic amateur tell.
4. **RIGHT — Editor owns canonical scene; runtime consumes** (Unity, Godot, O3DE). Kills save/load divergence bugs by construction.
5. **RIGHT — Frame/arena memory discipline with zero heap in hot paths** (Handmade Hero, Machinery, Naughty Dog fiber-era practice). Removes allocation stalls from the profile permanently.
6. **WRONG — GC-managed everything** (Unreal UObjects; Unity Mono era). Stall spikes, lifetime mysteries, and a permanent subsystem tax. Generational handles + explicit ownership is the lighter, solo-friendly answer.
7. **WRONG — Reflection via external codegen for a small team** (UHT; O3DE's autogen). Powerful at Unreal's scale, a build-system liability at MCE's. Macro+constexpr traits do the job with zero tooling.
8. **WRONG — Keeping two live paths of every subsystem** (Unity's old/new everything; Godot 3/4 fork pain). Parallel paths are unbounded maintenance debt; one path per concern, versioned formats, ADRs for breaks.
9. **WRONG — Scene-graph OOP as the universal object model** (Godot Node tree, pre-DOTS Unity). Ships games, but pointer-chasing transforms and per-node allocations cap scene complexity and teach the wrong data-layout habits; the archetype ECS is the industry's measured answer (DOTS, EnTT, flecs, Bevy).
10. **WRONG — Building without gates or budgets** (Hazel's stall, CryEngine's nurture imbalance). A solo engine dies from ungated scope, not from technology. Runnable-artifact milestones, frame-time and memory budgets, Tracy/RenderDoc from day one — the process layer is the real architecture.

---

## 6. Sources relied on (canonical, from knowledge; web search unavailable)

- Jason Gregory, *Game Engine Architecture* (layering; resource; renderer; gameplay framework chapters).
- Glenn Fiedler, *Fix Your Timestep* and networking series, Gaffer On Games.
- Casey Muratori, *Handmade Hero* (platform/game split, hot reload, arenas); *Semantic Compression*; CppCon 2014 Mike Acton, *Data-Oriented Design and C++*.
- Erin Catto, Box2D GDC slides (sequential impulse solver); Box2D source as solver reference.
- Bob Nystrom, *Game Programming Patterns* (Update Method, Component, Event Queue, Object Pool, Dirty Flag).
- Fabien Sanglard, *Game Engine Black Book: Doom 3* / *Wolfenstein 3D* code reviews.
- Niklas Frykholm & Tobias Persson, Bitsquid blog (*Building a Data-Oriented Entity System*) and Our Machinery blog (Vulkan layer, memory, simulation design).
- Skypjack, *ECS back to basics* (archetypes vs sparse sets); flecs and EnTT documentation as design references (studied, not linked).
- Frostbite, *FrameGraph: Extensible Rendering Architecture in Frostbite* (GDC 2017); Bevy render graph docs as the open-source expression.
- Christian Gyrling, *Parallelizing the Naughty Dog Engine Using Fibers* (GDC 2015) — concurrency reference for the later job-system phase.
- Public source/docs and developer blogs of Unity (DOTS, nested prefabs, IL2CPP), Unreal 5 (UHT, RHI, Nanite, Lumen, World Partition), Godot 4 (RenderingServer/RenderingDevice, Node/Resource model, editor self-hosting), Bevy (schedules, change detection, plugins), O3DE (Gems, prefabs, Atom RPI/RHI), Hazel (The Cherno series), Piccolo (GitHub), Overload, Flax, Stride, Kohi, id Tech (Sanglard reviews, John Carmack's .plan-era writings).

*Note: web search was unavailable in this environment; all claims above are grounded in canonical, widely documented sources from knowledge and named inline for verification.*
