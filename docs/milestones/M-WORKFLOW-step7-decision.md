# M-WORKFLOW step 7: what to do about #382 (decision note)

**Status:** proposal for review, docs only. Step 7 of [M-WORKFLOW](M-WORKFLOW.md). Owners: M-SCRIPT3 (items 1, 2 and 4b; its
step 5 already lists name lookup, several components per call and cutscenes), M-UI-DATA / kit (items 3, 4a and 6), M-ACCESS and
the player (item 5). **Constraint:** M-SCRIPT3 step 2 ([design](M-SCRIPT3-step2-design.md)) is under way in the script VM
(`script.cpp`), the compiler, the script importer and `api_world.cpp`. Every item says whether it touches those files. "VM-free"
means it touches none of them. Line numbers are from the tree as of 2026-10-17. Anything I did not check in the code is marked *inferred*.

## 1. Luau has no entity lookup by name

**Today.** The cook keeps entity names: `scene_importer.cpp:433` stores each `"name"`, `:721-729` writes the name table and
`SceneView::entity_name` reads it (`formats.h:1724`, `formats.cpp:1482`). Nothing calls it at run time. `scene::instantiate`
(`scene.cpp:37-100`) spawns the entities and drops the names. `kWorldLib` (`api_world.cpp:1011-1069`) and `kMceLib`
(`script.cpp:541-543`) have no lookup, so scripts compare a `UiScreen.document` guid instead.
**Options.** (A) `instantiate` fills a `scene::EntityNames` resource (sorted name hash to Entity, plus the source scene) from
the cooked names, and a binding `mce.find(name) -> entity or nil` reads it. (B) A reflected `kit::Name {FixedString<32>}`
component that authors add, found with `mce.each("Name", ...)`. This needs no binding at all, but every name has to be written twice.
**Recommend A.** The names are already in every scene. A resource adds no column, so existing hashes stay the same (*inferred*:
`state_hash` covers components only; check that in the PR). Prefab instances and `mce.spawn` entities have no name, and the
lookup returns only live entities (`world.alive`). Clear the resource in `change_scene` (`scene.cpp:136-150`).
**Size** M. **Touches** the scene module (`scene.cpp`, `scene.h` at 33 TUs) in PR 1a, which is VM-free. PR 1b adds the binding
in a new `api_names.cpp` plus **one registration line in `script.cpp` or `api_world.cpp`**, so it lands after M-SCRIPT3 step 2
merges. **Test:** `tests/scene/test_scene.cpp`: names resolve after load, are cleared after a change, and do not change the hash.
`tests/script/test_script_api.cpp`: `mce.find("Player")`, and nil for an unknown name.

## 2. `mce.set` takes one component per call; no state between ticks

**Today.** Part of this is stale. `l_get` and `l_set` (`script.cpp:404-455`) loop over `(field, value)` pairs and fetch the row
again whenever the component changes, so `mce.set(e, "A", "x", 1, "B", "y", 2)` already works. Guide 27 (lines 63-64) says
"one or more fields" but never says the fields may belong to different components. No test uses two components (checked with grep in
`tests/script`). "No state between ticks" is the rule (`script.h:11-14`, guide 27:167). `kit::ScriptVars` (16 floats per entity,
`components.h:365-373`) is where that state goes.
**Options.** (A) Docs plus a test only. (B) Add a table form, `mce.set(e, {A = {x = 1}, B = {y = 2}})`.
**Recommend A.** B is new API in `script.cpp`, which is contended, and it saves little. Point the guide's Luau-limits section
(M-SCRIPT3 step 6) at the mixed-component call and at `ScriptVars`. **Size** S. **Touches** docs and `tests/script` only.
The VM is untouched, but the test file sits beside the other thread's tests, so expect a trivial rebase.
**Test:** `test_script_api.cpp`: one `mce.get` / `mce.set` call over `LocalTransform` and `ScriptVars`.

## 3. `UiFlow` takes one action per hide/show/load and cannot react to signals or `GameState`

