# ADR-0047 — FBX import over ufbx (cook-only)

**Status:** Proposed (2026-10-17), the coordinator's call as a new third-party dependency; accepted when the step-3a pin
PR merges. **Milestone:** M-IMPORT3D step 3 (3a the pin, 3b the importer). **Plan:**
[`docs/milestones/M-IMPORT3D.md`](../milestones/M-IMPORT3D.md). **Reference:** `cmake/Dependencies.cmake` (`mce_ufbx`),
`src/engine/asset/cook/src/ufbx_impl.c`, `tests/asset/test_ufbx_smoke.cpp`.

**Context.** Artists' tools export FBX first (Maya, 3ds Max, MotionBuilder, Mixamo, most store packs); glTF and OBJ
are already cooked by hand-written importers. FBX is a proprietary format (binary with zlib arrays, plus an ASCII
form), with many versions, axis and unit settings per file, pivots, and animation curves on Euler rotations.
Writing a parser ourselves is months of work for a format with no public specification. The importer runs only in
the cook (`mce_asset_cook`, `mce_cook`, the editor's cook service), never in a runtime target (ADR-0017, ARCHITECTURE
§7: cook-time deps never reach `mce_engine`). FBX files are untrusted input: they arrive from asset stores and from
whatever an agent downloads.

**Options weighed.**

| Option | For | Against |
|---|---|---|
| **ufbx** (MIT **or** Unlicense, Samuli Raivio; one `ufbx.c` + `ufbx.h`, no dependencies) | Binary and ASCII FBX from version 3000 on; unit and axis conversion, pivots, skins, blend shapes, animation evaluation and baking, its own inflate; about 33 k lines of C99 that compile in ~6 s (`-O2`) to ~370 KB of code; loads from memory; allocator callbacks with memory and allocation limits; fuzzed upstream (AFL on the binary, ASCII and deflate parsers, structured and semantic fuzzing, built-in truncation, byte-flip and out-of-memory runs) | One more pinned dependency to track; its float paths call libm (see Determinism) |
| Assimp (BSD-3-Clause) | Many formats behind one API | A large C++ library (hundreds of thousands of lines, zlib, its own CMake with options per importer), exceptions in its code paths (the engine builds `-fno-exceptions`), a long list of published fuzzing crashes and CVEs in its readers; most of its formats are ones we do not want to accept |
| Autodesk FBX SDK | The reference reader | Proprietary licence with redistribution terms; a binary-only install per platform and compiler (no source build, no FetchContent, nothing for Linux arm64); breaks the "clean clone, three commands" rule (NFR-B-01) |
| Our own parser | No dependency | No public spec; months of work to reach what ufbx already does |

## Decisions

1. **ufbx v0.23.1, pinned by commit `26a482ae66871d7de36eb722aa060bce95bce274`** (the tag's commit, 2026-09-25), via
   FetchContent with no `GIT_SHALLOW`, like tinyexr. Licence: the repo's `LICENSE` offers MIT (Copyright (c) 2020
   Samuli Raivio) or the Unlicense, the user's choice; we take MIT and list it in ARCHITECTURE §7. A bump is a reviewed
   one-line diff plus the smoke test's version check.
