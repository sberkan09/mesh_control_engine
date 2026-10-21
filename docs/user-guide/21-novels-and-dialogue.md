# 21. Novels and dialogue (optional)

The visual-novel kit, `mce_kit_novel` (`mce/kit/novel.h`), is **opt-in** like networking. It does two
jobs with one story VM:

- **a visual novel**: title menu, pages typed out, choices, letters, checkpoints with an autosave,
  endings, a backlog, backgrounds and sprites by time of day, music and ambience, and a bot that
  plays the story through for tests;
- **NPC dialogue in any game**: a conversation runs on an entity from a label, its options can be
  gated by variables your game systems read and write, and it is saved with the game.

The script is plain text compiled ahead of time by `mce story compile` into a C++ header, so a typo
in a name fails the build, not the game. Undelivered (`mce example Undelivered`) is the full-size
reference: five endings, about 15,000 words, its own drawer screen.

## 21.1 Start a novel

```sh
mce new MyNovel --template novel
mce build MyNovel
mce play MyNovel                              # New game / Continue on the title screen
mce headless MyNovel --ticks 2000 -- -novel.autoplay 1   # the bot plays it to @end
```

The project holds:

| Path | What |
|---|---|
| `story/story.json` | the manifest: every name the script may use |
| `story/*.story` | the script |
| `story/REPORT.md` | written by the compiler: words, pages, choices, endings reached, unreachable labels |
| `games/<game>/module/story_data.h` | the compiled program (generated; do not edit) |
| `games/<game>/module/<game>.cpp` | installs the kit with the program and your hooks |
| `assets/textures/bg/`, `assets/textures/cast/` | backgrounds and sprites, found by name |

`mce_add_project(... STORY DEPS mce::mce_kit_novel)` in the project's `CMakeLists.txt` makes the
build recompile the story whenever a `.story` file or the manifest changes. `mce story compile <project>`
runs the compiler by hand; `--check` only validates, `--werror` fails on warnings.

## 21.2 The manifest

```json
{
  "format": "mce-story", "version": 1,
  "namespace": "my_novel::story",
  "output": "games/my_novel/module/story_data.h",
  "title": "My Novel",
  "characters": ["mira", {"id": "harbour_master", "name": "Yusuf Reis"}],
  "cast": ["mira"],
  "expressions": ["neutral", "warm"], "poses": ["stand"], "slots": ["left", "center", "right"],
  "locations": ["pier"], "times": ["day", "evening"], "luts": ["neutral"],
  "music": ["harbour"], "ambience": ["sea"], "sfx": ["page"], "timelines": ["harbour_intro"],
  "endings": [{"id": "open_water", "title": "Open Water"}],
  "tags": ["first", "kind"], "bot": {"kind": ["kind", "first"]},
  "flags": ["met_mira"], "counters": [{"name": "trust.mira", "min": -3, "max": 6}],
  "commands": ["map"],
  "title_screen": {"location": "pier", "time": "evening", "music": "harbour"},
  "text": {"font": "assets/fonts/MyFont.ttf", "px": 32, "line_px": 860, "lines_per_page": 4}
}
```

The compiler's docstring (`tools/story/story_compile.py`, or `mce story compile --help`) lists every
key. Art and sound are found by path, so the names here are the file names:

| Kind | Path |
|---|---|
| background layer | `textures/bg/<location>_<layer 0-4>[_<note>].png`, back to front |
| sprite | `textures/cast/<character>_<expression>_<pose>.png` |
| colour grade | `textures/lut_<lut>.png` |
| music / ambience / sound | `audio/music/<id>.wav`, `audio/ambience/<id>.wav`, `audio/sfx/<id>.wav` |
| cutscene | `timelines/<id>.mctimeline` (13.5) |

## 21.3 The script

One statement per line; a paragraph is one page.

```text
# a comment
@scene start bg=pier time=evening music=harbour
@show mira center warm
Mira: You came back.
Mira(neutral): The boat leaves at dawn. #portrait=neutral
The wind picks up.                       (no speaker: narration)
Note: the tide is turning.               (narration too: Note is not a character)
Mira:
  > "Ask about the boat" -> boat
  > "Give her the letter" [has_letter] -> letter          (greyed out unless has_letter)
  >? "Tell her the truth" [trust.mira >= 3 and has_letter] -> truth   (hidden unless it holds)
  > "Wave" {kind} -> wave                                  (a tag the bot can prefer)

@label boat
@set trust.mira +1
@flag met_mira
@if trust.mira >= 2 or met_mira -> warm
@jump done
```

