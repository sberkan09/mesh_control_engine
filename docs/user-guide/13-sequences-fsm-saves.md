# 13. Sequences, state machines and saves

Three engine facilities that save you from writing the same gameplay plumbing: scripted action
lists for cutscenes and timed events, a validated finite-state-machine helper for AI, and save
games and replays built on reflection.

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
