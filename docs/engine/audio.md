# audio — real-time float mixer, voices, buses, streams and the ECS audio bridge

`mce_audio` is an L2 library in `src/engine/audio/`. Its public dependencies are `mce_core`,
`mce_platform`, `mce_ecs`, `mce_refl` and `mce_asset` (`src/engine/audio/CMakeLists.txt`). It mixes
at 48 kHz, stereo float32, on the device callback thread. The device itself comes from
`mce_platform`'s `AudioOutput`, so this module contains no SDL symbol. It is linked by
`mce_gameplay_kit`, `mce_editor_core` (component registration and play-in-editor), `tools/cook`
(it registers `AudioSource` for scene cooking), the `mce_engine` umbrella, `mce_player_runtime`
(directly where game modules are unsupported) and the samples `03_platformer` and
`05_ashen_court`.

The main thread never touches mixer state. Every API call becomes a POD command (at most 32 bytes)
in an SPSC FIFO that the callback drains; results come back through a reply FIFO and stats atomics.

## Public surface

Headers live under `include/mce/audio/`.

`audio.h`: the main-thread API.

- Constants: `kSampleRate` (48000), `kChannels` (2), `kMaxRealVoices` (48), `kMaxVirtualVoices`
  (208), `kMaxClips` (1024), `kMaxStreams` (2), stream ring of `kStreamPages` (4) pages of
  `kStreamPageFrames` (4800, 100 ms), `kProtectedPriority` (32), `kBusCount` (12), `kEqBands` (4),
  `kMaxAttenuationCurves` (32), `kLimiterLookahead` (240 frames, 5 ms).
- Handles: `ClipHandle`, `AudioHandle` (generational; stale handles are silent no-ops). Buses:
  `Bus_Master`, `Bus_Music`, `Bus_SFX`, `Bus_UI`, user buses from `Bus_User0`.
- `ClipPayload` (48 kHz float32 frames owned by the caller, mono or stereo, `loop_start`) and
  `PlayParams` (gain, pan, loop, priority, bus, and `attenuation_id` + `position` for 3D).
- DSP: `BiquadDesc`, `BiquadCoeffs`, `biquad_coeffs`, `biquad_magnitude` (RBJ cookbook bands);
  `LimiterDesc`.
- 3D: `AttenuationCurve` (Inverse, Linear, Exponential, clamped to min/max distance),
  `attenuation_gain`, `Listener`, `Spatial`, `spatialize`.
- `Backend`: `Device` (platform audio thread), `Offline` (caller pulls blocks with
  `render_offline`), `Silent` (no device; `update()` mixes and discards one block).
- `AudioDesc` (backend, driver override, period, FIFO capacity, `bind_convars`,
  `silent_on_failure`) and `AudioStats` (voice counts, steals, drops, underruns, dropped commands,
  master peak, limiter gain, `mix_sample`).
- `AudioDevice`:
  - Clips: `register_clip` (in memory), `register_stream` (paged), `release_clip`,
    `is_clip_registered`.
  - Playback: `play`, `play_at(clip, mix_sample)` (sample-accurate), `stop(voice, fade_s)`,
    `set_handle_gain`, `set_handle_pan`, `set_handle_position`, `is_playing`.
  - Mixer: `set_bus_gain`, `set_bus_parent`, `set_bus_eq`, `set_limiter`.
  - 3D: `set_attenuation(id, curve)`, `set_listener`.
  - `mix_clock()`, `update()` (once per frame), `render_offline`, `stats()`, `command_hash()`,
    `live_handles()`.
- `db_to_linear`, `linear_to_db` (−96 dB and below is silence).

`clip_bank.h`: the assets-to-audio seam. `clip_payload_from_sfx` turns a cooked `CHNK_SFX` payload
into a `ClipPayload` without copying. `ClipBank` maps an Sfx GUID to a registered clip and holds
one asset retain per clip (`acquire`, `find`, `release`, `release_all`, `sync` for hot reload).

`components.h`: the ECS surface. `AudioSource` (reflected component: clip asset ref, gain, pan,
priority, bus, loop, `play_on_start`), `AudioSpatial` (makes a source 3D with an attenuation id),
`AudioListener` (the ear, usually on the camera), `PlaySfx` (one-shot event, optional 3D
position), `AudioRef` (world resource), `install(world, device, clips, after)` and the
`audio_bridge` system.

Internals in `src/`: `mixer.cpp` / `mixer.h` (the real-time mixer), `commands.h` (command and
reply types), `dsp.h` (biquad and limiter), `stream.cpp` (page ring), `spatial.cpp`,
`bridge.cpp`, `clip_bank.cpp`, `audio_device.cpp`.

## Rules and invariants

- **Main thread only** for every `AudioDevice`, `ClipBank` and bridge call. Nothing on the main
  thread waits on the audio thread.
