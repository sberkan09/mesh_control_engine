# M6 — Gameplay: SequenceRunner (milestone record)

**Scope:** the SequenceRunner item of ROADMAP M6. Design decisions: ADR-0032. **Measured on:** the Linux CI
container (4 cores), `scripts/ci/local_ci.sh`.

| ROADMAP M6 / FR item | Where | Evidence |
|---|---|---|
| Action-list sequences: wait / wait_event / move_to / anim / sound / stop_sound / emit / call / spawn / destroy (FR-ENT-100) | `.mcseq` → `CHNK_SEQ` (`sequence_importer.cpp`, `SequenceView`); `kit::SequencePlayer` (`components.h`), `kit::SequenceRegistry`, `install_sequences`, `sequence_runner` (`mce/kit/sequence.h`); installed by `mce_player` and play-in-editor | `test_kit` "sequence:" cases: waits complete on the tick their seconds add up to, move_to speed and relative anchors, a kinematic body driven through physics, emit → wait_event on the next tick and anim requests, the call table and a missing call, loops and the never-waiting loop rejection, destroy |
| Serializes and resumes (FR-ENT-100 acceptance) | `Save` fields on `SequencePlayer` | save at four cut points (mid-move, the move's last tick, mid-wait, two ticks before the end) → load into a fresh world → same completion tick and final position |
| Cook | `.mcseq` | cooked asset round trip, a scene whose player walks then spawns a prefab relative to itself; errors name the step (unknown action, foreign key, loop without a wait) |
