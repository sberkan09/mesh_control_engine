# ADR-0032: SequenceRunner (action-list scripting)

**Status:** Accepted (2026-10-09). **Milestone:** M6 (SequenceRunner v0.5). **Requirements:** FR-ENT-100.
**Design:** `docs/architecture/scripting.md` §2.6, `docs/architecture/audio-input-ui.md` (contract table, scripting row).

**Context.** Scripted beats (an intro walk, a boss telegraph, a door that opens after a lever) should be data, not
module code. scripting.md §2.6 fixes the shape: a closed step vocabulary, a `SequencePlayer` component whose few
fields are the whole runtime state (so sequences save and resume), one `FixedUpdate_Post` system, and a named call
table as the only code hook. The MVP "lite" runner the design planned for M3 was never built, so this lands the full
v0.5 runner directly.

## Decisions

1. **Asset.** `.mcseq` (JSON, MCE-native) cooks to `CHNK_SEQ` (`ChunkType::Sequence`, appended after `Tilemap`): a
   48-byte header (name, step count, loop flag) and 64-byte `asset::SequenceStep`s, End-terminated by the cook. One
   step = one object with one action key (`wait`, `wait_event`, `move_to`, `anim`, `sound`, `stop_sound`, `emit`,
   `call`, `spawn`, `destroy`) plus that action's own keys; anything else is a cook error at file:line:col. Sound
   (Sfx) and spawn (Prefab) GUIDs are cook dependencies. A looping sequence with no step that waits is rejected by the
   cook and by `SequenceRegistry::define` — it could never yield the tick. Names (`emit`, `wait_event`, `anim`,
   `call`) cook to `StringId` hashes.
2. **Component.** `kit::SequencePlayer` (in `kit/components.h`, so the cook, editor and MCP reflect it) holds the
   asset reference or a code-defined name, plus `active`, `step`, `elapsed`, `anchor` and `anchored`, all `Save`.
   That is the entire progress, so a save mid-wait or mid-move resumes on the same tick: the test cuts a run at
   four points, saves, loads into a fresh world and checks the completion tick and final position.
3. **Timing is tick math.** Each tick a player runs steps until one waits. A waiting step (wait > 0, wait_event,
   move_to) owns its tick, and the step after it starts on the next tick, so waits never share a tick and their
   durations add exactly (0.5 s + 0.25 s at 60 Hz ends on tick 45). Instant steps run in the tick they are reached.
   A Wait counts as done within a thousandth of a tick and a MoveTo arrives within a thousandth of a tick of travel,
   so float sums of `dt` cannot add a tick. A per-player budget of 64 steps per tick stops runaway loops with an
   error.
4. **Steps reach the engine only through existing paths.** `move_to` sets a physics2d body's velocity
   (`World2D::set_linear_velocity`, landing exactly on the last step and zeroing on arrival) or moves `LocalTransform`
   when the entity has no body; relative targets start from where the step began (`anchor`). `sound` pushes
   `audio::PlaySfx` (played by `audio_bridge` on the next tick: its queue is produced in `FixedUpdate_Pre`);
   `stop_sound` removes the entity's `AudioSource` (the bridge's remove hook fades it). `anim` pushes
   `kit::SeqAnimRequest` for the game's animator. `emit` pushes `kit::SeqSignal`; `wait_event` matches any signal with
   its id from the previous tick (signals are produced in `FixedUpdate_Post`). `spawn` instantiates a prefab at a
   position through `ecs::instantiate` (Commands inside the phase), `destroy` destroys the player's entity.
5. **Calls.** `SequenceRegistry::register_call(name, fn)` is the module-facing table; `validate_calls()` reports a
   sequence that names a missing call, and at run time a missing call stops that player on the step with an error.
   The step index moves past a call before the function runs, so a call may replace the player.
6. **Resolution.** Cooked sequences are copied out of the asset once and the asset released; a hot reload (the
   asset's generation moved) re-reads them. Code-defined sequences (`define`) are for tests and generated beats.
7. **Fix found on the way.** `scene::instantiate` registered components without their reflection id, so a world
   that only loaded a scene could not instantiate a prefab (`find_component_by_refl` failed). It now records the id.

## Consequences

- `mce_gameplay_kit` links `mce_audio`, `mce_physics2d` and `mce_ecs_scene` (all below it in the layer table).
- `mce_player` and play-in-editor install the runner next to particles.
- Not built: the editor sequence live view (with editor tooling) and TimelinePlayer (FR-ENT-110, v1.0).

**Note (2026-10-11):** the component header in Decision 2 is `mce/kit/components.h`
(`src/engine/kit/include/mce/kit/components.h`).
