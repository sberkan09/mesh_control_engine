# ADR-0044 — Compressed audio clips: Ogg Vorbis, libvorbis in the cook, stb_vorbis at run time

**Status:** Accepted (2026-10-16). **Milestone:** M-AUDIO2 step 1. **Plan:** `docs/milestones/M-AUDIO2.md`.
**Reference:** `src/engine/asset/include/mce/asset/vorbis.h`, `src/engine/asset/cook/src/wav_importer.cpp`,
`src/engine/audio/src/stream.cpp`.

**Context.** Every clip cooked to 48 kHz float PCM: 384 kB per stereo second. A three-minute music track is
69 MB in the pack, which a phone or browser download pays in full. The user guide said "WAV only". Games
ship compressed audio, and their artists hand over `.ogg` files.

**Options weighed.**

| Option | For | Against |
|---|---|---|
| **Ogg Vorbis**: libvorbis encodes in the cook, stb_vorbis decodes at run time | Royalty-free; the format artists already export; about a tenth of float PCM at quality 0.4 with no audible loss for game audio; stb_vorbis is one public-domain C file, decodes from memory into a caller's buffer (no malloc), seeks sample-exact | Two codebases (Xiph's encoder is large); Vorbis is older than Opus |
| Opus (libopus + opusfile) | Better quality per bit, the web's codec | Larger decoder with SIMD paths per architecture, fixed 48 kHz with pre-skip handling, no single-file decoder |
| IMA ADPCM | Trivial decoder | Only 4:1 against 16-bit (8:1 against float), audible noise on music |
| MP3 / AAC | Common | Patent history (AAC), and decoders are no smaller |

## Decisions

1. **Cook: libvorbis 1.3.7 and libogg 1.3.5, cook-only.** Built from source into `mce_vorbisenc` (host
   builds only) and linked by `mce_asset_cook` alone (the libvorbis link wall). `cook::encode_vorbis` is the
   one caller (the libvorbis wall keeps `vorbis/`, `ogg/` and their symbols in `vorbis_encode.cpp`). VBR,
   quality from the sidecar, a fixed Ogg serial and `-ffp-contract=off`, so one host cooks the same bytes
   every time (the cook's cache key stays meaningful).
2. **Run time: stb_vorbis (pinned with the rest of stb), in `mce_asset`.** `asset::VorbisReader` wraps it:
   memory streams only (`STB_VORBIS_NO_STDIO`, `NO_PUSHDATA_API`), and the decoder works in an arena the
   reader owns, grown at `open()` until the stream's setup fits, so nothing allocates after `open()`. The
   stb_vorbis wall keeps its symbols in `vorbis.cpp` and `stb_vorbis_impl.c`. The cook decodes `.ogg` sources
   with the same reader.
3. **Format: the Sfx chunk carries either.** `SfxFlags::Vorbis` marks a payload whose `data_offset` holds a
   byte count and the Ogg stream, with the header's frame count and loop points counted in decoded frames.
   Such chunks are written as SFX version 1.1 (a minor bump: readers check the major only).
4. **Import settings.** `.wav` keeps `"compress": "pcm"` as the default, so existing packs cook unchanged;
   `"compress": "vorbis"` re-encodes after the resample to 48 kHz and the normalization. `.ogg` (importer
   `"ogg"`) is decoded, resampled, normalized and re-encoded, `"vorbis"` by default. `quality` is -0.1..1
   (default 0.4). The importer version moved to 2, so every clip re-cooks once.
5. **Decode where the clip plays from.** The `ClipBank` decodes an in-memory clip whole at load and owns the
   samples; it frees them only after the mixer has dropped the clip (a retired list swept by `sync()`, and
   `release_all()` after `finish_releases()`). A streamed clip (`register_stream`, the Music bus) keeps the
   stream encoded and its page ring decodes on the filler (`update()`, main thread): consecutive frames
   decode in runs, and the reader seeks only when the mapped frame jumps (the loop seam, or pages the mixer
   skipped). The audio thread never decodes. `register_clip` refuses an encoded payload.
6. **Measured.** One stereo second of two-tone test audio at quality 0.4: 10.2 kB against 384 kB float (2.7%),
   37 dB SNR. A streamed loop with its seam mid-page decodes to exactly the whole-clip decode (the tests).
7. **Not now.** Opus, decode on a worker thread, and keeping in-memory clips compressed (decode per voice).
