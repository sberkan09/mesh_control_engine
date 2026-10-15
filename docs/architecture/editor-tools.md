# Architecture: Editor & Tools (`editor-tools`)

**Project:** Mesh Control Engine (MCE) · **Subsystem:** `editor-tools` · **Date:** 2026-10-06
**Status:** Implemented (M2–M8); as built in `docs/engine/editor.md`. This file is the design-time record. *ImGuizmo was
never taken: the gizmo is hand-rolled (ADR-0025, then ADR-0030 for rotate/scale), so every ImGuizmo mention below is
superseded.*
**Inputs:** `docs/REQUIREMENTS.md` (authoritative), `docs/ARCHITECTURE.md` (integration authority — fixes F7, F8, F10, F12 and ADR-0008/0015 applied here), `docs/research/existing-engines.md`, `docs/research/audio-input-ui.md` (§4 editor UI, §4.3 target wall), `docs/research/ecs-gameplay.md` (scene document, commands, events), `docs/research/platform-engineering.md` (threads, allocators, logging).
**Binding context honored:** Dear ImGui (docking branch, multi-viewport **off**), ImGuizmo via one-page ADR, editor owns the canonical scene and the runtime never mutates it, all scene/inspector/editor serialization flows through the hand-rolled reflection system, ImGui is editor/debug-only and CI-walled out of the engine library set (`mce_engine.dll` — fix F8 naming), fixed 60 Hz sim, generational handles, zero heap in hot paths, `Result<T,Error>` for fallible I/O, no `#ifdef MCE_EDITOR` in runtime code.

---

## 1. Responsibilities & non-responsibilities

### 1.1 Responsibilities

The editor is **an application, not a mode of the runtime** (the Godot self-hosting model — see `existing-engines.md` §2.3/§4 decision 7): a separate executable, `mce_editor.exe`, that is the *most demanding client* of every runtime subsystem's public API. It owns:

1. **The canonical scene document.** The editor process is the only writer of scene/prefab source data (`FR-ENT-060`). It owns the on-disk text scene format, autosave/crash recovery, dirty tracking, and save/load. The runtime consumes cooked `.mce` scene chunks and *cannot* write back — enforced by the CMake target wall, not by convention.
2. **Authoring operations**: entity CRUD, drag-drop, copy/paste, multi-select, gizmo transforms, prefab instantiate/override (v1.0), scene save/load — all funneled through a single mutation path that feeds the undo/redo stack and mirrors into a live preview world.
3. **The inspection surface**: the reflection-driven inspector (`FR-EDT-020` — every registered component inspectable with zero per-type editor code), the scene hierarchy panel, the asset browser, and the debug views (console, stats, event-flow monitor, ECS metrics, render debug overlays).
4. **The play-in-editor loop** (v0.5) and the **run-in-player** button (MVP): building a transient runtime world from the current scene document, driving it with fixed-step ticks, pause/step/slow-mo, and discarding all play-mode state on stop (`FR-EDT-060`).
5. **Pipeline integration**: the cook button and error-list panel (surfacing cook/shader/import errors with jump-to-asset, `FR-EDT-070`), shader hot-reload surfacing (`FR-AST-060`), asset hot-reload (`FR-AST-090`).
6. **The editor frame itself**: SDL3 window + ImGui docking shell, the editor main loop, input arbitration between ImGui and the game viewport, and the game-view render-target widget with DPI correctness (`FR-R2D-080`).

### 1.2 Non-responsibilities

- **No runtime code.** `mce_editor.exe` links `mce_engine.dll` (the ImGui-free engine library set — ARCHITECTURE fix F8), and nothing in that library set can see ImGui headers: the wall is a link error, not a convention (target-wall restatement in §6 here; `audio-input-ui.md` §4.3). Shipped-game UI is `mce::ui`'s problem, never ImGui (`FR-UI-048`).
- **No scene mutation in the runtime.** The editor never writes into a running play-world's scene representation except through the runtime's public gameplay API (component writes via the same ECS API a game system uses). The canonical document is only written by editor *commands*, on the editor thread.
- **No cook logic, no GUID policy, no format codecs.** The editor *drives* the `assets` subsystem's cook CLI/library and reads its GUID registry and error list; it implements none of them.
- **No ECS internals.** The editor is a client of `ecs-gameplay`'s public `World`/query/event API; it does not reach into archetype tables (it reads the metrics/debug views the ECS exposes for this purpose).
- **No DCC features** (mesh editing, texture painting, animation authoring — N5), **no visual scripting** (FR-EDT-150), **no plugin API, theming, or localization** (N8, N12).
- **No determinism machinery of its own.** Play-in-editor determinism is inherited from the runtime's fixed-step loop and the assets cook path; the editor's job is to *not break* it (see risk R3).

### 1.3 Editor-side tiering (what gets full design vs. sketch)

Per REQUIREMENTS priorities and Appendix A resolutions; milestone IDs are ROADMAP's (ARCHITECTURE §10.1 — finding F8), and the M0/M1/M2/M3 spread below follows ROADMAP's re-baseline (finding F4: reflection + inspector land with M2, editor CRUD/gizmos/viewport with M3):

| Tier | Fully designed here | Sketched only |
|---|---|---|
| **MVP (ROADMAP M0–M3)** | Docking shell + panel registry, scene document, mutation funnel, selection (shell + console at M0); reflection inspector + hierarchy (flat list) + save/load + autosave with `mce_refl` (M2); entity CRUD/copy-paste, translate gizmo, asset/entity reference pickers (Guid + type-filtered dropdown), run-in-player button, cook error panel, 2D authoring grid/snap + rect-fill (FR-EDT-120, M3 — finding 31) | — |
| **v0.5 (ROADMAP M5b–M7)** | Undo/redo command stack, asset browser + drag-drop, rotate/scale gizmos, multi-select grouped gizmo, play-in-editor with pause/step/slow-mo (M5b — finding 11), debug overlays, tilemap authoring panel (v0.5 tier, FR-EDT-121) | — |
| **v1.0 (ROADMAP M8)** | — | Prefab override UI, thumbnails, timeline/curve editor, multi-scene tabs, GPU color picking |

