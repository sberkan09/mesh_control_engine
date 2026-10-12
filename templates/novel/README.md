# @TITLE@

A visual novel on the MCE novel kit (`mce/kit/novel.h`), made with `mce new @PROJECT@ --template novel`.

- **Write** the story in `story/main.story` (add more `.story` files beside it). Every name the
  script uses (characters, locations, flags, endings) is declared in `story/story.json`, so a typo
  fails the build, not the game.
- **Build** with `mce build .`: the build compiles the story into
  `games/@GAME@/module/story_data.h` and writes `story/REPORT.md` (words, pages, reachable endings).
  `mce story compile . --check` only checks it.
- **Play** with `mce play .`. `-novel.autoplay 1` lets the bot play, `-novel.label <label>` starts
  at a label, `-novel.instant 1` turns the typewriter off. `mce headless . -- -novel.autoplay 1`
  plays the whole story without a window.
- **Art**: backgrounds are `assets/textures/bg/<location>_<layer>_<note>.png` (layers 0 to 4, back to
  front), sprites `assets/textures/cast/<who>_<expression>_<pose>.png`, music and ambience
  `assets/audio/music/<id>.wav` and `assets/audio/ambience/<id>.wav`, sounds `assets/audio/sfx/<id>.wav`.
- **Your own screens** (a map, an inventory, a puzzle): declare a command in story.json
  `"commands"`, write `@mycommand` in the script and handle it in `NovelHooks::command`
  (`games/@GAME@/module/@GAME@.cpp`). The Undelivered example's drawer is one.

The user guide's "Novels and dialogue" chapter has the whole script syntax.
