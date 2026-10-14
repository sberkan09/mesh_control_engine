// The MVP tool table (editor-mcp.md §4): discover, read, edit, persist, build, verify, console.
// Edits go through EditorScene (the mutation funnel); values are validated on a copy first, so a
// failed call changes nothing.

#include "tools.h"

#include "agent_sim.h"
#include "tools_docs.h"
#include "tool_args.h"
#include "tools_team.h"

#include <algorithm>
#include <cstring>

#include "mce/convar.h"
#include "mce/cook/fs.h"
#include "mce/cook/scene_text.h"
#include "mce/ecs/transform.h"
#include "mce/input/script.h"
#include "mce/log.h"
#include "mce/mcp/headless_run.h"
#include "mce/mcp/values.h"

namespace mce::mcp {

namespace {

using editor::DocComponent;
using editor::DocEntity;
using editor::EditorScene;
using editor::EntityId;

constexpr u64 kDefaultTicks = 300;
constexpr u64 kMaxTicks = 36000;  // ten minutes of sim at 60 Hz
constexpr u64 kMaxLogLines = 200;
constexpr usize kMaxDiagnostics = 200;

}  // namespace

// ---------------------------------------------------------------- helpers (tool_args.h)

bool fail(ToolCall& c, StringView msg) {
    c.error += msg;
    return false;
}

cook::JsonValue arg(const ToolCall& c, const char* name) { return c.args.is_object() ? c.args.get(name) : cook::JsonValue(); }

bool arg_missing(ToolCall& c, const char* name, const char* expected) {
    c.error += "argument '";
    c.error += name;
    c.error += "' must be ";
    c.error += expected;
    return false;
}

bool arg_string(ToolCall& c, const char* name, StringView& out, bool required) {
    const cook::JsonValue v = arg(c, name);
    if (!v.valid() && !required) return true;
    if (!v.is_string()) return arg_missing(c, name, "a string");
    out = v.as_string();
    return true;
}

bool arg_uint(ToolCall& c, const char* name, u64& out, u64 lo, u64 hi, bool required) {
    const cook::JsonValue v = arg(c, name);
    if (!v.valid() && !required) return true;
    if (!v.is_integer() || v.as_number() < static_cast<f64>(lo) || v.as_number() > static_cast<f64>(hi)) {
        c.error += "argument '";
        c.error += name;
        cook::appendf(c.error, "' must be an integer in [%llu, %llu]", static_cast<unsigned long long>(lo),
                      static_cast<unsigned long long>(hi));
        return false;
    }
    out = static_cast<u64>(v.as_number());
    return true;
}

bool arg_bool(ToolCall& c, const char* name, bool& out) {
    const cook::JsonValue v = arg(c, name);
    if (!v.valid()) return true;
    if (!v.is_bool()) return arg_missing(c, name, "true or false");
    out = v.as_bool();
    return true;
}

EditorScene& scene(ToolCall& c) { return c.server.session().scene(); }

// An entity argument: its id (an integer) or its name (a string; the name must be unique in the
// scene). Ids are persistent (saved in the file), names are what a client wrote.
bool resolve_entity(ToolCall& c, const char* arg_name, const cook::JsonValue& v, EntityId& out) {
    const editor::SceneDocument& doc = scene(c).doc();
    if (v.is_string()) {
        const StringView name = v.as_string();
        EntityId found = editor::kNoEntity;
        u32 matches = 0;
        for (const DocEntity& e : doc.entities()) {
            if (e.name.view() != name) continue;
            found = e.id;
            ++matches;
        }
        if (matches == 1) {
            out = found;
            return true;
        }
        c.error += "argument '";
        c.error += arg_name;
        c.error += matches == 0 ? "': no entity named '" : "': more than one entity is named '";
        c.error += name;
        c.error += matches == 0 ? "' in the scene (scene_list_entities lists them)" : "': use its id (scene_list_entities lists them)";
        return false;
    }
    if (!v.is_integer() || v.as_number() < 1.0 || v.as_number() > 4294967294.0) {
        c.error += "argument '";
        c.error += arg_name;
        c.error += "' must be an entity id (an integer) or an entity name (a string)";
        return false;
    }
    const u64 id = static_cast<u64>(v.as_number());
    if (!doc.find(static_cast<EntityId>(id))) {
        cook::appendf(c.error, "no entity with id %llu in the scene (scene_list_entities lists them)",
                      static_cast<unsigned long long>(id));
        return false;
    }
    out = static_cast<EntityId>(id);
    return true;
}

bool arg_entity(ToolCall& c, const char* name, EntityId& out) {
    const cook::JsonValue v = arg(c, name);
    if (!v.valid()) return arg_missing(c, name, "an entity id (integer) or name (string)");
    return resolve_entity(c, name, v, out);
}

bool arg_component(ToolCall& c, const char* name, const refl::TypeRecord*& out) {
    StringView type_name;
    if (!arg_string(c, name, type_name, true)) return false;
    const refl::TypeRecord* t = refl::find_type(StringId(type_name));
    if (!t || !t->is_component()) {
        c.error += "unknown component type '";
        c.error += type_name;
        c.error += "' (refl_list_components lists them)";
        return false;
    }
    out = t;
    return true;
}

bool need_project(ToolCall& c) {
    if (c.server.session().project().is_open()) return true;
    return fail(c, "no project is open: start mce_mcp with --project <dir>");
}

// A scene path as the tools accept it: relative to assets/, forward slashes, no "." / ".."
// segments, ending in .mcscene. Everything a tool writes stays inside the project.
bool scene_path(Server& server, StringView rel, cook::String& abs, cook::String& err) {
    if (!server.session().project().is_open()) {
        err += "no project is open: start mce_mcp with --project <dir>";
        return false;
    }
    bool ok = !rel.empty() && rel.size() < 400 && rel[0] != '/' && rel.find('\\') == StringView::npos &&
              rel.find(':') == StringView::npos &&
              ((rel.size() > 8 && rel.substr(rel.size() - 8) == ".mcscene") || (rel.size() > 9 && rel.substr(rel.size() - 9) == ".mcprefab"));
    for (usize start = 0; ok && start <= rel.size();) {
        usize end = rel.find('/', start);
        if (end == StringView::npos) end = rel.size();
        const StringView seg = rel.substr(start, end - start);
        if (seg.empty() || seg == "." || seg == "..") ok = false;
        start = end + 1;
    }
    if (!ok) {
        err += "invalid scene path '";
        err += rel;
        err += "': use a path under assets/ like \"scenes/level.mcscene\" or \"prefabs/enemy.mcprefab\" (forward slashes, no '..')";
        return false;
    }
    abs = server.session().project().asset_path(rel);
    return true;
}

namespace {

cook::String guid_ref(asset::Guid g) {
    cook::String s("guid:");
    s += asset::to_string(g).view();
    return s;
}

void write_diag(JsonWriter& w, const cook::Diagnostic& d) {
    w.begin_object();
    w.key("severity").str(d.severity == cook::Severity::Error ? "error" : d.severity == cook::Severity::Warning ? "warning" : "info");
    w.key("file").str(d.file.view());
    w.key("line").num_u(d.line);
    w.key("col").num_u(d.col);
    w.key("stage").str(d.stage);
    w.key("message").str(d.message.view());
    w.end_object();
}

}  // namespace

void append_diags(cook::String& out, const cook::DiagList& diags, usize max) {
    usize n = 0;
    for (const cook::Diagnostic& d : diags.all()) {
        if (d.severity != cook::Severity::Error || n++ == max) continue;
        out += '\n';
        cook::format_diagnostic(d, out);
    }
}

const char* level_name(log::Level l) {
    switch (l) {
        case log::Level::Trace: return "trace";
        case log::Level::Debug: return "debug";
        case log::Level::Info: return "info";
        case log::Level::Warn: return "warn";
        case log::Level::Error: return "error";
        case log::Level::Critical: return "critical";
        default: return "off";
    }
}

// Console lines logged after `since`, oldest first.
void read_log(u64 since, Array<log::ConsoleLine>& out) {
    out.resize(log::kConsoleRingSize);
    out.resize(log::read_console(since, out.span()));
}

void write_log_lines(JsonWriter& w, Span<const log::ConsoleLine> lines, usize max) {
    w.begin_array();
    for (usize i = lines.size() > max ? lines.size() - max : 0; i < lines.size(); ++i) {
        cook::String s("[");
        s += level_name(lines[i].level);
        s += "] ";
        s += lines[i].text.view();
        w.str(s.view());
    }
    w.end_array();
}

namespace {

void write_component_object(JsonWriter& w, const DocComponent& c) {
    cook::String text;
    write_component(*c.type, c.data(), text);
    w.raw(text.view());
}

struct Value {
    const refl::TypeRecord* type = nullptr;
    Array<u64> storage;
    void* data() { return storage.data(); }
};

void init_value(Value& v, const refl::TypeRecord& t, const void* from) {
    v.type = &t;
    v.storage.resize(t.size / 8 + 1, 0);
    if (from) std::memcpy(v.storage.data(), from, t.size);
    else if (t.construct_default) t.construct_default(v.storage.data());
}

void prefix_error(ToolCall& c, const refl::TypeRecord& t, cook::String& err) {
    c.error += t.name;
    c.error += ": ";
    c.error += err.view();
}

}  // namespace

void progress(Server& server, cook::JsonValue token, u64 done, u64 total) {
    if (!token.valid()) return;
    cook::String params;
    JsonWriter w(params);
    w.begin_object().key("progressToken").copy(token).key("progress").num_u(done).key("total").num_u(total).end_object();
    server.notify("notifications/progress", params.view());
}

namespace {

bool refuse_unsaved(ToolCall& c, const char* what) {
    bool force = false;
    if (!arg_bool(c, "force", force)) return true;
    if (!scene(c).dirty() || force) return false;
    c.error += "the open scene has unsaved changes: scene_save first, or pass force: true to ";
    c.error += what;
    c.error += " anyway";
    return true;
}

// ---------------------------------------------------------------- discover

bool refl_list_components(ToolCall& c) {
    c.out.begin_object().key("components").begin_array();
    for (const refl::TypeRecord* t : refl::all()) {
        if (!t->is_component()) continue;
        c.out.begin_object().key("name").str(t->name).key("fields").num_u(t->fields.size()).end_object();
    }
    c.out.end_array().end_object();
    return true;
}

// A component, or a reflected value type a component or a document nests (ColliderShape,
// UiAnchors, UiStyle): the same description either way.
bool refl_describe_component(ToolCall& c) {
    StringView type_name;
    if (!arg_string(c, "name", type_name, true)) return false;
    const refl::TypeRecord* t = refl::find_type(StringId(type_name));
    if (!t) {
        c.error += "unknown component type '";
        c.error += type_name;
        c.error += "' (refl_list_components lists them)";
        return false;
    }
    describe_type(c.out, *t);
    return true;
}

// ---------------------------------------------------------------- read

bool scene_info(ToolCall& c) {
    const EditorScene& s = scene(c);
    c.out.begin_object();
    c.out.key("name").str(s.doc().name());
    c.out.key("guid").str(asset::to_string(s.doc().guid()).view());
    c.out.key("path");
    if (c.server.scene_file().rel.empty()) c.out.null();
    else c.out.str(c.server.scene_file().rel.view());
    c.out.key("dirty").boolean(s.dirty());
    c.out.key("revision").num_u(s.revision());
    c.out.key("entities").num_u(s.doc().size());
    c.out.key("project_open").boolean(c.server.session().project().is_open());
    c.out.end_object();
    return true;
}

bool scene_list_entities(ToolCall& c) {
    const refl::TypeRecord* filter = nullptr;
    if (arg(c, "component").valid() && !arg_component(c, "component", filter)) return false;
    c.out.begin_object().key("entities").begin_array();
    for (const DocEntity& e : scene(c).doc().entities()) {
        if (filter && !e.find(*filter)) continue;
        c.out.begin_object().key("id").num_u(e.id).key("name").str(e.name.view());
        if (!e.prefab.is_null()) c.out.key("prefab").str(guid_ref(e.prefab).view());
        c.out.key("components").begin_array();
        for (const DocComponent& comp : e.components) c.out.str(comp.type->name);
        c.out.end_array().end_object();
    }
    c.out.end_array().end_object();
    return true;
}

bool scene_get_entity(ToolCall& c) {
    EntityId id = editor::kNoEntity;
    if (!arg_entity(c, "entity", id)) return false;
    const DocEntity& e = *scene(c).doc().find(id);
    c.out.begin_object();
    c.out.key("id").num_u(e.id);
    c.out.key("name").str(e.name.view());
    c.out.key("index").num_i(scene(c).doc().index_of(id));
    if (!e.prefab.is_null()) c.out.key("prefab").str(guid_ref(e.prefab).view());
    c.out.key("components").begin_object();
    for (const DocComponent& comp : e.components) {
        c.out.key(comp.type->name);
        write_component_object(c.out, comp);
    }
    c.out.end_object().end_object();
    return true;
}

bool asset_list(ToolCall& c) {
    if (!need_project(c)) return false;
    StringView type;
    if (!arg_string(c, "type", type, false)) return false;
    c.out.begin_object().key("assets").begin_array();
    for (const editor::ProjectAsset& a : c.server.session().project().assets()) {
        const char* tname = editor::asset_type_name(a.type);
        if (!type.empty() && type != tname) continue;
        c.out.begin_object().key("guid").str(guid_ref(a.guid).view()).key("path").str(a.path.view()).key("type").str(tname).end_object();
    }
    c.out.end_array().end_object();
    return true;
}

// Replaces the "settings" object of an asset's .mceasset sidecar (cook_run with adopt writes an
// empty one: a normal map or a LUT needs {"srgb": false}). The importer validates the keys at the
// next cook; the catalog is rescanned so the cook sees the change.
bool asset_set_import_settings(ToolCall& c) {
    if (!need_project(c)) return false;
    StringView path, guid_text;
    if (!arg_string(c, "path", path, false) || !arg_string(c, "guid", guid_text, false)) return false;
    const cook::JsonValue settings = arg(c, "settings");
    if (!settings.is_object()) return arg_missing(c, "settings", "an object of importer settings, e.g. {\"srgb\": false}");
    if (path.empty() == guid_text.empty()) return fail(c, "give one of 'path' (under assets/) or 'guid' (\"guid:<uuid>\")");
    editor::EditorProject& project = c.server.session().project();
    const editor::ProjectAsset* found = nullptr;
    if (!path.empty()) {
        for (const editor::ProjectAsset& a : project.assets())
            if (a.path.view() == path) found = &a;
        if (!found) return fail(c, "no registered asset at that path (asset_list lists them; a new file needs cook_run with \"adopt\": true first)");
    } else {
        if (guid_text.size() > 5 && guid_text.substr(0, 5) == "guid:") guid_text = guid_text.substr(5);
        const Result<asset::Guid> g = asset::parse_guid(guid_text);
        if (!g) return arg_missing(c, "guid", "\"guid:<uuid>\"");
        found = project.find(g.value());
        if (!found) return fail(c, "no registered asset with that guid (asset_list lists them)");
    }
    cook::String sidecar = project.asset_path(found->path.view());
    sidecar += ".mceasset";
    cook::String text;
    if (!cook::fs::read_text(sidecar.view(), text)) return fail(c, "could not read the asset's .mceasset sidecar");
    cook::JsonDoc doc;
    cook::JsonError je;
    if (!doc.parse(text.view(), je) || !doc.root().is_object()) return fail(c, "the asset's .mceasset sidecar is not a JSON object");
    // Every other top-level member is kept as it is (guid, importer, importer_version, ...).
    cook::String out;
    JsonWriter w(out);
    w.begin_object();
    const cook::JsonValue root = doc.root();
    for (u32 i = 0; i < root.size(); ++i) {
        if (root.key_at(i) == "settings") continue;
        w.key(root.key_at(i)).copy(root.at(i));
    }
    w.key("settings").copy(settings);
    w.end_object();
    out += '\n';
    if (!cook::fs::write_file_atomic(sidecar.view(), Span<const byte>(reinterpret_cast<const byte*>(out.c_str()), out.size())))
        return fail(c, "could not write the asset's .mceasset sidecar");
    const asset::Guid guid = found->guid;  // rescan invalidates `found`
    const cook::String rel = found->path;
    project.rescan();
    c.out.begin_object().key("guid").str(guid_ref(guid).view()).key("path").str(rel.view()).key("settings").copy(settings).end_object();
    return true;
}

// ---------------------------------------------------------------- edit

bool entity_create(ToolCall& c) {
    StringView name;
    bool transform = true;
    u64 at = 0;
    const bool has_at = arg(c, "at").valid();
    if (!arg_string(c, "name", name, true) || !arg_bool(c, "transform", transform) ||
        !arg_uint(c, "at", at, 0, scene(c).doc().size(), false))
        return false;
    if (name.size() >= 64) return fail(c, "argument 'name' must be shorter than 64 bytes");
    const cook::JsonValue comps = arg(c, "components");
    if (comps.valid() && !comps.is_object()) return arg_missing(c, "components", "an object of {\"Type\": {fields}}");
    // A prefab instance: the prefab must be a registered .mcprefab; components override its root.
    asset::Guid prefab;
    if (const cook::JsonValue p = arg(c, "prefab"); p.valid()) {
        const editor::ProjectAsset* a = nullptr;
        StringView ref = p.is_string() ? p.as_string() : StringView();
        if (ref.size() > 5 && ref.substr(0, 5) == "guid:") ref = ref.substr(5);
        if (const Result<asset::Guid> g = asset::parse_guid(ref); g && c.server.session().project().is_open())
            a = c.server.session().project().find(g.value());
        if (!a || a->type != asset::ChunkType::Prefab)
            return fail(c, "argument 'prefab' must name a .mcprefab of the project as \"guid:<uuid>\" (asset_list with type Prefab lists them)");
        prefab = a->guid;
        if (!transform) return fail(c, "a prefab instance always has its own LocalTransform: drop 'transform': false");
    }

    // Validate every value before the entity exists: a failed call changes nothing.
    Array<Value> values;
    for (u32 i = 0; comps.valid() && i < comps.size(); ++i) {
        const refl::TypeRecord* t = refl::find_type(StringId(comps.key_at(i)));
        if (!t || !t->is_component()) {
            c.error += "unknown component type '";
            c.error += comps.key_at(i);
            c.error += "'";
            return false;
        }
        Value& v = values.emplace_back();
        init_value(v, *t, nullptr);
        cook::String err;
        if (!apply_patch(*t, comps.at(i), v.data(), scene(c).doc(), err)) {
            prefix_error(c, *t, err);
            return false;
        }
    }
    EditorScene& s = scene(c);
    const i32 where = has_at ? static_cast<i32>(at) : -1;
    const EntityId id = prefab.is_null() ? s.create_entity(name, transform, where) : s.create_prefab_instance(name, prefab, where);
    for (Value& v : values) {
        s.add_component(id, *v.type);
        s.set_component(id, *v.type, v.data());
    }
    c.out.begin_object().key("id").num_u(id).key("index").num_i(s.doc().index_of(id));
    if (!prefab.is_null()) c.out.key("prefab").str(guid_ref(prefab).view());
    c.out.end_object();
    return true;
}

bool entity_delete(ToolCall& c) {
    EntityId id = editor::kNoEntity;
    if (!arg_entity(c, "entity", id)) return false;
    scene(c).destroy_entity(id);
    c.out.begin_object().key("deleted").num_u(id).end_object();
    return true;
}

bool entity_duplicate(ToolCall& c) {
    EntityId id = editor::kNoEntity;
    if (!arg_entity(c, "entity", id)) return false;
    Array<EntityId> created;
    const EntityId ids[1] = {id};
    if (!scene(c).duplicate_entities(Span<const EntityId>(ids, 1), &created)) return fail(c, "duplicate failed");
    c.out.begin_object().key("ids").begin_array();
    for (EntityId e : created) c.out.num_u(e);
    c.out.end_array().end_object();
    return true;
}

// Every entity of the open scene, one undo step: re-authoring a scene the .mceproj names by GUID
// (scene_open, scene_clear, then entity_create...) instead of one entity_delete per entity.
bool scene_clear(ToolCall& c) {
    EditorScene& s = c.server.session().scene();
    Array<EntityId> ids;
    for (const DocEntity& e : s.doc().entities()) ids.push_back(e.id);
    u64 deleted = 0;
    for (EntityId id : ids)
        if (s.destroy_entity(id)) ++deleted;  // a subtree goes with its root: later ids may be gone already
    c.out.begin_object().key("deleted").num_u(deleted).end_object();
    return true;
}

bool component_add(ToolCall& c) {
    EntityId id = editor::kNoEntity;
    const refl::TypeRecord* t = nullptr;
    if (!arg_entity(c, "entity", id) || !arg_component(c, "component", t)) return false;
    if (scene(c).doc().find(id)->find(*t)) return fail(c, "the entity already has that component: use component_set");
    Value v;
    init_value(v, *t, nullptr);
    if (const cook::JsonValue value = arg(c, "value"); value.valid()) {
        cook::String err;
        if (!apply_patch(*t, value, v.data(), scene(c).doc(), err)) {
            prefix_error(c, *t, err);
            return false;
        }
    }
    scene(c).add_component(id, *t);
    scene(c).set_component(id, *t, v.data());
    c.out.begin_object().key("entity").num_u(id).key("component").str(t->name).key("value");
    write_component_object(c.out, *scene(c).doc().find(id)->find(*t));
    c.out.end_object();
    return true;
}

bool component_remove(ToolCall& c) {
    EntityId id = editor::kNoEntity;
    const refl::TypeRecord* t = nullptr;
    if (!arg_entity(c, "entity", id) || !arg_component(c, "component", t)) return false;
    if (!scene(c).remove_component(id, *t)) return fail(c, "the entity does not have that component");
    c.out.begin_object().key("entity").num_u(id).key("removed").str(t->name).end_object();
    return true;
}

bool component_set(ToolCall& c) {
    EntityId id = editor::kNoEntity;
    const refl::TypeRecord* t = nullptr;
    StringView path;
    if (!arg_entity(c, "entity", id) || !arg_component(c, "component", t) || !arg_string(c, "path", path, false)) return false;
    const cook::JsonValue value = arg(c, "value");
    if (!value.valid()) return arg_missing(c, "value", "given (an object of fields, or the value at 'path')");
    const DocComponent* current = scene(c).doc().find(id)->find(*t);
    if (!current) current = scene(c).prefab_base(id, *t);  // inherited from the prefab: becomes an override
    if (!current) return fail(c, "the entity does not have that component: use component_add");
    Value v;
    init_value(v, *t, current->data());
    cook::String err;
    const bool ok = path.empty() ? apply_patch(*t, value, v.data(), scene(c).doc(), err)
                                 : apply_path(*t, path, value, v.data(), scene(c).doc(), err);
    if (!ok) {
        prefix_error(c, *t, err);
        return false;
    }
    const bool changed = scene(c).set_component(id, *t, v.data());
    c.out.begin_object().key("entity").num_u(id).key("component").str(t->name).key("changed").boolean(changed).key("value");
    write_component_object(c.out, *scene(c).doc().find(id)->find(*t));
    c.out.end_object();
    return true;
}

// ---------------------------------------------------------------- persist

void write_scene_summary(ToolCall& c) {
    const EditorScene& s = scene(c);
    c.out.begin_object();
    c.out.key("path").str(c.server.scene_file().rel.view());
    c.out.key("guid").str(asset::to_string(s.doc().guid()).view());
    c.out.key("name").str(s.doc().name());
    c.out.key("entities").num_u(s.doc().size());
    c.out.end_object();
}

bool scene_new(ToolCall& c) {
    StringView name;
    if (!arg_string(c, "name", name, true)) return false;
    if (refuse_unsaved(c, "discard them")) return false;
    if (name.empty() || name.size() >= 64) return fail(c, "argument 'name' must be 1-63 bytes");
    c.server.session().new_scene(name);
    c.server.scene_file() = Server::SceneFile{};
    c.out.begin_object().key("name").str(name).key("guid").str(asset::to_string(scene(c).doc().guid()).view()).end_object();
    return true;
}

bool scene_open(ToolCall& c) {
    StringView path;
    if (!arg_string(c, "path", path, true)) return false;
    if (refuse_unsaved(c, "discard them")) return false;
    if (!open_scene_file(c.server, path, c.error)) return false;
    write_scene_summary(c);
    return true;
}

// The "guid" of a scene file on disk (null when unreadable): an overwrite keeps it, so references
// to that file stay valid.
asset::Guid guid_of_file(StringView abs) {
    cook::String text;
    if (!cook::fs::read_text(abs, text)) return {};
    cook::JsonDoc doc;
    cook::JsonError err;
    if (!doc.parse(text.view(), err) || !doc.root().is_object()) return {};
    const cook::JsonValue g = doc.root().get("guid");
    if (!g.is_string()) return {};
    const Result<asset::Guid> parsed = asset::parse_guid(g.as_string());
    return parsed ? parsed.value() : asset::Guid{};
}

bool scene_save(ToolCall& c) {
    StringView path, name;
    bool force = false;
    if (!need_project(c) || !arg_string(c, "path", path, false) || !arg_string(c, "name", name, false) ||
        !arg_bool(c, "force", force))
        return false;
    if (!name.empty() && name.size() >= 64) return fail(c, "argument 'name' must be 1-63 bytes");
    Server::SceneFile& file = c.server.scene_file();
    editor::EditorSession& session = c.server.session();
    if (!name.empty() && name != scene(c).doc().name()) scene(c).set_name(name);
    cook::String abs;
    if (!path.empty() && path != file.rel.view()) {
        if (!scene_path(c.server, path, abs, c.error)) return false;
        asset::Guid keep;
        if (cook::fs::exists(abs.view())) {
            if (!force) return fail(c, "a file already exists there: pass force: true to overwrite it (the file keeps its GUID)");
            keep = guid_of_file(abs.view());
        }
        if (Result<void> r = session.save_scene_as(abs.view(), keep); !r) {
            FixedString<512> msg;
            format_error(r.error(), msg);
            return fail(c, msg.view());
        }
        file.rel = cook::String(path);
    } else {
        if (file.rel.empty()) return fail(c, "the scene was never saved: pass 'path' (under assets/)");
        abs = session.project().asset_path(file.rel.view());
        const u64 on_disk = file_content_hash(abs.view());
        // Another agent saved it meanwhile: merge their edits into ours by entity (tools_team.cpp),
        // or refuse with the conflicts; force overwrites theirs.
        if (file.disk_hash != 0 && on_disk != file.disk_hash && !force && !merge_scene_from_disk(c, abs.view())) return false;
        if (Result<void> r = session.save_scene(); !r) {
            FixedString<512> msg;
            format_error(r.error(), msg);
            return fail(c, msg.view());
        }
    }
    file.disk_hash = file_content_hash(abs.view());
    file.base.clear();
    (void)cook::fs::read_text(abs.view(), file.base);
    write_scene_summary(c);
    return true;
}

// ---------------------------------------------------------------- build

void write_report(JsonWriter& w, const cook::CookReport& r) {
    w.key("ok").boolean(r.ok());
    w.key("cooked").num_u(r.cooked);
    w.key("up_to_date").num_u(r.up_to_date);
    w.key("failed").num_u(r.failed);
    w.key("errors").num_u(r.diags.error_count());
    w.key("warnings").num_u(r.diags.warning_count());
    w.key("diagnostics").begin_array();
    usize n = 0;
    for (const cook::Diagnostic& d : r.diags.all())
        if (n++ < kMaxDiagnostics) write_diag(w, d);
    w.end_array();
}

bool cook_run(ToolCall& c) {
    bool pack = false, adopt = false;
    if (!need_project(c) || !arg_bool(c, "pack", pack) || !arg_bool(c, "adopt", adopt)) return false;
    progress(c.server, c.progress_token, 0, 1);
    const cook::CookReport& r = c.server.session().cook(pack, adopt);
    progress(c.server, c.progress_token, 1, 1);
    c.out.begin_object();
    write_report(c.out, r);
    // Output locations, relative to the project folder the server was started with.
    c.out.key("cooked_dir").str("cooked");
    if (pack) c.out.key("pack").str("assets.mce");
    c.out.end_object();
    return true;
}

// Deletes a source file under assets/ with its .mceasset sidecar (the GUID registry entry); the
// catalog is rescanned. The open scene's own file is refused. The next cook drops its outputs.
bool asset_delete(ToolCall& c) {
    StringView rel;
    if (!need_project(c) || !arg_string(c, "path", rel, true)) return false;
    if (rel.empty() || rel.size() > 400 || rel[0] == '/' || rel.find('\\') != StringView::npos ||
        rel.find(':') != StringView::npos || rel.find("..") != StringView::npos)
        return fail(c, "argument 'path' must be a file path under assets/ (forward slashes, no '..')");
    if (rel == c.server.scene_file().rel.view()) return fail(c, "that is the open scene: scene_open another scene first");
    editor::EditorProject& project = c.server.session().project();
    const cook::String abs = project.asset_path(rel);
    if (!cook::fs::exists(abs.view())) return fail(c, "no such file under assets/ (asset_list lists the registered ones)");
    if (cook::fs::is_directory(abs.view())) return fail(c, "argument 'path' names a directory; asset_delete removes one file");
    cook::String sidecar(abs.view());
    sidecar += ".mceasset";
    const bool had_sidecar = cook::fs::exists(sidecar.view());
    if (!cook::fs::remove(abs.view())) return fail(c, "the file could not be deleted");
    if (had_sidecar) cook::fs::remove(sidecar.view());
    project.rescan();
    c.out.begin_object().key("deleted").str(rel).key("sidecar").boolean(had_sidecar).end_object();
    return true;
}

bool cook_errors(ToolCall& c) {
    if (!need_project(c)) return false;
    const editor::EditorProject& p = c.server.session().project();
    c.out.begin_object().key("has_cooked").boolean(p.has_cooked());
    write_report(c.out, p.last_report());
    c.out.end_object();
    return true;
}

// ---------------------------------------------------------------- verify

struct RunProgress {
    Server* server;
    cook::JsonValue token;
};

// run_headless's `inspect`: which entities of the finished run to report, and the JSON written while
// the run's world is still alive.
constexpr usize kMaxInspected = 64;
constexpr usize kMaxInspectTypes = 32;

struct Inspect {
    const editor::SceneDocument* doc = nullptr;
    Array<EntityId> ids;                         // scene entities asked for by id
    const refl::TypeRecord* filter = nullptr;    // every live entity with this component
    Array<const refl::TypeRecord*> show;         // components to print (empty: the scene entity's own)
    cook::String json;                           // the "inspect" array
    Span<const Entity> spawned;                  // during the callback
};

const refl::TypeRecord* component_named(StringView name) {
    const refl::TypeRecord* t = refl::find_type(StringId(name));
    return t && t->is_component() ? t : nullptr;
}

bool arg_inspect(ToolCall& c, Inspect& in) {
    const cook::JsonValue v = arg(c, "inspect");
    if (!v.valid()) return true;
    if (!v.is_object()) return arg_missing(c, "inspect", "an object: {\"entities\": [ids], \"component\": \"Type\", \"show\": [\"Type\"]}");
    const cook::JsonValue ids = v.get("entities");
    if (ids.valid()) {
        if (!ids.is_array()) return arg_missing(c, "inspect.entities", "an array of entity ids or names");
        for (u32 i = 0; i < ids.size(); ++i) {
            // In the played scene (in.doc): the open one, or the file named by 'scene'.
            const cook::JsonValue e = ids.at(i);
            i32 found = -1;
            if (e.is_integer() && e.as_number() >= 1) {
                found = in.doc->index_of(static_cast<EntityId>(e.as_number()));
            } else if (e.is_string()) {
                for (usize k = 0; k < in.doc->size(); ++k) {
                    if (in.doc->at(k).name.view() != e.as_string()) continue;
                    if (found >= 0) return fail(c, "inspect.entities: two entities have that name in the played scene; give its id");
                    found = static_cast<i32>(k);
                }
            } else {
                return arg_missing(c, "inspect.entities", "entity ids or names");
            }
            if (found < 0) return fail(c, "inspect.entities: no entity with that id or name in the played scene");
            in.ids.push_back(in.doc->at(static_cast<usize>(found)).id);
        }
    }
    const cook::JsonValue comp = v.get("component");
    if (comp.valid()) {
        in.filter = comp.is_string() ? component_named(comp.as_string()) : nullptr;
        if (!in.filter) return fail(c, "inspect.component: unknown component type (refl_list_components lists them)");
    }
    const cook::JsonValue show = v.get("show");
    if (show.valid()) {
        if (!show.is_array()) return arg_missing(c, "inspect.show", "an array of component type names");
        for (u32 i = 0; i < show.size() && in.show.size() < kMaxInspectTypes; ++i) {
            const refl::TypeRecord* t = show.at(i).is_string() ? component_named(show.at(i).as_string()) : nullptr;
            if (!t) return fail(c, "inspect.show: unknown component type (refl_list_components lists them)");
            in.show.push_back(t);
        }
    }
    if (in.ids.empty() && !in.filter) return fail(c, "inspect: give 'entities' (scene ids), 'component' (a type), or both");
    return true;
}

// The scene id of a run entity (spawned[i] is scene entity i when the cooked scene has exactly the
// document's entities, i.e. no prefab instance expanded); kNoEntity for entities the run created.
EntityId scene_id_of(const Inspect& in, Entity e) {
    if (e.is_null() || in.spawned.size() != in.doc->size()) return editor::kNoEntity;
    for (usize i = 0; i < in.spawned.size(); ++i)
        if (in.spawned[i] == e) return in.doc->entities()[i].id;
    return editor::kNoEntity;
}

void write_inspected(Inspect& in, JsonWriter& w, const World& world, Entity e) {
    const EntityId id = scene_id_of(in, e);
    const DocEntity* de = id == editor::kNoEntity ? nullptr : in.doc->find(id);
    w.begin_object();
    if (id == editor::kNoEntity) w.key("id").null();
    else w.key("id").num_u(id);
    if (de) w.key("name").str(de->name.view());
    w.key("alive").boolean(world.alive(e));
    w.key("components").begin_object();
    auto print = [&](const refl::TypeRecord& t) {
        const ecs::ComponentId cid = world.find_component(t.type_hash);
        const void* value = cid == ecs::kInvalidComponent || !world.alive(e) ? nullptr : world.get_raw(e, cid);
        if (!value) return;
        cook::String text;
        cook::write_scene_component_state(t, value, text,
                                          [](const void* field, void* user) {
                                              Entity ref;
                                              std::memcpy(&ref, field, sizeof(ref));
                                              const EntityId rid = scene_id_of(*static_cast<const Inspect*>(user), ref);
                                              return rid == editor::kNoEntity ? cook::kNoSceneEntity : rid;
                                          },
                                          &in);
        w.key(t.name).raw(text.view());
    };
    if (!in.show.empty()) {
        for (const refl::TypeRecord* t : in.show) print(*t);
    } else if (de) {
        for (const DocComponent& dc : de->components) print(*dc.type);
    } else if (in.filter) {
        print(*in.filter);
        if (in.filter != &refl::type_of<LocalTransform>()) print(refl::type_of<LocalTransform>());
    }
    w.end_object().end_object();
}

void inspect_run(const World& world, Span<const Entity> spawned, void* user) {
    Inspect& in = *static_cast<Inspect*>(user);
    in.spawned = spawned;
    Array<Entity> picked;
    for (EntityId id : in.ids) {
        for (usize i = 0; i < in.doc->size() && i < spawned.size(); ++i)
            if (in.doc->entities()[i].id == id && spawned.size() == in.doc->size()) picked.push_back(spawned[i]);
    }
    bool truncated = false;
    if (in.filter) {
        const ecs::ComponentId cid = world.find_component(in.filter->type_hash);
        if (cid != ecs::kInvalidComponent) {
            for (const auto& table : world.tables()) {
                const Span<const ecs::ComponentId> ids = table->ids();
                if (std::find(ids.begin(), ids.end(), cid) == ids.end()) continue;
                for (Entity e : table->entities()) {
                    if (std::find(picked.begin(), picked.end(), e) != picked.end()) continue;
                    if (picked.size() >= kMaxInspected) {
                        truncated = true;
                        break;
                    }
                    picked.push_back(e);
                }
            }
        }
    }
    // Scene order first (by id), then entities the run made, by handle: stable across runs.
    std::sort(picked.begin(), picked.end(), [&](Entity a, Entity b) {
        const EntityId ia = scene_id_of(in, a), ib = scene_id_of(in, b);
        if ((ia == editor::kNoEntity) != (ib == editor::kNoEntity)) return ib == editor::kNoEntity;
        if (ia != ib) return ia < ib;
        return a.index < b.index;
    });
    JsonWriter w(in.json);
    w.begin_object().key("count").num_u(picked.size()).key("truncated").boolean(truncated).key("entities").begin_array();
    for (Entity e : picked) write_inspected(in, w, world, e);
    w.end_array().end_object();
}

}  // namespace

// run_headless's `input`: the text form of an input script (mce/input/script.h), or an array of
// segments {"from": tick, "to": tick, "actions": {"Move": [1, 0], "Jump": true, "Fire": 1}}.
bool arg_input(ToolCall& c, input::InputScript& script) { return input_from(c, arg(c, "input"), script); }

bool input_from(ToolCall& c, const cook::JsonValue& v, input::InputScript& script) {
    if (!v.valid()) return true;
    if (v.is_string()) {
        if (Result<void> r = script.parse(v.as_string()); !r) {
            c.error += "input: ";
            c.error += r.error().path.view();
            return false;
        }
        return true;
    }
    if (!v.is_array())
        return arg_missing(c, "input", "a script string (\"10-70 Move=1,0 Jump\") or an array of {from, to, actions}");
    for (u32 i = 0; i < v.size(); ++i) {
        const cook::JsonValue seg = v.at(i);
        if (!seg.is_object()) return arg_missing(c, "input[]", "an object {from, to, actions}");
        const cook::JsonValue from = seg.get("from"), to = seg.get("to"), actions = seg.get("actions");
        if (!from.is_integer() || from.as_number() < 1.0) return arg_missing(c, "input[].from", "a tick >= 1");
        const u64 f = static_cast<u64>(from.as_number());
        u64 t = f;
        if (to.valid()) {
            if (!to.is_integer() || to.as_number() < static_cast<f64>(f)) return arg_missing(c, "input[].to", "a tick >= from");
            t = static_cast<u64>(to.as_number());
        }
        if (!actions.is_object() || actions.size() == 0)
            return arg_missing(c, "input[].actions", "an object: action name -> true, a number, or [x, y]");
        for (u32 a = 0; a < actions.size(); ++a) {
            const cook::JsonValue val = actions.at(a);
            f32 x = 1.f, y = 0.f;
            if (val.is_bool()) x = val.as_bool() ? 1.f : 0.f;
            else if (val.is_number()) x = static_cast<f32>(val.as_number());
            else if (val.is_array() && val.size() == 2 && val.at(0).is_number() && val.at(1).is_number()) {
                x = static_cast<f32>(val.at(0).as_number());
                y = static_cast<f32>(val.at(1).as_number());
            } else return arg_missing(c, "input[].actions values", "true/false, a number, or [x, y]");
            if (!script.add(actions.key_at(a), f, t, x, y)) return arg_missing(c, "input[].actions keys", "action names of 1-31 characters");
        }
    }
    return true;
}

namespace {

bool run_headless_tool(ToolCall& c) {
    u64 ticks = kDefaultTicks;
    u64 log_lines = 20;
    if (!need_project(c) || !arg_uint(c, "ticks", ticks, 1, kMaxTicks, false) ||
        !arg_uint(c, "log_lines", log_lines, 0, kMaxLogLines, false))
        return false;
    input::InputScript script;
    if (!arg_input(c, script)) return false;
    if (script.last_tick() > ticks)
        MCE_LOG_WARN("run_headless: the input script reaches tick {} but the run is {} ticks", script.last_tick(), ticks);
    // The scene to play: the open one (saved), or another saved scene of the project by path.
    StringView other;
    if (!arg_string(c, "scene", other, false)) return false;
    const Server::SceneFile& file = c.server.scene_file();
    StringView played = file.rel.view();
    asset::Guid played_guid = scene(c).doc().guid();
    if (!other.empty() && other != file.rel.view()) {
        cook::String abs;
        if (!scene_path(c.server, other, abs, c.error)) return false;
        const editor::ProjectAsset* found = nullptr;
        for (const editor::ProjectAsset& a : c.server.session().project().assets())
            if (a.type == asset::ChunkType::Scene && a.path.view() == other) found = &a;
        if (!found) return fail(c, "argument 'scene': no such scene in the project (asset_list with type Scene lists them)");
        played = other;
        played_guid = found->guid;
    } else {
        if (file.rel.empty()) return fail(c, "the scene was never saved: scene_save it (with a path under assets/) first");
        if (scene(c).dirty()) return fail(c, "the scene has unsaved changes: scene_save first (run_headless plays the cooked file)");
    }
    const bool other_scene = played != file.rel.view();
    // Entity names and ids in inspect and trace name the played scene's entities.
    editor::SceneDocument played_doc;
    if (other_scene) {
        cook::String derr("scene: ");
        if (!load_scene_doc(c.server, played, played_doc, derr)) return fail(c, derr.view());
    }
    const editor::SceneDocument& doc = other_scene ? played_doc : scene(c).doc();
    Inspect inspect;
    inspect.doc = &doc;
    if (!arg_inspect(c, inspect)) return false;
    const bool inspecting = !inspect.ids.empty() || inspect.filter;
    Trace trace;
    if (const cook::JsonValue tv = arg(c, "trace"); tv.valid()) {
        cook::String terr("trace: ");
        if (!parse_trace(doc, tv, trace, terr)) return fail(c, terr.view());
    }
    SimRun sim;
    sim.trace = &trace;
    const RunHooks hooks = sim.hooks();

    const u64 since = log::console_last_seq();
    const cook::CookReport& r = c.server.session().cook(false);
    if (!r.ok()) {
        c.error += "cook failed:";
        append_diags(c.error, r.diags, 10);
        return false;
    }
    const cook::String cooked = cook::path_join(c.server.session().project().root(), "cooked");
    RunProgress rp{&c.server, c.progress_token};
    Result<HeadlessRun> run = run_headless(cooked.view(), played_guid, ticks,
                                           c.progress_token.valid() ? [](u64 done, u64 total, void* user) {
                                               auto* p = static_cast<RunProgress*>(user);
                                               progress(*p->server, p->token, done, total);
                                           } : static_cast<TickProgressFn>(nullptr),
                                           &rp, inspecting ? &inspect_run : nullptr, &inspect,
                                           script.empty() ? nullptr : &script, c.server.session().project().base_roots(),
                                           trace.active() ? &hooks : nullptr);
    Array<log::ConsoleLine> lines;
    read_log(since, lines);
    if (!run) {
        FixedString<512> msg;
        format_error(run.error(), msg);
        c.error += "run failed: ";
        c.error += msg.view();
        return false;
    }
    u32 warnings = 0, errors = 0;
    for (const log::ConsoleLine& l : lines) {
        if (l.level == log::Level::Warn) ++warnings;
        if (l.level >= log::Level::Error && l.level != log::Level::Off) ++errors;
    }
    const HeadlessRun& h = run.value();
    c.out.begin_object();
    c.out.key("scene").str(played);
    c.out.key("ticks").num_u(h.ticks);
    c.out.key("entities").num_u(h.entities);
    cook::String hash;
    cook::appendf(hash, "%016llx", static_cast<unsigned long long>(h.state_hash));
    c.out.key("state_hash").str(hash.view());
    c.out.key("input_segments").num_u(script.segments().size());
    c.out.key("metrics").begin_object();
    c.out.key("entities").num_u(h.metrics.entities);
    c.out.key("tables").num_u(h.metrics.tables);
    c.out.key("components").num_u(h.metrics.components);
    c.out.key("avg_table_occupancy").num_f(h.metrics.avg_table_occupancy_x100 / 100.0);
    c.out.key("pending_commands").num_u(h.metrics.pending_commands);
    c.out.end_object();
    c.out.key("log").begin_object().key("warnings").num_u(warnings).key("errors").num_u(errors).key("tail");
    write_log_lines(c.out, lines.span(), static_cast<usize>(log_lines));
    c.out.end_object();
    if (inspecting) c.out.key("inspect").raw(inspect.json.view());
    if (trace.active()) {
        c.out.key("trace");
        trace.write(c.out);
    }
    c.out.end_object();
    return true;
}

// ---------------------------------------------------------------- console

const char* cvar_type_name(CVarType t) {
    switch (t) {
        case CVarType::Bool: return "bool";
        case CVarType::Int: return "int";
        case CVarType::Float: return "float";
        default: return "string";
    }
}

bool console_list(ToolCall& c) {
    StringView prefix;
    if (!arg_string(c, "prefix", prefix, false)) return false;
    Array<ConVar*> vars;
    ConVar::collect(vars);
    c.out.begin_object().key("convars").begin_array();
    for (const ConVar* v : vars) {
        if (StringView(v->name()).substr(0, prefix.size()) != prefix) continue;
        FixedString<128> value;
        v->to_string(value);
        c.out.begin_object().key("name").str(v->name()).key("type").str(cvar_type_name(v->type())).key("value").str(value.view());
        c.out.key("flags").begin_array();
        if (v->flags() & CVarFlags::Cheat) c.out.str("cheat");
        if (v->flags() & CVarFlags::ReadOnly) c.out.str("read_only");
        if (v->flags() & CVarFlags::RequiresRestart) c.out.str("requires_restart");
        if (v->flags() & CVarFlags::Saved) c.out.str("saved");
        c.out.end_array();
        if (*v->help()) c.out.key("help").str(v->help());
        c.out.end_object();
    }
    c.out.end_array().key("confuncs").begin_array();
    Array<ConFunc*> funcs;
    ConFunc::collect(funcs);
    for (const ConFunc* f : funcs) {
        if (StringView(f->name()).substr(0, prefix.size()) != prefix) continue;
        c.out.begin_object().key("name").str(f->name());
        if (*f->help()) c.out.key("help").str(f->help());
        c.out.end_object();
    }
    c.out.end_array().end_object();
    return true;
}

bool console_exec(ToolCall& c) {
    StringView line;
    if (!arg_string(c, "line", line, true)) return false;
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
    if (line.empty() || line.size() > 255 || line.find('\n') != StringView::npos) return fail(c, "argument 'line' must be one console line");
    const usize space = line.find_first_of(" \t");
    const StringView name = line.substr(0, space);
    const bool sets = space != StringView::npos && line.find_first_not_of(" \t", space) != StringView::npos;
    if (const ConVar* v = ConVar::find(StringId(name)); v && sets && !c.server.options().allow_cheats &&
                                                       (v->flags() & (CVarFlags::Cheat | CVarFlags::RequiresRestart))) {
        c.error += "'";
        c.error += name;
        c.error += "' is a cheat or requires a restart: refused (start mce_mcp with --allow-cheats to permit it)";
        return false;
    }
    const u64 since = log::console_last_seq();
    const bool ok = console::execute(line);
    Array<log::ConsoleLine> lines;
    read_log(since, lines);
    if (!ok) {
        c.error += "unknown name or bad value: ";
        c.error += line;
        return false;
    }
    c.out.begin_object().key("output");
    write_log_lines(c.out, lines.span(), kMaxLogLines);
    c.out.end_object();
    return true;
}

// ---------------------------------------------------------------- the table

// Undo/redo over the same history the editor keeps (FR-EDT-040): every tools/call that edits the
// scene is one entry, labelled with the tool's name.
bool undo_redo(ToolCall& c, bool redo) {
    EditorScene& s = scene(c);
    cook::String label(redo ? s.redo_label() : s.undo_label());
    if (!(redo ? s.redo() : s.undo())) return fail(c, redo ? "nothing to redo" : "nothing to undo");
    c.out.begin_object();
    c.out.key(redo ? "redone" : "undone").str(label.view());
    c.out.key("can_undo").boolean(s.can_undo());
    c.out.key("can_redo").boolean(s.can_redo());
    c.out.key("entities").num_u(s.doc().size());
    c.out.end_object();
    return true;
}

bool scene_undo(ToolCall& c) { return undo_redo(c, false); }
bool scene_redo(ToolCall& c) { return undo_redo(c, true); }

constexpr const char* kNoArgs = R"({"type":"object","properties":{},"additionalProperties":false})";
constexpr const char* kEntityArg = R"({"type":"object","properties":{"entity":{"type":["integer","string"],"description":"Entity id, or its name when unique"}},"required":["entity"]})";

const ToolDef kTools[] = {
    {"refl_list_components", "List every reflected component type (name and field count).", kNoArgs, refl_list_components},
    {"refl_describe_component",
     "Describe a component type, or a reflected value type such as UiAnchors or UiStyle (runtime UI): fields with kind, "
     "array count, nested struct fields, enum values, editor range, read-only/transient flags and help text.",
     R"({"type":"object","properties":{"name":{"type":"string","description":"Component type name, e.g. Sprite"}},"required":["name"]})",
     refl_describe_component},
    {"scene_info", "The open scene: name, GUID, path under assets/, unsaved changes, entity count.", kNoArgs, scene_info},
    {"scene_list_entities", "List the scene's entities in file order: id, name, component types. Optional component filter.",
     R"({"type":"object","properties":{"component":{"type":"string","description":"Only entities with this component"}}})",
     scene_list_entities},
    {"scene_get_entity",
     "One entity (by id, or by name when unique) with its component values in .mcscene form (fields at their "
     "default are omitted; entity references are \"$<id>\"; a value you write may also name the entity). Ids are saved in the file and survive scene_open.",
     kEntityArg, scene_get_entity},
    {"asset_list", "Assets of the project (GUID registry): guid as \"guid:<uuid>\", path under assets/, type.",
     R"({"type":"object","properties":{"type":{"type":"string","description":"Texture, Font, Sfx, Scene, Mesh, Material, ..."}}})",
     asset_list},
    {"asset_set_import_settings",
     "Replace an asset's importer settings in its .mceasset sidecar (texture: {\"srgb\": bool, \"mips\": bool}; a normal map or LUT needs srgb false). Keys are validated by the next cook_run.",
     R"({"type":"object","properties":{"path":{"type":"string","description":"Path under assets/, e.g. textures/rock_n.png"},"guid":{"type":"string","description":"\"guid:<uuid>\" instead of path"},"settings":{"type":"object"}},"required":["settings"]})",
     asset_set_import_settings},
    {"entity_create",
     "Create an entity (with a LocalTransform unless transform is false) and optionally components, e.g. "
     "{\"Sprite\": {\"texture\": \"guid:...\", \"size\": [1, 1]}}. With 'prefab', an instance of that .mcprefab whose "
     "'components' override the prefab root's fields (LocalTransform places it). An entity reference field takes "
     "\"$<id>\", the entity's unique name, or null. Returns its id.",
     R"({"type":"object","properties":{"name":{"type":"string"},"prefab":{"type":"string","description":"\"guid:<uuid>\" of a .mcprefab: place an instance; components then override the prefab root's fields"},"transform":{"type":"boolean","default":true},"at":{"type":"integer","description":"Index in the hierarchy; default: the end"},"components":{"type":"object","description":"Type name -> object of fields"}},"required":["name"]})",
     entity_create},
    {"entity_delete", "Delete an entity (references to it become null).", kEntityArg, entity_delete},
    {"scene_clear", "Delete every entity of the open scene (one undo step): re-author a scene keeping its GUID and path.", kNoArgs, scene_clear},
    {"entity_duplicate", "Duplicate an entity right after itself. Returns the new id(s).", kEntityArg, entity_duplicate},
    {"component_add", "Add a component (default values, or patched with 'value': an object of fields).",
     R"({"type":"object","properties":{"entity":{"type":["integer","string"],"description":"Entity id, or its name when unique"},"component":{"type":"string"},"value":{"type":"object"}},"required":["entity","component"]})",
     component_add},
    {"component_remove", "Remove a component from an entity.",
     R"({"type":"object","properties":{"entity":{"type":["integer","string"],"description":"Entity id, or its name when unique"},"component":{"type":"string"}},"required":["entity","component"]})",
     component_remove},
    {"component_set",
     "Change a component. Without 'path', 'value' is an object of fields to change. With a property path "
     "(\"position\", \"position.x\", \"shape.half\", \"items[2]\"), 'value' is that property's value. An entity "
     "reference takes \"$<id>\", the entity's unique name, or null. Type errors name the expected type.",
     R"({"type":"object","properties":{"entity":{"type":["integer","string"],"description":"Entity id, or its name when unique"},"component":{"type":"string"},"path":{"type":"string"},"value":{}},"required":["entity","component","value"]})",
     component_set},
    {"scene_new", "Start a new, empty, unsaved scene (refuses to drop unsaved changes unless force).",
     R"({"type":"object","properties":{"name":{"type":"string"},"force":{"type":"boolean"}},"required":["name"]})", scene_new},
    {"scene_open", "Open a .mcscene, or a .mcprefab to edit the prefab itself (entity 0 is its root), by its path under assets/ (refuses to drop unsaved changes unless force).",
     R"({"type":"object","properties":{"path":{"type":"string","description":"e.g. scenes/demo.mcscene"},"force":{"type":"boolean"}},"required":["path"]})",
     scene_open},
    {"scene_save",
     "Save the scene (atomic write; entity ids are saved with it). With 'path' (under assets/): save as, with a "
     "fresh GUID for a new file. 'name' renames the scene. When another agent saved the file since it was opened, "
     "their edits are merged in entity by entity; edits to the same component of an entity conflict and nothing is "
     "written unless resolve picks a side. Refuses to overwrite another existing file unless force (an overwritten "
     "file keeps its GUID; force skips the merge). A path ending in .mcprefab saves the document as a prefab "
     "(entity 0 is the root; prefab_create_from makes one from an entity).",
     R"({"type":"object","properties":{"path":{"type":"string"},"name":{"type":"string","description":"New scene name"},"force":{"type":"boolean"},)"
     R"("resolve":{"type":"string","enum":["ours","theirs"],"description":"How merge conflicts are settled"}}})", scene_save},
    {"scene_undo", "Undo the last scene edit (one tools/call is one step; editor edits share the history).", kNoArgs,
     scene_undo},
    {"scene_redo", "Redo the last undone scene edit.", kNoArgs, scene_redo},
    {"cook_run",
     "Cook the project (incremental) into <project>/cooked. pack: also write <project>/assets.mce (a host built "
     "from the project plays its own build-tree pack unless started with -pack <that file or the cooked dir>). "
     "adopt: give new source files (a PNG, WAV or TTF added under assets/ with no .mceasset sidecar) a sidecar "
     "with a fresh GUID instead of a cook error. Returns counts, diagnostics with file:line and the output paths "
     "(relative to the project folder).",
     R"({"type":"object","properties":{"pack":{"type":"boolean","default":false},"adopt":{"type":"boolean","default":false}}})",
     cook_run},
    {"cook_errors", "Diagnostics of the last cook (file, line, col, stage, message).", kNoArgs, cook_errors},
    {"asset_delete", "Delete a source file under assets/ together with its .mceasset sidecar (not the open scene). The catalog is rescanned.",
     R"({"type":"object","properties":{"path":{"type":"string","description":"Path under assets/, e.g. scenes/try2.mcscene"}},"required":["path"]})",
     asset_delete},
    {"run_headless",
     "Cook, then play the saved scene headless for N fixed ticks exactly as the game's host plays it with -headless "
     "(the game's own systems included in a project server), or another saved scene of the project with 'scene'. "
     "Returns the World state_hash (same scene + ticks + input = "
     "same hash), World metrics and the log tail. input: scripted keyboard/gamepad actions per tick, as the game reads "
     "them from InputState (\"Move\" [x,y], \"Jump\", \"Fire\", \"Pause\", \"Confirm\", \"Cancel\" and any "
     "game-defined action name); a press is a one-tick segment, a hold a range. inspect: after the last tick, report the "
     "live component values (Transient runtime fields included) of scene entities by id and/or of every entity with a "
     "component (at most 64); show picks the components to print. trace: component fields of scene entities every N ticks, "
     "as series (how a value moved over the run, not only where it ended).",
     R"json({"type":"object","properties":{"scene":{"type":"string","description":"Play this saved scene (path under assets/) instead of the open one; inspect then takes 'component' only"},"ticks":{"type":"integer","minimum":1,"maximum":36000,"default":300},"log_lines":{"type":"integer","minimum":0,"maximum":200,"default":20},"input":{"description":"Either a script string, one segment per line: \"<from>[-<to>] Action[=value] ...\" (e.g. \"10-70 Move=1,0 Jump\\n100 Fire\"), or an array of segments","type":["string","array"],"items":{"type":"object","properties":{"from":{"type":"integer","minimum":1,"description":"First tick (1-based)"},"to":{"type":"integer","description":"Last tick, inclusive (default: from)"},"actions":{"type":"object","description":"Action name -> true, a number, or [x, y]","additionalProperties":{"type":["boolean","number","array"]}}},"required":["from","actions"]}},"inspect":{"type":"object","properties":{"entities":{"type":"array","items":{"type":["integer","string"]},"description":"Scene entity ids or names"},"component":{"type":"string","description":"Every live entity with this component (also ones the run spawned)"},"show":{"type":"array","items":{"type":"string"},"description":"Components to print (default: the entity's scene components, or the filter component + LocalTransform)"}}},"trace":{"type":"object","description":"Sample fields during the run: {\"every\": ticks (10), \"fields\": [{\"entity\": id or name, \"field\": \"LocalTransform.position.x\"}]} (at most 16 fields, 1000 rows)"}}})json",
     run_headless_tool},
    {"console_list", "List console variables (type, value, flags, help) and console functions. Optional name prefix.",
     R"({"type":"object","properties":{"prefix":{"type":"string"}}})", console_list},
    {"console_exec",
     "Execute one console line (\"name\" prints, \"name value\" sets, \"func args\" calls). Cheat and "
     "restart-only variables are refused unless the server runs with --allow-cheats.",
     R"({"type":"object","properties":{"line":{"type":"string"}},"required":["line"]})", console_exec},
};

}  // namespace

