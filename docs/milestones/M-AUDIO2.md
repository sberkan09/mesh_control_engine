# M-AUDIO2 — Audio a shipped game needs (milestone record)

**Status:** active (added 2026-10-16; steps 1–2 done). Plan: ROADMAP §M-AUDIO2; Linear SBA-118 … SBA-122.

**Why:** MCE's mixer has buses, 3D attenuation, voice virtualisation and streaming. A shipped game needs more,
and the user guide says what is missing (10.6): "WAV only", and "reverb sends, per-voice pitch and device
hot-swap are not built".

- Every clip ships as 48 kHz float. A phone or browser pack pays roughly ten times what Ogg Vorbis would.
- One-shots cannot vary in pitch or pick from a set, so footsteps and impacts repeat.
- Buses have volume and nothing else: no low-pass, no reverb, no limiter.
- Nothing ducks the music under dialogue, and a pause menu cannot muffle the world.
- 3D sound goes through walls.

Unity and Godot ship all of this, and Unreal adds MetaSounds; FMOD and Wwise are the usual add-ons. This
milestone brings the stock mixer up to what most games use.

It needs no device. The mixer can render offline into a buffer, so every step gates on rendered samples in
`local_ci.sh`, and the sim hash never moves.

**Not in scope:**
- FMOD or Wwise integration.
- An audio graph editor.
- HRTF.
- Device hot-swap (a platform task).
- MP3 or AAC (patent-encumbered decoders).

## Steps

| # | Step | Linear | State |
|---|---|---|---|
| 1 | Compressed clips: `.ogg` (Vorbis, stb_vorbis) in the cook, and `"compress": "vorbis"` on WAV imports. Decoding happens at load for short clips and in the stream pages for long ones | SBA-118 | done |
| 2 | Variation: per-voice pitch (resampling playback rate), and sound sets (`.mcsound`: clips picked at random or in sequence, with pitch and volume ranges), seeded from the sim so replays repeat | SBA-119 | done |
| 3 | Bus effects: biquad low/high-pass, a reverb send (Freeverb-style), a master limiter. Each bus's chain is project data, with parameters as convars and components | SBA-120 | not started |
| 4 | Ducking and snapshots: a bus ducks under another bus's level (sidechain), and named mixer snapshots (e.g. "paused" = muffled world) blend in over a fade from game events and the pause | SBA-121 | not started |
| 5 | Occlusion for 3D sources (a physics ray: low-pass and gain), Doppler, the user guide, MCP reach and the demo | SBA-122 | not started |

**Demo:** a sample walks a listener through rooms. Footsteps come from a sound set with pitch variation, a
cave bus adds reverb, the music ducks under a voice line, the pause snapshot muffles the world, and a wall
occludes a source. The offline render of a scripted run is the record, and the Ogg pack is smaller than the
WAV one.

**Gates:**
- Offline renders of fixed scripts hash the same on every run and machine (float mixing in a fixed order).
- The sim hash is unchanged by any audio feature.
- No allocation or lock on the audio thread (the existing rule, checked by the mixer tests).
- A Vorbis clip decodes within a set error of its source.
- `local_ci.sh` green on every step.

## Step 1 — Compressed clips (done, SBA-118, ADR-0044)

- **Cook.** `cook::encode_vorbis` (libvorbis 1.3.7, libogg 1.3.5, cook-only, `mce_vorbisenc`) encodes VBR at a
  sidecar `quality` (default 0.4). The `wav` importer takes `"compress": "pcm" | "vorbis"`; the new `ogg`
  importer decodes `.ogg` sources, resamples and normalizes them like WAV, and re-encodes (`vorbis` by default).
  Importer version 2. Vorbis Sfx chunks are SFX 1.1: `SfxFlags::Vorbis`, a byte count and the Ogg stream.
- **Runtime.** `asset::VorbisReader` (stb_vorbis in `mce_asset`) decodes from memory in an arena it owns, with
  no allocation after `open()`. `ClipPayload` gains `encoded` / `encoded_bytes`. The `ClipBank` decodes an
  in-memory clip at load and frees the samples once the mixer has dropped the clip. A streamed clip stays
  encoded and its page ring decodes on the filler, seeking at the loop seam. `register_clip` refuses an
  encoded payload.
- **Walls.** The stb_vorbis wall keeps the decoder in `vorbis.cpp` and `stb_vorbis_impl.c`; the libvorbis wall
  and link wall keep the encoder in the cook's `vorbis_encode.cpp`.
- **Measured** (`tests/asset/test_cook_vorbis.cpp`): one stereo second of two tones is 10.2 kB of Vorbis
  against 384 kB of float (2.7%), 37 dB SNR. The same bytes on a second encode. A seek lands on the whole
  decode's sample. A looping streamed clip with its seam mid-page renders exactly the whole-clip decode for
  4 s (worst error 0), as does the in-memory clip, with no underrun.
- **Docs.** User guide 5 (importer settings) and 10.6, ARCHITECTURE's dependency table, ADR-0044.

## Step 2 — Variation: pitch and sound sets (done, SBA-119)

- **Pitch.** `PlayParams::pitch` (0.25..4) and `AudioDevice::set_handle_pitch`. A voice keeps a 32.32 fixed-point
  position and step; between frames it interpolates linearly. At 1 the voice reads the samples as before, so every
  existing render and command hash is unchanged (the command carries the rate in the clip field's spare half, and
  the hash folds it only when it is not 1). Virtual voices and late `play_at` starts advance by the rate. Streams
  play at 1.
- **Sound sets.** `.mcsound` (importer `sound`) cooks to an Sfx chunk with `SfxFlags::Set` (SFX 1.2): a
  `SoundSetHeader` (pick random or sequence, no-repeat, pitch and gain ranges) and the member GUIDs, each a cook
  dependency that must be an Sfx and not another set. Because it is an Sfx, every sound field (AudioSource,
  SfxTrigger, PlaySfx, sequence and timeline keys) takes a set with no schema change.
- **Picking.** `ClipBank::pick(guid, seed)`: random from a splitmix of the seed (no immediate repeat), or the next
  in sequence; pitch and gain from the same seed. The bridge seeds from the tick and the source (the entity, or the
  event's place in the tick), so replays pick the same and the sim hash never sees it. An `AudioSource` keeps its
  pick's gain in a former pad byte (`pick_gain`, 0.5 dB steps; 0 for a plain clip, so existing hashes hold).
- **Reach.** MCP `asset_schema` / `asset_create` make a `SoundSet`; `engine_capabilities` lists it.
- **Tests.** `tests/audio/test_pitch_sets.cpp`: rate 2 and 0.5 against the clip's samples, pitch 1 bit-exact and
  hash-equal with the unpitched path, a pitched loop rendered twice the same, live pitch changes, clamping,
  streams at 1; random picks cover a set with no repeat inside the ranges and repeat across banks, sequences
  cycle, a nested set plays nothing; two bridge runs of footsteps over 120 ticks send the same commands and vary.
  `tests/asset/test_cook_sound.cpp`: the cooked chunk, and six bad sets plus a nested one failing the cook.
- **Docs.** User guide 10.3 (new), 5 (asset table).
