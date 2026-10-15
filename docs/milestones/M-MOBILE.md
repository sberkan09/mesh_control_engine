# M-MOBILE — iOS/Android port (milestone record)

**Status:** active (un-parked 2026-10-11 by the user). **Decision record:** ADR-0038 (one SDL3 GPU backend for
both phones). **Requirements:** FR-SHP-062, FR-INP-070, FR-RUN-100 (REQUIREMENTS slip note 2026-10-11).
**Scope:** the player only. The editor, the cook and the MCP server stay desktop tools; a phone runs a shipped game.

## Plan

Steps run in order; each lands as its own PR verified by `scripts/ci/local_ci.sh` (cloud) and, from step 6 on, by
a device or simulator run on the user's Mac.

| # | Step | Where it is verified | State |
|---|---|---|---|
| 1 | ADR-0038, REQUIREMENTS slip note, this plan | read-through (docs only) | done |
| 2 | `mce_rhi_sdlgpu` spike on Linux: window surface seam (`platform::gpu_api()`), device, clear + triangle, binding map, copy-pass split | `local_ci.sh` + lavapipe | done |
| 3 | `mce_rhi_sdlgpu` parity with GL: textures/arrays/MSAA/resolve, samplers incl. compare, MDI, storage data, readback; GPU tests run on both backends (`test_render_sdlgpu`, `MCE_RHI=sdlgpu`) | `local_ci.sh` + lavapipe | done (gaps below) |
| 4 | Touch input (FR-INP-070): `Touch{Down,Move,Up}` platform events, touch state per tick in `mce_input`, touch bindings (virtual stick, buttons, tap/swipe), `mce_ui` hit-testing by touch, record/replay of touches | `local_ci.sh` (synthetic touch streams, determinism) | done |
| 5 | App lifecycle (FR-RUN-100): background/foreground events, sim and audio pause, autosave on background, GPU surface loss and recreation, orientation and safe-area insets exposed to `mce_ui` | `local_ci.sh` (synthetic lifecycle events) | done (surface loss: device, step 6) |
| 6 | Android: `android-arm64` presets (NDK toolchain), SDL's Java activity, Gradle wrapper project stamped by `scripts/ship.py --target android`, APK/AAB with the cooked pack in assets | emulator + a device, on the Mac | done on the emulator (no Android phone attached yet) |
| 7 | iOS: `ios-arm64` / `ios-sim-arm64` presets, Xcode project generation, the statically linked game module (`MCE_PROJECT_STATIC_MODULE`), signing, IPA from `scripts/ship.py --target ios` | simulator + a device, on the Mac | done: Simulator, and a signed `.ipa` on an iPhone 11 (2026-10-14; Ninja app bundles instead of an Xcode project) |
| 8 | Mobile budgets: quality tiers (shadow cascades, MSAA, post FX, render scale), ASTC texture profile in the cook, frame/thermal/battery numbers per tier | devices, on the Mac | done; iPhone 11 numbers below (battery drain and an Android phone still open) |
| 9 | Demo: Ember Step (2D) and Solstice Reach or `04_diorama` (3D) playable by touch on one Android and one iOS device; 10-minute soak each | devices, on the Mac | done on the emulator, the Simulator and an iPhone 11 (Android phone open) |
| 10 | Store readiness (added 2026-10-12 from issues #137–#140): per-game app icon from `<project>/ship/icon.png` (iOS `AppIcon` asset catalog via `actool`, Android mipmaps + adaptive icon, shared with the Windows icon input) #137 · `PrivacyInfo.xcprivacy` from an audit of the required-reason APIs the engine and SDL3 touch, with a per-project append path #138 · Android `compileSdk`/`targetSdk` as one variable at Play's current requirement, insets re-checked, a yearly check noted here #139 · a store step after `ship.py`: monotonic build numbers, TestFlight upload with an App Store Connect API key, Play internal track via the Developer API, secrets from the environment, `--dry-run` validation #140 | simulator + emulator (+ a store dry run), on the Mac | done (store upload untested: no credentials) |
| 11 | Languages, menus and phone feel (added 2026-10-12 from issues #146, #147, #150, #151): per-locale string tables (`<project>/loc/<locale>.csv` cooked into one `CHNK_STR` each), device-language selection with a fallback chain and a runtime switch persisted in the save, font atlases from the glyphs the tables use (Turkish ğ ı İ ş), locale-aware case mapping, `{count}` placeholders and plural rules, a cook warning for missing keys, localized app names stamped by `ship.py` (reopens N12; its trigger, a commercial release, is met) #146 · `mce_ui` layout for many aspect ratios: anchors and pivots against the parent or safe-area rect, stack and grid containers, 9-slice sprites, a scroll view with touch drag and inertia, toggle / slider / modal / tabs, a UI scale from DPI, all reflected for the editor and MCP #151 · `request_review()` over SKStoreReviewController and Play In-App Review, a logged no-op on desktop #147 · `haptics::play(preset)` over UIKit feedback generators, Android `VibrationEffect` and SDL gamepad rumble, a global setting, never in the determinism hash #150 | `local_ci.sh` (layout screenshots at 1280×720, 2400×1080, 2048×1536; EN ↔ TR switch; fakes) + simulator and emulator, on the Mac | 11a–11f done in the cloud; 11c and 11d checked in the Simulator and the emulator (2026-10-14); 11a, 11b, 11e, 11f wait for a player game on phones (below) |

## What shipped (steps 2–3)

| Item | Where | Evidence |
|---|---|---|
| Backend | `src/engine/rhi_sdlgpu` (`sdlgpu_device.cpp`, `sdlgpu_translate.cpp`) | every `RenderDevice` call; per-slot geometry/indirect rings, transfer buffer and storage pool, slot fences from the `end_frame` command buffer |
| Shader path | `rhi_sdlgpu/src/shader_spirv.cpp` | GLSL 450 → glslang (Vulkan 1.1, SPIR-V 1.3) with `gl_VertexID`/`gl_InstanceID` kept at GL meaning and a vertex epilogue (clip z, y flip); `DescriptorSet`/`Binding` rewritten into SDL's sets; `test_shader_spirv` checks the tables without a GPU |
| Surface seam | `platform::gpu_api()` (`window.h`) | `MCE_RHI=sdlgpu` gives a Vulkan window; `rhi::create_default_device` follows it, so every sample, the player and the editor run on it unchanged |
| Tests | `tests/rhi_sdlgpu`, `test_render_sdlgpu` | `test_rhi_sdlgpu` (the `test_rhi_gl` cases) and all 20 `test_render_gl` cases (2D batches, MDI, text, tilemaps, 3D forward+, cascaded shadows, MSAA resolve, tonemap, skinning) pass on lavapipe |
| Samples | `04_diorama`, `05_ashen_court` with `MCE_RHI=sdlgpu` | 60/30-frame captures differ from GL's in 0.09 % / 0.13 % of pixels (edge rasterization), screenshots in the project files (`m-mobile/`) |

**Gaps (reported, not faked):** no GPU timestamps (`caps().timestamp_queries == false`; the profiler's GPU column reads −1); `bind_storage_buffer` ignores a non-zero offset (no engine code uses one); `validation_error_count()` stays 0 because SDL exposes no validation callback; BC textures are rejected where the GPU lacks them (phones; the cook writes RGBA8 or, for phones, ASTC since step 8); every frame ends with one full-screen blit from the offscreen backbuffer to the swapchain.

## What shipped (steps 4–5)

| Item | Where | Evidence |
|---|---|---|
| Touch events | `platform` (`TouchDown/Move/Up`, finger slots, `inject_touch`) | SDL's touch/mouse emulation is off, so a finger never moves the mouse; `test_platform` drives synthetic fingers |
| Touch bindings | `mce_input` (`TouchZone`, `TouchStick`, `TouchTap`, `TouchSwipe`), `.mcinput` keys, the default map | `test_input` "touch" cases: per-tick sampling, the one-tick floor, gestures from event timestamps, the same recording at 1 and 7 events per frame |
| Touches in replay | `InputState::touch_points()`, `hash_state`, the replay stream | a touch session replays to the same hash; touch-free streams keep their bytes and hashes |
| Touch UI | `ui::apply_touch_pointer`, `UpdateContext::safe_insets` | `test_ui`: a finger taps a widget; insets shrink the root |
| Lifecycle | `platform` (`AppBackground/Foreground/LowMemory/Terminating`, `SafeAreaChanged`, `OrientationChanged`, `simulate_app_event`), `AudioDevice::set_paused`, the player | `scene_demo_player_lifecycle`: 600 app ticks with 60 in the background hash equal to a plain 540-tick run, and the save is written in the background callback; `scene_demo_player_lifecycle_gpu` rehearses it windowed |

The player on a phone: the background pauses sim and audio, releases held input, writes the `-save` slot and stops
drawing; back in the foreground the pause overlay waits, a tap resumes (the tap never reaches the game) and a touch
in the top-right corner pauses. GPU surface loss is SDL's: its Vulkan backend recreates the surface and swapchain
after Android destroys them, and the backend skips a frame whose swapchain texture is missing. That path only runs
on a device, so it is checked in step 6.

## What shipped (step 6, Android)

| Item | Where | Evidence |
|---|---|---|
| Presets | `android-arm64-debug` / `-release` (`CMakePresets.json`), `cmake/MceMobile.cmake` | NDK r28 toolchain from `ANDROID_NDK_HOME`, API 29 (Android 10), `c++_static`; tests, editor, projects, net and Tracy off; `mce_rhi_sdlgpu` is the only backend (no GL, `MCE_RHI_SDLGPU_ONLY`) and game modules are linked in (`MCE_GAME_MODULES_SUPPORTED` off) |
| App library | `cmake/MceMobile.cmake`, `cmake/mobile/mce_sdl_main.h` | on Android an engine `add_executable` becomes `lib<target>.so` and `main()` becomes the exported `SDL_main` (force-included header: no SDL include outside `mce_platform`) |
| Cross cook | `MCE_HOST_COOK`, `samples/CMakeLists.txt` | the host preset's `mce_cook` cooks the packs; the phone build adds only the windowed samples (01_triangle, sprites_2d, 03_platformer, 04_diorama, 04_cubes, 05_ashen_court) |
| Bundled files | `platform::base_path()` (`platform.cpp`) | the pack ships in the APK's assets with `mce_bundle.txt`; the first launch (and the first after an update) extracts it to the app's files dir, so `resolve_shipped_path` and the memory-mapped mount work unchanged |
| Packaging | `scripts/ship.py --target android [--aab] [--smoke]`, `cmake/mobile/android_app.py`, `cmake/mobile/android/` | a Gradle project stamped per game (SDL's Java + wrapper from the same SDL checkout, `MceActivity` loading the game library, landscape, Vulkan 1.1 required); stripped library, symbols zip beside the APK; release signed with `MCE_ANDROID_KEYSTORE` or the debug keystore |
| Phone glue | `log.cpp`, `crash.cpp`, `window.cpp`, `platform.cpp` | log to logcat (`adb logcat -s MCE`); crash reports under the app's files dir and a backtrace over the unwinder; fullscreen (immersive) windows; `platform.orientations` (default landscape) |
| Local CI | `scripts/ci/local_ci_macos.sh android` | both APKs built and a 300-frame smoke run (activity finished, pack mounted from the bundle, no crash) on the Android 15 arm64 emulator |

Emulator results (Android 15, arm64, `-gpu host`, Vulkan through MoltenVK on Apple M4): Ember Step (7.0 MB APK)
and the 3D diorama (7.7 MB) both render correctly at 60 fps in landscape. Screenshots in the project files
(`android-ember-step.png`, `android-diorama.png`). With `-gpu swiftshader_indirect` the emulator's SwiftShader Vulkan never signals the frame's fence,
so the first present spins forever; that is the emulator, not the backend (SDL's own GPU clear loop runs fine on
`-gpu host`). Not yet done: a physical Android device (none is attached to the Mac).

## What shipped (step 7, iOS)

| Item | Where | Evidence |
|---|---|---|
| Presets | `ios-sim-arm64`, `ios-arm64` (`CMakePresets.json`) | iOS 16+, arm64, the same phone options as Android; `mce_rhi_sdlgpu` only, game code linked in (`MCE_GAME_MODULES_SUPPORTED` off: iOS loads no code, so samples and project hosts take the static path, `MCE_PROJECT_STATIC_MODULE`) |
| App bundles | `cmake/MceMobile.cmake`, `cmake/mobile/ios/` | every engine executable is `<target>.app` (`Info.plist.in`: landscape, fullscreen, Metal required); `ios_main.c` runs SDL's UIKit entry point, calls the game's renamed `main()` and exits with its code when it returns |
| Shaders | `rhi_sdlgpu/src/shader_msl.{h,cpp}` | the SPIR-V from `compile_glsl` goes through SPIRV-Cross to MSL with every resource pinned to the Metal index SDL binds (samplers → texture/sampler i, uniform bank b → buffer b, storage j → buffer uniform_count + j); same conventions as Vulkan, so the vertex epilogue is shared |
| Packaging | `scripts/ship.py --target ios [--preset ios-arm64] [--smoke]`, `cmake/mobile/ios_app.py` | the pack and CREDITS in the bundle root (`SDL_GetBasePath` is the bundle), display name, dSYM archived and the binary stripped; Simulator: ad-hoc signed, zipped `.app`; device: signed with `MCE_IOS_SIGN_IDENTITY` + `MCE_IOS_PROVISIONING_PROFILE` (embedded, its entitlements signed in), `.ipa` |
| Simulator | `cmake/mobile/ios/patch_sdl_simulator.cmake` (FetchContent `PATCH_COMMAND` on SDL) | the Simulator's virtual Metal device does not claim `MTLGPUFamilyApple3`, so SDL refuses it; the patch accepts it and skips the depth-clip call, both under `TARGET_OS_SIMULATOR` only. The smoke run turns the Simulator's Metal validation layer off (it rejects indirect draws the host GPU runs) |
| Audio quit | `platform/src/audio_output.cpp`, `platform.cpp` | found here: a device that never finishes opening (the Simulator's audio server timed out for minutes on this Mac) made the game hang on quit in `~AudioOutput`; the owner now waits 1 s, then leaves the opener to free it, and `platform::shutdown` skips `SDL_Quit` while one is pending |
| Local CI | `scripts/ci/local_ci_macos.sh ios` | both samples built, installed and played 300 frames in an iPhone 17 Pro Simulator (iOS 26), exit 0, pack mounted from the bundle |

Simulator results: Ember Step (2.7 MB zipped app) and the 3D diorama render correctly (screenshots in the project
files, `ios-sim-diorama-*.png`). Not done: a physical iPhone (none attached, and a device build needs Sami's signing identity
and a provisioning profile), audio on the Simulator (its audio server never answered on this Mac; macOS audio is
fine).

## What shipped (step 8, mobile budgets)

| Item | Where | Evidence |
|---|---|---|
| Quality tiers | `render/quality.h` (`QualityTier`, `quality_settings`, `apply_quality`), `r.quality` | Low / Medium / High set MSAA (1/2/4x), shadow map size (1024/1024/2048), cascades (1/2/3), bloom, FXAA and render scale (0.67/0.85/1.0); `auto` is Medium on Android/iOS and High elsewhere, so desktop output is unchanged. `04_diorama` and `05_ashen_court` take it (`r.msaa 0` = the tier's) |
| Render scale | `Forward3DDesc::render_scale`, `forward3d.cpp` | the 3D targets (HDR, depth, bloom chain) shrink; the tonemap/FXAA pass samples bilinearly into the full-size target, so 2D and UI drawn afterwards stay sharp |
| MSAA fallback | `rhi_sdlgpu` `create_texture` | a sample count the GPU lacks (the iOS Simulator has no 2x) falls back to the nearest it has, with a warning; pipelines follow the target |
| ASTC textures | `mce_cook --textures astc`, `texture_importer.cpp` (astc-encoder 5.6.0, no-SIMD build with FP invariance), `asset::TextureFormat::ASTC_4x4`, `rhi` `ASTC_4x4_UNorm/SRGB` | phone builds cook ASTC 4x4 by default (`MCE_COOK_TEXTURES`); the texture storage is part of the cook DB key, so switching re-cooks; `test_cook` checks block sizes, the re-cook and byte-identical re-cooks. The diorama pack drops from 1.53 MB to 0.50 MB and renders the same in the Simulator and the emulator |

**Numbers per tier on an iPhone 11** (A13, iOS 27.0.1, 2026-10-14, SBA-5; `04_diorama` with its slow orbit,
5 minutes per tier, vsync on, `-platform.power_log 30`):

| Tier | Frame rate | Thermal state (`NSProcessInfo`) |
|---|---|---|
| Low (MSAA 1x, 1024 shadows, 1 cascade, scale 0.67) | 60.0 fps (18000 frames / 300.0 s) | nominal → fair after 1 min → serious after 3 min |
| Medium (MSAA 2x, scale 0.85) | 60.0 fps (300.0 s) | serious throughout (the phone was still hot) |
| High (MSAA 4x, 2048 shadows, 3 cascades, scale 1.0) | 60.0 fps (300.2 s) | serious throughout |

Every tier holds 60 fps with no dropped frames, so `auto` = Medium leaves headroom on a 2019 phone. The thermal
column is dominated by charging: the phone was on the Mac's USB cable at 95% the whole time (the battery
stayed at 95%, so no drain figure), and it reached "serious" on the Low tier already. A drain and
cool-phone thermal run needs the phone unplugged (Wi-Fi debugging) and stays open, as does an Android phone.
`platform::power_info()` (`mce/platform/power.h`, new here) reads the battery through SDL and the thermal
state on iOS and macOS; `-platform.power_log <s>` logs both during a run.

Before the device (2026-10-12): On the emulator (MoltenVK on Apple M4) and the Simulator every tier
holds 60 fps because both present with vsync and the host GPU has headroom; `mce_rhi_sdlgpu` has no GPU timestamps
(step 3 gap), and thermal and battery figures need a phone.

## What shipped (step 9, demo)

| Item | Where | Evidence |
|---|---|---|
| Ember Step by touch | `samples/03_platformer/main.cpp` | the input map gains a floating stick (left half, `TouchStick`), JUMP and DASH buttons bottom right (`TouchZone`, held), a pause tap in the top-right corner, a tap on the right half to confirm in menus and the top-left corner to go back; once a finger touches the screen the HUD draws the buttons, the stick's base and knob, and touch help. Played with `adb shell input` on the emulator: the stick runs, the buttons jump |
| Diorama by touch | `samples/04_diorama/main.cpp` | one finger orbits, two fingers pinch to zoom (window-normalized `Touch*` events) |
| Soak, Android | Android 15 emulator (`-gpu host`) | Ember Step (`-autoplay 1`) and the diorama, 10 minutes each: same process throughout, PSS flat after the first minute (Ember Step 61.8 → 62.7 MB, diorama 68.4 → 68.4 MB), no fatal log line |
| Soak, iPhone | iPhone 11 (iOS 27.0.1), signed `.ipa` (2026-10-14) | Ember Step (`-autoplay 1`, won the first level, review asked once) and the diorama, 10 minutes each (36000 frames / 600.0 s, 60.0 fps both), exit 0, no crash report; on the USB cable, thermal "serious" throughout |
| Soak, iOS | iPhone 17 Pro Simulator (iOS 26) | Ember Step (`-autoplay 1`) and the diorama, 10 minutes each: same process throughout, no new crash report, resident memory falling after the first minutes (Ember Step 137 → 105 MB, diorama 196 → 161 MB) |

Both games are playable by touch on the emulator and the Simulator. **Not done: the physical devices** the plan
names (one Android phone, one iPhone): none is attached to this Mac, and an iPhone build also needs Sami's signing
identity and provisioning profile (`MCE_IOS_SIGN_IDENTITY`, `MCE_IOS_PROVISIONING_PROFILE`). Running
`scripts/ci/local_ci_macos.sh android ios` with a phone attached (Android) or the `.ipa` installed (iOS) is the
remaining check; Solstice Reach (a project, not a sample) ships to phones after `ship.py --target` learns
`--project`.

## What shipped (step 10, store readiness)

| Item | Where | Evidence |
|---|---|---|
| App icon (#137) | `cmake/mobile/app_icon.py`, `samples/*/ship/icon.png` | one 1024x1024 `ship/icon.png` per game. iOS: an `AppIcon` asset catalog compiled by `actool` into `Assets.car` (alpha flattened onto the edge colour), `CFBundleIcons` merged into `Info.plist`; `xcrun assetutil --info` lists the AppIcon set and both samples show their icon on the Simulator home screen. Android: SDL's default `ic_launcher` replaced by an adaptive icon (foreground = the image at 72 dp in each mipmap density, background = its edge colour) and a 512x512 Play listing icon beside the APK; Ember Step's icon shows in the emulator's launcher. A game without the file ships with a warning. The Windows `.ico` input stays separate for now |
| Privacy manifest (#138) | `cmake/mobile/ios/PrivacyInfo.xcprivacy`, `ios_app.py` | in every bundle root; a game's `ship/PrivacyInfo.xcprivacy` is merged in (tracking OR-ed, data types and domains appended, API reasons unioned per category) |
| Target API (#139) | `ANDROID_API = 36` in `cmake/mobile/android_app.py`, AGP 8.10.1 in `cmake/mobile/android/build.gradle` | Play requires API 36 for new apps and updates since 2026-08-31; both samples build at 36 (SDK platform `android-36`, build-tools 36.0.0, SDL's Gradle 8.12 wrapper) and Ember Step renders landscape, full screen, on the Android 15 emulator. Edge-to-edge was already enforced at 35, so the inset path did not change |
| Store step (#140) | `cmake/mobile/store.py`, `ship.py --store [--dry-run] [--build-number N]` | integer build numbers (`CFBundleVersion` and `versionCode`, default the commit count of `HEAD`; the iOS build number used to be `0.0.1.<commit>`, which App Store Connect refuses); `.ipa` → TestFlight with `xcrun altool` and an App Store Connect API key, `.aab` → the Play internal track through the Developer API (service-account JWT signed with `openssl`, no Google libraries); secrets from the environment only. `--dry-run` passes on both samples' Simulator bundles and on an AAB signed with a test upload key, and refuses a debug-signed AAB. No real upload yet: there are no store credentials |

**Privacy audit (#138).** Required-reason APIs reached on iOS: *file timestamp* (`stat`/`fstat` in SDL3's
`SDL_GetPathInfo` and `SDL_iostream`, `stat` in `mce_core`'s crash directory check, `std::filesystem` in `mce_ecs`
saves; all inside the app container, `C617.1`), *system boot time* (`mach_absolute_time` / `CLOCK_UPTIME_RAW` behind
`SDL_GetTicksNS` and `std::chrono::steady_clock`, elapsed time only, `35F9.1`) and *user defaults* (SDL3's UIKit event
loop reads the app's own `NSUserDefaults`, `CA92.1`). Not reached: disk space, active keyboards. No tracking and no
collected data. Re-check when SDL is updated or an SDK is added.

**Yearly check (#139).** Google Play raises the minimum target API every August. Each July: read
developer.android.com/google/play/requirements/target-sdk, bump `ANDROID_API` (and the Android Gradle plugin if the
new level needs it), install the platform with `sdkmanager`, run `scripts/ci/local_ci_macos.sh android` and look at
a game in landscape on the emulator.

## What shipped (step 11a, first part: string tables and the language choice)

- **String tables.** `assets/loc/<locale>.csv` (`key,text` header, RFC 4180 quoting, UTF-8 with or without a BOM,
  `key#one` / `key#other` plural forms, `#` comment rows) cooks with the `strings` importer to one `CHNK_STR` per
  locale (`mce/loc/string_table.h`). The file stem is the locale; a sidecar `"locale"` overrides it. Tables live
  under `assets/` rather than `<project>/loc/` so they get GUIDs, the cook DB and packs like every other asset.
- **Missing keys.** The cook compares every table with the base table (`en`, else the first by path) and warns,
  per file, about keys a locale lacks (they show the base text in play) and keys the base does not know.
- **Language choice.** The player loads every table into one `loc::Localization` world resource
  (`mce/loc/loc.h`, also through `mce/gameplay.h`): the player's saved choice (the `language` save, written the
  moment the game calls `set_locale`), then `-language` for one run, then the device languages
  (`platform::preferred_locales`, SDL), then `-default_language` (default `en`). Each step tries the exact tag,
  the bare language and any region of it; lookups walk the chain, so an untranslated key falls back to the base
  text and an unknown key shows itself.
- **Text helpers.** CLDR integer plural rules for the common language families, `{name}` placeholders with
  `{count}` filled by `plural()`, and upper/lower case that maps `i`/`İ` and `ı`/`I` the Turkish and Azeri way.
- Tests: `test_loc` (chunk round trip and damage, chain, EN ↔ TR switch, plurals, case mapping, the choice across
  a relaunch) and `test_cook` (`strings: …`, CSV cooking, errors with lines, the missing-key warning).

## What shipped (step 11a, second part: glyphs and app names)

- **Font glyphs from the tables.** A font sidecar's `"strings": true` bakes every codepoint the project's string
  tables use, plus their upper- and lower-case forms in each table's locale (so Turkish `İ` and `ı` are there for
  `upper()` / `lower()`). The tables become the font's dependencies, so editing a translation re-cooks the font;
  characters the font has no glyph for are a cook warning naming them (`U+011F`), not a silent gap in play.
- **Localized app names.** `scripts/ship.py` reads the `app.name` key of each `assets/loc/<locale>.csv`
  (`cmake/mobile/app_names.py`) and stamps it as the name under the icon: Android
  `res/values-<qualifier>/strings.xml` (`pt-BR` → `values-pt-rBR`), iOS `<locale>.lproj/InfoPlist.strings` plus
  `CFBundleLocalizations`. Locales without the key keep the artifact title.
- Tests: `test_cook` (`strings: a font with "strings": true …`, glyphs, warning, re-cook on a table edit) and
  `app_names` (the Python helper).

Still open in 11a: the simulator/emulator check with a Turkish device language. The language choice, the
string tables and the app names live in the player (`mce_player_runtime`) and in projects. Since SBA-30,
`ship.py --project <dir> --target android|ios` builds a project for phones (its pack cooked by the host
build's `<game>_cook`, `MCE_HOST_BUILD_DIR`); the phone check itself waits for a Mac run (see "Phone checks
on the Mac" below). The Simulator already runs in Turkish, and
StoreKit's system prompt follows it (11c).

## What shipped (step 11b: `mce_ui` on many screens)

- **Anchors to the parent, the safe area or the screen.** `Anchors::to` (`AnchorTo::Parent` by default; the root
  is the safe area) lets a backdrop bleed under the notch (`Screen`) or a bar inside a card hug the safe area
  (`SafeArea`). `Builder::to(...)`; `fill()` and `at()` keep it.
- **UI scale from the display.** `ui::compute_ui_scale(viewport, display_scale, user)` is the display's DPI scale
  times the user's `-ui_scale` (a saved convar of the player), lowered when the screen is smaller than 360 × 640
  at that scale, in 1/8 steps. The player's console and game UI layers use it.
- **Containers.** A grid with `columns = 0` fits as many cells as its width holds (a level grid on 16:9, 20:9 and
  4:3); a vertical stack hands its width down so such a grid sizes its height by it.
- **Scroll views.** `Builder::scroll` (a stack that may overflow): a finger or mouse drag past 8 px scrolls and lets
  go of the button under it, a release coasts and slows down (friction e^-4t), the wheel scrolls by 48 px a
  notch, gamepad focus scrolls the focused widget into view. Content is cut to the view: `SpriteBatch::add_rect`
  and `TextPlacement::clip` trim quads and glyphs with their uvs, so it stays in the same MDI groups.
- **9-slice images.** `Style::slice` (insets in texels of the region) draws nine quads: corners at their size
  times the UI scale, edges and middle stretched, shrunk together on a rect thinner than two corners.
- **Modals and tabs.** `Builder::modal` + `Document::open_modal` / `close_modal`: the modal draws two layers up,
  owns focus, navigation and the pointer, Cancel names it, closing gives focus back. `Builder::tabs` / `tab`:
  a bar of buttons and one visible page; confirming a tab sends `ValueChanged` on the tabs with the page index.
  Toggles and sliders were already there.
- **Reflected for the editor and MCP.** The authoring values moved to `mce/ui/types.h` and are reflected as
  `UiAnchors`, `UiStyle`, `UiSlice`, `UiStackLayout`, `UiGridLayout` and their enums; the editor and `mce_mcp`
  register them, and `refl_describe_component` describes them.
- Tests: `test_ui_layout` (scale, anchor spaces, fit grid, scroll drag / inertia / wheel / clip / focus,
  9-slice, modal, tabs, reflection), `test_mcp` (`UiAnchors`), and `test_ui_gl`'s layout screenshots: one
  layout at 1280×720, 2400×1080 (notch and home indicator) and 2048×1536, checked by rect and pixel and written
  to `build/<preset>/tests/ui/tmp/ui_layout_<w>x<h>.ppm`.

Still open in 11b: the same layout on the Mac simulator and Android emulator; it waits for a player game on
phones, as 11a does.

## What shipped (step 11c: the review prompt)

- **`platform::request_review()`** (`mce/platform/review.h`) asks the OS for its "rate this app" prompt. The OS
  decides whether it shows (Apple: three times a year; Play: its own quota) and says nothing back, so a game
  calls it at a good moment (after a won level, never from a button or on the first launch) and moves on.
  `review_backend()` names the side in use; `review_requests()` counts the calls.
- **Android:** `cmake/mobile/android/review/MceReview.java` runs Play In-App Review
  (`com.google.android.play:review:2.0.2`): `requestReviewFlow`, then `launchReviewFlow` on the UI thread.
  `android_app.py` stamps it, the library and `MceActivity.mceReviewRequest` into every game's Gradle project;
  `review.cpp` calls that method over JNI and is `None` when the activity lacks it.
- **iOS:** `review_storekit.swift` calls `AppStore.requestReview(in:)` (SKStoreReviewController's successor;
  iOS 16 is the minimum) on the foreground window scene. Opt-in with `-DMCE_STORE_REVIEW=ON` until it has been
  run on a Mac, like StoreKit purchases; without it the iOS backend is `none`.
- **Desktop:** `None`; each call logs `review: request_review() #n is a no-op here` and returns false.
- The store wall (`check_walls.py`) now also keeps Play In-App Review and `AppStore.requestReview` inside
  `mce_platform`.
- Tests: `test_platform` (the desktop no-op: backend, count, log line) and `android_bridges` (stamps a project
  over a fake SDL android-project and checks the dependency, `MceReview.java` and the JNI method's name).

Checked on the Mac (2026-10-14, SBA-5), with Ember Step asking once per run after a won level:

- **iOS Simulator** (iPhone 17, iOS 26.3, Turkish device language): the first real build of the Swift side
  failed twice and was fixed. CMake's Swift check targeted macOS (the iOS presets now set
  `CMAKE_Swift_COMPILER_TARGET`), and `mce_apply_compile_policy` handed `-Wall`, `-fno-rtti` and friends to
  `swiftc` (now C-family sources only). After that StoreKit's prompt shows over the game after the first level
  ("Ember Step (Artifact A) hoşunuza gidiyor mu?", five stars, "Şimdi Değil"; screenshot
  `ios-sim-review-prompt.png` in the project files). `MCE_STORE_REVIEW` is now ON by default; the `ios-arm64`
  device preset builds with it too.
- **Android emulator** (Android 15 with the Play Store, not signed in): every game failed to build since 11c,
  because Play In-App Review pulls AndroidX and the stamped `gradle.properties` said `android.useAndroidX=false`;
  it is `true` now. The call reaches Play Core (`ReviewService : requestInAppReview`), which cannot bind to the
  Play Store's review service for a sideloaded app; `MceReview` logs `requestReviewFlow failed` and the game
  plays on. Seeing the prompt itself needs an install from the internal test track.

## What shipped (step 11d: haptics)

- **`platform::haptics::play(preset)`** (`mce/platform/haptics.h`): seven presets (Selection, Light, Medium,
  Heavy, Success, Warning, Error) played on the device's own haptics and on every connected gamepad in one call.
- **iOS:** `haptics_uikit.swift` maps them to UIKit's feedback generators (selection, impact light / medium /
  heavy, notification success / warning / error), kept prepared. Opt-in with `-DMCE_UIKIT_HAPTICS=ON` until it
  has been run on a Mac; without it the device side is `none` and pads still rumble.
- **Android:** `cmake/mobile/android/haptics/MceHaptics.java`, stamped into every game with
  `MceActivity.mceHaptic`, plays the vibrator's predefined effects (tick, click, heavy click, double click) and
  two short waveforms for Warning and Error; the manifest already asked for `VIBRATE`.
- **Gamepads:** each preset has a rumble (`gamepad_rumble_of`: motor intensities and 25–220 ms) sent through
  SDL to every connected pad, on every platform. Virtual pads now take rumble too
  (`platform::virtual_gamepad_rumble`), which is how tests see it.
- **The setting:** `haptics::set_enabled`; the player keeps it in the saved convar `haptics` (on by default) and
  applies it every frame, so a settings menu or the console can flip it. Haptics are output only: nothing
  touches World state, so they never enter the state hash, and a headless run plays none.
- Tests: `test_platform` (names and ranges, the count without a pad, a virtual pad's rumble per preset, the
  setting suppressing it) and `android_bridges` (MceHaptics stamped, the JNI name, the preset numbering shared
  by the header, Java and Swift).

Checked on the Mac (2026-10-14, SBA-5), with Ember Step tapping on coins (Light), stomps (Medium), hurts (Error)
and wins (Success):

- **Android emulator:** `dumpsys vibrator_manager` lists the game's effects with usage TOUCH: `TICK` per coin,
  `CLICK` per stomp, `DOUBLE_CLICK` on the won level, as the preset table says.
- **iOS Simulator:** the Swift side builds and runs (the same two fixes as 11c) and a full autoplay run calls it
  with no error; the Simulator has no Taptic Engine, so feeling the taps waits for an iPhone. `MCE_UIKIT_HAPTICS`
  is now ON by default.

## What shipped (step 11e: story lines in the string tables)

- **Keys:** with `"localize": {"base": "en"}` in `story/story.json`, `mce story compile` gives every statement
  and option a key (`story.<FNV-1a of the text>`) and every character `story.name.<id>`, and writes those rows
  into `assets/loc/en.csv` beside the game's own rows. `--check` fails while the table is stale, so the cook's
  missing-key warning covers the story too.
- **Translations:** every other table's story rows are wrapped like the base text and appended to the
  program's text table, with a per-language table (`Program::locales`: pages, options, names). A statement
  keeps its base page count, so the VM, saves and `state_hash` are identical in every language; one that
  cannot fill those pages or overflows them is a warning and falls back to the base language.
- **Runtime:** `novel::story_locale` follows `loc::Localization`'s chain (none when the base comes first);
  the novel stage, the backlog and the dialogue box show `localized_page` / `localized_option` /
  `localized_speaker`, and the typewriter is scaled from the base page's reveal (`scale_reveal`). A language
  switch shows at once, including a page kept up under a choice.
- Tests: `test_novel` (chain, page, option and name mapping; the same run and hash in en and tr),
  `test_novel_ui` (Turkish typing, a runtime switch and back, Turkish options) and `story_compile` (the base
  table beside game rows, `--check` on a stale table, the page-count fallback).

The kit's own words followed (SBA-59): `novel.*` keys for the title menu and the panels (with `.touch`
variants), and `story.title` / `story.extras` / `story.ending.<id>` rows from the compile; user guide 21
lists them.

Still open in 11e: letters (`@lettertext`) show in the base language.

## What shipped (step 11f: taps and clicks on dialogue and novel choices)

- **In the sim:** `novel::pointer_pressed` reads a tap from the input snapshot: the first touch that went
  down this tick, else the stock map's new `Point` action (left mouse) at the mouse. Replays and headless
  runs see it like any other input; a keyboard run never reads it, so its hash is unchanged.
- **Novel stage:** a tap on a choice row or a title menu row moves the cursor there and picks it (the
  rows' sprites, through `ViewInfo::screen_to_world`); a tap beside the rows does nothing; elsewhere a
  tap reads on, showing the rest of a page first. A tap's tick ignores the keys, so a touch the stock map
  also reads as Jump does not pick twice.
- **Dialogue box:** `novel::DialogueLayout` (text size, box height) moved from `DialogueBoxStyle` into the
  world (`set_dialogue_layout`), and `dialogue_rects` gives the box and its rows in device pixels on the
  safe area at `ViewInfo::ui_scale` (new; the player writes `ui::compute_ui_scale`). The sim hit-tests
  those rows and the box; the box draws by the same numbers.
- **Haptics:** a pick by pointer plays `Selection` (step 11d).
- **Scripts:** `InputScript` takes `Touch=x,y` (a finger) and `Mouse=x,y`; `novel_probe_plays_by_touch`
  plays the novel template to `@end` by taps alone (`tests/kit_novel/novel_taps.input`).
- Tests: `test_novel` (taps play to the ending of the tapped row, a miss picks nothing, a click, the
  keyboard hash with and without a view), `test_novel_ui` (the hit rectangles equal the drawn ones on a
  2400×1080 screen with a notch at UI scale 2; a finger on a drawn row picks it), `test_input` (scripted
  Touch and Mouse).

Still open in 11f: taps in the iOS Simulator and an Android emulator; the novel kit reaches phones with a
player game, as 11a does.

## Phone checks on the Mac (2026-10-14, SBA-5)

| Check | Result |
|---|---|
| `local_ci_macos.sh android ios` on `dev_claude` | iOS passed; Android failed in Gradle since 11c (AndroidX, fixed above) |
| Review prompt, haptics (11c, 11d) | checked in the Simulator and on the emulator (above) |
| `ios-arm64` device build | every phone sample builds and links with the Swift bridges |
| Physical iPhone | iPhone 11 (iOS 27.0.1) over USB, Xcode 27.0. Profiles: Xcode's automatic signing made "iOS Team Provisioning Profile" for `org.meshcontrol.sample-03-platformer` and `-04-diorama` (Sami's personal team, valid 7 days), through a throwaway Xcode project built with `-allowProvisioningUpdates`; `ship.py --target ios --preset ios-arm64` then signs the `.ipa` with `MCE_IOS_SIGN_IDENTITY` / `MCE_IOS_PROVISIONING_PROFILE`, and `xcrun devicectl device install app` installs it. The first launch needs the developer certificate trusted on the phone (Settings → General → VPN & Device Management). Both games run on the A13 GPU through Metal; numbers in step 8, soaks in step 9. Arguments go after `--` with `devicectl device process launch` (otherwise `-r.quality` is read as a devicectl flag) |
| Physical Android phone | none attached (`adb devices` lists only the emulator) |
| 11a, 11b, 11e, 11f on phones | run 2026-10-15 (SBA-30) on the iOS Simulator (iPhone 17 Pro, iOS 26.3) and the Android emulator (API 35, device language tr-TR) with a project made from the novel template (en/tr tables, Crimson Pro) and Solstice Reach. **11e passed**: dialogue, speaker, choices and narration in Turkish, ğüşıöç İĞÜŞÖÇ drawn right. **11f passed**: taps on the title row, the box and a choice; the Selection haptic fires on Android (two `TICK`s in `dumpsys vibrator_manager`), and on iOS it is on the same path (`stage.cpp`) but not felt in the Simulator. **11a partly**: a Turkish device starts in Turkish and the home screen shows the localized names; choosing English on a Turkish device left the kit's own words Turkish, because the chain was [choice, device languages], fixed here (a chosen language falls back only to the base language). **11b not passed**: neither game has an `mce_ui` screen, so the safe-area and DPI path is still unchecked on a phone, and the novel stage draws in a 1280×720 area in the middle of a 2622×1206 / 2400×1080 screen instead of filling it (open). Also found: Metal validation in the Simulator aborts on the first indirect draw (SDL debug mode is now off in the Simulator), and `pbr_instanced.vert` failed MSL translation for iOS (`ios_support_base_vertex_instance` now on). The phone build of a project also needs the Mac's local `ship.py` fixes (MCE_ONLY_PROJECT, novel `text.font`, phone save path) that are not on `dev_claude` yet. The iPhone run waits for SBA-58 |
| Store upload (step 10) | not run: a real TestFlight or Play internal-track upload is irreversible and waits for Sami's go-ahead and credentials |

Found and fixed on the way (Xcode 27 arrived mid-session):

- **Ember Step crashed on quit in the Simulator** (`Mixer::mix_voice`, SIGSEGV, about one run in two): the
  app's synthesized clips and music were freed while the audio device, which outlives the app, still mixed
  them. `on_shutdown` now releases every clip it registered and waits for the mixer (`finish_releases`).
  It shows now because the Simulator's audio opens on this Mac since Xcode 27.
- **Metal validation failed every 3D sample** after M-VISUAL 2's IBL: with IBL off, `u_env`, `u_brdf_lut`
  and `u_env_sh` were declared but unbound. `Forward3D` binds 1×1 stand-ins (a 2D array and a 2D) then.
- **`solstice_reach_walk_gpu` ran past `-capture_tick 200`** under validation (slow frames caught up 202
  ticks); the player now holds the sim on the capture tick until the capture frame.

Still needed from Sami: an Android phone with USB debugging; the iPhone unplugged on Wi-Fi debugging for a
battery-drain run; and, for the store step, the go-ahead plus the App Store Connect API key and the Play
service account in the environment (`cmake/mobile/store.py`). The development profiles expire on
2026-10-21; Xcode renews them the same way.

## Constraints found so far

- The cloud cannot reach `dl.google.com`, so the Android SDK/NDK cannot be installed there. Steps 6–9 run on the
  user's Mac (command-line SDK + NDK r28 + JDK 17 for Android, Xcode for iOS). Steps 2–5 need no phone.
- `local_ci.sh` needs Mesa lavapipe (`mesa-vulkan-drivers`) for the `sdlgpu` GPU tests; `setup_env.sh` installs it.
- SDL is built with `SDL_GPU` / `SDL_VULKAN` only when `MCE_RHI_SDLGPU=ON` (the Linux, Android and iOS default;
  off on macOS, which keeps `mce_rhi_metal`). The backend is chosen at run time, so GL stays what every host uses unless `MCE_RHI=sdlgpu` is set.

## Rules (inherited from M-MAC)

- GL stays the reference backend; new RHI features land on GL first and get an `sdlgpu` catch-up here.
- Determinism baselines are per platform (ADR-0007); phone hashes are never compared with desktop hashes.
- No Must FR rides this track; it gates no tier.
