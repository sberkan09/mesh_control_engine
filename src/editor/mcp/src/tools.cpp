// The MVP tool table (editor-mcp.md §4): discover, read, edit, persist, build, verify, console.
// Edits go through EditorScene (the mutation funnel); values are validated on a copy first, so a
// failed call changes nothing.

#include "tools.h"

#include <cstring>

#include "mce/convar.h"
#include "mce/cook/fs.h"
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

// ---------------------------------------------------------------- helpers

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

bool arg_entity(ToolCall& c, const char* name, EntityId& out) {
    u64 id = 0;
    if (!arg_uint(c, name, id, 1, 0xFFFFFFFEu, true)) return false;
    if (!scene(c).doc().find(static_cast<EntityId>(id))) {
        cook::appendf(c.error, "no entity with id %llu in the scene (scene_list_entities lists them)",
                      static_cast<unsigned long long>(id));
        return false;
    }
    out = static_cast<EntityId>(id);
    return true;
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
              rel.find(':') == StringView::npos && rel.size() > 8 && rel.substr(rel.size() - 8) == ".mcscene";
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
        err += "': use a path under assets/ like \"scenes/level.mcscene\" (forward slashes, no '..')";
        return false;
    }
    abs = server.session().project().asset_path(rel);
    return true;
}

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

void progress(Server& server, cook::JsonValue token, u64 done, u64 total) {
    if (!token.valid()) return;
    cook::String params;
    JsonWriter w(params);
    w.begin_object().key("progressToken").copy(token).key("progress").num_u(done).key("total").num_u(total).end_object();
    server.notify("notifications/progress", params.view());
}

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

