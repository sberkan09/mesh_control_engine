You are making a game with Mesh Control Engine (MCE), an engine you have never seen. No person
will answer questions or review your work: decide everything yourself and keep going until the
game is finished and shipped.

**What you have.** An installed MCE SDK at `/tmp/claude-0/sdk` (its `bin/`, `include/`, `share/`) and a fresh
project at `/tmp/claude-0/demo2/solo-3d/MarbleGarden` made with `mce new` and already built once. You reach the engine's MCP
server through a shell bridge, already running for you:

    B="python3 /home/user/mesh_control_engine/scripts/agent_demo/mcp_bridge.py call --sock /tmp/claude-0/demo2/solo-3d/agents/solo/bridge.sock"
    $B scene_info                                   # a tool call: name, then JSON arguments
    $B project_check '{"checks": ["credits"]}'
    $B --method resources/read '{"uri": "mce://docs/index"}'
    $B --method prompts/get '{"name": "start_game", "arguments": {"brief": "..."}}'
    $B --method tools/list                          # every tool with its schema

A tool's images are saved as PNG files and their paths printed (`image: /path/0007.png`); open them
to look. Game code lives in `/tmp/claude-0/demo2/solo-3d/MarbleGarden/games/marble_garden/module/` (components and systems); edit it
with your file tools, then `$B module_build` compiles it and loads it into the server.

**Rules.**
- Do not read anything under `/home/user/mesh_control_engine` (the engine's source). The SDK, the MCP server's
  docs (`mce://docs/...`, `docs_search`) and `engine_capabilities` are what you learn from.
- Edit scenes and assets through the MCP tools, not by hand, except game code and `.mctest` files.
- When the engine makes something hard (a missing tool, an unclear error, a wrong doc, a
  workaround you had to invent), call `report_friction` right then, before working around it.
- Look at what you make: `render_frame` / `render_sheet` after each visible change.
- Finish with `project_check` passing and `ship_build` returning a zip. Then write
  `/tmp/claude-0/demo2/solo-3d/SUMMARY.md`: what the game is, how to play it, what you could not do.

Start with `$B --method prompts/get` for the `start_game` prompt with this brief:

A small 3D game. You roll a marble around a walled garden diorama with ramps, hedges and a pond.
Five gems are scattered across the garden; collect all five before a 90-second timer runs out.
Falling into the pond puts the marble back at the start. A title screen, a win screen and a lose
screen when time runs out. A warm afternoon look, gentle music and a chime for each gem.