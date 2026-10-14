// Project and scene settings tools (ROADMAP M-DATA-REACH step 3): the .mceproj (title, start
// scene, games, plugins) and the open scene's WorldSettings (gravity), through the same editor
// core calls as the Project Settings panel (mce/editor/project_settings.h).

#include "mce/cook/fs.h"
#include "mce/editor/project_settings.h"
#include "mce/kit/components.h"
#include "mce/mcp/values.h"
#include "tools.h"

namespace mce::mcp {

namespace {

bool fail(ToolCall& c, StringView msg) {
    c.error += msg;
    return false;
}

void write_world_settings(ToolCall& c) {
    const editor::EditorScene& scene = c.server.session().scene();
    const editor::EntityId id = editor::world_settings_entity(scene);
    c.out.key("world_settings");
    if (id == editor::kNoEntity) {
        c.out.null();
        return;
    }
    const editor::DocComponent* comp = scene.doc().find(id)->find(refl::type_of<kit::WorldSettings>());
    cook::String text;
    write_component(*comp->type, comp->data(), text);
    c.out.begin_object().key("entity").num_u(id).key("value").raw(text.view()).end_object();
}

bool project_get(ToolCall& c) {
    editor::EditorProject& project = c.server.session().project();
    if (!project.is_open()) return fail(c, "no project is open: start mce_mcp with --project <dir>");
    const cook::String path = editor::project_file_path(project);
    c.out.begin_object();
    if (path.empty()) {
        c.out.key("file").null().key("project").null();
    } else {
        cook::String text;
        cook::JsonDoc doc;
        cook::JsonError err;
        if (!cook::fs::read_text(path.view(), text) || !doc.parse(text.view(), err) || !doc.root().is_object())
            return fail(c, "the .mceproj file is not a JSON object");
        const StringView v = path.view();
        c.out.key("file").str(v.substr(v.rfind('/') + 1));
        c.out.key("project").copy(doc.root());
        // The start scene's path, so a client need not map the GUID itself.
        c.out.key("start_scene_path");
        const cook::JsonValue start = doc.root().get("start_scene");
        const editor::ProjectAsset* a = nullptr;
        if (start.is_string())
            if (const Result<asset::Guid> g = asset::parse_guid(start.as_string())) a = project.find(g.value());
        if (a) c.out.str(a->path.view());
        else c.out.null();
    }
    write_world_settings(c);
    c.out.end_object();
    return true;
}

bool project_set(ToolCall& c) {
    editor::EditorProject& project = c.server.session().project();
    if (!project.is_open()) return fail(c, "no project is open: start mce_mcp with --project <dir>");
    const cook::JsonValue values = c.args.is_object() ? c.args.get("values") : cook::JsonValue();
    if (!values.is_object()) return fail(c, "argument 'values' must be an object of .mceproj members (project_get shows them)");
    if (!editor::patch_project_file(project, values, c.error)) return false;
    return project_get(c);
}

bool scene_settings_get(ToolCall& c) {
    c.out.begin_object();
    write_world_settings(c);
    c.out.end_object();
    return true;
}

bool scene_settings_set(ToolCall& c) {
    const cook::JsonValue values = c.args.is_object() ? c.args.get("values") : cook::JsonValue();
    if (!values.is_object()) return fail(c, "argument 'values' must be an object of WorldSettings fields, e.g. {\"gravity_2d\": [0, -20]}");
    editor::EditorScene& scene = c.server.session().scene();
    const refl::TypeRecord& type = refl::type_of<kit::WorldSettings>();
    const editor::EntityId existing = editor::world_settings_entity(scene);
    // Validated on a copy first: a bad value creates nothing.
    kit::WorldSettings value;
    if (existing != editor::kNoEntity) value = *scene.get<kit::WorldSettings>(existing);
    cook::String err;
    if (!apply_patch(type, values, &value, scene.doc(), err)) {
        c.error += "WorldSettings: ";
        return fail(c, err.view());
    }
    const editor::EntityId id = editor::ensure_world_settings(scene);
    scene.set(id, value);
    c.out.begin_object();
    write_world_settings(c);
    c.out.end_object();
    return true;
}

const ToolDef kProjectTools[] = {
    {"project_get",
     "The project's .mceproj (name, title, engine_version, games, plugins, start_scene as a GUID, with "
     "start_scene_path) and the open scene's WorldSettings (null when the scene has none).",
     R"json({"type":"object","properties":{}})json", project_get},
    {"project_set",
     "Change .mceproj members: name, title, engine_version (strings), games, plugins (arrays of strings), "
     "start_scene (a scene's path under assets/ or \"guid:<uuid>\"). Checked before the file is written; other "
     "members are kept. A game's host reads start_scene when its build is configured: rebuild it to play the new one.",
     R"json({"type":"object","properties":{"values":{"type":"object","description":"e.g. {\"start_scene\": \"scenes/title.mcscene\", \"title\": \"My Game\"}"}},"required":["values"]})json",
     project_set},
    {"scene_settings_get", "The open scene's WorldSettings (2D and 3D physics gravity), or null when it has none.",
     R"json({"type":"object","properties":{}})json", scene_settings_get},
    {"scene_settings_set",
     "Change the open scene's WorldSettings fields (refl_describe_component WorldSettings lists them), e.g. "
     "{\"gravity_2d\": [0, -20]}; an entity \"World Settings\" carrying it is created when the scene has none. The "
     "stock player, run_headless and play-in-editor apply it at the first tick.",
     R"json({"type":"object","properties":{"values":{"type":"object"}},"required":["values"]})json", scene_settings_set},
};

}  // namespace

Span<const ToolDef> project_tool_table() { return Span<const ToolDef>(kProjectTools, sizeof(kProjectTools) / sizeof(kProjectTools[0])); }

}  // namespace mce::mcp
