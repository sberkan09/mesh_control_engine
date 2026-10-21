# M-CINEMA — Timeline, animation editing, cutscenes and video (milestone record)

**Status:** done on `dev_claude` (steps 1–6, the demo and the gates).
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

## Step 4 — cutscenes (SBA-79)

| Item | Where | Evidence |
|---|---|---|
| A cutscene hold: `World::set_cutscene_held` stops gameplay but runs the systems marked `run_during_cutscenes` (timelines, sequences, animators, attach, sprite animation, particles, effects, captions, follow and cinematic cameras, transforms, audio); a pause hold on top stops those too | `ecs/world.h`, `world.cpp`, each system's install | `test_kit` "cinematic: a cutscene hold stops gameplay only; a pause stops the cutscene's systems too" |
| `CameraRail3D` (Catmull-Rom dolly through up to 8 points, `t` keyed or `speed`, `loop`, `face_path`), `CameraLookAt3D` (target + offset, smoothing) | `kit/cinematic.h`, `cinematic.cpp` ("kit_cinematic") | "cinematic: rails pass through their points, look-ats face their target" |
| `CameraBlend3D` and a camera cut's `"blend"` seconds (key `v[0]`, no format change): the view eases position, orientation and field of view from the previous cut's camera | `timeline.cpp`, `kit3d_extract.cpp` `view_of`, cook `timeline_text.h`, Timeline panel key inspector | "cinematic: a cutscene holds gameplay while its timeline plays and blends between cameras" |
| `CameraShake3D` (trauma model of `CameraShake2D`; `ShakeTrigger` feeds both); `access.motion_scale` scales the view's shake | `effects.cpp`, `kit3d_extract.cpp` | "cinematic: a ShakeTrigger shakes a 3D camera; reduced motion is the view's, not the trauma's" |
| `Cutscene`: hold while its `TimelinePlayer` plays, letterbox bars sliding in and out, hold-to-skip on an action's `held_time` (`kit::timeline_skip`: remaining event keys fire, last animation keys start, state at the end); `kit::cutscene_view` for the player | `cinematic.cpp` ("kit_cutscene"), `timeline.cpp` | "cinematic: holding the skip action skips to the end, fires the remaining events and releases the hold" |
| The stock player draws the bars (under M-ACCESS's subtitle view, which shows the timeline's lines) and the skip prompt with its bar, and switches to the "UI" input map while a cutscene plays | `player.cpp` `add_cutscene_overlay`, `update_ui_map` | `mce_player -capture_ticks 60,240,300` on a rail-camera cutscene scene (offscreen Mesa, 0 GL errors): bars, subtitle, skip prompt, the blend and the wide camera's shake |
| Novel kit: `"timelines"` in story.json, `@cutscene <id>` (op `Cutscene`, mode `Cutscene`); the bot skips it; NPC dialogue skips the statement | `story_compile.py`, `novel.h`, `stage.cpp` | `test_novel` "novel: @cutscene plays a timeline as a cutscene and the story goes on when it ends or is skipped"; `story_compile` |

### Design notes

- **Two holds, not one.** `UiFlow` writes the pause hold every tick and a cutscene must not fight it, so the world
  keeps two flags; a system runs on a held tick only if it is marked for every hold that is on. Pausing over a
  cutscene freezes it; the cutscene alone keeps the camera, animation and sound alive.
- **The blend is timeline time.** A cut's blend is sampled like every state track (elapsed = playhead − key), so
  scrubbing and the editor preview show it; outside a timeline `kit_cutscene` advances `elapsed`. The view blends in
  the extract, so the camera entities themselves never move.
- **Skipping keeps beats.** A skipped cutscene still raises its remaining event keys, so gameplay or a sequence that
  waits on `door_open` does not stall; sounds stay silent.
- **Controls.** The skip action is the UI map's Cancel; the player pushes the UI map during a cutscene like a modal
  screen, so gameplay input (Pause included) waits. A game wanting pause in cutscenes sets `hold_gameplay` off and
  holds what it needs itself.
- Subtitles are not the cutscene's: M-ACCESS's subtitle view shows every timeline's lines with the player's
  subtitle settings, and the bars sit under it.

## Step 5 — video (SBA-80, ADR-0043)

