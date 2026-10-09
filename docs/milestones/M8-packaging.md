# M8 — Packaging v2 (milestone record)

**Scope:** ROADMAP M8, "packaging v2 (symbolicated crash dumps for shipped builds, shipped debug console, version metadata)". The shipped console itself landed with `mce_ui` (`M8-ui.md`, `ui::DevConsole`). This record covers the rest and the cheat guard that makes a shipped console safe.
**Measured on:** the Linux CI container. The Windows path is written but not run here (no Windows runner in this environment).

| Item | Where | Evidence |
|---|---|---|
| Crash handler | `mce/crash.h` in `mce_core`. POSIX hooks SEGV, BUS, FPE, ILL and ABRT on a 64 KiB alternate stack. Paths and the header are formatted at `install()`. The handler writes `<dump_dir>/<app>-<UTC>-<pid>.crash` with raw `open`/`write`, then re-raises with the default action, so the parent still sees the signal. A crash inside the handler `_exit`s. Under ASan the hooks stay off and ASan's report wins. | `test_core` "a fatal signal writes a report and still dies of it": a forked child dies of SIGSEGV, and the report has the header, signal, `addr 0x0`, frames, build ids and the breadcrumb |
| Report contents | Header (`app`, `version`), signal, code and fault address, pid, faulting pc. The backtrace (from the faulting frame down) is printed as `pc base +offset [dynamic symbol]`. Every loaded module is listed with its base, GNU build id and path. The log breadcrumbs come last, read with a try-lock (`log::try_visit_breadcrumbs`) so a thread that died holding the ring cannot hang the report. | sample report below |
| Windows | `SetUnhandledExceptionFilter`. A `.dmp` minidump comes from `MiniDumpWriteDump`, with dbghelp loaded at install so `mce_core` still links only spdlog + Tracy. A `.crash` text file sits beside it with the exception code, address and module+offset frames. | Not run in this container |
| Default location | Per-user: `%LOCALAPPDATA%/MCE/<app>/crashes` or `$XDG_DATA_HOME` (`~/.local/share`)`/MCE/<app>/crashes`. A shipped folder can be read-only. `-crash_dir` overrides it and `-crash_reports 0` turns it off. | |
| Symbol archive | `scripts/ship.py` runs `objcopy --only-keep-debug` on each shipped binary into `.build-id/ab/cdef….debug`, then strips the staged copies. The archive is zipped as `<stem>-symbols.zip` beside the game zip, never inside it. On Windows it holds the PDBs. `VERSION.txt` gains one `build_id` line per binary. | Ember Step 0.5.0 shipped from the debug tree: zip + symbols zip; `INDEX.txt` lists 3 build ids (exe, `libmce_engine.so`, `libplatformer.so`) |
| Symbolication | `scripts/symbolicate.py <report> --symbols DIR…` maps each frame's base to a module and its build id. It looks for the debug file in the archive layout, then the module path from the report (same build id), then any ELF under DIR (a build tree). It runs `addr2line -f -C -i` at offset − 1 for return addresses and prints inlined frames. | ctest `player_crash_report`: `mce_player -crash_test 1` dies of SIGSEGV and its report resolves `#0 mce::crash::trigger_test_crash() at crash.cpp:…` and `mce::player::run` at `player.cpp:…` |
| Version metadata | `samples::begin_build()` / `start_build()` in `samples/common/ship_paths.h` are used by `mce_player`, every template project host, Ember Step, the Diorama and Ashen Court. A shipped folder's `VERSION.txt` names the build (`version` + `preset`); a dev build names the engine stamp (`mce::engine_describe()`, `mce/version.h`). The line is logged at start, goes into the crash report header, and `-version` prints it and exits. | `Ember-Step -version` → `03_platformer 0.5.0 linux-gcc-debug (engine …)` |
| Cheat guard | `console::set_cheats_allowed(false)` refuses `Cheat` convars on the console and the command line and resets them to their defaults. Code can still set them, for example a game's own slow-motion. A shipped folder locks cheats before the command line is parsed. | `test_core` "cheat lock (shipped builds)"; the shipped Ember Step refuses `-time_scale 5` ("cheats are locked in this build") |

Sample report from the shipped, stripped Ember Step, after `symbolicate.py --symbols <unzipped symbols>`:

```
MCE crash report v1
app 03_platformer
version 0.5.0 linux-gcc-debug
signal 11 (SIGSEGV) code 1 addr 0x0
backtrace:
#0 mce::crash::trigger_test_crash() at src/engine/core/src/crash.cpp:507   [libmce_engine.so +0x342f21]
#1 mce::samples::start_build(char const*, mce::samples::BuildInfo const&) at samples/common/ship_paths.h:94   [Ember-Step +0x275d8]
#2 main at samples/03_platformer/main.cpp:952   [Ember-Step +0x226c0]
...
breadcrumbs (oldest first):
  command line: 'time_scale' is a cheat and cheats are locked in this build
```

## Gaps

- The Windows path (minidump + text report) compiles only on Windows and has not been run. The first Windows build should run `-crash_test 1` and open the `.dmp` with the archived PDB.
- `abort()` on Windows does not pass through the exception filter, so an assert in a Windows release build leaves no report yet.
- Only the crashing thread is unwound on POSIX. Other threads' stacks are not in the text report (no minidump on Linux).
- Crash reports stay on the player's machine. There is no upload; reporting is manual.
