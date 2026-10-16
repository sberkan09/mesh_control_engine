# Changelog

Engine versions and what each one added. How and when the version moves: [docs/VERSIONING.md](docs/VERSIONING.md).
Every PR that changes what a user of the engine sees adds one line under **Unreleased**.

## Unreleased

- Utility AI, attack tokens and point queries, M-AI step 4 (SBA-131): a `utility` node runs the child whose
  `score` (a base plus weighted linear, power, logistic or step curves of blackboard keys) is highest, and
  keeps a running child until another beats it by `hysteresis`. An `AiTokenPool` hands out a few tokens and a
  `token` node takes one or fails, so only some enemies attack at once. The stock task `find_point` stores the
  best point on rings around the entity: on the navmesh or grid, visible or hidden from a key, near, far or
  close. `.mcbt` cooks to `CHNK_BT` v3. User guide 13.8.
- AI perception, M-AI step 3 (SBA-130): a `Perceiver` sees every `Stimulus` of another team inside its cone and
  range with a clear line of sight (rays through the 3D or the 2D physics world), remembers a lost target for
  `forget` seconds and hears `kit::Noise` events (the tree's `make_noise` task makes one). It writes `target`,
  `sees`, `last_seen` and `heard_at` into its tree's blackboard. `interval` staggers a crowd's checks over the
  ticks. In `ai_demo` the guard now spots the thief. User guide 13.8.
- The Luau script API, M-SCRIPT step 2 (SBA-148): scripts spawn, change and destroy entities (through
  `Commands`), instantiate prefabs, read and emit events by name (`MCE_EVENT` reflects an event type; the
  physics trigger and contact events have it), send and hear the kit's named signals, play sounds, read input
  actions, change scene, quit, and query 2D and 3D physics. `mce.field` handles and multi-field `get` / `set`
  halve the per-entity cost: the 2,000-entity bench runs in about 0.35 ms per tick. User guide 27.3 lists it all.
- Luau scripts as assets, M-SCRIPT step 1 (SBA-147, ADR-0041): `.luau` files under `assets/` cook (importer
  `script`) to a `Script` asset of Luau bytecode, with syntax errors at `file:line:col`. A `ScriptSystem`
  component names a script, its phase (`Pre` or `Post`) and its order; `mce_player`, play-in-editor and MCP
  `run_headless` run the scene's scripts from bytecode where `MCE_WITH_SCRIPT` is on, and the player links no
  Luau compiler. Scenes without scripts run and hash as before. New user guide chapter 27 (Scripting in Luau).
- 2D bone animation, M-ANIM2D step 2 (SBA-141): a `.mcskel2d` holds `"animations"`, clips of per-bone rotate,
  translate and scale keys (offsets from the rest pose) with linear, stepped or bezier curves and timed events.
  A `Skeleton2DAnimator` plays them on the entity's `Skeleton2D`, cross-fades on a clip change and writes only
  the channels the clips key, so gameplay keeps the rest. An `.mcanim` graph drives it with the same state
  machine as the 3D `Animator`. Events raise `AnimEvent3D`. State is saved and hashed. User guide 7.13.
- Behavior tree stock tasks and aborts, M-AI step 2 (SBA-129): trees reach the kit with no code. `move_to` walks
  a `NavAgent3D` or a `PathAgent` to a vec3 key or after an entity key, `look_at` turns the entity, `animate`
  plays a sprite or Animator clip (and can wait for it), `play_sound`, `spawn`, `raise` and `wait_event` (which
  sequences hear too) and `log`. A guard's `abort` ("self", "lower", "both") re-checks its condition every tick
  and stops its own branch or a selector's lower-priority branch. `samples/ai_demo`: the stock player runs a
  guard that patrols, hears an alarm and catches a thief (`ctest -R ai_demo_player`). User guide 13.8.
- Bus effects, M-AUDIO2 step 3 (SBA-120): `AudioBusEffects` gives a bus a low-pass and high-pass (gliding, no
  clicks) and a send into one of two Freeverb reverb units, `AudioReverb` sets a unit up, and `AudioLimiter`
  turns on the master limiter, all from the scene and keyable in timelines. `AudioDevice::set_bus_filter`,
  `set_bus_send` and `set_reverb` do the same from code. `audio.reverb 0` turns reverb off. User guide 10.5.
- Behavior trees, M-AI step 1 (SBA-128, ADR-0045): `.mcbt` files describe an AI's decisions as data (sequence,
  selector, parallel, invert, succeed, fail, repeat, retry, cooldown, timeout, guard, check, wait, set, emit and
  task nodes) over typed blackboard keys, and cook to `CHNK_BT` with errors at file:line:col. An `AiBrain`
  component runs one per entity every fixed tick, and a `Blackboard` component holds the keys. All run state is in
  the two components, so trees are deterministic, hashed, replayed and saved. Games register tasks by name
  (`kit::BtRegistry::register_task`) and read and write keys by name (`kit::bb_*`). User guide 13.8.
