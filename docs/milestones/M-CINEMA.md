# M-CINEMA — Timeline, animation editing, cutscenes and video (milestone record)

**Status:** in progress on `dev_claude` (steps 1–3 merged).
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

## Step 2 — the Timeline panel (SBA-77, FR-EDT-130)

| Item | Where | Evidence |
|---|---|---|
| `.mctimeline` as data: `TimelineDocument` read by the importer and written canonically (defaults omitted, shortest round-tripping floats, one key per line); the importer is now read → `build_timeline_chunk` | `cook/timeline_text.h`, `timeline_importer.cpp` | `test_editor` "timeline editor: the canonical text reads back to the same document and chunk" |
| `TimelineEditor`: open / create / sync, selection, playhead, add / remove track, add / set / move / remove key, duration, name; each edit written canonically and read back by the cook before it is kept; own undo stack of canonical texts, saved on disk | `editor/timeline_editor.h`, `timeline_editor.cpp` | `test_editor` "timeline editor: edits, refusals, undo and redo on disk", "… create makes a cooked timeline" |
| Snapping (`snap_time`), sampling for curves (`sample_timeline_track`, `kit::timeline_eased`) | `timeline_editor.cpp`, `kit/timeline.cpp` | same tests |
| Record mode: `EditorScene::set_edit_observer` sees every `set_component`; transform edits key the transform track (channels merged at one time), float / vector / color / quaternion fields key `Component.field` tracks, an unbound entity takes the first free binding | `editor_scene.h/.cpp`, `timeline_editor.cpp` | `test_editor` "timeline editor: record mode keys inspector edits at the playhead" |
| Preview: a paused play session, the open (unsaved) timeline registered with `TimelineRegistry::define`, the player scrubbed to the playhead; Play runs on from there | `TimelinePreview` | `test_editor` "timeline editor: the preview scrubs the play world and never touches the document" (gate: scrubbing never mutates the saved scene) |
| The ImGui panel: timeline picker and New, Undo / Redo, Preview, Play / Pause, Record, snap, playhead, length, zoom; track rows with key diamonds, range bars and subtitle text; ruler scrub, key drag with snap, double-click to add, Delete; curve view with vertical key drags; key inspector per track type; double-click a `.mctimeline` in Assets; `-editor.open` | `layer/timeline_panel.cpp`, `editor_shell.cpp`, `asset_panel.cpp`, `app/main.cpp` | `mce_editor -project <dir> -editor.open intro.mctimeline -frames 30 -capture` (0 GL errors) |

### Design notes

- **Cut: free tangents.** Curves use the per-key ease presets (linear, step, smooth, ease in, ease out) the runtime
  already plays; Bézier tangents would add a key format change and runtime cost for little gain at this scale.
- **Undo is the panel's own**, like the UI Document panel: one step per run of edits until a save, every step on
  disk. The scene side of record mode (a new binding) is a normal scene edit with the scene's undo.
- **The preview is play mode, paused**, so the document never moves; resuming play plays the timeline on from the
  playhead with the game's systems running.

## Step 3 — animation editing (SBA-78)

| Item | Where | Evidence |
|---|---|---|
| `.mcanim` as data: `AnimGraphDocument` read by the importer and written canonically; the importer is now read → `build_animgraph_chunk`; a state's optional `pos` (editor layout, ignored by the cook) | `cook/animgraph_text.h`, `animgraph_importer.cpp` | `test_editor` "animgraph editor: the canonical text reads back to the same document and chunk"; `test_kit` anim graph case unchanged |
| `AnimGraphEditor`: open / create / sync, states (unique names, clip or 1D blend, loop, speed, events, position), transitions (any-state, conditions, fade, on finish), start state, parameters (refused while in use), canonical read-back, own undo stack saved on disk | `editor/animgraph_editor.h`, `animgraph_editor.cpp` | `test_editor` "animgraph editor: edits, refusals, undo and redo on disk" |
| Clip list of a cooked mesh (`mesh_clips`) | `animgraph_editor.cpp` | same tests |
| Preview (`AnimPreview`): a paused play session; one clip posed at a scrubbed time with the graph set aside, or the entity's graph run with panel parameter values; the live state | `animgraph_editor.cpp` | `test_editor` "animgraph editor: the preview poses clips and runs the graph without touching the document" |
| The ImGui Animation panel: graph canvas (drag, pan, right-click menus, arrows both ways), inspector, clip preview with scrub and play, graph run with parameters; double-click an `.mcanim` in Assets; `-editor.open` | `layer/animation_panel.cpp`, `editor_shell.cpp`, `asset_panel.cpp` | `mce_editor -editor.open walker.mcanim -frames 30 -capture` (0 GL errors) |

### Design notes

- **Cut: keyframe editing of mesh clips.** A mesh clip is node TRS keys inside the cooked mesh (ADR-0022: a clip
  only animates the nodes of its own file), so editing one in the engine would mean a clip format of its own or
  rewriting meshes. Models keep their clips from glTF; keyframed movement of entities and of reflected properties
  is a timeline (step 2), which loops and binds up to eight entities. User guide 8.9 says so.
- The graph's layout lives in the source (`pos` per state), so MCP, text edits and the panel agree on it; the cook
  ignores it, so moving a box never changes the cooked bytes.
- Previewing sets the Animator's clip and time in the play world only; "Run the graph" puts the entity's graph
  back, so the preview never needs the document.