**Today.** `UiFlow` holds one `FixedString<32>` each for `show_on`, `hide_on`, `load_on` and `quit_on` (`components.h:138-152`).
`kit_ui_flow` (`stock.cpp:61-82`) reads only `InputState` actions (`pressed`, `:55-59`). The engine has no `GameState`: it is the
game's own component or resource (`samples/03_platformer/game/platformer.h:162`, guide 06:214). Signals are events:
`SeqSignal` and `TimelineSignal` are produced in Post (`sequence.cpp:389`, `timeline.cpp:495`), `BtSignal` in Pre (`ai.cpp:564`).
**Options.** (A) Widen `UiFlow`: more action slots plus `show_when "Comp/field"`. This changes the layout of a component every
UI scene bakes. (B) Add a component `kit::UiFlowRules` in a new header `kit/ui_flow.h`. It holds a fixed array of 8 rules
`{when: Action|Signal|FieldEquals|FieldAbove, name, field "Comp/field", value f32, do: Show|Hide|Toggle|Load|Quit|Pause, scene}`.
Rules are evaluated in `kit_ui_flow` after `UiFlow`. A field is read from the first entity that has the component, as widget
`bind` does (`components.h:111-113`). Signals are read with `f.events().read<...>()`, so a Post signal acts the next tick
(*inferred* from the double-buffer swap, `world.h:145`).
**Recommend B.** `UiFlow` and every baked scene stay as they are, a HUD gate becomes `{FieldEquals, "GameState/phase", 1, Show}`
and an end screen `{Signal, "lost", Show}`. Fixed arrays of structs already import (`TimelinePlayer.bindings`). **Size** M.
**Touches** kit only (`ui_flow.h` new, `stock.cpp`), plus MCP schema docs. VM-free. **Test:** `tests/kit/test_ui_flow.cpp`:
a signal shows a screen the next tick, a field change hides it, two actions on one rule, a held (paused) tick still evaluates the
rules, and an existing `UiFlow`-only world hashes the same as before.

## 4. `pause: true` silences audio and holds `SfxTrigger`; cutscenes hold Luau `ScriptSystem`

**Today.** A pause hold runs only `run_when_held` systems (`world.cpp:476`). Only `kit_ui_flow` is marked
(`stock.cpp:89`). `audio_bridge` (`bridge.cpp:86-87`) and `kit_effects`, which owns `SfxTrigger` (`effects.cpp:400`), are marked
`run_during_cutscenes` only. While a title or pause screen shows, no `PlaySfx` and no `AudioSource` start. UiFlow never pauses the
device (`AudioDevice::set_paused` is called only for ads and the background, `player.cpp:1624, 2206`), so voices that already
started keep playing (*inferred*). The cutscene hold (`cinematic.cpp:125`) skips `script.pre` and `script.post`, which are not
marked (`scene_scripts.cpp:127-128`).
**4a options.** (A) A third hold class in `World` (`world.h` reaches 325 TUs: a seam PR and a full rebuild). (B) A `UiFlow` flag
`keep_audio` (default false) in the existing `pad_` (`components.h:145`). `kit_ui_flow` writes `audio::PauseMix{keep}`, a new small
resource in the audio module. `audio_bridge` becomes `run_when_held` and returns early when `paused() && !keep`. `SfxTrigger` moves
out of `kit_effects` into its own `kit_sfx` system, `run_when_held` with the same gate.
**Recommend B.** The default keeps today's behaviour, so the gate is that the replay hashes do not change. A new system bumps
`change_tick_` once per run (`world.cpp:482`), so a determinism run must confirm the hashes are unchanged. **Size** M, Opus.
**Touches** audio (`bridge.cpp`, a resource header) in PR 4a-1 and kit (`stock.cpp`, `effects.cpp`, `components.h` at 119 TUs) in
PR 4a-2. VM-free. **Test:** `tests/kit/test_ui_flow.cpp`: with `keep_audio` a Manual `SfxTrigger` on the visible screen plays on a
held tick and gameplay does not move. Without it, it does not play. Add an `audio_bridge` held-tick case in `tests/audio`.
**4b options.** (A) Mark both script systems `run_during_cutscenes`, so every script runs. This changes gameplay during
cutscenes. (B) Use a `ScriptSystem.during_cutscenes` flag in its `pad_` (`components.h:359`). `scene_scripts.cpp` loads flagged
scripts into group `phase | 2` (the host already filters by group, `script.cpp:728`). Mark both systems `run_during_cutscenes`.
On a cutscene-held tick run only group `phase | 2`; otherwise run `phase`, then `phase | 2`.
**Recommend B.** One caveat: flagged scripts run after the unflagged ones in their phase. That is acceptable for new data, but the
guide must say so, and section 13.6 must say what runs during a cutscene. **Size** S. **Touches** `scene_scripts.cpp` (script module,
not the VM, the importer or `api_world.cpp`) and the `components.h` pad byte. It is VM-free but shares the module, so tell the
step 2 thread. **Test:** `tests/script/test_scene_scripts.cpp`: a flagged script ticks under `set_cutscene_held(true)`, an unflagged
one does not, and neither runs under a pause hold.

## 5. Subtitles and the skip prompt use the engine pixel font; no way to ship convar defaults; UI too small at 1080p