- Sound sets and pitch, M-AUDIO2 step 2 (SBA-119). A `.mcsound` names several clips and picks one per play (random
  with no immediate repeat, or in sequence) with a pitch and gain range; it is an Sfx asset, so `AudioSource`,
  `SfxTrigger`, `PlaySfx`, sequences and timelines take it as they are. Picks come from the tick, so replays repeat.
  `PlayParams::pitch` and `AudioDevice::set_handle_pitch` play any clip at a rate (0.25..4). MCP `asset_create`
  makes a `SoundSet`. User guide 10.3 is new; the audio chapter's later sections move up one number.
- 2D skeletons, M-ANIM2D step 1 (SBA-140): `.mcskel2d` assets (bones with a rest pose, textured slots in draw
  order) cook to `CHNK_SKEL2D`. A `Skeleton2D` component poses one on its entity every fixed tick from the rest
  pose and its per-bone `pose` offsets (up to 64 bones), and the sprite extraction draws its slots with the
  component's tint, flips, layer and order. `Bone2DFollow` keeps an entity on a named bone. Poses are in the
  state hash and in saves. MCP `asset_create` makes `.mcskel2d` files. User guide 7.13.
- Material extensions, M-VISUAL2 step 4 (SBA-104): clear coat, sheen, anisotropy and transmission. `.mcmat`
  gains `clearcoat`, `clearcoat_roughness`, `sheen_color`, `sheen_roughness`, `anisotropy`,
  `anisotropy_rotation`, `transmission` and `ior`; glTF files bring the same from `KHR_materials_clearcoat`,
  `_sheen`, `_anisotropy`, `_transmission` and `_ior` (factors only). Transmissive materials show the opaque
  scene behind them, tinted and slightly refracted. Materials without them draw exactly as before. User guide
  5.7 and 8.5.
- Steam demo and guide, M-STEAM step 6 (SBA-111): `samples/steam_demo` plays on Steam through the stock player;
  `ctest -R sample_steam_demo` runs it against the stand-in `steam_api` (Steam Input, the overlay, rich presence,
  achievements, a leaderboard, Steam Cloud, the same state hash with and without Steam, the depot started like
  Steam starts it). New user guide chapter 26 (Steam) with a release checklist.
- `ship --steam`, M-STEAM step 5 (SBA-110): `scripts/ship.py --steam <app id>` and `mce ship --steam` lay out the
  desktop build's Steam depot (`steam_api` beside the executable, no `steam_appid.txt`) and the steamcmd app and
  depot build scripts in `dist/<Name>-steam/`; shipping on each OS into the same folder adds its depot. Nothing is
  uploaded. The MCP tool `ship_build` takes `steam_app_id`. A game started by Steam signs in from `SteamAppId`
  without `-steam.app_id` (`-1` keeps Steam off). Covered by ctest `steam_depot`. User guide 15.7.
- Compressed audio clips, M-AUDIO2 step 1 (SBA-118, ADR-0044). `.ogg` files import (importer `ogg`), and a
  `.wav` sidecar's `"compress": "vorbis"` stores the clip as Ogg Vorbis, about a tenth of float PCM
  (`"quality"`, default 0.4). A compressed clip decodes when it loads; on the Music bus its stream pages decode
  as they fill. libvorbis encodes in the cook, stb_vorbis decodes at run time. The WAV importer's version is
  now 2, so clips re-cook once.
