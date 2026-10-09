# M8 — Runtime UI `mce_ui` (milestone record)

**Scope:** ROADMAP M8, "runtime UI (FR-UI-010/020/030/045)", as designed in `docs/architecture/ui.md`.
**Library:** `src/engine/ui` (L4; depends on render, input, core). It is part of the `mce_engine` umbrella.
**Measured on:** the Linux CI container (Mesa llvmpipe, Xvfb), Debug build, 1280×720.

| ui.md item | Where | Evidence |
|---|---|---|
| Retained widget tree per document, generational ids | `ui::Document`: a pool of nodes with sibling links. `add` / `remove` are deferred to the next `update()`; a freed slot comes back with a new generation. | `test_ui` "removal is deferred and invalidates ids; find walks paths" |
| uGUI anchored-rect layout, pixel-snapped (§4.1) | `layout_node`: `lo = parent.min + parent.size·a.min + offset_min·scale` (same for `hi`), snapped to whole pixels. A point anchor with no authored size takes the preferred size around its pivot. | "anchors resolve to pixel-snapped rects at ui scale 1, 1.5 and 2"; "a point anchor with no size takes the preferred size" |
| Stack + grid containers | A Stack (vertical or horizontal; spacing, padding, align) sizes itself to its children, and text measures through the font. A Grid fills its cells row-major. | "a stack of three labelled buttons sizes itself to its text"; "a 3x3 grid fills row-major" |
| Dirty-only layout | One document-wide dirty flag, set by anchors, text, visibility, viewport or scale. A static HUD does no layout work per frame. | "a static document does no layout work after its first frame" (1 pass over 121 frames; a resize adds exactly 1) |
| Action-driven focus + navigation (§4.2) | A modal document always has a focus: `default_focus`, else the first interactable widget. Navigation follows explicit `NavEdges` first. Otherwise it is geometric: the lowest `along + 2·perp` in the half-plane of the direction. It reads `UI_Navigate` when the map defines it, else `InputState::move`. A direction steps on its edge, then repeats while held (0.4 s, then every 0.12 s). | "geometric navigation reaches every cell of a 3x3 grid from each corner"; "navigation stops at the edge and explicit edges override geometry"; "holding a direction repeats after the delay"; "the named UI_Navigate action drives navigation" |
| Confirm once per press; cancel | `Confirmed` fires on `confirm_pressed`, which is an edge. `cancel_pressed` emits `Cancelled` on the root. | "confirm held for 30 ticks emits one Confirmed" |
| Pointer: hover ≠ focus, press focuses, release clicks | Hit testing runs in reverse document order. `blocks_pointer` shields what is below. A release away from the pressed widget does not click. | "a pointer press focuses; navigation continues from there"; "a release off the pressed widget does not click; blocks_pointer shields what is below" |
| Non-modal documents (HUD) never take focus | `set_modal(false)` | "a non-modal document never takes focus" |
| Per-state styles | `Style` × {Normal, Hover, Pressed, Focused, Disabled}. Colors are sRGB as authored; the batch gets linear. `apply_default_style` sets the stock look. | screenshots |
| Convar binding, two-way (§4.4, FR-UI-030) | Sliders, toggles and string labels bind to a convar. A UI change goes through `console::execute`, so it is logged. Convar changes show up on the next `update()` without an event. Cheat convars are refused. | "sliders and toggles bind two-way to convars; cheats are refused" |
| Property-path binding, read-only (§4.4, FR-UI-030) | `bind_property(w, world, entity, "Health/hp", "HP %.0f")`. The component and field resolve once through `mce_refl` (the inspector's path syntax; numeric and bool leaves only). Each `update()` reads the field and touches the widget only when the value changed: a Text re-formats, a slider or bar takes the value. A dead entity reads nothing, and the binding is dropped with its widget. | "a health bar bound to UiHealth/hp updates on change and is untouched otherwise": 30 unchanged frames do 0 binding updates and 0 layout passes; a change updates exactly the 2 bound widgets |
| Console widget (FR-UI-045) | Line editing (UTF-8-safe backspace), Enter submits (`Submitted` + `console_submitted()`), a 32-line history on Up/Down, and a 256-line scrollback ring. | "the console edits, submits and recalls history" |
| Shipped developer console (FR-UI-045) | `ui::DevConsole`: a modal document over the top of the screen. It mirrors the log ring (`log::read_console`, the whole ring, so startup warnings show; warnings yellow, errors red). It runs lines through `console::execute`; `help` lists convars (cheats hidden) and console functions, and `clear` empties the scrollback. Esc closes it. **`mce_player`** opens it with \` or F1 (`-console 0` removes it; `-console_open 1` starts with it open). While it is open it owns the keyboard and platform text input is on. | `test_ui` "the developer console mirrors the log, runs convars and closes on cancel"; GPU `scene_demo_player_console_gpu` (0 GL errors); screenshot `player-console.png` |
| Drawing through `SpriteBatch`, ≤ 2 MDI groups (§4.5, FR-UI-020 acceptance) | Quads use the skin's white region on `sort_layer`; text uses the font on `sort_layer + 1`. That makes one group per set, with text above its document's quads whatever the set ids are. | `test_ui` "a menu draws through SpriteBatch in two groups"; GPU `test_ui_gl`: pixel probes of panel, focus tint and label ink, 2 indirect draws, 0 GL validation errors |
| Built-in skin | `create_builtin_skin`: ProggyClean (MIT) baked at 13 px by `scripts/gen_builtin_font.py` into `builtin_font_data.inc` (printable ASCII), plus a white texture. | "the built-in font covers printable ASCII at 13 px" |
| ImGui stays out of the runtime (FR-UI-048) | `ui_no_imgui_symbols`: `nm` over `libmce_engine.so` and `mce_player` must find no `ImGui` symbol. The same scan over `mce_editor` finds 2,283, so the test can fail. | ctest `ui_no_imgui_symbols` |
| Gameplay never includes `mce/ui` (FR-UI-030) | `check_walls.py` "UI gameplay wall": no `mce/ui/` include under `samples/*/game/`, `samples/*/module/`, `games/*/game/` or `games/*/module/`. | walls stage |
| Demo | `samples/ui_gallery` has four pages:<br>• title menu<br>• options bound to convars, including `ui.scale` itself<br>• 3×3 level grid with locked cells skipped by navigation<br>• console<br>Keyboard and mouse both work, F1 toggles the console, and `Window::set_text_input` is new in `mce_platform`. | `sample_ui_gallery_{title,options,levels,console}` (GL debug layer, 0 errors); screenshots |

## Budgets (ui.md §7)

| Item | Budget | Measured (Debug, llvmpipe container) |
|---|---|---|
| `update()` of a menu, nothing dirty | < 0.02 ms | 0.019–0.023 ms for a whole page (options: 14 widgets plus the HUD document) |
| update + submit + batch build of a page | — | 0.13–0.17 ms |
| Draw cost | ≤ 2 MDI groups per document | 2 for page + HUD together; 4 with the console open on its own layer (each document stays at 2) |

## Not yet

- **Entity lookup by name.** The runtime has no entity names, so `bind_property` takes the `Entity` and the host finds it (ui.md wrote `player/Health/hp` with a name in front).
- **Cheat convars can still be set from the console.** `console::execute` does not check the `Cheat` flag; a shipped-build guard belongs with packaging v2.
- **Style transitions (`transition_ms`), 9-slice images and world-space UI** are not built. Images draw one region.
- **Replay determinism of a recorded menu session** (ui.md §6, last row) is not tested yet. `update()` is a pure function of the context, so it should hold.

## Screenshots

`/mnt/project-files/m8-ui/ui-gallery-{title,options,levels,console}.png`, `/mnt/project-files/m8-ui/player-console.png`
