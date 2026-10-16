# @PROJECT@

A script game: Luau scripts and data, no C++ and no compiler. Made with `mce new @PROJECT@ --template script`.

| Path | What |
|---|---|
| `@PROJECT@.mceproj` | project identity: name, engine version, start scene |
| `assets/scenes/main.mcscene` | the scene; a `ScriptSystem` entity names each script it runs |
| `assets/scripts/*.luau` | the game's code (user guide chapter 27): `update()` runs every tick |
| `assets/textures/` | art |
| `CREDITS.md` | third-party content; `mce ship` copies it into the shipped folder |

With the installed SDK (`mce` on your PATH):

    mce build @PROJECT@                 # cook assets/ into @PROJECT@/cooked and @PROJECT@/assets.mce (checks the scripts)
    mce play @PROJECT@                  # play it (the stock player; Esc pauses, ` opens the console)
    mce headless @PROJECT@              # run without a window; prints the state hash
    mce editor @PROJECT@                # the editor (play-in-editor reloads a script you save)
    mce mcp @PROJECT@                   # the MCP server for an AI agent (script_check checks a script)
    mce ship @PROJECT@ --version 1.0    # the ship zip in @PROJECT@/dist/

Scripts are checked when they are cooked: a misspelt component, field or `mce` function is an error at
`file:line:col` with the closest name. `cooked/mce.d.luau` describes the `mce` API for an editor with a
Luau language server (luau-lsp).
