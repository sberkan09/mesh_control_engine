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
| `clip` | the `.wav` asset |
| `gain_db`, `pan` | −60..12 dB, −1..1 |
| `priority` | 0..255; louder and higher priority voices survive when voices run out |
| `bus` | `Bus_Master`, `Bus_Music`, `Bus_SFX` (default), `Bus_UI`, or a user bus |
| `loop` | gapless; the clip's loop points come from the WAV's own markers or the sidecar's `loop_start` / `loop_end` |
| `play_on_start` | start when the entity appears |

Changing `gain_db` or `pan` from code ramps over 10 ms (no clicks). Removing the component stops
the voice with a short fade. Music is an ordinary looping `AudioSource` on `Bus_Music`.

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

## 10.3 3D sound

- `AudioListener` on the camera entity (the first in ECS order is the ear; its transform gives
  position and orientation).
- `AudioSpatial {"attenuation_id": 1}` next to an `AudioSource` makes it 3D: the bridge follows the
  entity's position every tick. Curve 1 is an inverse-distance curve by default; a custom host can
  define up to 32 curves (`AudioDevice::set_attenuation`) with inverse, linear or exponential
  falloff and min/max distances.
- 2D games can ignore all of this; a source without `AudioSpatial` is plain stereo.

## 10.4 Volume and buses

Four saved convars drive the named buses: `audio.master_db`, `audio.music_db`, `audio.sfx_db`,
`audio.ui_db`. Set them from the console (`audio.music_db -12`), the command line, or bind them to
sliders in a runtime options menu (chapter 12). `audio.enabled 0` runs silent. The player mutes the
master bus while the window is unfocused (`-pause_on_focus_loss 0` turns that off).

## 10.5 How it behaves under pressure

- 48 real voices; beyond that, the quietest and lowest-priority voices go virtual and come back
  when a voice frees. Voices below the protected priority (32) are stolen only by their peers,
  which keeps music safe.
- Long clips can stream in 100 ms pages (two streams at a time); loops are gapless by
  construction.
- A device that cannot open falls back to silence with one error rather than failing the game.
- The mixer never allocates or locks on the audio thread; game code never waits on it.
- Audio commands are produced by the deterministic sim, so a replay produces the same sound
  commands; the harness hashes them.

## 10.6 Content notes

- WAV only (PCM); the cook resamples to 48 kHz and peak-normalizes to −1 dBFS unless
  `"normalize": false`.
- Mono clips pan; stereo clips keep their image. Music should be stereo at 48 kHz to skip
  resampling at cook.
- Reverb sends, per-voice pitch and device hot-swap are not built.
