# ADR-0018 — Portable build: Linux presets beside MSVC, CI on both

**Status:** Accepted (2026-10-07).

**Context.** The binding toolchain is MSVC v143 on Windows 10 (REQUIREMENTS §8 Q2), and the GL 4.5 reference
machine stays Windows. The implementing agents run on Linux, and the Mac migration note already expects a
second platform later.

**Decision.** Engine code stays portable C++20. `CMakePresets.json` carries `linux-gcc-*`, `linux-clang-*` and
`linux-asan` next to the `msvc-*` presets. The compile policy in `cmake/MceTarget.cmake` maps one-to-one:
`/EHs-c- /GR- /fp:precise` ≙ `-fno-exceptions -fno-rtti -ffp-contract=off`, warnings as errors on all.
CI runs both `windows-latest` (MSVC Debug + Release) and `ubuntu-24.04` (gcc Debug, clang Release, gcc ASan+UBSan).

**Consequences.** Windows-only features (minidumps, `OutputDebugStringA`, `MoveFileEx`) live behind
`#if defined(_WIN32)` inside `mce_platform`/`mce_core` with a POSIX twin. Determinism (ADR-0007) is still
defined per machine and binary; Linux hashes are not compared against Windows hashes.

## Amendment 2026-10-11

There is no hosted CI. The GitHub Actions workflows were removed on 2026-10-08 (commit "CI: drop GitHub Actions,
add scripts/ci/local_ci.sh"); CI is now `scripts/ci/local_ci.sh` (walls, the `linux-*` presets incl. ASan+UBSan,
clang-tidy, determinism) and `scripts/ci/local_ci_macos.sh` (the `macos-*` presets added by ADR-0028), run by the
developer or agent before merging. The `msvc-*` presets still exist in `CMakePresets.json`, but nothing runs them,
so the Windows build is unverified.