- Timelines over MCP and the cinema demo, M-CINEMA step 6 (SBA-81): `timeline_create`, `timeline_add_track` and
  `timeline_add_key` write `.mctimeline` files checked by the cook, and `timeline_preview` draws the scene with every
  timeline held at a given second (the player's `-timeline.scrub`). `samples/cinema_demo` opens on a splash film and
  plays a cutscene with a camera dolly, a rising gate, music, subtitles and a blended cut, all as data.
  Undelivered opens on a ferry cutscene. A `Cutscene` no longer hashes three undefined padding bytes, and the
  fullscreen video is no longer drawn upside down.
- Sprite-sheet UI skins, M-UI-DATA: a `.mcui` document's or `.mcuitheme`'s `"sprites"` sheet is now drawn by the
  stock player, `ui_preview` and the editor's UI Document panel (style `sprite_region` and 9-slice `slice` index
  the sheet's regions; a sheet that does not load falls back to the built-in skin with a warning). The `pixel`,
  `clean` and `story` starter themes of `mce new` carry a skin each (bevelled, rounded, paper with an ink rule).
  User guide 12.2. `ui_preview`'s `"scene"` result no longer reads freed memory.
- Spectators and clock sync, M-NET3 step 2 (SBA-124): a `RollbackSession` with `.spectator = true` watches
  without playing, fed confirmed inputs by one player (`write_spectator_packet`), and checks its hashes. With
  `time_sync` (on by default) a rollback peer whose clock runs ahead skips a tick now and then instead of
  stalling in bursts: in `sample_net_rollback -drift 30` the fast peer stalls 10 times instead of 40. Rollback
  packets are now version 2. `ReplicationServer::set_view` / `set_sees_all` give replication spectators a
  camera or every entity. User guide 20.7 and 20.10.
- Per-client delta snapshots, M-NET3 step 1 (SBA-123): `ReplicationServer::write_snapshot_for` writes each client
  only the entities relevant to it (`NetRelevance { radius }` around the entities it owns) and codes them against
  the newest snapshot it acknowledged (`write_ack` / `read_ack`): a still entity costs two bytes. `apply` reads
  both forms. `sample_net_headless` now sends about a third of the bytes it did. User guide 20.10.
- Replication as data, M-NET2 step 7 (SBA-88): mark entities `NetReplicated` in a scene and
  `ReplicationServer::assign_tagged` gives them their `NetId` with no code. Component fields flagged
  `Replicated` (`MCE_COMPONENT(Health, (hp, Replicated))`) travel in every snapshot and land in the client's
  copy; other fields keep the client's value, and `Entity` fields come back as the client's own entities.
  `NetOwner` names the player that controls an entity. A component built with other flagged fields on the
  client is skipped and counted, never misread. `refl_describe_component` marks the fields `"replicated"`.
  Snapshot version 2. User guide 20.9.
- Performance counter gate and guide, M-PERF step 5 (SBA-117). `scripts/ci/perf_gate.py` runs `perf_run` on
  a copy of a project and fails when draws, passes, dispatches, triangles, meshes, sprites or sprite batches
  grow past the project's `perf/counters.json` (`--update` writes new ones). Glowmoth and Marble Garden run it
  in `local_ci.sh` (`*_perf_counters`), and `glowmoth_perf_counters_seeded` proves it catches a planted draw
  regression. New user guide chapter 25, "Performance". M-PERF is done on the desktop.
- Steam Input, M-STEAM step 4 (SBA-109): while Steam runs, Steam reads every controller as the input map's actions
  and the player feeds them in by name next to the map's own bindings (`InputSystem::set_action_feeds`). The options
  screen names gamepad bindings by Steam's own names. `mce_player -steam.write_input_config <dir>` writes the action
  manifest and the Steam Deck, Xbox, PlayStation and Switch Pro default configurations from the pack's input map;
  `-steam.input 0` turns it off. Headless hashes are unchanged. Covered on the stand-in by `test_input`,
  `test_platform` and `player_steam_input`. User guide 23.10.
- Contact shadows, M-VISUAL2 step 3 (SBA-103): `Environment3D.contact_shadows` (or `Scene3D::set_contact_shadows`)
  marches a short screen-space ray toward the sun so creases, feet and small details get the shadows the cascades
  miss (`contact_length`, `contact_thickness`, `contact_intensity`). Sun only for now; off on the Low tier;
  `r.contact_shadows 0` turns it off. User guide 8.
- The editor's Profiler reads captures, M-PERF step 4 (SBA-116). A new **Capture** tab opens a `.mcperf` from
  the player or `perf_run`: the budget verdict for a target, a strip of every frame (spikes red, over-budget
  amber; click one), the chosen frame's phases, GPU stages and counters, the spikes and the slowest systems.
  **Compare with** adds a second capture's before/after tables. `mce_editor -editor.perf_capture <file>` opens
  one at start-up. The comparison is now shared editor code (`mce/editor/perf_compare.h`) that MCP's
  `perf_compare` uses too. User guide 3 and 14.
- Steam overlay and rich presence, M-STEAM step 3 (SBA-108): the Steam overlay holds the game like a lost focus
  (pause, mute, the focus-loss save). A `RichPresence` component says what friends see (status, a localization
  token and two substitutions), sent only when it changes. The `OpenOverlay` event opens the friends,
  achievements, stats, community, settings, store or web page (`mce/kit/presence.h`). Neither changes the state
  hash. Covered on the stand-in by `test_kit`, `test_platform` and `player_steam_overlay`.
- `perf_run` and `perf_compare` over MCP, M-PERF step 3 (SBA-115). `perf_run` measures a scene with the real player,
  one sim tick per frame, so its counters repeat run to run. It judges the result against a target's budget, with
  advice for each limit it is over. It also returns frame, sim, render and GPU times, draws, triangles and sprites,
  the slowest systems, the GPU stages and spike frames, and it keeps the `.mcperf` capture. `perf_compare` compares
  two captures (by default the last two runs) and says what got better or worse. User guide 14.
- Steam game services, M-STEAM step 2 (SBA-107): while Steam runs, it is the desktop game services backend, so
  `GameServices` unlocks, progress, scores and cloud saves reach Steam achievements, leaderboards (found once,
  uploaded keep-best) and Steam Cloud files. `.mcservices` entries take an optional `steam` name (the catalog id
  otherwise); the catalog chunk is v2, so packs re-cook. `steam::await_call` waits for Steam's asynchronous call
  results. The stand-in `steam_api` covers it all in CI (`test_platform`, `player_game_services_steam`).
- Performance budgets per target, M-PERF step 2 (SBA-114). The `.mceproj` takes `"budgets"` for desktop, mobile
  and web: frame, sim, render and GPU milliseconds, draws, triangles, sprites, upload bytes, heap allocations,
  sprite textures per scene and the largest texture edge. Fields left out keep the engine defaults. `project_set`
  checks the limits, and `project_get` shows each target's budget in force. `project_check` takes a `target` and
  flags images over its `texture_size` and scenes over its `sprite_textures`, with the target's `sim_ms` as the
  per-tick budget. `perf::judge` gives a capture's verdict against a budget. User guide 2.2 and 14.
- Steam, M-STEAM step 1 (SBA-106, ADR-0042): `mce/platform/steam.h` opens Valve's `steam_api` at run time and
  talks to it through the Steamworks flat API, so neither the repository nor the build needs the SDK and a game
  without Steam loads nothing. The stock player signs in with `-steam.app_id <id>` (`-steam.library`,
  `-steam.restart` for release builds Steam relaunches) and pumps Steam's callbacks every frame; the sim hash is
  unchanged. User guide 23.10.
- Performance captures, M-PERF step 1 (SBA-113): `mce_player -perf_capture <file>` writes a `.mcperf` JSON capture
  of the run. Each frame gets its wall, sim, render CPU and GPU time, its GPU stages (3D), sim time per ECS phase and
  its draws, passes, triangles, sprites, upload bytes and live heap allocations. The run adds per-system totals, p50/p95/p99/max
  and the spike frames with their likely cause ("sim: FixedUpdate_Post", "gpu: shadow"). `-perf_warmup N` skips
  loading frames. A capture never changes the state hash. User guide 16.
- A spatial upscaler and sharpening, M-VISUAL2 step 2 (SBA-102): frames drawn below the output size (Low and
  Medium tiers without TAA) now reach it through an edge-adaptive upsample by default in kit games
  (`Environment3D.upscaler`), and `Environment3D.sharpen` sharpens the final frame. Players override both with the
  saved `r.upscaler` (`scene`, `bilinear`, `spatial`) and `r.sharpen`. User guide 8.
- Screen-space reflections, M-VISUAL2 step 1 (SBA-101): `Environment3D.ssr` (or `Scene3D::set_reflections`) makes
  smooth surfaces mirror what is on screen over the probes and the sky (`ssr_max_distance`, `ssr_thickness`,
  `ssr_max_roughness`, `ssr_roughness_fade`, `ssr_intensity`). It reads the previous frame, so it shows from the
  second frame; off on the Low quality tier, `r.ssr 0` turns it off. User guide 8.
- Rooms through a relay, and WebSocket (SBA-87, M-NET2 step 6): `mce_relay` serves rooms with six-letter codes
  over UDP and WebSocket at once, so players behind home routers and browsers can play together without opening
  a port. `net::RelayClient` is a Transport whose peers are the room's members, so replication, prediction and
  rollback run through it unchanged; `net::WebSocketHost` is a Transport over WebSocket (native TCP, or the
  browser's in a web build). `sample_net_headless -sim 1 -relay 1` plays a session through a room. User guide 20.8.
- Accessibility checks and guide, M-ACCESS step 5 (SBA-99). `project_check` now flags small and low-contrast
  `.mcui` text, timelines with sound but no subtitles, and uncaptioned sounds. The novel stage speaks its pages and
  rows through the screen reader, and a control in a labelled row says the label first ("Move Right, D, button").
  The player takes `-access_log <file>` and no longer drops menu presses when it draws fewer frames than it runs
  ticks. Kindle and Undelivered get the stock options screen, and Kindle captions its sounds. User guide chapter 24.
- Video, M-CINEMA step 5 (SBA-80, ADR-0043): `.mpg` files (MPEG-1 with MP2 sound; ffmpeg converts anything)
  import as videos, and a `VideoPlayer` plays one on its entity's `Sprite` or over the whole screen as a splash,
  with its sound in step (play, pause, loop, restart, seek, `finished` at the end). A timeline `video` track plays
  a film as a cutscene beat. User guide 13.7.
- Round-3 friction fixes (SBA-61): reloading a game module drops its old convars; an input script's `=0`
  presses an action with value 0; a Text in a horizontal Stack no longer wraps below UI scale 1; game code can
  quit the app (`scene::request_quit`, `UiFlow::quit_on` for a Quit button); `render_frame`, `render_sheet` and
  `run_headless` take `convars` for one run; a `walk_to` waypoint `{at, wait}` stands still there; a smooth
  `Decal3D` fills the normal map's bumps under it (puddles no longer light every bump as a dotted pattern);
  `asset_delete` removes the file's `CREDITS.md` row and a font's license text with the family's last font.
- Screen reader support, M-ACCESS step 4 (SBA-98): the stock player speaks the focused menu item and a
  focused slider's new value through TalkBack on Android (and, built with `MCE_SCREEN_READER_NATIVE`, VoiceOver on
  iOS and macOS and Narrator, NVDA or JAWS on Windows), while the OS reports a reader on (`access.screen_reader`).
  `ui::build_access_tree` gives each widget a role, a name and a value. User guide 12.5.
- Rollback netcode (SBA-86, M-NET2 step 5): `net::RollbackSession` runs the whole game on every peer and
  exchanges only inputs. It guesses late remote inputs, rolls back and replays when a guess was wrong, waits only
  past `max_prediction` (0 is lockstep), and compares state hashes of confirmed frames so a desync is reported at
  the frame it happened. `samples/net_rollback` keeps four peers in sync at 100 ms with 5% loss. User guide 20.7.
- Subtitles and sound captions, M-ACCESS step 1 (SBA-95): the stock player draws timeline subtitle lines,
  `kit::SoundCaption` tags on sounds and `kit::ShowCaption` lines at the bottom of the screen
  (`ui::SubtitleView`), with `access.subtitles`, `access.captions`, `access.subtitle_speaker`, `access.subtitle_size`
  and `access.subtitle_background` on the stock options screen. The NPC dialogue box follows the speaker and
  background settings. User guide 10.7.
- Cutscenes, M-CINEMA step 4 (SBA-79): a `Cutscene` next to a `TimelinePlayer` holds gameplay while the timeline
  plays (timelines, animators, cameras, particles and audio play on; `World::run_during_cutscenes` adds your own
  systems), and the player draws letterbox bars under the timeline's subtitles and a hold-to-skip prompt (Esc / B);
  skipping still fires the remaining event keys. Cinematic cameras: `CameraRail3D` (a dolly on a curve through up
  to 8 points), `CameraLookAt3D`, `CameraShake3D` (`ShakeTrigger` shakes it; Reduce motion tones it down) and
  `CameraBlend3D`; a timeline camera cut with `"blend": seconds` eases between cameras. Novels: `"timelines"` in
  story.json and `@cutscene <id>`. User guide 13.6.
- The editor's Animation panel, M-CINEMA step 3 (SBA-78): an `.mcanim` animation graph drawn as boxes and arrows
  (drag states, add transitions from a right-click, any-state transitions, the start state), an inspector for
  parameters, states (clip or 1D blend points, loop, speed, events) and transitions (conditions, fade, on finish),
  and a viewport preview that poses any clip of an entity's mesh at a scrubbed time or runs its graph with typed
  parameter values, without touching the scene. A state may carry `pos` (where the editor draws it; the cook ignores
  it). Double-click an `.mcanim` in Assets, or `mce_editor -editor.open <file>`. User guide 8.9.
- Fair hits over lag (SBA-85, M-NET2 step 4): `net::LagCompensator` keeps a short history of `net::HitBox`es on
  the server and checks a shot against the tick its client was drawing (`ReplicationClient::drawn_tick`), within
  `max_rewind_s`. Over a 100 ms link every shot at the drawn target hits when rewound and none at the server's
  present. `sample_net_headless` fires at a drifter from every client. User guide 20.4.
- A stock options and remap screen, M-ACCESS step 3 (SBA-97): `mce new` projects get `assets/ui/options.mcui`
  (display, audio and controls tabs, shown by Pause). The controls tab lists the game's actions by name with a
  button per binding that captures the next press, a hold-to-toggle switch per button action, left- and
  right-hand keyboard presets and a look sensitivity slider. The player now keeps every Saved convar and the key
  bindings in `settings.cfg` / `<app>.bindings` in the save folder (`-settings 0` turns that off). Cooked input
  maps carry their action names (chunk v2; v1 still loads).
- Predict the local player (SBA-84, M-NET2 step 3): `net::Predictor` runs the player's inputs through the same
  step function as the server and draws the result at once; `net::InputReceiver` runs each input once on the
  server, in order, across lost packets. On each snapshot the client reconciles with the server's ack, replays
  what the server has not seen yet and glides to any correction instead of jumping.
  `ReplicationClient::set_predicted` keeps snapshots from drawing that entity. In `sample_net_headless` at 75 ms
  the player is drawn about 10 ms behind its input, against 200 ms without prediction. User guide 20.4.
- The editor's Timeline panel, M-CINEMA step 2 (SBA-77, FR-EDT-130): tracks and keys on a time ruler with
  scrubbing, snapping, key drags and a key inspector, a curve view of transform and property tracks, a viewport
  preview that plays the scene paused at the playhead (the scene document never changes), and record mode, which
  keys inspector and gizmo edits at the playhead and binds new entities. Double-click a `.mctimeline` in Assets,
  or `mce_editor -editor.open <file>`. User guide 13.5 "Editing timelines in the editor".
- Accessibility options, M-ACCESS step 2 (SBA-96): `access.colorblind` (protanopia, deuteranopia, tritanopia
  correction in the 2D post and 3D tonemap passes) with `access.colorblind_strength`, `access.text_scale` for UI
  text, `access.high_contrast` (a high-contrast palette over every UI document) and `access.motion_scale` (camera
  shake, chromatic aberration, motion blur). All are saved convars outside the state hash.
- Smooth remote entities (SBA-83, M-NET2 step 2): `ReplicationClient::set_interpolation` draws replicated entities a
  little in the past through a jitter buffer that adapts to the measured snapshot interval and arrival jitter,
  blending the snapshots around the render time. Past the newest snapshot an entity moves on for at most 0.1 s, then
  holds. Over a 60 ms ± 40 link with 10% loss, motion is more than 10× smoother than raw snapshots.
  `sample_net_headless` interpolates by default (`-interp 0` for raw). User guide 20.4.
- Timelines, M-CINEMA step 1 (SBA-76, FR-ENT-110): a `.mctimeline` asset of keyed tracks (transform, property by
  reflected path, activation, animation clip, audio, camera cut, event, subtitle) and a `TimelinePlayer` component
  that plays one on its entity and up to eight bound entities, with play, pause, speed, loop, restart and a
  finished flag. Keys fire on tick math, the state lives in the component (state hash, replays, saves), and
  `kit::timeline_scrub` moves the playhead without firing cues. Event keys also push `SeqSignal`, so sequences
  can wait on a timeline beat. MCP `asset_create` makes `.mctimeline` files; user guide 13.5.
- Multiplayer test harness (SBA-82, M-NET2 step 1): `net::SimNetwork` is an in-memory network on a virtual clock
  with per-link latency, jitter and loss from a seed, so a server and several clients in one process give the same
  hash on every run and build. `net::Transport` is the interface both it and `net::Host` implement, so game code
  runs over either. `Host::set_conditions` plays a real build over a simulated bad network.
  `sample_net_headless -sim 1 -clients N` is the headless multi-client runner, and `local_ci.sh`'s determinism stage
  runs it. `net::NetStatsMeter` adds a `NET` line (RTT, loss, bandwidth, drops) to the player's stats overlay (F3)
  through the new `DevStatsLines` world resource. User guide 20.6.
- Phone fixes from the Simulator and emulator checks (SBA-30): a language the player chooses no longer falls back
  to the device's other languages (English chosen on a Turkish phone showed the kit's words in Turkish); SDL GPU's
  debug mode is off in the iOS Simulator, whose Metal validation aborted on the first indirect draw; MSL for iOS
  supports base vertex and base instance, so instanced PBR (Solstice Reach) opens on iOS.
  `ship.py --project <dir> --target android|ios` builds the project (`MCE_ONLY_PROJECT`; the phone presets add
  none), saves on phones go to the app's sandbox (`crash::user_data_root()`), and the novel stage draws the box
  with `story.json`'s `text.font` instead of the pack's first font. The novel template's and Undelivered's camera
  fit the stage to the screen, so a phone no longer shows it as a 1280×720 box in the middle.
