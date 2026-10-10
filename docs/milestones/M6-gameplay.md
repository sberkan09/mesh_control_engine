# M6 — Gameplay: SequenceRunner, FSM helper, lifecycle polish (milestone record)

**Scope:** the SequenceRunner and lifecycle-polish items of ROADMAP M6. Design decisions: ADR-0032. **Measured on:** the Linux CI
container (4 cores), `scripts/ci/local_ci.sh`.

| ROADMAP M6 / FR item | Where | Evidence |
|---|---|---|
| Action-list sequences: wait / wait_event / move_to / anim / sound / stop_sound / emit / call / spawn / destroy (FR-ENT-100) | `.mcseq` → `CHNK_SEQ` (`sequence_importer.cpp`, `SequenceView`); `kit::SequencePlayer` (`components.h`), `kit::SequenceRegistry`, `install_sequences`, `sequence_runner` (`mce/kit/sequence.h`); installed by `mce_player` and play-in-editor | `test_kit` "sequence:" cases: waits complete on the tick their seconds add up to, move_to speed and relative anchors, a kinematic body driven through physics, emit → wait_event on the next tick and anim requests, the call table and a missing call, loops and the never-waiting loop rejection, destroy |
| Serializes and resumes (FR-ENT-100 acceptance) | `Save` fields on `SequencePlayer` | save at four cut points (mid-move, the move's last tick, mid-wait, two ticks before the end) → load into a fresh world → same completion tick and final position |
| Cook | `.mcseq` | cooked asset round trip, a scene whose player walks then spawns a prefab relative to itself; errors name the step (unknown action, foreign key, loop without a wait) |
| FSM helper: timed transitions, entry/exit actions, ordered guards, install-time validation; state + state time are component fields, so inspectable and saveable (FR-ENT-090) | `mce/kit/fsm.h` (header-only): `fsm::Trans`, `fsm::validate`, `fsm::step`, `fsm::run<C>`, optional `fsm::StateChanged<S>` event | `test_kit` "fsm:" cases: a 1.5 s timed transition fires on tick 90 and resets state time; exit → entry exactly once; the first passing guard in table order wins; one transition per entity per tick; the event; every validation error and the unreachable-state count; a save 1.0 s into a state resumes and transitions on the same tick 90 |
| Focus loss / minimize pauses the sim (FR-RUN-060) | `mce_player`: `FocusOut`/`Minimized` raise the pause overlay and mute the master bus, `FocusIn`/`Restored` unmute (`-pause_on_focus_loss 0` turns it off); held input is already cleared on focus loss by `mce_input` | manual (needs a desktop session); the input side is covered by `test_input` focus-loss cases |
| Clean quit flushes the save (FR-RUN-060) | `mce_player -save <slot> [-save_dir <dir>]`: the slot loads on top of the scene at start and is written in `on_shutdown` before teardown (atomic write + `.bak`, `ecs/save.h`) | `extras_2d_save_write` (120 ticks → "written (tick 120)") then `extras_2d_save_load` ("loaded … tick 120" → 60 more ticks → "written (tick 180)") |

Not built: the crash handler / minidump half of FR-RUN-060 (not in the ROADMAP M6 line). *(Built later in `M8-packaging.md`.)*
