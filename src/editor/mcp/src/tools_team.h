#pragma once
// Several agents on one project (ROADMAP M-AGENT step 7, tools_team.cpp). Each agent runs its own
// server (its own MCP session and undo history); they meet in the project folder: an event feed
// (.mce/events.jsonl), asset locks (.mce/locks/), a shared task file (tasks.mctasks) and a
// three-way merge of scene files by entity, so an edit is never silently lost. Private to
// mce_mcp_core.

#include "tools.h"

namespace mce::mcp {

// Server::call_tool's hooks around every tool: before, a write to a file another agent locked is
// refused (false, with c.error); after a successful write, the event feed gets a line.
bool team_before_tool(ToolCall& c, StringView tool);
void team_after_tool(ToolCall& c, StringView tool);

// scene_save found the open scene's file changed on disk: merges that version's edits into the open
// document (base: scene_file().base). False with the conflicts in c.error when both sides changed
// the same component of an entity and no `resolve` ("ours" / "theirs") was given.
bool merge_scene_from_disk(ToolCall& c, StringView abs);

struct MergeConflict {
    i64 entity = 0;           // entity id (0: a scene-level key)
    cook::String name;        // the entity's name
    cook::String field;       // a component name, or a key of the entity / scene
    cook::String what;        // "both changed", "you changed, they deleted", ...
};

struct MergeResult {
    cook::String text;  // the merged scene
    Array<MergeConflict> conflicts;
    u32 taken_from_theirs = 0;  // fields or entities their version brought in
    u32 renumbered = 0;         // our new entities whose id they had also used
};

// Three-way merge of .mcscene / .mcprefab texts by entity id, component by component. `resolve`:
// "ours" or "theirs" settles conflicts that way; empty records them (theirs is kept in `text`).
// False when a text does not parse.
bool merge_entity_files(StringView base, StringView ours, StringView theirs, StringView resolve, MergeResult& out, cook::String& err);

}  // namespace mce::mcp
