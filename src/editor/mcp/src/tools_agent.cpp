// The M-AGENT tool table (ROADMAP M-AGENT). Kept apart from the MVP table in tools.cpp so the two
// tracks that add tools touch different files; tool_table() concatenates them.

#include "tools_agent.h"

namespace mce::mcp {

namespace {

// Shared schema pieces (render_frame's arguments, also nested in image_diff's 'render').
#define MCE_RC0 "\"scene\":{\"type\":\"string\",\"description\":\"Render this saved scene (path under assets/) instead of the open one\"},"
#define MCE_RC1 "\"input\":{\"description\":\"Scripted input as run_headless takes it: a script string (\\\"10-70 Move=1,0 Jump\\\") or an array of {from, to, actions}\",\"type\":[\"string\",\"array\"]},"
#define MCE_RC2 "\"camera\":{\"type\":\"object\",\"description\":\"Render from another camera (render only, the sim is unchanged): {\\\"entity\\\": id or name} of a camera entity in the open scene, {\\\"index\\\": n} (scene entity index), {\\\"position\\\": [x,y,z], \\\"target\\\": [x,y,z], \\\"fov\\\": deg} (free 3D) or {\\\"center\\\": [x,y], \\\"zoom\\\": z} (free 2D)\"},"
#define MCE_RC3 "\"safe_area\":{\"type\":\"object\",\"description\":\"Pretend the screen has these safe-area insets in pixels {left, top, right, bottom}: UI lays out inside them and the image outlines them\"},"
#define MCE_RC4 "\"software\":{\"type\":\"boolean\",\"description\":\"Force the offscreen software renderer (automatic when there is no display)\"}"
#define MCE_RENDER_COMMON_PROPS MCE_RC0 MCE_RC1 MCE_RC2 MCE_RC3 MCE_RC4
#define MCE_RF0 "\"tick\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":36000,\"default\":1,\"description\":\"Capture the first frame after this sim tick\"},"
#define MCE_RF1 "\"width\":{\"type\":\"integer\",\"minimum\":64,\"maximum\":3840,\"default\":960},\"height\":{\"type\":\"integer\",\"minimum\":64,\"maximum\":3840,\"default\":540},"
#define MCE_RENDER_FRAME_PROPS MCE_RF0 MCE_RF1 MCE_RENDER_COMMON_PROPS

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
     "<game>_headless_300 smoke test). Returns each test's result and the output's end when one failed.",
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
     R"j({"type":"object","properties":{"bot":{"type":"object","description":"{kind: walk_to|explore|masher, actor: entity, target: [x,y(,z)] or entity (walk_to), plane: xy|x|xz (default xy; x = side view with Jump, xz = 3D ground), reach: m (0.5), seed, move: action (Move), jump: action (Jump), actions: [names] (masher), period: ticks (8)}"},)j"
     R"j("scene":{"type":"string","description":"A saved scene under assets/ (default: the open scene)"},)j"
     R"j("ticks":{"type":"integer","minimum":1,"maximum":36000,"default":1800},)j"
     R"j("until":{"type":"object","description":"Stop early when this check holds (a field, alive or event check)"},)j"
     R"j("death":{"type":"object","description":"Count a death each time this check becomes true, e.g. {entity, field: Health.hp, <=: 0}"},)j"
     R"j("assert":{"type":"array","items":{"type":"object"},"description":"Checks as in .mctest files"},)j"
     R"j("trace":{"type":"object","description":"{every: ticks, fields: [{entity, field}]}: sample fields during the run"}},"required":["bot"]})j",
     playtest},
};

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
