# Research: Asset Pipelines & Tools for MCE

**Domain:** assets-tools | **Date:** 2026-10-06 | **Status:** research input to `docs/architecture/assets.md`
**Scope:** glTF 2.0 as interchange (meshes/materials/animation), texture compression (KTX2/BasisU vs BCn-at-cook), import-compile-cache pipeline design, GUID-based asset databases, dependency tracking, hot reload, scene serialization formats, versioning/migration.
**Committed stack context:** C++20/MSVC, GL 4.5 Core behind a thin RHI, SDL3, ImGui editor, archetype ECS, hand-written glTF importer, cook pipeline to versioned chunked `.mce` containers, runtime never parses JSON/PNG (binding asset policy, FR-AST-010/120).

Ground rule (binding): no web search — every claim is grounded in canonical sources from knowledge (Khronos glTF 2.0 spec, Basis Universal docs, engine source/blog corpora: Unity, Unreal, Godot 4, Bevy, Bitsquid/Our Machinery, id Tech via Sanglard's black books; Gregory, *Game Engine Architecture*; Gaffer On Games). Citations inline by name.

---

## 1. State of the art: what every pipeline converged on by 2026

Strip away the branding and Unity, Unreal, Godot, Bevy, O3DE, and id Tech all agree on six things:

1. **Two trees: human-authored sources, machine-optimized runtime forms.** Source formats are chosen for tool interop and diffability; runtime formats for load speed and memory. The bridge is a deterministic import/cook step. Unity's `Library/`, Godot's `.godot/imported/`, Unreal's Derived Data Cache and cooked packages, id Tech's compiled `.bsp`/`assets.pk3` — same shape every time (Gregory, *Game Engine Architecture*, ch. 20 "A Game of Resources"; Sanglard, *Game Engine Black Books*).
2. **A cache keyed by more than the source mtime.** Every mature pipeline keys cached output on (source content hash, importer settings, importer/tool version). Unity 2020.2's import overhaul, Unreal's DDC version keys, and Bevy's asset processor all learned the same lesson: mtime-only caches break across checkouts and tool upgrades.
3. **Stable identity independent of path.** Unity's `.meta` GUIDs, Unreal's package GUIDs, Godot's `uid://` resource IDs. Bevy is the counter-example — its path-keyed asset server is the single most-cited pain point in its asset story, and Godot 4.4's addition of `.uid` sidecar files was precisely a retrofit of the sidecar model. Identity must survive move/rename.
4. **A dependency graph, extracted at import, not inferred by scanning.** Importers *declare* what they consumed; the graph drives incremental cook and reverse-lookup (find-references). Scanning text for references (Bevy's approach for scene dependencies) is simpler but cannot see into binary sources and produces false edges.
5. **Hot reload as a first-class pipeline consumer.** The reload path must be the *same* import code as full cook, not a parallel editor-only path (Unity's parallel editor import paths were a chronic source of editor-vs-build content drift; Our Machinery's asset system posts are the clean statement of "one importer, watched").
6. **Scene serialization split in two.** A diffable text document the editor owns (Godot `.tscn`, Unity YAML, Bevy RON scenes) and a cooked columnar binary the runtime loads. Nobody ships the authoring format; nobody hand-edits the runtime format.

What did **not** converge: whether sources should be pushed through a universal GPU-ready texture container (KTX2/BasisU) at *authoring* time or compressed to native BCn at *cook* time (§3); and how heavy the asset "database" should be (Unity's central DB + cache server vs. sidecar files in VCS vs. Bevy's no-database). These are the real forks MCE must decide deliberately.

---

## 2. glTF 2.0 as interchange

### 2.1 Why glTF won the interchange war

glTF 2.0 (Khronos, 2017; ratified as ISO/IEC 12149:2022) is the only 3D interchange format with all four properties MCE needs: (a) an **open spec with conformance tests**, (b) **PBR-metallic-roughness as the core material model** — not a DCC-native soup to be reinterpreted, (c) **JSON structure + binary buffers** — parseable with a cook-time JSON lib, no proprietary SDK, and (d) **first-class exporter support from Blender, 3ds Max, Maya, and every web pipeline**. It is to 3D what PNG/WAV are to 2D/audio: the lingua franca everything exports.

Compare the alternatives, considered and rejected:

- **FBX** — the historical default, but a closed SDK (Autodesk), an undocumented binary format, a per-OS runtime library, and a licensing minefield. Every engine that could leave it, left it: Unreal added glTF export, Godot dropped FBX2glTF-dependent import for native glTF, and O3DE routes import through it only as a compatibility fallback. Hand-writing an FBX parser is a multi-month tar pit; linking Autodesk's SDK violates the single-purpose-library policy.
- **OBJ/MTL** — trivial to parse but no hierarchy, no skeleton, no animation, no PBR, no units. Fine for a test cube, useless as a backbone.
- **USD** — the right answer for *studios with a scene-sharing problem*; massively over-scoped (a DCC-interchange scene graph, not a game asset format) for a solo dev. Note USD's *lesson* though: layering and referencing done right is why Pixar built it — MCE gets 80% of that value from prefab + GUID refs.
- **alembic/caches** — animation caches, not asset sources. Irrelevant at MCE scale.

**Committed:** glTF 2.0 (`.gltf` and `.glb`) is the sole mesh/skeleton/animation interchange format. PNG/JPG for textures, WAV for audio, TTF for fonts — already fixed by FR-AST-010.

### 2.2 What glTF actually gives (and what the importer must extract)

The importer (cook-time only, hand-written per the binding from-scratch list — study `tinygltf` and cgltf for API shape, never link them; nlohmann/json is already approved for cook):

- **Meshes:** `meshes[] → primitives[]` with indexed or non-indexed accessors into shared `bufferViews`. The primitive is the real unit — one primitive = one draw = one material. MCE cooks *per primitive* to `CHNK_MESH` sub-mesh ranges. Accessor component types (byte/ushort/uint/float, normalized flags) mean the importer must normalize everything to float first, then quantize *once, at cook* (meshopt quantization) — never trust DCC accessor choices.
- **Materials:** core spec is metallic-roughness (`baseColorFactor/Texture`, `metallicFactor`, `roughnessFactor`, `normalTexture` + scale, `occlusionTexture`, `emissiveFactor/Texture`, `alphaMode`, `doubleSided`). This maps 1:1 onto MCE's forward-PBR param block. Commit: import *extracts* glTF materials into `.mcmat` files on first import (the Unity "materialize on import" move) so the editor has a native, editable material asset and glTF stays pure interchange. Re-import of a `.gltf` must **not** clobber hand-edited `.mcmat` files — re-extraction only for materials the glTF changed (keyed by material name + a content hash of its glTF definition; changed → new `.mcmat` version beside the old, with a console note, never a silent overwrite).
- **Scene/nodes:** a node hierarchy with TRS (note: scale is a plain vec3, non-uniform — MCE stores it as-is; the scene importer flattens the node graph into editor entities unless the node carries a mesh, matching Godot's "import as authored" stance).
- **Animation:** `animations[] → channels` targeting node TRS with linear/slerp/cubic-spline samplers. Commit (matches FR-R3D-010 tiering): MVP imports sampled node-transform clips (`CHNK_ANIM`, 30 Hz bake at cook — resample to fixed keys so runtime playback is a lerp, matching the fixed-timestep interpolation discipline); skinning/joints (`skins[]`) is v0.5 and the mesh chunk format already reserves joint/weight attribute slots.
- **Units and axes:** glTF is **meters, Y-up, right-handed, +Z forward** — exactly MCE's convention (binding math policy). The DCC pitfall is exporters: Blender exports Y-up correctly by default; Maya/3ds Max exporters rely on conversion flags. Cook validates bounds (a model whose extent is 1000 units is a Z-up export that was never converted → cook error, not a silent giant model). This is a two-line check that saves hours.

### 2.3 Extensions: an allowlist, not a rejectlist implementation

glTF extensions are the format's escape hatches and its biggest importer trap. The importer must implement a **closed allowlist and error on anything else, naming the extension** (FR-AST-010 posture; the architecture doc's risk #2 already locks this). The 2026-era landscape, from knowledge:

| Extension | Verdict | Why |
|---|---|---|
| `KHR_texture_basisu` | **Reject (won't)** | See §3 — MCE compresses BCn at cook from PNG/JPG sources; a KTX2 source adds a decoder for zero benefit on desktop-only GL 4.5 |
| `KHR_draco_mesh_compression`, `EXT_meshopt_compression` | **Reject** | Compression at the *interchange* layer is the wrong place; MCE does meshopt at cook from raw data. Accepting them means linking Draco/meshopt *decoders* into cook and losing determinism control over the input |
| `KHR_mesh_quantization` | **Accept (parse)** — it only constrains accessor types | The importer normalizes to float anyway; quantization happens at cook, once, under MCE's control |
| `KHR_materials_unlit`, `KHR_texture_transform` | **Reject for MVP**, warn | Cheap to add later; unlit maps to a material flag, UV transform to a `.mcmat` field |
| `KHR_materials_specular`, `KHR_materials_clearcoat`, `KHR_materials_emissive_strength`, `KHR_materials_variants` | **Reject** | Beyond MCE's forward-PBR param block; variants are a studio-scale authoring feature |
| `KHR_lights_punctual`, `EXT_mesh_gpu_instancing`, `KHR_animation_pointer` | **Reject for MVP**, warn | Lights are authored in the editor; instancing/pointer are v1.0-era sketch material at best |

Warning-not-failure on *known, harmless* rejects; hard failure on *unknown* extensions (an unknown extension may change how existing data must be read — silently ignoring it corrupts content).

### 2.4 DCC export quality is a content problem, not an importer problem

The classic solo-dev failure is building a tolerant importer that de-facto becomes a second DCC. The discipline (architecture doc risk #2): **if a DCC's glTF export is broken, fix the export settings, not the importer.** Blender's glTF exporter (maintained by Khronos-affiliated contributors) is the reference exporter; the sample project standardizes on Blender → glTF 2.0, and the importer's validation errors are written to be actionable against Blender's export options. `tinygltf`-style strictness (reject invalid ranges, validate accessor/bufferView pairing, bounds-check every index) is the model — glTF's own spec defines validators, and cook should fail loudly on invalid files rather than "best-effort" import them.

---

## 3. Texture compression: KTX2/BasisU vs BCn-at-cook

### 3.1 The survey

- **GPU-native block compression (BCn)** is what desktop GL 4.5 actually consumes: `GL_COMPRESSED_*_BC*` formats are uploadable directly from `glCompressedTextureSubImage2D` (DSA form) with zero runtime decode. The relevant blocks, per-class (FR-AST-050 already commits): **BC1** opaque color (8:1), **BC3/BC7** color+alpha (BC7 higher quality, slower to encode), **BC5** two-channel normals (best normal-map quality per bit, the standard since DirectX 10-era), **BC4** single-channel masks. Encoders in the wild: DirectXTex (Microsoft, the reference `texconv`), **ispc_texcomp** (Intel, SIMD, deterministic, job-parallel — the quality/speed/determinism sweet spot), Compressonator.
- **KTX2** (Khronos container, `libktx`) is a *generic* texture container: multiple mip levels, multiple "supercompression" schemes (Zstandard/Zstd, Zstandard-with-BasisU), metadata (KHR_df), and crucially **multiple codec variants** (Basis Universal ETC1S/UASTC planes) that a transcoder picks from at load. **Basis Universal** (Binomial, open-sourced 2019, backed by the KHR_texture_basisu glTF extension) encodes once to a universal intermediate and *transcodes* — CPU-cheap, no quality loss vs. direct encode — to whichever GPU format the device wants: BC1/3/5/7 on desktop, ETC/ASTC/PVRTC on mobile. Its design goal is exactly what a download-and-run-anywhere runtime (web, mobile catalogs, one-file-per-texture stores) needs.
- **Godot 4** ships BasisU inside its import cache: its VRAM-compressed import mode transcodes to the target platform's format at import time. **Unity** compresses to per-target-platform forms in `Library/` (BCn for D3D11+/GL-core desktop, ASTC/ETC2 for mobile), selectable per build target. **Bevy** processes images to whatever `wgpu` supports per platform, with KTX2/BasisU supported as a source for its `bevy_render` loaders. **Unreal** cooks per-platform texture formats into cooked packages. The pattern across all four: *the transcode/compress decision belongs to the build target, made at import/cook — never at runtime.*

### 3.2 The tradeoff, stated honestly

KTX2/BasisU buys: one source format covering all GPU families; small distribution size (ETC1S mode is aggressively compact); a standardized container with metadata. It costs: a **transcoder in the runtime or a decode step at load** (direct BCn upload needs the transcode step — that's the entire point of BasisU), an extra source format (artists producing KTX2 means a KTX2 pipeline anyway), and — decisive for MCE — **a foreign binary format in the content path whose determinism and chunk integration MCE does not control**.

MCE's target is **Windows x64 desktop, GL 4.5, forever** (committed stack). There is exactly one GPU family: BCn. The universal-transcode value proposition is empty; the runtime-transcode cost is pure waste; and the `.mce` container already *is* MCE's texture container — a chunk with 16-byte alignment, CRC, per-chunk schema version, and mmap-zero-copy load. Wrapping that in KTX2 would mean two containers doing one job.

**Committed (matches FR-AST-050 and architecture §5.2):**

- **Sources stay PNG/JPG** (stb_image decode at cook — decode time is irrelevant when it happens once per asset version).
- **Cook compresses to BCn directly**: BC1 (opaque color), BC7 (color+alpha where BC1 artifacts show), BC5 (tangent-space normals), per-class selection in the sidecar settings, via **ispc_texcomp** (deterministic, job-parallel; DirectXTex as the validation oracle for the determinism test — encode with both, compare, keep ispc output in shipped content). sRGB flag carried as chunk metadata with a cook-time assert against double-correction.
- **KTX2/BasisU: won't.** Recorded as a deliberate rejection, not an omission. If MCE ever grows a web or mobile target (it won't before 2030 at the stated scope), BasisU transcoding is a cook-stage *addition* behind the closed importer interface — the chunk format doesn't change, because `CHNK_TEX` already stores the *GPU-ready* bytes and the format enum lives in `TocEntry::user_flags`.
- Mips at cook (box filter MVP, Kaiser if artifacts show), never at runtime — a runtime mipchain build on the CPU is load-time waste; GL can't do it server-side fast enough to matter at these sizes.

---

## 4. Import → compile → cache pipeline design

### 4.1 How the surveyed engines do it

- **Unity:** every source asset gets a `.meta` (GUID + importer settings + version); import produces native artifacts in `Library/` keyed by a hash of (asset, importer, settings, Unity version). The 2020.2 "Asset Import Pipeline v2" rewrite moved from a per-asset cache to an **Artifact Database** — content-addressed artifact files — precisely because the old mtime-based `Library/` was untrustworthy across VCS operations (Unity's own changelog and blog: "reduces project size, avoids reimport churn, makes cache sharing possible"). The **Accelerator/Cache Server** exists because "reimport everything on branch switch" hurt enough that teams bought infrastructure for it. Lesson: the cache key must include *everything that affects output*, and the cache must survive VCS round-trips.
- **Unreal:** the **Derived Data Cache (DDC)** — a shared, versioned, content-keyed store of *derived* data (cooked textures, compiled shaders, build meshes) keyed by `class + version + settings + source hash`. Rules Unreal's own docs state: DDC is disposable, rebuildable, and *must never be a source of truth*; cooked build outputs live in `.pak`/IoStore instead. The "disposable cache, authoritative sources" separation is the load-bearing idea.
- **Godot 4:** `.import` sidecar per source (importer name + settings + `uid://` ID) + a cache under `.godot/imported/` holding the reimported binary form, named by source path hash + params hash. The editor scans on focus, reimports what changed. Godot's *runtime* additionally loads binary `.res`/`.scn` resources — imported assets are stored in their imported (often binary) form and referenced by UID. Godot's pain point, per its own issue tracker: `.godot/` must be excluded from VCS, and imports are re-run by content hash, making editor boot after a branch switch slower than it should be — the cost of a cache that is neither content-addressed on disk nor disposable-documented.
- **Bevy (asset pipeline v2, 0.12+):** an `AssetServer` for unprocessed assets plus an **asset processor** producing "processed" assets under `imported_assets/` keyed by (source path, asset hash, settings hash, processor version), with the processed forms mounted read-only at runtime. Bevy *documents* that the processor output is git-ignorable and deterministically rebuildable — the same "sources authoritative, derived disposable" contract. Bevy's known gaps (from its own tracking issues): path-keyed identity (rename = break references until the editor fixes them), and dependency declaration that is best-effort for scene-internal references.
- **Bitsquid/Our Machinery** (the small-engine corpus MCE most resembles): Bitsquid serialized editor data as human-readable units, cooked to binary "compiled" units, with a build pipeline that is a pure function `source set → package set` (Niklas Frykholm's Bitsquid blog posts on build pipelines and unit compilation). Our Machinery's asset posts describe watching sources, re-importing on change, and treating the cooked form as a pure cache — *no editor-only import path* — which is the specific discipline MCE's reload design (architecture §7.5) copies.
- **id Tech** (Sanglard, *Game Engine Black Books*): compile-time-checked asset manifests, runtime formats designed so "load = mmap + header validate" whenever possible. The 60-second-full-cook / 5-second-incremental targets (FR-AST-070/080) are calibrated so this shape stays true at sample-project scale.

### 4.2 What goes in the cache key (the part everyone gets wrong once)

A cached output is valid iff **every input to the pure cook function is unchanged**. The key must therefore be a hash over: source content bytes (not mtime — mtimes lie across checkouts; hash with `xxhash64`, mtime as a *short-circuit only*), importer settings (the sidecar `settings` blob), **importer code version** (a hand-bumped integer per importer), **tool versions** (meshoptimizer, glslang, ispc_texcomp, MCE itself — pinned in vcpkg and recorded), and the output hashes of all **dependencies**. Omit tool versions and "we changed the quantizer" becomes archaeology; omit dependency hashes and editing a texture referenced by a material leaves stale material blobs. This is the exact key tuple in architecture §5.3, and Unity's post-2020 Artifact DB and Unreal's DDC version tuples are the industry precedent for including tool/importer versions.

### 4.3 Recommendation for MCE — pipeline shape

Committed (architecture §5 is the normative elaboration):

- **One cook tool, one code path, editor-integrated.** `mce_cook` is a library with a CLI skin; the editor's cook button calls the same `CookService`. No editor-only import path (Our Machinery lesson; Unity's drift warning).
- **Dev layout:** `assets/` (sources + `.mceasset` sidecars, in git) → `cooked/<aa>/<guid>/<content-hash>.mce` (git-ignored, content-addressed, temp-then-rename) + `cooked/manifest.mce` (GUID → {logical path, hash, offsets}). **Ship layout:** one packed `assets.mce` (chunk-concatenation, TOC rewrite, `--pack`).
- **Incremental:** re-cook iff any key component changed; reverse-dep edges re-cook dependents; **the cook DB is a disposable rebuildable journal** (Unreal DDC contract: delete `.mcedb`, full re-cook, identical outputs — and the determinism CI test proves it).
- **Determinism is a tested property, not a hope** (FR-AST-030): pure stages, parallel-disjoint outputs, GUID-sorted assembly, pinned tools. CI: cook, hash, cook, hash, equal.
- **Load path is mmap + validate, zero decode** — the payoff for cooking GPU-ready forms (id Tech / architecture §3 contract).

---

## 5. GUID-based asset databases

### 5.1 The survey

- **Unity:** `.meta` sidecar per file, containing a 128-bit GUID (plus importer settings). References in scenes/prefabs/materials are `fileID` + GUID pairs in YAML. Moves in-Editor preserve the GUID because the sidecar moves with the file; **external moves/renames break references until the editor's refresh re-links** — the classic failure the sidecar model tolerates only if the sidecar travels with the source. GUIDs are the *only* stable identity; paths are a hint. This is the pattern MCE's binding (FR-AST-040) adopts.
- **Godot:** historically path-based; Godot 4.0 introduced `uid://` resource IDs stored *inside* imported resources and inside `.import` files; Godot 4.4 added **`.uid` sidecar files for non-imported text resources** (scripts, shaders) precisely because path-references broke on rename — i.e., Godot *converged on the Unity sidecar model* when its own rename pain became intolerable. That convergence is the strongest available evidence that sidecars are the right default for a file-tree-based project.
- **Unreal:** `.uasset` packages carry their own GUIDs and name tables; cross-references are **soft** (path-based, null-tolerant) or **hard** (load-on-load). Unreal survives path moves via a redirector system written on move — an in-editor operation. Lesson: rename/move must be an *editor-aware operation that rewrites references*; a raw filesystem rename is a repair scenario (MCE: content-hash auto-repair + info log, architecture §4.2).
- **Bevy:** no GUID database. Assets are keyed by path; handles are weak/strong typed keys. Simple, zero ceremony, and — per Bevy's own asset docs and issue tracker — the source of its worst UX (renames rewire nothing; "missing asset" errors point at a path, not a stable object). MCE should not copy this even though Bevy's *processing* architecture is otherwise excellent.
- **Central-DB alternative** (a project-wide `.mcedb` registry file, Unreal-style global registry): rejected. A central DB is a merge-conflict generator in git, a single point of corruption, and unnecessary at 10k-asset scale; sidecars parallelize across files and survive partial checkouts. Unity's Accelerator exists because central DBs *scale* — MCE never will need to.

### 5.2 Recommendation for MCE — identity

Committed (architecture §4 is normative): **128-bit UUIDv4 GUIDs in `<file>.mceasset` sidecars checked into VCS beside the source; MCE-native text assets (`.mcscene`/`.mcmat`/`.mcprefab`) carry the GUID inline** (they're already MCE-controlled files — a second file per scene is noise). GUID created once at first import (explicit "adopt" step, never silent), stable across move/rename/re-import; registry uniqueness enforced every scan (duplicate GUID = batched cook error); external renames auto-repaired by content-hash match. References in cooked chunks are 16-byte GUIDs, never paths; at runtime, cook interns GUIDs → stable 32-bit `AssetKey`s baked into the manifest, so the runtime never parses strings. This is the Unity model minus the central DB, with the Godot-4.4 convergence as validation.

---

## 6. Dependency tracking & hot reload

### 6.1 Who declares dependencies

Committed: **importers declare** external references at import time (glTF → texture URIs resolved to GUIDs; `.mcmat` → texture GUIDs; `.mcscene` → embedded GUIDs, entity refs, prefab refs). No source-tree grep heuristics. Consequences, all already in the architecture doc and all validated by the surveys: reverse edges power find-references and the reload affected-set; cycles among MCE-native text assets are a hard cook error naming the cycle; glTF/PNG/WAV cannot form cycles (they reference only opaque assets), so the cycle check is cheap. Error reporting must be batched — a cook that reveals one error per run is a broken cook (architecture §5.1; Bevy's per-asset error surfacing is the cautionary example, Unreal's compile-error batching the aspiration).

### 6.2 Hot reload across engines

- **Unity:** asset changes trigger reimport, but *scripts* trigger **domain reload** (Mono AppDomain teardown) — historically seconds, and the perennial "why is my iteration slow" complaint (fixed only partially by Enter Play Mode Options). Lesson: reload must be *data-scope*; nothing in MCE's design may require tearing down code or world state to swap a texture.
- **Godot:** imported resources hot-reload on disk change (textures update live; scenes reload if not running); scripts hot-reload per-language. Reasonable behavior, and Godot's guard — scenes in the running game don't hot-swap — matches MCE's "reload the scene document only outside play mode" (architecture §7.5).
- **Bevy:** first-class **hot reloading** via a file watcher (`notify` crate) → `Assets<T>` mutation events → systems observe `AssetEvent::Modified`. It works because Bevy's handles are generational and consumers poll events — the same shape as MCE's `AssetHandle` generation + `AssetReloaded` event. Bevy is the closest surveyed analog to MCE's reload design and validates it end-to-end.
- **Our Machinery:** live editing as a core principle — everything is data, watchers re-import, the engine re-loads, nothing reboots. The philosophical anchor: if a content change requires a restart, the pipeline is broken.
- **The Windows-specific landmine** (architecture §3.1, risk #3): you cannot rename-over or delete a memory-mapped file on Windows without `ACCESS_DENIED`. Engines that mmap content must therefore never reload *in place*. MCE's content-addressed outputs (new file + manifest swap + deferred unmap) dissolve the problem structurally — the same trick the .NET runtime uses for shadow-copying assemblies and what Unreal's DDC does with hashed filenames.

### 6.3 Recommendation for MCE — reload behavior

Committed (architecture §7.5 normative; summary here for the research record):

- **Watch:** editor-owned 500 ms mtime poll on the IO worker (not `ReadDirectoryChangesW` — event storms and editor-only lifetime buy nothing; SDL3 has no watcher). Shipped games never spawn it.
- **Path:** save → hash-check against cook DB → (changed) re-import with *the same importer code as full cook* → reverse-dep affected set → job-parallel re-cook → write content-addressed outputs temp-then-rename → fsync → commit DB+manifest → main-thread drain at frame start → `commit_reload` bumps handle generations → old blob freed after the frame fence at refcount zero.
- **Failure modes are visible:** cook error → error panel with file + node-path/line, old content keeps serving; failed texture → magenta placeholder, handles stable (FR-DX-050). All-or-nothing per-asset commit; a mid-reload crash leaves the last good cook as truth.
- **Budgets:** save-to-pixels ≤ 1 s for texture/mesh/material/scene (expected ~50–150 ms); shader fast path bypasses the queue (validate → recompile on render thread → old program keeps rendering on failure) — the DX-H1 iteration win the whole GL stack was chosen for.
- **Scope guard:** reload machinery is editor-only and never linked into the shipped runtime (FR-RUN-010; CI symbol scan).

---

## 7. Scene serialization formats

### 7.1 The survey

| Engine | Authoring format | Runtime format | Notes |
|---|---|---|---|
| Unity | YAML scenes/prefabs, GUID refs, reflection-driven (serializable fields only) | cooked binary in `Library/`/serialized files | The "reflection is the only serialization path" rule is Unity's, and it's correct |
| Godot | `.tscn` text (ini-like, node-tree oriented) / binary `.scn` twin | compiled resources in `.godot/imported/`, packed `.pck` | The text/binary *twin format* with identical semantics is the elegant move |
| Bevy | RON `DynamicScene` (text, human-editable) | processed binary assets | Bevy's scene files are intentionally debuggable; its own docs call them a starting point, not a save format |
| Unreal | `.umap` binary + `.uasset` (editor-owned, binary) | `.pak` cooked packages | The counter-example: no diffable authoring format; code review and collaboration suffer, hence Unreal's growing text-source tooling (e.g., externalized plot/data tables) |

Decision inputs:

- **JSON vs YAML vs RON/custom-text.** YAML's spec complexity (anchor/merge semantics, the Norway problem, float/sexagesimal parsing variance) is a known serialization trap — Unity's YAML subset is hand-rolled for a reason. RON is Rust-ecosystem-bound. **JSON wins for MCE**: nlohmann/json is already approved cook-side, every diff tool speaks it, and canonicalization (sorted keys, shortest-round-trip float printing) is well-understood. The Bitsquid precedent is decisive at this scale: Bitsquid built an entire engine on human-readable JSON-family formats for authoring + compiled binary for runtime, and shipped on it for years.
- **Text authoring + binary runtime** is the converged shape (Unity, Godot, Bevy). Binary-only authoring (Unreal) is the documented pain; text-only runtime (Godot's early years) is the load-speed trap.
- **Entity-row orientation.** Godot's `.tscn` node-per-block layout is the human-diffable shape; Unity's YAML is flatter but GUID-noisy. MCE's `.mcscene` (architecture §6) is entity-row + component-nested with typed `"guid:..."` refs — one place, written and read only by the reflection-driven serializer (binding: no ad-hoc field lists).

### 7.2 Recommendation for MCE — serialization

Committed: **`.mcscene` JSON text** (editor canonical, autosave format, git-diffable, versioned, migratable at *editor* load) cooked to a **columnar `CHNK_SCENE` binary** (component columns + entity-id remap + GUID→AssetKey fixups) — the Unity/Godot/Bevy-converged text-source→binary-runtime pair, with the cook as the only transform. Runtime never sees JSON (FR-AST-010; the scene chunk's cook embeds `xxhash64` of the canonical source for the FR-ENT-060 round-trip test).

---

## 8. Versioning & migration

The survey's cautionary tales: **Godot 3→4** (a scene-format and API break so large the ecosystem fractured for years — the single best argument for never making version upgrades unbounded); **Unity's serialization versioning** (per-object version numbers + type trees, enabling *forward* compatibility of old builds reading new data — powerful, and one of the few places Unity's engine complexity earns its keep); **Unreal's `FCustomVersion`** registry (per-package version GUIDs, with migration code living next to the type); glTF's own **extension mechanism** (features behind named extensions, never silent semantic changes — the discipline MCE should steal for its own formats).

Recommendations, committed (architecture §3/§6 normative):

1. **Version at three granularities:** container major/minor (breaking vs additive, reader rejects major-ahead with a diagnosable error); **per-chunk schema version** (each type evolves independently); scene-document `"version"` integer.
2. **Additive-only within a major.** Append to the chunk-type enum and Toc flags; never repurpose fields; zero the reserved bytes and *reject nonzero* on read so future extensions can't silently corrupt old readers.
3. **Migration happens at editor load, in one table.** `scene_migrators[old → new]` chain, run when the *editor* opens an old document (interactive, user-visible, file upgraded on save). **Cook never migrates** — it accepts only the current version and errors older ones with "open and re-save in the editor first." One code path, the migration is reviewable, and the runtime never carries migration code at all (a shipped `.mce` pack is always current-version by construction, since it was just cooked).
4. **Importer/tool versions are cache keys** (§4.2): an importer upgrade re-cooks dependents automatically — the Unity/Unreal pattern, and the difference between "we improved the normal-map quantizer" being one flag flip versus an archaeology project.
5. **A `version` bump is an ADR.** Any chunk schema change requires `docs/adr/` (the binding dependency policy's ADR habit, extended to formats) — version bumps are cheap, silent version rot is not.

---

## 9. Recommendation for MCE — the committed pipeline in one place

The decisions below are the research doc's contract with `docs/architecture/assets.md` (normative detail lives there; where they disagree, REQUIREMENTS FR-AST-* wins). Everything here is already binding-consistent: from-scratch importer, cook-time-only deps, `.mce` containers, no runtime JSON/PNG, reflection-only serialization.

| # | Decision | Committed choice | Rejected alternative & why |
|---|---|---|---|
| 1 | 3D interchange | **glTF 2.0** (`\|.gltf`/`\|.glb`), closed extension allowlist, bounds/axes validated at cook | FBX (closed SDK/licensing), OBJ (no hierarchy/anim), USD (studio-scale scene graph, wrong tool) |
| 2 | Materials | Extract glTF PBR → native **`.mcmat`** on first import; re-import never silently clobbers edits | Editing glTF-embedded materials directly (no editor handle, no diffability) |
| 3 | Textures | **PNG/JPG sources → BC1/5/7 at cook (ispc_texcomp)**, mips at cook, sRGB flag in metadata | KTX2/BasisU (§3.2: desktop-only target, transcode cost, foreign container in the content path); runtime mips |
| 4 | Runtime formats | Versioned chunked **`.mce`** containers, content-addressed filenames, mmap+validate load, zero decode | Generic compression in MVP (mmap zero-copy beats pack size at this scale); KTX2 as runtime container |
| 5 | Identity | **128-bit GUID sidecars (`.mceasset`)**, inline GUIDs for MCE-native text assets, cook-interned 32-bit `AssetKey`s at runtime | Path-keyed identity (Bevy's known pain), central DB (merge-conflict generator at solo scale) |
| 6 | Cook pipeline | One library (`mce_cook` CLI + editor `CookService`), importer-declared dep graph, Kahn topological order, job-parallel pure stages, content-hash cache incl. importer+tool versions, disposable journal DB | mtime-keyed cache (lies across checkouts), editor-only import path (drift), build-system-based cooking (CMake-cook coupling) |
| 7 | Determinism | Pure stages + parallel-disjoint outputs + GUID-sorted assembly + pinned tools; **CI: cook-hash-cook-hash equality every PR** | "Usually deterministic" (untestable, and pack assembly becomes uncacheable) |
| 8 | Hot reload | Editor-owned 500 ms mtime poll → same importer code as cook → reverse-dep re-cook → all-or-nothing commit → generation bump at frame start → deferred free post-fence; ≤ 1 s budget; magenta-placeholder failure mode | `ReadDirectoryChangesW` (event storms, no benefit at editor scale); in-place file replacement (Windows mmap `ACCESS_DENIED`); restart-on-change (Our Machinery's axiom: broken pipeline) |
| 9 | Scene format | **`.mcscene` JSON text** (editor canonical, reflection-driven, canonicalized for hashing) → **columnar `CHNK_SCENE`** binary at cook | YAML (spec complexity), binary-only authoring (no diff), text at runtime (load speed, FR-AST-010) |
| 10 | Versioning/migration | Three granularities (container / chunk / scene-doc), additive-only within major, editor-run migrator table, cook rejects-and-defers, version bumps require ADR | Cook-time auto-migration (two code paths, runtime carries migrators), Godot-3→4-style unbounded breaks |

**On-disk layout (committed shape):**

```
project/
  assets/                     # in git — human-authored truth
    models/hero.gltf          + hero.gltf.mceasset   (GUID + importer settings)
    textures/hero_alb.png     + hero_alb.png.mceasset
    materials/hero.mat        (.mcmat — GUID inline)
    scenes/level_01.mcscene   (GUID inline, "version": N)
    audio/... .wav            fonts/... .ttf         input/... .mcinput
  cooked/                     # git-ignored, disposable (Unreal-DDC contract)
    .mcedb                    cook DB: append-only journal + snapshot
    manifest.mce              GUID -> {logical path, content hash}  (atomically swapped)
    <aa>/<guid>/<hash>.mce    content-addressed per-asset containers
  game.mce                    # ship: single pack (chunk-concatenated, --pack)
```

**Reload behavior in one line:** *watch (500 ms poll) → hash-check → re-import (same code as cook) → re-cook the reverse-dep affected set on jobs → write content-named files temp-then-rename → fsync → commit DB+manifest → frame-start drain bumps handle generations → old blob freed after the frame fence.* Shader changes take the ≤ 150 ms bypass path; every failure is a visible, non-crashing degradation; shipped runtimes contain none of this machinery.

---

## 10. References (canonical, from knowledge)

- Khronos Group, *glTF 2.0 Specification* (core profile; extension registry: `KHR_texture_basisu`, `KHR_draco_mesh_compression`, `EXT_meshopt_compression`, `KHR_mesh_quantization`, `KHR_materials_*`); glTF-Validator conformance tooling.
- Khronos Group, *KTX2 container spec* + **Basis Universal** (Binomial LLC) encoder/transcoder design docs (ETC1S/UASTC modes, `transcodeTo*` targets).
- Zeux (Arseny Kapoulkine), **meshoptimizer** README/API: `meshopt_optimizeVertexCache`, `meshopt_optimizeVertexFetchRemap`, `meshopt_simplify`, quantization notes; "Small Ammunition" / vertex-cache talks.
- Intel **ispc_texcomp** (BC7/BC6H ISPC encoders); Microsoft **DirectXTex**/`texconv` (reference BCn encoder, test oracle).
- Unity Technologies — asset pipeline documentation and the 2020.2 Import Pipeline v2 / Artifact Database changelog; `.meta` GUID model; YAML serialization + type trees.
- Epic Games — Unreal Engine docs: **Derived Data Cache** (shared, disposable, version-keyed), `.uasset` package format, `FCustomVersion` serialization versioning, cooked `.pak` format.
- Godot Engine — docs + source: import system (`.import` sidecars, `.godot/imported/` cache, `uid://` IDs), 4.4 `.uid` sidecar files, `.tscn`/`.scn` text-binary resource twins, `.pck` packs.
- Bevy — `bevy_asset` docs (AssetServer, processed assets, hot reload via `notify`), Bevy 0.12 asset re-architecture notes (Cart).
- Niklas Frykholm, Bitsquid foundation blog posts on build pipelines, unit compilation, and data serialization; Our Machinery blog, asset/live-reload posts.
- François G. / Sanglard, *Game Engine Black Books* (id Tech: WAD/pk3 container discipline, compile-time asset checks).
- Jason Gregory, *Game Engine Architecture*, 3rd ed., ch. 6 (containers/allocators for cook), ch. 20 ("A Game of Resources" — resource pipelines, GUIDs, reload).
- Glenn Fiedler, Gaffer On Games, "Fix Your Timestep" (sim/render separation underpinning sampled-animation and reload-timing decisions).
- Erin Catto, Box2D GDC slides (dependency of the from-scratch physics on cooked collider data — cross-reference `physics.md`).
