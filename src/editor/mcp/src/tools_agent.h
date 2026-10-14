#pragma once
// The M-AGENT tools (ROADMAP M-AGENT: human-free development), one function per tool. Their table
// is agent_tool_table() in tools_agent.cpp; tool_table() lists them after the MVP tools.

#include "tools.h"

namespace mce::mcp {

// tools_render.cpp (SBA-48): frames from the real player, contact sheets, golden diffs.
bool render_frame(ToolCall& c);
bool render_sheet(ToolCall& c);
bool image_diff(ToolCall& c);

// tools_code.cpp (SBA-49): rebuild + hot reload of the game module, its ctest tests.
bool module_build(ToolCall& c);
bool module_test(ToolCall& c);

// tools_test.cpp (SBA-50): .mctest scenarios, bot playtests with a feel report.
bool test_run(ToolCall& c);
bool playtest(ToolCall& c);

// tools_docs.cpp (SBA-51): searching the shipped docs, what the engine reaches through what.
bool docs_search(ToolCall& c);
bool engine_capabilities(ToolCall& c);

// tools_content.cpp (SBA-52): procedural generators and the bundled CC0/OFL content library.
bool gen_palette(ToolCall& c);
bool gen_sprite(ToolCall& c);
bool gen_tileset(ToolCall& c);
bool gen_texture(ToolCall& c);
bool gen_sfx(ToolCall& c);
bool gen_music(ToolCall& c);
bool library_search(ToolCall& c);
bool library_add(ToolCall& c);

}  // namespace mce::mcp
