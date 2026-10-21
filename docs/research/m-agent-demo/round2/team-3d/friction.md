# Friction backlog

Collected by mce unknown from 1 project(s).

## major (1)

### render (1)

- render_frame free camera silently falls back to the game camera when position.x is negative (MarbleGarden, level, render_frame)
  - expected: camera {position:[-16,20,16],target:[0,0,0]} draws from that point
  - actual: the frame is the scene camera view, no error or warning; [16,9,16] and [0,26,22] work
  - workaround: only use free cameras with x >= 0

## minor (12)

### mcp (4)

- gen_sfx params.wave accepted values are not in the tool schema (MarbleGarden, art, gen_sfx)
  - expected: the schema to enumerate wave values (square, saw, sine, noise)
  - actual: params is a free object; wave=triangle refused only at call time
  - workaround: used sine
- Entity refs in entity_create/component_set values need numeric "$<id>"; an entity name is refused (MarbleGarden, level, entity_create)
  - expected: "$Marble" (or a name) accepted like the entity argument accepts unique names
  - actual: TOOL ERROR: expected "$<entity id>" of an entity in the scene
  - workaround: look up the id from the create result and pass "$3"
- run_headless inspect.entities names resolve against the open scene, not the scene named by the scene argument (MarbleGarden, level, run_headless)
  - expected: with scene=scenes/garden.mcscene, inspect names Game/Marble of that scene
  - actual: TOOL ERROR no entity named Game in the scene (the open scene was lose.mcscene)
  - workaround: scene_open the scene first
- project_check text contrast ignores the panel sprite behind HUD text (MarbleGarden, art, project_check)
  - expected: contrast measured against what is actually behind the label (a Sprite panel under it, or the rendered frame)
  - actual: dark text on a cream Sprite panel over a 3D scene is reported as 1.1:1 against the 2D camera clear colour, which is never visible on the 3D path
  - workaround: trying the HUD camera clear_color set to the panel colour; otherwise accept the warning

### docs (2)

- Namespaces of common gameplay types are not documented: WorldTransform is mce::WorldTransform (in mce/ecs/hierarchy.h, not mce::ecs), TextLabel/SfxTrigger/Camera3DComponent are mce::kit:: (MarbleGarden, code)
  - expected: guide ch.6/8 name the qualified type and header for WorldTransform, TextLabel, SfxTrigger
  - actual: two compile rounds guessing namespaces (ecs::WorldTransform does not exist)
  - workaround: grep the SDK headers
- MCE_LOG_INFO is spdlog/fmt-style ({}), not printf; a printf format compiles and logs the raw %s %u (MarbleGarden, code, module_build)
  - expected: guide 6.13 says the format syntax, or the macro rejects printf specifiers at compile time
  - actual: log showed: round over, %s, %u/%u gems in %.1f s
  - workaround: use {} placeholders

### playtest (2)

- walk_to bot has one target: no waypoint list, so proving a collect-all level needs a hand-built input script (MarbleGarden, level, test_run)
  - expected: bot {kind: walk_to, targets: [Gem 1, Gem 2, ...]} or legs, visiting each in order
  - actual: one target per scenario; a .mctest cannot start the actor elsewhere either
  - workaround: closed-loop: replay run_headless with inspect every 15 ticks to synthesize a scripted input route, then store it as the scenario input
- .mctest event checks need the C++ namespace-qualified type name; an unknown or unqualified name silently counts 0 (MarbleGarden, level, test_run)
  - expected: event GemCollected matches marble_garden::GemCollected, or an unknown event name is an error listing registered events
  - actual: actual: 0, check fails as if no event fired
  - workaround: use marble_garden::GemCollected / kit::TriggerEnter3D

### project_check (1)

- project_check accessibility contrast ignores a backing Sprite panel behind a TextLabel and compares against the 3D camera clear colour (MarbleGarden, level, project_check)
  - expected: dark text on the cream ui/panel.png sprite drawn behind it counts as high contrast
  - actual: HUD Timer: text contrast against the background colour is 1.1:1 (and its fix suggests a backing panel, which is already there)
  - workaround: treat the warning as a false positive, verified by render_frame

### render (1)

- No particle system for 3D scenes: ParticleEmitter/.mcpart are 2D-only (MarbleGarden, art, asset_schema)
  - expected: a way to puff sparkles at a 3D position when a gem is collected (data-only, like ParticleBurst)
  - actual: .mcpart is described as a 2D emitter preset (direction in degrees, 2D gravity); 08-3d.md mentions no 3D particles
  - workaround: gem pickup feedback is the chime plus the gem disappearing; no sparkles

### shipping (1)

- project_check lists assets/input/controls.mcinput as an unused asset, though the player loads the first .mcinput in the pack implicitly (MarbleGarden, code, project_check)
  - expected: an input map the player auto-loads counts as referenced
  - actual: info: asset nothing references
  - workaround: ignore the info

### testing (1)

- .mctest event checks need the C++-qualified event name (marble_garden::GemCollected); the unqualified name silently counts 0 (MarbleGarden, code, test_run)
  - expected: "event": "GemCollected" matches the game event as in the guide example (CoinCollected), or an unknown event name is an error listing known names
  - actual: actual 0, ok false, no hint
  - workaround: use "marble_garden::GemCollected"