- Screens that open and close themselves, M-UI-DATA demo (SBA-38): `kit::UiFlow` shows, hides and holds a
  `UiScreen` on input actions (`show_on`, `hide_on`, `load_on` a scene, `pause`), and a paused screen holds the
  world (`World::set_held`: fixed-step systems stop, the tick count goes on, the hash replays). Undertow gets a
  title, a pause menu, options with music and effects volume and a bound HUD, authored only through its MCP
  server (`tools/author_ui_mcp.py`), in two themes on two scenes; library fonts Pixelify Sans, Press Start 2P,
  Atkinson Hyperlegible Next and Karla; `mce://docs` serves `templates/`.
- Remote config builds for iOS and Android again: a float value is read with `strtod` instead of `std::from_chars`,
  which iOS before 26 and the NDK's libc++ do not have. The values it accepts are unchanged. The Game Center
  backend (`-DMCE_GAMEKIT=ON`) now compiles for the iOS Simulator and a `ship --target android` build with Play
  Games produces its APK on a Mac (SBA-23).
- The editor's UI Document panel (SBA-36, M-UI-DATA step 6): a `.mcui` document's widget tree (add, remove,
  reorder, undo), an inspector for the selected widget (anchors, per-state or every-state styles, layouts, nav,
  action, bind) and a live preview at phone, tablet and desktop sizes with the theme applied. Edits are read back by
  the cook before they are kept and saved in the canonical form the MCP `ui_*` tools write, so a document goes
  between the panel and MCP unchanged. Double-clicking a `.mcui` in Assets opens it.
