#pragma once
// diff_scenes / format_diff — what changed between two versions of a scene (M-SCENEDIFF). Entities are matched by
// their persistent id; the result lists entities added, removed or changed (renamed, prefab link, components added,
// removed or with different fields, named by the reflected type and field names). A line-based text diff of a
// .mcscene is hard to read after a reorder or an id renumbering; this one speaks the document's own terms, for review,
// merge checks and agents. Headless, tested in CI.

#include "mce/containers/array.h"
#include "mce/cook/string.h"
#include "mce/editor/scene_document.h"

namespace mce::editor {

enum class DiffKind : u8 { Added, Removed, Changed };
enum class ComponentOp : u8 { Added, Removed, Changed };

struct ComponentChange {
    const refl::TypeRecord* type = nullptr;
    ComponentOp op = ComponentOp::Changed;
    u64 fields = 0;  // Changed: bit i = type->fields[i] differs (a prefab instance's override set counts)
};

struct EntityDiff {
    EntityId id = kNoEntity;
    DiffKind kind = DiffKind::Changed;
    FixedString<64> name;      // the name in `b` (in `a` for a removed entity)
    FixedString<64> old_name;  // Changed and renamed: the name in `a`; empty otherwise
    bool prefab_changed = false;
    Array<ComponentChange> components;  // Changed only
};

struct SceneDiff {
    bool header_changed = false;  // the scene's GUID, name or settings differ
    bool reordered = false;       // the entities both have appear in a different order
    Array<EntityDiff> entities;   // in `b`'s order, removed ones after the rest in `a`'s order

    bool empty() const { return !header_changed && !reordered && entities.empty(); }
};

// Compares `a` (before) with `b` (after).
SceneDiff diff_scenes(const SceneDocument& a, const SceneDocument& b);

// A short report, one line per entity: "+ Wall (id 3)", "- Torch (id 4)", "~ Door (id 5): renamed from "Gate";
// LocalTransform.position; +Sprite". Empty text for an empty diff.
cook::String format_diff(const SceneDiff& diff);

}  // namespace mce::editor