Span<const ToolDef> tool_table() {
    static const Array<ToolDef> all = [] {
        Array<ToolDef> a;
        for (const ToolDef& t : kTools) a.push_back(t);
        for (Span<const ToolDef> group : {agent_tool_table(), asset_tool_table(), prefab_tool_table(), project_tool_table()})
            for (const ToolDef& t : group) a.push_back(t);
        return a;
    }();
    return all.span();
}

const ToolDef* find_tool(StringView name) {
    for (const ToolDef& t : tool_table())
        if (name == t.name) return &t;
    return nullptr;
}

bool tool_arg_entity(ToolCall& c, const char* name, editor::EntityId& out) { return arg_entity(c, name, out); }

bool tool_arg_component(ToolCall& c, const char* name, const refl::TypeRecord*& out) { return arg_component(c, name, out); }

bool open_scene_file(Server& server, StringView rel, cook::String& err) {
    cook::String abs;
    if (!scene_path(server, rel, abs, err)) return false;
    if (!cook::fs::exists(abs.view())) {
        err += "no such scene: ";
        err += rel;
        return false;
    }
    editor::EditorSession& session = server.session();
    if (!session.open_scene(abs.view())) {
        err += "cannot open ";
        err += rel;
        err += ':';
        append_diags(err, session.scene_diags(), 10);
        return false;
    }
    cook::String base;
    (void)cook::fs::read_text(abs.view(), base);
    server.scene_file() = Server::SceneFile{cook::String(rel), file_content_hash(abs.view()), static_cast<cook::String&&>(base)};
    return true;
}

