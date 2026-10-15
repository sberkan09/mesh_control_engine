# Friction backlog

Collected by mce unknown from 1 project(s).

## major (4)

### assets/materials (1)

- .mcmat has no UV scale/offset, and gen_mesh boxes and planes map a texture once per face, so a 30 m courtyard floor cannot tile a 256 px stone texture (LanternCourt, solo, asset_schema)
  - expected: a uv_scale field on .mcmat (the importer already supports KHR_texture_transform in glTF) or a uv tiling option on gen_mesh
  - actual: Material fields: base_color, metallic, roughness, emissive, alpha, textures only
  - workaround: paving laid as many small slab instances from module code (StaticInstances3D)

### mcp/module_test (1)

- module_test fails after module_build because the smoke test plays build/project/assets.mce, which module_build and cook_run pack do not refresh (LanternCourt, solo, module_test)
  - expected: module_test runs against the current content, or says the pack is stale
  - actual: scene: column 0 names a component type that is not reflected (the template Spin, removed from the module); initialization failed
  - workaround: ran mce build LanternCourt from the SDK to recook the build pack

### render/kit3d (1)

- Kit PointLight3D / SpotLight3D have no cast_shadows, so lantern lights cannot cast shadows from scene data (LanternCourt, solo, refl_describe_component)
  - expected: PointLight3D.cast_shadows (the renderer has PointLight::cast_shadows and r.local_shadows) reachable from a scene, as the brief asks for lanterns that throw real shadows
  - actual: PointLight3D fields are color, offset, intensity, radius, enabled only; the stock player extract fills the render light and a game module cannot reach it
  - workaround: only the sun cascades cast shadows; lanterns light without shadows

### render/settings (1)

- r.quality is requires_restart and not saved, so an in-game options screen cannot set the engine quality tier (LanternCourt, solo, console_list)
  - expected: r.quality saved (like audio.music_db) and applied on the next start, or switchable live
  - actual: flags requires_restart only; a UI convar binding to it changes nothing now or later
  - workaround: a game convar lc.quality (saved) that the module applies itself: volumetric fog, cascades, bloom, grass count, GI volume and probes

## minor (9)

### assets/credits (1)

- asset_delete leaves the deleted file row in CREDITS.md, and project_check then warns about it (LanternCourt, solo, asset_delete)
  - expected: asset_delete removes the CREDITS.md row it or gen_*/library_add wrote (and a font license file next to it)
  - actual: the row stays; the OFL text of a deleted font stays too
  - workaround: edited CREDITS.md and removed the OFL texts by hand

### gameplay/app (1)

- A game module has no way to quit the app, so a title screen cannot offer a Quit button (LanternCourt, solo)
  - expected: an app-quit request a system can raise (like scene::request_change)
  - actual: App::quit lives in the host; the module sees only the World
  - workaround: no Quit button; closing the window quits

### input/scripts (1)

- A scripted action with value 0 ("10 Quality=0") is not pressed, so a tabs or slider action whose new value is 0 cannot be replayed from an input script (LanternCourt, solo, run_headless)
  - expected: the UI presses its action with x = the new value, including 0; the script form should be able to say the same
  - actual: Quality=1 arrives as pressed with x 1; Quality=0 and Quality=0.0 never arrive
  - workaround: the tabs are also bound to the convar, which is what the player uses; low quality is previewed by changing the convar default

### mcp/module_build (1)

- module_build reload leaves the old module convars registered: console_list shows lc.quality three times after three reloads (LanternCourt, solo, console_list)
  - expected: a reloaded module replaces its MCE_CVARs
  - actual: duplicate entries with the same name; console_exec sets one of them, not necessarily the live module one
  - workaround: none needed for shipping; avoided relying on console_exec

### mcp/render_frame (1)

- render_frame and run_headless take no convars, so a game setting read from a convar (graphics quality) cannot be previewed (LanternCourt, solo, render_frame)
  - expected: a convars argument passed to the player (e.g. {"lc.quality": 0, "r.quality": "low"})
  - actual: console_exec changes the server process only; render_frame draws with a separate mce_player that never sees it
  - workaround: the options tabs also raise a Quality action the game applies, so input scripts can switch quality

### playtest/bots (1)

- walk_to bot cannot wait at a waypoint, so a level where the player must stand still for a moment (hold to light) needs zig-zag waypoints (LanternCourt, solo, test_run)
  - expected: a per-waypoint dwell (e.g. through: [{"at": [x,y,z], "wait": 1.5}])
  - actual: through takes points or entities only; the bot moves on as soon as it is within reach
  - workaround: several waypoints inside the lantern reach so the bot stays near it long enough

### render/decals (1)

- Decal3D roughness/metallic on paving drawn as StaticInstances3D shows no gloss: puddles read as dark patches with a dotted edge, not wet reflections (LanternCourt, solo, render_frame)
  - expected: a decal with roughness 0 and a reflection probe overhead to look wet
  - actual: only the base color darkens; the low-alpha edge of the texture comes out as a regular dot pattern
  - workaround: left the puddles as dark wet patches

### render/kit3d (1)

- SSAO and TAA are only reachable on Scene3D (AmbientOcclusion::enabled, PostFx::taa); Environment3D does not expose them, so kit scenes cannot ask for them (LanternCourt, solo)
  - expected: Environment3D.ssao / taa fields like volumetric_fog
  - actual: r.ssao / r.taa convars say for scenes that ask for it, but nothing in the kit asks
  - workaround: none; FXAA only

### ui/layout (1)

- A Text widget in a horizontal Stack word-wraps when the UI scale is below 1 (a 480x270 view), although the panel has room: Lanterns 0 / 7 breaks before the 7 (LanternCourt, solo, ui_preview)
  - expected: the text measured at the scaled font size, no wrap while it fits
  - actual: any text with spaces wraps at small sizes; text without spaces (3:00) does not
  - workaround: label and count as separate Texts, count without spaces (0/7)
