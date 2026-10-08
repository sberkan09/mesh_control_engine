# Architecture: macOS port (`mce_rhi_metal`) — design and collaboration note

**Project:** Mesh Control Engine (MCE) · **Subsystem:** `rhi` (second backend) + `platform` (macOS) · **Date:** 2026-10-08
**Status:** Plan (design-only; no implementation yet). **Build:** ROADMAP M-MAC. **Requirement:** FR-SHP-061 (Should).
**Inputs:** `docs/REQUIREMENTS.md` §8 Q2 (the Mac migration note), ADR-0001 (two-layer RHI seam), ADR-0007
(determinism scope), ADR-0023 (3D render path), `docs/architecture/rhi-renderer.md`.

---

## 1. Why, and why Metal

The Mac migration note in REQUIREMENTS §8 Q2 already rules that the GL 4.5 backend is **not** downgraded to the
GL 4.1 subset macOS ships (no DSA, no persistent mapping, no MDI, no `ARB_shader_draw_parameters`, no compute), and
that the Mac gets a **second backend behind the `mce_rhi` seam**. This note picks **Metal** over MoltenVK:

- Metal is the native API on Apple silicon; MoltenVK would add a Vulkan backend *and* a translation layer, and
  Vulkan on its own is still N14 (deferred until both anchor games ship).
- The `mce_rhi` interface is already explicit-shaped (pipeline objects, recorded `CommandBuffer`, frame slots,
  barrier no-ops), which maps onto Metal command buffers/encoders with little friction.

The GL backend stays the **reference backend**: golden images, frame budgets (HD 630) and determinism baselines stay
anchored to it. Metal is a port, not a second source of truth.

## 2. What has to change (the seams)

| Seam | Today | Needed for Metal |
|---|---|---|
| Backend selection | Every host calls `rhi::gl::create_device(window, desc)` directly (`samples/*/main.cpp`, `src/player`, `src/editor/app`) | One factory in `mce_rhi` (e.g. `rhi::create_default_device`) that picks GL or Metal by platform/CMake option; hosts call only that |
| Window surface | `WindowDesc::opengl`, `SDL_WINDOW_OPENGL`, GL attributes in `window.cpp` | A Metal-capable window (`SDL_WINDOW_METAL` + `SDL_Metal_CreateView`/`CAMetalLayer`); the GL attribute block only on the GL path |
| Shaders | `ShaderSource::glsl` (GLSL 4.50, separable programs, `gl_PerVertex`, binding banks, `gl_BaseInstanceARB`) | MSL for Metal. Default: GLSL stays the single source and is translated **at cook time** (glslang → SPIR-V → SPIRV-Cross → MSL), so no translator links into a runtime target (FR-AST-010). ADR-0025 records the pick |
| MDI + base instance | `glMultiDrawElementsIndirect`, `gl_BaseInstanceARB` object data | `drawIndexedPrimitives` per indirect record (or ICBs later), `[[base_instance]]` |
| Persistent-mapped rings + fences | `glBufferStorage` + `glFenceSync`/`glClientWaitSync`, `MCE_FRAMES_IN_FLIGHT = 3` | `MTLStorageModeShared` buffers + a semaphore signalled from the command buffer completion handler, same 3 slots |
| Dynamic libraries | `load_library` (`.dll` / `.so`) | `.dylib`, plus `@rpath` for `game` modules and the player |
| Shipping | zip with `.exe` (Windows) / ELF (Linux) | `.app` bundle; signing/notarization out of scope until FR-SHP-030 says otherwise |

Determinism: `cmake/MceTarget.cmake` already passes `-ffp-contract=off` on every non-MSVC compiler, which matters
on arm64 (clang would otherwise fuse FMAs and Debug/Release hashes could drift). Per ADR-0007 the sim hash is only
required to be identical **within one machine and binary**; the Mac gets its own baselines and is never compared
against the Windows/Linux hashes.

## 3. Who owns what (cloud threads vs the local Mac session)

