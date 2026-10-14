# Glowmoth: code agent summary

## What I built (games/glowmoth/module/)

- **Moth** (player controller, kinematic body + CharacterController2D): run, jump with coyote time
  and a jump buffer, one flutter (air jump) per take-off, glide while Jump is held, falling out
  sends the moth back to the start, blink while invulnerable.
- **Lantern**: a sensor that the moth lights by touching it. It swaps to the lit sprite and plays its sound.
- **MoonGate**: opens once every lantern in the scene is lit; entering it loads `next_scene`.
- **Bat**: ping-pong patrol with bob, no body needed. Touching the moth counts a hit and sends the
  moth home; the run's 3rd hit (GameRules.max_hits) loads `lose_scene`.
- **GameRules** ("Game" entity): rules plus runtime mirrors (lanterns_lit/total, hits, complete)
  that the tests assert on.
- **MenuScreen** (title, win, lose): Jump or Confirm after a grace period loads `next_scene`;
  `new_run` on the title resets hits.
- **HudText**: Level / Lanterns / Lives / Hint / Prompt (blinking) text.
- **Scene changes at run time**: the engine has none. The module loads the next scene itself.
  It gets the AssetSystem from `kit::SequenceRegistry::assets()`, registers any component the
  new scene needs, instantiates the scene with `ecs::load_scene` inside FixedUpdate_Pre, and
  destroys every old entity through Commands. Run state (hits) is a World resource, so it
  survives the change. Verified in MCP run_headless and in the real `build/glowmoth` host
  (title -> level 1 by Jump).
- Scene links are **strings** (`"guid:<uuid>"` or `"scenes/x.mcscene"`), not `AssetRef<Scene>`.
  The title -> level -> win -> title loop is a cook dependency cycle otherwise.
- Effect hooks: when a lantern is lit, the gate opens or a bat hits, the code fires any Manual
  ParticleBurst / ShakeTrigger / SfxTrigger on that entity (and on the Player when it is hit).

## Verified
- playtest walk_to finishes level 1 (16 s, 0 deaths); a test_run inline scenario showed lanterns 3/3,
  the gate open and complete, then the scene changing to level 2.
- With scripted input: 3 bat hits -> lose screen -> Jump -> title -> Jump -> level 1 (full loop).

## Tasks finished
#1 input/moth/camera contract, #2 lanterns/gate/state/scene changes, #3 bats and lose, #4 menus
and HUD, #15 way back to the title without a scene AssetRef.

## Friction filed (report_friction)
1. engine_capabilities features list contradicts the components (AnimatedSprite, CameraFollow2D,
   SfxTrigger exist).
2. No run-time scene change; the module cannot reach the AssetSystem.
3. module_build says reloaded but keeps the old image (same `_loaded_<pid>_0.so` name every time),
   so new components stay unknown. Workaround: a fresh server per code change on a second bridge
   socket (the original bridge was left running).
4. AssetRef<Scene> loops fail the cook as a dependency cycle.
5. Module log lines never reach run_headless / test_run log checks.
6. module_test finds no tests (no `<game>_headless_300` in an SDK project).

## End state
All 15 tasks are done. level ran project_check and ship_build (project_watch seq 173-174). All 8
of level's .mctest scenarios pass against the final module (title, both levels, bat hit, three
hits -> lose, win/lose -> title).
