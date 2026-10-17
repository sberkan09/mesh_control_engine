# ADR-0017 — Third-party dependencies via CMake FetchContent (pinned git tags)

**Status:** Accepted (2026-10-07) · supersedes the "vcpkg manifest" line of ARCHITECTURE §7 and core.md §1.4 for now.

**Context.** The architecture names a pinned vcpkg manifest. Implementation is agent-driven and the agents
build in sandboxes that can `git clone` from GitHub but cannot download release archives, which is how
vcpkg fetches its ports. NFR-B-01 (clean clone → running build in ≤ 3 commands) still has to hold on the
Windows reference machine.

**Decision.** Dependencies are declared in `cmake/Dependencies.cmake` with `FetchContent_Declare(GIT_REPOSITORY …
GIT_TAG <release tag> GIT_SHALLOW TRUE)`. Each tag bump is a reviewed one-line diff. The ARCHITECTURE §7
dependency rules are unchanged: `mce_core` links only spdlog + Tracy; anything new needs an ADR; cook-time-only
deps never reach `mce_engine`.

**Consequences.** `cmake --preset <p> && cmake --build --preset <p> && ctest --preset <p>` is the whole setup on
every machine (three commands, no vcpkg bootstrap). First configure clones the deps into the build tree
(~10 s). Swapping back to a vcpkg manifest later only touches `cmake/Dependencies.cmake`.

Pinned today: spdlog v1.17.0, Tracy v0.14.1 (on-demand client), doctest v2.5.3 (tests only), DirectXMath jun2026 (math oracle test only,
`test_math_oracle`), SDL3 release-3.4.18
(static; `mce_platform` + `mce_rhi_gl` only). glad2 is a generator, not a library: its GL 4.5 core output is
vendored in `third_party/glad` (the regenerate command is in `cmake/Dependencies.cmake`) and linked privately by
`mce_rhi_gl`. On Linux, SDL3 needs the X11 development headers (`libx11-dev libxext-dev libxcursor-dev libxrandr-dev
libxi-dev libxss-dev libxtst-dev libxfixes-dev libxkbcommon-dev`). stb @2c980bb is cook-time only (`mce_asset_cook`, ADR-0019), as is meshoptimizer v0.25 (ADR-0022).

## Amendment 2026-10-11

The "Pinned today" list above is a 2026-10-07 snapshot and is incomplete: Dear ImGui (docking), Box2D (test
oracle, ADR-0016), Jolt (ADR-0003), glslang (shader cook, ADR-0033) and, on macOS, glslang + SPIRV-Cross
(ADR-0028) were added later. The source of truth for what is fetched and at which tag is
`cmake/Dependencies.cmake` plus `cmake/macos/MetalDependencies.cmake`. SDL3 is also linked by `mce_rhi_metal`
(privately) and by `mce_editor_layer` (headers only where it links the `mce_engine` umbrella, linked on Windows),
not only by `mce_platform` and `mce_rhi_gl`.

## Amendment 2026-10-17

bc7enc_rdo (richgel999, MIT or Unlicense) joins the cook-time dependencies for block-compressed desktop textures
(M-CONTENT step 1b, `mce_cook --textures bc`). It has no tags, so `cmake/Dependencies.cmake` pins the commit
`b9438627eef73a1157e84201b6fa6eb2ffd6d9f0` (no `GIT_SHALLOW`, like tinyexr) and builds `bc7enc.cpp`, `rgbcx.cpp` and
`bc7decomp.cpp` (tests) as `mce_bc7enc`, with `-ffp-contract=off` so the cooked bytes do not depend on the compiler. The
repo's ISPC encoder, `ispc.exe` and RDO files are never built. The "bc7enc wall" and "bc7enc link wall" in
`scripts/ci/check_walls.py` confine it to the cook's `texture_bc.cpp` (and its test). Cook golden hashes of BC output are
guaranteed on x86_64 only until `tests/asset/test_cook_bc_arch.cpp` has run on arm64.

ufbx (MIT or Unlicense, Samuli Raivio) joins the cook-time dependencies for FBX import (M-IMPORT3D step 3, ADR-0047): release v0.23.1 pinned by
its commit `26a482ae66871d7de36eb722aa060bce95bce274` (no `GIT_SHALLOW`), built from `ufbx.c` through the cook's
`ufbx_impl.c` as `mce_ufbx` with `-ffp-contract=off` / `/fp:precise`, OBJ and geometry-cache readers compiled out. The
"ufbx wall" and "ufbx link wall" confine it to the cook's FBX importer and `tests/asset/`.
