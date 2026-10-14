# 22. Purchases and ads (optional)

In-app purchases and ads, `mce_monetize` (`mce/monetize/monetize.h`, reachable from `mce/gameplay.h`),
are **opt-in** per project. The engine talks to the stores and the ad network; your game only decides
what it sells, where an ad shows and what a purchase or a reward gives. One API covers:

- **purchases** on the App Store (StoreKit 2) and Google Play (Play Billing): consumables (gems) and
  non-consumables (remove ads, a level pack), restore, entitlements that survive a relaunch offline;
- **ads** through Google Mobile Ads (AdMob): rewarded, interstitial and banner, with Google's consent
  form (UMP) and, on iOS, the App Tracking Transparency prompt;
- **fakes** of both that run on a desktop, in the editor and in tests, with no account behind them.

`samples/shop_demo` is the worked example: a headless runner with a shop that buys gems and "remove
ads", shows a rewarded continue and interstitials between levels, and replays to the same hash.

## 22.1 Turn it on

In the project's host (`games/<game>/host/main.cpp`):

```cpp
mce::player::PlayerDesc desc;
// ...
desc.monetize = true;  // the player installs the Store and Ads resources before your game module
```

The player then reads the pack's store catalog, installs the `monetize::Store` and `monetize::Ads`
resources, keeps the entitlements in the save folder (`entitlements.mcesave`) and pauses the sim and
the audio while a fullscreen ad covers the screen. Without it the resources do not exist:
`f.try_resource<monetize::Store>()` is null, so code written for both cases checks it.

To try a game with purchases on without changing its host, pass `-monetize 1` to the player.

## 22.2 The catalog

Put one `.mcstore` file in the project's assets (`assets/store.mcstore`). The cook turns it into a
binary chunk in the pack; the game never parses it at run time.

```json
{
  "format": "mce-store", "version": 1, "guid": "<a uuid>",
  "products": [
    { "id": "remove_ads", "type": "non_consumable", "removes_ads": true,
      "ios": "com.studio.game.remove_ads", "android": "remove_ads" },
    { "id": "gems_100", "type": "consumable",
      "ios": "com.studio.game.gems_100", "android": "gems_100" } ],
  "ads": [
    { "id": "continue",  "format": "rewarded" },
    { "id": "level_end", "format": "interstitial",
      "ios": "ca-app-pub-1234567890123456/1111111111", "android": "ca-app-pub-1234567890123456/2222222222" },
    { "id": "menu",      "format": "banner" } ],
  "admob": { "ios": "ca-app-pub-1234567890123456~3333333333", "android": "ca-app-pub-1234567890123456~4444444444" }
}
```

| Field | Meaning |
|---|---|
| `products[].id`, `ads[].id` | the name your game code uses; the store and ad ids stay in this file |
| `type` | `consumable` (bought again and again) or `non_consumable` (bought once, owned forever, restorable) |
| `ios`, `android` on a product | the product id in App Store Connect / the Play Console. Empty: not sold on that platform |
| `removes_ads` | a non-consumable that turns every ad off once owned (22.4) |
| `format` | `rewarded`, `interstitial` or `banner` |
| `ios`, `android` on an ad | the AdMob ad unit id. Empty: Google's test unit for that format |
| `admob` | the AdMob **app** ids (with `~`), written into the Android manifest and the iOS Info.plist when the game is packaged. Missing: Google's test app id, with a warning from `ship` |

The cook rejects a malformed id, a `removes_ads` on a consumable and a duplicate id, with the file and
the field in the error. With several catalogs in one pack the first by path is used; `-store_catalog
<path or guid>` picks another.

## 22.3 Selling

Game code runs in FixedUpdate systems like everything else. A store answers whenever it likes, on its
own thread; the answer waits and enters your sim at the start of a tick as a `StoreEvent`, so the
game stays deterministic and replays include purchases.

```cpp
void shop(Frame& f) {
    monetize::Store* store = f.try_resource<monetize::Store>();
    if (!store) return;
    if (pressed_buy_gems) store->purchase("gems_100");
    if (pressed_restore) store->restore_purchases();
    for (const monetize::StoreEvent& e : f.events().read<monetize::StoreEvent>()) {
        if (e.kind == monetize::StoreEventKind::Purchased && e.product == store->find("gems_100")) {
            add_gems(100);              // grant first ...
            store->finish(e.transaction);  // ... then let the store forget the transaction
        }
    }
    show_remove_ads_button(!store->owns("remove_ads"));
}
```