Cut-ladder note: rung 2 ("drop editor polish") removes undo/redo, the asset browser, and rotate/scale gizmos — the design below therefore keeps each of those as an *isolated module* (its own class, its own tests), not load-bearing for the rest of the editor, so cutting one does not tear the shell.

---

## 2. Public interface

The editor is the top of the dependency graph — nothing consumes its API at runtime. The "public interface" below is therefore (a) the seams *between the editor layer and the runtime API it consumes*, and (b) the internal module boundaries that keep the editor testable headless. Everything lives in `mce::editor` unless noted.

### 2.1 Process model (decided)

**One process at a time, two executables, one shared library wall:**

```
mce_editor.exe  ->  mce_editor_layer ->  mce_editor_core ->  mce_engine.dll
                        |                     (headless logic:      (L0–L4 umbrella — the
                        + imgui(docking)       document, undo,       ImGui-free library set;
                        + ImGuizmo(ADR)        picking, PlaySession, fix F8 naming)
                                               cook requests)
game.exe        ->  mce_engine.dll + game.dll (module)             (no ImGui anywhere downstream)
mce_devhost.exe ->  mce_devhost_layer -> mce_engine.dll + imgui    (dev-only debug harness)
                    [as built: tests/devhost/mce_devhost links only mce_engine; no ImGui, no layer]
```

The three-target split is ARCHITECTURE fix F7: `mce_editor_core` compiles headless in CI (no ImGui), `mce_editor_layer` is the only ImGui consumer, and `mce_editor.exe` is a ~20-line main. Cook requests go through `mce_asset`'s `CookService` (assets.md §7.6), reached from `mce_editor_core` — the editor drives cook, never implements it.

- **Play-in-editor is in-process** (v0.5): the editor embeds the same runtime *host* object that `game.exe`'s `main()` constructs, drives it from the editor loop, and renders its output into a texture the viewport widget displays. This is what makes Play entry ≤ 1 s (NFR-M-03) and live entity inspection possible — you cannot inspect what you spawned in a child process.
- **Run-in-player (MVP) launches `game.exe` as a child process** over a temp-cooked content pack (Appendix A, A4). It exists precisely because play-in-editor needs a proven, deterministic scene-load path first; the child-process button has no such prerequisite.
- **Separate `main`s, no `#ifdef MCE_EDITOR`** (Godot/Unity shape; `audio-input-ui.md` §4.3). The editor owns the canonical scene *because* it is a different binary that happens to link the same runtime library — not a tumor inside it (the explicit Unreal lesson, `existing-engines.md` §2.2).

### 2.2 Editor application shell

```cpp
namespace mce::editor {

struct EditorDesc {
    const char* project_path;        // folder with <project>.mceproj + assets/ (CF17)
    bool        headless;            // CI: no window, no ImGui — editor_core only
    uint32_t    target_fps;          // 0 => vsync
};

// Composition root. mce_editor.exe's main() is ~20 lines calling this.
class EditorApp {
public:
    Result<void, Error> startup(const EditorDesc& d);
    void                shutdown();

    // One editor frame: input -> panels/commands -> play ticks -> render -> ImGui.
    void run_frame();

    // --- owned services (references, not singletons; explicit wiring) ---
    SceneDocument&     document();       // the active scene document (MVP: one)
    EditorWorld&       preview();        // live ECS instance rendering the viewport
    PlaySession&       play();           // the embedded runtime world (or stopped)
    SelectionSet&      selection();
    UndoStack&         undo();
    PanelRegistry&     panels();
    ConsoleModel&      console();
    CookStatusView&    cook();           // drains the cook worker's event queue
    AssetBrowserModel& assets();         // v0.5
    EditorSettings&    settings();       // dock layout, recent files, toggles
};

Result<int, Error> editor_main(const EditorDesc& d);   // editor entry point
}
```

### 2.3 The scene document (canonical representation)

The document is **not** the ECS. It is the serialized, addressable, undo-able truth; the ECS is a *derived preview* of it. Entity identity is split deliberately:

- **`EntityId` (persistent, u64)** — lives in the document, survives save/load/rename, is what undo commands, prefab overrides, and cross-entity references serialize. Format: `u32 local_index | u32 version` per scene.
- **`ecs::Entity` (runtime generational handle)** — owned by whichever world instance (preview or play) exists right now, never serialized.