- `ui_theme_set` (SBA-57, M-UI-DATA step 5): an MCP tool sets the `.mcuitheme` a UI document is drawn with (by path
  or GUID, `null` for the default look) through the same read-back and canonical write as the widget tools, and
  `ui_list` with a document names its theme. There is no project-wide default theme: each document names its own.
- Starter UI themes, M-UI-DATA step 7 (SBA-37): `mce new` projects carry `assets/ui/themes/pixel.mcuitheme`
  (Pixelify Sans, Press Start 2P), `clean.mcuitheme` (Atkinson Hyperlegible Next, Karla) and `story.mcuitheme`
  (Crimson Pro), whose OFL fonts bake Latin-1 and Latin Extended-A, with `CREDITS.md` rows and license texts.
  User guide 14 and the MCP `start_game` prompt tell agents to pick or make a theme for each game.
- Game services run on Game Center and Play Games Services v2 (SBA-23): `liveops::PlatformGameServices` sends
  achievements and scores under the catalog's `"ios"` / `"android"` ids and keeps cloud-save versions inside
  Game Center saved games and Play Games snapshots, so a second device gets a conflict as on the local stand-in;
  a saved game the engine did not write is never overwritten. iOS builds it with `-DMCE_GAMEKIT=ON`; `ship`
  stamps Play Games into a game with a `.mcservices` when `MCE_PLAY_GAMES_APP_ID` is set. Not yet run on a device.
