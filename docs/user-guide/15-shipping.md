# 15. Shipping

One command builds, cooks, packages and zips a game:

```sh
python3 scripts/ship.py --project projects/MyGame
python3 scripts/ship.py --project projects/MyGame --version 1.0.0 --preset linux-gcc-release
python3 scripts/ship.py --project projects/MyGame --smoke --smoke-headless   # run it once after packaging
```

Output: `dist/<Name>-<version>-<os>.zip` and, beside it, `<Name>-<version>-<os>-symbols.zip`, where
`<os>` is `linux-x64`, `win64` or `macos`.

| Option | Meaning |
|---|---|
| `--preset` | the CMake preset to build (default: the platform's release preset) |
| `--version` | default `<engine version>+<short commit>` |
| `--out` | output directory (default `dist/`) |
| `--no-build` | package what is already built |
| `--keep-symbols` | do not strip the binaries (Linux, macOS) |
| `--smoke`, `--smoke-headless` | run the packaged game once (headless needs no display) |

The samples ship the same way: `python3 scripts/ship.py platformer` (Ember Step) or `diorama`.

## 15.1 What is in the zip

One folder, double-click the executable (on macOS the folder holds `<Name>.app` and `README.txt`;
see section 15.4):

| File | What |
|---|---|
| `<Name>` (`.exe` on Windows, with icon and version resource) | the host |
| `lib<game>.so` | the game module, `.so` on macOS too (not on Windows, where it is linked in) |
| `libmce_engine.so` | the engine (`libmce_engine.dylib` on macOS) |
| `assets.mce` | the cooked pack, CRC-checked once at mount |
| `VERSION.txt` | version, engine commit, preset, file hashes, one build id per binary |
| `CREDITS.md` | copied from the project (content attribution) |
| `README.txt` | how to run, controls |

Executables load from their own folder (relative rpaths), never from the build tree, so the folder
can be moved or read-only.

## 15.2 Shipped-build behaviour

- **Cheats are locked** before the command line is parsed: `time_scale 5` on the console or
  `-time_scale 5` on the command line answers "is a cheat and cheats are locked in this build".
  Code can still set cheat convars (your own slow-motion).
- **The developer console stays** (backtick or F1); `-console 0` hides it.
- **Saves** go to the per-user folder; the install folder may be read-only.
- **Crash reports** are written to the per-user crashes folder (section 2.5): a plain-text
  `.crash` with the signal, the program counter, a module-plus-offset backtrace, loaded modules
  with build ids and the last 64 log breadcrumbs; Windows adds a minidump. `-crash_reports 0` turns
  it off, `-crash_dir` moves it, `-crash_test 1` crashes on purpose to prove the chain.
- `<Name> -version` prints the version line and exits.

## 15.3 Symbols and crash reports

Shipped binaries are stripped; their debug info is in the symbols zip, which you keep and do not
distribute. To read a crash report from a player:

```sh
unzip MyGame-1.0.0-linux-x64-symbols.zip -d symbols/
python3 scripts/symbolicate.py player-report.crash --symbols symbols/
```

On Linux the symbols zip holds `objcopy --only-keep-debug` files under `.build-id/`, on Windows the
PDBs, on macOS one `.dSYM` per binary. Each frame resolves to a function, file and line (inlined frames included) when the symbols match
the build ids in the report. A dev-tree build can be symbolicated against `build/<preset>/`.

## 15.4 Platforms

| | Linux | Windows | macOS |
|---|---|---|---|
| Ship script | yes, tested | packages with icon and version resource; the crash path has not been exercised yet | yes: the zip holds `<Name>.app`, see below |
| Game module | shared object | linked into the host | shared object (`.so`) beside `libmce_engine.dylib` |
| Renderer | OpenGL 4.5 | OpenGL 4.5 | Metal |

**The macOS bundle** (`cmake/macos/macos_bundle.py`, called by `ship.py`): the executable, the
game module and `libmce_engine.dylib` go to `<Name>.app/Contents/MacOS/`; `assets.mce`,
`VERSION.txt` and `CREDITS.md` go to `Contents/Resources/`. Before bundling, every binary's debug
info is archived as a `.dSYM` in the symbols zip and the binaries are stripped (`--keep-symbols`
skips both). The bundle is signed ad hoc (`codesign -s -`), which Apple silicon requires; it is not
Developer ID signed or notarized, so a player may have to right-click it and choose Open the first
time. `Info.plist` asks for macOS 26.0 or later. When the `.app` runs from a read-only location
(App Translocation, `/Applications` without write access), the host maps the game module in place
instead of copying it (chapter 6, section 6.13).

## 15.5 Checklist before shipping

1. Every scene cooks clean (`my_game_cook projects/MyGame --pack` exits 0).
2. `my_game -headless 1 -ticks 3600` runs a minute without an error in the log.
3. `CREDITS.md` names every third-party asset and licence (Kenney CC0 packs, fonts, sounds).
4. `python3 scripts/ship.py --project projects/MyGame --smoke` starts and quits cleanly on the
   target platform.
5. Keep the symbols zip with the version.
