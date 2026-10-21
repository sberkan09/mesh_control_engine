# M6 — Ember Step v0.5 content (milestone record)

**Scope:** ROADMAP M6's content line: "full Ember Step ships". This covers 5 authored levels, coyote time, jump buffering, wall-slide and dash, crumbling and moving platforms, patrolling and chasing enemies, best-time persistence, particles, parallax, menu/pause/results, keyboard **and gamepad**, and 60 fps documented.

The game is `samples/03_platformer`. Gameplay is the `platformer` game module (`game/`); the host is `main.cpp`.

**Measured on:** the Linux CI container (4 cores, Mesa llvmpipe), `scripts/ci/local_ci.sh`.

| ROADMAP M6 item | Where | Evidence |
|---|---|---|
| 5 authored levels | `game/level.h`: `kLevels`. 1 First Steps, 2 Crumble Bridge, 3 The Chimney, 4 Ember Dash, 5 The Summit. Each level after the first leans on one mechanic; the last uses all of them. | `test_ember_step` "every level loads": one player, one flag and coins; the previous level is fully destroyed. All layouts also passed an offline reachability check (start → every coin → flag) against the movement limits below. |
| Coyote time, jump buffer, variable jump height | `pf_player` (unchanged from M3) | M3 record; the attract bot still clears level 1 (`sample_03_platformer_300`, determinism stage) |
| Wall slide + wall jump | `pf_player`: falling into a wall caps the fall at 3 m/s. A jump kicks away at 8 m/s and up at 12.5 m/s, with 6 wall-coyote ticks and 10 ticks of steering lock. | `test_ember_step` "wall slide caps the fall". "the authored shafts are climbable": a scripted climber wall-jumps up The Chimney's and The Summit's shafts. |
| Dash | `pf_player`: 18 m/s flat for 10 ticks (≈ 3 m), once per air time; ground or a wall recharges it. Bound to the `Dash` action (Fire when a map has no Dash). | "a dash is a flat 3 m burst, once per air time"; "jump + dash clears a 6-tile spike gap" (Ember Dash's first pit, landing on the next pillar) |
| Crumbling + moving platforms | `X` tiles: a `Crumble` component on a static body. Landing starts a 30-tick shake, then `World2D::set_enabled(false)`. It grows back after 150 ticks once the player is clear. Moving platforms are unchanged from M3. | "crumbling blocks give way under the player and grow back" |
| Patrolling / chasing enemies | `pf_enemies` FSM (M3) | M3 record |
| Best-time persistence | A `Records` component (`Save` fields: a best time per level and how many levels are open) on an entity with `SaveKey 1`. The host loads it with `save::load_game` and writes it with `save::save_game` when a run sets a record and again on quit. The save is named `-save ember_step` and lives under the per-user `MCE/EmberStep/saves`; it is used only in interactive play, so headless and autoplay runs stay hermetic. | "best times round-trip through a save payload"; "menu → play → pause → restart → win → results → next level" (new best, unlock, a slower finish keeps the old best) |
| Particles + parallax | Four code-built `kit::ParticleSystem` presets (dust, sparks, embers, debris). The game queues `FxRequest`s and the host bursts a ring of emitters at those positions: jump/land dust, dash and wall-jump embers, coin and flag sparks, stomp and crumble debris. The parallax hills and clouds are from M3. | `sample_03_platformer_gpu` (240 frames, GL debug layer, 0 errors); screenshots below |
| Menu / pause / results | `GameState::mode`: Menu (level select with best times, locked levels shown) → Playing ⇄ Paused (Resume / Restart / Level select) → Results (time, best, NEW BEST, coins, stomps, deaths; Next / Retry / Level select). Menus read `Move.y` edges and Confirm. | the flow test above |
| Keyboard and gamepad | The host's `EmberStep` map (`build_input_map`):<br>• Move: WASD, arrows, left stick or d-pad<br>• Jump: Space or Z; A<br>• Dash: Shift, K or X; X or RB<br>• Pause: Esc or P; Start<br>• Confirm: Enter or Space; A<br>• Cancel: Backspace; B | manual (needs a pad); the bindings go through the same `mce_input` path as `test_input`'s gamepad cases |
| 60 fps documented | — | 600 frames at 1280×720, vsync off, Debug build, llvmpipe software GL, under CI load: 6.5 s on level 1 and 6.3 s on level 5, startup included. That is ≈ 95–100 fps, so a hardware GPU and a Release build have a wide margin. |

Movement limits the levels were checked against: run 7 m/s, jump 13 m/s against 34 m/s² gravity (apex ≈ 2.5 m, ≈ 5.3 m across). That gives these clearances:

- a jump: 4 tiles across or 2 up
- a dash: 7 across
- a wall-jump shaft: at most 3 tiles wide

**Honest gaps:**

- **Art.** The art is the M3 procedural pixel set plus three new layers (crumbling block, particle dot, dash pose), not Kenney's CC0 packs. kenney.nl is unreachable from the build container (HTTP 403), so the "fully arted from CC0 packs" bar is still open.
- **Authoring hours.** The levels were written as ASCII by an agent and validated by a reachability script plus the scripted-climb tests, so there is no human authoring-hours measurement for the F34 spike.
- **Bot coverage.** The attract-mode bot plays level 1 only; levels 2–5 are covered by the mechanic tests, not a full scripted run.

## Screenshots

`/mnt/project-files/m6-2d/ember-step-*.png` (menu, levels 3 and 5).
