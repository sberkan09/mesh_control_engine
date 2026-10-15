#pragma once
// The M-AGENT tools (ROADMAP M-AGENT: human-free development), one function per tool. Their table
// is agent_tool_table() in tools_agent.cpp; tool_table() lists them after the MVP tools.

#include "tools.h"

namespace mce::mcp {

// tools_render.cpp (SBA-48): frames from the real player, contact sheets, golden diffs.
bool render_frame(ToolCall& c);
bool render_sheet(ToolCall& c);
bool image_diff(ToolCall& c);
// ... and a timeline paused at a time (M-CINEMA step 6).
bool timeline_preview(ToolCall& c);

// tools_code.cpp (SBA-49): rebuild + hot reload of the game module, its ctest tests.
bool module_build(ToolCall& c);
bool module_test(ToolCall& c);
// ... and the release zip (SBA-53): full build, smoke run, `mce ship`.
bool ship_build(ToolCall& c);

// tools_check.cpp (SBA-53): the checks before shipping, as findings.
bool project_check(ToolCall& c);

// tools_test.cpp (SBA-50): .mctest scenarios, bot playtests with a feel report.
bool test_run(ToolCall& c);
bool playtest(ToolCall& c);

// tools_docs.cpp (SBA-51): searching the shipped docs, what the engine reaches through what.
bool docs_search(ToolCall& c);
bool engine_capabilities(ToolCall& c);

// tools_content.cpp (SBA-52): procedural generators and the bundled CC0/OFL content library.
bool gen_palette(ToolCall& c);
bool gen_sprite(ToolCall& c);
bool gen_pixels(ToolCall& c);
bool gen_mesh(ToolCall& c);
bool gen_tileset(ToolCall& c);
bool gen_texture(ToolCall& c);
bool gen_sfx(ToolCall& c);
bool gen_music(ToolCall& c);
bool library_search(ToolCall& c);
bool library_add(ToolCall& c);

// tools_team.cpp (SBA-54): several agents on one project: the event feed, locks, tasks, scenes.
bool project_watch(ToolCall& c);
bool asset_lock(ToolCall& c);
bool asset_unlock(ToolCall& c);
bool asset_locks(ToolCall& c);
bool task_list(ToolCall& c);
bool task_add(ToolCall& c);
bool task_claim(ToolCall& c);
bool task_update(ToolCall& c);
bool scene_switch(ToolCall& c);
bool scene_documents(ToolCall& c);

// tools_friction.cpp (SBA-55): friction notes for the engine's backlog.
bool report_friction(ToolCall& c);

}  // namespace mce::mcp
