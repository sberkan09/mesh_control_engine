# ADR-0049 — MP3 and FLAC import over dr_libs (cook-only)

**Status:** Proposed (2026-10-18), awaiting the coordinator's acceptance (a new third-party dependency). **Milestone:**
M-IMPORTMEDIA step 2. **Plan:** [`docs/milestones/M-IMPORTMEDIA.md`](../milestones/M-IMPORTMEDIA.md). **Reference:**
`cmake/Dependencies.cmake` (`mce_drlibs`), `src/engine/asset/cook/src/audio_decode.cpp`, `drlibs_impl.c`,
`tests/asset/test_cook_audio_formats.cpp`.

**Context.** Music and effects from stores and recording tools arrive as MP3 and FLAC; the cook reads only WAV and Ogg
Vorbis. The decoders run in the cook only (`mce_asset_cook`, `mce_cook`, the editor's cook service): the runtime
already plays the cooked `CHNK_SFX` (PCM or Vorbis) and gets no new code. Source files are untrusted input.

**Options.**

| Option | For | Against |
|---|---|---|
| **dr_mp3 + dr_flac** (dr_libs, David Reid; public domain or MIT-0) | one header per format, C99, no dependencies, decode from memory, allocation callbacks, `f32` output; dr_mp3 is minimp3-derived | upstream is a one-person project; dr_flac has had memory-safety fixes in the past, so the cook never trusts it (see below) |
| minimp3 + libFLAC | the reference FLAC decoder | two build systems; libFLAC is BSD-3 with an autotools/CMake build and metadata code we do not need |
| ffmpeg / libav | everything | LGPL/GPL options, huge, a process or library per platform; contradicts NFR-B-01 |
| Convert outside the engine | no dependency | every MP3/FLAC needs a manual step; the status quo |

## Decisions

1. **dr_libs @ `dfe8377631000664666519fdb83da193fd8037f4`** (master, 2026-10-18), by commit via FetchContent (no tags worth
   pinning). Licence: the repo's `LICENSE` offers the Unlicense or MIT-0, the user's choice; we take MIT-0 and list it in
   ARCHITECTURE §7.
2. **`mce_drlibs`**, a static library built like `mce_ufbx`: `drlibs_impl.c` defines `DR_MP3_IMPLEMENTATION` and
   `DR_FLAC_IMPLEMENTATION` with `DR_MP3_NO_STDIO`, `DR_FLAC_NO_STDIO` and `DR_FLAC_NO_OGG` (no file access, no Ogg-FLAC
   container parser); warnings off, `-ffp-contract=off` / `/fp:precise`.
3. **Walls.** The "dr_libs wall" keeps `dr_mp3.h` / `dr_flac.h` and every `drmp3_*(` / `drflac_*(` call in
   `audio_decode.cpp` and `drlibs_impl.c`; the "dr_libs link wall" keeps `mce_drlibs` in CMake files under
   `src/engine/asset/cook/`. Nothing under `include/` names dr_libs; the importer factories live in the cook-private
   `audio_decode.h`, not `importer.h`.
4. **Untrusted input.** Both decoders run on a budgeted allocator (256 MiB outstanding; the callbacks refuse past it), the
   decoder is driven frame by frame into the clip instead of trusting a frame count in the header (a FLAC header can claim
   2^36 frames), decoding stops and the cook fails at one hour or 128 Mi samples, only mono and stereo are accepted
   (1000..384000 Hz), and the stream must produce at least one frame. `tests/asset/test_cook_audio_formats.cpp`
   truncates and byte-flips both fixtures and runs the empty and junk cases; the M-IMPORTMEDIA step 3 fuzz seeds extend
   this under ASan.
5. **Defaults.** An `.mp3` is already lossy, so it is stored as Ogg Vorbis (`"compress": "vorbis"`); a `.flac` is stored as
   PCM like a `.wav` (a lossless source's size is the author's call). The sidecar `"compress"`, `"quality"`, `"normalize"`,
   `"loop"` keys work as for WAV; MP3 and FLAC carry no embedded loop points.
6. **Determinism.** Decoding is integer/float maths in dr_libs, compiled `-ffp-contract=off`; the clip then follows the
   WAV path (resample, normalise). As with libvorbis, the same file cooks to the same bytes on one host; across hosts the
   compiler's float handling of dr_mp3's synthesis filter is not pinned, so no cross-host byte claim is made for MP3.
