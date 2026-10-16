# 15. Shipping

From the SDK, with the path of your project folder (`MyGame` below), build the project and zip it:

```sh
mce build MyGame                        # cooks the assets, builds the module and the pack
mce ship MyGame                         # MyGame/dist/MyGame-<version>-<os>.zip
mce ship MyGame --version 1.0.0 --out release
```

`mce ship` zips the host, the game module, the engine library and the cooked pack, with `VERSION.txt`, `README.txt` and
your `CREDITS.md`; it stops with "run `mce build` first" when the pack is missing, and a script project (no C++
module) gets the stock player and a launcher script instead. `<os>` is `linux-x64`, `win64` or `macos`. It takes
`--target web` and `--steam` (15.6 and 15.7); it has no `--preset`, `--smoke` for desktop or symbols zip.

**From an engine checkout** (the repository, not the SDK) `scripts/ship.py` does the same with the full set of
options and is what the rest of this chapter shows: it builds the CMake preset itself, strips and keeps the
symbols, and runs a smoke test. Projects there live under `projects/`.

```sh
python3 scripts/ship.py --project projects/MyGame
python3 scripts/ship.py --project projects/MyGame --version 1.0.0 --preset linux-gcc-release
python3 scripts/ship.py --project projects/MyGame --smoke --smoke-headless   # run it once after packaging
```

Output: `dist/<Name>-<version>-<os>.zip` and, beside it, `<Name>-<version>-<os>-symbols.zip`.

