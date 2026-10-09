# ui — runtime UI: retained widget documents and the shipped developer console

`mce_ui` is an L4 library in `src/engine/ui/`. Its public dependencies are `mce_render`,
`mce_input`, `mce_ecs`, `mce_refl` and `mce_core` (`src/engine/ui/CMakeLists.txt`). It draws
retained widget documents (menus, the HUD, the developer console) through `mce_render`'s
`SpriteBatch` in `PassId::Screen2D`. There is no ImGui and no GL code in it (FR-UI-048). It is part
of the `mce_engine` umbrella and is linked by `mce_player_runtime` (which hosts the `DevConsole`),
`samples/ui_gallery`, and `test_ui` / `test_ui_gl`. Gameplay code never links or includes it.

## Public surface

One header, `include/mce/ui/ui.h`, namespace `mce::ui`. Coordinates are pixels, +y down, origin at
the viewport's top-left; offsets are authored at UI scale 1 and multiplied by
`UpdateContext::ui_scale`.

- `Document`: one retained widget tree in a node pool. `add` / `remove` (removal is applied at the
  next `update()`), setters for anchors, styles, text, value and range, visibility, navigation
  edges, stack and grid layout, default focus, `set_blocks_pointer`, `set_modal`.
  `update(UpdateContext)` runs removal, dirty-only layout, input, widget state machines and
  bindings. `submit(batch, sort_layer)` emits quads on `sort_layer` and text on `sort_layer + 1`.
  `events()` returns the last update's `UiEvent`s. `find("a/b/c")` looks up widgets by path.
- `WidgetId`: a generational `Handle`; a removed widget's id resolves to nothing.
- `WidgetKind`: `Panel`, `Image`, `Text`, `Button`, `Toggle`, `Slider`, `Stack`, `Grid`, `Console`.
- `Anchors`: uGUI-style parent-relative min/max anchors, pixel offsets and pivot. `Rect`.
- `Style`: one per `WidgetState` (`Normal`, `Hover`, `Pressed`, `Focused`, `Disabled`); colours are
  sRGB as authored. `apply_default_style` sets the stock look.
- `StackLayout`, `GridLayout`, `NavEdges`.
- `UpdateContext`: viewport, UI scale, the UI action map snapshot (`input::InputState`), pointer
  state, typed text and console keys, presentation `dt`.
- `UiEvent`: `Confirmed`, `ValueChanged`, `FocusChanged`, `Cancelled`, `Submitted`.
- Bindings: `bind_convar` (two-way, changes go through `console::execute`, cheat convars refused)
  and `bind_property(w, world, entity, "Component/field.path", format)` (read-only, resolved once
  through `mce_refl`, numeric and bool leaves only, the widget is touched only when the value
  changes).
- Console widget API: `console_push_line`, `console_clear`, `console_submitted`, `console_input`,
  `console_line_count`.
- `Builder`: fluent authoring sugar (`panel`, `image`, `text`, `button`, `toggle`, `slider`, `stack`,
  `grid`, `console`, then `fill`, `at`, `style`, `tint`, `nav`, `bind_convar`, `default_focus`,
  `blocks_pointer`). C++ builders are the authoring format at v1.0.
- `Skin`, `BuiltinSkin`, `create_builtin_skin(renderer, out)`: a white region plus ProggyClean (MIT)
  at 13 px, printable ASCII, baked by `scripts/gen_builtin_font.py` into
  `src/builtin_font_data.inc`. `builtin_font(...)` returns the tables alone for headless use.
- `kNavigateAction` (`"UI_Navigate"`): the vec2 action navigation reads when the map defines it,
  else `InputState::move`.
- `DevConsole` (`src/dev_console.cpp`): the shipped developer console (FR-UI-045). A modal document
  with one console widget; it mirrors the log ring (`log::read_console`), runs lines through
  `console::execute` (`help`, `clear`), and closes on cancel. The host toggles it and feeds text
  input; `mce_player` uses backtick or F1.

## Rules and invariants

- UI gameplay wall (`scripts/ci/check_walls.py`, path wall): no `mce/ui/` include under
  `samples/*/game/`, or under `games/*/module/` (including `projects/*/games/...` and
  `templates/project/games/...`). UI intents reach gameplay as events through the runtime state, and
  HUD values are read by binding, not pushed by game code (ui.md §4.4, FR-UI-030).
- ImGui wall: `ImGui` may appear only under `src/editor/layer/`. The ctest `ui_no_imgui_symbols`
  also checks the built `mce_engine` and `mce_player` with `nm`.
- Drawing goes only through `SpriteBatch` in Screen2D: at most two MDI groups per document (one
  sprite set, one font). Stack documents two sort layers apart.
- Threading and phase: documents update in `Render_Pre` on the main thread; nothing in `mce_ui`
  runs during sim ticks (ui.md §5.1).
- Determinism (ui.md §5.3): UI never writes gameplay state directly. Focus and navigation run on
  the quantized action snapshot; hover and pointer position are presentation only.
- A modal document always has a focus (`default_focus`, else the first interactable widget). A
  non-modal document (HUD) never takes focus. `Confirmed` fires once per press (edge).
- Layout runs only when the document is dirty (anchors, text, visibility, viewport or scale changed).
- `WidgetId`s never outlive their document. A bound `World` must outlive its property bindings.
- Engine container ban applies.

## How it is tested

ctest `test_ui` (no GPU), `tests/ui/test_ui.cpp`, about 20 cases, including:

- "ui: anchors resolve to pixel-snapped rects at ui scale 1, 1.5 and 2"
- "ui: a static document does no layout work after its first frame"
- "ui: geometric navigation reaches every cell of a 3x3 grid from each corner"
- "ui: confirm held for 30 ticks emits one Confirmed"
- "ui: sliders and toggles bind two-way to convars; cheats are refused"
- "ui: a health bar bound to UiHealth/hp updates on change and is untouched otherwise"
- "ui: a menu draws through SpriteBatch in two groups, text above its panel"
- "ui: the developer console mirrors the log, runs convars and closes on cancel"

GPU tests (label `gpu`, registered only with `MCE_GPU_TESTS`): `test_ui_gl` (pixel probes, two
indirect draws, no GL validation errors), `sample_ui_gallery_{title,options,levels,console}`, and
`scene_demo_player_console_gpu`.

`ui_no_imgui_symbols` runs on Linux when `mce_engine` and `mce_player` are built and `nm` is found.

## Design records

- [Runtime UI architecture](../architecture/ui.md): §2 interface, §3 data model, §4 layout,
  navigation, bindings and drawing, §5 lifecycle, §6 test plan, §7 budgets, §8 RmlUi fallback,
  §9 decisions.
- [Audio, input and UI architecture](../architecture/audio-input-ui.md) §2.5 and §3.9 (the earlier
  sketch ui.md supersedes).
- [M8 runtime UI milestone](../milestones/M8-ui.md): what shipped, budgets, what is not done.

## Known gaps

From the M8 UI milestone record ("Not yet") and ui.md:

- No entity lookup by name: `bind_property` takes the `Entity` and the host finds it.
- The M8 UI record noted that `console::execute` does not check the `Cheat` flag. Packaging v2
  ([M8-packaging](../milestones/M8-packaging.md)) added `console::set_cheats_allowed(false)`,
  which shipped folders call through `samples/common/ship_paths.h`; development builds still allow
  cheat convars from the console.
- Style transitions (`transition_ms`), 9-slice images and world-space UI are not built. Images draw
  one region.
- Replay determinism of a recorded menu session is not tested yet.
- No cooked `CHNK_UI` document format; documents are C++ code (ui.md §5.4).
