# Research: Gameplay Architecture — ECS and Everything Around It

**Domain:** ecs-gameplay · **Project:** Mesh Control Engine (MCE) · **Date:** 2026-10-06
**Status:** Research → feeds the gameplay programming model. Binding constraints from the stack decision apply: the ECS is **hand-written** (flecs/EnTT/Bevy are study references, never linked), fixed-timestep simulation at 60 Hz, generational-index handles everywhere, zero per-frame heap allocation in hot paths, reflection-driven serialization, and the editor owns the canonical scene.

---

## 1. Scope and framing

"Gameplay architecture" is the layer between the engine's machinery (renderer, physics, audio) and the game itself: how the user of MCE declares game objects, attaches data and behavior, reacts to things happening, and composes scenes. The modern answer to that question is the Entity-Component-System pattern, but an ECS is **not** a complete gameplay model — it has known blind spots (state machines, sequenced content, UI, resource graphs) that need adjacent machinery. This document surveys:

1. ECS storage models in depth — archetype vs sparse-set vs bitset, against the three best-documented reference implementations (flecs, EnTT, Bevy).
2. Systems, scheduling, structural changes, events, change detection.
3. Hierarchies/relationships, prefabs, scene composition.
4. Where ECS alone is not enough — and what to build instead.
5. An honest comparison against simpler OOP/component models.
6. The committed recommendation for MCE, with a user-authored-behavior sketch.

Out of scope: rendering architecture (separate doc), netcode (not a goal), scripting-VM internals (Lua is a phase-2 decision already recorded in the stack panel).

---

## 2. State of the art — what the reference ECSs actually do

A short, accurate history so we steal from the right era:

- **2007–2013, the "OOP hangover" era.** Deep `GameObject` inheritance hierarchies (the pre-Unity model, described in *Game Engine Architecture*, Gregory, ch. 14) gave way to composition. Bob Nystrom's *Game Programming Patterns* codified the **Component**, **Update Method**, **Event Queue**, and **Dirty Flag** patterns — still the best plain-language treatment of what an ECS is assembled from.
- **2015–2018, the data-oriented turn.** Vittorio Romeo's ECS talks and demos (self-implementing an entity system in C++), the Bitsquid/Stingray blog series on building a data-oriented entity system (Niklas Frykholm), and Skypjack's *ECS back to basics* series (2019) established the two storage models that still dominate: **sparse sets** (EnTT) and **archetypes** (Unity DOTS, flecs, Bevy). Catherine West's RustConf 2018 keynote ("Using Rust for Game Development", the Eve-Online-scale server argument) is the classic motivation for strict ECS discipline: queryable, table-oriented state instead of object graphs.
- **2017, Overwatch.** Timothy Ford's GDC talk "Overwatch Gameplay Architecture and Netcode" is the best real-world case study of *behavioral* ECS at scale — components as dumb data, systems as state machines, commands as the deferred mutation channel. Directly relevant to how MCE should structure AI and player state.
- **2020s, the modern shape.** Bevy ECS (archetype tables + sparse-set hybrid, automatic parallel scheduling from query access, tick-based change detection, relations since 0.16 in the form of `ChildOf`), flecs v3 (archetype tables + sparse sets + first-class **relationships/pairs** + observers + prefab inheritance via `IsA`), EnTT v3 (sparse-set pools, grouping for iteration, signals). Unity DOTS ships the archetype model commercially but its ergonomic friction is itself a cautionary data point.

The consensus the field converged on: **entities are ids, components are columnar data in tables, systems are stateless functions over queries, structural change is deferred through command buffers, and reactions go through either buffered events or hooks.** Everything below picks concrete points on that consensus.

---

## 3. Storage deep-dive: archetype vs sparse-set vs bitset

This is the single most consequential ECS decision. The three viable models:

### 3.1 Sparse-set (EnTT's model)

One pool per component type: a **dense array** of components (plus a parallel dense array of entities) and a **sparse array** mapping `entity → dense index`. An entity is "in the pool" if `sparse[e.index]` is valid and the dense slot's back-reference agrees.

- **Add/remove:** O(1) swap-remove into/out of the dense array. Adding a component does **not** move the entity's other components. Structural change is essentially free.
- **Single-entity lookup:** O(1) via the sparse array — the fastest of the three models for random access.
- **Iteration:** pick the smallest pool in the query, walk its dense array, and probe the other pools per entity via sparse lookup. Each probe touches a different cache line region per component type. This is the model's weakness: iteration is a "gather" pattern, not a linear scan. EnTT mitigates it with **groups** (sorting shared entities contiguously across pools at a runtime cost when a group is created) and its `view::each` fast paths.
- **Memory:** proportional to live components; the sparse array costs ~`4 bytes × entityCapacity` per pool.