- Novel kit words in the player's language (SBA-59, M-MOBILE 11e follow-up): the title menu, checkpoint, ending
  card and extras page read `novel.*` keys from the string tables (English when no table has one; a `.touch` row
  after a finger read on, "Tap" for "Enter"), and the story's title, extras row and ending titles read
  `story.title`, `story.extras` and `story.ending.<id>`, which `mce story compile` now writes to the base table.
  `novel::kit_text` / `kit_count` serve a game's own panel hook. The English checkpoint line says "1 page read".
- Remote config fetches from Firebase Remote Config on phone builds shipped with Firebase (SBA-24): each
  console value is read as the type of its `.mcconfig` default (`liveops::parse_config_value`), and one the
  defaults lack or that does not read as its type is skipped. `ship` adds `firebase-config` on Android, and
  iOS links `FirebaseRemoteConfig` from `MCE_FIREBASE_SDK_DIR`. Not yet run on a device.
- UI themes, M-UI-DATA step 3 (SBA-33): a `.mcuitheme` asset holds a palette (`UiPalette`), a font per role
  (body, title, button) and per-kind `UiStyle` looks. A `.mcui` document names its theme with `"theme"`, a
  `UiScreen` may override it, and every style field a document leaves at its kind's default look takes the theme's
  value. Widgets pick a font with `"font_role"` (`ui::Skin` gains `title_font` / `button_font`). The engine's own
  look is now `ui::default_theme()`. `samples/ui_screens` has a day and a night scene. MCP's `asset_create`
  handles `.mcuitheme`.
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
- UI documents as data, M-UI-DATA step 1 (SBA-31): a `.mcui` asset (JSON) describes a `mce_ui` widget tree
  (anchors, per-state styles, stack and grid layouts, tabs, modals, nav edges, convar bindings, the skin's font and
  sprite sheet by reference) and cooks to a `UiDocument` chunk; `ui::build_document` builds it and
  `ui::extract_document` / `cook::write_ui_document` write a document back, byte-identical after a re-cook. A widget
  starts from the look `ui::Builder` gives its kind. Mistakes fail the cook with `file:line:col`. MCP's
  `asset_create` / `asset_write` handle `.mcui`; the stock player does not load documents yet (step 2).
