# Instructions given to a solo agent

These are the instructions each demo agent started from, word for word apart from the paths
(`{...}`). Nothing else was said to it during the run.

---

You are making a game with Mesh Control Engine (MCE), an engine you have never seen. No person
will answer questions or review your work: decide everything yourself and keep going until the
game is finished and shipped.

**What you have.** An installed MCE SDK at `{sdk}` (its `bin/`, `include/`, `share/`) and a fresh
project at `{project}` made with `mce new` and already built once. You reach the engine's MCP
server through a shell bridge, already running for you:

    B="python3 {bridge} call --sock {sock}"
    $B scene_info                                   # a tool call: name, then JSON arguments
    $B project_check '{"checks": ["credits"]}'
    $B --method resources/read '{"uri": "mce://docs/index"}'
    $B --method prompts/get '{"name": "start_game", "arguments": {"brief": "..."}}'
    $B --method tools/list                          # every tool with its schema

A tool's images are saved as PNG files and their paths printed (`image: /path/0007.png`); open them
to look. Game code lives in `{project}/games/{game}/module/` (components and systems); edit it
with your file tools, then `$B module_build` compiles it and loads it into the server.

**Rules.**
- Do not read anything under `{engine_checkout}` (the engine's source). The SDK, the MCP server's
  docs (`mce://docs/...`, `docs_search`) and `engine_capabilities` are what you learn from.
- Edit scenes and assets through the MCP tools, not by hand, except game code and `.mctest` files.
- When the engine makes something hard (a missing tool, an unclear error, a wrong doc, a
  workaround you had to invent), call `report_friction` right then, before working around it.
- Look at what you make: `render_frame` / `render_sheet` after each visible change.
- Finish with `project_check` passing and `ship_build` returning a zip. Then write
  `{run}/SUMMARY.md`: what the game is, how to play it, what you could not do.

Start with `$B --method prompts/get` for the `start_game` prompt with this brief:

{brief}
