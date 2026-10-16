# 10. Audio

Audio is a 48 kHz stereo float mixer on its own thread with 48 real voices, buses with EQ and a
limiter, sample-accurate scheduling and 3D attenuation. From a game you touch it in three ways: an
`AudioSource` component for sounds that belong to an entity, a `PlaySfx` event for one-shots, and
convars for the bus volumes.

## 10.1 Sounds that belong to an entity

```json
"Torch": {
  "LocalTransform": { "position": [4, 1, 0] },
  "AudioSource": { "clip": "guid:<crackle.wav>", "loop": true, "play_on_start": true, "gain_db": -6 }
}
```

| Field | Meaning |
|---|---|
| `clip` | the `.wav` or `.ogg` asset |
| `gain_db`, `pan` | −60..12 dB, −1..1 |
| `priority` | 0..255; louder and higher priority voices survive when voices run out |
| `bus` | `Master`, `Music`, `SFX` (default) or `UI` (`audio::AudioBus`; scenes written with the number, `"bus": 1`, still load). `Music` streams the clip in 100 ms pages instead of holding it decoded (two music streams at a time; a third plays from memory) |
| `loop` | gapless; the clip's loop points come from the WAV's own markers or the sidecar's `loop_start` / `loop_end` |
| `play_on_start` | start when the entity appears |
| `fade_s` | seconds the voice fades in when it starts and out when the component or entity is removed |

Changing `gain_db` or `pan` from code ramps over 10 ms (no clicks). Removing the component stops
the voice with a short fade (`fade_s` when it is longer). Music is a looping `AudioSource` on the
`Music` bus; to crossfade two tracks, give both a `fade_s`, remove the old source and add the new
one in the same tick.

## 10.2 One-shots from gameplay

Push a `PlaySfx` event in `FixedUpdate_Pre` (the bridge plays it in the same tick):

```cpp
// in install(): world.register_event<audio::PlaySfx>(Phase::FixedUpdate_Pre);
void play(Frame& f, const asset::AssetRef<asset::Sfx>& clip, f32 gain_db = 0) {
    if (clip.guid.is_null()) return;
    audio::PlaySfx ev;
    ev.clip = clip.guid;
    ev.gain_db = gain_db;
    f.events().push(ev);
}
```

Keep the sound references as `AssetRef<Sfx>` fields on your components (`jump_sound`,
`hurt_sound`, `Coin::sound`) so designers pick them in the inspector. A one-shot with
`attenuation_id` and `position` set plays in 3D.

Without code, `SfxTrigger` on an entity plays its `clip` once: `gain_db`, `gain_jitter_db` (each
play adds a value in ± that many dB, drawn from the entity and play count, so replays match), `pan`
and `bus` (`Master`, `Music`, `SFX`, `UI`). `on` picks when it fires: `Manual` when its `fire` flag is set (by data, a sequence field track or MCP `entity_set_component`; the engine clears it), `Start` on the first tick, `TriggerEnter` / `ContactBegin` when a 2D physics event names the entity (as the sensor, the other body or either contact body), `Landed` when a 3D character on the entity touches down, `Skid` when a wheel of the entity's `Vehicle3D` starts to skid. `fired` counts the plays. It plays as a `PlaySfx` on the next
tick. For a different clip or pitch on each play, name a sound set (10.3) as its `clip`.

## 10.3 Sound sets and pitch

A footstep that plays the same sample every time sounds mechanical. A **sound set** (`.mcsound`)
names several clips and picks one on each play, with a pitch and gain inside a range:

```json
{ "format": "mce-sound", "version": 1, "guid": "<uuid>",
  "clips": ["guid:<step1.wav>", "guid:<step2.wav>", "guid:<step3.wav>"],
  "pick": "random", "no_repeat": true, "pitch": [0.92, 1.08], "gain_db": [-3, 0] }
```

| Key | Meaning |
|---|---|
| `clips` | the sounds to pick from (`.wav` or `.ogg` assets; not other sets) |
| `pick` | `random` (default) or `sequence` (in order, then round again) |
| `no_repeat` | `random` never picks the clip it picked last (default true) |
| `pitch` | playback rate range, 0.25..4: 2 is an octave up and twice as fast, 1.06 about a semitone |
| `gain_db` | added to the play's own gain |

