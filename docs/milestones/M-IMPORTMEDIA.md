# M-IMPORTMEDIA — The texture and audio formats artists actually have (milestone record)

**Status:** active (added 2026-10-18; **owner: the routine 1 thread (M-IMPORT3D's), claimed 2026-10-18**; no Linear issues: the workspace is at its free limit, this record is the tracker).

**Why.** After M-IMPORT3D the 3D path takes glTF, OBJ and FBX, but 2D and audio import still stop at PNG/JPEG/HDR/EXR
and WAV/Ogg. `is_media` (the credits check) already lists `.mp3` and `.flac`, yet no importer cooks them. Artists and
asset stores ship TGA and BMP textures, and MP3 and FLAC music and effects; today each needs a manual conversion step,
and the cook says nothing useful about it.

**Boundaries.** Cook-time only: nothing changes in a runtime target, the cooked chunk formats or the render path. No
GPU texture formats (KTX2, DDS, Basis) here: they are pre-compressed and need a RHI decision, so they stay a
follow-up. Stay off render, script VM and perf files.

| # | Step | Model | State |
|---|---|---|---|
| 1 | `.tga` and `.bmp` textures through the existing stb_image decoder (no new dependency), a dimension pre-check before any pixel allocation, tests, guide | Sonnet | done (this PR, GH-401): `STBI_ONLY_TGA` / `STBI_ONLY_BMP` in `stb_impl.c`; the texture importer reads the header first and refuses over 16384 a side; `raw_pixels_truncated` refuses raw TGA and BMP files cut short (stb leaves the rest of its buffer unwritten); `tests/asset/test_cook_texture_formats.cpp` |
| 2 | `.mp3` and `.flac` audio decoded at cook time into the existing `CHNK_SFX` / music chunk (a public-domain single-header decoder, pinned, cook-only, walled, ADR; sidecar settings as for WAV), tests, guide | Sonnet (Opus: the new dependency and its untrusted-input limits) | planned |
| 3 | Fuzz seeds for both in `tests/fuzz/test_importers.cpp`, M-FUZZ survey rows, the close | Sonnet | planned |

## Decisions

- TGA and BMP only from stb_image: it is already compiled for PNG/JPEG/HDR, and enabling two more decoders adds no
  dependency. PSD, GIF, PIC and PNM stay disabled (a layered or animated file has no single right answer).
- Every 8-bit decode goes through `stbi_info_from_memory` first and refuses dimensions over 16384 per side before
  stb allocates: a TGA or BMP header can claim 65535 x 65535 or more, and the old PNG path decoded before checking.

## Open items

- stb_image decodes a raw TGA cut short without an error and leaves part of the buffer unwritten (not zeroed): the importer
  checks the promised size itself. RLE and compressed BMP read to zeros at the end of the file, deterministically, and are accepted.
- Real artist files (Photoshop TGA with extension area, RLE TGA, 16-bit BMP, BMP v4/v5 headers) have not been tried.
