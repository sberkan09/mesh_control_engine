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
- A Text widget's `"text_align"` (`Left`, the default, `Center` or `Right`) places its lines
  across its own rect, so a centered title needs no wrapping Stack: give it the full width
  (`"anchors"` or `"at"` with a width) and `"text_align": "Center"`. In C++ it is
  `Document::set_text_align`. Buttons always center their label.
- A theme restyles every field a widget leaves at its kind's default look, so a field you set *to*
  that default reads as unset and the theme's value wins. `"own_style": ["tint", "text_size"]` says
  "the kind's default, not the theme's" for those fields (`tint`, `text_color`, `accent`, `sprite`
  = region + slice + quad, `text_size`, `letter_spacing`, `text_outline`, `text_shadow`), in every
  state; fields not listed still take the theme's. In C++ it is `Document::set_own_style`; the
  editor's inspector has a "Keep over the theme" group.
- Names are unique among siblings; `"nav"` edges and `Document::find` take paths of names.
- The cook rejects a document the runtime would misread (an unknown kind or field, a duplicate
  name, a nav path that leads nowhere, a value outside its range) with `file:line:col`.
- `ui::extract_document` turns a live document back into data and `cook::write_ui_document`
  writes it as `.mcui` text; cooking that text gives the same chunk byte for byte.
- Over MCP, `ui_create`, `ui_widget_add`, `ui_widget_set` and `ui_widget_remove` edit a document by
  widget path, `ui_theme_set` picks its theme and `ui_preview` shows it at a screen size, safe area and UI scale (chapter 14).

### Editing documents in the editor

The editor's **UI Document** panel (View menu, or double-click a `.mcui` in Assets) edits the
same files:

- **New...** makes an empty document; the combo opens any other one.
- The tree shows the widgets by name and kind. **Add** puts a widget of the chosen kind under the
  selected one (or at the root) with its kind's look and a free name; **Remove**, the arrows and
  **Undo** / **Redo** change the tree.
- The inspector shows the selected widget's fields: text, value and range, the flags, action, bind,
  format and convar, the font role, the anchors, one state's style (with **Every state**, a field you
  change there changes in every state), the stack or grid layout and the nav edges as widget
  paths. Changing the kind gives the widget that kind's look. With nothing selected it shows the
  document's own fields: modal, font and theme.
- The preview draws the document alone, as the player does (the theme and its fonts applied) at
  Phone (390 × 844), Phone landscape, Tablet and Desktop (1280 × 720) sizes. The pointer works in it,
  so buttons show their hover and pressed looks.

Every edit is read back by the cook before it is kept, so one the cook would refuse (a duplicate
name, a bad format) shows the cook's message and changes nothing. When no field is held any more,
the document is saved in the canonical form the MCP tools write and hot-reloaded. A change made on
disk meanwhile (an MCP tool, a text editor) shows up in the panel.

### Screens in a scene (`UiScreen`)

A `UiScreen` component names a `.mcui` document the stock player shows over the game: a HUD, a
title, a pause or a win screen, with no host code. `samples/ui_screens` is a HUD and a pause menu
done this way.

```json
{ "name": "Hud",   "components": { "UiScreen": { "document": "guid:<hud.mcui>" }, "Score": {} } },
{ "name": "Pause", "components": { "UiScreen": { "document": "guid:<pause.mcui>", "order": 10, "visible": false } } }
```

In a split screen (M-VIEWS 4) each player gets their own HUD: a `UiScreenView {"camera": "$<n>"}`
next to the `UiScreen` lays the document out in that camera's view instead of the whole window, at
a UI scale picked for the view's size, with the safe area only on the window's edges. The pointer
reaches it only inside the view. While the camera draws no view the screen covers the window.

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

### Screens that open and close themselves (`UiFlow`)

A title, a pause menu and an options page usually need no game code at all: `UiFlow` on the
screen's entity shows, hides and holds it on input actions (the stock kit installs it).

