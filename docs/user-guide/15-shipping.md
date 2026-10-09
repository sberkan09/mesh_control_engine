# 15. Shipping

One command builds, cooks, packages and zips a game:

```sh
python3 scripts/ship.py --project projects/MyGame
python3 scripts/ship.py --project projects/MyGame --version 1.0.0 --preset linux-gcc-release
python3 scripts/ship.py --project projects/MyGame --smoke --smoke-headless   # run it once after packaging
```

Output: `dist/<Name>-<version>-<os>.zip` and, beside it, `<Name>-<version>-<os>-symbols.zip`.

| Option | Meaning |
|---|---|
| `--preset` | the CMake preset to build (default: the platform's release preset) |
| `--version` | default `<engine version>+<short commit>` |
| `--out` | output directory (default `dist/`) |
| `--no-build` | package what is already built |
| `--keep-symbols` | do not strip the Linux binaries |
| `--smoke`, `--smoke-headless` | run the packaged game once (headless needs no display) |

The samples ship the same way: `python3 scripts/ship.py platformer` (Ember Step) or `diorama`.

## 15.1 What is in the zip

One folder, double-click the executable:

| File | What |
|---|---|
| `<Name>` (`.exe` on Windows, with icon and version resource) | the host |
| `lib<game>.so` / `.dylib` | the game module (not on Windows, where it is linked in) |
| `libmce_engine.so` | the engine |
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
unzip MyGame-1.0.0-linux-symbols.zip -d symbols/
python3 scripts/symbolicate.py player-report.crash --symbols symbols/
```

Each frame resolves to a function, file and line (inlined frames included) when the symbols match
the build ids in the report. A dev-tree build can be symbolicated against `build/<preset>/`.

## 15.4 Platforms

| | Linux | Windows | macOS |
|---|---|---|---|
| Ship script | yes, tested | packages with icon and version resource; the crash path has not been exercised yet | builds and runs on Metal; `.app` bundling is not done, the zip holds a plain folder |
| Game module | shared object | linked into the host | dylib |
| Renderer | OpenGL 4.5 | OpenGL 4.5 | Metal |

## 15.5 Checklist before shipping

1. Every scene cooks clean (`my_game_cook projects/MyGame --pack` exits 0).
2. `my_game -headless 1 -ticks 3600` runs a minute without an error in the log.
3. `CREDITS.md` names every third-party asset and licence (Kenney CC0 packs, fonts, sounds).
4. `python3 scripts/ship.py --project projects/MyGame --smoke` starts and quits cleanly on the
   target platform.
5. Keep the symbols zip with the version.
