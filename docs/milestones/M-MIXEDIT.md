# M-MIXEDIT — Editing the audio mixer in the editor (milestone record)

**Status:** in progress 2026-10-20. **Owner: the "Geliştirme rutini 3" thread** (a gap taken from the assessments: no unowned milestone was left). No Linear issues (Linear is unreachable and the workspace is at its free issue limit); this record is the tracker.
Source: `assessments/motor-erisim-bosluklari.md`, section 11 (Editor): "Editörü olmayanlar: ... ses mikseri ...".

**Goal.** A `Mixer` panel shows the open scene's mixer as strips: one per bus (Master, Music, SFX, UI, Voice) with its low-pass, high-pass and reverb send, the two reverb units, the master limiter and the ducks. Today those are `AudioBusEffects`, `AudioReverb`, `AudioLimiter` and `AudioDuck` components that a person finds and edits one entity at a time in the Inspector. The panel edits the same components, so the scene file stays the only source, every change is one undo entry, and the sim hash is untouched.

**Not in scope:** live metering or listening (the editor has no audio device in CI), mixer snapshots (`AudioSnapshot` stays an Inspector component), the per-bus EQ and the bus graph (code-only today).

## Steps

| # | Step | State |
|---|---|---|
| 1 | `MixerModel` in editor core: reads and writes the four component kinds across the scene through `EditorScene` (a setter creates a `Mixer ...` entity, a clear removes the component and the empty entity, one undo entry each), with tests | done |
| 2 | The `Mixer` panel (strips, sliders, reverb and limiter sections, ducks), the Window menu entry, user guide rows | planned |

**Gates:** `test_mixer_model.cpp` in `test_editor` (step 1); step 2 is build-checked (no display in CI).