```cpp
namespace mce::editor {

using EntityId = uint64_t;   // persistent document identity

struct SceneDocument {
    uint32_t        format_version;   // versioned text format (FR-EDT-100)
    Guid            scene_guid;
    bool            dirty;

    // --- mutation funnel: the ONLY way anything changes the scene ---
    // 1. records into the UndoStack (with coalescing)   2. mutates the document
    // 3. mirrors the same write into the EditorWorld    4. marks dirty
    void            apply(const SceneCommand& cmd);

    // --- queries (const, editor-thread only) ---
    span<const EntityId>           entities() const;        // stable order
    std::string_view               name(EntityId e) const;
    bool                           has(EntityId e, ComponentId c) const;
    const void*                    get(EntityId e, ComponentId c) const;  // reflected blob
    EntityId                       parent(EntityId e) const;        // v0.5, ChildOf mirror
    span<const EntityId>           children(EntityId e) const;

    // --- I/O (fallible, never throws — binding error policy) ---
    Result<void, Error> save_as(std::filesystem::path p);   // temp-write + atomic rename
    Result<void, Error> load(std::filesystem::path p);      // version check + migrator table
                                                            // (assets.md §6: migration runs HERE at
                                                            //  editor load; cook only accepts current
                                                            //  version); ahead-of-reader => Result error
    Result<void, Error> autosave_backup();                  // crash-recovery backup (FR-EDT-100)

    // serialization goes through the reflection registry — never ad-hoc field lists
    Result<std::vector<std::byte>, Error> serialize_entity(EntityId e) const; // for undo/copy-paste
    Result<EntityId, Error>               deserialize_entity(span<const std::byte> blob);
};

// Variant of commands (POD + small serialized payloads; heap use is editor-side,
// not a hot path — std::vector is allowed in tool code per platform-engineering §2.3)
struct SceneCommand {
    enum class Kind { SetProperty, AddComponent, RemoveComponent,
                      CreateEntity, DestroyEntity, DuplicateEntity,
                      RenameEntity, SetParent, PasteEntities,
                      InstantiatePrefab /* v1.0 */ } kind;
    EntityId  target;
    ComponentId component;
    FieldId     field;                    // reflection field id (inspector/gizmo writes)
    std::vector<std::byte> old_value;     // serialized via reflection (undo payload)
    std::vector<std::byte> new_value;
    // ... creation payloads carry name + component set + serialized rows
};
}
```

### 2.4 Undo/redo (v0.5, Must at v1.0 — FR-EDT-040)

A **command stack over reflection-serialized property diffs**, not ImGui state — the same stance `audio-input-ui.md` §4.1 commits to. Every entry stores enough to invert itself; nothing stores live pointers into the document.

```cpp
namespace mce::editor {

class UndoStack {
public:
    void push(std::unique_ptr<ISceneCmd> cmd);  // clears redo; evicts oldest beyond bounds
    bool undo(SceneDocument& doc);
    bool redo(SceneDocument& doc);

    // drag coalescing: a continuous inspector drag or gizmo drag is ONE command.
    // open() begins a session; subsequent pushes with the same (entity, component,
    // field) key merge; commit() on mouse-release/dwell>500ms finalizes it.
    void begin_coalesce(CoalesceKey k);
    void commit_coalesce();

    static constexpr size_t kMaxEntries = 256;
    static constexpr size_t kMaxBytes   = 16 << 20;   // serialized-blob budget
    size_t entries() const;
};
}
```

### 2.5 Selection & gizmos

```cpp
namespace mce::editor {

class SelectionSet {
    std::vector<EntityId> sel_;          // ordered; [0] is the anchor
public:
    void set(EntityId e);                // click
    void toggle(EntityId e);             // ctrl-click
    void range_to(EntityId e);           // shift-click (hierarchy order)
    void clear();
    span<const EntityId> items() const;
    EntityId anchor() const;             // gizmo pivot / inspector subject
    // Manual gate (FR-EDT-050): multi-select gizmo transforms the group about
    // the anchor's pivot; all writes go through SceneCommand (one undo entry).
};

enum class GizmoOp { Translate, Rotate, Scale };   // MVP: Translate only (FR-EDT-030)
enum class GizmoSpace { World, Local };

struct GizmoFrameResult { bool active; bool changed; };

// v0.5+ full service; MVP: op = Translate, space = World.
// While dragging: writes a *preview* LocalTransform directly into the EditorWorld
// every frame (children follow via ECS dirty propagation — FR-EDT-030 acceptance).
// On release: ONE SetPropertyCmd per affected entity (old = drag-start, new = final).
GizmoFrameResult run_gizmo(DrawCtx& ui, GizmoOp op, GizmoSpace space,
                           EditorApp& app, float snap_step /* 0 = off */);
}
```

### 2.6 Reflection-driven inspector (FR-EDT-020)

The inspector has **zero per-type code**. It walks the `core` reflection registry's field list and dispatches widgets by field trait. The draw function is written against a tiny `DrawCtx` abstraction (not raw ImGui) so the *round-trip logic* is testable headless in CI:

```cpp
namespace mce::editor {

// The editor's only ImGui dependency seam: editor_core is compiled headless in CI.
class DrawCtx {
public:
    bool  drag_float(const char* label, float* v, float speed, float2 range, const char* tooltip);
    bool  drag_int(...);        bool  checkbox(const char*, bool*);
    bool  combo_enum(const char*, uint32_t* v, span<const EnumValue> values);
    bool  color_edit(const char*, float* rgba);
    bool  text_edit(const char*, std::string*);
    bool  entity_pick(const char*, EntityId* e, const EditorApp&);   // scene dropdown/search
    bool  asset_pick (const char*, Guid* g, AssetType);
    // asset_pick at MVP (finding F6, FR-EDT-020): Guid text field + type-filtered
    // dropdown populated from the GUID registry scan (the registry exists for
    // sidecars since M2 — no browser dependency). v0.5 upgrade: browser-linked
    // drag-drop picker once the asset browser exists (FR-EDT-110).
    void  read_only_text(const char* label, std::string_view value);
};

// One entry point; ui-hints (range, tooltip, read-only) come from reflection traits
// (research/ecs-gameplay §"reflection designed inspector-first"; O3DE's warning honored).
void draw_inspector(DrawCtx& ui, EditorApp& app);
void draw_hierarchy (DrawCtx& ui, EditorApp& app);   // ImGui tree over ChildOf (v0.5);
                                                     // flat clippered list at MVP
}
```

Every widget commit produces `SceneCommand{SetProperty}` — the inspector **never** writes the document or the preview world directly. This one rule is what makes undo, mirroring, and the FR-EDT-020 round-trip test fall out by construction.

