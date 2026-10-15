# 12. UI and the console

Three kinds of UI exist, and which you use depends on how much of the host you own:

| Need | Use | Available to |
|---|---|---|
| In-world and HUD text | `TextLabel` entities (with `Parallax {0, 0}` for a HUD) | every project, authored in scenes |
| Menus, options, health bars with widgets | `mce_ui` documents, cooked from `.mcui` assets and placed in a scene with `UiScreen` (12.2), or built in C++ | every player-based project (`UiScreen`); programs that own their host (C++) |
| A debugging command line in the shipped game | the developer console (`DevConsole`) | every player-based program, out of the box |

## 12.1 HUD through scene text

A `TextLabel` pinned to the camera with `Parallax {"factor": [0, 0]}` is a HUD element. Your game
writes the text each tick:

```cpp
char line[64];
std::snprintf(line, sizeof line, "COINS %u/%u", coins, total);
label.text.assign(StringView(line));
```

Mark which label shows what with a small component of your own (`HudText {"kind": "Status"}`), so
designers place and style labels in the editor and code only fills them. The play view in the
editor is the viewport's size, so a label authored for a 1280 x 720 window can sit outside a small
viewport; the game window shows it where you meant.

## 12.2 Runtime UI documents

`mce_ui` draws retained widget trees (panels, images, text, buttons, toggles, sliders, stacks,
grids, a console widget) through the sprite batch in screen space, with anchored layout, keyboard
and gamepad navigation, pointer input, focus, and two-way bindings to convars. Documents are built
in C++ with a fluent builder (`samples/ui_gallery/main.cpp`):

```cpp
ui::Builder root(doc, doc.root());
root.panel("bg").fill().tint({0.05f, 0.05f, 0.08f, 1.0f});
ui::Builder col = root.stack("menu", ui::StackLayout{ui::Axis::Vertical, 10.0f, 12.0f}).at({0.5f, 0.55f}, {280, 0});
play_id    = col.button("play", "PLAY").default_focus().id();
options_id = col.button("options", "OPTIONS").id();
col.slider("music", -60.0f, 0.0f, 3.0f).bind_convar("audio.music_db");    // an engine convar, in dB
col.toggle("shake", "SCREEN SHAKE").bind_convar("gallery.screen_shake");  // the host's own MCE_CVAR

// each frame, in the host
doc.update(ctx);                       // viewport, UI scale, the UI action snapshot, pointer, dt
for (const ui::UiEvent& e : doc.events())
    if (e.kind == ui::UiEvent::Confirmed && e.widget == play_id) start_game();
doc.submit(batch, sort_layer);
```

- **Anchors** are parent-relative min/max plus pixel offsets at UI scale 1; the host's
  `UpdateContext::ui_scale` multiplies them. The engine has no UI-scale convar: `ui_gallery`
  declares its own `ui.scale` and passes it (times the display scale) in `ui_scale`.
- **Navigation** is geometric when no edges are authored (a grid is navigable from any corner);
  `Confirm` fires once per press.
- **Modal** documents take focus and block the pointer; a HUD document never takes focus.
- **Bindings:** `bind_convar` is two-way through the console (cheat convars are refused);
  `bind_property(widget, world, entity, "Health/hp")` reads a component field for a health bar
  and touches the widget only when the value changes.
- **Styles** per widget state (normal, hover, pressed, focused, disabled); a built-in skin ships a
  white region and a 13 px pixel font. A style's text look is in pixels at UI scale 1, like
  `text_size`: `letter_spacing`, `text_outline` with `text_outline_color`, and `text_shadow` (offset,
  +y down) with `text_shadow_color`.

The rule that keeps gameplay deterministic: UI never writes gameplay state directly. Menus raise
events the host turns into state changes; HUD values are read by binding. Gameplay modules may not
include `mce/ui/` at all (a build wall enforces it), which is why UI documents live in the host.
The template host is the stock player runtime and does not build documents, so a project that
wants menus today either authors them as scene entities (sprites and labels driven by a small state
machine in the module, the way the MCP-authored games handle their title, pause and win screens) or writes its own host from the
samples.

### UI documents as data (`.mcui`)

