# 23. Live services (optional)

Live services, `mce_liveops` (`mce/liveops/liveops.h`), are for shipped games and are **opt-in** per
project. The engine has no accounts, no backend and no telemetry of its own. A game that turns live
services on sends what it chooses to a vendor it picks, and only with the player's consent. Nothing
here touches the sim or the determinism hash, and the game stays fully playable offline.

Built so far (M-LIVEOPS step 1, vendor-neutral part):

- **crash reports sent on the next launch**: the reports the crash handler already writes (chapter 15)
  are handed to an uploader once the player consents;
- **analytics events**: `analytics::event(name, params)` from game code, delivered to one backend;
- **the consent gate** that both obey.

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

## 23.5 Store declarations

Turning on a vendor changes what you declare (chapter 22.8 lists where):

- crash data and diagnostics: "Crash data", collected, not linked to the player, not used for
  tracking (App Store); "Crash logs" and "Diagnostics", collected (Play Data safety);
- analytics events: "Product interaction", collected, for analytics; linked to the player only if
  your vendor ties events to a user id.

With the `local` provider nothing leaves the device and there is nothing to declare.

## 23.6 Known gaps

- No vendor yet: the SDK side on iOS and Android, symbol upload from `ship`, and the vendor's own
  privacy manifest entries are the next part of M-LIVEOPS step 1.
- Game services (achievements, leaderboards, cloud saves) and remote config are later steps.
