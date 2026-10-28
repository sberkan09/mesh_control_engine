// The M-AGENT tool table (ROADMAP M-AGENT). Kept apart from the MVP table in tools.cpp so the two
// tracks that add tools touch different files; tool_table() concatenates them.

#include "tools_agent.h"

namespace mce::mcp {

namespace {

// Shared schema pieces (render_frame's arguments, also nested in image_diff's 'render').
#define MCE_RC0 "\"scene\":{\"type\":\"string\",\"description\":\"Render this saved scene (path under assets/) instead of the open one\"},"
#define MCE_RC1 "\"input\":{\"description\":\"Scripted input as run_headless takes it: a script string (\\\"10-70 Move=1,0 Jump\\\") or an array of {from, to, actions}\",\"type\":[\"string\",\"array\"]},"
#define MCE_RC2 "\"camera\":{\"type\":\"object\",\"description\":\"Render from another camera (render only, the sim is unchanged): {\\\"entity\\\": id or name} of a camera entity in the open scene, {\\\"index\\\": n} (scene entity index), {\\\"position\\\": [x,y,z], \\\"target\\\": [x,y,z], \\\"fov\\\": deg} (free 3D) or {\\\"center\\\": [x,y], \\\"zoom\\\": z} (free 2D). A camera object also takes (alone, it keeps the scene's own camera) \\\"focus\\\" (depth of field: meters to the sharpest plane, or \\\"target\\\" = the distance from position to target) and \\\"exposure\\\" (a multiplier of the scene exposure)\"},"
#define MCE_RC2B "\"view\":{\"type\":\"object\",\"description\":\"Look at one local player's view or one render texture's camera (M-VIEWS), full size: {\\\"player\\\": 1..4} or {\\\"texture\\\": entity id or name}. Join the players with input (\\\"5 P2/Join\\\") so their cameras follow them. Not with camera\"},"
#define MCE_RC3 "\"safe_area\":{\"type\":\"object\",\"description\":\"Pretend the screen has these safe-area insets in pixels {left, top, right, bottom}: UI lays out inside them and the image outlines them\"},"
#define MCE_RC4 "\"software\":{\"type\":\"boolean\",\"description\":\"Force the offscreen software renderer (automatic when there is no display)\"},"
#define MCE_RC5 "\"convars\":{\"type\":\"object\",\"description\":\"Console variables for this run, name -> value (e.g. {\\\"lc.quality\\\": 0}): preview a setting without changing the project\"}"
#define MCE_RC2C "\"environment\":{\"type\":\"object\",\"description\":\"Override Environment3D fields for this shot only (the scene is unchanged): numbers, true/false for flags, [x,y,z] / [r,g,b] for vectors and colors, e.g. {\\\"fog_density\\\": 0.04, \\\"sun_color\\\": [1, 0.6, 0.3], \\\"depth_of_field\\\": 1} (refl_describe_component Environment3D lists the fields)\"},"
#define MCE_RENDER_COMMON_PROPS MCE_RC0 MCE_RC1 MCE_RC2 MCE_RC2B MCE_RC2C MCE_RC3 MCE_RC4 MCE_RC5
#define MCE_RF0 "\"tick\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":36000,\"default\":1,\"description\":\"Capture the first frame after this sim tick\"},"
#define MCE_RF1 "\"width\":{\"type\":\"integer\",\"minimum\":64,\"maximum\":3840,\"default\":960},\"height\":{\"type\":\"integer\",\"minimum\":64,\"maximum\":3840,\"default\":540},"
#define MCE_RENDER_FRAME_PROPS MCE_RF0 MCE_RF1 MCE_RENDER_COMMON_PROPS

// The generators' `save` and `overwrite` (tools_content.cpp).
#define MCE_SAVE_PROPS R"j("save":{"type":"string","description":"Also write the result under assets/ at this path (with its .mceasset sidecar and a CREDITS.md row), e.g. sprites/slime.png"},"overwrite":{"type":"boolean","default":false,"description":"Replace an existing file (its GUID is kept)"})j"

const ToolDef kAgentTools[] = {
    {"render_frame",
     "See the game: draw the open (saved) or another saved scene with the real player at sim tick N, with scripted input, "
     "from its own camera or another, at a chosen size and safe area. Returns the frame as a PNG image. Works without a GPU "
     "(offscreen software rendering). 'save' also writes the PNG under the project folder.",
     "{\"type\":\"object\",\"properties\":{" MCE_RENDER_FRAME_PROPS
     R"j(,"save":{"type":"string","description":"Also write the frame as a PNG at this path inside the project folder, e.g. shots/title.png"}}})j",
     render_frame},
    {"render_sheet",
     "A contact sheet: frames at several ticks (columns) from one or more cameras (rows) in one image, each cell labelled "
     "with its tick. One player run per camera.",
     R"j({"type":"object","properties":{"ticks":{"type":"array","items":{"type":"integer"},"minItems":1,"maxItems":16,"description":"Ascending sim ticks"},)j"
     R"j("cameras":{"type":"array","maxItems":4,"items":{"type":"object"},"description":"Camera objects as render_frame's 'camera'; default: the scene's own"},)j"
     R"j("cell_width":{"type":"integer","minimum":64,"maximum":3840,"default":320},"cell_height":{"type":"integer","minimum":64,"maximum":3840,"default":180},)j" MCE_RENDER_COMMON_PROPS
     R"j(},"required":["ticks"]})j",
     render_sheet},
    {"timeline_preview",
     "See a cutscene at a moment: draw the scene with every timeline in it paused at 'time' seconds (state tracks applied, "
     "cinematic cameras placed, the timeline's video frame shown) with the real player, as render_frame does. Returns the "
     "frame as a PNG image; 'save' also writes it under the project folder.",
     R"j({"type":"object","properties":{"time":{"type":"number","minimum":0,"maximum":3600,"description":"Seconds into the timelines"},)j" MCE_RENDER_FRAME_PROPS
     R"j(,"save":{"type":"string","description":"Also write the frame as a PNG at this path inside the project folder"}},"required":["time"]})j",
     timeline_preview},
    {"image_diff",
     "Compare a frame with a golden PNG in the project: 'image' (a PNG in the project) or 'render' (render_frame's arguments). "
     "Returns the score (mean channel difference, 0..1), the changed fraction and pass (score <= max_score), with a heat map "
     "image. With no golden yet, or update: true, the image becomes the golden.",
     R"j({"type":"object","properties":{"golden":{"type":"string","description":"PNG path inside the project folder, e.g. goldens/title.png"},)j"
     R"j("image":{"type":"string","description":"PNG path inside the project folder to compare"},)j"
     "\"render\":{\"type\":\"object\",\"description\":\"render_frame arguments: the frame to compare\",\"properties\":{" MCE_RENDER_FRAME_PROPS "}},"
     R"j("threshold":{"type":"integer","minimum":0,"maximum":255,"default":8,"description":"Channel difference a pixel may have and still count as unchanged"},)j"
     R"j("max_score":{"type":"number","minimum":0,"maximum":1,"default":0.01},"update":{"type":"boolean","description":"Write the image as the new golden"}},"required":["golden"]})j",
     image_diff},
    {"perf_run",
     "Measure a scene with the real player: N frames at one sim tick per frame (counters repeat run to run), "
     "judged against a target's performance budget (project_get budgets_in_force). Returns the verdict with advice "
     "for each limit over, frame/sim/render/GPU ms (p50/p95/p99/max), draws, triangles, sprites and other counters, "
     "draw_causes (3D scene draws that were not instanced, by cause, each with what to change), the slowest systems, the GPU stages, and spike frames with their cause. The .mcperf capture is kept "
     "(cooked/.perf/last.mcperf, or 'save'); perf_compare compares two. Works without a GPU (software rendering; "
     "times are then the CPU's). headless: true measures the sim alone. With a progressToken it sends "
     "notifications/progress with the phase (launching, warmup N/M, capturing N/M, reading the capture).",
     R"j({"type":"object","properties":{"scene":{"type":"string","description":"Scene path under assets/ (default: the open scene, saved)"},)j"
     R"j("frames":{"type":"integer","minimum":1,"maximum":3600,"default":300,"description":"Frames captured after the warmup; 1-29 is a quick look (counters still repeat, percentiles are noisy and the report says so)"},"warmup":{"type":"integer","minimum":0,"maximum":3600,"default":30,"description":"Frames run first and left out (loading, first uploads)"},)j"
     R"j("target":{"type":"string","enum":["desktop","mobile","web"],"default":"desktop"},)j"
     R"j("width":{"type":"integer","minimum":64,"maximum":3840,"default":960},"height":{"type":"integer","minimum":64,"maximum":3840,"default":540},)j"
     R"j("headless":{"type":"boolean","default":false},"software":{"type":"boolean","default":false,"description":"Force the software rasterizer"},)j"
     R"j("top":{"type":"integer","minimum":1,"maximum":64,"default":8,"description":"Systems listed"},)j"
     MCE_RC1 MCE_RC5 ","
     R"j("save":{"type":"string","description":"Also keep the capture at this path in the project folder, e.g. perf/before.mcperf"}}})j",
     perf_run},
    {"perf_compare",
     "Compare two .mcperf captures (default: perf_run's last two): p50/p95 of frame, sim, render and GPU ms, the "
     "worst frame's counters, the systems whose ms per frame moved most, which metrics got better or worse, and the "
     "after capture's verdict against the target's budget.",
     R"j({"type":"object","properties":{"before":{"type":"string","description":"Path in the project folder (default cooked/.perf/previous.mcperf)"},)j"
     R"j("after":{"type":"string","description":"Default cooked/.perf/last.mcperf"},"target":{"type":"string","enum":["desktop","mobile","web"],"default":"desktop"},)j"
     R"j("top":{"type":"integer","minimum":1,"maximum":64,"default":8}}})j",
     perf_compare},
    {"module_build",
     "Rebuild the game module with the project's CMake build (the tree the loaded module came from) and map the new build "
     "into this server: new and changed components and systems are live for the next tool call, no restart. Returns the "
     "compiler's errors (and with warnings: true its warnings) as {severity, file, line, col, message}, plus the output's "
     "end when the build failed. The open scene is kept (re-read with the new layouts); the undo history restarts.",
     R"j({"type":"object","properties":{"reload":{"type":"boolean","default":true,"description":"Map the new build after a successful build"},)j"
     R"j("warnings":{"type":"boolean","default":false,"description":"Also list warnings and notes"},)j"
     R"j("target":{"type":"string","description":"CMake target to build (default <game>_game)"}}})j",
     module_build},
    {"module_test",
     "Run the project's tests with ctest in the module's build tree (default filter: tests named <game>_*, such as the "
     "<game>_headless_300 smoke test). Builds the host and the cooked pack first, so the tests play the current "
     "content. Returns each test's result and the output's end when one failed.",
     R"j({"type":"object","properties":{"filter":{"type":"string","description":"ctest -R regular expression (default ^<game>_)"}}})j",
     module_test},
    {"test_run",
     "Run playtest scenarios without a person: .mctest files (JSON) under the project's tests/ folder, one file or folder "
     "('path'), or one inline scenario ('test'). A scenario plays a saved scene headless with scripted input or a built-in "
     "bot, may stop early ('until'), and checks component fields (at the end or at a tick), entities alive, event counts, "
     "the log and the bot's feel metrics. Returns each scenario's checks with actual values, its state_hash, and the feel "
     "report (time to finish, deaths, stuck spots, idle time) when it has an actor.",
     R"j({"type":"object","properties":{"path":{"type":"string","description":"A .mctest file or a folder of them inside the project (default tests/)"},)j"
     R"j("test":{"type":"object","description":"One scenario inline, in the .mctest format: {name, scene, ticks, input | bot, actor, until, death, assert: [checks], trace}"}}})j",
     test_run},
    {"playtest",
     "Let a built-in bot play the scene and say how it felt: walk_to (head for a point or entity, jumping or sidestepping "
     "when stuck), explore (seeded random wandering, coverage) or masher (random button chords, crash and soft-lock "
     "hunting). Returns the feel report: time to finish, deaths, stuck spots with positions, idle time, distance, cells "
     "visited. Needs no game code: the bot drives the game's input actions (Move, Jump, ...).",
     R"j({"type":"object","properties":{"bot":{"type":"object","description":"{kind: walk_to|explore|masher, actor: entity, target: [x,y(,z)] or entity (walk_to), through: [waypoints, points or entities or {at, wait: ticks to stand there}, reached in order before target] (walk_to), air_jumps: 0-4 extra Jump presses in the air (walk_to, side view), plane: xy|x|xz (default xy; x = side view with Jump, xz = 3D ground), reach: m (0.5), seed, move: action (Move), jump: action (Jump), actions: [names] (masher), period: ticks (8), hold: [actions held all run, e.g. Crouch], hold_when: [{action, entity, field, value}] (held while the field check is true), stop_when: {entity, field, value} (stands still while true); a walk_to waypoint {at, wait, press: [actions pressed once on arrival]}}"},)j"
     R"j("scene":{"type":"string","description":"A saved scene under assets/ (default: the open scene)"},)j"
     R"j("ticks":{"type":"integer","minimum":1,"maximum":36000,"default":1800},)j"
     R"j("until":{"type":"object","description":"Stop early when this check holds (a field, alive or event check)"},)j"
     R"j("death":{"type":"object","description":"Count a death each time this check becomes true, e.g. {entity, field: Health.hp, <=: 0}"},)j"
     R"j("assert":{"type":"array","items":{"type":"object"},"description":"Checks as in .mctest files"},)j"
     R"j("trace":{"type":"object","description":"{every: ticks, fields: [{entity, field}]}: sample fields during the run"}},"required":["bot"]})j",
     playtest},
    {"docs_search",
     "Search the engine's shipped docs (user guide, engine reference), the worked samples and the example games. Returns "
     "the best sections with their mce://docs/... resource URI, heading, line and a snippet; read the whole file with "
     "resources/read. Start with mce://docs/index.",
     R"j({"type":"object","properties":{"query":{"type":"string","description":"Words to look for, e.g. \"camera shake\" or \"RigidBody2D\""},)j"
     R"j("group":{"type":"string","enum":["guide","engine","architecture","samples","examples"],"description":"Search one group only"},)j"
     R"j("limit":{"type":"integer","minimum":1,"maximum":20,"default":8}},"required":["query"]})j",
     docs_search},
    {"engine_capabilities",
     "What a game reaches through what: each engine feature with how it is reached today (data: a component; asset: a "
     "source file type; mcp: a tool; console; code: needs game C++ or Luau). Scenes, prefabs, UI documents, timelines, "
     "skeletons, behaviour trees, terrain and scene changes are data you author through the tools; game rules and "
     "custom components are the code. Also lists every component, importable asset type, tool and the convar count. "
     "Read this before planning a game, to keep game code to the rules.",
     R"j({"type":"object","properties":{"area":{"type":"string","description":"Only this area's features (2D, 3D, physics, audio, input, ui, gameplay, scenes, agent, ...)"}}})j",
     engine_capabilities},
    {"gen_palette",
     "Generate a color palette (dark to light) from a seed and a scheme: analogous, complementary, triad, pastel, earth, neon "
     "or mono. Returns the colors as hex with a swatch image. 'save' writes it as a 1-pixel-high PNG strip under assets/.",
     R"j({"type":"object","properties":{"seed":{"type":"integer","minimum":0,"default":1},"count":{"type":"integer","minimum":2,"maximum":32,"default":6},)j"
     R"j("scheme":{"type":"string","enum":["analogous","complementary","triad","pastel","earth","neon","mono"],"default":"analogous"},)j" MCE_SAVE_PROPS "}}",
     gen_palette},
    {"gen_sprite",
     "Generate pixel-art sprites: a mirrored, outlined, shaded creature, character, ship, item or blob from a seed, in "
     "'colors' (hex, dark to light, at least 3; the darkest outlines) or a palette 'scheme'. 'variants' puts that many "
     "(seeds seed, seed+1, ...) in one row. Returns the frames' rects and an enlarged preview. 'save' writes the PNG under "
     "assets/ (nearest filtering, no mips) with its sidecar and a CREDITS.md row.",
     R"j({"type":"object","properties":{"seed":{"type":"integer","minimum":0,"default":1},"kind":{"type":"string","enum":["creature","character","ship","item","blob"],"default":"creature"},)j"
     R"j("width":{"type":"integer","minimum":4,"maximum":128,"default":16},"height":{"type":"integer","minimum":4,"maximum":128,"default":16},)j"
     R"j("variants":{"type":"integer","minimum":1,"maximum":16,"default":1},"colors":{"type":"array","items":{"type":"string"},"description":"Hex colors, dark to light"},)j"
     R"j("scheme":{"type":"string","description":"Palette scheme when 'colors' is not given (gen_palette's)"},)j" MCE_SAVE_PROPS "}}",
     gen_sprite},
    {"gen_pixels",
     "Draw a pixel-art image from text: 'rows' is one string per pixel row (all the same length), 'palette' maps each "
     "character to a hex color ('.' and ' ' are transparent). For a specific shape (a moth, a lantern, a door) that "
     "gen_sprite's seeds will not give. Returns an enlarged preview; 'save' writes the PNG with its sidecar (nearest "
     "filtering) and a CREDITS.md row.",
     R"j({"type":"object","properties":{"rows":{"type":"array","items":{"type":"string"},"minItems":1,"maxItems":256},)j"
     R"j("palette":{"type":"object","additionalProperties":{"type":"string"},"description":"One-character keys to \"#rrggbb\" or \"#rrggbbaa\""},)j" MCE_SAVE_PROPS "},"
     R"j("required":["rows","palette"]})j",
     gen_pixels},
    {"gen_mesh",
     "Make a simple 3D mesh for blocking out a level or a toy-like game: a box, plane, sphere, cylinder, cone, disc or "
     "ramp (a wedge rising towards +X), centred on the origin with Y up, 'size' its full extent in metres, in one "
     "'color' with 'roughness'. 'humanoid' is a skinned figure (size [shoulder width, height, depth], default "
     "[0.5, 1.8, 0.28], facing -Z, feet at -height/2 so a CharacterController3D's capsule centre is its origin) with "
     "13 bones (hips, spine, head, upper_arm_l/r, forearm_l/r, thigh_l/r, shin_l/r, foot_l/r) and the clips Idle and "
     "Walk: an Animator plays them, Ragdoll3D makes it go limp, AttachToBone pins props to its bones. Returns its "
     "vertex and triangle counts and a shaded preview; 'save' writes it as a .glb (with its material inline) and a "
     "CREDITS.md row. Place it with the 3D kit's MeshRef (and MeshCollider3D).",
     R"j({"type":"object","properties":{"shape":{"type":"string","enum":["box","plane","sphere","cylinder","cone","disc","ramp","humanoid"],"default":"box"},)j"
     R"j("size":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3,"description":"[x, y, z] in metres (default [1, 1, 1])"},)j"
     R"j("segments":{"type":"integer","minimum":3,"maximum":128,"default":24,"description":"Around a round shape"},)j"
     R"j("color":{"type":"string","description":"\"#rrggbb\" or \"#rrggbbaa\" (default light grey)"},)j"
     R"j("roughness":{"type":"number","minimum":0,"maximum":1,"default":0.8},)j" MCE_SAVE_PROPS "}}",
     gen_mesh},
    {"gen_tileset",
     "Generate a pixel-art tile set in one row: fill, fill_2, fill_3 (variations), top, top_left, top_right (surface "
     "edges), platform and decoration. Styles: grass, stone, brick, dirt, sand, snow, water, wood, metal. Returns each "
     "tile's rect and a preview; 'save' writes the PNG under assets/ for a TileMap's tile set.",
     R"j({"type":"object","properties":{"seed":{"type":"integer","minimum":0,"default":1},"style":{"type":"string","enum":["grass","stone","brick","dirt","sand","snow","water","wood","metal"],"default":"grass"},)j"
     R"j("tile":{"type":"integer","minimum":8,"maximum":64,"default":16},"colors":{"type":"array","items":{"type":"string"},"description":"4 fill colors dark to light, or 4 fill + 3 surface colors (default: the style's)"},)j" MCE_SAVE_PROPS "}}",
     gen_tileset},
    {"gen_texture",
     "Generate a noise texture (value, perlin, fbm, ridged, worley; tileable by default) mapped through 'colors' gradient "
     "stops, or an equirect sky (kind sky: horizon, zenith and cloud colors, 'clouds' cover, 'stars' density; width = 2 x "
     "height) for a skybox or a backdrop. 'save' writes the PNG under assets/ (linear filtering, mips).",
     R"j({"type":"object","properties":{"seed":{"type":"integer","minimum":0,"default":1},"kind":{"type":"string","enum":["value","perlin","fbm","ridged","worley","sky"],"default":"fbm"},)j"
     R"j("width":{"type":"integer","minimum":16,"maximum":2048,"default":256},"height":{"type":"integer","minimum":16,"maximum":2048,"default":256},)j"
     R"j("scale":{"type":"number","minimum":0.5,"maximum":64,"default":4,"description":"Features across the image"},"octaves":{"type":"integer","minimum":1,"maximum":8,"default":5},)j"
     R"j("tileable":{"type":"boolean","default":true},"colors":{"type":"array","items":{"type":"string"},"description":"Gradient stops low to high (sky: horizon, zenith, clouds)"},)j"
     R"j("clouds":{"type":"number","minimum":0,"maximum":1,"default":0.5},"stars":{"type":"number","minimum":0,"maximum":1,"default":0},)j" MCE_SAVE_PROPS "}}",
     gen_texture},
    {"gen_sfx",
     "Generate a retro sound effect with an sfxr synthesizer: a preset (pickup, laser, explosion, powerup, hit, jump, blip, "
     "random) varied by the seed, with any sfxr parameter overridden in 'params'. Returns the parameters used (tweak and "
     "call again), the length and level, and a waveform image. 'save' writes a 16-bit WAV under assets/.",
     R"j({"type":"object","properties":{"seed":{"type":"integer","minimum":0,"default":1},"preset":{"type":"string","enum":["pickup","laser","explosion","powerup","hit","jump","blip","random"],"default":"pickup"},)j"
     R"j("params":{"type":"object","description":"sfxr parameters to override, e.g. {\"wave\": \"sine\", \"base_freq\": 0.4, \"env_decay\": 0.3}; wave is square, saw, sine or noise; the result lists every parameter with its value"},)j" MCE_SAVE_PROPS "}}",
     gen_sfx},
    {"gen_music",
     "Generate a seamless music loop (bass, chords, melody and optional drums) in a scale on a root note, exactly 'bars' "
     "bars of 4 beats at 'bpm', at most 60 seconds. Returns its length and a waveform image; 'save' writes a 16-bit WAV "
     "under assets/ (set the AudioSource to loop).",
     R"j({"type":"object","properties":{"seed":{"type":"integer","minimum":0,"default":1},"bpm":{"type":"integer","minimum":60,"maximum":200,"default":120},)j"
     R"j("bars":{"type":"integer","minimum":1,"maximum":32,"default":8},"root":{"type":"integer","minimum":0,"maximum":11,"default":0,"description":"0 = C ... 11 = B"},)j"
     R"j("scale":{"type":"string","enum":["major","minor","dorian","pentatonic","blues"],"default":"major"},"drums":{"type":"boolean","default":true},)j"
     R"j("volume":{"type":"number","minimum":0,"maximum":1,"default":0.6},)j" MCE_SAVE_PROPS "}}",
     gen_music},
    {"library_search",
     "Search the engine's bundled content library: CC0 models and sounds (Kenney), OFL and MIT fonts, and generator "
     "recipes (skies, textures, sprites, sounds, music). Every word must match an entry's id, title or tags. Returns ids "
     "for library_add, with license and author.",
     R"j({"type":"object","properties":{"query":{"type":"string","description":"Words, e.g. \"road corner\" or \"coin\"; empty lists everything"},)j"
     R"j("kind":{"type":"string","enum":["model","sound","music","font","texture","sprite","recipe"]},"license":{"type":"string","description":"e.g. CC0, OFL, MIT"},)j"
     R"j("limit":{"type":"integer","minimum":1,"maximum":100,"default":20}}})j",
     library_search},
    {"library_add",
     "Copy a library entry into the project's assets/ (its files, the files they need such as a model's texture, and a "
     "font's license text) with fresh .mceasset sidecars and a CREDITS.md row naming the author and license; a recipe "
     "runs its generator and saves the result. Default folder by kind: models/, sfx/, music/, fonts/, textures/.",
     R"j({"type":"object","properties":{"id":{"type":"string"},"path":{"type":"string","description":"A folder under assets/ ending in '/', or the main file's new path"},)j"
     R"j("overwrite":{"type":"boolean","default":false}},"required":["id"]})j",
     library_add},
    {"project_check",
     "Check the project before shipping and get findings to act on, each with severity, file, line, message and a fix: "
     "cook (assets and scenes that fail to import), references (GUIDs no asset has), unused (assets nothing references), "
     "credits (media files with no CREDITS.md row), srgb (data textures imported as sRGB, colour textures as linear), "
     "performance (against the target's budget: each scene run headless for ms per tick, images larger than "
     "texture_size, scenes whose sprites use more textures than sprite_textures) and accessibility (2D text height in pixels "
     "on a 1280x720 screen and its contrast against the background). pass is true when there is no error.",
     R"j({"type":"object","properties":{"checks":{"type":"array","items":{"type":"string","enum":["cook","references","unused","credits","srgb","performance","accessibility"]},"description":"Run only these (default all)"},)j"
     R"j("ticks":{"type":"integer","minimum":1,"maximum":36000,"default":300,"description":"Ticks per scene for the performance check"},)j"
     R"j("budget_ms":{"type":"number","exclusiveMinimum":0,"maximum":1000,"description":"Average sim milliseconds per tick allowed (default: the target budget's sim_ms)"},)j"
     R"j("target":{"type":"string","enum":["desktop","mobile","web"],"default":"desktop","description":"Whose performance budget (project_get budgets_in_force)"},)j"
     R"j("max_findings":{"type":"integer","minimum":1,"maximum":2000,"default":200}}})j",
     project_check},
    {"ship_build",
     "Make the release zip: the project's full CMake build (host, game module and the cooked pack; the module is mapped "
     "again), a smoke run of the host headless for 'ticks' ticks, then the SDK's `mce ship` (host, module, engine "
     "library, assets.mce, CREDITS.md). Returns each stage and the zip's path; ok is false with the stage that failed "
     "and the end of its output. Needs the server of an installed SDK (mce mcp <project>). target \"web\": the browser "
     "build for itch.io instead (`mce ship --target web`, from the engine checkout the SDK came from, for a project under "
     "its projects/; needs the Emscripten SDK and Playwright): it plays 'ticks' ticks in headless Chromium and returns "
     "the zip, the state_hash, matches_headless (the same ticks run headless in the browser) and a screenshot of the "
     "last frame. Desktop with steam_app_id: also the Steam depot for this OS and the steamcmd build scripts "
     "(<out>/<Name>-steam; nothing is uploaded).",
     R"j({"type":"object","properties":{"target":{"type":"string","enum":["desktop","web"],"default":"desktop"},"ticks":{"type":"integer","minimum":1,"maximum":36000,"default":300},"build":{"type":"boolean","default":true,"description":"Run the full build first (false: ship what is built; desktop only)"},)j"
     R"j("version":{"type":"string","description":"Version in the zip's name (default: the .mceproj's, else the SDK's)"},"out":{"type":"string","description":"Folder inside the project for the zip (default dist/)"},)j"
     R"j("steam_app_id":{"type":"integer","minimum":1,"maximum":4294967295,"description":"Desktop: also make the Steam depot and build scripts for this app"},)j"
     R"j("steam_library":{"type":"string","description":"With steam_app_id: the steam_api to ship (default: from the server's STEAMWORKS_SDK)"}}})j",
     ship_build},
    {"project_watch",
     "What the other agents on this project did: the event feed (.mce/events.jsonl), one event per write with agent, "
     "tool, path and time, numbered by seq. Pass the last call's next as since to get only new events; others: true "
     "leaves out your own. agent is your name (--agent, else the client's name and a suffix).",
     R"j({"type":"object","properties":{"since":{"type":"integer","minimum":0,"default":0},"limit":{"type":"integer","minimum":1,"maximum":500,"default":50},)j"
     R"j("others":{"type":"boolean","default":false}}})j",
     project_watch},
    {"asset_lock",
     "Lock a file under assets/ (a scene, prefab, material, texture...) so other agents' writes to it are refused "
     "until you unlock it or the lock's ttl runs out. Locking a file you hold refreshes it; an expired lock is taken over.",
     R"j({"type":"object","properties":{"path":{"type":"string","description":"Path under assets/, e.g. scenes/level1.mcscene"},"note":{"type":"string","description":"What you are doing, shown to the others"},)j"
     R"j("ttl":{"type":"integer","minimum":10,"maximum":604800,"default":1800,"description":"Seconds until the lock expires"}},"required":["path"]})j",
     asset_lock},
    {"asset_unlock",
     "Release a lock you hold. force: true breaks another agent's lock (the feed records it).",
     R"j({"type":"object","properties":{"path":{"type":"string"},"force":{"type":"boolean","default":false}},"required":["path"]})j",
     asset_unlock},
    {"asset_locks", "List the project's asset locks: path, agent, since, ttl, note and whether each expired.",
     R"j({"type":"object","properties":{}})j", asset_locks},
    {"task_list",
     "The project's shared task list (tasks.mctasks): id, title, status (open, claimed, done, blocked), owner, notes, "
     "depends and files, with counts per status.",
     R"j({"type":"object","properties":{"status":{"type":"string","enum":["open","claimed","done","blocked"]}}})j",
     task_list},
    {"task_add",
     "Add a task to the shared list. depends: ids of tasks that must be done before it can be claimed; files: the "
     "assets it will touch, so others know.",
     R"j({"type":"object","properties":{"title":{"type":"string"},"notes":{"type":"string"},"depends":{"type":"array","items":{"type":"integer"}},)j"
     R"j("files":{"type":"array","items":{"type":"string"}}},"required":["title"]})j",
     task_add},
    {"task_claim",
     "Claim a task: it becomes yours (status claimed). Without id, the first open task whose dependencies are done. Two "
     "agents never get the same task.",
     R"j({"type":"object","properties":{"id":{"type":"integer","minimum":1}}})j", task_claim},
    {"task_update",
     "Change a task's status (done, blocked, or open to give it back) and append a note signed with your name. Only "
     "the owner changes a claimed task's status.",
     R"j({"type":"object","properties":{"id":{"type":"integer","minimum":1},"status":{"type":"string","enum":["open","claimed","done","blocked"]},)j"
     R"j("notes":{"type":"string"}},"required":["id"]})j",
     task_update},
    {"scene_switch",
     "Make another scene the open one, keeping the current one open in the background with its unsaved edits; "
     "switching back restores them. scene_save saves the current scene.",
     R"j({"type":"object","properties":{"path":{"type":"string","description":"Scene path under assets/"}},"required":["path"]})j",
     scene_switch},
    {"scene_documents", "The scenes this server has open: the current one and the ones scene_switch parked, each with dirty.",
     R"j({"type":"object","properties":{}})j", scene_documents},
    {"scene_diff",
     "What changed between two versions of a scene: entities added, removed or changed (matched by id), renames, "
     "components added or removed and the changed fields by name, a reorder, a header change. 'a' is the before "
     "(a scene path under assets/); 'b' the after, or the open scene (unsaved edits included) when left out. "
     "Returns identical, counts and a text report, one line per entity.",
     R"j({"type":"object","properties":{"a":{"type":"string","description":"Scene path under assets/ (before)"},"b":{"type":"string","description":"Scene path under assets/ (after); default: the open scene"}},"required":["a"]})j",
     scene_diff},
    {"scene_lint",
     "Mistakes in a scene that load fine and misbehave later: an entity without a name, siblings that share a name "
     "(a path like Room/Wall then names both), a scale of zero on an axis, a position, rotation or scale that is not "
     "a number, a parent that does not exist, a reference to an entity the scene lacks. 'path' is a scene under "
     "assets/; left out, the open scene (unsaved edits included). Returns clean, the findings (entity, name, kind) "
     "and a text report, one line per finding.",
     R"j({"type":"object","properties":{"path":{"type":"string","description":"Scene path under assets/; default: the open scene"}}})j",
     scene_lint},
    {"scene_lint_fix",
     "Repairs what scene_lint finds in a saved scene and writes the file back in the canonical text: an empty name becomes \"Entity <id>\", a later sibling with a taken name gets \" (2)\", \" (3)\", a parent or a reference to an entity the scene lacks is dropped, a position, rotation or scale that is not a number returns to its default, a zero scale axis becomes 1. 'path' is a scene under assets/. Returns fixed (findings repaired), written (false: nothing to fix, the file is untouched) and a text report of what was found. The open scene in the editor is not changed: reload it with scene_open afterwards (unsaved edits to it would be lost).",
     R"j({"type":"object","properties":{"path":{"type":"string","description":"Scene path under assets/"}},"required":["path"]})j",
     scene_lint_fix},
    {"scene_lint_all",
     "scene_lint over every scene and prefab under assets/ in one call: is the whole project tidy. Returns clean, scenes_checked, findings (the total) and unreadable (files that do not parse), and files: one row per scene or prefab that has findings or cannot be read, with its path, the number of findings and the text report (the same lines as scene_lint). Read-only unless fix is true; then every file with findings is repaired and written back in the canonical text like scene_lint_fix (the rows and the top level also carry fixed, and clean is true when nothing unreadable is left). The open scene in the editor is not changed: reload it with scene_open afterwards.",
     R"j({"type":"object","properties":{"fix":{"type":"boolean","description":"Repair the findings and write the files; default false (read-only)"}}})j", scene_lint_all},
    {"scene_stats",
     "What a scene holds at a glance: entity count, roots, hierarchy depth, prefab instances, the box the local "
     "positions span and the components in use (most used first). 'path' is a scene under assets/; left out, the "
     "open scene (unsaved edits included). Returns the numbers and a text report (the same as `mce scene stats`).",
     R"j({"type":"object","properties":{"path":{"type":"string","description":"Scene path under assets/; default: the open scene"}}})j",
     scene_stats},
    {"scene_find",
     "The entities of a scene that match a question: 'name' is a part of the entity name (ignoring case), "
     "'component' a component type the entity carries, 'prefab' keeps only prefab instances; every condition "
     "given must hold, none given lists every entity. 'path' is a scene under assets/; left out, the open scene "
     "(unsaved edits included). Returns count, the hits (entity id, name, hierarchy path such as Room/Wall/Torch) "
     "and a text report.",
     R"j({"type":"object","properties":{"path":{"type":"string","description":"Scene path under assets/; default: the open scene"},"name":{"type":"string","description":"Part of the entity name, ignoring case"},"component":{"type":"string","description":"A component type the entity carries, e.g. Sprite"},"prefab":{"type":"boolean","description":"Only prefab instances"}}})j",
     scene_find},
    {"asset_used_by",
     "The assets whose files name a given asset, so you can see what deleting or rewriting it would "
     "break. Give 'path' (under assets/) or 'guid' (\"guid:<uuid>\"). Returns count and used_by, the paths "
     "of the assets that name it (the same answer as the Assets panel's Used by...). Only asset files are "
     "read: a script that loads by path, the start scene and files outside assets/ are not seen.",
     R"j({"type":"object","properties":{"path":{"type":"string","description":"Asset path under assets/"},"guid":{"type":"string","description":"guid:<uuid>"}}})j",
     asset_used_by},
    {"report_friction",
     "Write down something the engine made hard (a missing tool, a confusing error, a doc that was wrong, a "
     "workaround you had to invent) as a note in the project's friction.jsonl, so the engine's developers fix it. "
     "Call it when it happens, before working around it. blocking: you could not finish without a workaround.",
     R"j({"type":"object","properties":{"summary":{"type":"string","description":"One line: what was hard"},"area":{"type":"string","description":"The tool or engine area, e.g. render_frame, physics2d, docs"},)j"
     R"j("severity":{"type":"string","enum":["blocking","major","minor"],"default":"major"},"tool":{"type":"string","description":"The tool call that hit it"},)j"
     R"j("expected":{"type":"string"},"actual":{"type":"string"},"workaround":{"type":"string","description":"What you did instead"},"details":{"type":"string"}},)j"
     R"j("required":["summary","area"]})j",
     report_friction},
};

#undef MCE_SAVE_PROPS
#undef MCE_RENDER_FRAME_PROPS
#undef MCE_RENDER_COMMON_PROPS
#undef MCE_RF0
#undef MCE_RF1
#undef MCE_RC0
#undef MCE_RC1
#undef MCE_RC2
#undef MCE_RC3
#undef MCE_RC4

}  // namespace

Span<const ToolDef> agent_tool_table() { return Span<const ToolDef>(kAgentTools, sizeof(kAgentTools) / sizeof(kAgentTools[0])); }

}  // namespace mce::mcp
