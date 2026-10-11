# Undelivered

The engine's narrative showcase: a light novel on the 2D kit. Kuzgun Island, 1994, the last week
of the ferry. Deniz, nineteen, comes back to pack up the post office their grandmother ran for
forty years and finds, in a locked drawer, seven letters she never delivered. Six are for people
still on the island. One is for Deniz. Seven days, six deliveries, five endings, about forty
minutes a run. Design: [docs/design/undelivered.md](../../docs/design/undelivered.md).

Everything in it is the project's own: the script (`story/*.story`, 14,700 words, compiled to a
table by `tools/compile_story.py`), the paper cut-out art and the sound (`tools/gen_assets.py`),
the scene (authored through the editor MCP server by `tools/author_scene_mcp.py`). The fonts are
OFL (CREDITS.md).

## Play

Inside the engine tree:

    cmake --preset linux-gcc-debug
    cmake --build --preset linux-gcc-debug --target undelivered
    build/linux-gcc-debug/projects/Undelivered/undelivered

With the installed SDK (user guide chapter 1): `mce example Undelivered` copies it out, then
`mce build Undelivered`, `mce play Undelivered`,
`mce editor Undelivered`, `mce mcp Undelivered`, `mce ship Undelivered --version 1.0`.

The stock player's default input map (the game adds no keys of its own):

| Action | Keys | Gamepad |
|---|---|---|
| Advance a page, confirm | Enter, Space, J, left mouse | A, X |
| Choose, move between envelopes | arrows / WASD | d-pad, left stick |
| Skip read text | hold J or left mouse | hold X |
| Backlog (open / scroll / close) | mouse wheel up, then up and down; wheel down or Enter closes | - |
| Leave the drawer shut today | mouse wheel down on the drawer screen | - |
| Pause, quit | Esc (the player's overlay), then Q | Start |

Saves: the game writes `undelivered_autosave` at every night journal and the player writes it
again on a clean quit; *Continue* on the title picks it up. `-save <name>` plays another slot,
`-newgame` is the title's *New game*. Useful flags: `-headless 1 -ticks N` (no window; prints
`N ticks, E entities, state_hash H`), `-frames N -capture shot.ppm` (quit after N frames and write
the last one).

## Layout

| Path | What |
|---|---|
| `Undelivered.mceproj` | project identity: name, engine version, games, start scene |
| `story/*.story` | the script: Day 1, the day loop (mornings, the drawer, afternoons, nights), the seven letters and six deliveries, the afternoons, Day 7 and the endings; `story/REPORT.md` is the compiler's word count |
| `tools/compile_story.py` | the script compiler: wraps and paginates with the cooked font's metrics (Pillow), checks labels, reachability and the five endings, writes `games/undelivered/module/story_data.h` |
| `tools/gen_assets.py` | regenerates `assets/textures` and `assets/audio` (deterministic; about a minute) |
| `tools/author_scene_mcp.py` | re-authors `assets/scenes/main.mcscene` through `undelivered_mcp` (`--mcp build/.../undelivered_mcp`) |
| `assets/scenes/main.mcscene` | the stage: 79 entities, every one a `UiPart` (paper layers, cast slots, box, rows, letter card, panel, envelopes, lights, sound beds, the camera, the save carrier) |
| `games/undelivered/module/` | `components.h` (`UiPart`, `NovelSave`), `undelivered.cpp` (the story VM, the bot, input, presentation, the autosave), `story_data.h` (generated) |
| `games/undelivered/host/main.cpp` | the stock player with this game's module and pack; adds `-save undelivered_autosave` to windowed runs |
| `tests/test_undelivered.cpp` | the script tables, the authored scene, a player's inputs, the five bot policies to their endings, a save reloaded mid-run, determinism |

## How it is built

- The script is data. `compile_story.py` turns `story/*.story` (a small line-oriented format:
  `@scene`, `Speaker(expr): text`, `> "option" [gate] {tag} -> label`, `@if`, `@set`, `@letter`,
  `@drawer`, `@journal`, `@ending`) into one `Op` table, one string table and one option table,
  pre-wrapped into pages of four lines of at most 62 bytes, because `TextLabel.text` is a
  `FixedString<64>` and the runtime never measures text.
- The VM (`undelivered.cpp`) runs that table over one `NovelSave` component: trust per character,
  delivered/read bitmasks, 32 flags, 17 counters, the program counter and a four-deep call stack,
  and the picture (location, hour, LUT, music, cast slots). Every field carries the `Save` flag,
  so the engine's save game *is* the run: `save::write_save` at the journal, the player's `-save`
  load at start, *Continue* on the title.
- The presentation writes into the scene's `UiPart` entities each tick: the five paper layers of
  the location (lit sprites with one shared paper normal map, an ambient light and a lamp that move
  with the hour), the cast with a speaking-character lift and the others dimmed, the typewriter on
  the four lines, choice rows with gated options greyed, the letter card in the handwriting font,
  the panel for the journal, the backlog, the ending card and the title's *Letters* page, the
  drawer's envelopes and stamps, the LUT on the camera's `Post2DComponent`, and the two looping
  `AudioSource`s whose clips it swaps.
- The bot (`NovelState::autopilot`) plays whole runs by option tag (`first`, `leave`, `stay`,
  `shut`, `write`), which is how the test reaches all five endings headless, in a few seconds.