### 2.7 Panels & the viewport

```cpp
namespace mce::editor {

class IEditorPanel {
public:
    virtual const char* name() const = 0;
    virtual void draw(DrawCtx& ui, EditorApp& app) = 0;
    virtual ~IEditorPanel() = default;
};

class PanelRegistry {   // fixed set at MVP; v1.0 adds tabs/multi-scene
public:
    void add(std::unique_ptr<IEditorPanel> p);
    span<std::unique_ptr<IEditorPanel>> all();
    // MVP set: Hierarchy, Inspector, Viewport, Console, CookErrors, Toolbar, Stats
    // v0.5  +: AssetBrowser, TilemapEditor, DebugOverlays, EventFlow, EcsMetrics, AudioMixer
};

// Viewport: the runtime renders into an offscreen RT owned via rhi-renderer's
// public API; this widget displays it (ImGui::Image over the RT's texture handle)
// and owns picking + gizmo + drop-target behavior. DPI: RT sized to widget
// pixels * content scale, rebuilt on resize (FR-R2D-080).
struct ViewportPanel final : IEditorPanel { /* ... */ };
}
```

**Tilemap authoring panel (v0.5, FR-EDT-121):** paint/erase on a 2D grid over the viewport (tile picker from the atlas sprite set, rect-fill via the FR-EDT-120 selection), writes the `.mctilemap` document through the same mutation funnel as everything else (§2.3). The re-bake loop: panel edit → document save → `CookService` request → tilemap chunk re-bake → hot-reload swap in the preview world (< 1 s, FR-AST-090) — the panel never builds GPU geometry itself; rhi-renderer consumes only the baked chunk (rhi-renderer §3.4 tilemap note).

**UI document panel (M-UI-DATA step 6):** `UiDocumentEditor` (editor core, headless) holds one `.mcui` as `ui::DocumentData`. Structural edits (add, remove, move, kind change) re-order the flat depth-first widget list and remap parent, nav and default-focus indices. Every edit is written by `cook::write_ui_document`, read back whole by `cook::read_ui_document` and kept only when it reads, the same path as the MCP `ui_*` tools. `save()` writes that canonical text through `EditorSession::write_asset` (hot reload). Undo keeps canonical texts, one step per run of edits between saves. `draw_ui_inspector` draws a widget through the `DrawCtx` seam; anchors, styles and layouts go through the reflected `Ui*` structs (`draw_fields`). `UiDocumentPreview` builds a `ui::Document` the way the player's `UiScreens` does (the theme applied, the theme's fonts) with the preview renderer's built-in skin. It submits into a `Screen2D` batch at the chosen size and rebuilds when the document, the mount or the hot-reload count moves. The ImGui panel (`ui_panel.cpp`) is a thin adapter.

### 2.8 Play session (v0.5; MVP ships run-in-player)

```cpp
namespace mce::editor {

class PlaySession {
public:
    enum class State { Stopped, Playing, Paused };

    // Enter: serialize current doc -> transient in-memory/temp .mce scene chunk
    // (assets cook library, same serializer the shipped runtime load path uses)
    // -> construct the runtime Host exactly as game.exe does -> boot scene load.
    // Budget: <= 1 s at sample-project scale (FR-EDT-060, NFR-M-03).
    Result<void, Error> enter(const SceneDocument& doc);
    void stop();                       // destroys the play world; preview untouched;
                                       // play-mode edits are discarded BY DESIGN
    void tick(uint32_t max_fixed_steps, float alpha);   // drives the runtime's own loop

    // Sim inspection (FR-DBG-050): pause/step ride on the runtime's fixed step —
    // single-step = run exactly one 60 Hz tick; slow-mo = scale the accumulator
    // fill rate (FR-RUN-050 determinism applies at any scale).
    void pause();  void step_one_tick();  void set_time_scale(float s);

    ecs::World*  world();              // null when stopped; read-only inspection only
    State        state() const;
};
}
```

### 2.9 Console, cook status, debug views

```cpp
namespace mce::editor {

// Console panel (FR-DBG-040): spdlog sink -> SPSC ring (single producer) or
// MpScQueue (multi-producer fan-in, fix F10) -> panel drain
// (platform-engineering §6.1). Entries carry level, message, file:line; the
// error row is click-to-copy/jump-to-source. Convars come from core's registry:
//   set(name, value) takes effect same-frame (convar reads are live, not cached).
class ConsoleModel {
public:
    void sink_line(LogLevel, std::string_view msg, const char* file, int line);
    span<const LogEntry> drain();            // called once per editor frame
    bool exec_command(std::string_view);    // "r.show_colliders 1", confuns
};

// Cook errors panel (FR-EDT-070): rows = {severity, code, path, message};
// click -> asset browser navigates to the path (v0.5) / selects in project list.
class CookStatusView {
public:
    void enqueue(CookEvent e);               // from the CookService worker only (MpScQueue, fix F10)
    span<const CookEvent> drain();           // main thread, once per frame
    bool  cook_in_progress() const;
    void  request_cook(CookTarget);          // posts a job to the assets/cook service
};

// Event-flow monitor (FR-ENT-030): reads per-type live counts + producer/consumer
// registration from ecs-gameplay's event registry; read-only, every frame.
// ECS metrics view: table count, rows/table, fragmentation from ECS debug stats.
// Render debug: toggles routed to rhi-renderer (wireframe, bounds, light frustums,
// colliders via physics debug draw) + "open last RenderDoc capture" button.
}
```

### 2.10 Entity picking (FR-EDT-030)

Decided: **CPU picking, deterministic, no GPU readback** (GPU color-ID picking is the v1.0 sketch if triangle precision is ever needed).

