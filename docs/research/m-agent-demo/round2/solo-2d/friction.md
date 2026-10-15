# Friction backlog

Collected by mce unknown from 1 project(s).

## major (1)

### mcp (1)

- module_build reloads code but keeps the old reflection of a component whose fields changed (Glowmoth, solo, module_build)
  - expected: after adding a field to an existing component (Screen.sound) and module_build, refl_describe_component and component_set know the new field, or module_build says a restart is needed
  - actual: module_build returns ok/reloaded with no warning; refl_describe_component Screen still shows the old fields and size 80; entity_create with the new field fails with unknown field. The new code then runs against scenes loaded with the old layout
  - workaround: rename the component (Screen -> MenuScreen) so it registers as a new type
  - details: The recipe new-component-live only covers new components; nothing says changed ones need a server restart, and an agent behind a bridge cannot restart the server

## minor (4)

### docs (2)

- 07-2d.md says the MCP server has no tilemap tool, but tilemap_paint exists (Glowmoth, solo)
  - expected: guide 7.8 points at tilemap_paint / asset_create for .mctilemap
  - actual: 7.8 Limits: "the MCP server has no tilemap tool (an agent paints by writing the .mctilemap JSON, section 5.7)" while chapter 14 lists tilemap_paint
  - workaround: trust chapter 14 and engine_capabilities
- MCE_LOG_* takes fmt-style {} placeholders, not printf; the guide does not say so and printf formats compile silently (Glowmoth, solo, module_build)
  - expected: 6.13 Logging to say MCE_LOG_INFO uses {} (spdlog/fmt) placeholders, or a compile-time check
  - actual: MCE_LOG_INFO("lit (%u/%u)", a, b) compiles without warning and logs the literal %u/%u
  - workaround: read mce/log.h in the SDK (SPDLOG_INFO) and switch to {}

### playtest (2)

- walk_to bot: arriving at an entity target ends the run before the game reacts, and a scene change counts as a death (Glowmoth, solo, playtest)
  - expected: a way to keep running after the bot arrives (so until {scene} can see the gate load the next level), and deaths not counting the actor vanishing because the scene changed
  - actual: target Gate stops the run at arrival (stopped_early, scene unchanged, the game waits 40 ticks before request_change); with until {scene} the actor disappearing on the change counts as deaths: 1
  - workaround: target a point past the gate inside the level wall ([70, 5]) so the bot never arrives and until {scene} ends the run; assert deaths <= 1
- walk_to bot cannot use a double jump, so levels a person can finish with an air flap fail the bot (Glowmoth, solo, playtest)
  - expected: an option for the side-view bot to press Jump again in the air (or a jump-hold length), so moves beyond one jump are testable
  - actual: the bot stuck at a 3-tile step that a second Jump press in the air clears (stuck_spots at x 19.6)
  - workaround: kept every step on the bot route at 2 tiles or less; level 2 is harder through more, faster, lower bats instead
