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
libxi-dev libxss-dev libxtst-dev libxfixes-dev libxkbcommon-dev`).