- **2D:** walk visible sprites top-most-first (sort layer desc, then order desc), test point-in-rotated-quad against each sprite's world quad (the same corners the sprite batch computes). First hit wins. Handles pixel-perfect for any scene size at editor frame rates.
- **3D:** build a ray from the viewport camera through the cursor; ray-vs-AABB (slab method) against the render extract's per-entity bounds (the same boxes the frustum culler built this frame), nearest-hit wins. AABB precision is honest for a diorama-scale editor; triangle-exact picking is deferred with the GPU picking sketch, not hand-rolled via mesh raycasts.

---

## 3. Internal design

### 3.1 The two-world model (the load-bearing data structure)

The binding "editor owns the canonical scene; the runtime never mutates scenes" plus A11 ("same-frame visibility is against the editor's **live preview world**") resolve into **one canonical document + two derived worlds**:

```
                 SceneCommand (the ONLY write path)
                        |
                        v
   +---------------- SceneDocument (canonical, undo-able) ----------------+
   |                                                                       |
   | rebuild-on-load / mirror-on-command                    build on Play  |
   v                                                                       v
EditorWorld (mce::ecs::World)                        PlayWorld (runtime Host)
 - full component set, attached via ECS API           - built from a transient
 - sim phases NOT running (render-only extract)         cook of the document
 - systems NOT registered; only render extract      - runs the real fixed-step
   + editor preview queries run                       pipeline; editor adds nothing
   - exists so inspector edits are visible same-frame - destroyed on Stop; edits
   - rebuilt wholesale if mirroring drifts (assert)     inside it are thrown away
```

- **Mirroring rule:** `SceneDocument::apply` performs the identical reflected write on both the document row and the preview world's component (`world.set<T>(entity, value)`), so inspector edits are visible the same frame without the runtime ever learning about the editor.
- **Rebuild-verify test (drift killer):** a CI test applies N random commands, then rebuilds a fresh `EditorWorld` from the document and asserts the two worlds' component bytes are identical. If mirroring ever drifts, the test catches it — the funnel plus this test *is* the consistency guarantee (Hazel's editor/scene divergence is the cautionary tale, `existing-engines.md` §2.7).
- **Document storage:** entities in a sparse array indexed by `EntityId.local_index` (freelist reuse); components stored **per entity** as reflection-serialized field blobs in a small inline map (`ComponentId -> blob`). Rationale: the document is editor-scale (≤ 10k entities, mutated one entity at a time), so it optimizes for *undo blobs, copy-paste, and serialization simplicity* — columnar performance belongs to the ECS, and the archetype ECS already is the columnar store. The document never hot-loops.

### 3.2 Mutation funnel invariants

All four consumers of `SceneCommand` live in one function so the invariants are enforced once:

1. Undo entry captured **before** mutation (old values serialized first).
2. Document mutated; `dirty = true`; autosave timer possibly armed.
3. Preview world mirrored via the ECS public API (entity mapping below).
4. Selection/inspector caches invalidated by component id.

### 3.3 Entity mapping (document id ↔ world handle)