- **Real-time callback.** The mixer runs with `mem::NoHeapScope` active: any heap traffic in it
  asserts and counts a violation (FR-AUD-010). No locks, no ECS. Callbacks of any size are mixed in
  512-frame sub-blocks.
- **FIFO full.** A command that does not fit is dropped and counted; `play` returns a null handle.
  Nothing corrupts and every handle comes back.
- **Click-free changes.** Gain and pan ramp over 10 ms; `stop` always fades at least 2 ms; a stolen
  voice fades over 5 ms.
- **Voice stealing.** Order is priority, then quieter, then older. Voices below
  `kProtectedPriority` are stolen only by peers. Less important sounds go virtual and are
  promoted when a real voice frees.
- **Bus graph.** Every bus but Master feeds one parent with a lower id, so the graph is acyclic
  by construction and mixes in one pass. EQ runs after a bus's children are summed, before its
  gain. The limiter is off by default because it adds 239 frames of latency; enable it once at
  init.
- **Scheduling.** `play_at` starts a voice on an exact mixer frame. A time already past starts an
  in-memory clip that far in; streams start at once.
- **Streams.** The filler (`update()` today) publishes whole pages ahead of the mixer. A late page
  mixes as silence, counts an underrun, and the voice keeps its timeline. Loops are gapless and
  bit-exact by construction.
- **Clip memory.** A `ClipPayload`'s frames must stay valid until `release_clip` completes
  (`is_clip_registered` turns false after `update()`). `ClipBank` holds the asset retain for this;
  call `ClipBank::sync()` once per frame before the sim ticks.
- **Determinism.** `audio_bridge` runs in `FixedUpdate_Post`, so audio commands are a function of
  sim state. The harness hashes commands (`command_hash()`), not output. 3D is a separate
  component so 2D `AudioSource` keeps its M3 byte layout and existing world hashes.
- **Failure.** With `silent_on_failure` (the default) a device that cannot open falls back to the
  Silent backend with one error. The `audio.enabled` convar set to false runs silent.
- **Convars.** With `bind_convars`, the saved convars `audio.master_db`, `audio.music_db`,
  `audio.sfx_db` and `audio.ui_db` drive the four named bus gains.
- **Walls.** No SDL here (SDL wall and SDL include wall); the engine container ban applies.

## How it is tested

`test_audio` (`tests/audio/`, no GPU, links `mce_audio`; helpers in `audio_test_util.h`). Most
cases use the Offline backend.

- `test_mixer.cpp`: one-shots, pan law, gain ramps, fade on stop, "voice stealing: priority, then
  quieter, then older; stolen handles fail cleanly", virtual voices, protected music, bus gains,
  FIFO overflow, released clips, clip validation, "FR-AUD-010: the callback body never touches the
  heap", "SPSC under a real second thread: commands and replies stay consistent", command hash.
- `test_mixer_v05.cpp`: 48 voices and the 49th steal, `play_at` on the requested frame for any
  callback size, late `play_at`, "FR-AUD-030: the bus graph matches an offline reference mix within
  -60 dB", biquad coefficients and EQ sweeps, the look-ahead limiter, attenuation and pan, 3D voice
  attenuation, listener and 3D sources following transforms.
- `test_stream.cpp`: "a looping stream with an intro plays gapless and bit-exact for 60 s",
  non-looping end, underrun, stream slot limits.
- `test_device.cpp`: "Device backend pulls from the mixer on the platform audio thread" (SDL
  `dummy` audio driver, so no sound card needed), Silent backend, bridge behaviour.
- `test_clip_bank.cpp`: `CHNK_SFX` decoding, `ClipBank` retains and quiet failures, the bridge
  playing by GUID and following a hot reload.

## Design records

- [audio-input-ui.md](../architecture/audio-input-ui.md): §2.3 main-thread API, §2.4 ECS surface,
  §3.3 mixer graph and RT contract, §3.4 sample-accurate scheduling, §3.5 voice stealing, §3.6
  streaming, §3.7 DSP chain, §3.8 spatialization, §4.1 threads, §4.3 audio lifecycle, §6.2 tests.
- [ADR-0026](../adr/0026-m6-subsystems.md) §1: audio v0.5 as built.
- Milestones: [M6-subsystems.md](../milestones/M6-subsystems.md) (audio section),
  [M6-gameplay.md](../milestones/M6-gameplay.md) (master bus muted on focus loss by the player).

## Known gaps

- ADR-0026 §1 "Not built": the Freeverb send (FR-AUD-060), per-voice pitch, and device re-open on
  a default device change.
- Stream pages are filled from `update()` on the main thread; the headers name the IO worker as
  the later filler ("update(), later the IO worker").
- M7: Ashen Court's audio is synthesized and has no music.
