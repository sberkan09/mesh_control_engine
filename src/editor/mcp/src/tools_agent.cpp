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
     R"(,"save":{"type":"string","description":"Also write the frame as a PNG at this path inside the project folder, e.g. shots/title.png"}}})",
     render_frame},
    {"render_sheet",
     "A contact sheet: frames at several ticks (columns) from one or more cameras (rows) in one image, each cell labelled "
     "with its tick. One player run per camera.",
     R"({"type":"object","properties":{"ticks":{"type":"array","items":{"type":"integer"},"minItems":1,"maxItems":16,"description":"Ascending sim ticks"},)"
     R"("cameras":{"type":"array","maxItems":4,"items":{"type":"object"},"description":"Camera objects as render_frame's 'camera'; default: the scene's own"},)"
     R"("cell_width":{"type":"integer","minimum":64,"maximum":3840,"default":320},"cell_height":{"type":"integer","minimum":64,"maximum":3840,"default":180},)" MCE_RENDER_COMMON_PROPS
     R"(},"required":["ticks"]})",
     render_sheet},
    {"image_diff",
     "Compare a frame with a golden PNG in the project: 'image' (a PNG in the project) or 'render' (render_frame's arguments). "
     "Returns the score (mean channel difference, 0..1), the changed fraction and pass (score <= max_score), with a heat map "
     "image. With no golden yet, or update: true, the image becomes the golden.",
     R"({"type":"object","properties":{"golden":{"type":"string","description":"PNG path inside the project folder, e.g. goldens/title.png"},)"
     R"("image":{"type":"string","description":"PNG path inside the project folder to compare"},)"
     "\"render\":{\"type\":\"object\",\"description\":\"render_frame arguments: the frame to compare\",\"properties\":{" MCE_RENDER_FRAME_PROPS "}},"
     R"("threshold":{"type":"integer","minimum":0,"maximum":255,"default":8,"description":"Channel difference a pixel may have and still count as unchanged"},)"
     R"("max_score":{"type":"number","minimum":0,"maximum":1,"default":0.01},"update":{"type":"boolean","description":"Write the image as the new golden"}},"required":["golden"]})",
     image_diff},
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