| Statement | Does |
|---|---|
| `@scene <label> [bg= time= lut= music= ambience=]` | a label that also sets the stage |
| `@label`, `@jump`, `@call`, `@return`, `@end` | flow; every file ends with `@end`, `@jump` or `@return` |
| `@bg`, `@music`, `@ambience`, `@sfx` | the stage's look and sound |
| `@show <char> <slot> [expr] [pose]`, `@hide <char>\|all`, `@expr`, `@pose` | the cast |
| `@set <var> +n\|-n\|=n`, `@flag`, `@unflag`, `@if <condition> -> label` | variables |
| `@letter <n>` … `@lettertext <n>` … `@endletter` | a letter read on its own card |
| `@checkpoint` (or `@journal`) | autosave and the checkpoint panel |
| `@ending <id>` | marks an ending reached |
| `@fade`, `@wait <seconds>` | pacing |
| `@cutscene <timeline>` | plays a timeline from `"timelines"` as a cutscene (13.6: letterbox, its subtitles, hold Esc to skip), then goes on; the bot skips it. Its tracks drive the playing entity (`"self"`): sounds, events, subtitles |
| `@<command> [args]` | a game command declared in `"commands"` (21.5) |

Conditions are `flag`, `!flag`, `var >= n` (also `<= == != > <`), joined by `and` and `or`; `and`
binds tighter. A `Name:` prefix is a speaker only when `Name` is a declared character (its id, or the
first word of its display name); anything else is narration, with a warning when it looks like a
misspelt character. The compiler also checks the stage along straight-line flow (an `@show` into a
slot someone else holds, one character in two slots), labels, variables, and whether every ending is
reachable from `start`.

## 21.4 Playing and testing

The host (`games/<game>/host/main.cpp`) includes `mce/kit/novel_host.h`, which declares the kit's
flags and the autosave slot:

| Flag | Does |
|---|---|
| `-novel.autoplay N` | the bot plays: 1 + the index of a tag in `"tags"` picks options by that tag's `"bot"` row |
| `-novel.label <label>` | start a new run at a label (screenshots, authoring) |
| `-novel.skip N` | with `-novel.label`: advance N pages first |
| `-novel.instant 1` | whole pages, no typewriter |

A test installs the game, sets `NovelState::autopilot` and ticks until `NovelState::finished`;
`NovelState::ending` names the ending reached. Undelivered's test drives all five endings that way.

## 21.5 Your own screens

`novel::install(world, story::kProgram, hooks)` takes `NovelHooks`, all optional:

- `command`: an `@<command>` from the script. Return true to own the screen (`Mode::Game`) until your
  code calls `novel::resume(c)`.
- `game_input`, `game_bot`: your screen's input and the bot's turn while you own it.
- `screen`, `present`: change the stage's look before it is drawn; draw your own parts after.
- `row`, `panel_line`, `hud`: the choice rows on your screen, extra panel lines, a HUD line.
- `new_run`: reset your own state when a run starts.

Undelivered's drawer (envelopes to sort, stamps to read) is one `@drawer` command and these hooks.

The stage finds its parts in the scene by a `StagePart` component: a `role` (`Background`, `Cast`,
`Box`, `NameText`, `Line`, `ChoiceRow`, `Music`, ... in `novel_components.h`) and an `index` for
roles with several parts. Moving, restyling or replacing a part in the editor or over MCP changes
the look without code; the stock editor and `mce_mcp` know these components.

## 21.6 NPC dialogue in any game

A game with no novel stage installs only the dialogue VM:

```cpp
#include "mce/kit/novel.h"
#include "story_data.h"

novel::install_dialogue(world, story::kProgram);
// on the NPC entity
world.add(npc, novel::Dialogue{});
world.add(npc, novel::DialogueInput{});   // the stock map drives it: Enter, up/down
// when the player talks to it
novel::start_dialogue(world, npc, "mira");          // or push a DialogueStart event from a system
```

