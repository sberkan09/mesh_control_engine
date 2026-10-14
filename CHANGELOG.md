# Changelog

Engine versions and what each one added. How and when the version moves: [docs/VERSIONING.md](docs/VERSIONING.md).
Every PR that changes what a user of the engine sees adds one line under **Unreleased**.

## Unreleased

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
