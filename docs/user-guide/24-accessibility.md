# 24. Accessibility

Players who cannot hear the sound, see every color, read small text, hold a controller in both hands
or see the screen at all can still play an MCE game, provided the game uses the pieces below.
Most of it is on by default in the stock player. This chapter lists what the player gets, what a
game has to do, and what `project_check` looks for.

## 24.1 What the player gets

Every option is a `Saved` console variable. The player keeps it in `settings.cfg` (11.4), and a
flag on the command line still wins. None of them is written into a component, so two players
with different settings share the state hash, replays and multiplayer included.

| Need | Convars | Where it is explained |
|---|---|---|
| Subtitles and sound captions | `access.subtitles`, `access.captions`, `access.subtitle_speaker`, `access.subtitle_size`, `access.subtitle_background` | 10.8 |
| Color vision | `access.colorblind` (0 off, 1 protanopia, 2 deuteranopia, 3 tritanopia), `access.colorblind_strength` | 24.3 |
| Text size | `access.text_scale` (0.5..3, on top of `ui_scale`) | 24.3 |
| High-contrast menus | `access.high_contrast` | 24.3 |
| Less motion | `access.motion_scale` (0..1: camera shake, flashes, chromatic aberration, motion blur) | 24.3 |
| Remapping, one hand, hold-to-toggle | `<app>.bindings`, `input.toggle_actions`, `input.look_sensitivity` | 11.4 |
| Screen readers | `access.screen_reader` (0 never, 1 while the OS reports a reader on, 2 always) | 12.5, 24.4 |

The options screen that sets all of them is `assets/ui/options.mcui`, which `mce new` puts in every
project. Pause shows it and Back hides it. It has three tabs: Display, Audio and Controls (the
remap list). To give an older project the same screen, copy the template's `options.mcui` (with a
new GUID) and add an entity with:

```json
"UiFlow": { "show_on": "Pause", "hide_on": "Back", "pause": true },
"UiScreen": { "document": "guid:<the options.mcui guid>", "order": 10, "visible": false }
```

Kindle and Undelivered carry it this way.

## 24.2 What a game does

- **Caption its sounds.** Put `SoundCaption` on entities with an `SfxTrigger` or an `AudioSource`.
  For sounds your code plays with `PlaySfx`, push a `kit::ShowCaption` with `kind = CaptionKind::Sound`
  next to it. Kindle's `play(f, clip, "coin")` does both. Voiced lines go on a timeline's
  `subtitle` track or in a `ShowCaption` with a speaker.
- **Name its actions.** The remap list shows the names the `.mcinput` gives each action, so name
  them the way a player would say them ("Jump", not "Action2").
- **Keep effects in the view.** The engine scales the shake and the flashes it owns. A game that
  writes its own shake or flash into a component scales it at draw time by
  `render::accessibility_settings().motion_scale`, and never in the simulation.
- **Use `mce_ui` for menus.** The screen reader, text scale and high contrast reach every
  `.mcui` document. Text a game draws itself (a `TextLabel` in the world, its own HUD) gets none of
  them. The novel kit's stage is the exception: it speaks its own pages and rows (24.4).

## 24.3 Color, text and motion

The color-vision correction (daltonization) filters the game frame. It runs last in the 2D post
pass and in the 3D tonemap pass, after the color grade. It does not filter the UI drawn over the
frame: the menus have `access.high_contrast` instead, which draws every document with
`ui::high_contrast_theme()` (text at 7:1 or more) and outlines bare labels.

`access.text_scale` grows every UI text, and widgets sized by their text grow with it. Pixel fonts
grow in whole steps, so a small change can look the same until the next step.

`access.motion_scale` at 0 turns off camera shake (`CameraShake2D`, `render::ScreenShake`), the 2D
chromatic aberration, 3D motion blur and the flashes the samples draw.

## 24.4 Screen readers

The player speaks the focused item of the menu that owns focus, and a focused slider's new value
(12.5). A visual novel made with the novel kit (21) also speaks its stage, because the stage draws
its own text:

- a page once it is fully shown, with the speaker first ("Mira: Morning.");
- the selected row of the title menu or a choice, with its place ("Wave back, 1 of 2"), and
  "dimmed" for a locked option;
- a letter, and the checkpoint, ending and extras panels.

Each is said once. Nothing is spoken while the options screen is up: it pauses the story, and the
menu speaks instead. Custom code checks `platform::screen_reader::wanted()` (the player's setting
and the OS) before calling `announce`.

On a desktop without a reader, `-access_log <file>` writes every announcement to a text file, one
line each. Use it with `-access.screen_reader 2` to read what a blind player would hear:

    undelivered -access.screen_reader 2 -access_log heard.txt -novel.autoplay 1 -novel.instant 1

## 24.5 `project_check`

The MCP server's `project_check` (14) has an `accessibility` section:

| Finding | Severity | Fix |
|---|---|---|
| A `TextLabel` smaller than 16 px on a 1280 x 720 screen | warning | raise its `height` |
| A `TextLabel` below 4.5:1 contrast against its backing sprite or the clear color | warning | change either color |
| `.mcui` Text, Button and Toggle labels smaller than 16 px at UI scale 1 | warning | raise `style.text_size` |
| `.mcui` labels below 4.5:1 against the nearest opaque panel behind them | warning | change the text or panel color |
| A `.mctimeline` with an audio track and no subtitle track | warning | add the lines on a `subtitle` track |
| A scene entity with an `SfxTrigger`, or an `AudioSource` not on the Music bus, and no `SoundCaption` | info | add a `SoundCaption` |

Contrast follows WCAG AA (4.5:1 for normal text). The `.mcui` check judges a document as the
player draws it: the kind's default look and the document's `theme` applied, the Normal state's size and
color, and the finding names the widget by its path (`menu/buy`). An image behind text is not seen. The scene check sees one backing sprite, so text on a card drawn by a
sprite the check does not pair with it (Undelivered's letter lines) can be flagged though it reads
well. Read a finding against the game before changing it.

## 24.6 What is not there yet

- The novel stage keeps its own text box. It does not follow the subtitle size, background or
  speaker settings.
- Readers hear announcements only. There is no explorable tree: touch exploration and a reader's
  list of the items on screen are not built, and the HUD is never read.
- The iOS, macOS and Windows reader bridges are built only with `-DMCE_SCREEN_READER_NATIVE=ON`
  until they have been run on each device.
- The one-hand presets move keyboard bindings only.
- An `.sfx` asset has no caption field. Captions come from `SoundCaption` tags and `ShowCaption`
  events.
