# Glowmoth: summary

## What the game is

Glowmoth is a short 2D pixel-art platformer set on a meadow at night. You play a moth. Each of
the two levels hides three lanterns. Light all three and the moon gate at the end of the level
opens. Bats fly back and forth between the flowers and swoop down to them at each end. A bat
that touches you costs one of your three lives and sends you back to the start of the level.

- **Title** (`scenes/main.mcscene`, the start scene): a flapping moth, the moon and the controls.
- **Level 1** (`scenes/level1.mcscene`, 64 m): low steps, one shallow ditch and two slow bats
  that stay above a moth walking on the ground.
- **Level 2** (`scenes/level2.mcscene`, 80 m): higher blocks, a lantern down in a ditch, a
  lantern on the top ledge and six faster bats that swoop down to ground level. You have to
  time your way past them.
- **Win screen** after level 2 and **lose screen** after the third hit. Both go back to the
  title.
- The art is drawn with `gen_pixels` (moth, bats, lanterns, gate, flowers, moon, sky, hills).
  The ground tiles come from `gen_tileset` and use a night palette. The sound effects come from
  `gen_sfx`, and the four chiptune loops (title, meadow, win, lose) come from `gen_music`.
  Every asset has a row in `CREDITS.md`.

## How to play

| Input | Action |
|---|---|
| Arrows / A D (left stick) | run |
| Space (gamepad A) | jump. Press it again in the air to flap once; hold it while falling to glide |
| Space / Enter | start, or continue from the win and lose screens |

Touch a lantern to light it. The HUD shows `LANTERNS n/3   LIVES n`. When all three are lit,
the banner reads "THE MOON GATE IS OPEN!". Fly into the gate to finish the level.

Run it with `mce play Glowmoth`, or unzip `Glowmoth/dist/Glowmoth-1.0.0-linux-x64.zip` and run
`Glowmoth/Glowmoth`.

## How it is built

The game code is in `games/glowmoth/module/`. Its components are `MothController`, `Lantern`,
`MoonGate`, `Bat`, `HudText` and `MenuScreen`, and it adds six systems. Hits carry over from
level to level in a `GlowState` resource. Scenes change with `scene::request_change`.
Everything else is engine data: Sprite, AnimatedSprite and `.mcsheet` flipbooks, TilemapLayer
and `.mctilemap` (painted with `tilemap_paint`), CameraFollow2D with bounds, Parallax layers,
Post2D vignette, and AudioSource music.

Checks:

- 8 `.mctest` scenarios in `tests/`, all passing:
  - the title starts level 1
  - a walk_to bot finishes level 1 into level 2
  - a walk_to bot finishes level 2 into the win screen
  - hopping blindly into bats reaches the lose screen
  - the gate stays shut while its lanterns are unlit
  - the win screen and the lose screen each go back to the title
  - a masher bot on level 2 logs no errors
- Goldens for all five screens are in `goldens/`.
- `module_test` passes, and `project_check` passes with 0 errors, 0 warnings and 0 infos.
- `ship_build` 1.0.0 made `dist/Glowmoth-1.0.0-linux-x64.zip` and its headless smoke run
  passed.

## What I could not do or chose not to do

- **Gameplay limits.** The walk_to bot cannot double-jump, so no step on the main route is
  higher than 2 tiles and there are no pits. Level 2 is harder because it has more bats, they
  are faster and they fly lower, not because the platforming is harder. The bat phases in
  level 2 were tuned so that the bot's run gets through. A person has to time the bats.
- **Lighting.** The lanterns' glow is a halo drawn into the lit sprite. I did not use 2D
  lights, because those would need normal maps on every sprite.
- **Untested.** I never heard the audio, only looked at its waveforms. I never played the game
  in a window; every check was headless or an offscreen render.
- **Component change.** The server did not pick up a field added to an existing component
  (`Screen`), so I renamed the component to `MenuScreen` (friction note 3).
- **Friction notes.** I filed 5 notes in `Glowmoth/friction.jsonl`:
  - a wrong statement in the docs about tilemap tools
  - the docs do not say that `MCE_LOG_*` takes `{}` placeholders
  - `module_build` does not reload the reflection of a changed component (major)
  - how a walk_to bot's arrival ends a run and how a scene change counts as a death
  - the bot cannot double-jump