### 3.2 Archetype tables (Unity DOTS, flecs, Bevy)

Every entity whose component-type *set* is identical lives as a **row in one archetype table**, with each component stored as a contiguous column. An archetype graph (edges labeled by component add/remove) routes structural changes to the neighboring table.

- **Iteration:** the gold standard. A query over `{Transform, Velocity}` walks tables whose component bitset matches and streams the columns linearly — this is the hardware-perfect access pattern (Nystrom's Object Pool + data-oriented design taken to completion; exactly what the Bitsquid series argues for).
- **Add/remove:** O(row size) — the entity's *entire* row is copied from the old table to the new one (swap-remove from the old, append to the new). Spawning and destroying entities mid-gameplay is therefore not free; a wave of structural churn (bullet spawn storm) moves a lot of bytes. This is the standard objection, and the standard answer is: **defer structural changes to sync points** (command buffers, §5), where they amortize.
- **Single-entity lookup:** needs a side index (entity id → {table, row}). Bevy keeps this in a sparse set of `EntityLocation`s; flecs keeps an entity index. Lookup is two indirections instead of one.
- **Memory:** fragmenting into many archetypes has a cost. If a game uses, say, 200 distinct component combinations with 30 components each, average table occupancy drops and memory blows up. Mitigation is design discipline: keep the component vocabulary small, avoid per-enemy one-off components (see §11.4), and tag sparsely. Unity DOTS mitigates with **chunked tables** (fixed 16 KiB chunks); Bevy uses 64 KiB table components.

### 3.3 Bitset ("boolean" / "component bitmask") systems

The older OOP-adjacent model (the one in many blog tutorials): each entity is a row of fixed-size storage with a bitmask of which components are present. Simple, predictable, but every entity pays full storage for all components and iteration degenerates to scanning all entities. Nobody ships this at scale anymore; it is listed only because it is the easy first implementation and a common design trap. **Rejected for MCE.**

### 3.4 Performance model, honestly

For MCE's actual target — small complete games, thousands of entities, not millions:

| Operation | Sparse-set (EnTT) | Archetype | Winner |
|---|---|---|---|
| Linear iteration over hot query | good (gathers), great with groups | **best (linear columns)** | Archetype |
| `entity.get<T>()` random access | **best (one indirection)** | two indirections | Sparse-set |
| Add/remove component (hot path) | **best (O(1))** | O(row) copy | Sparse-set |
| Spawn/destroy storms | **best** | O(row) copy per op | Sparse-set |
| Cache behavior on a full-frame system sweep | good | **best** | Archetype |
| Determinism of iteration order | stable per pool | stable per table (given same op order) | tie |

The honest summary: **at MCE's scale, either model is fast enough, and EnTT's sparse-set would ship a 2D game faster.** The archetype model is chosen anyway (see §13) because iteration is the dominant access pattern for a game loop, because it is the model the commercial field (Unity DOTS, flecs, Bevy) standardized on and therefore the literature to study, and because columnar tables compose with the engine's other commitments (deterministic iteration order, reflection-driven batch serialization of scene files, zero-alloc queries). MCE should also steal sparse-set thinking where it wins: the **entity-location index and the hierarchy/relationship indexes are sparse sets**, which is precisely the hybrid flecs 3 and Bevy converged on.

### 3.5 Table sizing decision

flecs uses dynamically growing tables; Bevy/Unity DOTS use fixed-size chunks. **MCE commits to dynamic tables first** (columns are `std::vector`-like growable arrays; rows move on add/remove, which is already the model) **with chunking as a recorded optimization, not a v1 feature.** Stable pointers are not required — the binding constraint forbids raw pointers across frames anyway (generational handles only). Chunking buys cache-defragmentation and cheap row *reservation*, not correctness; it can be added behind the same table API.

---

## 4. Systems, scheduling, and the frame

### 4.1 What a system is

A system is a stateless (or explicitly-singleton-stateful) function that runs a query and mutates data. Reference practice: Bevy `fn movement(query: Query<...>)`; flecs systems with `.kind(Phase)`; EnTT leaves it to you (views + your own loop) — a genuine gap in EnTT as a *gameplay framework* rather than a storage library.

### 4.2 Scheduling models

- **Explicit ordered lists** (Unity DOTS `SystemGroup`, classic engines): you hand-order everything. Predictable, debuggable, deterministic — and tedious.
- **Automatic parallel scheduling** (Bevy): each system declares its query access as reads/writes; the scheduler builds a dependency graph from access conflicts and runs independent systems in parallel. Elegant, but the parallelism is opaque when it misfires, and debugging a race in an auto-scheduler is genuinely hard (Bevy added `ambiguous_with` escape hatches for exactly this reason).
- **flecs pipelines**: systems are ordered into named phases; multi-threading is a mode where phase-member systems with disjoint query overlap run in parallel. Middle ground.

**MCE decision:** explicit **phase pipeline** as the only v1 model. Named phases (`Startup`, `FixedUpdate.Pre/Physics/Post`, `Render.Pre/Submit`, `Editor`) each contain an explicitly ordered system list — exactly Overwatch's model and the fixed-timestep-friendly shape. Automatic parallelism (deriving `read/write` access sets from query templates and running phase members on the job system when disjoint) is a **flagged M3+ experiment**, off by default, because determinism of the fixed-step sim is a binding test target and auto-parallel scheduling silently changes system order. Single-threaded first, parallelizable shape second.

### 4.3 The frame loop (binding)

Fixed-timestep simulation per Glenn Fiedler's *Fix Your Timestep* (Gaffer On Games): accumulator at 60 Hz, `alpha`-interpolated render transforms, spiral-of-death clamp (max 5 steps/frame then drop time). Systems are partitioned **by phase, not by choice**: gameplay/physics/AI run in `FixedUpdate`; input sampling, camera lerp, render extraction, and UI run per-render-frame. Change ticks (§7) make interpolation cheap: only entities whose `Transform` changed since last render tick get re-interpolated. This is a gameplay-architecture constraint as much as a loop constraint — every system author must know which phase they live in, and the API makes the wrong thing hard (there is simply no per-render-frame "gameplay update" to register into).

---

## 5. Structural changes and command buffers

The classic ECS bug: a system iterating `{A, B}` calls `world.add<C>(e)` on an entity, invalidating the table/row the iterator points at. All three reference ECS solve it with **deferral**:

- **flecs:** defers commands automatically per-stage; merged at sync points.
- **Bevy:** `Commands` queue, applied at explicit sync points between systems.
- **EnTT:** nothing built-in — a well-known sharp edge; you hand-roll command buffers (the docs recommend deferral patterns; community wrappers exist).

**MCE decision:** per-phase **command buffers** (`Commands`), applied at sync points at phase ends. The command buffer records `spawn/destroy/add/remove/set` as POD records into a **frame-linear allocator** (binding: zero hot-path heap allocation), and application is a batch: group by archetype transition, execute table moves in bulk. This matches Overwatch's architecture (their talk explicitly credits deferred commands for stable iteration) and gives two free wins: bulk application makes structural storms cheaper than naive per-entity adds, and **determinism holds because application order is recorded order.** Rule for users: a system's query may not be invalidated under it; if you need to see your own structural change immediately, you split into two systems across a sync point — the pipeline makes that visible instead of making it a race.

## 6. Events, observers, hooks

Three distinct mechanisms get conflated in most ECS write-ups; MCE needs all three, separated:

1. **Buffered event queues** — *something happened; readers react later.* Double-buffered `EventQueue<T>`: writers push to the back buffer during the frame; the swap+drain happens at a sync point, so every reader sees a stable, immutable list (Nystrom's **Event Queue** pattern; Bevy's `Events<T>`/`EventReader`/`EventWriter` is the same design with a two-frame reader-generation guard so a reader registered mid-frame never misses). Use for gameplay semantics: `DamageDealt`, `EnemyDied`, `LevelLoaded`. Deterministic, replayable, network-shaped, easy to trace in the debugger.
2. **Hooks (lifecycle callbacks)** — *a component was added/removed/set.* Synchronous, per-entity, fire at command-application time. flecs calls these `on_add`/`on_remove`/`on_set` hooks; EnTT has `on_construct`/`on_destroy`/`on_update` sinks. Use for engine-owned invariants, not gameplay: initializing a sprite's GPU handle when a `Sprite` is added, releasing the physics body when `RigidBody2D` is removed. Crucially, hooks run during command application, so they are the sanctioned place to touch non-ECS subsystems (renderer/physics/audio) in response to structural change.
3. **Observers/triggers** — *react to a pattern across entities* (flecs observers: "on add of `Dead` to anything with `Enemy`"). Powerful and seductive; **rejected for v1** because observer cascades are the #1 source of non-obvious ordering bugs in flecs codebases and they destroy determinism-by-reading-the-pipeline. MCE's rule: reactions are *systems reading event queues* unless a hook is provably the only correct tool.

Anti-pattern to design against: events as a global god-queue. Events are typed, individually registered, and each has an explicit producing system and consuming systems — the editor's debug UI will render the live event flow (phase, type, count) precisely because the registry knows the graph.

---

## 7. Change detection

Per-component **change ticks** are the modern standard (Bevy: `Added<T>`/`Changed<T>` filters backed by per-cell u32 ticks and a global frame counter; flecs: dirty-state per table/pair; Unity DOTS: per-chunk change versions). MCE commits to the Bevy-shaped design, simplified:

- A global u32 **frame tick** counter (fixed-step count, not wall time).
- Every component column carries a parallel **u32 changed-tick column** (per entity row, not per table — per-table granularity is cheaper but wrong for transforms where one entity moving shouldn't dirty the table). Memory cost is 4 bytes per component instance — ~6% on a 64-byte `WorldTransform`, trivial on a 12-byte `Velocity`.
- `set<T>()` bumps the tick; raw `mut` access obtained through a `Write<T>` query item does NOT auto-bump (documented footgun; the query item can carry an explicit `touch()` helper, mirroring Bevy's `DetectChanges` API: `is_added()`, `is_changed()`).
- Tick wraparound: u32 at 60 Hz overflows after ~2.2 years of continuous fixed steps; handle it the Bevy way — track "last run tick" per system and treat ticks as a rotating window with a same-frame sentinel, so the wrap is a correctness footnote, not a bug.

Killer app at M1: **transform hierarchy propagation runs only on dirty entities** (§8), and **render interpolation touches only entities whose transform changed since the previous render tick.** Secondary app: the editor inspector highlights changed components. All three fall out of one mechanism.

---

## 8. Hierarchies and relationships

Three reference approaches:

- **Unity/Godot:** the scene graph *is* the object model — every object has exactly one parent. Simple, but conflates "logical grouping" with "transform parenting" and forces hierarchy overhead on everything (Godot's `Node` is the extreme case).
- **flecs relationships (v3):** `(ChildOf, parent)` as a first-class *pair* — a component whose id includes another entity. Tables fragment along the parent axis, which makes "iterate all children of X" a table-aligned operation, and query **traversal** (`up(ChildOf)`) plus **cascade ordering** (process parents before children regardless of table order) make hierarchies query-level features. This is the most powerful design and also the most machinery.
- **Bevy relations (0.16+):** `ChildOf` as an ordinary component plus a `Children` component on the parent, with transform propagation as a system that walks dirty subtrees in depth order.

**MCE decision:** Bevy-shaped **`ChildOf{parent: Entity}`** component, kept simple and honest:

- One parent per entity (a real tree — the overwhelmingly common case; if a DAG need appears, that is a prefab/instancing problem, not a hierarchy problem, see §9).
- The parent's `Children` list is maintained by a hook, not stored by users (avoiding Bevy's dual-component desync footguns by construction — users can never write `Children` directly).
- **Transform propagation** runs in `FixedUpdate.Post`, iterating entities with `Changed<LocalTransform>` in depth order (a per-tree topological walk seeded from roots with dirty descendants), writing `WorldTransform` and bumping its tick. Nystrom's **Dirty Flag** pattern, table-fed.
- No flecs-style generic pair/relationship language in v1. The pair model is the right *generalization*, but MCE can ship three concrete relationships (`ChildOf`, `OwnerOf`, physics `JointOf`) as plain components and add pair machinery only if a fourth concrete case appears. Record that as the trigger condition, mirroring how the stack panel treats flecs' `isa` (adopt as phase-2 if needed).

---

## 9. Prefabs

Survey of the options:

- **flecs `IsA` inheritance:** instances *share* the prefab's table; overrides are per-entity components shadowing the inherited value. Runtime inheritance — powerful (edit prefab, all instances change live) but complex (override semantics, traversal in every query) and it couples editor semantics to ECS internals.
- **Unity-style runtime instancing:** prefab = serialized template; instantiate = deep-copy into live entities with id remapping; modifications after spawn are instance-local. Editor-time property *overrides* are stored per instance and re-applied on reimport.
- **Cooked template assets (Unreal-ish, data-oriented):** the prefab is compiled at cook time into the `.mce` container as a template archetype row-set; instantiation is a table append + handle remap. Same as option 2 with the parse cost moved offline (binding asset policy says the runtime never parses JSON — so a prefab *must* be cooked anyway).

**MCE decision:** cooked template assets. A prefab is authored in the editor as an entity sub-tree, serialized through the reflection system into the editor's text format, and **cooked into a `.mce` template chunk** (component columns in archetype order + a reference table mapping internal template ids → placeholder handles). Instantiation is: allocate N entities, bulk-copy the template rows into their archetype tables, remap intra-prefab handles (including `ChildOf` targets and any `Entity`-typed fields), then apply the instance's **override list** (component diffs recorded at edit time). Nested prefabs are flattened at cook time with the override stack applied in order — no runtime inheritance graph exists. Live-reload of prefab *definitions* is an editor-time feature (re-instantiate or diff-apply), explicitly not runtime inheritance. Deferred, with a trigger condition: if a game needs runtime-shared mutable prefab state (e.g., "open all doors of class X"), that is an explicit `PrefabTag` component + system problem, not an `IsA` mechanism.

---

## 10. Scenes and composition

Binding constraint: **the editor owns the canonical scene; the runtime never mutates scenes** (Unity/Godot model). Concretely:

- The **editor scene document** is the canonical representation: a list of entities with reflected component payloads, text-serialized (editor-side; JSON via nlohmann is cook-time-only so it never ships) with asset references as stable GUID paths, and prefab instances stored as (template ref + overrides).
- **Cooking** bakes the scene into a `.mce` *scene chunk*: component columns keyed by component id, entity id remapping table, prefab instances expanded with overrides applied. Runtime load is: mmap the chunk, bulk-attach rows to archetypes, resolve asset handles. No parsing, no per-entity JSON.
- **Sub-scenes/streaming** are out of scope until a game needs them; the chunked container format leaves room for multiple scene chunks per world, which is the natural seam.
- Singletons: a small closed set of world-level resources (`Time`, `Input`, `ActiveCamera`, physics world, mixer) live outside the ECS as explicit world state — Bevy's `Resource` model, not "a lone entity with one component" (flecs supports the latter; it makes singleton access asymmetric and invites entity-id misuse).

---

## 11. Where ECS alone is NOT enough

The honest section. An ECS is a data store plus an iteration model. It is *not* a behavior model, a sequencing model, or a UI model. Each gap and the committed answer:

### 11.1 State machines (player control, AI)

The mismatch: states have *transitions with guards and side effects*, and ECS naturally flattens everything into "components + systems," which forces state into enums and transitions into `if` cascades inside giant systems — the Overwatch talk shows this is survivable at scale, but it is not ergonomic.

**MCE decision — enum-state components + per-state systems, with a small FSM helper (no behavior trees in v1):**

- State is data: `MCE_COMPONENT(EnemyBrain, EnemyState state; float stateTime; Entity target;)` — the FSM's *data* lives in the ECS so it is inspectable, serializable, save-game-able, and tick-tracked for free.
- Behavior per state is a system scoped by a query filter on the state component's value (`With<State<EnemyState::Chase>>`), so state execution stays a table-aligned query and transitions are explicit writes that bump change ticks. `stateTime` is reset by a hook on state change, giving "time in state" uniformly.
- A tiny `fsm.h` helper (transition table as data: `from, event/guard → to, actions`) covers the 20% of cases where enum-cascades get ugly — timed states, entry/exit actions, hierarchical substates. **HFSM as data** is the phase-2 upgrade path; behavior trees and GOAP are explicitly not planned (they are AI-content tools, and MCE's games don't need them; a utility-scoring helper is the only plausible future addition).

This is deliberately the Overwatch pattern at indie scale: components are facts, systems are the finite-state logic, events carry the interesting transitions.

### 11.2 Cutscenes, scripted sequences, timelines

ECS systems answer "what happens every tick," not "what happens *in order over 30 seconds*." Sequenced content needs a different shape. References: Unity Timeline's track/clip model, Naughty Dog-style scripted sequences, and Nystrom's **Update Method** + coroutine patterns.

**MCE decision — two tools, both data, both ECS-adjacent:**

1. **A timeline system for authored cutscenes (M4, editor-visible):** a `TimelineAsset` (cooked to `.mce`) with named tracks (transform keys, animation clips, audio, camera cuts, event triggers) targeting entities by stable handle; a `TimelinePlayer` component binds an asset to a world-space instantiation. Evaluation is one system in `FixedUpdate` (or a render-phase system when scrubbing in the editor). This is the same data shape the editor's curve/gizmo tooling will need anyway.
2. **A lightweight action-list ("script sequence") for gameplay scripting:** a `Sequence` component holding an index into a script-defined list of steps (`wait`, `move_to`, `play_anim`, `emit`, `call`); a single `SequenceRunner` system advances active sequences. This is the coroutines-without-coroutines pattern: no stackful machinery, trivially serializable, deterministic. C++20 coroutines were evaluated and rejected for v1 — coroutine frames allocate and do not serialize, both of which violate binding constraints; the action-list is the discipline that survives a save/load.

Dialogue, quests, and inventory follow the same principle: they are **data plus a runner system**, not ECS magic.

### 11.3 UI

Retained-mode hierarchies with layout, focus, and input routing have almost no query-shaped workload; folding them into ECS produces the worst of both. Binding constraint already defers shipped-game UI. MCE commits: **UI is a separate retained-mode system with its own tree, its own input pass, and an explicit bridge into the ECS** (UI elements observe queries/events; ECS never reaches into UI). ImGui remains editor/debug-only by binding. Design doc for the runtime UI system is a phase-2 deliverable — the only requirement pinned now is that gameplay code must not call ImGui (CI-checkable).

### 11.4 The "component soup" failure mode (a design-level gap)

The most common real-world ECS failure is vocabulary explosion: 40 one-off flag components (`IsStunned`, `CanDoubleJump`, `HasSeenPlayer`) that fragment archetypes and turn logic into tag archaeology. The countermeasures to build into MCE's culture from day one: prefer **data over tags** (`Health`/`Stun` timer beats `IsStunned`), prefer **relationship components with payloads** (`Targeting{Entity target; float lockTime;}` beats `HasTarget`), and give the editor a component-usage browser so the soup is visible. Archetype fragmentation is the failure mode to watch in Tracy's table-count graph.

---

## 12. Honest comparison against simpler models

For balance, the case for NOT doing an archetype ECS at all, and why it loses anyway — argued at full strength:

- **Deep OOP inheritance (`GameObject` hierarchies).** Brittle under orthogonal feature combinations (`FlyingSwimmingShootingEnemy`), but *genuinely fine* for tiny scope, and simpler to debug for a beginner. Verdict: the literature has been over this for 15 years (Nystrom's Component chapter exists because inheritance lost); MCE's from-scratch mandate makes re-learning this the wrong use of budget.
- **Unity-style GameObject/Component (reference-based composition, virtual `Update`).** The pragmatic industry default for 15+ years. Honest strengths: superb editor ergonomics, inspector-driven iteration, zero ECS ceremony; honest costs: per-component heap objects and virtual dispatch (irrelevant below ~10k entities), cache-hostile scatter access (relevant for bullet storms and large crowds), and serialization/reflection burden identical to MCE's. A reference-based component model would ship M1's 2D game marginally faster. But it forfeits the *entire learning objective* of the project's core, and it retro-fits badly: Unity built DOTS because the model genuinely caps out, and retrofitting data-orientation onto a reference model (Unity's actual multi-year struggle) is harder than starting there.
- **Godot-style Node tree.** One hierarchy for everything (scene = UI = audio = gameplay) is seductively uniform and great for tools; the costs are conflation (§8), string-typed signals, per-node overhead, and a scene tree that resists data-parallelism. MCE keeps the tree where it belongs — transforms and UI — and keeps logic tabular.
- **"Plain structs + free functions" (data-oriented, no ECS).** The underrated option: for a single small game, `std::vector<Enemy>` plus loops beats any ECS in simplicity. Honest assessment: **for a game, yes; for an engine, no** — MCE is building the reusable substrate, and hand-rolled scenes/serialization/editor-inspection over ad-hoc vectors is exactly the ad-hoc-field-list problem the binding reflection rule forbids. The ECS *is* MCE's reflection-driven, serializable, editor-facing object model; that is its justification, more than performance.
- **Where OOP-style models still win, honestly:** single-digit-thousands of entities, tooling-first workflows, heavy designer iteration via inspectors, and teams where a virtual call per object costs less than teaching data-oriented thinking. MCE accepts a real upfront complexity tax against those cases; the payoff is iteration speed on tables, determinism, and an editor that inspects *anything* through reflection with zero per-type editor code.

---

## 13. Recommendation for MCE

### 13.1 Committed decisions

| # | Decision | Commitment |
|---|---|---|
| 1 | Storage | **Custom archetype ECS** (tables per component-set, archetype graph edges, dynamic tables; chunking = recorded optimization). flecs/EnTT/Bevy studied, never linked. |
| 2 | Entity id | `Entity { u32 index; u32 generation; }` — generational handle, 8 bytes, trivially copyable, the *only* thing gameplay code holds across frames (binding). |
| 3 | Lookup index | Sparse-set **entity-location index** (entity → table+row) and hierarchy indexes; the flecs-3/Bevy hybrid shape. |
| 4 | Component limit | 256 component types (bitset-per-table matching, u32×8). Room for years of games; keeps matching trivial. |
| 5 | Systems | Plain structs with an `update(Frame&)`, registered into **explicit named phases** with explicit intra-phase order. Auto-parallel = M3+ experiment, off by default (determinism first). |
| 6 | Frame model | Fixed 60 Hz sim + interpolated render (*Fix Your Timestep*); phases split `FixedUpdate`/`Render`; `Frame` exposes `dt()` (fixed) vs `alpha`/`renderDt`. |
| 7 | Structural change | Per-phase **command buffers** over frame-linear memory, applied at sync points; hooks (`on_add/on_remove/on_set`) run at application time; user queries are never invalidated mid-iteration. |
| 8 | Events | **Double-buffered `EventQueue<T>`** for gameplay events; hooks for subsystem invariants; no observer/trigger cascades in v1. |
| 9 | Change detection | Per-row u32 **changed tick** column per component; `set()` bumps; `is_added()/is_changed()` query filters; drives dirty-transform propagation and render interpolation. |
| 10 | Hierarchy | `ChildOf{parent}` component; `Children` maintained by hook; depth-ordered dirty propagation writes `WorldTransform`; generic relationship *pairs* deferred with a named trigger (4th concrete relationship type). |
| 11 | Prefabs | Cook-time **template chunks** in `.mce`; instantiate = bulk row copy + handle remap + override list; nested prefabs flattened at cook. No runtime inheritance. |
| 12 | Scenes | Editor-owned text scene document → cooked `.mce` scene chunk (columns + remap table); runtime never parses text (binding). Closed set of world **resources** for singletons. |
| 13 | Behavior | Enum-state components + per-state systems + small `fsm.h` helper; **sequences = data-driven action lists**; **timelines = cooked track/clip assets** (M4); no behavior trees/GOAP/coroutines in v1. |
| 14 | UI boundary | Gameplay never calls ImGui; runtime UI is a separate retained system (phase-2 doc), bridged via events only. |
| 15 | Errors | No exceptions/RTTI (binding); asset/IO paths return `Result<T, Error>`; programmer errors assert; gameplay-visible failures are components/events (`FailedToPath{}`), not exceptions. |

### 13.2 What user-authored behavior looks like

The contract for a gameplay programmer on MCE — components are reflected plain structs; systems are registered functions; commands/events are explicit:

```cpp
// ---- components: reflected plain data (inspector + serialization come free)
MCE_COMPONENT(Velocity, Vec3 v;)
MCE_COMPONENT(Health, float hp; float max;)
MCE_COMPONENT(LocalTransform, Vec3 pos; Quat rot; Vec3 scale = Vec3::one();)

enum class EnemyState : uint8_t { Idle, Patrol, Chase, Attack };

MCE_COMPONENT(EnemyBrain, EnemyState state = EnemyState::Idle;
                          float   stateTime = 0.0f;
                          Entity  target;)          // generational handle

// ---- a system: registered into a named phase, explicit order
MCE_SYSTEM(EnemyThink, mce::Phase::FixedUpdate, after(input_export_sys))
void enemy_think(Frame& f) {
    const InputState& in = f.resource<InputState>();

    f.query<EnemyBrain&, const LocalTransform&>()
     .each([&](Entity e, EnemyBrain& b, const LocalTransform& t) {
        b.stateTime += f.dt();

        switch (b.state) {
        case EnemyState::Idle:
            if (b.stateTime > 2.0f) { b.state = EnemyState::Patrol; b.stateTime = 0; }
            break;
        case EnemyState::Patrol:
            if (can_see(t.pos, in.playerPos)) {             // transition guard
                b.state  = EnemyState::Chase; b.stateTime = 0;
                f.events().push(EnemyAlerted{ .enemy = e }); // buffered event
            }
            break;
        case EnemyState::Chase:
            if (b.target.is_null()) { b.state = EnemyState::Idle; b.stateTime = 0; }
            break;
        case EnemyState::Attack: break;
        }
    });
}

// ---- reads/write via query access; spawn through commands (deferred)
MCE_SYSTEM(EnemyMove, mce::Phase::FixedUpdate, after(enemy_think_sys))
void enemy_move(Frame& f) {
    f.query<Velocity&, const EnemyBrain&, const LocalTransform&>()
     .each([&](Entity, Velocity& v, const EnemyBrain& b, const LocalTransform& t) {
        if (b.state != EnemyState::Chase) return;
        v.v = seek(t.pos, b.target, /*speed*/ 4.5f);   // writes bump changed-ticks
    });
}

// ---- runtime wiring: explicit, ordered, deterministic (MCE_SYSTEM auto-registers
// into the phase; registration order within the phase is the declared order)
void gameplay_module(World& w) {
    w.register_components<Health, LocalTransform, EnemyBrain, Velocity>();
    w.register_system<enemy_think>();
    w.register_system<enemy_move>();
}
```

Properties this sketch buys by construction: the inspector shows `EnemyBrain` (reflection), the state machine is save-game-able (it is data), `enemy_move` never sees stale rows (command-buffer sync points), transitions are traceable (event queue), and Tracy zones wrap each system automatically.

### 13.3 Milestone mapping

- **M1:** ECS core — archetypes, tables, queries, entity-location sparse set, phases, command buffers, event queues, reflection-bounded components, scene save/load, inspector. (This is the biggest single M1 item; the renderer competes for the same weeks — see the stack panel's death-spiral risk.)
- **M2:** change detection (ticks + filters), `ChildOf` hierarchy + dirty transform propagation, interpolation-driven rendering.
- **M3:** prefab cook/instantiate + overrides, hooks for physics/audio subsystem bridge, optional auto-parallel experiment behind a flag.
- **M4:** timeline/sequencer assets + editor curve tooling; runtime-UI design doc (separate system).

### 13.4 Risks

1. **Scope creep in the ECS itself** (relationships, observers, scripting hooks) — mitigated by the explicit trigger conditions in decisions 10/11/8: features ship when a *fourth concrete use case* appears, not when the reference implementations look incomplete.
2. **Archetype fragmentation from component soup** (§11.4) — visible in Tracy's table metrics from day one.
3. **Command-buffer discipline** is a learning curve for the author-as-user: deferred mutations feel indirect at first; the mitigations are documentation of the sync-point model and an editor "deferred ops" debug view.
4. **Determinism regressions** from any auto-parallel experiment — contained by keeping the feature flag-gated and the fixed-step determinism test in CI (binding test policy).

---

## 14. References (canonical sources named in text)

- Skypjack, *ECS back to basics* blog series (2019) — archetype vs sparse-set mechanics; EnTT design notes.
- flecs v3 documentation — relationship pairs, `IsA`/prefabs, observers/hooks, pipelines, table internals.
- Bevy ECS docs and source — archetype tables + sparse-set locations, change ticks, `Commands`, relations (`ChildOf`, 0.16), auto-parallel scheduling.
- EnTT documentation — sparse-set pools, groups, signals (`on_construct`/`on_destroy`).
- Timothy Ford, "Overwatch Gameplay Architecture and Netcode," GDC 2017 — ECS behavioral patterns, deferred commands, state-machine systems.
- Catherine West, "Using Rust for Game Development," RustConf 2018 — the scale/determinism case for ECS.
- Bitsquid/Stingray blog, "Building a Data-Oriented Entity System" series (Niklas Frykholm et al.) — columnar layout, table sizing, tooling.
- Bob Nystrom, *Game Programming Patterns* — Component, Update Method, Event Queue, Dirty Flag, Object Pool chapters.
- Glenn Fiedler, "Fix Your Timestep," Gaffer On Games — fixed-step + interpolation.
- Jason Gregory, *Game Engine Architecture* (3rd ed.), ch. 14 (object models) — the OOP-hierarchy history this research reacts to.
- Unity DOTS documentation — chunked archetype storage, system groups, the ergonomic-friction cautionary record.
