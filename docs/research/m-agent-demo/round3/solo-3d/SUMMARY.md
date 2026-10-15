# Lantern Court — summary

**Shipped:** `/root/r3/LanternCourt/dist/LanternCourt-1.0-linux-x64.zip`. `project_check` passes with 0 errors, 0 warnings and 0 infos. All 4 `.mctest` scenarios pass, and so does the module's ctest smoke test.

## What the game is
It is a small third-person 3D game. Dusk is falling over a walled temple courtyard. You play a lamplighter who carries a pole with a small flame, and you have to light seven stone lanterns before night falls. A three-minute timer runs while the sky goes from an orange and violet dusk to a deep blue night. The game code moves the sun, the ambient light, the sky gradient, the fog and the exposure toward a `Nightfall` target.

- Each lantern you light eases in a warm point light and an emissive paper shade that feeds the bloom. Each one plays a chime.
- When the seventh lantern is lit, the brazier in the middle of the court bursts into GPU sparks with a whoosh and its own light. The win screen follows. If time runs out you get the lose screen.
- Scene features:
  - Low volumetric ground fog that is lit by the sun and the lanterns.
  - Swaying `Foliage3D` grass in four beds.
  - Paving laid as instanced slabs.
  - Dark puddle decals and red painted lotus marks, drawn with `Decal3D`.
  - Cascaded sun shadows.
  - Image-based lighting from the sky.
- **The shrine** stands at the back. It has no lights of its own. A `GiVolume3D` lights the interior with the sun and lantern light that comes through the door and bounces off the walls. A box-projected `ReflectionProbe3D` makes the polished bronze bell reflect the doorway and the courtyard. A second probe covers the courtyard, and it re-captures each time a lantern is lit.
- **Screens:** title, pause, options, HUD (lantern count and m:ss clock), win and lose. All six `.mcui` documents name one theme, `assets/ui/paper_ink.mcuitheme`: warm paper panels, dark ink text, a vermilion accent and Crimson Pro fonts. Changing that one file re-skins every screen.
- **Options:**
  - Graphics quality has three tabs: Low, Medium and High. They are bound to the saved game convar `lc.quality` and also raise a `Quality` action. Low turns off the volumetric fog, uses one shadow cascade, turns bloom off, keeps 30 % of the grass, and turns off the GI volume and the courtyard probe. Medium uses 2 cascades, shorter fog and 65 % of the grass. High has everything.
  - Music volume is a slider bound to `audio.music_db`.
- **Audio:** a gentle 72 bpm pentatonic loop with no drums (from `gen_music`), a two-tone sine chime and a soft spark burst (both from `gen_sfx`).

## How to play
- Choose **Begin** on the title screen.
- WASD or the arrow keys walk; the mouse turns the camera.
- Stand beside an unlit lantern for about a second. The wick glows and then catches.
- Light all seven before the clock reaches 0:00.
- Esc pauses. From the pause menu you can resume, open the options, start over or go back to the title.

## Layout
- **Scenes:** `assets/scenes/title.mcscene` (the start scene: the court at dusk, an orbiting camera, half the lanterns lit) and `assets/scenes/court.mcscene` (the game).
- **Code:** `games/lantern_court/module/`. Components are `Court`, `Screen`, `Lamplighter`, `Carried`, `FollowCam`, `OrbitCam`, `Lantern`, `Brazier`, `Nightfall`, `PavingField` and `QualityScaled`. The systems handle the round and its screens, the title, the character, the camera, the lanterns and brazier, the paving, the dusk-to-night sky and quality, and the settings.
- **Tests:** `tests/light_all_lanterns.mctest` (a walk_to bot lights all seven and wins), `night_falls.mctest` (lose screen), `pause_and_options.mctest` and `title_flow.mctest`.
- **Goldens:** `goldens/` holds the title, the title's options, the court at start and the court paused.

## What I could not do
- **Lantern shadows.** The kit's `PointLight3D` and `SpotLight3D` have no `cast_shadows` field, so lanterns light the court but do not cast shadows. Only the sun casts shadows, and lantern light passes through walls.
- **SSAO and TAA.** `Environment3D` does not expose them.
- **The engine quality tier.** `r.quality` only applies at restart and is not saved. The options screen therefore sets a game-level quality that the module applies itself.
- **Textures on large surfaces.** Materials have no UV tiling. The court uses flat-coloured meshes and individual paving slabs instead of tiled textures.
- **Wet-looking puddles.** The puddle decals do not show glossy reflections, so they read as dark wet patches.
- **A Quit button.** A game module cannot quit the app. Closing the window quits.
- **Art.** Everything is primitive meshes from `gen_mesh`, and the lamplighter is Kenney's CC0 character.
- **Difficulty.** The game is gentle: a direct route takes about 30 s of the 180 s.
- **Watching and listening.** I could only judge the game through software-rendered stills. I never heard the music or watched it in real time.