A document can also be written as a `.mcui` asset (JSON source, cooked to a `UiDocument` chunk) and
built at run time with `ui::build_document` (`mce/ui/document_data.h`). The stock player shows the
documents a scene names with `UiScreen` (below); a host that owns its loop can build one itself:

```json
{ "format": "mce-ui", "version": 1, "guid": "<uuid>",
  "font": "guid:<Font>", "sprites": "guid:<SpriteSheet>", "white_region": 0,
  "widgets": [
    { "name": "bg", "fill": true, "style": { "tint": [0, 0, 0, 0.6] } },
    { "name": "menu", "kind": "Stack", "at": [0.5, 0.45, 320, 0], "stack": { "spacing": 12 },
      "children": [
        { "name": "play", "kind": "Button", "text": "PLAY", "default_focus": true,
          "styles": { "Hover": { "tint": [0.3, 0.3, 0.4, 1], "slice": { "left": 4, "right": 4 } } },
          "nav": { "down": "menu/volume" } },
        { "name": "volume", "kind": "Slider", "range": [-60, 0, 3], "convar": "audio.music_db" },
        { "name": "tabs", "kind": "Tabs", "tabs": [ { "name": "audio", "label": "AUDIO", "children": [] } ] } ] },
    { "name": "confirm", "modal": true, "children": [ { "name": "yes", "kind": "Button", "text": "YES" } ] } ] }
```

```cpp
ui::DocumentData data;
ui::decode_ui_document(assets.resolve(handle), data);   // the UiDocument chunk
ui::build_document(data, doc);                          // the skin (data.font, data.sprites) is the host's
```

- A widget starts from the look the C++ `Builder` gives its kind, so the JSON above and the builder
  code earlier in this section make the same document. `"style"` changes the `UiStyle` fields it
  lists in every state, `"styles"` in one state; `"anchors"`, `"stack"` and `"grid"` take the
  `UiAnchors`, `UiStackLayout` and `UiGridLayout` fields, by the names the editor shows.
- `"fill": true` and `"at": [x, y, width, height]` are `Builder::fill` and `Builder::at`. A Panel
  with `"modal": true` is `Builder::modal`'s veil; a Tabs widget's `"tabs"` are `Builder::tab`
  pages and its `"value"` is the page shown.
- Names are unique among siblings; `"nav"` edges and `Document::find` take paths of names.
- The cook rejects a document the runtime would misread (an unknown kind or field, a duplicate
  name, a nav path that leads nowhere, a value outside its range) with `file:line:col`.
- `ui::extract_document` turns a live document back into data and `cook::write_ui_document`
  writes it as `.mcui` text; cooking that text gives the same chunk byte for byte.
- Over MCP, `ui_create`, `ui_widget_add`, `ui_widget_set` and `ui_widget_remove` edit a document by
  widget path, `ui_theme_set` picks its theme and `ui_preview` shows it at a screen size, safe area and UI scale (chapter 14).

### Screens in a scene (`UiScreen`)

A `UiScreen` component names a `.mcui` document the stock player shows over the game: a HUD, a
title, a pause or a win screen, with no host code. `samples/ui_screens` is a HUD and a pause menu
done this way.

```json
{ "name": "Hud",   "components": { "UiScreen": { "document": "guid:<hud.mcui>" }, "Score": {} } },
{ "name": "Pause", "components": { "UiScreen": { "document": "guid:<pause.mcui>", "order": 10, "visible": false } } }
```

Gameplay never includes `mce/ui`; it talks to the screens only through data:

- **Showing a screen**: set `UiScreen::visible`. `order` stacks screens (higher on top). While a
  screen whose document is modal (`"modal": true`, the default) is visible, it takes focus and
  the input switches to the `"UI"` action map (Navigate, Confirm, Cancel), which blocks the game's
  map below it. With screens in the scene, Esc and the top-right corner no longer pause the player:
  they are the game's `Pause` action.
- **Values on screen**: a widget's `"bind": "Component/field"` shows that field read-only (a Text
  as `"format"`, a printf format with one `%f` / `%e` / `%g`, default `%g`; other widgets as their
  value). The field is read from the screen's own entity when it has the component, else from the
  first scene entity that does, and the binding waits until one does.