- `query_products()` asks the store for localized prices; `price(index)` returns "$0.99" or "₺34,99"
  once its `ProductInfo` event arrived. Show the store's text, never a price of your own.
- `owns("remove_ads")` is true from the tick its `Purchased` or `Restored` event enters, and after a
  relaunch with no network: the entitlement is saved locally.
- A **consumable** stays unfinished until you call `finish` after granting it. If the game dies
  between payment and grant, the store delivers the purchase again on the next launch. Grant it then.
- A **non-consumable** is finished by the engine. Buying one you own returns request 0 and never
  reaches the store.
- `Pending` means someone else must approve (Ask to Buy, a slow payment method); a `Purchased` follows
  later, possibly in another session. `Cancelled` means the player backed out; `Failed` that the store
  refused, and nothing was charged.
- Offer a **Restore purchases** button. Apple requires one for non-consumables, and it is how a player
  gets "remove ads" back after a reinstall.

## 22.4 Showing ads

```cpp
monetize::Ads* ads = f.try_resource<monetize::Ads>();
if (ads && level_finished) ads->show("level_end");    // interstitial between levels
if (ads && player_died) continue_request = ads->show("continue");  // rewarded
for (const monetize::AdEvent& e : f.events().read<monetize::AdEvent>())
    if (e.kind == monetize::AdEventKind::Rewarded && e.request == continue_request) revive();
```

- **Fullscreen ads pause the game.** From the ad's `Shown` until its `Closed` the player stops the
  sim and the audio, exactly like going to the background, and resumes on its own. Your systems do
  not need to know.
- **Rewarded:** grant the reward on `Rewarded`, not on `Closed`. A player who closes the ad early
  gets `Closed` without `Rewarded`.
- `load("continue")` preloads an ad so `show` is instant; `ready("continue")` tells you it is loaded.
  `NoFill` is normal (the network had nothing). Carry on without the ad.
- **Banners:** `show_banner("menu", BannerEdge::Bottom)` keeps the banner inside the safe area, so
  a notch or home indicator never covers it; `hide_banner()` removes it.
- **Remove ads:** once a `removes_ads` product is owned, `removed()` is true and every `load`,
  `show` and `show_banner` returns 0 without asking the network; a banner on screen is hidden. When
  it is owned at launch the ad SDK is not even started: no consent form, no ad network traffic.
  Rewarded ads go too. If your game rewards something through ads, give ad-free players another
  way to earn it (the demo gives them the continue for free).

## 22.5 Consent

On the phones the ad network asks for consent before it requests any ad:

1. **Google's consent form (UMP)**, where the law needs one (the EEA, the UK, some US states). You
   design its text and choices in the AdMob console under *Privacy & messaging*; the engine shows it
   at the first launch.
2. **App Tracking Transparency** on iOS, after the form. The prompt's text is the Info.plist's
   `NSUserTrackingUsageDescription`; packaging adds a default you can override in the game's
   Info.plist additions.

Requests wait until both are done. Declined consent or denied tracking still shows ads, but
**non-personalized** ones. Where UMP applies, the game must offer a way back to the form: show a
"Privacy options" button in its settings while `ads->privacy_options_required()` is true, and call
`ads->show_privacy_options()` when it is pressed.

To see the form on a test device from outside the EEA, run with `-monetize.ads.test_device 1`.

## 22.6 Testing without an account

`-monetize.provider fake` swaps in the fake store and ad network. They answer on the next update, the
same way every run, and their behaviour comes from convars:

| Convar | Values | Default |
|---|---|---|
| `-monetize.fake.purchase` | `success`, `cancel`, `pending`, `failure`: how every purchase ends | `success` |
| `-monetize.fake.pending_updates` | updates until a pending purchase completes | 60 |
| `-monetize.fake.owned` | `"remove_ads,level_pack"`: what the fake account already owns (restore finds it) | empty |
| `-monetize.fake.price` | the price every product reports | `$0.99` |
| `-monetize.fake.fill` | `fill`, `nofill`, `failure`: how every ad request ends | `fill` |
| `-monetize.fake.close` | `reward` (watched to the end) or `close` (closed early) | `reward` |
| `-monetize.fake.ad_updates` | how long a fullscreen ad stays up | 30 |
| `-monetize.fake.reward` | what a rewarded ad is worth | 1 |
| `-monetize.fake.delay` | updates before each answer | 1 |

