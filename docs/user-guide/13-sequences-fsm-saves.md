# 13. Sequences, state machines and saves

Engine facilities that save you from writing the same gameplay plumbing: scripted action lists for
timed events, keyed timelines for cutscenes (13.5), a validated finite-state-machine helper for AI,
and save games and replays built on reflection.

## 13.1 Sequences

A sequence is an action list an entity plays step by step on the fixed tick. Author it as a
`.mcseq` asset and attach it with `SequencePlayer`:

```json
{ "format": "mce-sequence", "version": 1, "guid": "<uuid>", "name": "intro", "loop": false,
  "steps": [
    {"wait": 0.5},
    {"move_to": [4, 2], "speed": 3, "relative": true},
    {"anim": "attack"},
    {"sound": "guid:<sfx uuid>", "gain_db": -3},
    {"stop_sound": true},
    {"wait_event": "door_open"},
    {"emit": "beat_done"},
    {"call": "shake_camera"},
    {"spawn": "guid:<prefab uuid>", "at": [0, 1], "relative": true},
    {"destroy": true}
  ] }
```

```json
"Hero": { "LocalTransform": { "position": [1, 0, 0] },
          "SequencePlayer": { "sequence": "guid:<intro>" } }
```

| Step | Does |
|---|---|
| `wait` | seconds |
| `wait_event` | waits for a `SeqSignal` with that id |
| `move_to` | moves the entity (its transform, or its kinematic body's velocity) at `speed`; `relative` to where the step started; done within `tolerance` metres (default 0.01) |
| `anim` | pushes a `SeqAnimRequest` for your animation system to act on |
| `sound`, `stop_sound` | plays a one-shot, or stops the entity's `AudioSource` |
| `emit` | pushes a `SeqSignal` other sequences or your systems can wait for or read |
| `call` | a named code hook registered with `SequenceRegistry::register_call` |
| `spawn` | instantiates a prefab at an offset |
| `destroy` | destroys the playing entity |

Rules: one action per step; a looping sequence must contain a waiting step (the cook rejects a
loop of instant steps); timing is tick math, so a sequence that waits 0.5 s waits exactly 30
ticks and a save mid-step resumes on the same tick. `SequencePlayer`'s progress fields carry the
`Save` flag; give the entity a `SaveKey` and the sequence survives a save and load.

From code, `kit::SequenceRegistry::define(name, steps, loops = false)` builds a sequence with the
static step factories (`wait`, `move_to`, `emit`, ...; the code-hook step is `call_step(id)`) and a
`SequencePlayer` with that `name` and no asset plays it. Your
systems read signals with `f.events().read<kit::SeqSignal>()` and push them with
`f.events().push(kit::SeqSignal{...})`.

## 13.2 State machines

`fsm.h` turns an enum on a component into a validated state machine with timed transitions,
guards and entry/exit actions, while the per-state behaviour stays in ordinary systems:

```cpp
enum class Grunt : u8 { Idle, Patrol, Chase, WindUp, Recover, Count };
MCE_ENUM(Grunt, Idle, Patrol, Chase, WindUp, Recover)
struct GruntBrain { Grunt state = Grunt::Idle; u8 pad_[3]; f32 state_time = 0; };
MCE_COMPONENT(GruntBrain, (state, Save), (state_time, Save, ReadOnly))

constexpr fsm::Trans<Grunt> kGrunt[] = {
    {Grunt::Idle,   Grunt::Patrol,  nullptr,      1.5f},                    // timed
    {Grunt::Patrol, Grunt::Chase,   &sees_player, -1.0f, &alert},           // guarded, entry action
    {Grunt::WindUp, Grunt::Recover, nullptr,      0.35f, nullptr, &swing},  // timed, exit action
};

// install():  MCE_TRY(fsm::validate(Span(kGrunt), Grunt::Idle));
// a system:   void grunt_fsm(Frame& f) { fsm::run<GruntBrain>(f, Span(kGrunt)); }
```

Semantics: `state_time` advances first; the current state's rows are checked in table order; at
most one transition per entity per tick; exit runs, then the state changes and `state_time`
resets, then entry runs. `validate` rejects rows that are both timed and guarded (or neither) and
ambiguous duplicates. Register `fsm::StateChanged<Grunt>` as an event to get a notification per
transition. The state and its time are component fields, so they show in the inspector and save
like anything else. Ashen Court's enemies (`samples/05_ashen_court/game/court.cpp`) are three such
tables.

## 13.3 Save games

A save is a reflection dump of the `Save`-flagged fields of every entity that has a `SaveKey`,
applied on top of the base scene on load ("base world plus deltas"). You decide what persists:

1. Give each persistent entity a `SaveKey {"key": <unique number>}` in the scene.
2. Mark the fields to persist with `Save` in `MCE_COMPONENT`. Everything else (asset handles,
   caches, physics bodies) is rebuilt from the scene and your systems.
3. Bump the module version in `MCE_GAME_MODULE` when the meaning of saved fields changes; a
   mismatch is reported in `LoadStats`, not fatal.

The player runtime does the file work: `-save <slot>` loads the slot at start if it exists and
writes it on a clean quit. From code (a custom host or a menu action):

```cpp
save::SaveInfo info; info.scene = scene_guid; info.tick = ticks; info.module_version = module_version;
MCE_TRY(save::save_game(world, "slot1", info));            // <root>/slot1.mcesave, atomic, .bak kept
MCE_TRY_ASSIGN(const save::LoadStats st, save::load_game(world, "slot1", &info, module_version));
```

Files live in the per-user save folder (section 2.5). Unknown components or fields from another
version are skipped and counted; a save from a newer format version fails with `VersionTooNew`
and the path, never a crash. Entities whose key has no live entity are spawned.

What a save is not: a snapshot of the whole World. Physics state, particles and anything without
`Save` restart from the scene, so design saves around checkpoints, not mid-air frames. Saves do not
track streamed chunks: a `SaveKey` on an entity inside a chunk scene is not supported yet (keep
persistent entities in the base scene; section 4.8, `04-scenes-and-prefabs.md`).

## 13.4 Replays

A replay is the per-tick input stream framed with the scene GUID, the tick count and the start
and final state hashes. Replaying it through `InputReplayer` must reproduce the final hash, which is
how the engine's own tests prove determinism. The player does not record replays yet; the pieces
(`InputRecorder`, `save::write_replay`, `read_replay`) are for custom hosts and tests.

## 13.5 Timelines

A timeline is a set of keyed tracks played against the fixed tick: camera moves, doors, lights,
music cues, subtitles. Where a sequence is a list of steps one after another, a timeline lays
everything out on one clock. Author it as a `.mctimeline` asset and play it with `TimelinePlayer`:

```json
{ "format": "mce-timeline", "version": 1, "guid": "<uuid>", "name": "intro", "duration": 6,
  "tracks": [
    {"type": "transform", "target": 0, "keys": [
      {"t": 0, "position": [0, 1, 4]},
      {"t": 3, "position": [0, 1, 0], "rotation": [0, 0.38, 0, 0.92], "ease": "smooth"}]},
    {"type": "property", "target": 1, "path": "PointLight3D.intensity",
     "keys": [{"t": 0, "value": 0}, {"t": 2, "value": 4}]},
    {"type": "activation", "target": 2, "path": "MeshRef.visible", "ranges": [{"from": 1, "to": 3}]},
    {"type": "animation", "target": 3, "keys": [{"t": 0.5, "clip": "walk", "loop": true}]},
    {"type": "audio", "keys": [{"t": 0, "sound": "guid:<music uuid>", "gain_db": -6}]},
    {"type": "camera", "cuts": [{"t": 0, "camera": 4}, {"t": 3, "camera": 5}]},
    {"type": "event", "keys": [{"t": 2.5, "emit": "door_open"}]},
    {"type": "subtitle", "lines": [{"t": 1, "duration": 2, "text": "Who's there?", "speaker": "Mira"}]}
  ] }
```

```json
"Director": { "TimelinePlayer": { "timeline": "guid:<intro>",
                                  "bindings": ["$1", "$2", "$3", "$4", "$5", "$6", null, null] } }
```

A track drives its `target`: `"self"` (the default, the entity with the `TimelinePlayer`) or a
binding index 0 to 7, which names an entity in the player's `bindings`. So one timeline asset can
drive the same moves on different entities.

| Track | Keys | Does |
|---|---|---|
| `transform` | `t`, any of `position`, `rotation` (quaternion `[x, y, z, w]`), `scale`, `ease` | the target's `LocalTransform`, interpolated (rotations slerp) |
| `property` | `t`, `value`, `ease` | any reflected float, vector, color or quaternion field, by path `Component.field` |
| `activation` | `from`, `to` | a bool or integer field: 1 inside a range, 0 outside (`MeshRef.visible`, a light's `enabled`) |
| `animation` | `t`, `clip`, `loop`, `fade` | sets the target's `Animator` clip and restarts it |
| `audio` | `t`, `sound`, `gain_db` | plays a one-shot sound |
| `camera` | `t`, `camera` | camera cuts: that camera is `primary` until the next cut, the track's other cameras are not |
| `event` | `t`, `emit` | pushes a `kit::TimelineSignal`, and a `SeqSignal` so a sequence's `wait_event` can follow the timeline |
| `subtitle` | `t`, `duration`, `text`, `speaker` | the line shown in that range: the stock player draws it as a subtitle (10.7); `kit::timeline_subtitle` reads it |

`ease` shapes the curve from a key to the next one: `linear` (default), `step` (hold, then jump),
`smooth`, `ease_in`, `ease_out`. Values hold before the first key and after the last. The cook
checks property paths against the reflected components, so a typo is a cook error with the file
position. `duration` defaults to the end of the last key.

Playback: `playing` (default on), `speed`, `loop`, and `restart` (set it to play again from the
start; the engine clears it). A non-looping timeline stops at its end with `finished` set; a looping
one wraps and fires its keys again. Timing is tick math, like sequences: a key at 0.5 s fires on
tick 30 at 60 Hz. The player's state is its own component fields, so a timeline is in the state hash,
replays reproduce it, and with a `SaveKey` a save mid-timeline resumes on the same tick.

`kit::timeline_scrub(world, entity, seconds)` moves the playhead and applies the transform,
property, activation, camera and subtitle tracks at that time without firing the cues it passes
(animation, audio, event); playing on from there fires the keys after it. Animate entities without
a dynamic physics body: physics owns a moving body's transform.

### Editing timelines in the editor

The **Timeline** panel edits a `.mctimeline` (double-click one in Assets, or **New...** in the
panel; a new timeline has one event track with a `start` key). Each track is a row on the time
ruler; its keys are diamonds, activation ranges and subtitle lines are bars.

- **Scrub** by dragging on the ruler, or type the playhead's time. **Snap** rounds the playhead and
  dragged keys to 1/30 s, 0.1 s, 0.25 s, 0.5 s or 1 s. The mouse wheel scrolls the ruler; Ctrl +
  wheel or the zoom slider zooms.
- **Keys:** click one to select it and edit it on the right (time, ease, values, clip, signal,
  subtitle text...); drag it to move it; double-click a row to add a key there (it takes the track's
  current value); Delete removes the selected key. **+ Track** adds a track of any type for the
  player itself or one of its bindings (the bound entities are listed by name).
- **Curves:** the selected transform or property track is drawn below as one line per component
  (x red, y green, z blue, w grey), with its keys as dots. Drag a dot up or down to change that value.
  The curve between two keys is the first key's `ease`; there are no free tangents.
- **Preview** shows the open scene in the viewport at the playhead: the editor enters play mode
  paused and scrubs the scene's `TimelinePlayer` that names this timeline, with your unsaved edits.
  **Play** runs it from the playhead; **Pause** hands the time back to the playhead. The scene
  document is never changed by previewing; uncheck Preview (or Stop) to go back to editing.
- **Record:** with Record on, an edit you make in the Inspector or with a gizmo to an entity the
  timeline drives is keyed at the playhead: moving, rotating or scaling keys its transform track,
  a float, vector, color or quaternion field keys a `Component.field` property track. An entity the
  `TimelinePlayer` does not bind yet takes its first free binding. The edit still changes the scene,
  as any edit does.

Every change is checked by the cook before it is kept (a bad path is refused with the cook's
message) and saved as soon as you let go, so the file on disk is always one the cook reads. The
panel has its own **Undo** and **Redo**; the scene's undo (Ctrl+Z) covers the scene side, such as a
binding record mode added. A change written by an MCP tool or a text editor is read in while the
panel has nothing unsaved.
