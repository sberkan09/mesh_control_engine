# 23. Live services (optional)

Live services, `mce_liveops` (`mce/liveops/liveops.h`), are for shipped games and are **opt-in** per
project. The engine has no accounts, no backend and no telemetry of its own. A game that turns live
services on sends what it chooses to a vendor it picks, and only with the player's consent. Nothing
here touches the sim or the determinism hash, and the game stays fully playable offline.

Built so far (the vendor-neutral parts of M-LIVEOPS):

- **crash reports sent on the next launch**: the reports the crash handler already writes (chapter 15)
  are handed to an uploader once the player consents;
- **analytics events**: `analytics::event(name, params)` from game code, delivered to one backend;
- **the consent gate** that both obey;
- **remote config** (M-LIVEOPS step 3): values the game reads by key, cooked defaults plus newer
  values fetched in the background (23.5);
- **game services** (M-LIVEOPS step 2): achievements, leaderboards and cloud saves through one API,
  with a local stand-in for Game Center and Play Games (23.6).

The vendor side (the SDK that receives reports and events on the phones) is the next part of
the step. Until it lands, the `local` provider below writes everything to files.

## 23.1 Turn it on

In the project's host (`games/<game>/host/main.cpp`):

```cpp
mce::player::PlayerDesc desc;
// ...
desc.liveops = true;
desc.analytics_backend = &my_backend;  // optional: a vendor (mce/liveops/analytics.h IBackend)
desc.crash_uploader = &my_uploader;    // optional: a vendor (mce/liveops/crash_upload.h ICrashUploader)
```

Without a backend, events are dropped. Without an uploader, crash reports stay on the player's
device. To try it on any game: `-liveops 1 -liveops.provider local`. That writes events to
`<save folder>/analytics.jsonl` and copies crash reports to `<save folder>/crash_outbox/`.

| Flag | Values | Default |
|---|---|---|
| `-liveops` | `1`: on even when the host does not turn it on | `0` |
| `-liveops.provider` | empty: the host's; `local`: the files above; `none`: neither | empty |
| `-liveops.consent` | `auto`, `granted`, `denied` (23.2) | `auto` |

## 23.2 Consent

Nothing leaves the device until consent is known:

| Consent | Analytics events | Crash reports |
|---|---|---|
| unknown (not asked yet) | held in memory, at most 256; the oldest go first | wait on disk |
| granted | sent | sent on this and later launches |
| denied | dropped | stay on this device, never sent |

With `-liveops.consent auto` (the default), the player takes the answer from the ad network's consent
form when the game shows ads (chapter 22.5). Personalized ads mean granted. Declined, or tracking
denied on iOS, means denied. A game without ads, or one with its own privacy screen, sets it itself:

```cpp
mce::liveops::set_consent(accepted ? mce::liveops::Consent::Granted : mce::liveops::Consent::Denied);
```

Save the player's answer with the game's settings and set it again at every launch. Consent does not
persist on its own.

## 23.3 Analytics events

```cpp
#include "mce/gameplay.h"
using namespace mce::liveops;

analytics::event("level_end", analytics::Params().add("level", 3).add("time_s", 41.5).add("result", "win"));
```

- Call it from anywhere, any thread. Events are queued and sent from the player's main loop.
- **Names follow the strictest vendor's rules** (Firebase Analytics), so an event valid here is valid
  everywhere. An event name or param key is 1 to 40 letters, digits and `_`, and starts with a
  letter. It does not start with `firebase_`, `google_` or `ga_`. An event has at most 25 params, and
  a string value is at most 100 bytes. A bad event is rejected and logged, not truncated, so a typo
  shows up while you develop.
- Params are integers, floating-point numbers or strings. Never put a player's name, email or any
  other personal data in them. The store declarations (23.5) assume you don't.
- Events never change the game or its state hash. A replayed run calls `event()` again like any
  other run, so its events are sent again.

The local file holds one JSON object per line:

```json
{"t":1760412345123,"name":"level_end","params":{"level":3,"time_s":41.5,"result":"win"}}
```

## 23.4 Crash reports

When the game dies, the crash handler writes `<crashes folder>/<app>-<UTC stamp>-<pid>.crash`
(chapter 15.2). On the next launch with live services on, the player lists the reports not sent yet.
Once consent is granted, it hands them to the uploader one at a time, oldest first. A sent report is
renamed `.crash.sent`, and the newest ten of those are kept for the player's own support requests.
A report that fails to send (offline, a server error) stays and is tried on the next launch.

Reports carry build ids, never symbols. Read one with `scripts/symbolicate.py` against the symbol
archive `ship` keeps beside the zip (chapter 15.3). Keep that archive for every build you release.

## 23.5 Remote config

Tune a released game without a new build: how often ads show, a bonus, an event switch, a message
of the day. Put one `.mcconfig` file in the project's assets. Its values are the defaults and the
schema: a fetched value whose key the defaults lack, or whose type differs, is ignored.

```json
{ "format": "mce-config", "version": 1, "guid": "<uuid>",
  "values": { "ad_every_levels": 3, "gem_bonus": 1.5, "winter_event": false, "motd": "Welcome",
              "drop_rate": { "type": "float", "value": 1 } } }
```

A value's type comes from its JSON: `true`/`false` is a bool, a whole number an int, any other
number a float, text a string. The `{ "type", "value" }` form names it, for a float written as a
whole number. Keys are 1-63 letters, digits, `_`, `-` and `.`. Strings are at most 255 bytes, and an
int must be within ±2^53. Anything else is a cook error.

Game code reads values from the `RemoteConfig` resource:

```cpp
const auto& rc = f.resource<mce::liveops::RemoteConfig>();
const i64 every = rc.get<i64>("ad_every_levels", 3);  // the code's own default if the key is missing
if (rc.get<bool>("winter_event", false)) { /* ... */ }
```