The conversation runs from the label until `@end` or a top-level `@return`. `Dialogue` holds the
page (`speaker`, `expression`, `line[]`, the page's `#tags`), the visible options while a choice
waits, and the highlight. Stage statements are skipped and `@sfx` plays. The variables are the
world's `StoryState`, so your systems read and write the same flags and counters
(`novel::dialogue_vars(world)`, `get_var`, `set_var`, the generated `story::var::` ids). Every field
is in the save, so a save in the middle of a conversation resumes on the same line.

The text is typed out on sim ticks (45 code points a second; `set_dialogue_speed`, 0 for whole
pages): Enter first shows the rest of the page, then advances.

**The dialogue box.** `mce_kit_novel_ui` (`mce/kit/novel_ui.h`) draws a default box and choice list
on `mce_ui`. The host adds it to the stock player; game code never includes `mce/ui`:

```cmake
mce_add_project(my_game ... DEPS mce::mce_kit_novel HOST_DEPS mce::mce_kit_novel_ui)
```

```cpp
// games/my_game/host/main.cpp
#include "mce/kit/novel_ui.h"
...
mce::novel::DialogueBox box;
const mce::ui::Layer* layers[] = {box.layer()};
desc.ui_layers = layers;
desc.ui_layer_count = 1;
return mce::player::run(argc, argv, desc);
```

It shows the conversation on the `DialogueInput` entity: the speaker's name, the page as far as it
has been typed, and the options with the highlighted one marked and gated ones greyed. Colours are
a `DialogueBoxStyle`; the text size and box height are the world's `novel::DialogueLayout`
(`set_dialogue_layout`), because the sim hit-tests the same rectangles. Any `ui::Layer` you write
is added the same way.

### Taps and clicks

A phone plays both the novel stage and the dialogue box by touch, and a desktop by mouse. A tap on
a choice row (or a title menu row) picks that row; a tap anywhere else reads on, and while a
choice waits it does nothing. On the dialogue box a tap on the box first shows the rest of the
page, then advances. Taps reach the sim through the input snapshot (the first finger down, or the
stock map's `Point` action at the mouse), so replays and headless runs see them, and a pick plays
the `Selection` haptic. Your own maps add a `Point` action to get mouse clicks.

### Translating the story

Add `"localize": {"base": "en"}` to the manifest. Each compile then writes the story's rows into
`assets/loc/en.csv`, keeping the rows your game wrote there:

| Key | Row |
|---|---|
| `story.title` | the title screen's name (when the manifest sets `"title"`) |
| `story.extras` | the title menu's third row (when the manifest sets `"extras"`) |
| `story.ending.<id>` | an ending's title (the ending card and the extras page) |
| `story.name.<character>` | a speaker's name tag |
| `story.<8 hex digits>` | a statement or an option, keyed by its text |

Copy them into `assets/loc/tr.csv` (or any locale) and translate the text column. Rewording a base
line gives it a new key, so its old translation stops showing until it is redone; the cook's
missing-key warning lists what is left. `mce story compile --check` fails while `en.csv` is out of
date with the script.

The editor's Strings panel (a double click on a `.csv` in Assets) shows all locales side by side, so the
missing translations are one filter away and are edited in place.

The novel stage, the backlog and the dialogue box show the language the player chose (chapter
on languages, `loc::Localization`), and switch with it at once. A translated statement keeps the
page count of its base line, so a playthrough is the same in every language and saves stay valid
across a switch: a translation that cannot fill those pages or overflows them is a compile warning
and shows in the base language. An option takes at most two lines. Letters are not translated yet.

The kit's own words go through the same tables. Add the rows you want to any locale's CSV; a key
no table has shows the English words below, never the key. A `.touch` row is used after the
player read on with a finger, so a phone can say "Dokun" where a keyboard says "Enter":

| Key | English |
|---|---|
| `novel.new_game`, `novel.continue` | New game, Continue (the title menu) |
| `novel.pages_read#one`, `#other` | {count} page read, {count} pages read (the checkpoint) |
| `novel.saved_go_on`, `novel.go_on` (`.touch`) | Saved.  Enter to go on. / Enter to go on. (Tap to go on.) |
| `novel.ending_of` | Ending {n} of {total} |
| `novel.enter` (`.touch`) | Enter (Tap) |
| `novel.ending_unseen` | an ending not yet reached (the extras page) |
| `novel.go_back` (`.touch`) | Enter to go back (Tap to go back) |

A game's own panel hook (`NovelHooks::panel_line`) can look them up with `novel::kit_text` and
`novel::kit_count`.