bool refl_describe_component(ToolCall& c) {
    const refl::TypeRecord* t = nullptr;
    if (!arg_component(c, "name", t)) return false;
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
    const EntityId id = s.create_entity(name, transform, has_at ? static_cast<i32>(at) : -1);
    for (Value& v : values) {
        s.add_component(id, *v.type);
        s.set_component(id, *v.type, v.data());
    }
    c.out.begin_object().key("id").num_u(id).key("index").num_i(s.doc().index_of(id)).end_object();
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

bool scene_save(ToolCall& c) {
    StringView path;
    bool force = false;
    if (!need_project(c) || !arg_string(c, "path", path, false) || !arg_bool(c, "force", force)) return false;
    Server::SceneFile& file = c.server.scene_file();
    editor::EditorSession& session = c.server.session();
    cook::String abs;
    if (!path.empty() && path != file.rel.view()) {
        if (!scene_path(c.server, path, abs, c.error)) return false;
        if (cook::fs::exists(abs.view()) && !force) return fail(c, "a file already exists there: pass force: true to overwrite it");
        if (Result<void> r = session.save_scene_as(abs.view()); !r) {
            FixedString<512> msg;
            format_error(r.error(), msg);
            return fail(c, msg.view());
        }
        file.rel = cook::String(path);
    } else {
        if (file.rel.empty()) return fail(c, "the scene was never saved: pass 'path' (under assets/)");
        abs = session.project().asset_path(file.rel.view());
        const u64 on_disk = file_content_hash(abs.view());
        if (file.disk_hash != 0 && on_disk != file.disk_hash && !force)
            return fail(c, "the file changed on disk since it was opened or saved: scene_open it again, or pass force: true to overwrite");
        if (Result<void> r = session.save_scene(); !r) {
            FixedString<512> msg;
            format_error(r.error(), msg);
            return fail(c, msg.view());
        }
    }
    file.disk_hash = file_content_hash(abs.view());
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
    bool pack = false;
    if (!need_project(c) || !arg_bool(c, "pack", pack)) return false;
    progress(c.server, c.progress_token, 0, 1);
    const cook::CookReport& r = c.server.session().cook(pack);
    progress(c.server, c.progress_token, 1, 1);
    c.out.begin_object();
    write_report(c.out, r);
    if (pack) c.out.key("pack").str("assets.mce");
    c.out.end_object();
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

bool run_headless_tool(ToolCall& c) {
    u64 ticks = kDefaultTicks;
    u64 log_lines = 20;
    if (!need_project(c) || !arg_uint(c, "ticks", ticks, 1, kMaxTicks, false) ||
        !arg_uint(c, "log_lines", log_lines, 0, kMaxLogLines, false))
        return false;
    const Server::SceneFile& file = c.server.scene_file();
    if (file.rel.empty()) return fail(c, "the scene was never saved: scene_save it (with a path under assets/) first");
    if (scene(c).dirty()) return fail(c, "the scene has unsaved changes: scene_save first (run_headless plays the cooked file)");

    const u64 since = log::console_last_seq();
    const cook::CookReport& r = c.server.session().cook(false);
    if (!r.ok()) {
        c.error += "cook failed:";
        append_diags(c.error, r.diags, 10);
        return false;
    }
    const cook::String cooked = cook::path_join(c.server.session().project().root(), "cooked");
    RunProgress rp{&c.server, c.progress_token};
    Result<HeadlessRun> run = run_headless(cooked.view(), scene(c).doc().guid(), ticks,
                                           c.progress_token.valid() ? [](u64 done, u64 total, void* user) {
                                               auto* p = static_cast<RunProgress*>(user);
                                               progress(*p->server, p->token, done, total);
                                           } : static_cast<TickProgressFn>(nullptr),
                                           &rp);
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
    c.out.key("scene").str(file.rel.view());
    c.out.key("ticks").num_u(h.ticks);
    c.out.key("entities").num_u(h.entities);
    cook::String hash;
    cook::appendf(hash, "%016llx", static_cast<unsigned long long>(h.state_hash));
    c.out.key("state_hash").str(hash.view());
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
constexpr const char* kEntityArg = R"({"type":"object","properties":{"entity":{"type":"integer","description":"Entity id"}},"required":["entity"]})";

const ToolDef kTools[] = {
    {"refl_list_components", "List every reflected component type (name and field count).", kNoArgs, refl_list_components},
    {"refl_describe_component",
     "Describe a component type: fields with kind, array count, nested struct fields, enum values, editor range, "
     "read-only/transient flags and help text.",
     R"({"type":"object","properties":{"name":{"type":"string","description":"Component type name, e.g. Sprite"}},"required":["name"]})",
     refl_describe_component},
    {"scene_info", "The open scene: name, GUID, path under assets/, unsaved changes, entity count.", kNoArgs, scene_info},
    {"scene_list_entities", "List the scene's entities in file order: id, name, component types. Optional component filter.",
     R"({"type":"object","properties":{"component":{"type":"string","description":"Only entities with this component"}}})",
     scene_list_entities},
    {"scene_get_entity",
     "One entity with its component values in .mcscene form (fields at their default are omitted; entity "
     "references are \"$<id>\").",
     kEntityArg, scene_get_entity},
    {"asset_list", "Assets of the project (GUID registry): guid as \"guid:<uuid>\", path under assets/, type.",
     R"({"type":"object","properties":{"type":{"type":"string","description":"Texture, Font, Sfx, Scene, Mesh, Material, ..."}}})",
     asset_list},
    {"entity_create",
     "Create an entity (with a LocalTransform unless transform is false) and optionally components, e.g. "
     "{\"Sprite\": {\"texture\": \"guid:...\", \"size\": [1, 1]}}. Returns its id.",
     R"({"type":"object","properties":{"name":{"type":"string"},"transform":{"type":"boolean","default":true},"at":{"type":"integer","description":"Index in the hierarchy; default: the end"},"components":{"type":"object","description":"Type name -> object of fields"}},"required":["name"]})",
     entity_create},
    {"entity_delete", "Delete an entity (references to it become null).", kEntityArg, entity_delete},
    {"entity_duplicate", "Duplicate an entity right after itself. Returns the new id(s).", kEntityArg, entity_duplicate},
    {"component_add", "Add a component (default values, or patched with 'value': an object of fields).",
     R"({"type":"object","properties":{"entity":{"type":"integer"},"component":{"type":"string"},"value":{"type":"object"}},"required":["entity","component"]})",
     component_add},
    {"component_remove", "Remove a component from an entity.",
     R"({"type":"object","properties":{"entity":{"type":"integer"},"component":{"type":"string"}},"required":["entity","component"]})",
     component_remove},
    {"component_set",
     "Change a component. Without 'path', 'value' is an object of fields to change. With a property path "
     "(\"position\", \"position.x\", \"shape.half\", \"items[2]\"), 'value' is that property's value. Type errors "
     "name the expected type.",
     R"({"type":"object","properties":{"entity":{"type":"integer"},"component":{"type":"string"},"path":{"type":"string"},"value":{}},"required":["entity","component","value"]})",
     component_set},
    {"scene_new", "Start a new, empty, unsaved scene (refuses to drop unsaved changes unless force).",
     R"({"type":"object","properties":{"name":{"type":"string"},"force":{"type":"boolean"}},"required":["name"]})", scene_new},
    {"scene_open", "Open a .mcscene by its path under assets/ (refuses to drop unsaved changes unless force).",
     R"({"type":"object","properties":{"path":{"type":"string","description":"e.g. scenes/demo.mcscene"},"force":{"type":"boolean"}},"required":["path"]})",
     scene_open},
    {"scene_save",
     "Save the scene (atomic write). With 'path' (under assets/): save as. Refuses to overwrite a file that "
     "changed on disk since it was opened, or another existing file, unless force.",
     R"({"type":"object","properties":{"path":{"type":"string"},"force":{"type":"boolean"}}})", scene_save},
    {"scene_undo", "Undo the last scene edit (one tools/call is one step; editor edits share the history).", kNoArgs,
     scene_undo},
    {"scene_redo", "Redo the last undone scene edit.", kNoArgs, scene_redo},
    {"cook_run", "Cook the project (incremental). pack: also write assets.mce. Returns counts and diagnostics with file:line.",
     R"({"type":"object","properties":{"pack":{"type":"boolean","default":false}}})", cook_run},
    {"cook_errors", "Diagnostics of the last cook (file, line, col, stage, message).", kNoArgs, cook_errors},
    {"run_headless",
     "Cook, then play the saved scene headless for N fixed ticks exactly as mce_player -headless does. Returns the "
     "World state_hash (same scene + ticks = same hash), World metrics and the log tail.",
     R"({"type":"object","properties":{"ticks":{"type":"integer","minimum":1,"maximum":36000,"default":300},"log_lines":{"type":"integer","minimum":0,"maximum":200,"default":20}}})",
     run_headless_tool},
    {"console_list", "List console variables (type, value, flags, help) and console functions. Optional name prefix.",
     R"({"type":"object","properties":{"prefix":{"type":"string"}}})", console_list},
    {"console_exec",
     "Execute one console line (\"name\" prints, \"name value\" sets, \"func args\" calls). Cheat and "
     "restart-only variables are refused unless the server runs with --allow-cheats.",
     R"({"type":"object","properties":{"line":{"type":"string"}},"required":["line"]})", console_exec},
};

}  // namespace

Span<const ToolDef> tool_table() { return Span<const ToolDef>(kTools, sizeof(kTools) / sizeof(kTools[0])); }

const ToolDef* find_tool(StringView name) {
    for (const ToolDef& t : kTools)
        if (name == t.name) return &t;
    return nullptr;
}

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
    server.scene_file() = Server::SceneFile{cook::String(rel), file_content_hash(abs.view())};
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
    } else if (uri == "mce://refl/schema") {
        JsonWriter s(text);
        s.begin_object().key("components").begin_array();
        for (const refl::TypeRecord* t : refl::all())
            if (t->is_component()) describe_type(s, *t);
        s.end_array().end_object();
    } else {
        return false;
    }
    w.begin_object().key("contents").begin_array();
    w.begin_object().key("uri").str(uri).key("mimeType").str(mime).key("text").str(text.view()).end_object();
    w.end_array().end_object();
    return true;
}

}  // namespace mce::mcp