Each world instance (preview or play) owns an `EntityMap`: `EntityId -> ecs::Entity` (and reverse), built at world construction by remapping in document order (deterministic, so the cooked scene's entity remap table and the play world agree — this is what makes `Play→Stop→Play` yield identical first-tick state, FR-EDT-060). Commands carry `EntityId`; translation to `ecs::Entity` happens at the funnel's mirror step and is a hard assert (never silent) if absent.

### 3.4 Algorithms of note

- **Hierarchy panel:** ImGui tree over the document's parent/children lists, using `ImGuiListClipper` on visible rows only; the tree model is a cached adjacency rebuilt on `SetParent/Create/Destroy` commands — never per frame (NFR-B-06: 60 fps with 5k entities while inspecting). MVP renders the flat entity list; the tree lands with `ChildOf` (v0.5) and costs only the adjacency cache.
- **Inspector widget dispatch:** a `constexpr` trait table (from core reflection) mapping field-kind + ui-hint → `DrawCtx` call; enum fields read their `EnumValue` table from the trait; asset/entity reference fields read-only-degrade when the picker source (asset registry, live scene) is unavailable — a missing reference renders magenta text + "missing", never a crash (FR-DX-050 failure policy).
- **Gizmo transform math:** screen-space delta → plane intersection on the camera's view basis for translate; anchor pivot for multi-select (each selected entity's transform composed with the anchor's delta; rotation/scale about the pivot); snapping quantizes *before* the write so the document never holds unsnapped values. All math via `mce::math` (no ImGui-side matrix types cross the seam).
- **Copy/paste:** serialize selected entity sub-trees (children included) into a clipboard blob (reflection-serialized, same format as undo blobs); paste = `deserialize_entity` per root with fresh `EntityId`s and remapped internal references. The clipboard is process-memory only (no OS clipboard formats at MVP).
- **Autosave/crash recovery:** 5-minute dirty-timer + save-on-focus-loss writes `scene.mcscene.autosave` via temp-write-then-rename; on editor launch, an autosave newer than the last user save triggers a recovery prompt (`FR-EDT-100`). Kill -9 mid-write leaves the previous file intact because of the atomic rename.
- **Frame-order discipline:** the editor never allocates from the runtime's frame-linear allocator for UI state (editor uses ordinary allocation — it is tool code; the zero-heap binding applies to runtime hot paths). The play world's frame, however, uses the runtime's own allocators untouched.

### 3.5 Editor/runtime separation mechanism (recap with teeth)

1. **CMake wall:** `mce_editor_layer` links `mce_engine.dll` + ImGui; nothing under `src/engine/` links UI. Leakage is a link error, not a review finding.
2. **Include grep:** no `imgui.h` outside `src/editor/` and `tests/devhost/` (CI). *As built, the ImGui wall in `check_walls.py` allows only `src/editor/layer/`.*
3. **Symbol scan:** `dumpbin /symbols` on `mce_engine.dll` must contain no `ImGui*` (CI, `FR-UI-048`; scan target per ARCHITECTURE fix F8).
4. **No `#ifdef MCE_EDITOR`:** separate mains; the editor registers *editor* systems into a `Phase::Editor`-shaped slot only when it runs its own preview loop, and registers **nothing** into a play world's phases.
5. **ImGuizmo + (later) ImPlot** enter via the one-page ADR named in `audio-input-ui.md` §4.1 — single-purpose, ImGui-native, 2-file.

---

## 4. Threading & lifecycle

### 4.1 Thread inventory (decided; consistent with platform-engineering §3)

| Thread | Owner | Does | Rules |
|---|---|---|---|
| **Main** | editor | SDL pump, ImGui, panels/commands, preview extract, play ticks, GL submission, undo, selection, picking | Owns the GL context, `SceneDocument`, both worlds, the undo stack. Everything else talks to it via queues. |
| **Cook worker** (1) | assets' `CookService` (job system) | runs the `assets` cook library over the content tree (shader validation, imports, pack); spawned when the editor's cook button posts a request | posts `CookEvent`s over an `MpScQueue` (core, fix F10); never touches the document; cancel flag polled per asset. Editor-owned *requests*, assets-owned thread — ARCHITECTURE §6.1 |
| **File watcher** | editor-owned, on the platform IO worker | polls `assets/` mtimes every 500 ms (ADR-0015: polling, not `ReadDirectoryChangesW` — portable, sufficient, no filter-driver surprises) | posts `HotReloadEvent`s; the assets subsystem owns re-cook + handle stability |
| **SDL audio callback** | runtime-owned | mixer pull | editor never touches it; play session commands go through the runtime's FIFO |
| Tracy/RenderDoc | tooling | — | editor main thread only |

There is deliberately **no editor render thread**: GL 4.5's context is single-thread-bound (platform-engineering §3.1) and the editor's 60 fps budget (NFR-B-06) is dominated by ImGui + a single RT blit, not by render parallelism.

### 4.2 Editor frame order

```
pump SDL -> ImGui_ImplSDL3 backend (may consume input) -> editor hotkeys
drain queues: logs, cook events, hot-reload events  (single consumer: main thread)
panels draw + emit SceneCommands        (immediate mode; commands applied same frame)
if playing:  play.tick(fixed_steps_from_accumulator, alpha)   [runtime loop owns pacing]
else:        preview render extract (no sim phases; interpolation alpha = 1)
render active world -> RT; viewport widget displays RT
undo coalesce commit; autosave timer; FrameMark
```

Input arbitration (decided, from `audio-input-ui.md` §3.2): ImGui receives SDL events first and marks consumed; the viewport only receives clicks/gizmo drags ImGui did not consume; while **playing**, captured-mouse relay (for game feel testing) is a v0.5 toggle, not a default.

### 4.3 Lifecycle

- **Startup (≤ 5 s, NFR-M-01):** crash-handler install → core/rhi/audio init → GUID registry load → open last scene (or crash-recovery prompt) → dock layout restore → first frame. Editor settings persist as `imgui.ini` + a small JSON-ish settings file written through the same reflection serializer (editor-side text is allowed; the *runtime* never parses text).
- **Play:** `PlaySession::enter` serializes + temp-cooks + boots the host; deterministic because serialization and load are both deterministic and the play world registers exactly the game's systems, nothing editor-flavored (risk R3).
- **Stop:** destroy play world; preview world never paused, so the document view is exactly as before Play (modulo discarded play-mode edits, which FR-EDT-060 mandates).
- **Shutdown:** flush autosave, prompt on dirty document, cook worker join, GL teardown in reverse init order.
- **Crash:** SEH filter + minidump from core runs in the editor too (`platform-engineering` §6.3); recovery is the autosave path above.

---

## 5. Dependencies

### 5.1 Upstream (consumed) — assumes the subsystem list: core, rhi-renderer, ecs-gameplay, physics, assets, editor-tools, audio-input-ui, scripting

| Subsystem | What editor-tools consumes | Notes |
|---|---|---|
| **core** | `Result<T,Error>`, `Handle<T>`, `Guid`, allocators (editor uses ordinary allocation; cook scratch uses the assets pool), convar/confun registry, spdlog sinks + SPSC ring, `MpScQueue` (fix F10: cook events, hot-reload events, log fan-in), crash handler, job system (cook worker), Tracy client | The convar registry is the console panel's data source (`FR-DBG-040`). |
| **ecs-gameplay** | `World`, `Entity`, reflected component registry, query API (inspector reads live components in play mode), event-queue registry (event-flow monitor), pipeline/phase registration view (`FR-DX-030` pipeline view), `ChildOf` adjacency (hierarchy tree), change ticks (inspector "changed this frame" highlight), ECS metrics (table counts, fragmentation) | Editor registers zero systems into play phases; preview world runs render-extract only. |
| **rhi-renderer** | RenderDevice + frame submission for the preview/play worlds, an offscreen render-target + texture handle the viewport widget displays, debug-draw primitives (bounds, frustums, colliders, grid), shader hot-reload status callback, RenderDoc capture trigger | Editor never calls GL; even `ImGui_ImplOpenGL3`'s context is confined to the editor backend file. |
| **assets** | cook library/CLI invocation + progress + typed error list (`FR-EDT-070`), GUID registry (browser, pickers, rename-with-GUID-stability), `.mce` mmap load, hot-reload events, thumbnail provider (v1.0) | The editor drives cook; it does not implement it. |
| **physics** | collider debug data for the overlay panel (`FR-EDT-090`). Edit-mode picking touches **no physics world** (the preview world has none — physics §4.2): 2D picking is CPU point tests over sprite quads + `Collider2D` shape data (`ColliderShape` Box2/Circle2/Convex2, CF20), 3D picking is ray-vs-AABB over render-extract bounds (§2.10). The `World2D`/`World3D` raycast/overlap query API is **play-mode-only**, consumed by gameplay module systems, never by editor picking | Read-only queries. |
| **audio-input-ui** | SDL3 event pump contract (editor receives raw events first, ImGui backend consumes), action-map data for the input-authoring view (v0.5: view bindings; rebinding UI is FR-INP-040's options UI, editor-adjacent), audio bus handles for the mixer panel (v0.5) | |
| **scripting** | registered systems/phases visible in the pipeline view; FSM state components inspectable live (`FR-ENT-090` — "enemy FSM inspectable in the editor live") | Editor adds no systems. |

### 5.2 Downstream (provided to)

None at runtime — `editor-tools` is the top of the dependency graph. It **provides surfaces to the developer**, not to other subsystems: the error list (fed by `assets`), the console (fed by `core` logging), and the pipeline view (fed by `ecs-gameplay`/`scripting` registrations). The shipped runtime has no build-time or link-time relationship with this subsystem at all.

---

## 6. Test strategy

The decisive design choice enabling testing: **`editor_core` (document, commands, undo, selection, picking, play-session control, models) compiles headless with no ImGui**, and `DrawCtx` is an interface — the ImGui implementation is one thin adapter. CI therefore tests editor *logic*, which is where the bugs live; ImGui pixels are verified by the manual gates the requirements already pin.

| Layer | Test | Ties to |
|---|---|---|
| Unit (doctest, headless) | Undo/redo fuzz: N random commands (inspector-style property sets, add/remove component, create/destroy/duplicate) → undo all → re-serialize document → **byte-identical to before** | FR-EDT-040 acceptance |
| Unit | Scene round-trip: save → load → serialize → byte-identical; version-ahead file ⇒ `Result` error with code+path, never a crash | FR-EDT-100, FR-DX-050 |
| Unit | Inspector property round-trip through `DrawCtx`'s test double: set → undo → set → redo equality on the reflected bytes | FR-EDT-020 |
| Unit | Coalescing: rapid same-field writes collapse to one undo entry; different-field drags do not merge | FR-EDT-040 |
| Unit | Picking: 2D point-in-rotated-quad over a sort-layered sprite set; 3D ray-AABB over synthetic extract bounds (nearest-hit + layer order) | FR-EDT-030 |
| Unit | EntityMap: stale `EntityId` after destroy fails loudly; duplicate/paste remaps internal `ChildOf` and `Entity`-typed fields | FR-EDT-050 |
| Integration (headless) | Play determinism: `Play→tick(600)→Stop→Play→tick(600)` — first-tick and final component hashes identical, reusing the FR-RUN-040 harness | FR-EDT-060, NFR-M-03 |
| Integration (headless) | Two-world consistency: N random commands → rebuild world from document → component bytes equal mirrored world (drift killer, §3.1) | binding architecture-authority rule |
| Integration | Run-in-player: temp-cook + child-process spawn + exit-code capture | FR-EDT-060 (MVP path) |
| Manual gates | Editor ≥ 60 fps with 5k entities while idle/inspecting (Tracy); DPI-crisp viewport; 2-hour authoring soak with 0 crashes; 30-second-loop timing (drag→gizmo→Play→Stop→tweak→Save→Cook→launch) | NFR-B-06, FR-R2D-080, NFR-M-05, UC-C |
| CI hygiene | ImGui include-grep + `mce_engine.dll` symbol scan (fix F8 scan target); clang-tidy on `src/editor/`; ASan build exercises headless editor tests | FR-UI-048, binding test policy |

Golden-image tests do not apply to the editor itself (it is not shipped), except that the *viewport's RT display path* participates in the renderer's existing golden-image plumbing at cook time.

---

## 7. Risks & mitigations

| # | Risk | Mitigation |
|---|---|---|
| R1 | **Undo/redo is the classic editor tar pit** — drag coalescing across widgets, create/destroy ordering, multi-select group ops | Single mutation funnel (§3.2) so there is exactly one place commands are born; undo blobs are reflection-serialized *before* mutation (old state never aliased); the byte-identical fuzz test is a merge gate, not an aspiration; cut-ladder rung 2 exists if it overruns (isolated module, removable). |
| R2 | **Two-world drift** (document vs preview world silently diverging) | One funnel + the rebuild-verify consistency test (§3.1); wholesale world rebuild available as a user command if drift is ever suspected (cheap at ≤ 10k entities, and it is the fix of last resort). |
| R3 | **Play-in-editor breaks sim determinism** (editor systems, debug writes, or non-deterministic doc serialization leaking into the play world) | The play world is constructed through the runtime's normal cooked-scene load path — the editor cannot register systems into its phases by construction (no API for it); Play→Stop→Play hash test in CI; serialization is deterministic and covered by the cook-determinism test. |
| R4 | **ImGui editor scope creep** (the "editor is the product" failure mode — the Hazel stall lesson, `existing-engines.md` §2.7) | Tier discipline from §1.3; panels are isolated objects so cut-ladder rung 2 removes three of them cleanly; timeline/curve editor and prefab-override UI are admitted by slip-note only (v1.0, FR-EDT-130/080). |
| R5 | **Viewport widget complexity** (RT resize/DPI, GL context sharing with ImGui, image-handle lifetime) | One backend file owns the ImGui↔GL bridge; RT sizing driven by a single `ViewportPanel::layout_changed` path verified against a manual DPI gate at ROADMAP M0 (where the shell viewport ships — finding F4/F5 re-baseline); every render feature behind the viewport still passes the RenderDoc capture gate (`FR-DBG-020`). |
| R6 | **Hot-reload races** (file watcher / cook worker mutating resources mid-frame) | All cross-thread results arrive as main-thread-drained events; the assets subsystem guarantees handle stability across reload (magenta placeholder on failure, `FR-AST-090`); the editor never dereferences an asset outside a frame after draining. |
| R7 | **Immediate-mode performance cliffs** (5k-entity hierarchy, fat inspectors) | `ImGuiListClipper` everywhere lists appear; hierarchy adjacency cached and rebuilt on structural commands only; inspector renders only the selection; NFR-B-06 is a tracked budget with a Tracy zone around the ImGui frame from M0. |
| R8 | **ImGuizmo gaps** (multi-select pivot modes, snapping edge cases, 2D-mode fit) | ADR'd as required; 2D viewport uses the same gizmo with an orthographic camera and axis-locked ops; if ImGuizmo's 2D behavior fights us, the fallback is the same math hand-rolled on ImGui draw lists (the gizmo module owns a `GizmoBackend` seam so the swap is local). |
| R9 | **Editor crash → lost authoring work** (solo dev, 2-hour sessions are the gate) | Autosave + atomic writes + crash-recovery prompt (FR-EDT-100 acceptance is kill -9 tested); minidumps with per-CI-build PDBs (platform-engineering §6.3) make the crash itself diagnosable. |

---

## 8. Milestone mapping (editor-tools slice)

Tier names with ROADMAP milestone IDs (ARCHITECTURE §10.1 is the mapping authority); the MVP spread reflects ROADMAP's M0/M2/M3 re-baseline (finding F4).

- **ROADMAP M0 (MVP):** shell (docking + panel registry + input arbitration), viewport + console panels over a stub world, dock layout persistence. *Artifact: FR-EDT-010 gate (shell portion).*
- **ROADMAP M2 (MVP):** reflection inspector (`FR-EDT-020`) + hierarchy flat list + **scene create/save** — lands with `mce_refl` (moved here from the old M1; finding F4).
- **ROADMAP M3 (MVP):** entity CRUD + copy/paste (`FR-EDT-050`), translate gizmo + entity picking (`FR-EDT-030`), asset/entity reference pickers (Guid + type-filtered dropdown, finding F6), autosave, cook error panel (`FR-EDT-070`), run-in-player button (A4), 2D viewport RT (`FR-R2D-080`), 2D grid/snap + rect-fill authoring aids (`FR-EDT-120`, finding 31). *Artifact A is authorable end-to-end in the editor.*
- **ROADMAP M4.5 (off-tier tooling):** headless editor MCP server `mce_mcp` over `mce_editor_core` (`FR-EDT-160`; design in `editor-mcp.md`) — scene read/edit through the mutation funnel, cook, headless runs for an external AI client.
- **v0.5 (ROADMAP M5b–M7):** undo/redo (`FR-EDT-040`), asset browser + drag-drop (`FR-EDT-110`), rotate/scale gizmos + multi-select (`FR-EDT-030/050`), **play-in-editor with pause/step/slow-mo + live inspection (`FR-EDT-060`, M5b — finding 11)**, debug overlays + event-flow monitor + ECS metrics (`FR-EDT-090`), tilemap authoring panel (v0.5 tier, FR-EDT-121), audio bus mixer panel.
- **v1.0 (ROADMAP M8):** prefab instantiate/override UI (`FR-EDT-080`), browser thumbnails, multi-scene tabs, timeline/curve editor (`FR-EDT-130`) alongside FR-ENT-110.

## 9. References

- Dear ImGui (docking branch) documentation — docking, multi-viewport caveats, `ImGuiListClipper`, `ImGui_ImplSDL3` backend.
- ImGuizmo — standard ImGui gizmo companion (ADR-required per `audio-input-ui.md` §4.1).
- Unity editor manual (serialized object model, GUID references, play-mode edit-discarding), Godot 4 editor architecture (editor-as-engine-client), O3DE prefab/inspector-first warnings — per `existing-engines.md` §2/§4.
- Hazel (The Cherno) series — the layer-stack editor shell and the editor/scene-mutation anti-patterns it documents.
- Bob Nystrom, *Game Programming Patterns* — Command (undo stack), Event Queue (console ring), Dirty Flag (hierarchy cache).
- Casey Muratori, "Immediate-Mode Graphical User Interfaces" (2005) — the IMGUI ergonomics argument the editor bets on.
- Jason Gregory, *Game Engine Architecture*, 3rd ed. — tools/editor chapter framing, editor-as-runtime-client layering.
- Piccolo / Overload — solo-dev ImGui editors as scale references for gizmo and serialization scope.
- Repo-internal: `docs/ARCHITECTURE.md` §5.1 (binding fixes F7/F8/F10/F12 — three-target split, `mce_engine.dll` naming, `MpScQueue`, watcher ownership; §4.2 play-mode sequence; ADR-0008, ADR-0015), `docs/architecture/assets.md` §6–§7 (`.mcscene` migrators, `CookService`, GUID registry), `docs/research/audio-input-ui.md` §4 (UI toolkit + target wall), `docs/research/existing-engines.md` §4 (editor authority decision 4/7), `docs/research/platform-engineering.md` §3/§6 (threads, logging sink), `docs/research/ecs-gameplay.md` §10 (scene document ownership).
