# 26. Steam

A desktop game on Steam gets sign-in, achievements, leaderboards, Steam Cloud saves, the overlay, rich
presence and Steam Input from the stock player. The overlay, presence and Steam Input need no code;
achievements, scores and cloud saves are the same `GameServices` calls a game makes on phones.

## 26.1 Turn it on

Desktop games reach Steam through Valve's `steam_api` library, which the engine opens at run time
(ADR-0042): the Steamworks SDK is not part of the engine or its build, and a game that never asks for
Steam loads nothing. Put the library for your platform (`libsteam_api.so`, `steam_api64.dll`,
`libsteam_api.dylib`, from the Steamworks SDK's `redistributable_bin`) next to the game's executable
and start the player with your app id:

```
mce_player -pack assets.mce -steam.app_id 480
```

| Option | Default | Meaning |
|---|---|---|
| `steam.app_id` | `0` | open Steam for this app (0: the `SteamAppId` Steam starts the game with, else no Steam; -1: never). A run outside Steam names the app to the client the way `steam_appid.txt` does |
| `steam.library` | next to the executable | the `steam_api` to open |
| `steam.restart` | `0` | a run started outside Steam asks Steam to relaunch it and quits (release builds) |
| `steam.input` | `1` | read controllers through Steam Input while Steam runs |
| `steam.write_input_config` | empty | write the Steam Input action manifest and default configurations to this folder (needs `steam.app_id`) |

The log says `Steam on (app 480, <player name>)`, or `no Steam (<reason>)` and the game goes on
without it: no library, Steam not running, a client older than the library. Steam never changes the
simulation.

## 26.2 Try it: the Steam demo

`samples/steam_demo` is a small project (a scene with a `RichPresence`, a `.mcservices` with Steam names)
that CI plays against a stand-in `steam_api` (`ctest -R sample_steam_demo`): Steam Input with one
controller, the overlay holding the game, the presence sent once, an achievement, progress, a score and a
Steam Cloud save, the same state hash with and without Steam, then the build laid out as a depot and
started the way Steam starts it. To play it against the real client, put the SDK's `steam_api` beside
`mce_player`, start Steam, and run

```
mce_player -pack build/<preset>/samples/steam_demo/project/assets.mce -steam.app_id 480
```

480 is Valve's public test app (Spacewar). Sign-in, the overlay, presence and Steam Input work under it;
its achievements and leaderboards are Valve's, not the demo's, so give the demo your own app id to see
those.

## 26.3 Achievements, leaderboards and Steam Cloud

While Steam runs and no `liveops.provider` is named, Steam is the game services backend
([23.6](23-live-services.md)), so the same `unlock`, `add_progress`, `submit_score` and cloud saves reach it:

- an achievement unlocks under its `steam` name (or its `id`) once its steps are reached; progress
  below that shows Steam's progress toast. Define the achievements on the app's Steamworks page;
- a score goes to the leaderboard of that name with "keep best": set each board's sort order on the
  Steamworks page to match `order`. Steam scores are 32-bit, so larger ones are clamped;
- a cloud save is a Steam Cloud file named after the save. Give the app enough Steam Cloud quota.

`show_achievements()` opens the overlay's achievements page and `show_leaderboard()` its stats page.

## 26.4 The overlay

Steam's overlay (Shift+Tab) holds the game like a lost focus: the sim pauses with the pause overlay
up, the master bus mutes, and with `-save` the slot is written. Closing it unmutes; the player
resumes from the pause overlay as after any focus loss.

A game opens overlay pages with the `OpenOverlay` event (`mce/kit/presence.h`), from any
FixedUpdate system:

```cpp
f.events().push(mce::kit::OpenOverlay{mce::kit::OverlayPage::Store, {}});  // this game's store page
```

Pages are `Friends`, `Achievements`, `Stats`, `Community`, `Settings`, `Store` and `Web` (with `url`).
Without Steam the request is dropped.

## 26.5 Rich presence

Rich presence is data: a `RichPresence` component on any entity (a level's root, say) says what
friends see. The first enabled one by entity order is shown; none clears it. Its fields:

| Field | Steam key | Meaning |
|---|---|---|
| `status` | `status` | plain text in the friends list's game info |
| `steam_display` | `steam_display` | a token of the app's rich presence localization file, `#StatusInLevel` |
| `key1` / `value1`, `key2` / `value2` | the key | substitutions the token uses: `level` = `Ashen Court` fills `%level%` |

The player sends it when it changes, never every frame. Neither the overlay requests nor the
presence change the state hash.

## 26.6 Steam Input

While Steam runs, Steam reads every controller (Xbox, PlayStation, Switch, the
Steam Deck's own) as the game's actions, and each player can rebind them in Steam. The player feeds
those values into the input map's actions by name, next to the map's own keyboard, mouse and touch
bindings, so gameplay code reads `Jump` and `Move` as before. Steam has to know the actions first:
write its files from the same pack the game ships with,

```
mce_player -pack assets.mce -headless 1 -ticks 1 -steam.app_id 480 -steam.write_input_config steam_input
```

which writes `game_actions_480.vdf` (the action manifest) and a default configuration per controller
type (`controller_neptune.vdf` for the Steam Deck, `controller_xboxone.vdf`, `controller_ps4.vdf`,
`controller_switch_pro.vdf`). Give them to Steam on the app's Steamworks page (the Steam Input
settings, a custom configuration); the manifest names the four configurations.

- There is one action set, `Game`, holding every action of the map that has a name in the input map
  editor: a Button is a Steam button, a trigger axis an analog trigger, any other axis a stick.
- The default configurations take the map's own gamepad bindings: face buttons, d-pad, shoulders,
  Back/Start, stick clicks, sticks and triggers. The Steam Deck's adds the right trackpad to the
  action on the right stick.
- The options screen names a gamepad binding by Steam's own name for it ("A Button", or whatever
  the player rebound it to), since Steam owns the controller's bindings.
- `-steam.input 0` leaves Steam Input off; controllers then go through the map's own gamepad
  bindings as without Steam.

Headless runs play scripted input only, so Steam Input never changes a `-headless` state hash.

## 26.7 Shipping on Steam

`ship.py --steam <app id>` (or `mce ship --steam`, or the MCP tool `ship_build` with `steam_app_id`) lays
out the Steam depot of each desktop platform, with `steam_api` beside the executable, and the steamcmd
scripts that upload it: [15.7](15-shipping.md). Nothing is uploaded for you. A game started by Steam needs
no `-steam.app_id`: the player takes `SteamAppId` from Steam, and no `steam_appid.txt` ships.

## 26.8 Checklist for a Steam release

1. On the Steamworks page: the achievements (API names = the `steam` names in `.mcservices`), the
   leaderboards with their sort order, the Steam Cloud quota, the rich presence localization file with
   your `steam_display` tokens, and the Steam Input action manifest (`-steam.write_input_config`).
2. `ctest -R steam` passes, and the game plays to the same state hash with and without `-steam.app_id`.
3. `python3 scripts/ship.py --project <game> --steam <app id>` on each desktop OS into one `--out`.
4. Upload with the line in `dist/<Name>-steam/UPLOAD.txt`, install from a beta branch, and check that the
   overlay (Shift+Tab), an achievement and a cloud save work in the client.