| Item | Where | Evidence |
|---|---|---|
| ADR-0043: pl_mpeg (MIT, MPEG-1 + MP2) over WebM codecs and platform decoders; pinned `c871f2b`, compiled into `mce_render`, the pl_mpeg wall | `docs/adr/0043-video-pl-mpeg.md`, `cmake/Dependencies.cmake`, `check_walls.py` | walls pass |
| `render::VideoDecoder`: open from memory, `show(t)` steps forward or seeks, RGBA8 pictures, `count_frames`, `decode_audio` | `render/video.h`, `video.cpp`, `plmpeg_impl.c` | `test_render` "video: the decoder reads the header and shows the frame at a time" (a committed 2 s red/blue clip: the colour at 0.5 s, 1.5 s and back at 0.2 s, 5 frames decoded for a 0.2 s step), "video: the decoder decodes the sound and refuses what is not MPEG-1" |
| `CHNK_VID` (appended `ChunkType::Video`): header, the MPEG bytes, the sound as 48 kHz f32; `.mpg` / `.mpeg` importer (`audio`, `normalize`), exact length by frame count, a warning above 854x480 | `formats.h` (`VideoHeader`, `VideoView`), `video_importer.cpp`, `scene_importer.cpp`, editor asset type names | `test_cook` "video: .mpg cooks to CHNK_VID with its sound at 48 kHz", "video: a file that is not MPEG-1 fails the cook naming it" |
| `VideoPlayer` and `"kit_video"`: the playhead as sim state (play, end, loop, restart, pause); the sound as one mixer voice started into the clip at the playhead, restarted on a jump or wrap, stopped on pause, mute, speed ≠ 1 or destroy | `kit/video.h`, `kit/video.cpp`, `stock.cpp` | `test_kit` "video: a VideoPlayer plays to its end, loops and restarts", "video: the sound follows the playhead" (offline mixer) |
| Timeline `video` track (`TimelineTrackKind::Video`, appended): a state track setting the target's video, time and playing inside a key's range; scrub and skip show the right frame; the Timeline panel adds and edits video keys | `formats.h`, `timeline_importer.cpp`, `timeline_text.h`, `timeline.cpp`, `timeline_panel.cpp` | `test_kit` "video: a timeline video track drives the player, and a skip stops it" |
| Render: `VideoTextures` (one decoder and texture per shown player, updated in place); a `VideoPlayer`'s `Sprite` draws the film; the stock player draws a `fullscreen` one fitted under the letterbox, subtitles and UI | `kit/video.cpp`, `sprite_extract.cpp`, `player.cpp` `add_fullscreen_video` | `mce_player -capture_ticks 30,150` (offscreen Mesa, 0 GL errors) on a 320x180 test-pattern clip: the sprite shows the frame counter "2" at 2.5 s, the fullscreen splash "0" at 0.5 s |
| Low-tier budget: decode + RGBA convert 1.1 ms/frame at 640x360, 4.0 ms at 1280x720 on one desktop core; the cook warns above 854x480 | ADR-0043 §6, `video_importer.cpp` | measured on the CI container with the pinned pl_mpeg at `-O2` |

### Design notes

- **No `.mcvideo` wrapper.** The ROADMAP named one; an `.mpg` with the usual import sidecar does the same job
  (settings, a GUID) without a second file format.
- **The sound is cooked, not streamed.** The mixer plays only 48 kHz f32 and has no codec; decoding MP2 once at
  cook keeps the run time to pictures. A film's sound costs its length in PCM (about 23 MB for a minute of
  stereo), which suits splashes and cutscene beats, not feature-length video.
- **pl_mpeg's length is short.** It ends at the last packet's time stamp, so the cook counts frames for the exact
  length and the decoder's forward steps carry past pl_mpeg's seek limit.
- **A UI image is not in this step.** A UI image is a region of the skin's one sprite set; a film there needs a
  per-widget texture. The fullscreen view covers the splash case.
- Mac-only check not run: the Metal backend's `update_texture` on the video texture (no Mac here).


## Step 6 — MCP, docs, the demo and the gates (SBA-81)