| `ship.py` option | Meaning |
|---|---|
| `--preset` | the CMake preset to build (default: the platform's release preset) |
| `--version` | default `<engine version>+<short commit>` |
| `--out` | output directory (default `dist/`) |
| `--no-build` | package what is already built |
| `--keep-symbols` | do not strip the binaries (Linux, macOS) |
| `--smoke`, `--smoke-headless` | run the packaged game once (headless needs no display) |

The samples ship the same way from a checkout: `python3 scripts/ship.py platformer` (Ember Step) or `diorama`.

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
instead of copying it (chapter 6, section 6.14).

## 15.5 Phones and the stores

`ship.py --target android` builds an APK (`--aab` adds the Play bundle), `--target ios` a Simulator
`.app` zip or, with `--preset ios-arm64`, a signed `.ipa`. Projects ship to phones the same way:

    python3 scripts/ship.py --project projects/MyGame --target android
    python3 scripts/ship.py --project projects/MyGame --target ios

The game is linked into the app (phones load no game modules), and the pack is cooked on your computer:
`ship.py` first builds the project's `<game>_cook` on the host preset (`--host-preset`), and the phone build
cooks with it (`MCE_HOST_BUILD_DIR`). What the stores need from the game:

| File | Used for |
|---|---|
| `ship/icon.png` | the app icon: square PNG, 1024x1024. iOS gets an `AppIcon` asset catalog (transparency flattened onto the image's edge colour), Android an adaptive launcher icon (the image at 72 dp on that colour) and `<stem>-play-icon.png` (512x512) for the Play listing. Without it the game ships with a warning and no icon, and both stores refuse it |
| `ship/PrivacyInfo.xcprivacy` | optional: what the game adds to the engine's iOS privacy manifest (`cmake/mobile/ios/PrivacyInfo.xcprivacy`: file timestamps, boot time and user defaults inside the app, no tracking, no collected data). An SDK for ads, analytics or purchases brings its own data types and reasons; list them here and they are merged in |

**Build numbers.** Both stores want a whole number that grows with every upload: `CFBundleVersion`
on iOS, `versionCode` on Android. `ship.py` uses `--build-number`, by default the commit count of
`HEAD`. The version players see is `--version` without its `+commit` part.

**Uploading.** `--store` uploads after packaging: an `.ipa` to App Store Connect (it appears in
TestFlight after Apple processes it), an `.aab` to the Play internal testing track. `--store --dry-run`
uploads nothing; it checks the package (icon, privacy manifest, integer build number, signature,
Play's upload key instead of the debug key) and, with the credentials set, asks App Store Connect to
validate the `.ipa`.

    python3 scripts/ship.py platformer --target android --store --dry-run
    python3 scripts/ship.py platformer --target ios --preset ios-arm64 --store

Credentials come from environment variables only; keep the files outside the project and never
commit them. `ship.py` does not print them.

| Variable | What |
|---|---|
| `MCE_ASC_KEY_ID`, `MCE_ASC_ISSUER_ID`, `MCE_ASC_KEY_PATH` | an App Store Connect API key (Users and Access → Integrations, App Manager role) and the path of its `AuthKey_<id>.p8` |
| `MCE_IOS_SIGN_IDENTITY`, `MCE_IOS_PROVISIONING_PROFILE` | an Apple Distribution certificate and an App Store profile for the bundle id |
| `MCE_PLAY_SERVICE_ACCOUNT` | the JSON key of a Google Cloud service account invited in the Play Console with release rights for the app |
| `MCE_PLAY_TRACK`, `MCE_PLAY_RELEASE_STATUS` | default `internal` and `completed`; use `draft` until the app's first release is published in the Console |
| `MCE_ANDROID_KEYSTORE`, `MCE_ANDROID_STORE_PASSWORD`, `MCE_ANDROID_KEY_ALIAS`, `MCE_ANDROID_KEY_PASSWORD` | the upload key (Play App Signing keeps the app signing key) |

The first Play upload of a new app has to be made by hand in the Play Console (the API cannot create
the app); after that `--store` takes over. Android builds target the API level Google Play currently
requires (API 36, `ANDROID_API` in `cmake/mobile/android_app.py`).

## 15.6 The web

`ship.py --target web` builds the game for browsers (WebAudio, WebGPU, ADR-0039) and zips it for an
itch.io HTML upload. It needs the Emscripten SDK in the shell (`source <emsdk>/emsdk_env.sh`); like the
phone builds, the pack is cooked on your computer first (`--host-preset`).

    python3 scripts/ship.py platformer --target web --smoke
    python3 scripts/ship.py --project projects/MyGame --target web
    mce ship projects/MyGame --target web        # the same, from mce (the project must be under projects/)
    mce serve dist/Ember-Step-<version>-web.zip   # play it: http://127.0.0.1:8000/

The zip is flat, with `index.html` at its root:

| File | What |
|---|---|
| `index.html` | the page (`scripts/web/shell.html`): a loading bar while the pack downloads, the icon from `ship/icon.png`, "Click to play" (or "Tap to play" on a phone), which also starts the audio (browsers keep it silent until then), a full-screen button, and a message when the browser has no WebGPU. The canvas fills the page and follows the window and the phone's rotation |
| `<Name>.js`, `<Name>.wasm` | the game |
| `assets.mce`, `VERSION.txt` | fetched by the game when it starts |

Browsers do not run the page from a `file://` address; `mce serve` serves a zip, a folder or a project's
newest web zip with the right types (`application/wasm`) and headers (`--lan` lets a phone on the same
network open it). On itch.io: upload the zip as "HTML", tick "This file will be played in the browser",
and turn on "Mobile friendly" for touch games. `--smoke` opens the staged page in headless Chromium and
checks that it runs and draws (it needs Playwright). For a game on the stock player (a project),
`--smoke-ticks N` also runs N ticks drawn and N ticks headless in the browser and prints the state hash;
the two must match. A browser build hashes differently from a desktop build of the same game (another
compiler and maths library), so compare web hashes only with web hashes.

Before an upload, `scripts/ci/local_ci.sh web` runs what the engine checks for every web build
(`scripts/web/web_ci.py`): each sample in headless Chromium, the input and relay probes, Ember Step's
page at desktop and phone size, Kindle's web state hash against `tests/web/baselines.json`, the zip
against itch.io's HTML rules (`index.html` at the root, a flat zip, at most 1000 files and 500 MB
extracted, every file the page loads present), and a cold-load budget: what a browser downloads before
Ember Step's first frame, gzipped, must stay under `cold_load.budget_bytes`. After a deliberate change
to a hash or a size, `python3 scripts/web/web_ci.py --skip-build --update` writes the new measurement.

The game needs WebGPU: current Chrome, Edge and Safari, and Firefox 141 and later. Saves and settings
go to the browser's IndexedDB, so they stay with that browser on that device.

## 15.7 Steam

`--steam <app id>` ships the desktop build as usual and also lays out its Steam depot and the steamcmd
build scripts. Nothing is uploaded.

    python3 scripts/ship.py --project projects/MyGame --steam 1234560 --steam-library <sdk>/redistributable_bin/linux64/libsteam_api.so
    mce ship projects/MyGame --steam 1234560     # the same from mce (STEAMWORKS_SDK names the Steamworks SDK)

| Option | Meaning |
|---|---|
| `--steam <app id>` | the app id from the Steamworks page |
| `--steam-library <file>` | the `steam_api` to ship; by default the one in `$STEAMWORKS_SDK/redistributable_bin/<platform>/` |
| `--steam-depot <id>` | this platform's depot (default: app + 1 for Windows, + 2 Linux, + 3 macOS, as Steamworks numbers a new app's depots) |
| `--steam-branch <name>` | set the build live on this branch once it uploads (default: none, set it live on the Steamworks page) |

Output, in `dist/<Name>-steam/`:

| Path | What |
|---|---|
| `content/<os>/` | the shipped folder as players get it, with `steam_api` beside the executable (inside the `.app` on macOS) |
| `scripts/app_build_<app>.vdf` | the app build, naming each platform's depot |
| `scripts/depot_build_<depot>.vdf` | one per platform: its `content/<os>/`, debug files left out |
| `UPLOAD.txt` | the depots and the `steamcmd +login <account> +run_app_build ... +quit` line that uploads them |

Ship on each OS into the same `--out` folder (copy `dist/<Name>-steam/` between machines): each run fills
its own `content/<os>/` and the app build lists every platform present. No `steam_appid.txt` is shipped:
Steam starts the game with `SteamAppId` set, and the player signs in with it; run outside Steam, the game
plays without Steam (`-steam.app_id -1` keeps Steam off even under Steam). On the Steamworks page, put the
depots in a package and give each platform a launch option that runs its executable.

The MCP tool `ship_build` takes `steam_app_id` (and `steam_library`) and returns the app build script's
path as `steam_build_script`.

## 15.8 Checklist before shipping

1. Every scene cooks clean (`mce cook MyGame` exits 0; in a checkout `my_game_cook projects/MyGame --pack`).
2. `my_game -headless 1 -ticks 3600` runs a minute without an error in the log.
3. `CREDITS.md` names every third-party asset and licence (Kenney CC0 packs, fonts, sounds).
4. The packaged game starts and quits cleanly on the target platform (unzip the `mce ship` zip and run it with
   `-headless 1 -ticks 60`; in a checkout `python3 scripts/ship.py --project projects/MyGame --smoke`).
5. Keep the symbols zip with the version.
