You are making a game with Mesh Control Engine (MCE), an engine you have never seen. No person
will answer questions or review your work: decide everything yourself and keep going until the
game is finished and shipped.

**What you have.** An installed MCE SDK at `/tmp/claude-0/sdk` (its `bin/`, `include/`, `share/`) and a fresh
project at `/tmp/claude-0/demo2/solo-2d/Glowmoth` made with `mce new` and already built once. You reach the engine's MCP
server through a shell bridge, already running for you:

    B="python3 /home/user/mesh_control_engine/scripts/agent_demo/mcp_bridge.py call --sock /tmp/claude-0/demo2/solo-2d/agents/solo/bridge.sock"
    $B scene_info                                   # a tool call: name, then JSON arguments
    $B project_check '{"checks": ["credits"]}'
    $B --method resources/read '{"uri": "mce://docs/index"}'
    $B --method prompts/get '{"name": "start_game", "arguments": {"brief": "..."}}'
    $B --method tools/list                          # every tool with its schema

A tool's images are saved as PNG files and their paths printed (`image: /path/0007.png`); open them
to look. Game code lives in `/tmp/claude-0/demo2/solo-2d/Glowmoth/games/glowmoth/module/` (components and systems); edit it
with your file tools, then `$B module_build` compiles it and loads it into the server.

**Rules.**
- Do not read anything under `/home/user/mesh_control_engine` (the engine's source). The SDK, the MCP server's
  docs (`mce://docs/...`, `docs_search`) and `engine_capabilities` are what you learn from.
- Edit scenes and assets through the MCP tools, not by hand, except game code and `.mctest` files.
- When the engine makes something hard (a missing tool, an unclear error, a wrong doc, a
  workaround you had to invent), call `report_friction` right then, before working around it.
- Look at what you make: `render_frame` / `render_sheet` after each visible change.
- Finish with `project_check` passing and `ship_build` returning a zip. Then write
  `/tmp/claude-0/demo2/solo-2d/SUMMARY.md`: what the game is, how to play it, what you could not do.

Start with `$B --method prompts/get` for the `start_game` prompt with this brief:

A short 2D platformer. You are a moth on a night meadow. Each level hides three lanterns; light
all three to open the way to the moon gate at the end. Bats patrol between the flowers and send
you back to the level's start when they touch you. Two levels, the second one harder than the
first. A title screen, a win screen after the last level and a lose screen after three hits.
Pixel art, a night palette, chiptune music and retro sound effects.