| Item | Where | Evidence |
|---|---|---|
| `timeline_create`, `timeline_add_track`, `timeline_add_key`: `.mctimeline` edits read back by the cook's reader (a bad edit is refused with its `file:line`, nothing written) and saved canonically; keys merge in by time | `tools_timeline.cpp` | `test_mcp_agent` "timeline tools: create, add tracks and keys; the file stays canonical and cooks" |
| `timeline_preview`: `render_frame`'s capture with every timeline held at a time (the player's `timeline.scrub` convar: `timeline_scrub`, speed 0, a cutscene's bars in) | `tools_render.cpp`, `player.cpp` `scrub_timelines` | `test_mcp_agent` (gpu) "timeline_preview: an agent-authored cutscene seen at several moments": same frame twice at one time, different frames at 0 s and 3 s, the letterbox rows black, `render_sheet` over the playing cutscene, the saved scene and timeline unchanged |
| Video import settings in `asset_set_import_settings`, user guide 5.2 and 14; the timeline tools in user guide 13.5 and 14, `editor-mcp.md` | `tools.cpp`, `docs/user-guide/05-assets.md`, `13-sequences-fsm-saves.md`, `14-mcp-ai-workflow.md`, `docs/architecture/editor-mcp.md` | `capabilities.md`, the transcript golden's tool list |
| Demo: `samples/cinema_demo`, a data-only project the stock player plays: a studio splash film on a fullscreen `VideoPlayer` (a timeline `video` track and an activation track on `VideoPlayer.fullscreen`), then a cutscene: a dolly on a four-point `CameraRail3D` with a look-at, two lamps fading up, the gate rising, music, three subtitle lines, a 1.5 s blended cut to a second camera and a courier walking through on an animation track; `tools/gen_content.py` synthesizes the music and the film | `samples/cinema_demo` | `cinema_demo_player` (two plays hash the same; a held skip changes the run; the skip replayed from its input hashes the same), `cinema_demo_player_gpu` (0 GL errors) |
| A novel scene plays a cutscene: Undelivered opens on `@cutscene ferry` (the ferry's motor and horn, two subtitle lines) | `projects/Undelivered/assets/timelines/ferry.mctimeline`, `story/00_day1.story`, `story.json` | `undelivered_run` "undelivered: a player's inputs start a run, read a page and take a choice" (the cutscene shows a subtitle, then the first page); the bot runs skip it |
| Gate: a recorded cutscene replays to the same hash | `test_cinematic.cpp` | `test_kit` "cinematic: a recorded cutscene with a skip replays to the same state hash" (an `InputRecorder` stream with a held skip, replayed through `InputReplayer`) |

### Design notes

- **The replay gate found a bug.** `Cutscene` had three padding bytes after `pad_`; a fresh component left them
  undefined and the state hash read them, so two runs of the same cutscene hashed differently. `pad_` is now
  four bytes and a `static_assert` keeps `bars` right after it.
- **The fullscreen film was upside down.** The HUD's y runs down the screen; the film is flipped there (the
  sprite path was right). Fixed in step 5's PR; the demo's splash is the check.
- **The demo is a new sample, not Ashen Court.** Ashen Court (`samples/05_ashen_court`) is code only, with no
  scene for the timeline panel to edit, and Solstice Reach's module writes its camera every frame. A data-only
  project shows that a cutscene and a splash need no code at all. The scene and timeline were written as files,
  not in the Timeline panel (no display here); an agent's authoring over MCP is the gpu test above.
- **The stock player records no replay files** (13.4), so the demo's headless check replays its scripted input;
  the recorded-stream replay is the kit test.
- **`timeline_preview` holds every timeline in the scene** at the time; scenes with several unrelated timelines
  scrub them together, which is what a cutscene's preview wants.
- Mac-only checks not run: none new in this step (the Metal video texture is step 5's).

## Gates

| Gate | Evidence |
|---|---|
| A timeline played twice gives the same replay hash | `test_kit` "timeline: two runs hash the same every tick, and a save resumes to the same end" (step 1); "cinematic: a recorded cutscene with a skip replays to the same state hash"; `cinema_demo_player` |
| Scrubbing never mutates the saved scene | the Timeline panel's preview (step 2, "timeline editor: the preview scrubs the play world and never touches the document"); `timeline_preview` (the gpu test compares the files) |
| Video decode inside the Low-tier frame budget | 1.1 ms/frame at 640x360 (step 5); the cook warns above 854x480 |
| `local_ci.sh` green on every step | each step's PR |