- **Buttons to gameplay**: a widget's `"action": "Resume"` raises that input action. A confirmed
  button presses it for one tick, a slider, toggle or tabs change presses it with `x` = the new
  value, and a cancelled modal panel (Cancel / Esc) presses its own action. The action rides in
  the next tick's `InputState` exactly as an `-input` script line `<tick> Resume` would, so a
  script replays a run with the same state hash. Like a scripted action, it is in the snapshot only
  on its press and release ticks: check `has_action` before reading it.

```cpp
const input::InputState* in = f.try_resource<input::InputState>();
if (in && in->has_action(MCE_SID("Resume")) && in->pressed(MCE_SID("Resume"))) pause_screen.visible = false;
```

The documents use the built-in skin, or their `"font"`, or their theme's fonts (below);
sprite-sheet skins are not drawn yet.

### Themes (`.mcuitheme`)

A theme restyles every screen of a game in one place. It is a `.mcuitheme` asset (JSON source,
cooked to a `UiTheme` chunk):

```json
{
  "format": "mce-uitheme", "version": 1, "guid": "<uuid>",
  "palette": { "panel": [0.04, 0.05, 0.16, 0.92], "button": [0.35, 0.08, 0.32, 0.95], "accent": [0.1, 0.75, 0.7, 1] },
  "fonts": { "body": "guid:<Font>", "title": "guid:<Font>", "button": "guid:<Font>" },
  "styles": { "Button": { "style": { "text_outline": 1 } }, "Text": { "style": { "text_shadow": [2, 2] } } }
}
```

- **`palette`**: the colors every widget kind's look is made of (`UiPalette`: `panel`, `button`,
  `button_hover`, `button_pressed`, `button_focused`, `button_disabled`, `label` for Text widgets,
  `text` for text on buttons, `text_focused`, `text_disabled`, `accent`, `console`,
  `console_text`). Colors it leaves out are the engine's own look, which is the default theme.
- **`fonts`**: a font per role. A widget's `"font_role"` picks one: `Title`, `Body` or `Button`
  (default `Auto`: buttons, toggles and sliders use `button`, everything else `body`). A role with
  no font uses the document's `"font"`, then the built-in one.
- **`styles`**: per widget kind, `UiStyle` fields over every state (`"style"`) or one state
  (`"styles": { "Hover": {...} }`), on top of the palette's look.

A document names its theme with `"theme": "guid:<.mcuitheme>"`, and a `UiScreen` may name one
too (`"theme"`), which wins: two scenes can show one document in two looks. A theme changes only
what the document leaves to it: each style field that is still at its widget kind's default look
takes the theme's value, and a field the document sets (a `"style"` tint, a text size) stays.
`samples/ui_screens` shows its HUD and pause menu under a day theme (`scenes/main.mcscene`) and a
night theme (`scenes/night.mcscene`) with the same two documents.

## 12.3 The developer console

Every player-based program (the stock player, every project host, the samples) has a console:
backtick or F1 opens it, Esc closes it, Up and Down recall lines. It mirrors the engine log and runs
console lines:

| Line | Effect |
|---|---|
| `help` | list convars (except cheat convars) and functions with their help text |
| `clear` | clear the view |
| `r.vsync` | print a convar's value and help |
| `r.vsync 0` | set it |
| `audio.music_db -12` | any convar; strings with spaces go in quotes |
| `time_scale 0.25` | slow motion (a cheat convar: works in development builds, refused in shipped ones) |

`-console 0` disables the console; `-console_open 1` starts with it open. While the console is open
the game receives no key presses (releases still land).

## 12.4 Convars

A convar is a named engine setting with a type, a help string and flags. Chapter 16 lists the ones
the player and the editor define. Flags you will meet:

| Flag | Meaning |
|---|---|
| `Saved` | persisted across runs (the bus gains, `r.vsync`) |
| `Cheat` | refused from the console and the command line in shipped builds (`time_scale`); code may still set it |
| `RequiresRestart` | read at start-up only (`headless`, `r.gl_debug`) |
| `ReadOnly` | reported, never set |

Every convar can be set on the command line as `-name value`, including a game module's own
boot-time switches (`MCE_CVAR` in the module: the player applies them once the module loads and
only then warns about names nobody declared). Game state still belongs in the World (chapter 6.7).
Shipped folders lock cheats before parsing the command line (chapter 15).
