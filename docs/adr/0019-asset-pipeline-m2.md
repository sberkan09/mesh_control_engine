# ADR-0019 — M2 asset pipeline: stb dependency, hand-written cook JSON, container layout fixes

**Status:** Accepted (2026-10-07) · refines `docs/architecture/assets.md` §3, §5, §9.2 for the first
`mce_asset` / `mce_cook` implementation.

**Context.** assets.md specifies the `.mce` container, the cook pipeline and its third-party deps
(nlohmann/json, stb, meshoptimizer, glslangValidator). Implementing it surfaced three places where the
sketch cannot be taken literally, plus the need to admit the first cook-time dependency.

**Decisions.**

1. **stb (stb_image, stb_truetype, stb_rect_pack) via FetchContent, pinned by commit** (the repository has
   no release tags). It is compiled into one third-party TU (`mce_stb`) linked privately by
   `mce_asset_cook` only. A new CI "stb wall" keeps `stbi_*/stbtt_*/stbrp_*` and `stb_*.h` includes
   inside `src/engine/asset/cook/`; a "cook wall" keeps runtime code from including `mce/cook/`.
   `mce_asset` (what shipped runtimes link) has no third-party code at all (FR-AST-010).
2. **Hand-written JSON reader/writer in the cook library instead of nlohmann/json.** Every node carries
   its source line/column, which is what lets the cook report `file:line:col` for a missing reference or a
   bad scene field (FR-AST-040) — nlohmann only knows positions for syntax errors. It also keeps
   exceptions and STL strings out of an engine target (the editor links the cook library). It accepts
   `//` and `/* */` comments (the sidecars in assets.md are jsonc) and rejects duplicate keys. The
   canonical writer (sorted keys, shortest round-trip numbers via `std::to_chars`) is what the scene
   source hash is taken over (assets.md §6).
3. **Container layout.** The assets.md §3 field order cannot satisfy its own `static_assert`s under
   natural alignment (the header comes to 80 bytes, the TOC entry to 48). The header stays 64 bytes by
   moving `flags` into the alignment hole and `header_crc` to the last four bytes (it still covers
   `[0, 60)`); the TOC entry is 48 bytes with explicit zeroed reserved fields. `payload_crc` covers
   `[64, total_size)` — TOC and payloads — so a corrupt TOC entry that passes the bounds checks is still
   caught. Chunk schema versions are `major << 8 | minor`: readers reject a different major
   (`ChunkVersionTooNew`) and accept newer minors.

**Consequences / scope of the first slice.** Importers: PNG/JPG → RGBA8 + box mips (sRGB-aware),
WAV → 48 kHz float32 (32-tap windowed sinc, −1 dBFS peak normalize, validated loop points),
TTF → baked glyph atlas + metrics + kern-table pairs, `.mcmat` materials, `.mcscene` → reflection-driven
columnar scene chunk. Deferred, each to the milestone that consumes it: glTF static mesh + meshoptimizer
(M4), shader validation via glslangValidator (with the RHI), sprite atlases and input maps (M3), CHNK_AUDP
streaming pages and IMA-ADPCM (mixer, M3/v0.5). Runtime loads read whole files until `mce_platform`'s
mmap lands; only `mce/asset/file_io.h` changes when it does. Jobs run inline until the job system's
runtime flag (v0.5); the cook already schedules stages level-by-level through `mce::jobs`.
