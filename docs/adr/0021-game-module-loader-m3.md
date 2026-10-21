# ADR-0021 — The M3 game-module loader and the `mce_engine` shared library, as built

**Status:** Accepted (2026-10-08).

**Context.** `scripting.md` §2.1–§2.2 and §3.1 commit the game-module boundary: a game module exports one
symbol (`mce_game_module`) returning a `ModuleInfo{install(World&)}`, the loader copies the module before
mapping it, `MCE_ENGINE_ID` gates stale builds, and host and module share one engine instance through the
`mce_engine` umbrella shared library. ROADMAP puts the loader at M3 and the `mce_engine.dll` packaging at M4.
A loader without a shared engine would give the module its own copy of every engine global (convar registry,
log, refl registry), so the umbrella is needed now.

**Decisions.**

1. **`mce_engine` is built now, on ELF platforms only.** It is a shared library containing every runtime engine
   library linked whole (`--whole-archive`), with default symbol visibility, so the engine's symbols are its
   exports. All code is built position-independent (`CMAKE_POSITION_INDEPENDENT_CODE`). On Windows the umbrella
   needs the `MCE_API` decorations and the explicit-instantiation ledger (finding F33), which stay M4 work; until
   then `MCE_GAME_MODULES_SUPPORTED` is OFF there and module targets/tests are skipped. The loader itself
   (`mce_module_host`) builds everywhere.
2. **Hosts link `mce::mce_engine` and no static engine library.** Static engine libraries in the same process would
   be a second engine instance. `mce_engine`'s interface carries the engine libraries' compile requirements
   (`$<COMPILE_ONLY:...>`) without linking them. Existing samples and unit tests keep linking the static libraries:
   they never load a module.
3. **`install` returns `ResultV`** rather than `void`, so `Errc::ModuleInstallFailed` has an error to report.
   After a failed install the World may already hold the module's code pointers, so the library stays mapped for
   the process lifetime and the caller discards the World.
4. **Teardown order is checked.** The loader puts a binding resource in the World; its destructor tells the loader
   the World is gone, and `unload_game_module` asserts that before unmapping. One module per World.
5. **`MCE_ENGINE_ID`** is the first 64 bits of SHA-256 over `git describe --dirty`, build type, compiler id/version
   and the ASan setting, generated at configure time into `mce/engine_version.h` (CMake has FNV only through
   custom code; any stable hash serves). Engine and modules built in one tree always agree.
6. **Copy-and-load** maps `<stem>_loaded_<N><ext>` next to the original, `N` being the lowest index not currently
   mapped, and deletes the copy on unload. Up to 8 modules can be mapped at once.
7. **Engine systems belong to the host.** A host installs the runtime systems (input resource, physics2d, audio
   bridge, render extract) on the World before loading the module; `install` registers only the game's own
   components, events, systems and resources.
8. **Headless module host.** `mce_devhost -module <path> -ticks N` ticks a module through the one app loop and
   prints the World's `state_hash`; local CI runs it for determinism (3 runs, Debug vs Release).

**Not in this change.** The `mce/gameplay.h` include wall (the module currently sees every engine include
directory; the header is the convention), the MSVC export ledger, the editor's run-in-player button and
play-in-editor (M5b).

## Amendment 2026-10-11

- **macOS.** `mce_engine` is also built on macOS, where `-Wl,-force_load,<lib>` replaces `--whole-archive`
  (`src/module/CMakeLists.txt`). Windows still links the static libraries (no export ledger yet).
- **Read-only installs.** When the copy cannot be written next to the module (a read-only folder, a translocated
  `.app`), the host maps the module in place and logs it (`src/module/src/module_host.cpp`); only rebuilding a
  mapped module needs the copy.
- **More hosts load modules.** The editor, `mce_mcp` and `mce_cook` (and their libraries) are `UMBRELLA` targets
  (`cmake/MceTarget.cmake`): where modules are supported they link `mce_engine` instead of the static engine
  libraries, so they share one engine instance with the module they load. The editor loads one with `-module`
  (ADR-0029 amendment).
