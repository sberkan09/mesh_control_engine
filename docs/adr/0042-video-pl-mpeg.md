# ADR-0042 — Video playback over pl_mpeg (MPEG-1)

**Status:** Accepted (2026-10-15). **Milestone:** M-CINEMA step 5 (off-tier, opt-in). **Plan:**
`docs/milestones/M-CINEMA.md`. **Reference:** `src/engine/render/include/mce/render/video.h`,
`src/engine/kit/include/mce/kit/video.h`.

**Context.** MCE had no video playback. Games want a splash or intro film, a film on a screen in the world, and a
film as one beat of a cutscene timeline. The player runs on Linux, Windows, macOS, Android, iOS and the web, so
the decoder must build everywhere from source with no system codec, stay small, and stay out of the way of the
fixed-tick simulation (replays and state hashes must not depend on decode timing).

**Options weighed.**

| Option | For | Against |
|---|---|---|
| **pl_mpeg** (MIT, one C header, pinned by commit) | MPEG-1 video, MP2 audio and the program-stream demuxer in about 4 k lines with no dependencies; decodes from memory; exact seek; ffmpeg encodes to it in one command; builds unchanged on every platform MCE ships, the web included | MPEG-1 is an old codec: files are larger than modern codecs at the same quality, and above about 854x480 a phone may spend most of a frame decoding |
| WebM over dav1d (AV1) or libvpx (VP9) | Modern compression, the web's own formats | Two or three large libraries (a demuxer, a video codec, Opus), SIMD assembly per architecture, a much bigger build and port surface for a feature a game uses for a few minutes |
| Platform decoders (Media Foundation, AVFoundation, MediaCodec, the browser's `<video>`) | Hardware decode, any modern codec | Five back ends with different threading and texture models, none on Linux without GStreamer or ffmpeg, and results that differ per platform |

## Decisions

1. **pl_mpeg, pinned `c871f2b`, compiled into `mce_render`.** Its implementation is one C file
   (`render/src/plmpeg_impl.c`, warnings off, `PLM_NO_STDIO`: memory sources only); `render::VideoDecoder`
   (`render/src/video.cpp`) wraps it. The pl_mpeg wall in `scripts/ci/check_walls.py` keeps its header and symbols
   in those two files. `mce_render` is where both callers already link: the player's texture upload and the
   cook (through `mce_ui`).
2. **The cook keeps the stream and decodes the sound.** An `.mpg` (or `.mpeg`) with an `importer: "video"`
   sidecar cooks to `CHNK_VID`: a 64-byte `VideoHeader`, the MPEG bytes as imported, and the MP2 sound decoded once
   and resampled to 48 kHz f32 stereo. The mixer plays that PCM like any clip, so the run time decodes pictures
   only, and the mixer needs no resampling or streaming codec. The cook counts the frames for the exact length
   (pl_mpeg's own length stops at the last packet's time stamp) and warns above 854x480. Sidecar settings:
   `audio` (default true), `normalize` (default false).
3. **The playhead is sim state; pictures are not.** `kit::VideoPlayer` holds `time`, `playing`, `loop`,
   `finished` and `restart`; `"kit_video"` (FixedUpdate_Post, before the timeline player, through cutscene holds)
   advances it by the fixed dt. Replays, saves and state hashes see the playhead, never decoded pixels. A timeline
   video track (`TimelineTrackKind::Video`, appended) is a state track: while a key's range holds the playhead it
   sets the target's `video`, `time` and `playing`, so scrubbing and skipping show the right frame.
4. **The sound follows the playhead.** One voice per playing player, started with `play_at` that many samples into
   the clip. A start, a wrap, another video, or a jump of more than 0.1 s restarts it at the playhead; pausing,
   muting, a speed other than 1 or a destroyed player stops it. The voice is on the Music bus.
5. **Render side: one decoder and one texture per shown player.** `kit::VideoTextures` steps each decoder to its
   player's `time` once per rendered frame. A forward step decodes only the frames in between; a step back or a
   jump of more than a second seeks to the intra frame before the target and decodes forward. A picture that
   changed is uploaded into the player's RGBA8 texture with `update_texture` in place (a texture a frame in flight
   still samples may show the new picture early: a benign race). A `Sprite` on the same entity draws the film in
   place of its texture; `fullscreen` makes the stock player draw it over the scene, fitted with black bars, under
   the letterbox, subtitles and UI.
6. **Budget.** Decode plus the RGBA conversion measured 1.1 ms a frame at 640x360 and 4.0 ms at 1280x720 on one
   desktop core (30 fps clips, so a 60 Hz frame decodes a picture every other frame). A Low-tier phone core is 3 to
   5 times slower, which keeps 640x360 to 854x480 inside the 16.7 ms frame; the cook warns above that.
7. **Not now.** A UI image widget showing a film (the fullscreen view covers splashes; a widget needs a per-image
   texture in the UI skin), decode on a worker thread, YUV textures converted on the GPU, sound at speeds other
   than 1, and other codecs. Each can come later behind the same `VideoPlayer`.
