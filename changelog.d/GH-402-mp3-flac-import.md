- MP3 and FLAC audio (M-IMPORTMEDIA step 2, GH-402): `.mp3` and `.flac` clips cook to a 48 kHz `CHNK_SFX` like WAV (MP3 stored as
  Vorbis, FLAC as PCM; `"compress"` overrides). Decoders are dr_mp3 and dr_flac (dr_libs, pinned, cook-only, ADR-0049) on a
  budgeted allocator with frame caps; mono and stereo only. User guide 5 lists the formats.
