// Hierarchy tools (M-HIERTOOLS step 1): reparent, rename, group, ungroup and reset the transform of entities of the
// open scene. The same commands as the editor's Hierarchy and Edit menus, each one undo step (scene_undo).

#include "mce/editor/group_entities.h"
#include "mce/editor/reset_transform.h"
#include "tool_args.h"
#include "tools_agent.h"

namespace mce::mcp {

namespace {

Span<const editor::EntityId> span_of(const Array<editor::EntityId>& a) {
    return Span<const editor::EntityId>(a.data(), a.size());
}

bool entity_set_parent(ToolCall& c) {
    editor::EntityId child = editor::kNoEntity, parent = editor::kNoEntity;
    if (!arg_entity(c, "entity", child)) return false;
    const cook::JsonValue p = arg(c, "parent");
    if (p.valid() && !p.is_null() && !resolve_entity(c, "parent", p, parent)) return false;
    editor::EditorScene& s = scene(c);
    if (parent != editor::kNoEntity && (parent == child || s.is_ancestor(child, parent)))
        return fail(c, "parent: an entity cannot become a child of itself or of one of its own children");
    c.out.begin_object().key("changed").boolean(s.set_parent(child, parent)).end_object();  // keeps the world transform
    return true;
}

bool entity_rename(ToolCall& c) {
    editor::EntityId id = editor::kNoEntity;
    StringView name;
    if (!arg_entity(c, "entity", id) || !arg_string(c, "name", name, true)) return false;
    if (name.empty()) return fail(c, "argument 'name' must not be empty");
    c.out.begin_object().key("changed").boolean(scene(c).rename_entity(id, name)).end_object();
    return true;
}

bool entity_group(ToolCall& c) {
    Array<editor::EntityId> ids;
    if (!arg_entities(c, ids, 1)) return false;
    const editor::EntityId g = editor::group_entities(scene(c), span_of(ids));
    if (g == editor::kNoEntity) return fail(c, "nothing was grouped");
    c.out.begin_object().key("group").num_u(g).end_object();
    return true;
}

bool entity_ungroup(ToolCall& c) {
    Array<editor::EntityId> ids;
    if (!arg_entities(c, ids, 1)) return false;
    const Array<editor::EntityId> freed = editor::ungroup_entities(scene(c), span_of(ids));
    c.out.begin_object().key("freed").begin_array();
    for (editor::EntityId id : freed) c.out.num_u(id);
    c.out.end_array().end_object();
    return true;
}

bool entity_reset_transform(ToolCall& c) {
    Array<editor::EntityId> ids;
    if (!arg_entities(c, ids, 1)) return false;
    c.out.begin_object().key("changed").num_u(editor::reset_transforms(scene(c), span_of(ids))).end_object();
    return true;
}

#define MCE_ENT R"j("entity":{"type":["integer","string"],"description":"Entity id, or its name when unique"})j"
#define MCE_ENTS \
    R"j("entities":{"type":"array","items":{"type":["integer","string"]},"description":"Entity ids, or names when unique"})j"

const ToolDef kHierarchyTools[] = {
    {"entity_set_parent",
     "Make 'entity' a child of 'parent' (left out or null: a root), keeping where it sits in the scene (its "
     "LocalTransform is "
     "recomputed). Refuses a loop. Returns changed (false: it already had that parent). One undo step.",
     R"j({"type":"object","properties":{)j" MCE_ENT
     R"j(,"parent":{"type":["integer","string","null"],"description":"The new parent, or null for the scene root"}},"required":["entity"]})j",
     entity_set_parent},
    {"entity_rename", "Rename an entity. Returns changed (false: it already had that name). One undo step.",
     R"j({"type":"object","properties":{)j" MCE_ENT R"j(,"name":{"type":"string"}},"required":["entity","name"]})j",
     entity_rename},
    {"entity_group",
     "Wrap entities in a new empty \"Group\" entity under the parent of the first one; the top-level ones move under "
     "it and "
     "keep their place in the scene. Returns the group's id. One undo step.",
     R"j({"type":"object","properties":{)j" MCE_ENTS R"j(},"required":["entities"]})j", entity_group},
    {"entity_ungroup",
     "Move the children of each listed entity up to its parent and delete the entity (the reverse of entity_group). "
     "Returns "
     "freed, the children that moved. One undo step; nothing changes when none had a child.",
     R"j({"type":"object","properties":{)j" MCE_ENTS R"j(},"required":["entities"]})j", entity_ungroup},
    {"entity_reset_transform",
     "Put the LocalTransform of the entities back to the identity (position 0, no rotation, scale 1) relative to their "
     "parent. "
     "Returns changed (how many differed). One undo step.",
     R"j({"type":"object","properties":{)j" MCE_ENTS R"j(},"required":["entities"]})j", entity_reset_transform},
};

}  // namespace

Span<const ToolDef> hierarchy_tool_table() {
    return Span<const ToolDef>(kHierarchyTools, sizeof(kHierarchyTools) / sizeof(kHierarchyTools[0]));
}

}  // namespace mce::mcp