```json
{ "name": "UI Title", "components": {
    "UiScreen": { "document": "guid:<title.mcui>", "order": 20 },
    "UiFlow": { "hide_on": "Play", "pause": true } } },
{ "name": "UI Pause", "components": {
    "UiScreen": { "document": "guid:<pause.mcui>", "order": 10, "visible": false },
    "UiFlow": { "show_on": "Pause", "hide_on": "Resume", "load_on": "ToTitle",
                "scene": "guid:<main.mcscene>", "pause": true } } }
```

- `show_on` / `hide_on` name the actions that show and hide the screen: a button's `"action"`, a
  panel's Cancel action, Esc (`Pause` once screens are in the scene) or an `-input` script line.
- `load_on` loads `scene`. Loading the scene you are in restarts it, with every screen back at its authored
  `visible`: a title screen that is `visible: true` in that scene shows again. Keep the title in its own scene
  (`title.mcscene`, its Play button's `load_on` loading the level) so that Retry loads the level and "Quit to
  title" loads the title; one scene holding both cannot tell a restart from a first start.
- `quit_on` ends the game, as the window's close button does: `mce_player` quits cleanly (the
  `-save` slot is written), a headless run stops there and play mode in the editor pauses. Game
  code asks the same with `scene::request_quit(world)`.
- `pause: true` holds the game while the screen is visible: gameplay, physics, audio and every
  other fixed-step system stop, while the tick count, the screens and the drawing go on. A held
  tick is part of the run, so a script that pauses and resumes replays with the same state hash.
  Code reads it with `World::held()`, and a system that must run while held registers with
  `World::run_when_held`.

`projects/Undertow` uses all of this (`tools/author_ui_mcp.py`): a title, a pause menu, an options
page with music and effects volume, and a HUD bound to the drone, the same documents drawn in two
themes on two scenes.

The documents use the built-in skin, or their `"font"`, or their theme's fonts (below). A document's
`"sprites"` (or its theme's) names a sprite sheet (`.mcsheet`) the widgets draw from: a style's
`sprite_region` is a region of that sheet, in the sheet's order (a `"grid"`'s cells first, then the
named `"regions"`), its `slice` insets are texels of that region, and `"white_region"` is a region of
plain white texels that flat quads (tracks, checks, carets, sprite_region `4294967295`) draw with.
The widget's tint multiplies the texels, so a skin drawn in greys takes the palette's colors. A sheet
that does not load, or whose `white_region` is not one of its regions, leaves the built-in skin and
logs a warning. With `"filter": "linear"` on the texture, keep a margin of the same texels around
each region (the white one most of all), or the edges blend with their neighbours.

### Themes (`.mcuitheme`)

A theme restyles every screen of a game in one place. It is a `.mcuitheme` asset (JSON source,
cooked to a `UiTheme` chunk):

```json
{
  "format": "mce-uitheme", "version": 1, "guid": "<uuid>",
  "palette": { "panel": [0.04, 0.05, 0.16, 0.92], "button": [0.35, 0.08, 0.32, 0.95], "accent": [0.1, 0.75, 0.7, 1] },
  "fonts": { "body": "guid:<Font>", "title": "guid:<Font>", "button": "guid:<Font>" },
  "sprites": "guid:<SpriteSheet>", "white_region": 0,
  "styles": { "Button": { "style": { "text_outline": 1, "sprite_region": 2, "slice": { "left": 4, "top": 4, "right": 4, "bottom": 4 } } },
              "Text": { "style": { "text_shadow": [2, 2] } } }
}
```

- **`palette`**: the colors every widget kind's look is made of (`UiPalette`: `panel`, `button`,
  `button_hover`, `button_pressed`, `button_focused`, `button_disabled`, `label` for Text widgets,
  `text` for text on buttons, `text_focused`, `text_disabled`, `accent`, `console`,
  `console_text`). Colors it leaves out are the engine's own look, which is the default theme.
- **`fonts`**: a font per role. A widget's `"font_role"` picks one: `Title`, `Body` or `Button`
  (default `Auto`: buttons, toggles and sliders use `button`, everything else `body`). A role with
  no font uses the document's `"font"`, then the built-in one.
- **`sprites`** and **`white_region`**: the skin, a sprite sheet as for a document (above). A
  document's own `"sprites"` wins over its theme's.
