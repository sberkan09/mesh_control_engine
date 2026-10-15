# M-ACCESS — Accessibility (milestone record)

**Status:** in progress (engine gap track, ROADMAP "Engine gap tracks"; Linear SBA-95…99).
**Why:** N15 kept accessibility beyond rebinding out of v1.0; MCE now ships an SDK, store builds and agent-made
games, and stores and players expect these options.
**Measured on:** the Linux CI container (Mesa llvmpipe GL 4.5 and lavapipe Vulkan, Xvfb).

Step order: step 1 (subtitles, SBA-95) waits on M-CINEMA's timeline (SBA-76), so steps 2–5 go first.

## Step 2 — Visual options (SBA-96)

Every option is a `Saved` console variable read each frame by the extractors and the player; none is written into a
component, so players with different settings share the state hash (`test_kit` "reduced motion scales the view's
shake, never the camera's state").

| Convar | Range | Effect |
|---|---|---|
| `access.colorblind` | 0 none, 1 protanopia, 2 deuteranopia, 3 tritanopia | color-vision correction, last in the 2D post pass and in the 3D tonemap pass (after the LUT grade, before FXAA's luma) |
| `access.colorblind_strength` | 0..1 | how much of the correction to apply |
| `access.text_scale` | 0.5..3 | UI text size on top of `ui_scale`; widgets sized by their text grow with it |
| `access.high_contrast` | bool | every UI document (images aside) drawn with `ui::high_contrast_theme()`'s colors; bare labels get a black outline |
| `access.motion_scale` | 0..1 | camera shake (`CameraShake2D`, `render::ScreenShake` via `scale_shake`), the 2D chromatic aberration, 3D motion blur and the platformer sample's flash; 0 turns them off |

| Item | Where | Evidence |
|---|---|---|
| Daltonization over the Machado 2009 severity-1 simulation, CPU reference | `render/accessibility.h`, `accessibility.cpp` | `test_render` "color-vision correction keeps greys and moves the colors a viewer confuses" |
| The same math in GLSL, shared by both passes | `render/src/shaders_cvd.h`; `kPostFS` (`PostU.enable.w`, `grade.y`), `kTonemapFS` (`u_cluster.z/w`) | `test_render_gl` "FR-R2D-100: 2D post ..." and "M-ACCESS: the tonemap pass applies the color-vision correction after the grade", on GL and SDL_GPU (lavapipe), within 3–4 levels of the CPU reference |
| Settings folded into the post chains | `apply_accessibility(Post2D&)`, `apply_accessibility(PostFx&)`, called by `SpriteExtract::post_2d` and `Kit3DExtract` | `test_render` "the access.* convars, clamped, and the post chains they change" |
| Text scale and high contrast in `mce_ui` | `UpdateContext::text_scale` / `high_contrast`, `Document::measure` / `submit`, `ui::high_contrast_theme()`, `ui::high_contrast_style()`; set by the player for every document and by the editor's UI Document preview | `test_ui` "text scale grows the text ...", "the high-contrast theme keeps text at 7:1 or more ..." |

Limits: the correction filters the game frame, not the UI drawn over it (the UI has the high-contrast theme), and
on a 3D frame the 2D sprites drawn over it are not corrected. Games that write shake or flashes into their own
components scale them by reading `render::accessibility_settings()` in their render code; the engine scales only
the effects it owns. Undertow (`CameraRig`, `ScreenFlash`, written in its simulation) does not yet.

## Step 3 — Options and remap screen (SBA-97)

| Item | Where | Evidence |
|---|---|---|
| Action labels in the cooked input map (chunk v2: one length-prefixed name per action; v1 still read; id-only maps keep v1 bytes) | `InputMapSet::labels` / `label()`, `InputMapBuilder::action(StringView)`, the `.mcinput` cook (importer v2) | `test_input` "the cooked map keeps action labels (v2) and still reads v1 chunks" |
| Binding names for the UI ("W", "Mouse Right", "Pad South", "Left Stick Up") | `mce/input/binding_names.h` | `test_input` "binding names read the way a player says them" |
| Look sensitivity, hold-to-toggle, one-hand keyboard presets, a bindings generation | `InputSystem::set_look_sensitivity`, `set_toggle`, `apply_hand_preset(Hand)`, `bindings_generation` | `test_input` "look sensitivity scales Look only ...", "one-hand presets move the keyboard to one side ..." |
| The remap list: rows per action (per direction for axis actions), capture on confirm, Toggle switches, preset and reset buttons | `ui::OptionsPanel` (widgets found by name: `remap_list`, `preset_*`, `reset_bindings`), driven by the player's `UiScreens` | `test_ui` "the options panel lists the remappable actions and rebinds by capture", "the Toggle switch sets hold-to-toggle ..." |
| Saved convars persist | `console::write_saved` / `apply_saved` / `saved_generation`, `ConVar::is_default` | `test_core` "Saved convars write to text and read back; the command line wins over the file" |
| Settings and bindings files | the player: `<app>.settings` and `<app>.bindings` in the save folder, loaded after the input map, written once a change settles for 30 frames and on quit; `-settings 0`, and capture runs skip them | user guide 11.4 |
| The stock screen | `templates/project/assets/ui/options.mcui` (Display, Audio and Controls tabs), on the template's main scene behind `UiFlow` show_on Pause / hide_on Back | the template probe cooks it |

Limits: the one-hand presets move keyboard bindings only (mouse, gamepad and touch stay); the gamepad is
rebound per binding. Look sensitivity applies to the action named `Look`.
