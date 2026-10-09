# 12. UI and the console

Three kinds of UI exist, and which you use depends on how much of the host you own:

| Need | Use | Available to |
|---|---|---|
| In-world and HUD text | `TextLabel` entities (with `Parallax {0, 0}` for a HUD) | every project, authored in scenes |
| Menus, options, health bars with widgets | `mce_ui` documents built in C++ | programs that own their host (the samples); not yet the project template |
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
col.slider("volume", 0.0f, 1.0f, 0.1f).bind_convar("audio.music_db");
col.toggle("shake", "SCREEN SHAKE").bind_convar("game.screen_shake");

// each frame, in the host
doc.update(ctx);                       // viewport, UI scale, the UI action snapshot, pointer, dt
for (const ui::UiEvent& e : doc.events())
    if (e.kind == ui::UiEvent::Confirmed && e.widget == play_id) start_game();
doc.submit(batch, sort_layer);
```

- **Anchors** are parent-relative min/max plus pixel offsets at UI scale 1; `ui.scale` multiplies
  them.
- **Navigation** is geometric when no edges are authored (a grid is navigable from any corner);
  `Confirm` fires once per press.
- **Modal** documents take focus and block the pointer; a HUD document never takes focus.
- **Bindings:** `bind_convar` is two-way through the console (cheat convars are refused);
  `bind_property(widget, world, entity, "Health/hp")` reads a component field for a health bar
  and touches the widget only when the value changes.
- **Styles** per widget state (normal, hover, pressed, focused, disabled); a built-in skin ships a
  white region and a 13 px pixel font.

The rule that keeps gameplay deterministic: UI never writes gameplay state directly. Menus raise
events the host turns into state changes; HUD values are read by binding. Gameplay modules may not
include `mce/ui/` at all (a build wall enforces it), which is why UI documents live in the host.
The template host is the stock player runtime and does not build documents, so a project that
wants menus today either authors them as scene entities (sprites and labels driven by a small state
machine in the module, the way the MCP-authored games handle their title, pause and win screens) or writes its own host from the
samples.

## 12.3 The developer console

Every player-based program (the stock player, every project host, the samples) has a console:
backtick or F1 opens it, Esc closes it, Up and Down recall lines. It mirrors the engine log and runs
console lines:

| Line | Effect |
|---|---|
| `help` | list convars and functions with their help text |
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

Every convar can be set on the command line as `-name value`. Game modules may not declare
convars of their own (a module keeps its state in the World); a custom host may, with `MCE_CVAR`.
Shipped folders lock cheats before parsing the command line (chapter 15).