A sound set is an Sfx asset, so it goes wherever a sound does: `AudioSource.clip`,
`SfxTrigger.clip`, `PlaySfx.clip`, and sequence and timeline sound keys. The pick comes from the tick
and the source (the entity, or the event's place in the tick), never from a clock or a global random
number, so a replay of the same ticks picks the same clips and the sim hash does not move. A
`sequence` set advances on each play. An `AudioSource` picks when it starts and keeps that clip
while it loops. MCP `asset_create` makes one (`"path": "sfx/steps.mcsound"`).

From code, `PlayParams::pitch` plays any clip at a rate (`AudioDevice::set_handle_pitch` changes
it on a live voice, say for an engine's revs). Between samples the mixer interpolates linearly; at
1 it reads the samples as they are. Streamed clips (the `Music` bus) always play at 1.

## 10.4 3D sound

- `AudioListener` on the camera entity (the first in ECS order is the ear; its transform gives
  position and orientation, and its `gain_db`, −60..12, scales everything it hears).
- `AudioSpatial {"attenuation_id": 1}` next to an `AudioSource` makes it 3D: the bridge follows the
  entity's position every tick. Curve 1 is an inverse-distance curve by default; a custom host can
  define curves 1..31 (id 0 is reserved for plain 2D) (`AudioDevice::set_attenuation`) with inverse, linear or exponential
  falloff and min/max distances.
- 2D games can ignore all of this; a source without `AudioSpatial` is plain stereo.

## 10.5 Volume, buses and effects

Five saved convars drive the named buses: `audio.master_db`, `audio.music_db`, `audio.sfx_db`,
`audio.ui_db` and `audio.voice_db` (the `Voice` bus, for dialogue). Set them from the console (`audio.music_db -12`), the command line, or bind them to
sliders in a runtime options menu (chapter 12). `audio.enabled 0` runs silent. When the window loses focus
or minimizes, the player pauses the sim (the pause overlay waits for the player on return) and
mutes the master bus until focus comes back (`-pause_on_focus_loss 0` turns both off). On a
phone, the app going to the background pauses the audio device itself (`AudioDevice::set_paused`):
voices keep their place and pick up where they stopped when the app comes back.

**Bus effects.** Three components put effects on the mixer from the scene. Put them on any entity
(a "Mixer" entity in the level is a good home), and timelines and sequences can key their fields:

| Component | Fields | What it does |
|---|---|---|
| `AudioBusEffects` | `bus`, `lowpass_hz`, `highpass_hz`, `reverb_unit`, `reverb_send_db` | A low-pass and a high-pass on one bus (12 dB/octave, 0 = off) and a post-fader send into a reverb unit (-96 = none). Master filters everything and has no send. |
| `AudioReverb` | `unit` (0 or 1), `room_size`, `damping`, `width`, `wet_db` | A Freeverb reverb. Buses that send to it are heard in it; its return goes into Master. |
| `AudioLimiter` | `ceiling_db`, `release_ms` | Holds the output under the ceiling, adding 5 ms of latency. |

A cave level that makes its footsteps ring and muffles the music:

```json
{ "name": "Mixer", "components": {
    "AudioReverb": { "unit": 0, "room_size": 0.9, "damping": 0.3, "wet_db": -3 },
    "AudioBusEffects": { "bus": "SFX", "reverb_unit": 0, "reverb_send_db": -6 } } },
{ "name": "Music mix", "components": {
    "AudioBusEffects": { "bus": "Music", "lowpass_hz": 1200 } } }
```

- One entity per bus and per reverb unit counts: the first in ECS order wins. Removing it (or changing
  scene) puts that bus back to dry and unfiltered, or turns the unit off.
- A new cutoff glides there by an octave every 512 frames (about 10 ms), so a filter that closes
  as the player dives underwater sweeps instead of clicking. A filter turned off glides open and then
  costs nothing.
- `audio.reverb 0` (saved) turns every reverb off, for players who find it tiring and for slow devices.
- Code has the same controls on `AudioDevice`: `set_bus_filter`, `set_bus_send`, `set_reverb`,
  `set_limiter`, and for the next two `set_bus_duck` and `set_bus_trim`. The per-bus EQ bands (`set_bus_eq`) and the bus graph (`set_bus_parent`) are
  code-only.

**Ducking.** `AudioDuck` turns one bus down while another plays: the music drops under a voice line
and comes back after it.

```json
{ "name": "Duck music", "components": {
    "AudioDuck": { "bus": "Music", "key": "Voice", "depth_db": -12, "attack_ms": 20, "release_ms": 400 } } }
```

The ducked bus is `Music`, `SFX` or `UI`; the key is any other bus but `Master`. Put voice lines on the
`Voice` bus (`"bus": "Voice"` on their `AudioSource`). The key is heard before its own volume, so a player
who turns dialogue down in the options still gets the music out of the way. `threshold_db` (default -40)
is how loud the key must be.

**Snapshots.** An `AudioSnapshot` changes one bus while it is on: a gain offset (`gain_db`) and a
low-pass or high-pass (`lowpass_hz`, `highpass_hz`; 0 leaves the bus's own). It fades in and out over
`fade_s`. Set `active` from gameplay, a sequence or a timeline key, or tick `while_paused` to make it the
pause snapshot: the stock player turns it on while its pause overlay is up, so the world sounds
muffled behind the menu.

```json
{ "name": "Pause mix", "components": {
    "AudioSnapshot": { "bus": "SFX", "while_paused": true, "fade_s": 0.3, "gain_db": -6, "lowpass_hz": 600 } } }
```

Snapshots on the same bus add their gains, and the tightest filter wins. A snapshot's fade is sound
state, not game state: it never changes the save, the replay or the state hash.

## 10.6 How it behaves under pressure

- 48 real voices; beyond that, the quietest and lowest-priority voices go virtual and come back
  when a voice frees. Voices below the protected priority (32) are stolen only by their peers,
  which keeps music safe.
- Long clips can stream in 100 ms pages (two streams at a time); loops are gapless by
  construction.
- A device that cannot open falls back to silence with one error rather than failing the game.
- The mixer never allocates or locks on the audio thread; game code never waits on it.
- Audio commands are produced by the deterministic sim, so a replay produces the same sound
  commands; the harness hashes them.

## 10.7 Content notes

- Sources are `.wav` (PCM) or `.ogg` (Ogg Vorbis). The cook resamples to 48 kHz and peak-normalizes
  to −1 dBFS unless `"normalize": false`.
- `"compress": "vorbis"` in the sidecar's settings stores the clip as Ogg Vorbis, about a tenth of the
  size; `"pcm"` stores float samples. A `.wav` defaults to `pcm` and an `.ogg` to `vorbis`. `"quality"`
  (−0.1..1, default 0.4) trades size for fidelity:

  ```json
  { "guid": "...", "importer": "wav", "importer_version": 2, "settings": { "compress": "vorbis", "quality": 0.4 } }
  ```

  A compressed clip decodes once when it loads. On the `Music` bus it stays compressed and its stream
  pages decode as they fill, so a long track costs its Ogg size in memory. Compress music and long
  ambiences; short, frequent one-shots can stay `pcm` to load faster.
- Mono clips pan; stereo clips keep their image. Music should be stereo at 48 kHz to skip
  resampling at cook.
- Device hot-swap is not built.

## 10.8 Subtitles and sound captions

The stock player draws subtitles at the bottom of the screen, one box per line, newest at the
bottom, under the menus. Lines come from three places:

- a timeline's `subtitle` track (13.5): the line shows while the playhead is in its range;
- `SoundCaption` on an entity with an `SfxTrigger` or an `AudioSource`: each play of the trigger
  shows its text (`"[door slams]"`) for `seconds`, and a looping source keeps it up while it plays;
- a `kit::ShowCaption` event from game code, for anything else (a voiced line, a one-shot `PlaySfx`):

```cpp
kit::ShowCaption c;
c.speaker.assign("Ada");
c.text.assign("Run!");
c.seconds = 2.0f;  // 0: three seconds
f.events().push(c);
```

The player chooses what shows (all `Saved`, on the stock options screen's Audio tab):

| Convar | Default | |
|---|---|---|
| `access.subtitles` | 1 | speech lines |
| `access.captions` | 0 | sound captions, in brackets |
| `access.subtitle_speaker` | 1 | the speaker's name before the line, in its own color |
| `access.subtitle_size` | 1 | text size (0.5..3), on top of `ui_scale` and `access.text_scale` |
| `access.subtitle_background` | 0.6 | the box's opacity; 0 draws outlined text with no box |

The default NPC dialogue box (21) follows `access.subtitle_speaker` and scales its box opacity by
`access.subtitle_background`. Captions are presentation only: the lines live in a resource, never
in a component, so a run hashes the same with them on or off. A host with its own UI reads the
lines with `kit::caption_lines` and draws them with `ui::SubtitleView`.