**Today.** The player draws subtitles and the skip prompt with the built-in skin (`player.cpp:523-525`, `:1874`,
`skin_.font.face`). The skip prompt scales by `floor(h/360)` (`:1868`). Subtitles use `text_px 22 * ui_scale`
(`subtitles.h:35`). `compute_ui_scale` (`ui.cpp:153-163`) is `display_scale * ui_scale`, clamped only downward, so a 1080p monitor
at display scale 1 gets 1.0 (pinned by `test_ui_layout.cpp:114`). `access.captions` defaults to false (`subtitles.cpp:12`). A
command-line value wins over `settings.cfg` and is never saved (`convar.h:145-151`), so a launcher flag (`mce.py:428-432`) would
lock the player out of the option, and no defaults file exists (`player.cpp:498-511`).
**Options.** (a) Font: `SubtitleView` takes its font from a `.mcuitheme` named by a convar or the start scene (`UiScreen.theme`
already exists, `components.h:125`). (b) Defaults: `console::set_defaults(path)` rewrites `default_` (`convar.h:89`) from a
`defaults.cfg` that the cook packs from `.mceproj "convars"`, applied before `load_settings`, so the player's saved choice still
wins. (c) Scale: a `ui.reference_height` convar (0 = off, default), with `s *= max(1, h / ref)`, shipped through (b) as 720.
**Recommend** all three as separate PRs. Each default stays as it is, so the goldens do not move. **Size** S (a), M (b, `convar.h` at
141 TUs, near the 150 seam line), S (c). **Touches** the player and ui (`player.cpp`, `subtitles.cpp`), core plus cook plus
`mce.py`, and ui (`ui.cpp`). VM-free. **Test:** `test_subtitles.cpp` (the theme font is used), `test_ui_layout.cpp` (1920x1080
with ref 720 gives 1.5, ref 0 is unchanged), and a core convar test (a default from the file, a `settings.cfg` value over it, the
default not written back).

## 6. Retry / ToTitle reload `main.mcscene`, so the title shows again after a loss

**Today.** Undertow puts the title (`UiFlow {hide_on: Play, pause: true}`, visible) and the `load_on: ToTitle` pause menu in one
scene (`projects/Undertow/assets/scenes/main.mcscene:5984-5992`). Guide 12:203 recommends `load_on` loading "the scene itself".
`change_scene` destroys every entity and reloads (`scene.cpp:143-145`), so the title's `visible: true` comes back.
`kit_ui_flow` drops the hold on a load (`stock.cpp:76-78`). Nothing carries why the scene was loaded.
**Options.** (A) Authoring: split `title.mcscene` from the level, so Retry loads the level and ToTitle loads the title, and fix
guide 12:203 and the MCP prompt. (B) Add `SceneChange.entry` (`scene.h:44`). `UiFlow.load_on` and `UiFlowRules` Load pass their action as the
entry, and a screen whose `hide_on` equals the entry starts hidden.
**Recommend A now**, VM-free, docs plus a sample. Keep B as a follow-up only if a one-scene game asks for it. **Size** S.
**Test:** `test_ui_flow.cpp`: Retry loads the level scene with no title screen (two-scene fixture), and `check_docs.py` passes.

## Recommended order (one module per PR; stack seam, then feature, then polish)

| PR | What | Module | VM / importer / api_world? | Model |
|---|---|---|---|---|
| 1 | Item 2 docs and mixed-component test, item 6 guide fix and split scene | docs, tests/script, sample | no | Sonnet |
| 2 | Item 5c `ui.reference_height` | ui | no | Sonnet |
| 3 | Item 5a subtitle and skip-prompt font from a theme | player / ui | no | Sonnet |
| 4 | Item 5b `set_defaults` + `defaults.cfg` cooked from `.mceproj` | core, then cook + cli (split if cook grows) | no | Opus |
| 5 | Item 3 `UiFlowRules` (new header, `stock.cpp`) | kit | no | Opus |
| 6 | Item 4a-1 `audio::PauseMix`, `audio_bridge` held-tick gate | audio | no | Opus |
| 7 | Item 4a-2 `UiFlow.keep_audio`, `kit_sfx` split | kit | no | Opus |
| 8 | Item 4b `ScriptSystem.during_cutscenes` (group trick) | script (`scene_scripts.cpp` only) | no, same module: after step 2 or coordinated | Sonnet |
| 9 | Item 1a `scene::EntityNames` | scene | no | Sonnet |
| 10 | Item 1b `mce.find` binding (`api_names.cpp` + one registration line) | script | **yes, after M-SCRIPT3 step 2 merges** | Sonnet |

PRs 1 to 7 and 9 can run beside M-SCRIPT3 step 2. PRs 5 and 7 both edit `kit_ui_flow`, so run them in sequence, not in parallel.
PR 8 waits for step 2 or a note to that thread. PR 10 waits for step 2. Gates on every PR: the replay hashes stay the same, there is
a regression test, and `local_ci.sh --task` is green.
