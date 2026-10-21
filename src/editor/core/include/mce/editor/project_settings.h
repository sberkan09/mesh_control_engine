#pragma once
// Project and scene settings as data (M-DATA-REACH 3): the project's <name>.mceproj (its title,
// start scene, games, plugins) and the open scene's WorldSettings component (gravity), edited the
// same way by MCP's project_get / project_set / scene_settings_* tools and the editor's Project
// Settings panel. Headless.

#include "mce/cook/json.h"
#include "mce/editor/editor_scene.h"
#include "mce/editor/project.h"
#include "mce/perf_budget.h"

namespace mce::editor {

// <root>/<something>.mceproj (the first, by name); empty when the project has none.
cook::String project_file_path(const EditorProject& project);

// Applies `patch`, an object of .mceproj members, to the file: "name", "title", "engine_version"
// (strings), "games", "plugins" (arrays of strings), "start_scene" (a scene's "guid:<uuid>" or
// <uuid>, or its path under assets/; stored as the bare GUID) and "budgets" (M-PERF: an object of
// targets, each an object of perf::budget_fields() limits; it replaces the file's "budgets"). Every
// member is checked before the file is written (atomically; other members and their order are kept).
// False with `err` set.
bool patch_project_file(const EditorProject& project, cook::JsonValue patch, cook::String& err);

// Checks a "budgets" value: {"mobile": {"draws": 300, "frame_ms": 33.3}, ...}. False with `err` set.
bool check_budgets(cook::JsonValue budgets, cook::String& err);
// The target's budget: perf::default_budget, with the .mceproj's "budgets" entry for the target over
// it. A project without a file or an entry gets the defaults; a malformed entry is an error.
bool project_budget(const EditorProject& project, perf::Target target, perf::Budget& out, cook::String& err);

// The scene's settings entity: the first one with a WorldSettings (kNoEntity when none).
EntityId world_settings_entity(const EditorScene& scene);
// The same, creating an entity named "World Settings" with default WorldSettings when missing.
EntityId ensure_world_settings(EditorScene& scene);

}  // namespace mce::editor
