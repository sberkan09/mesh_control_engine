# 13. Sequences, state machines and saves

Engine facilities that save you from writing the same gameplay plumbing: scripted action lists for
timed events, keyed timelines for cutscenes (13.5), a validated finite-state-machine helper for AI,
behavior trees for AI as data (13.8), and save games and replays built on reflection.

## 13.1 Sequences

A sequence is an action list an entity plays step by step on the fixed tick. Author it as a
`.mcseq` asset and attach it with `SequencePlayer`. In the editor, a double click on a sequence in Assets opens the Sequence panel (chapter 3): the steps as a list you can add to, reorder and edit, saved only if the cook accepts them. The file format is:

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
    {"type": "camera", "cuts": [{"t": 0, "camera": 4}, {"t": 3, "camera": 5, "blend": 1.5}]},
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
drive the same moves on different entities. `bindings` is a fixed array of 8: write all 8 entries (`"$<id>"`
or `null`), as above; fewer is an error, not "the first few".

| Track | Keys | Does |
|---|---|---|
| `transform` | `t`, any of `position`, `rotation` (quaternion `[x, y, z, w]`), `scale`, `ease` | the target's `LocalTransform`, interpolated (rotations slerp) |
| `property` | `t`, `value`, `ease` | any reflected float, vector, color or quaternion field, by path `Component.field` |
| `activation` | `from`, `to` | a bool or integer field: 1 inside a range, 0 outside (`MeshRef.visible`, a light's `enabled`) |
| `animation` | `t`, `clip`, `loop`, `fade` | sets the target's `Animator` clip and restarts it |
| `audio` | `t`, `sound`, `gain_db` | plays a one-shot sound |
| `camera` | `t`, `camera`, `blend` | camera cuts: that camera is `primary` until the next cut, the track's other cameras are not; `blend` seconds ease the view over from the previous cut's camera (13.6) |
| `event` | `t`, `emit` | pushes a `kit::TimelineSignal`, and a `SeqSignal` so a sequence's `wait_event` can follow the timeline |
| `subtitle` | `t`, `duration`, `text`, `speaker` | the line shown in that range: the stock player draws it as a subtitle (10.8); `kit::timeline_subtitle` reads it |
| `video` | `t`, `video`, `duration`, `gain_db` | the target's `VideoPlayer` plays that video from `t` for `duration` seconds (0, the default: to its end), then stops on its last frame (13.7) |

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
property, activation, camera, subtitle and video tracks at that time without firing the cues it passes
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

**Over MCP** an agent writes a timeline with `timeline_create`, `timeline_add_track` and
`timeline_add_key` (the same format, checked by the cook before it is written) and looks at it with
`timeline_preview`, which draws the scene with every timeline held at a given second (chapter 14).
The player does the same with `-timeline.scrub <seconds>`.

## 13.6 Cutscenes

A cutscene is a timeline played with a `Cutscene` component next to its `TimelinePlayer`
(`mce/kit/cinematic.h`, installed by the stock player, play-in-editor and `run_headless`). While the
timeline plays:

- **Gameplay is held.** The world's gameplay systems stop, as under a pause screen, but the
  timelines, sequences, animators, particles, cameras, transforms and audio play on. Your own system
  can play on too: `world.run_during_cutscenes(phase, "my_system")` after you add it. A pause
  screen (`UiFlow`, 12) over a cutscene stops all of it. Turn `hold_gameplay` off for a cutscene the
  game keeps running under (a novel's, below).
- **Letterbox.** The player draws black bars `letterbox` of the screen high (0.12 by default; 0 for
  none) that slide in and out over `letterbox_time` seconds, under the scene's UI screens.
- **Subtitles.** The timeline's subtitle lines show over the lower bar, like every timeline's
  (the player's subtitle view and its settings, 10.8).
- **Skip.** With `skippable` on, the player shows `skip_prompt` ("Hold to skip") and a bar that
  fills while `skip_action` (the UI map's `Cancel`: Esc, gamepad B) is held. Held for `skip_hold`
  seconds (1 by default) the cutscene skips: the event keys after the playhead still fire, so
  gameplay waiting on a beat hears it, each animation track's last clip starts, sounds stay silent,
  the state tracks jump to the end and the player finishes. `skipped` records it.

While a cutscene plays, the player switches the controls to the "UI" input map, so the game's own
actions (Pause included) wait until it ends. `active`, `bars` and `skip_progress` are engine-written;
`hold_gameplay` lifts on the tick after the timeline ends (on the same tick when it is skipped).

```json
"Intro": { "TimelinePlayer": { "timeline": "guid:<intro>", "bindings": ["$2", "$3", "$4", null, null, null, null, null] },
           "Cutscene": { "letterbox": 0.1, "skip_prompt": "Hold Esc to skip" } }
```

**Cinematic cameras.** Put these on a `Camera3DComponent` entity; a timeline's camera track cuts
between them and property tracks drive their fields.

| Component | Does |
|---|---|
| `CameraRail3D` | a dolly: the camera moves along a Catmull-Rom curve through the first `count` of its 8 `points` (a scene or `component_set` must still write all 8 `[x, y, z]` entries, unused ones `[0, 0, 0]`; a shorter array is an error). `t` 0..1 is the place on the path (0 the first point, 1 the last); key it with a property track (`CameraRail3D.t`) or give it a `speed` (with `loop` the path closes). `face_path` turns the camera along the path |
| `CameraLookAt3D` | turns the camera to face `target` plus `offset`; `smoothing` (1/s) eases it, 0 snaps |
| `CameraShake3D` | trauma shake of the view: up to `max_translation` metres and `max_rotation` radians at trauma 1, growing with trauma². Set `trauma` (a property track keys it) or fire a `ShakeTrigger`, which shakes 2D and 3D cameras. The Reduce motion setting (`access.motion_scale`) scales the view's shake |
| `CameraBlend3D` | eases the view from `from`'s view (position, orientation, field of view) to this camera's over `duration` seconds. A camera cut with `"blend"` writes it; set it yourself to blend outside a timeline |

```json
{"type": "property", "target": 4, "path": "CameraRail3D.t", "keys": [{"t": 0, "value": 0}, {"t": 4, "value": 1, "ease": "smooth"}]},
{"type": "camera", "cuts": [{"t": 0, "camera": 4}, {"t": 4, "camera": 5, "blend": 1.5}]}
```

All of it is sim state, so a cutscene replays and hashes the same, and the Timeline panel's preview
shows the rail, look-at and blend at the playhead. `samples/cinema_demo` is a whole one as data: a splash
film, a dolly with a look-at, lamps, a rising gate, music, subtitles and a blended cut. In a novel, `@cutscene <timeline>` plays one
(21).

## 13.7 Video

A `VideoPlayer` plays a film: on its entity's `Sprite` (a screen in the world), or over the whole
screen as a splash (`mce/kit/video.h`, installed by the stock player, play-in-editor and
`run_headless`).

**Import.** The engine plays MPEG-1 with MP2 sound (ADR-0043). Convert any film with ffmpeg and drop
it in `assets/`:

```sh
ffmpeg -i intro.mp4 -vf scale=640:-2 -c:v mpeg1video -q:v 4 -c:a mp2 -f mpeg intro.mpg
```

The editor writes the `.mpg.mceasset` sidecar (`importer: "video"`). Its settings: `audio` (keep the
sound, default true) and `normalize` (raise the sound to -1 dBFS, default false). The cook decodes
the sound once, so playing it costs the mixer what any sound costs, and warns about a film larger
than 854x480, which a Low-tier phone may not decode inside its frame.

| Field | Does |
|---|---|
| `video` | the film |
| `playing` | on plays (the default); off pauses. The engine turns it off when a non-looping film ends and sets `finished` |
| `loop` | start again at the end |
| `restart` | set it to play from the start; the engine clears it |
| `time` | seconds into the film; set it to jump |
| `speed` | playback rate; the sound plays only at 1 |
| `gain_db`, `sound` | the sound's gain, and whether it plays |
| `fullscreen` | draw it over the whole screen, fitted with black bars, under the letterbox, subtitles and UI, in place of drawing on the `Sprite` |

```json
"Splash": { "VideoPlayer": { "video": "guid:<intro.mpg>", "fullscreen": true } },
"TV":     { "LocalTransform": {}, "Sprite": { "size": [3.2, 1.8] },
            "VideoPlayer": { "video": "guid:<news.mpg>", "loop": true, "gain_db": -12 } }
```

The playhead (`time`, `playing`, `finished`) is sim state: replays and saves see it and a film
plays the same on every run. The pictures are decoded each frame for what is shown. A timeline's
`video` track plays a film as one beat of a cutscene (13.5): skipping or scrubbing the cutscene shows
the right frame. To leave a splash when it ends, watch `finished` in a sequence or your own system.


## 13.8 Behavior trees

A behavior tree (`.mcbt`) decides what an enemy or an NPC does next, as data. An `AiBrain` component
runs one on its entity every fixed tick, and a `Blackboard` component holds the values the tree reads
and writes. Behavior trees are the next step up from a state machine (13.2): a tree reads as a plan,
and a designer or an agent can change it without touching code.

```json
{ "format": "mce-bt", "version": 1, "guid": "<uuid>", "name": "guard",
  "blackboard": { "alert": {"type": "bool", "default": false}, "alarms": {"type": "int", "default": 0},
                  "target": "entity" },
  "root": {"selector": [
    {"sequence": [{"check": "alert", "value": true}, {"emit": "alarm"}, {"set": "alarms", "add": 1},
                  {"set": "alert", "value": false}], "name": "alarm"},
    {"sequence": [{"wait": 2}, {"task": "step_patrol"}], "name": "patrol"}
  ]} }
```

```json
"Guard": { "SaveKey": { "key": 7 }, "LocalTransform": {}, "AiBrain": { "tree": "guid:<guard.mcbt>" } }
```

Every node is an object with exactly one kind key, plus that kind's options and an optional `name`
that the debugger shows:

| Node | Does |
|---|---|
| `sequence: [..]` | runs its children in order; fails at the first failure, succeeds when all have |
| `selector: [..]` | runs its children in order; succeeds at the first success, fails when all have |
| `parallel: [..]`, `policy` | runs every child each tick. `"all"` (the default) succeeds when all have and fails at the first failure; `"one"` succeeds at the first success |
| `invert`, `succeed`, `fail: node` | flips the child's result, or forces it once the child finishes |
| `repeat`, `retry: node`, `count` | runs the child again after each success (repeat) or failure (retry), `count` times in all; 0 means forever |
| `cooldown: node`, `seconds` | after the child finishes, fails without running it for `seconds` |
| `timeout: node`, `seconds` | fails, and stops the child, when it runs longer than `seconds` |
| `guard: node`, `key`, `op`, `value`, `abort` | runs the child only when the condition holds as it starts; with `abort` it keeps watching (below) |
| `check: key`, `op`, `value` | the condition as a result. Ops are `is_set`, `not_set`, `==`, `!=`, `<`, `<=`, `>` and `>=`; the default is `==` with a `value`, else `is_set` |
| `wait: seconds` | runs that long, then succeeds |
| `set: key` with `value`, `add`, `clear: true` or `from: key` | writes the key, then succeeds. An entity key's value is `"self"` |
| `emit: signal` | raises a `kit::BtSignal`, readable on the next tick |
| `task: name`, `key`, `value`, `vector`, `arg` | a stock task (below) or a function your game registers by name |

Keys are `bool`, `int`, `float`, `vec3` or `entity`, with an optional `default`. A tree has at most 16
bool, int and float keys, 8 vec3 keys and 8 entity keys. Only numbers compare; a vec3 or entity key
tests `is_set`, and an entity key that names a destroyed entity counts as unset. From game code, read
and write keys by name:

```cpp
kit::bb_set_entity(world, guard, "target", player);
f32 alarms = 0;
if (kit::bb_number(world, guard, "alarms", alarms)) { ... }
```

Tasks are the code hook. A task gets the entity, its blackboard, the node's arguments and a float of
memory of its own. It returns `Running` until it is done, and it is called once more with `halting`
set when a parent stops it early:

```cpp
BtStatus step_patrol(kit::BtTaskCall& t) {
    if (t.entering) t.memory = 0;  // the waypoint index lives in the node's memory
    ...
    return arrived ? BtStatus::Success : BtStatus::Running;
}
world.resource<kit::BtRegistry>().register_task("step_patrol", &step_patrol);
```

The stock tasks need no code. Each takes its own named arguments:

| Task | Does |
|---|---|
| `move_to`, `key`, `radius` | walks to a vec3 key, or after an entity key, with the entity's `NavAgent3D` (navmesh) or `PathAgent` (grid). Runs until it arrives or comes within `radius`; fails on no path or no agent. A parent that stops it stops the agent |
| `look_at`, `key` | turns the entity toward a vec3 or entity key: about +Z on a 2D entity (one with a `PathAgent`, `Sprite` or `AnimatedSprite`; +X faces it), else about +Y (−Z faces it) |
| `animate`, `clip`, `finish` | plays the clip on the entity's `AnimatedSprite`, or on its `Animator` when that has no graph. With `finish: true` it runs until a Once clip finishes |
| `play_sound`, `sound`, `gain_db` | plays an Sfx asset (`"guid:<uuid>"`) once, through `audio::PlaySfx` |
| `spawn`, `prefab`, `key`, `offset` | instantiates a prefab at the key's place (or the entity's), plus `offset` |
| `raise`, `event` | raises a signal that `wait_event` and sequences' `wait_event` (13.1) hear on the next tick |
| `wait_event`, `event` | runs until that signal is raised (by `raise`, `emit` or a sequence's `emit`) |
| `log`, `text` | writes the text to the log with the tree's name and the entity |
| `make_noise`, `loudness` | makes a noise perceivers hear from `loudness` metres (below) |

```json
{"selector": [
  {"guard": {"sequence": [{"task": "move_to", "key": "thief", "radius": 1}, {"task": "animate", "clip": "grab", "finish": true}]},
   "key": "alert", "value": true, "abort": "lower", "name": "chase"},
  {"repeat": {"sequence": [{"task": "move_to", "key": "post_a"}, {"wait": 0.5}, {"task": "move_to", "key": "post_b"}]},
   "name": "patrol"}
]}
```

A guard with `abort` watches its condition every tick, not only as it starts:
- `"self"` stops the guard's running child as soon as the condition fails, and the guard fails.
- `"lower"` (only on a selector's child) stops the selector's later, lower-priority branch as soon as
  the condition holds, and the selector runs the guard's branch instead. Above, an alert stops the
  patrol mid-walk.
- `"both"` does the two.

The tasks that were stopped hear it as `halting`, like any other stop.

How a tree runs:
- A running node carries on next tick where it was. A sequence or selector resumes at its running
  child and does not look at the earlier ones again until the tree starts over (unless an earlier
  guard aborts lower branches). Without an abort, an alert raised mid-patrol is noticed when the
  patrol finishes.
- When the root finishes, `AiBrain.result` and `runs` record it, and the tree starts again on the
  next tick. Repeat and retry also restart their child on the next tick, so a tree never loops
  within one tick.
- Timing is tick math, as with sequences: `wait` 0.5 s completes on its 30th tick at 60 Hz.
- `AiBrain.active` off pauses the tree where it is; `restart` starts it again from scratch.
- A brain whose entity has no `Blackboard` gets one on its first tick and starts on the next. Key
  defaults are written when the brain starts, unless the key is already set. When you write a `Blackboard`
  yourself, `numbers` (16), `vectors` (8) and `entities` (8) are fixed arrays: pass every entry (`null` for an
  unset entity); a shorter array is an error.

Everything a tree knows is in `AiBrain` and `Blackboard`. Trees are deterministic, they are in the
state hash and in replays, and a save (give the entity a `SaveKey`) resumes them on the same node.
`BtRegistry::validate_tasks()` names a task that a loaded tree uses but your game never registered;
such a node fails and is logged once. The design is ADR-0045. `samples/ai_demo` is a data-only project
in which the stock player runs guards that patrol, investigate, take turns attacking and flee (the
demo, at the end of this section).

### Perception

A `Perceiver` gives an AI eyes and ears, and writes what it notices into its tree's blackboard. A
`Stimulus` is something it can see. A `Noise` is something it can hear.

```json
"Guard": { "LocalTransform": {}, "AiBrain": { "tree": "guid:<guard.mcbt>" },
           "Perceiver": { "sight_range": 10, "sight_angle": 100, "forget": 4, "team": 1 } },
"Thief": { "LocalTransform": {}, "Stimulus": { "team": 2 } }
```

- **Sight.** Each check, every `Stimulus` of another team inside the cone (`sight_angle` degrees in
  all, around −Z in 3D or +X in the 2D plane, from the `eye` offset) and within `sight_range` times
  its `visibility` is tested with a ray. In 3D the ray goes through the kit's physics world, and in
  2D (`space: Plane2D`) through physics2d's. Colliders in the `mask` layers block it. The nearest one
  in sight becomes the target.
- **Which way it looks.** Forward is the **world** rotation of the entity that holds the `Perceiver`,
  applied to −Z (3D) or +X (2D plane). The `eye` offset turns with it. So a `Perceiver` on a child
  (an `Eyes` entity under a guard) looks where the child's world yaw points: the parent's yaw plus
  the child's own. `NavAgent3D` with `face_motion` already turns the entity it moves toward its
  velocity, so a second system that also turns the child (a "face the player" script, a `Pivot`)
  doubles the turn and the cone points off. Put the `Perceiver` on the entity that faces, and let
  one thing turn it. To see where it points, call the MCP tool `perception_debug_lines` (below).
- **Memory.** A target out of sight is remembered for `forget` seconds. `awareness` falls from 1 to 0
  over that time, and then the target is dropped.
- **Hearing.** A `kit::Noise` event (push one from code, or use the `make_noise` task) is heard within
  its `loudness` times the perceiver's `hearing`. A noise from the perceiver's own team is not heard.
- **The blackboard.** If the tree declares them, the perceiver writes `target` (entity; cleared when
  forgotten), `sees` (bool), `last_seen` (vec3) and `heard_at` (vec3; it stays set until the tree
  clears it). The key names are fields of the `Perceiver`, so two trees can use their own names.
- **A crowd.** `interval: 4` checks sight every fourth tick, staggered by entity, so 400 guards cast
  rays for 100 of them per tick. `kit::PerceptionStats` counts the checks and rays each tick.

A guard that reacts the tick it sees someone puts the chase first, behind a guard that aborts the
lower branches:

```json
{"selector": [
  {"guard": {"task": "move_to", "key": "target", "radius": 1}, "key": "target", "abort": "lower"},
  {"guard": {"sequence": [{"task": "move_to", "key": "heard_at"}, {"set": "heard_at", "clear": true}]},
   "key": "heard_at", "abort": "lower"},
  {"repeat": {"sequence": [{"task": "move_to", "key": "post_a"}, {"task": "move_to", "key": "post_b"}]}}
]}
```

Perception runs just before the trees in each tick, and its state is in the `Perceiver`, so it is
deterministic and saved. `kit::perception_debug_lines` returns each perceiver's cone and a line to its
target, for a debug overlay.

Over MCP, `perception_debug_lines {"ticks": N}` plays the saved scene headless and returns, for every
`Perceiver`, its world `eye`, `forward` and `yaw_deg` (0 looks along −Z, 90 along −X), `sight_range`,
`sight_angle`, whether it `sees`, its `target` and `last_seen`, plus `lines`: the cone edges and range
arc, and a line to the target it sees or last saw (`from`, `to`, `color` as RGBA). A guard that seems
to look 90° off shows it at once: compare `forward` with the direction it walks.

### Choosing by score, sharing tokens, finding a spot

Three more pieces let a tree weigh options instead of only checking them in order.

**Utility.** A `utility` node's children are `score` nodes. Each tick every score is computed, and
the child with the highest one runs (on a tie, the first). A score is `base` plus, for each
consideration in `by`, `weight` times a response curve of a blackboard key:

- The key's value is mapped from `from`…`to` onto 0…1 and clamped. A bool or vector key counts 1
  when set and 0 when not. `invert: true` turns 0…1 around.
- `curve` shapes it: `linear` (the default), `power` (to the `exponent`, 2 by default), `logistic`
  (an S around 0.5, steepness `exponent`, 10 by default) or `step` (1 at or above `exponent`, 0.5 by
  default).

A running child keeps running until another one scores more than `hysteresis` above it, so an AI
does not flicker between two close options. When no child scores above 0 the node fails.

```json
{"utility": [
  {"score": {"task": "attack"}, "base": 0.1,
   "by": [{"key": "health", "from": 0, "to": 100, "curve": "power", "exponent": 2}]},
  {"score": {"task": "move_to", "key": "cover"}, "by": [{"key": "health", "to": 100, "invert": true}]}
], "hysteresis": 0.1}
```

**Tokens.** An `AiTokenPool` component (a `name` and a `capacity`, up to 8) hands out a limited
number of tokens, so only two of ten enemies attack at once. A `token` node takes one when it starts
and fails if none is free; it gives it back when its child finishes or is halted, and a dead holder's
token is freed. The pool is the one on the entity in `key` (an entity key) or, without `key`, the
first pool in the scene with that `pool` name:

```json
{"selector": [
  {"token": {"task": "attack"}, "pool": "attackers"},
  {"task": "move_to", "key": "flank"}
]}
```

**Point queries.** The stock task `find_point` tries points on `rings` rings (1–8, 2 by default) of
`count` points each (1–32, 8 by default) out to `radius` (5 by default) around the entity, or
around the `around` key. Points off the navmesh, or on a blocked `NavGrid` cell, are skipped. With
`from` (a vec3 or entity key, such as a threat), `need: "visible"` or `"hidden"` keeps only the
points with or without a clear line of sight from it. `prefer` picks among the rest: `near` the
entity (the default), `far` from `from`, or `close` to it. The best point goes into the `store` vec3
key and the task succeeds; with none, it fails. Hiding from the player and moving there:

```json
{"sequence": [
  {"task": "find_point", "store": "cover", "from": "target", "need": "hidden", "radius": 8},
  {"task": "move_to", "key": "cover"}
]}
```

`kit::bt_utility_scores` returns a utility node's current scores, for a debug view.

### Editing and debugging trees in the editor

The **Behavior Tree** panel edits a `.mcbt` (double-click one in Assets, or **New...** in the
panel; a new tree is a selector over a one-second wait). The tree is drawn as an outline: each line
is a node's kind, its value (a task's name, a check's key, a wait's seconds) and its `name`.

- **Right-click a node** to add a child of any kind (a composite or decorator comes with a `wait 0`
  placeholder child, a check or set names the first key), wrap it in a decorator or composite,
  unwrap it, move it up or down among its siblings, or remove it with its subtree.
- **The inspector** shows the selected node's value and members as JSON (`"text"`, `2`, `true`,
  `[0, 1, 0]`). Edit one and press Enter; **x** removes a member; **+ Field** adds one (`abort`,
  `radius`, `by`...).
- **The blackboard** lists the keys with their type and default. Renaming a key renames it in the
  nodes that name it; a key a node still uses cannot be removed.
- **Problems** lists what the cook says about the tree, with line and column. Every edit is cooked
  before it is kept, and one that breaks a tree that cooks is refused with the cook's message. A
  tree that does not cook when you open it (a hand edit) can be edited until it does.

Edits are saved as soon as you let go, so the file on disk is always the tree the panel shows. The
panel has its own **Undo** and **Redo**, and reads a change an MCP tool or a text editor wrote
while nothing is unsaved.

**While playing**, the panel debugs a brain running the tree (pick it from the list when several
do). Each node is coloured by its status: green while it runs (the active path), blue when it last
succeeded, red when it last failed, grey when it has not run. A utility node's children show their
current scores in brackets, and the blackboard shows each key's live value. **Run edits** runs the
tree as it is now in the play world (its brains start again) without saving. The viewport draws
each `Perceiver`'s sight cone and a line to the target it sees (green) or last saw (yellow); turn it
off with **Perception** in the panel or the View menu.

### Writing and checking trees over MCP

An agent edits trees with four tools (chapter 14). `bt_create` writes a new `.mcbt` from a
`blackboard` (a key's type, or `{"type", "default"}`) and a `root` in the `.mcbt` node form above.
`bt_add_node` adds a node under `parent` (by default the root) at `index`. `bt_set` changes a node's
`value` or `fields` (`null` removes a field), replaces it with another subtree (`replace`) or removes
it (`remove`), and changes the tree's `name` and its blackboard keys (`null` removes a key). Every
answer is the tree as an outline, one line per node, numbered in the order the tools use:

```
0 parallel
1   sequence  "listen"
2     task "wait_event"
...
5   selector
6     guard  "chase"
...
12    guard  "investigate"
```

Each edit is cooked before it is written, so a mistake (a key the tree does not have, a second child
under a decorator, a field a node does not take) fails with the cook's message and the file stays
as it was. What is written is the canonical form the editor's panel also writes.

`bt_trace` plays the open scene headless (save it first) and reports what one entity's brain did:
every `every` ticks, the tree's result, how many times it ran, the running path and the blackboard,
and in `started` the tick each named or leaf node started running. That is how an agent checks a
tree it wrote:

```
bt_trace {"entity": "Guard", "ticks": 1200, "every": 200}
  rows: {"tick": 200, "running": "0 parallel > 5 selector > 12 guard \"investigate\" > 13 sequence > 15 task \"move_to\"",
         "blackboard": "sees=false heard_at=(8.00, 0.10, 8.00) alert=false ..."}, ...
  started: ["tick 2: 19 repeat \"patrol\"", "tick 182: 12 guard \"investigate\"",
            "tick 305: 6 guard \"chase\"", "tick 498: 11 wait 1000"]
```

The trace is deterministic: the same scene and ticks report the same thing.

### The demo

`samples/ai_demo` is a data-only project the stock player runs. In `scenes/yard.mcscene` a guard
patrols between two posts on the navmesh, hears a thief's noise, leaves its patrol to look, spots
the thief and catches it, and a bell's alarm reaches it through `wait_event`. In
`scenes/yard_squad.mcscene` (`mce_player -scene scenes/yard_squad.mcscene`) two guards run one
utility tree: a thrown stone makes them investigate; when they see the thief they take turns
attacking through the thief's `AiTokenPool` of one token while the other waits its turn, and a guard
the thief strikes back at loses health, so its flee score wins and it falls back to a point far from
the thief that `find_point` picks, giving the token to the other guard.
