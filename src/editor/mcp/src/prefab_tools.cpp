// Prefab tools (ROADMAP M-DATA-REACH step 2): an agent makes a prefab from an entity, writes an
// instance's overrides back into its prefab, and drops them again, through the same EditorScene
// and EditorSession calls the editor's inspector and hierarchy use. scene_open / scene_save take a
// .mcprefab to edit a prefab itself, and entity_create {"prefab": ...} places an instance
// (tools.cpp).

#include "mce/ecs/transform.h"
#include "tools.h"

namespace mce::mcp {

namespace {

bool fail(ToolCall& c, StringView msg) {
    c.error += msg;
    return false;
}

cook::String guid_ref(asset::Guid g) {
    cook::String s("guid:");
    s += asset::to_string(g).view();
    return s;
}

bool instance(ToolCall& c, editor::EntityId& id) {
    if (!tool_arg_entity(c, "entity", id)) return false;
    if (c.server.session().scene().doc().find(id)->prefab.is_null())
        return fail(c, "that entity is not a prefab instance (entity_create with 'prefab' places one)");
    return true;
}

// The components a call works on: the named one, or every component the instance overrides
// (its LocalTransform, the placement, aside).
bool overridden(ToolCall& c, editor::EntityId id, Array<const refl::TypeRecord*>& out) {
    const editor::DocEntity& e = *c.server.session().scene().doc().find(id);
    if (c.args.is_object() && c.args.get("component").valid()) {
        const refl::TypeRecord* t = nullptr;
        if (!tool_arg_component(c, "component", t)) return false;
        if (!e.find(*t)) return fail(c, "the instance has no override of that component");
        out.push_back(t);
        return true;
    }
    for (const editor::DocComponent& dc : e.components)
        if (dc.type != &refl::type_of<LocalTransform>() && dc.overrides != 0) out.push_back(dc.type);
    return true;
}

bool prefab_create_from(ToolCall& c) {
    editor::EntityId id = editor::kNoEntity;
    if (!tool_arg_entity(c, "entity", id)) return false;
    const cook::JsonValue path = c.args.get("path"), replace = c.args.get("replace");
    if (!path.is_string()) return fail(c, "argument 'path' must be a new file under assets/ like \"prefabs/enemy.mcprefab\"");
    if (replace.valid() && !replace.is_bool()) return fail(c, "argument 'replace' must be true or false");
    const StringView rel = path.as_string();
    if (rel.empty() || rel.size() >= 400 || rel[0] == '/' || rel.find('\\') != StringView::npos || rel.find(':') != StringView::npos ||
        rel.find("..") != StringView::npos)
        return fail(c, "argument 'path' must be a file path under assets/ (forward slashes, no '..')");
    editor::EditorSession& session = c.server.session();
    Result<asset::Guid> r = session.create_prefab_from(id, rel, !replace.valid() || replace.as_bool());
    if (!r) {
        FixedString<512> msg;
        format_error(r.error(), msg);
        return fail(c, msg.view());
    }
    c.out.begin_object().key("path").str(rel).key("guid").str(guid_ref(r.value()).view());
    if (!replace.valid() || replace.as_bool()) {
        // The instance that took the subtree's place: the newest entity with this prefab.
        editor::EntityId inst = editor::kNoEntity;
        for (const editor::DocEntity& e : session.scene().doc().entities())
            if (e.prefab == r.value() && e.id > inst) inst = e.id;
        c.out.key("instance").num_u(inst);
    }
    c.out.end_object();
    return true;
}

bool prefab_apply(ToolCall& c) {
    editor::EntityId id = editor::kNoEntity;
    Array<const refl::TypeRecord*> types;
    if (!instance(c, id) || !overridden(c, id, types)) return false;
    for (const refl::TypeRecord* t : types)
        if (t == &refl::type_of<LocalTransform>()) return fail(c, "an instance's LocalTransform is its placement: it stays the instance's own");
    editor::EditorScene& scene = c.server.session().scene();
    c.out.begin_object().key("prefab").str(guid_ref(scene.doc().find(id)->prefab).view()).key("applied").begin_array();
    for (const refl::TypeRecord* t : types) {
        if (Result<void> r = scene.apply_override(id, *t); !r) {
            FixedString<512> msg;
            format_error(r.error(), msg);
            c.error += t->name;
            c.error += ": ";
            return fail(c, msg.view());
        }
        c.out.str(t->name);
    }
    c.out.end_array().end_object();
    return true;
}

bool prefab_revert(ToolCall& c) {
    editor::EntityId id = editor::kNoEntity;
    Array<const refl::TypeRecord*> types;
    if (!instance(c, id) || !overridden(c, id, types)) return false;
    editor::EditorScene& scene = c.server.session().scene();
    c.out.begin_object().key("reverted").begin_array();
    for (const refl::TypeRecord* t : types)
        if (scene.revert_override(id, *t)) c.out.str(t->name);
    c.out.end_array().end_object();
    return true;
}

const ToolDef kPrefabTools[] = {
    {"prefab_create_from",
     "Save an entity and its ChildOf subtree as a new .mcprefab (fresh GUID; the root first, references that leave "
     "the subtree nulled) and cook it. replace (default true): the subtree becomes an instance of the new prefab at "
     "the same place, parent and transform, one undo step. Returns the prefab's \"guid:<uuid>\" and the instance id.",
     R"json({"type":"object","properties":{"entity":{"type":["integer","string"],"description":"Entity id, or its name when unique"},"path":{"type":"string","description":"New file under assets/, e.g. prefabs/enemy.mcprefab"},"replace":{"type":"boolean","default":true}},"required":["entity","path"]})json",
     prefab_create_from},
    {"prefab_apply",
     "Write a prefab instance's overrides (of 'component', or of every overridden component except LocalTransform) "
     "into its .mcprefab root and re-cook it: every instance sees the values, and they stop being this instance's "
     "overrides. Entity references stay overrides.",
     R"json({"type":"object","properties":{"entity":{"type":["integer","string"],"description":"Entity id, or its name when unique"},"component":{"type":"string"}},"required":["entity"]})json",
     prefab_apply},
    {"prefab_revert",
     "Drop a prefab instance's overrides (of 'component', or of every component except LocalTransform): the fields "
     "take the prefab's values again, and a component the prefab root lacks is removed.",
     R"json({"type":"object","properties":{"entity":{"type":["integer","string"],"description":"Entity id, or its name when unique"},"component":{"type":"string"}},"required":["entity"]})json",
     prefab_revert},
};

}  // namespace

Span<const ToolDef> prefab_tool_table() { return Span<const ToolDef>(kPrefabTools, sizeof(kPrefabTools) / sizeof(kPrefabTools[0])); }

}  // namespace mce::mcp