**When values change.** A fetch never changes what the game reads mid-run. Fetched values are saved
(the `remote_config` save) and the next launch starts with them. To use them sooner, call
`f.resource<RemoteConfig>().apply_fetched()` at a quiet moment, such as the main menu, when
`fetched_waiting()` is true; they apply at the start of the next tick. `version()` counts applies
since launch. Replays record the values in force and every apply with its tick, so they hash the same.

**Fetching** happens at launch and when the game calls `fetch()`. The host picks the fetcher with
`desc.config_fetcher` (`mce/liveops/remote_config.h IConfigFetcher`). A fetcher that talks to a
server waits for consent (23.2). The static-file fetcher reads a cooked pack holding a `.mcconfig`.
Try it with `-liveops 1 -remote_config.file <pack> -remote_config.dump 1`; the dump prints every
value at start and after each apply. A failed fetch keeps the values in force.

| Flag | Values | Default |
|---|---|---|
| `-remote_config.file` | a cooked pack (or `cooked/` folder) holding a `.mcconfig` | empty: the host's fetcher |
| `-remote_config.dump` | `1`: print the values in force | `0` |

`-liveops.provider none` turns the fetcher off too.

## 23.6 Game services

Achievements, leaderboards and cloud saves through one API. Game Center and iCloud on iOS, Play Games
Services on Android, and a local folder on desktop and in CI. List them in one `.mcservices` file in
the project's assets:

```json
{ "format": "mce-services", "version": 1, "guid": "<uuid>",
  "achievements": [ { "id": "first_win", "ios": "com.studio.game.first_win", "android": "CgkI..." },
                    { "id": "win_10", "steps": 10 } ],
  "leaderboards": [ { "id": "best_time", "order": "lower_is_better", "ios": "...", "android": "..." } ],
  "cloud_saves": [ "progress" ] }
```

Game code names everything by `id`. The store ids stay in this file. `steps` greater than 1 makes an
achievement incremental. A leaderboard's `order` is `higher_is_better` (the default) or
`lower_is_better`, for times. `cloud_saves` lists the saves (`save::write` names) that may sync.

With live services on and this file in the pack, the player installs the `GameServices` resource:

```cpp
if (auto* gs = f.try_resource<mce::liveops::GameServices>()) {
    if (won) {
        gs->unlock("first_win");
        gs->add_progress("win_10", 1);
        gs->submit_score("best_time", time_ms);
    }
    for (const auto& e : f.events().read<mce::liveops::GameServicesEvent>())
        if (e.kind == mce::liveops::GameServicesEventKind::CloudLoaded) reload_progress();
}
```

**Offline first.** An unlock, progress or score counts on the device at once (`unlocked()`,
`progress()`, `best_score()`) and is kept in the `game_services` save. It waits there until the player
is signed in and the platform takes it. A failure is tried again later, also after a relaunch, and
nothing is sent twice. Sign-in is silent at launch; call `sign_in()` from your own "Sign in" button.

**Cloud saves** are versioned. `cloud_save("progress")` uploads the local save on top of the
version this device last synced. `cloud_load("progress")` downloads: a newer cloud version replaces
the local save if the local one has not changed since the last sync, and the game gets
`CloudLoaded` and reloads. When both changed (two devices played), that is a conflict:

| `GameServicesOptions::conflict` | What happens |
|---|---|
| `AskGame` (default) | a `CloudConflict` event; nothing changes until `resolve_conflict(name, keep_local)` |
| `PreferCloud` | the cloud's save replaces the local one (`CloudLoaded`) |
| `PreferDevice` | the local save is uploaded over the cloud's (`CloudSaved`) |

A good conflict screen shows both saves' progress and lets the player choose.

**Events** enter at the start of a tick like store events (chapter 22), and replays record them,
so replays hash the same. Game services are the player's own platform account, not analytics, so
they do not wait for the consent in 23.2.

Try it with `-liveops 1 -liveops.provider local`: a folder plays the platform. Accepted achievements
and scores go to `game_services.jsonl` in it, and cloud saves to `cloud/<name>.blob`.

| Flag | Values | Default |
|---|---|---|
| `-game_services.dir` | the folder playing the platform; give two save folders the same one to play two devices | `<save folder>/game_services_platform` |
| `-game_services.offline` | `1`: sign-in and every call fail | `0` |
| `-game_services_test` | `"tick:action[:id[:value]] ..."` with `sign_in`, `unlock`, `progress`, `score`, `write`, `cloud_save`, `cloud_load`, `keep_local`, `keep_cloud`; prints every event | empty |

## 23.7 Store declarations

Turning on a vendor changes what you declare (chapter 22.8 lists where):

- crash data and diagnostics: "Crash data", collected, not linked to the player, not used for
  tracking (App Store); "Crash logs" and "Diagnostics", collected (Play Data safety);
- analytics events: "Product interaction", collected, for analytics; linked to the player only if
  your vendor ties events to a user id;
- game services: "Gameplay content" (cloud saves) and "User IDs" (the Game Center or Play Games
  player id), linked to the player, for app functionality.

With the `local` provider nothing leaves the device and there is nothing to declare.

## 23.8 Known gaps

- No vendor yet: the SDK side on iOS and Android, symbol upload from `ship`, and the vendor's own
  privacy manifest entries are the next part of M-LIVEOPS step 1.
- Remote config has no server fetcher yet: a vendor's config service (or an HTTP download of a
  cooked pack) comes with the vendor choice. The static-file fetcher and the cache work today.
- Game services have no native provider yet: Game Center / iCloud and Play Games Services v2 need a
  Mac, devices and test accounts. The API, the outbox, cloud versions and the local fake work today.