- UI tools over MCP, M-UI-DATA step 5 (SBA-35): `ui_list`, `ui_create`, `ui_widget_add`, `ui_widget_set` and
  `ui_widget_remove` edit `.mcui` documents by widget path through the cook's reader and writer (a bad edit fails with
  the cook's message and writes nothing; documents are saved canonically and round-trip unchanged), and `ui_preview`
  returns the real player's frame of one document at a viewport, safe area and UI scale (`mce_player -ui_preview
  <guid>`: the document alone over the clear color). `ui_theme_set` follows the themes (SBA-33).
- UI screens in the stock player, M-UI-DATA step 2 (SBA-32): a `UiScreen` component names a `.mcui` document a scene
  shows (HUD, title, pause, win); `mce_player` builds it, draws it over the game, feeds it the pointer and, while a
  modal screen is up, the `"UI"` action map. Gameplay stays behind the UI wall: it toggles `UiScreen::visible`,
  widgets show fields through `"bind": "Component/field"` (+ `"format"`), and a widget's `"action"` raises that
  input action in the next tick's `InputState` exactly as an `-input` script line would, so scripts replay runs
  with the same hash (`ui::ActionRelay`, `ui::bind_document` in `mce/ui/screen_link.h`). With screens in a scene,
  Esc and the corner tap are the game's `Pause`. `samples/ui_screens` is a HUD and a pause menu with no host code.
  The `UiDocument` chunk is version 2 (re-cook `.mcui` assets).
- Text with a look (M-UI-DATA step 4, SBA-34): `TextLabel` gains `letter_spacing`, `line_height`,
  `outline_width`/`outline_color` and `shadow_offset`/`shadow_color` (fractions of the label's height, editable
  through MCP `component_set`), and `ui::Style` gains `letter_spacing`, `text_outline`/`text_outline_color` and
  `text_shadow`/`text_shadow_color`. `render::TextLayoutDesc::letter_spacing` and the outline and shadow fields of
  `render::TextPlacement` do the work for both. A font sidecar with `"sdf": true` (and an optional `"spread"`)
  bakes a signed distance field instead of coverage, so text drawn far above its baked size keeps a sharp edge on
  every backend.
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
