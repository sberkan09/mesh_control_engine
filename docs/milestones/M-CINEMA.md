# M-CINEMA — Timeline, animation editing, cutscenes and video (milestone record)

**Status:** in progress on `dev_claude` (step 1 merged).
**Why:** the timeline and curve editor (FR-ENT-110, FR-EDT-130) was cut at M8 only for the solo-hours budget; the
novel kit and the narrative games now want cutscenes, and MCE had no video playback (ROADMAP "Engine gap tracks").
**Measured on:** the Linux CI container (`scripts/ci/local_ci.sh`).
**Linear:** SBA-76 … SBA-81 (milestone "M-CINEMA — Timeline, animation editing, cutscenes, video").

## Step 1 — `.mctimeline` and `TimelinePlayer` (SBA-76, FR-ENT-110)

| Item | Where | Evidence |
|---|---|---|
| `CHNK_TIMELINE` (the reserved `ChunkType::Timeline`): header, tracks, 64-byte keys, string pool; bounds-checked reader | `formats.h` (`TimelineHeader`, `TimelineTrack`, `TimelineKey`, `TimelineView`), `formats.cpp` | `test_kit` "timeline: .mctimeline cooks to CHNK_TIMELINE" |
| `.mctimeline` importer: transform (split into `LocalTransform` position / rotation / scale channels), property, activation, animation, audio, camera, event and subtitle tracks; property paths checked against reflection; sound GUIDs are dependencies | `timeline_importer.cpp` | `test_kit` "timeline: cook errors name the track and the file position" (9 error cases, each with `file:line`) |
| `TimelinePlayer` (timeline, 8 entity bindings, time, speed, playing, loop, finished, restart) and the `timeline_player` FixedUpdate_Post system, installed by `install_stock` before kit3d | `kit/timeline.h`, `kit/timeline.cpp`, `stock.cpp` | `test_kit` "timeline: every track kind plays against the tick" |
| Loops fire every key once per lap; `restart` replays | `timeline.cpp` | `test_kit` "timeline: a looping timeline fires its keys every lap" |
| `timeline_scrub`: state tracks applied, cues skipped | `timeline.cpp` | `test_kit` "timeline: scrubbing applies the state tracks and skips the cues it passes" |
| Determinism: same state hash every tick across two runs; save at ticks 1/45/90/119 resumes to the same end tick and state | `timeline.cpp` | `test_kit` "timeline: two runs hash the same every tick, and a save resumes to the same end" |
| MCP asset kind `Timeline` (`asset_create` / `asset_write`), asset panel filter, scene `AssetRef<Timeline>` | `asset_tools.cpp`, `asset_panel.cpp`, `scene_importer.cpp` | `capabilities.md` row |

### Design notes

- **Targets are bindings, not names.** MCE has no runtime name component, so a track names `"self"` or a binding
  index into `TimelinePlayer::bindings[8]` (scene `"$<index>"` references). One asset drives different entities.
- **Tick math, like sequences.** A key fires on the first tick whose time reaches it within a thousandth of a tick;
  the window is `(fired_to, time]`, and a fresh or restarted player (`fired_to < 0`) includes the keys at 0. A loop
  fires the rest of the lap, wraps with `fmod`, then fires the new lap's keys up to the new time.
- **State tracks vs cues.** Property, activation, camera and subtitle tracks are sampled at the playhead every
  playing tick (and on scrub); animation, audio and event keys fire once when crossed. Writes compare before
  copying, so a held value does not mark the component changed every tick.
- **Activation is a field, not a new flag.** The engine has no generic enabled flag; an activation track names a
  bool or integer field (`MeshRef.visible`, a light's `enabled`).
- **Camera cuts** set `primary` on `Camera3DComponent` / `Camera2DComponent`: the cut's camera on, the track's other
  cameras off. Blends between cameras are step 4.
- **Subtitles** expose the showing line's key index in `TimelinePlayer::subtitle` and the text through
  `kit::timeline_subtitle`; drawing them is M-ACCESS step 1.
- `TimelineRegistry::define(guid, payload)` registers a cooked payload ahead of the asset system, for the editor
  panel's live preview (step 2).
