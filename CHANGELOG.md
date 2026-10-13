# Changelog

Engine versions and what each one added. How and when the version moves: [docs/VERSIONING.md](docs/VERSIONING.md).
Every PR that changes what a user of the engine sees adds one line under **Unreleased**.

## Unreleased

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