2. **`mce_ufbx`, a static library built like `mce_tinyexr`.** `src/engine/asset/cook/src/ufbx_impl.c` includes
   `ufbx.c`; warnings off (`-w`, `/W0`), `-ffp-contract=off` / `/fp:precise`, position-independent code, host builds
   only (a cross build cooks with the host's `mce_cook`). The define set stays inside the `.c` (none changes the
   header's types: `ufbx_real` stays `double`):
   - `UFBX_NO_FORMAT_OBJ`: ufbx can also read OBJ/MTL. The cook has its own OBJ importer (step 2); a hostile `.fbx`
     that sniffs as OBJ must not reach a second, unreviewed OBJ parser.
   - `UFBX_NO_GEOMETRY_CACHE`: the `.mc` / `.mcx` / `.pc2` vertex-cache readers are reachable from a scene that names
     external caches; the cook never asks for them.
   - Everything else stays on: triangulation, scene and skinning evaluation and animation baking are step 3b's tools;
     subdivision and tessellation run only when called. ufbx has no compile switch for threading, and needs none: it
     runs on the calling thread unless `ufbx_load_opts.thread_opts.pool` is set, which the importer never does (a
     thread pool would change only speed, but one thread keeps the cook's job scheduling the only parallelism).
3. **Walls.** The "ufbx wall" in `scripts/ci/check_walls.py` keeps `ufbx.h` / `ufbx.c` and every `ufbx_*(` call in
   `fbx_importer.cpp` (step 3b), `ufbx_impl.c` and the smoke test; the "ufbx link wall" keeps `mce_ufbx` in CMake files
   under `src/engine/asset/cook/` and `tests/asset/`. Nothing under `include/` names ufbx.
4. **Untrusted input: ufbx is fuzzed upstream, our importer still enforces limits.** ufbx's own fuzzing is the first
   line, not the only one. Without limits ufbx trusts an array header: a 150-byte binary file claiming 2^29 doubles
   makes it ask the allocator for 4 GiB (measured on the pin, raw and zlib-encoded arrays alike). The importer
   therefore always sets `temp_allocator.memory_limit` and `result_allocator.memory_limit` (the smoke test uses
   64 MiB each; step 3b picks the budget and names it in the error), loads from memory with
   `file_format = UFBX_FILE_FORMAT_FBX` (no content or extension sniffing), never sets `load_external_files`, and keeps
   its own caps on vertices, indices, joints and keys as the glTF and OBJ importers do. The smoke test shows a huge
   claim ending in `UFBX_ERROR_MEMORY_LIMIT` with no request above the limit, and every truncation of the cube, 400
   seeded byte mutations and a byte flip at every offset of the binary file returning an error or a walkable scene
   (clean under ASan + UBSan). An FBX file joins the M-FUZZ survey with step 3b's importer.
5. **Fallback: convert via Blender.** A file ufbx refuses (a format quirk, a limit the art really
   needs raised) is not a reason to add a second FBX reader: import it in Blender and export glTF (or FBX 7.4 binary),
   which the cook takes. Step 3b's import errors say so.

## Determinism

The cook must produce the same bytes on Linux and Windows (and macOS) for the same FBX. What ufbx does with floats:

- **Parsing** is integer and IEEE-exact: its decimal-to-double parser takes an exact fast path only when
  `FLT_EVAL_METHOD` is 0 or 1 and rounding is to nearest (checked at run time), else a big-integer path; both are
  correctly rounded, so ASCII and binary values read identically on every host. SSE2 is used only for memory copies and
  the Adler-32 checksum.
- **Unit and axis conversion** builds the axis matrix from 0 / +-1 entries and multiplies by the unit scale: plain
  IEEE multiplies and adds (and `sqrt`, correctly rounded) once `-ffp-contract=off` / `/fp:precise` stop the compiler
  from fusing them. `UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY` bakes the scale into the vertices; the axis turn stays a
  rotation in the node's adjust transform (the smoke test checks both, and that two loads give identical positions).
- **Not exact across hosts: libm.** `ufbx_euler_to_quat` (every node's `Lcl Rotation`, every evaluated rotation key)
  calls `sin` / `cos`; quaternion-to-Euler, slerp, layer weights and camera field of view call `atan2`, `asin`,
  `acos`, `pow`, `tan`. glibc, the MSVC CRT and Apple's libm are not bit-identical for these, so a node rotation or a
  sampled animation pose can differ by an ulp between hosts. Most such differences vanish in the mesh back end's
  quantization, but not all (a value on a rounding edge). Until the step-3b importer has a cross-host check, FBX cook
  golden hashes are pinned per host (as BC hashes are pinned to x86_64 until checked on arm64, ADR-0017
  amendment). If byte-identical FBX cooks across hosts become a requirement, `UFBX_EXTERNAL_MATH` lets the cook supply its own portable `sin` / `cos` / `atan2` /... to
  ufbx without patching it.
- glTF stores rotations as quaternions and needs none of this; the difference is FBX's Euler angles.

## Consequences

- One more FetchContent clone in a host configure (~1.4 MB of source) and ~6 s of compile for one translation unit;
  nothing reaches a runtime target.
- Step 3b writes `fbx_importer.cpp` on the step-0 mesh back end with the options above; step 4 reuses it for lights
  and cameras.
- A ufbx bump re-runs the smoke test and, from step 3b, the FBX cook goldens.
