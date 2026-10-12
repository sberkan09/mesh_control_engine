# M-NOVEL — Visual-novel kit (milestone record)

**Status:** done (2026-10-12). Plan: ROADMAP §M-NOVEL; issues #115 (the kit, the compiler, the template) and #154
(NPC dialogue on the same VM).
**Why:** a second light novel had to copy about 2,000 lines of Undelivered's game code (story VM, stage driver,
script compiler); every further novel would carry the same copy and miss its fixes, and other games had no
branching dialogue without an Ink or Yarn importer.

## Steps

Each step was one PR into `dev_claude`, verified with `scripts/ci/local_ci.sh` (walls, linux-gcc-debug,
linux-clang-release, linux-gcc-release, linux-asan, tidy, determinism) before merge.

| # | Step | PR |
|---|---|---|
| 1 | `mce_kit_novel` (`mce/kit/novel.h`), `mce story compile` (`tools/story/story_compile.py`), Undelivered ported onto both, the dialogue VM | #169 |
| 2 | `mce new <name> --template novel`, `mce_add_project(... STORY)`, the novel template probe and SDK test | #170 |
| 3 | The NPC dialogue box on `mce_ui` (`mce_kit_novel_ui`), game UI layers in the stock player, the sim-side typewriter | step 3 PR |
| 4 | Docs: user guide chapter 21, this record, ROADMAP status | docs PR |

## What shipped

- **The kit** (`src/engine/kit_novel`, opt-in like `mce_net`): an UMBRELLA static library that is not in
  `MCE_ENGINE_LIBS`, is guarded by a walls check and is installed with the SDK as `mce::mce_kit_novel`.
  - It provides the story VM (pages, choices with gates, calls, flags and counters), the title menu, letters,
    checkpoints with the autosave, endings, the backlog, fades, music and ambience beds, cast slots by time of day,
    LUTs and the playthrough bot.
  - Its save is one Save-flagged component, `StoryState`.
  - Games add their own screens through `NovelHooks` (commands, input, bot, look, panels, HUD).
  - Hosts take the `novel.*` flags from `mce/kit/novel_host.h`.
- **The compiler** reads its id tables from `story/story.json` and fixes the gaps found writing a 10k-word script:
  - no `.story` file falls through into the next one;
  - `@lettertext` compiles outside the flow;
  - `Note: …` is narration, with a warning on a near-miss name;
  - conditions take `and`/`or` and the comparison operators;
  - stage checks catch an occupied slot and a character in two slots.

  It adds trailing `#tags`, game `commands` and `story/REPORT.md`. The Python test `story_compile` has one case
  per gap.
- **Undelivered** runs on the kit. Its module is its tables, its hooks and its drawer, which is one game command:
  267 lines, down from 1,113. Its five-ending bot test is unchanged and passes.
- **The template.** `templates/novel` is an overlay on `templates/project`: a one-page story, the StoryState
  carrier, art by the kit's path conventions, and a host with the flags.
  - `mce_add_project(... STORY)` recompiles the story on every build.
  - The configure-time probe builds a novel and plays it to `@end`.
  - `tests/sdk` stamps, builds, plays and ships one from the installed SDK.
- **NPC dialogue (#154).**
  - `install_dialogue` and the `Dialogue` component run a conversation from a label outside a novel stage.
    Options are gated by variables that game systems read and write through the world's `StoryState`. Page
    `#tags` carry speaker, portrait and sfx hints.
  - Every field is saved, so a save in the middle of a conversation resumes on the same line
    (`tests/kit_novel/test_novel`).
  - The typewriter runs on sim ticks: Enter shows the rest of a page, then advances.
  - `mce_kit_novel_ui` (`mce/kit/novel_ui.h`) is the default dialogue box and choice list on `mce_ui`. A host adds
    it as a `ui::Layer` through `PlayerDesc::ui_layers` (`HOST_DEPS mce::mce_kit_novel_ui`), so game modules
    still never include `mce/ui`.
  - `tests/kit_novel/test_novel_ui` checks the box headless, and `tests/sdk` builds a host with it.

## Gates (ROADMAP)

- Undelivered's module shrinks to its tables, its hooks and its drawer: met (#169).
- `tests/sdk` builds the novel template: met (#170). It also plays the template to `@end` and ships it.
- Compiler tests cover each fixed gap: met (`tests/kit_novel/test_story_compile.py`).

## Not done

- **Localization.** Dialogue and novel lines are not routed through localization tables yet; that waits on
  M-MOBILE step 11.
- **No GPU run of the dialogue box.** The box's GPU path (the player's screen-space pass) builds and links in
  every preset, but this environment has no display. It is checked headless through the `ui::Document` it
  drives; a windowed capture is the first thing to look at on a machine with a GPU.
- **No pointer input.** Options are picked with the stock map (Enter, up/down) in the sim. Taps and clicks on the
  box's rows are not wired, because the box is a view.