`-monetize.provider none` has neither: purchases fail and ads never fill (a phone with no store, or
a relaunch with no network). `-save_dir <dir>` keeps the entitlements somewhere disposable.

To rehearse a flow on the stock player without game code, `-monetize_test` runs a script of requests
and prints every event:

```sh
mce_player -pack assets.mce -headless 1 -ticks 120 -monetize 1 -monetize.provider fake \
    -monetize_test "1:query 2:buy:gems_100 3:buy:remove_ads 5:show:continue 7:show:level_end 9:banner:menu"
```

The demo runs the full set of flows in `ctest` (`sample_shop_demo_flows`): purchase, cancel, restore
after a reinstall, a relaunch that still owns "remove ads" and requests no ad, and a recorded run
replayed to the same state hash:

```sh
build/<preset>/samples/shop_demo/sample_shop_demo -ticks 1850 -record run.rec
build/<preset>/samples/shop_demo/sample_shop_demo -ticks 1850 -replay run.rec   # same hash, no store asked
```

## 22.7 On the phones

- **Debug builds show Google's test ads**, whatever unit the catalog names
  (`-monetize.ads.test auto`). `on` forces test units in any build, `off` uses the catalog's units
  even in debug. Never click your own real ads: AdMob suspends accounts for it.
- **Android:** packaging (chapter 15) adds Play Billing to a game whose catalog sells products, and
  Google Mobile Ads with UMP plus the AdMob app id to a game with ad placements. A game without a
  `.mcstore` gets neither.
- **iOS:** StoreKit 2 builds with `-DMCE_STOREKIT=ON`, Google Mobile Ads with UMP and ATT with
  `-DMCE_ADMOB=ON -DMCE_ADMOB_SDK_DIR=<folder with GoogleMobileAds.xcframework and
  UserMessagingPlatform.xcframework>`. Both are off by default until they have been run on a device.
- **Test purchases:** in the iOS Simulator, a StoreKit configuration file (`.storekit`) with the
  catalog's product ids, selected in the Xcode scheme; on a device, a Sandbox account. On Android, the
  products set up in the Play Console, the app on an internal testing track and your Google account
  added as a license tester.

No real store or ad account is needed to build, test or ship a debug build. You need them for
release: the products in App Store Connect and the Play Console with the ids the catalog names, and an
AdMob account with the app and its ad units.

## 22.8 Store declarations

Ads and purchases change what the stores ask you to declare:

| Where | What to fill in |
|---|---|
| iOS privacy manifest | merged automatically for a game with ads: the data Google Mobile Ads collects (device id, product interaction, advertising data) and that it is used for tracking. Add other SDKs' entries in `ship/PrivacyInfo.xcprivacy` (chapter 15) |
| Info.plist | `GADApplicationIdentifier`, `NSUserTrackingUsageDescription` and Google's SKAdNetwork id are added at packaging. If you mediate other networks, add their SKAdNetwork ids too |
| App Store Connect, App Privacy | the same data types as the manifest, "used to track you"; "In-App Purchases" listed on the product page |
| Play Console, Data safety | device or other ids and app interactions collected and shared for advertising; purchase history if you keep it on a server |
| Play Console, App content | "Contains ads": yes; the target audience. A game aimed at children has stricter ad rules on both stores (families policies) |
| AdMob, Privacy & messaging | the GDPR consent message (and US state messages) that UMP shows |

## 22.9 Server-side receipt validation

The engine trusts the store's answer on the device: StoreKit 2 verifies its own signed transactions,
and Play Billing's purchase reaches the game only after Google confirmed it. That is enough for most
single-player games. A game with an online economy (gems spent on a server, competitive items) should
also validate on its own server. The place to plug that in is the `Purchased` event: send the
transaction to your server, grant only when it answers yes, then call `Store::finish`. Until
`finish`, the store keeps redelivering the purchase, so a lost connection loses nothing. The
store's own transaction id is what your server checks: StoreKit's `Transaction` id with the App Store
Server API, or the Play purchase token with the Google Play Developer API. A server, its keys and the
validation call are yours to build. The engine has no backend.

## 22.10 Known gaps

- The StoreKit 2 and iOS Google Mobile Ads sides have not been run on a device yet; that is why
  they sit behind `MCE_STOREKIT` and `MCE_ADMOB`. The Android side has not been run on a device
  either.
- One ad network (AdMob). Mediation adapters for other networks are not set up by the engine.
- Subscriptions are not supported; products are consumable or non-consumable only.