- **`styles`**: per widget kind, `UiStyle` fields over every state (`"style"`) or one state
  (`"styles": { "Hover": {...} }`), on top of the palette's look.

A document names its theme with `"theme": "guid:<.mcuitheme>"`, and a `UiScreen` may name one
too (`"theme"`), which wins: two scenes can show one document in two looks. A theme changes only
what the document leaves to it: each style field that is still at its widget kind's default look
takes the theme's value, and a field the document sets (a `"style"` tint, a text size) stays.
`samples/ui_screens` shows its HUD and pause menu under a day theme (`scenes/main.mcscene`) and a
night theme (`scenes/night.mcscene`) with the same two documents.

A project from `mce new` starts with three themes in `assets/ui/themes/`: `pixel` (Pixelify Sans and
Press Start 2P), `clean` (Atkinson Hyperlegible Next and Karla) and `story` (Crimson Pro). Their fonts
are OFL, bake Latin-1 and Latin Extended-A (Turkish included), and are listed in the project's
`CREDITS.md`; chapter 14 says which fits which game. Each has a sprite skin beside it
(`<theme>_skin.png` and `<theme>_skin.mcsheet`: a white region, a panel and a button): `pixel` draws
bevelled buttons with a hard outline, `clean` rounded corners, `story` a paper panel with a double ink
rule. Edit the PNG to restyle every panel and button of the game.

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
| `Saved` | kept between runs in `<save folder>/settings.cfg` (the bus gains, `r.vsync`, `r.quality`, `fullscreen`): the player loads it at start, after the game module's convars exist, and writes the ones changed during the run on quit, on focus loss and when the app goes to the background. A value given on the command line wins for that run and is not written; `-settings 0` neither reads nor writes the file, and captures (`-capture`) never do |
| `Cheat` | refused from the console and the command line in shipped builds (`time_scale`); code may still set it |
| `RequiresRestart` | read at start-up only (`headless`, `r.gl_debug`) |
| `ReadOnly` | reported, never set |

Every convar can be set on the command line as `-name value`, including a game module's own
boot-time switches (`MCE_CVAR` in the module: the player applies them once the module loads and
only then warns about names nobody declared). Game state still belongs in the World (chapter 6.7).
Shipped folders lock cheats before parsing the command line (chapter 15).

## 12.5 Screen readers

The engine draws its own UI, so to a screen reader a game is one picture. The stock player speaks
for it: when focus moves in the menu that owns it (the topmost modal screen, or the console), the
device's reader says the item ("Music, slider, 40%", "Subtitles, switch, on", "Display, tab,
selected"), and a focused slider or switch says its new value as it changes.

| Platform | Reader | Bridge |
|---|---|---|
| Android | TalkBack | `View.announceForAccessibility`, stamped into every game's activity |
| iOS | VoiceOver | `UIAccessibility` announcements (build with `-DMCE_SCREEN_READER_NATIVE=ON`) |
| macOS | VoiceOver | AppKit announcement requests (same option) |
| Windows | Narrator, NVDA, JAWS | UI Automation notifications on the game's window (same option) |
| Linux, web | — | none |

`access.screen_reader` (Saved, on the options screen's Display tab): 0 never, 1 while the OS
reports a reader on (the default), 2 always. What a reader calls a widget is its text; a slider or
switch with no text takes the label just before it in its parent, so the usual "label, then
control" layout reads well. Labels written in capitals are read in sentence case. A control in a horizontal row that starts with a label says
the label first ("Move Right, D, button" in the remap list). Code that wants
the same lines (a custom host, a test) uses `ui::build_access_tree`, `ui::access_line` and
`ui::AccessAnnouncer` (`mce/ui/access.h`) and `platform::screen_reader::announce`.

A novel made with the novel kit also speaks its stage: each page once it is shown, the selected
row of a menu or choice, letters and panels (24.4). Custom code asks
`platform::screen_reader::wanted()` first. On a desktop, `-access_log <file>` writes every
announcement to a file.

The reader hears announcements, not an explorable tree: touch exploration (dragging a finger over
the screen to hear what is under it) is not built.
