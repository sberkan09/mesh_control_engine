# Changelog

Engine versions and what each one added. How and when the version moves: [docs/VERSIONING.md](docs/VERSIONING.md).
Every PR that changes what a user of the engine sees adds one line under **Unreleased**.

## Unreleased

- Scene changes at run time (SBA-56, from the M-AGENT demo's friction notes): a game system calls
  `scene::request_change(world, "scenes/level2.mcscene")` and `mce_player`, the editor's play mode and every
  MCP run swap every entity for the target scene after the tick, keeping resources and running Startup again.
  Scenes may now reference each other in a loop (a scene's `AssetRef<Scene>` is a link, not a cook dependency),
  and a `.mctest` checks `{"scene": "scenes/win.mcscene"}`. Also from the demo: `module_build` really loads the
  rebuilt module (each load maps a fresh copy); `asset_schema` and `docs_search` no longer break the MCP stream
  with raw newlines; `gen_pixels` draws exactly the sprite an agent describes and `gen_mesh` makes a box, plane, sphere, cylinder,
  cone, disc or ramp as a `.glb`; `docs_search` cuts snippets on character boundaries; `project_check` counts every
  `.mc*` document as a referrer, skips a drop shadow behind a readable copy of its text in the contrast check
  and says when the performance check could not run; `project_set` keeps
  `start_scene` when the patch does not name it; `engine_capabilities` lists the components that already do
  what it called code; an SDK project's `ctest` runs its 300-tick smoke test.
- From the M-AGENT demo's second round (SBA-56): `module_build` reloads a component whose fields changed (the
  host drops the old module's reflection even when the loader keeps its image mapped); `render_sheet` and
  `render_frame` play a long input script exactly as `run_headless` does (the player reads `-input` by sim tick);
  a free camera left of the origin renders (a negative number on the command line is a value); `walk_to` takes
  `through` waypoints and `air_jumps`, and runs half a second past the arrival; a scene change ends a bot's level
  instead of counting a death; `.mctest` event checks accept the name without its namespace and fail on an
  unknown one; entity references in values take the entity's name; `run_headless` `inspect` names the played
  scene's entities; `project_check` measures text contrast against the sprite panel behind it, flags
  `CREDITS.md` rows for files that are gone and treats input maps as entry points; `RigidBody3D.stop_spin`
  zeroes a body's spin; a music source of the next scene waits for the same streamed clip to fade out instead
  of staying silent; `MCE_LOG_*` with a printf format does not compile.
- Friction notes, M-AGENT step 8 (SBA-55): `report_friction` lets an agent write down what the engine made hard
  (summary, area, severity, expected, actual, workaround) as a line in the project's `friction.jsonl`, and
  `mce friction <projects...>` collects the notes into one Markdown backlog (or JSON), blocking ones first.
- Several agents on one project, M-AGENT step 7 (SBA-54): each agent runs its own MCP server (`--agent <name>`)
  on the same folder. `project_watch` reads the event feed (`.mce/events.jsonl`), `asset_lock` refuses other
  agents' writes to a file, `task_add` / `task_claim` / `task_update` share `tasks.mctasks`, and `scene_switch`
  keeps several scenes open. `scene_save` merges another agent's save of the same scene by entity and refuses
  with the conflicts (or settles them with `resolve`) instead of overwriting.
- Quality gates for agents, M-AGENT step 6 (SBA-53): `project_check` returns findings with a fix for cook
  errors, broken and unused references, missing credits, sRGB on data textures, each scene's sim time per tick
  against a budget, and 2D text size and contrast; `ship_build` runs the full build, a headless smoke run of the
  host and `mce ship`, and returns the zip's path. `mce ship` now puts the project's `CREDITS.md` in the zip.
- Projects on phones (M-MOBILE, SBA-30): `scripts/ship.py --project <dir> --target android|ios` builds a project
  made from the templates for phones. Its pack is cooked by the host build's `<game>_cook`
  (`MCE_HOST_BUILD_DIR`, which `ship.py` passes after building it on `--host-preset`); a cross build no longer
  defines a project cook it cannot run.
- Live services vendor, Firebase (M-LIVEOPS step 1): `mce/platform/firebase.h` with Crashlytics for native crashes
  and Analytics for `analytics::event`. `ship` stamps it into a phone build only when `MCE_FIREBASE_ANDROID_CONFIG` or
  `MCE_FIREBASE_IOS_CONFIG` names the project's config file (never committed), uploads symbols to Crashlytics, starts
  both SDKs with collection off and adds the privacy manifest entries; the player turns collection on with consent.
  iOS builds it with `-DMCE_FIREBASE=ON -DMCE_FIREBASE_SDK_DIR=...`. The engine's crash handler now hands each crash on
  to a handler installed before it (Crashlytics' NDK handler) after writing its own report. `-liveops.provider
  firebase` picks it explicitly. Not yet run on a device.
- Content without a human, M-AGENT step 5 (SBA-52): the MCP server generates palettes, pixel sprites, tile sets,
  noise textures and skies, sfxr sound effects and music loops (`gen_palette`, `gen_sprite`, `gen_tileset`,
  `gen_texture`, `gen_sfx`, `gen_music`), each with a preview image and a `save` that writes the asset, its sidecar
  and a `CREDITS.md` row; `library_search` and `library_add` serve a bundled CC0/OFL/MIT library (Kenney models
  and sounds, three fonts, generator recipes) installed at `share/mce/library`, with credits written on copy.
- Self-contained knowledge, M-AGENT step 4 (SBA-51): the MCP server serves the shipped user guide, engine and
  architecture docs, the worked samples and the example projects as `mce://docs/...` resources (index, folders,
  `mce://docs/components/<Type>` schemas), searches them with `docs_search`, offers the prompts `start_game`,
  `add_level` and `polish_pass`, and `engine_capabilities` lists how each feature is reached (data, asset, MCP,
  console or code). The SDK now installs the samples' sources (`share/mce/samples`); `mce_mcp --docs <dir>` picks
  another docs folder. New user-guide recipes: a level a bot can finish, screens that stay right, a component live.
- Playtesting without a person, M-AGENT step 3 (SBA-50): `test_run` runs `.mctest` scenarios (JSON under the
  project's `tests/`: a saved scene, scripted input or a built-in bot, `until`, `death`, and checks on component
  fields at the end or at a tick, entities alive, event counts, the log and feel metrics) and `playtest` lets a
  kit bot play: `walk_to` (heads for a point or entity, jumps or sidesteps when stuck), `explore` (seeded wandering)
  and `masher` (random chords), all through the game's input actions with no game code. Bot runs return a feel
  report: time to finish, deaths, stuck spots, idle time, distance and coverage. `run_headless` gains `trace`
  (chosen fields every N ticks) and `InputScript::clear_segments` lets a per-tick planner keep the edge rules.
- Code loop for agents, M-AGENT step 2 (SBA-49): `module_build` rebuilds the game module with the project's CMake
  tree and maps the new build into the running MCP server (new components and systems live on the next call, the
  open scene kept), returning compiler errors as file, line, column JSON; `module_test` runs the project's ctest
  tests and reports each result.
- Eyes for agents, M-AGENT step 1 (SBA-48): the MCP server gains `render_frame` (the saved scene drawn by the
  real player at sim tick N, with scripted input, from its own camera, another camera entity or a free 2D/3D camera,
  at a chosen size and safe area, returned as an MCP image), `render_sheet` (several ticks and cameras in one
  labelled contact sheet) and `image_diff` (score, changed fraction and a heat map against a golden PNG; a missing
  golden is written). Without a display the player draws on SDL's offscreen driver with Mesa's software GL, so a
  GPU-less cloud box works. The player gains `-capture_ticks`, `-view.entity`, `-view.free` and `-safe_area`, and a
  capture frame is drawn without interpolation so captures repeat exactly. The GL backend now clears the default
  framebuffer with `glClear` (Mesa ignored the DSA clear on an offscreen surface, so frames piled up).
- Compute on Metal, M-VISUAL step 1 (SBA-6): `mce_rhi_metal` and SDL GPU's Metal driver (iOS, `MCE_RHI=sdlgpu` on
  macOS) report `DeviceCaps::compute` and run compute pipelines, `dispatch`/`dispatch_indirect`, storage images and
  `read_buffer`, so `Forward3D` assigns Forward+ lights on the GPU on Macs and iPhones too (`-r.gpu_light_clusters 0`
  keeps the CPU path).
- Phones on the Mac (M-MOBILE, SBA-5): StoreKit's review prompt and UIKit haptics are now built into iOS by
  default (`MCE_STORE_REVIEW`, `MCE_UIKIT_HAPTICS` ON; the iOS presets set the Swift target), Android games build
  again with Play In-App Review (`android.useAndroidX=true`), and Ember Step taps the phone on coins, stomps, hurts
  and wins and asks for a review once per run after a won level.
- Battery and thermal probe (M-MOBILE step 8): `platform::power_info()` (`mce/platform/power.h`) reports the
  battery (SDL, every platform) and the thermal state (iOS and macOS); `-platform.power_log <seconds>` logs it
  during a run, which is how device soaks record drain and heat.
- Fixes found on Xcode 27 (SBA-5): Ember Step no longer crashes on quit when audio is playing (its clips are released
  before the app goes), 3D scenes with IBL off pass Metal validation again, and `mce_player -capture_tick N`
  holds the sim on tick N for the capture instead of running past it on slow frames.
- Live services, the demo (M-LIVEOPS): `samples/liveops_demo`, a headless level runner wired to game services,
  remote config and analytics against local stand-ins. Its ctest covers achievements and scores reaching the
  platform, a reinstall restoring progress from the cloud save, an offline outbox that drains on the next launch, a
  fetched value that changes the game after relaunch and survives airplane mode, no analytics written with consent
  denied, and a recorded run replayed to the same hash; its hash is checked across runs and Debug/Release in
  `local_ci.sh`. User guide 23.7 covers the demo and vendor setup (keys from the environment, test accounts).
- Taps and clicks on dialogue, step 11f (M-MOBILE): a tap or click on a choice row picks it on the novel stage
  and the dialogue box, and a tap elsewhere reads on, through the input snapshot (replays and headless runs see
  it) with a `Selection` haptic. The stock map gains `Point` (left mouse), input scripts take `Touch=x,y` and
  `Mouse=x,y`, and `kit::ViewInfo` carries the UI scale. The box's text size and height moved from
  `DialogueBoxStyle` to `novel::DialogueLayout` (`set_dialogue_layout`).
- Story translation, step 11e (M-MOBILE): with `"localize"` in `story/story.json`, `mce story compile` writes the
  story's lines, options and speaker names into `assets/loc/<base>.csv` and reads translations from the other
  tables; the novel stage, backlog and dialogue box show the player's language and switch with it, with the same
  run and state hash in every language. A translation that does not fit its line's pages falls back to the base.
- Haptics, step 11d (M-MOBILE, #150): `platform::haptics::play(preset)` (`mce/platform/haptics.h`) taps the phone
  (UIKit feedback generators on iOS with `-DMCE_UIKIT_HAPTICS=ON`, the vibrator's effects on Android) and rumbles
  every connected gamepad; the player's saved `haptics` convar turns it off, and it never enters the state hash.
- Review prompt, step 11c (M-MOBILE, #147): `platform::request_review()` (`mce/platform/review.h`) asks for the
  store's rating prompt: Play In-App Review on Android (stamped into every game), StoreKit's `AppStore.requestReview`
  on iOS (`-DMCE_STORE_REVIEW=ON`), a logged no-op on desktop.
- Runtime UI on phones, step 11b (M-MOBILE, #151): `mce_ui` anchors to the parent, the safe area or the whole
  screen; `ui::compute_ui_scale` and the player's `-ui_scale` size the UI from the display's DPI; scroll views
  (drag with inertia, wheel, clipping, focus into view), 9-slice images, modals, tabs and fit-width grids. The
  authoring values (`mce/ui/types.h`) are reflected, and the MCP server's `refl_describe_component` describes them.
- Languages, step 11a (M-MOBILE, #146): string tables in `assets/loc/<locale>.csv` cook to one `CHNK_STR` each,
  and the player installs a `loc::Localization` resource (`mce/loc/loc.h`) that picks the language from the
  player's saved choice, the device languages and `-default_language`, with plural rules, `{name}` placeholders
  and Turkish-aware upper and lower case. `-language tr` plays in Turkish for one run; the cook warns about keys
  a locale lacks. A font sidecar's `"strings": true` bakes every glyph the tables use (with Turkish case forms),
  and `ship.py` names the phone app per language from each table's `app.name`.
- Game services (M-LIVEOPS step 2, vendor-neutral part): a `.mcservices` file cooks to `CHNK_GSVC` with the
  project's achievements (one-shot or incremental), leaderboards and cloud saves. Game code uses the
  `liveops::GameServices` resource. Unlocks and scores count on the device at once and wait in a saved outbox until
  the platform takes them. Cloud saves are versioned, and a two-device conflict goes to the game (`CloudConflict`,
  `resolve_conflict`) or to a policy. Events enter at a tick start and replays record them.
  `-liveops.provider local` plays the platform with a folder, and `-game_services_test` rehearses it. Game Center
  and Play Games need the Mac and test accounts.
- Remote config (M-LIVEOPS step 3, vendor-neutral part): a `.mcconfig` file cooks to `CHNK_RCFG`, the project's
  defaults and schema. Game code reads `liveops::RemoteConfig` (`get<i64>`, `get<f64>`, `get<bool>`, strings) as a
  world resource. Fetched values are saved and used from the next launch, or at the start of a tick after
  `apply_fetched()`, never mid-tick; replays record every apply. `-remote_config.file <pack>` fetches from a cooked
  pack and `-remote_config.dump 1` prints the values in force. A server fetcher waits for the vendor choice.
- Live services, step 1 (M-LIVEOPS, vendor-neutral part): `mce_liveops`, off unless the host sets
  `PlayerDesc::liveops` (or `-liveops 1`). Crash reports that earlier runs left are sent on the next launch through an
  uploader, `analytics::event(name, params)` (from `mce/gameplay.h`) goes to a backend, and both wait for the
  player's consent (`liveops::set_consent`, the ad network's consent form, or `-liveops.consent`). `-liveops.provider
  local` writes `analytics.jsonl` and `crash_outbox/` in the save folder. New user guide chapter 23, *Live services*.
  The vendor SDK is not wired in yet.
- Purchases and ads, the demo (M-MONETIZE): `samples/shop_demo`, a headless runner with a shop against the fake
  store and ad network (gems and "remove ads" bought, a rewarded continue, interstitials between levels), whose ctest
  covers purchase, cancel, restore, a relaunch that keeps the entitlement and requests no ad, and a recorded run
  replayed to the same hash; its hash is checked across runs and Debug/Release in `local_ci.sh`. New user guide
  chapter 22, *Purchases and ads*: the catalog, game code, consent, the fakes, store declarations and where
  server-side receipt validation plugs in.
- Purchases and ads, step 5 (M-MONETIZE): a non-consumable marked `"removes_ads": true` in the `.mcstore` turns every
  ad off once owned: `Ads::load` / `show` / `show_banner` return 0 without reaching the network, `Ads::removed()`
  says so, a banner on screen is hidden, and a relaunch with it owned never starts the ad network (no consent form).
- Purchases and ads, step 4 (M-MONETIZE): Google Mobile Ads with Google's consent form (UMP) and, on iOS, the
  App Tracking Transparency prompt, behind `mce/platform/ads.h`; `monetize::PlatformAds` is the player's default ad
  network, with Google's test units in debug builds (`-monetize.ads.test`) and real units only from the `.mcstore`,
  whose new `"admob"` object holds the app ids. `Ads::privacy_options_required` / `show_privacy_options` reopen the
  consent form. Games with ad placements get the SDKs stamped into their Android build, and their iOS privacy
  manifest and Info.plist extended; the iOS SDK is linked with `-DMCE_ADMOB=ON`. Not yet run on a device.
- Purchases and ads, step 3 (M-MONETIZE): the phone's own store, Play Billing on Android and StoreKit 2 on iOS,
  behind `mce/platform/store.h`; `monetize::PlatformStore` is the player's default store. `ship.py --target
  android` stamps Play Billing in for games with a `.mcstore`; StoreKit 2 is built with `-DMCE_STOREKIT=ON`. Both
  still wait on a check on a real device or emulator.
- Purchases and ads, step 2 (M-MONETIZE): a fake store and ad network (`mce/monetize/fake.h`, `-monetize.provider
  fake`, outcomes set with `-monetize.fake.*`), `-monetize 1` and `-monetize_test` on `mce_player`, and a
  `player_monetize` test running every outcome in `local_ci.sh`.
- Purchases and ads, step 1 (M-MONETIZE): `mce/monetize/monetize.h` gives games a Store (products, purchase, restore,
  entitlements kept across relaunches) and Ads (rewarded, interstitial, banner) over provider interfaces, with the
  catalog in a cooked `.mcstore`; off unless the host sets `PlayerDesc::monetize`. Real stores and ad networks come
  in later steps; until then purchases fail and ads never fill.

## 0.9.0 (2026-10-13)

The first version bump since M0 (0.0.1, 2026-10-07). Everything the ROADMAP lists up to the v1.0 tier is built;
the v1.0 gate's human-only checks (non-programmer double-click test, 2-hour editor session) are still open, and
1.0.0 waits on them.

- MVP (M0–M4): ECS and determinism harness, asset cook and hot reload, the 2D stack, the 3D gate, shipping.
- v0.5 (M5a–M7): the 3D renderer, 3D physics, play-in-editor, Ember Step and Ashen Court.
- v1.0 (M8–M9): runtime UI, project template and generator, packaging v2, editor maturity, Kindle (UC-D).
- Off-tier: editor MCP server (M4.5), macOS/Metal, iOS/Android (SDL GPU backend), skinned animation,
  networking, visual-novel kit, 3D kit, scene streaming, data mods.
- Versioning: `scripts/bump_version.py` sets the version everywhere, `local_ci.sh`'s walls stage checks the
  copies agree, and this changelog starts here (docs/VERSIONING.md).