// ---------------------------------------------------------------- resources

void list_resources(JsonWriter& w) {
    struct Res {
        const char* uri;
        const char* name;
        const char* description;
        const char* mime;
    };
    static constexpr Res kResources[] = {
        {"mce://scene/current", "scene", "The open scene as .mcscene text (the in-memory document, saved or not)", "application/json"},
        {"mce://log", "log", "The last console/log lines", "text/plain"},
        {"mce://refl/schema", "schema", "Every reflected component type, described as refl_describe_component does", "application/json"},
        {"mce://assets/schema", "asset-schema", "Every JSON source asset type, described as asset_schema does", "application/json"},
    };
    for (const Res& r : kResources)
        w.begin_object().key("uri").str(r.uri).key("name").str(r.name).key("description").str(r.description).key("mimeType").str(r.mime).end_object();
}

bool read_resource(Server& server, StringView uri, JsonWriter& w) {
    cook::String text;
    const char* mime = "application/json";
    if (uri == "mce://scene/current") {
        server.session().scene().serialize(text);
    } else if (uri == "mce://log") {
        mime = "text/plain";
        Array<log::ConsoleLine> lines;
        read_log(0, lines);
        for (usize i = lines.size() > kMaxLogLines ? lines.size() - kMaxLogLines : 0; i < lines.size(); ++i) {
            text += '[';
            text += level_name(lines[i].level);
            text += "] ";
            text += lines[i].text.view();
            text += '\n';
        }
    } else if (uri == "mce://assets/schema") {
        JsonWriter s(text);
        write_asset_schemas(s);
    } else if (uri == "mce://refl/schema") {
        JsonWriter s(text);
        s.begin_object().key("components").begin_array();
        for (const refl::TypeRecord* t : refl::all())
            if (t->is_component()) describe_type(s, *t);
        s.end_array().end_object();
    } else if (!docs_read_resource(server, uri, text, mime)) {
        return false;
    }
    w.begin_object().key("contents").begin_array();
    w.begin_object().key("uri").str(uri).key("mimeType").str(mime).key("text").str(text.view()).end_object();
    w.end_array().end_object();
    return true;
}

}  // namespace mce::mcp
