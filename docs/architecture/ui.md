# Architecture: Runtime UI (`mce_ui`) — design document

**Status:** Final design (ROADMAP M4 deliverable, FR-UI-020 / ARCHITECTURE CF21). **Build:** ROADMAP M8 (v1.0).
**Seed:** audio-input-ui.md §2.5 and §3.9 — this document supersedes their sketch depth; the decisions recorded there
(retained tree, sprite-batch path, action-driven focus, events-only bridge) are unchanged.
**Applies** the binding errata from ARCHITECTURE §5.1.

MVP and v0.5 games ship scene-state menus and text-sprite HUDs (A9; FR-UI-010's early tiers). This document fixes
the shape of the system that replaces them at v1.0 so nothing built in between pre-couples to the wrong model.

---

## 1. Scope

### 1.1 Owns

- A **retained widget tree** per UI document (a menu screen, the HUD, the dev console), with stable `WidgetId`s.
- **Anchored-rect layout** (the uGUI model): every widget's rect is a function of its parent rect, two anchors,
  two offsets and a pivot; plus two container layouts (stack, grid) that write those fields for their children.
- **Input routing** from *actions* (never raw devices): focus, navigation, confirm/cancel, pointer hover/press,
  text entry for the console only.
- **Drawing through the 2D batch**: quads, 9-slice quads and text emitted into a `render::SpriteBatch` in
  `PassId::Screen2D` — no second renderer, no second pipeline family.
- **Read-only data binding** from gameplay state (reflection property paths and ECS events) to widget properties.
- The **shipped debug console** widget over convars (FR-UI-045).

### 1.2 Does not own

- Gameplay code paths. Gameplay never includes `mce/ui/*` (FR-UI-030, CI grep wall).
- The editor's UI: ImGui stays editor-only (FR-UI-048 — CMake target wall + symbol scan already enforced).
- Rich markup, IME-heavy text entry and localization pipelines (FR-UI-060, Won't). RmlUi is the pre-approved
  fallback ADR if a game genuinely needs markup (§8).
- Fonts and atlases: cooked by mce_cook (CHNK_FONT, CHNK_TEX/atlas) and owned by the renderer's stores.

### 1.3 Constraints inherited from the engine

| Constraint | Consequence for `mce_ui` |
|---|---|
| Engine containers only, no exceptions/RTTI | Widgets are records in a pool with a `WidgetKind` enum, not a virtual class hierarchy |
| Determinism (FR-RUN-040) | UI reads the post-quantization `InputState`; its outputs that touch gameplay are *events*, hashed like any other |
| L4 layer (ARCHITECTURE §3) | Depends on core, input, render (2D), asset; never on ecs-gameplay kit internals |
| 2D batch budget (rendering-2d D4) | A full-screen menu is ≤ 1 extra MDI group per (atlas × blend); the HUD shares the game's Screen2D batch |

---

## 2. Public interface

```cpp
// mce/ui/ui.h
namespace mce::ui {

using WidgetId = Handle<struct WidgetTag>;          // generational, stable across re-layout

enum class WidgetKind : u8 { Panel, Image, Text, Button, Toggle, Slider, Stack, Grid, Console };

struct Anchors {                                    // uGUI RectTransform, in parent-relative units
    math::vec2 min{0, 0}, max{0, 0};                // 0..1 of the parent rect
    math::vec2 offset_min{0, 0}, offset_max{0, 0};  // pixels, at UI scale 1
    math::vec2 pivot{0.5f, 0.5f};                   // rotation/scale origin and size-change anchor
};

struct Style {                                       // resolved per state (normal/hover/pressed/focused/disabled)
    render::Color tint{1, 1, 1, 1};
    u32 sprite_region = ~0u;                         // into the document's sprite set; ~0 = flat colour
    math::vec4 slice{0, 0, 0, 0};                    // 9-slice insets in texels (0 = stretch)
    render::Color text_color{1, 1, 1, 1};
    f32 text_size = 16.0f;
};

class Document {                                     // one screen / HUD / console
public:
    explicit Document(mem::IAllocator& a);
    WidgetId root() const;
    WidgetId add(WidgetId parent, WidgetKind kind, StringView name);
    void remove(WidgetId w);                          // subtree; deferred to the next update()

    void set_anchors(WidgetId w, const Anchors& a);
    void set_style(WidgetId w, WidgetState s, const Style& st);
    void set_text(WidgetId w, StringView utf8);
    void set_value(WidgetId w, f32 v);                // slider / toggle
    void set_visible(WidgetId w, bool v);
    void set_interactable(WidgetId w, bool v);
    void set_nav(WidgetId w, NavEdges edges);         // explicit up/down/left/right; unset = geometric

    // Per frame, main thread, Render_Pre (after the sim, before extract):
    void update(const UpdateContext& ctx);            // layout -> input -> state machines -> bindings
    void submit(render::SpriteBatch& batch) const;    // quads + text into PassId::Screen2D

    Span<const UiEvent> events() const;               // this frame's confirms/changes (double-buffered)
    WidgetId focused() const;
    WidgetId find(StringView path) const;             // "pause/buttons/resume"
};

struct UpdateContext {
    rhi::Extent2D viewport;                           // pixels
    f32 ui_scale = 1.0f;                              // display scale x user setting (FR-R2D-080)
    const input::InputState* actions = nullptr;       // the UI action map's snapshot
    math::vec2 pointer{-1, -1};                       // pixels; negative = no pointer this frame
    bool pointer_down = false, pointer_pressed = false, pointer_released = false;
    f64 dt = 0.0;                                     // presentation time, for transitions only
};

struct UiEvent {
    enum Kind : u8 { Confirmed, ValueChanged, FocusChanged, Cancelled } kind;
    WidgetId widget;
    f32 value = 0.0f;
};

// Builder sugar over Document (C++ is the authoring format until an artist needs a cooked one, §5.4).
class Builder {
public:
    Builder(Document& doc, WidgetId parent);
    Builder panel(StringView name);
    Builder button(StringView name, StringView label);
    Builder text(StringView name, StringView utf8);
    Builder slider(StringView name, f32 lo, f32 hi);
    Builder& fill();                                  // anchors 0..1, zero offsets
    Builder& at(math::vec2 anchor, math::vec2 size);  // point anchor + size
    Builder& style(const Style& s);
    Builder& bind(StringView property_path);          // §4.4
    Builder& bind_convar(StringView convar);          // two-way for options screens only (§4.4)
    WidgetId id() const;
};

}  // namespace mce::ui
```

A pause menu, end to end:

```cpp
ui::Document pause(mem::heap());
ui::Builder b(pause, pause.root());
b.panel("bg").fill().style({.tint = {0, 0, 0, 0.6f}});
auto col = b.panel("buttons").at({0.5f, 0.45f}, {320, 0});        // a vertical Stack
col.button("resume", "RESUME");
col.button("options", "OPTIONS");
col.button("quit", "QUIT TO MENU");
// The runtime's pause state (FR-RUN-030) owns the document and reacts to its events:
for (const ui::UiEvent& e : pause.events())
    if (e.kind == ui::UiEvent::Confirmed && e.widget == pause.find("buttons/resume")) states.pop();
```

---

## 3. Data model

### 3.1 The widget pool

A `Document` is a struct-of-arrays pool indexed by `WidgetId.index`:

| Array | Contents |
|---|---|
| `tree` | `parent`, `first_child`, `next_sibling`, `depth` (u32 indices; the ECS hierarchy index's shape) |
| `kind`, `flags` | `WidgetKind`; visible / interactable / dirty-layout / dirty-text bits |
| `anchors` | `Anchors` (authoring input) |
| `rect` | resolved `Rect` in pixels (layout output) |
| `styles` | index into a style table: 5 states × `Style` per widget (shared by value-equal styles) |
| `content` | text (String, small-buffer), value (f32), sprite region, nav edges |
| `bindings` | optional binding slot (§4.4) |

Children are kept in **document order**; that order is also draw order and the fallback tab order. Removal is
deferred to `update()` and recycles the slot with a bumped generation, so a stale `WidgetId` held by a game
resolves to nothing (one error, FR-DX-050) instead of to a different widget.

### 3.2 Why not entities

Considered and rejected: UI widgets as ECS entities with a `RectTransform` component. The UI tree is
presentation state that changes every frame (hover, focus, transitions), lives on the main thread in `Render_Pre`,
must not enter the state hash and must not be serialized into scenes. Making widgets entities would put all of that
inside the deterministic world and its sync points. A widget may carry an optional `owner` entity for world-space
nameplates (§4.6), which is a read-only link.

---

## 4. Algorithms

### 4.1 Layout (per `update()`, only dirty subtrees)

For each widget in depth order (the pool is kept depth-sorted on structural change, so this is one linear pass):

```
parent = rect[parent(w)]                                       // root: the viewport / ui_scale
lo = parent.min + parent.size * a.min + a.offset_min * scale
hi = parent.min + parent.size * a.max + a.offset_max * scale
rect[w] = snap_to_pixels(Rect{lo, hi})                         // FR-R2D-040: crisp edges at any scale
```

Containers run before their children and *write* the children's anchors:

- **Stack** (vertical/horizontal): children's anchors become a point on the cross axis and an offset along the
  main axis; `spacing`, `padding`, `align`. Text children report their preferred size from the font metrics, so a
  stack of labelled buttons sizes itself.
- **Grid**: fixed cell size or columns-count; fills row-major.

Only Text measures content (one `layout_text` call per dirty text, cached by string + size). There is no
constraint solver and no flexbox. That is the uGUI model's whole point: predictable, one pass, no iteration.

Dirtiness: changing anchors/visibility/text marks the widget and its descendants; a viewport or ui_scale change
marks the root. A static HUD costs zero layout work per frame.

### 4.2 Input routing and focus

Inputs come from a dedicated **"UI" action map** pushed on the input stack while a menu document is modal
(audio-input-ui §3.1: layered maps). The fixed `InputState` fields carry `confirm_pressed`/`cancel_pressed`;
navigation reads the named action `UI_Navigate` (a vec2 with key repeat applied by the action layer).

Focus rules:

1. A modal document owns focus; a non-modal one (the HUD) never takes it.
2. On open, focus goes to the first interactable widget in document order, or the one marked `default_focus`.
3. Navigation moves along explicit `NavEdges` when set; otherwise **geometric**: among interactable, visible
   widgets in the half-plane of the direction, pick the minimum of `distance + 2 × perpendicular offset`
   (the uGUI/Godot heuristic that makes grids and columns behave without authoring).
4. Pointer hover sets *hover*, not focus; a pointer press on an interactable widget focuses it. Pointer and
   gamepad can therefore alternate without fighting.
5. `confirm` on the focused widget emits `Confirmed` exactly once per press (pressed edge, not held).
   `cancel` emits `Cancelled` on the document (the pause state maps it to "resume").

Hit-testing walks the tree in reverse document order (topmost first) and skips invisible or non-interactable
subtrees. A full-screen panel with `blocks_pointer` stops the walk, which is how a modal dims and shields the game.

### 4.3 Widget state machines

Each interactable widget is a tiny state machine over {normal, hover, pressed, focused, disabled}. The resolved
`Style` is the style of the current state, optionally interpolated over `transition_ms` (presentation time; not in
the hash). Slider: `UI_Navigate.x` steps by `step`, pointer drag maps x to value; emits `ValueChanged` when the
value changes. Toggle: confirm flips and emits `ValueChanged` (0/1).

### 4.4 Data binding (FR-UI-030)

Two binding kinds, both polled in `update()`:

- **Property path, read-only.** `bind("player/Health/hp")` resolves *entity name / component / field* once through
  `mce_refl` (the same path syntax the inspector uses) into an `(EntityId, ComponentId, field offset, FieldType)`
  tuple and re-resolves when the entity generation changes. Each frame the bound value is read and compared with the
  cached one; only a change dirties the widget (text re-layout or fill amount). Gameplay is unaware.
- **Convar, two-way.** Options screens bind sliders and toggles to convars (`audio.master_db`, `r.vsync`). Writing a
  convar is the one sanctioned UI → engine write, and it goes through the console's command path so it is logged
  and replayable. Convars flagged `Cheat` cannot be bound in shipped builds.

Gameplay → UI notifications that are not state (a "LEVEL CLEARED" banner) are ECS events the UI layer reads
(`EventQueue<T>`); UI → gameplay intents are `UiEvent`s the *runtime state* (not gameplay) turns into state-stack
operations or gameplay events. Gameplay code never includes `mce/ui`.

### 4.5 Drawing (FR-UI-020 acceptance: through the sprite batcher)

`submit()` walks visible widgets in document order and appends to a `render::SpriteBatch` begun with
`compute_screen_view(viewport)` in `PassId::Screen2D`:

| Widget part | Emitted as |
|---|---|
| Flat panel | one `SpriteInstance` with the white region of the UI sprite set, tinted |
| Image / 9-slice | one quad, or nine quads for a 9-slice (corners fixed, edges stretched on one axis, centre on both) |
| Text | `layout_text` + `SpriteBatch::add_text` with the document's font (coverage path, rendering-2d D9) |
| Clip regions | scroll views cut their content on the CPU: `SpriteBatch::add_rect` and `TextPlacement::clip` trim each quad and glyph (and its uvs) to the view, so clipping adds no scissor and no batch group (M-MOBILE 11b) |

`sort_order` is the widget's document-order index, so equal-layer widgets draw in tree order and the radix sort's
stability keeps text above its own panel. The whole document is one sprite set (UI atlas) plus one font, so a typical
menu is 2 MDI groups (sprite + text). No new RHI concept and no new shader are needed. The 2D path already has
everything (ADR-0020), which is the point of the decision.

### 4.6 World-space UI

Nameplates and damage numbers are *not* documents. They are sprites and text placed by a presentation system with
`compute_view_2d` or a 3D-to-screen projection. A widget may follow an `owner` entity's projected position
(`Anchors` offset recomputed per frame), which covers "health bar over the boss" without a world-space canvas.

### 4.7 Phones and many aspect ratios (M-MOBILE step 11b)

- **Anchor spaces.** `Anchors::to` picks what the fractions are of: the parent (default; the root is the safe
  area), the safe area, or the whole screen (a backdrop under the notch).
- **Scale.** `compute_ui_scale(viewport, display_scale, user)`: the display's DPI scale times the user setting,
  lowered so a 360 × 640 layout still fits, in 1/8 steps.
- **Scroll views** are stacks that may overflow: drag past an 8 px slop (the press is let go), inertia with
  friction, the wheel, and focus scrolled into view. Moving one re-lays its subtree only.
- **Modals** are a stack on the document: the top one owns focus, navigation, the pointer and Cancel, and draws
  two sort layers up so the page's text never shows through it.
- **Tabs** are a bar of buttons over a set of pages, one visible.
- **9-slice** insets live in `Style::slice` (texels of the region; corners scale with the UI scale).
- The authoring values (`mce/ui/types.h`) are reflected as `UiAnchors`, `UiStyle`, ... for the editor and MCP.

---

## 5. Lifecycle

### 5.1 Where it runs

`Render_Pre`, main thread, after the fixed-step sim for the frame and before extract. Order: input sampling (already
done by the frame) → `Document::update` for each live document (modal first) → `submit` into the Screen2D batch the
frame records after the world pass. Nothing in `mce_ui` runs during sim ticks.

### 5.2 Ownership

Runtime states own documents: the boot state owns the title screen, gameplay owns the HUD, pause owns the pause
menu. Pushing a state with a modal document pushes the "UI" action map. Popping pops it. A document's lifetime is
its state's lifetime; `WidgetId`s never outlive their document.

### 5.3 Determinism

UI never writes gameplay state directly. Its outputs (`UiEvent`s turned into state-stack ops or gameplay events) are
ordinary events and are part of the hashed stream. Because focus and navigation run on the quantized action
snapshot, a replay produces the same events. Hover, transitions and pointer position are presentation-only.

### 5.4 Authoring

C++ builders at v1.0 (§2). Since M-UI-DATA step 1 a document is also data: a `.mcui` JSON source cooks to a
`CHNK_UIDOC` chunk (`mce/ui/document_data.h`), a flat serialization of the widget pool in document order (names,
kinds, anchors, the five per-state styles, values, layouts, nav edges, convar bindings) plus the skin's font and
sprite sheet by GUID. `build_document` turns it into a `Document` and `extract_document` turns a `Document` back into
it, and a `.mcui` widget starts from the same per-kind defaults `Builder` applies (`builder_defaults`), so code and
data make the same documents. The cook reads anchors, styles and layouts through the reflected `Ui*` types. The
editor would preview documents in its viewport through the same `submit` path (no ImGui reimplementation of
widgets).

### 5.5 Hot reload

Styles reference sprite regions and fonts by asset handle. A reloaded atlas or font is picked up through
`AssetRef` re-stamping (assets.md §7.5) and marks every text widget dirty. Documents themselves are code.

---

## 6. Test plan (lands with the M8 build; contracts pinned now)

| Test | Kind | Pins |
|---|---|---|
| Anchors → rects for fill / point / stretch-x / pivot cases at ui_scale 1, 1.5, 2 | unit | §4.1 arithmetic and pixel snapping |
| Stack of three labelled buttons sizes to its text; grid 3×3 fills row-major | unit | containers |
| Dirty tracking: a static HUD does zero layout passes on frames 2..N | unit | §4.1 cost claim |
| Geometric navigation reaches every widget of a 3×3 grid from each corner; explicit edges override | unit | §4.2 rule 3 |
| `confirm` held for 30 ticks emits one `Confirmed` | unit | §4.2 rule 5 |
| Pointer press focuses; gamepad navigation afterwards continues from it | unit | §4.2 rule 4 |
| Panel + button + text renders through `SpriteBatch` → golden image, ≤ 2 MDI groups | GPU | FR-UI-020 acceptance |
| Health bar bound to `player/Health/hp` updates on change, untouched otherwise | unit | §4.4 |
| Gameplay targets include no `mce/ui` header | CI grep wall | FR-UI-030 |
| No `ImGui*` symbol in `mce_engine` or a shipped executable | CI symbol scan | FR-UI-048 (exists today) |
| Replay of a recorded menu session produces the same `UiEvent` stream and state hash | determinism | §5.3 |

---

## 7. Budgets

| Item | Budget | Basis |
|---|---|---|
| `update()` of a 60-widget menu, nothing dirty | < 0.02 ms | linear passes over SoA arrays, no allocation |
| Full re-layout of a 60-widget menu (resize) | < 0.1 ms | one pass + text measurement of dirty labels |
| Draw cost | ≤ 2 MDI groups per document | one UI atlas + one font, Screen2D pass |
| Memory | < 64 KiB per typical document | SoA pool + style table + text strings |

---

## 8. Risks and the fallback

| Risk | Mitigation |
|---|---|
| `mce_ui` eats the v1.0 tier (audio-input-ui R6) | The scope above is the whole of v1.0: no markup, no scroll views, no cooked format. FR-UI-040 widgets are Could and cut first. If the M8 prototype (panel + button + text + focus) is not green by the M8 mid-point, the **RmlUi ADR** is executed: RmlUi's render interface is implemented on the same `SpriteBatch` path and the `Document` API above shrinks to a thin wrapper. |
| Navigation feels wrong on irregular layouts | Explicit `NavEdges` always win; the geometric rule is a default, not a contract |
| Text measurement cost on large consoles | The console widget lays out only visible lines (ring buffer of laid-out lines) |
| A game reaches into UI from gameplay | Grep wall in CI (§6); the binding API makes reaching in unnecessary |

---

## 9. Decisions recorded

1. Retained tree in an SoA pool with a kind enum, not virtual widgets and not ECS entities (§3).
2. uGUI anchored rects plus two container layouts; no constraint solver (§4.1).
3. Action-driven focus on a pushed "UI" action map; pointer is hover + focus-on-press (§4.2).
4. Drawing exclusively through `render::SpriteBatch` in `PassId::Screen2D`; no UI-specific renderer (§4.5).
5. Bindings: read-only property paths via `mce_refl`, two-way only for convars on options screens (§4.4).
6. C++ builders were the authoring format at v1.0; `.mcui` documents (M-UI-DATA) are the cooked one (§5.4).
7. RmlUi is the pre-approved fallback, triggered by the M8 mid-point prototype check (§8).
