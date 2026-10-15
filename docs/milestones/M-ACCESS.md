# M-ACCESS — Accessibility (milestone record)

**Status:** in progress (engine gap track, ROADMAP "Engine gap tracks"; Linear SBA-95…99).
**Why:** N15 kept accessibility beyond rebinding out of v1.0; MCE now ships an SDK, store builds and agent-made
games, and stores and players expect these options.
**Measured on:** the Linux CI container (Mesa llvmpipe GL 4.5 and lavapipe Vulkan, Xvfb).

Step order: step 1 (subtitles, SBA-95) waited on M-CINEMA's timeline (SBA-76), so steps 2 and 3 went first.

## Step 1 — Subtitles and captions (SBA-95)

| Item | Where | Evidence |
|---|---|---|
| Caption lines in the sim: `ShowCaption` events, `SoundCaption` tags on an `SfxTrigger` (each play) or an `AudioSource` (a loop while it plays), at most 8 lines, kept in the `Captions` resource; `kit::caption_lines` adds the playing timelines' subtitle lines | `mce/kit/captions.h`, installed by `kit::install_stock` | `test_kit` "ShowCaption lines show for their seconds ...", "a SoundCaption captions its SfxTrigger's plays and its looping AudioSource", "captions leave the state hash alone"; `test_kit` timeline "every track kind plays against the tick" reads Mira's line through `caption_lines` |
| The subtitle view: a box per line centred at the bottom of the safe area, the speaker's name in its own color, sounds bracketed, wrapping at 80 % of the width, the newest three | `ui::SubtitleView`, `ui::subtitle_style()` (`mce/ui/subtitles.h`) | `test_ui` "subtitles show speech by default, sounds when captions are on ...", "subtitle size, background and safe area", "subtitle_style reads the access convars" |
| The player draws it under the scene's UI screens | `PlayerApp::update_subtitles`, `record_overlay` | — |
| The NPC dialogue box follows the subtitle settings | `novel::DialogueBox::update`: the name tag follows `access.subtitle_speaker`, the box opacity scales with `access.subtitle_background` (outlined text below 0.5) | defaults leave the box as styled (the novel goldens hold) |
| Options | `access.subtitles`, `access.captions`, `access.subtitle_speaker`, `access.subtitle_size`, `access.subtitle_background` (Saved), on the template's options screen (Audio tab) | user guide 10.7 |

Limits: the full-screen novel stage (`novel::install`) draws its own text box and does not read the subtitle
settings yet; the dialogue box keeps its text size (its rows are the rectangles the sim hit-tests). Sound
captions come from tags, not from the clip asset: an `.sfx` has no caption field.


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
| Saved convars persist | `console::load_settings` / `save_settings` (SBA-60's settings.cfg) plus `console::saved_generation` so a change is written when it settles | `test_core` "a Saved convar change moves the settings generation", "saved convars round-trip through the settings file" |
| Settings and bindings files | the player: `settings.cfg` and `<app>.bindings` in the save folder, loaded after the input map, written once a change settles for 30 frames and on quit; `-settings 0`, and capture runs skip them | user guide 11.4 |
| The stock screen | `templates/project/assets/ui/options.mcui` (Display, Audio and Controls tabs), on the template's main scene behind `UiFlow` show_on Pause / hide_on Back | the template probe cooks it |

Limits: the one-hand presets move keyboard bindings only (mouse, gamepad and touch stay); the gamepad is
rebound per binding. Look sensitivity applies to the action named `Look`.