The port is built by a Claude Code session on the user's Mac, in parallel with the cloud threads that keep working
through the roadmap. To keep the two out of each other's way, files are split three ways.

**Mac-owned — only the Mac session edits these:**

- `src/engine/rhi_metal/**` (new module `mce_rhi_metal`, `.mm`/`.cpp`/`.h`)
- `tests/rhi_metal/**`
- `src/engine/platform/src/macos/**` (any macOS-only platform code: Metal view, bundle paths, `.dylib` loading)
- `cmake/macos/**`, the `macos-*` entries in `CMakePresets.json`
- `scripts/ci/local_ci_macos.sh`
- `docs/adr/0025-metal-backend.md`, `docs/milestones/M-MAC.md`

**Cloud-owned — the Mac session does not edit these:** everything under `src/engine/rhi_gl/**`, the renderer
(`src/engine/render/**`), and every other engine module. A rendering bug the Mac session finds in shared code goes
to the project chat as a report, not as a drive-by fix.

**Shared seams — touched only in small, dedicated "seam PRs":** `src/engine/rhi/include/**` (the RHI interface),
`src/engine/platform/include/mce/platform/window.h` + `window.cpp`, the host `main.cpp` files that create the device,
top-level/engine `CMakeLists.txt`, `CMakePresets.json` outside the `macos-*` entries, `scripts/ci/check_walls.py`.
Rules for a seam PR:

1. One concern per PR, branched from the latest `dev_claude`, merged quickly so it never sits under other work.
2. It must pass the full Linux `scripts/ci/local_ci.sh` before it merges (the Mac cannot run it, so a cloud thread
   verifies and merges it; ask in the project chat).
3. An `mce_rhi` interface change keeps `mce_rhi_gl` compiling and green in the same PR.

**Feature parity rule.** New RHI features (M5a PBR, CSM, post chain, …) land on GL first, from the cloud threads. No
cloud milestone ever waits for Metal. Catching Metal up to a new RHI feature is the Mac session's backlog, tracked in
`docs/milestones/M-MAC.md`.

**New wall.** Seam PR #1 adds a "Metal wall" to `check_walls.py`: `\bMTL[A-Z]\w*|[<"]Metal/|[<"]QuartzCore/` legal
only under `src/engine/rhi_metal/` and `src/engine/platform/src/macos/`, mirroring the GL wall.

## 4. Verification on the Mac (cannot run in the cloud)

The cloud containers are Linux; everything below runs only on the Mac, collected into `scripts/ci/local_ci_macos.sh`,
and must be green before a Mac PR merges. The Mac session pastes the summary table into the PR description.

1. `cmake --preset macos-debug && cmake --build --preset macos-debug && ctest --preset macos-debug` (Apple clang, arm64).
2. Same for `macos-release` and `macos-asan` (ASan + UBSan).
3. `python3 scripts/ci/check_walls.py` (GL, SDL and Metal walls).
4. GPU tests: `tests/rhi_metal` plus the backend-agnostic render tests against Metal; run once with
   `MTL_DEBUG_LAYER=1` and `MTL_SHADER_VALIDATION=1` with zero validation errors.
5. Samples: `01_triangle`, `sprites_2d`, `03_platformer`, `04_diorama` (and later `04_cubes`) open a window, render,
   and exit cleanly; screenshots attached to the PR. Golden images are compared perceptually against the GL
   reference (R9 policy), not byte-exact.
6. Determinism: `scripts/ci/determinism.py` on `02_boids`, `physics2d_headless`, `03_platformer -headless 1`,
   `04_diorama -headless 1`, the devhost module run and `physics3d_headless` — 3 runs each, Debug vs Release hashes
   equal on the Mac.
7. A Metal frame capture (Xcode GPU capture) per new render feature, the Metal counterpart of FR-DBG-020's RenderDoc
   rule.
8. Ship check: the packaged `.app` launches on the Mac from Finder with the cooked pack, 10-minute soak clean.

Anything that also changes shared seams additionally needs the Linux `local_ci.sh` run from §3.
