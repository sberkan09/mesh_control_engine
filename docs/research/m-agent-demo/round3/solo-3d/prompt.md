# Instructions given to the round-3 agent

The solo instructions (briefs/agent_solo.md) with this run's paths, a note that the machine has no GPU, a request for a short final report, and the Lantern Court brief (briefs/3d_r3.md).

---


You are making a game with Mesh Control Engine (MCE), an engine you have never seen. No person
will answer questions or review your work: decide everything yourself and keep going until the
game is finished and shipped.

**What you have.** An installed MCE SDK at `/root/r3/sdk` (its `bin/`, `include/`, `share/`) and a fresh
project at `/root/r3/LanternCourt` made with `mce new` and already built once. You reach the engine's MCP
server through a shell bridge, already running for you:

    B="python3 scripts/agent_demo/mcp_bridge.py call --sock /root/r3/agents/solo/bridge.sock"
    $B scene_info                                   # a tool call: name, then JSON arguments
    $B project_check '{"checks": ["credits"]}'
    $B --method resources/read '{"uri": "mce://docs/index"}'
    $B --method prompts/get '{"name": "start_game", "arguments": {"brief": "..."}}'
    $B --method tools/list                          # every tool with its schema

A tool's images are saved as PNG files and their paths printed (`image: /path/0007.png`); open them
to look. Game code lives in `/root/r3/LanternCourt/games/lantern_court/module/` (components and systems); edit it
with your file tools, then `$B module_build` compiles it and loads it into the server.

**Rules.**
- Do not read anything under `the engine checkout` (the engine's source). The SDK, the MCP server's
  docs (`mce://docs/...`, `docs_search`) and `engine_capabilities` are what you learn from.
- Edit scenes and assets through the MCP tools, not by hand, except game code and `.mctest` files.
- When the engine makes something hard (a missing tool, an unclear error, a wrong doc, a
  workaround you had to invent), call `report_friction` right then, before working around it.
- Look at what you make: `render_frame` / `render_sheet` after each visible change.
- Finish with `project_check` passing and `ship_build` returning a zip. Then write
  `/root/r3/SUMMARY.md`: what the game is, how to play it, what you could not do.

Start with `$B --method prompts/get` for the `start_game` prompt with this brief:

A small 3D game. At dusk you walk a lamplighter through a walled temple courtyard and light seven
lanterns before night falls: a three-minute timer while the sky darkens. Each lantern you light
throws real light and shadows across the walls and stones, and a low fog drifts over the ground
and glows around the lit lanterns. Long grass sways beside the paths; there are rain puddles and
painted marks on the paving. When the last lantern is lit, the brazier in the middle of the court
bursts into sparks. At the back stands a small shrine: inside, the room is lit only by the light
that comes in through its door and bounces off its walls, and a polished bronze bell reflects the
courtyard. The game should look as good as the engine can make it on a desktop, and still run on a
weaker machine.

Screens: a title screen, a pause menu, an options screen (graphics quality low / medium / high,
music volume), a win screen, a lose screen and an in-game HUD with the lantern count and the time
left. All of them share one visual style (warm paper and ink) that could be re-skinned by changing
it in one place. Gentle music and a chime for each lantern.
