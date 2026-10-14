// Project Settings (M-DATA-REACH 3): the .mceproj's title and start scene, and the open scene's
// WorldSettings, over the same calls as MCP's project_set / scene_settings_set
// (mce/editor/project_settings.h).

#include <imgui.h>

#include <cstdio>

#include "mce/cook/fs.h"
#include "mce/editor/editor_shell.h"
#include "mce/editor/project_settings.h"
#include "mce/kit/components.h"
#include "mce/log.h"
#include "scene_panels.h"

namespace mce::editor {

namespace {

class ProjectSettingsPanel final : public IEditorPanel {
public:
    const char* name() const override { return "Project Settings"; }

    void draw(EditorShell& shell) override {
        EditorSession& session = shell.session();
        if (!session.project().is_open()) {
            ImGui::TextDisabled("No project is open.");
            return;
        }
        if (!loaded_) load(session);
        if (file_.empty()) {
            ImGui::TextDisabled("The project folder has no .mceproj file.");
        } else {
            ImGui::TextDisabled("%s", file_.c_str());
            ImGui::InputText("Title", title_, sizeof(title_));
            Array<const ProjectAsset*> scenes;
            session.list_scenes(scenes);
            const char* current = "(none)";
            for (const ProjectAsset* a : scenes)
                if (a->guid == start_) current = a->path.c_str();
            if (ImGui::BeginCombo("Start scene", current)) {
                for (const ProjectAsset* a : scenes)
                    if (ImGui::Selectable(a->path.c_str(), a->guid == start_)) start_ = a->guid;
                ImGui::EndCombo();
            }
            if (ImGui::Button("Save project")) save(session);
            ImGui::SameLine();
            if (ImGui::Button("Reload")) loaded_ = false;
            ImGui::TextDisabled("A game's build reads the start scene when it is configured.");
        }

        ImGui::Separator();
        ImGui::TextUnformatted("Scene: World Settings");
        EditorScene& scene = shell.scene();
        const EntityId id = world_settings_entity(scene);
        if (id == kNoEntity) {
            if (ImGui::Button("Add World Settings")) ensure_world_settings(scene);
            return;
        }
        kit::WorldSettings ws = *scene.get<kit::WorldSettings>(id);
        bool changed = ImGui::DragFloat2("Gravity 2D", &ws.gravity_2d.x, 0.1f);
        changed |= ImGui::DragFloat3("Gravity 3D", &ws.gravity_3d.x, 0.1f);
        if (changed) scene.set(id, ws);
        if (ImGui::IsItemDeactivatedAfterEdit()) scene.seal_undo();
    }

private:
    void load(EditorSession& session) {
        loaded_ = true;
        file_ = project_file_path(session.project());
        title_[0] = '\0';
        start_ = {};
        cook::String text;
        cook::JsonDoc doc;
        cook::JsonError err;
        if (file_.empty() || !cook::fs::read_text(file_.view(), text) || !doc.parse(text.view(), err)) return;
        if (const cook::JsonValue t = doc.root().get("title"); t.is_string())
            std::snprintf(title_, sizeof(title_), "%.*s", static_cast<int>(t.as_string().size()), t.as_string().data());
        if (const cook::JsonValue s = doc.root().get("start_scene"); s.is_string())
            if (const Result<asset::Guid> g = asset::parse_guid(s.as_string())) start_ = g.value();
    }

    void save(EditorSession& session) {
        cook::String patch("{\"title\": ");
        cook::append_json_string(patch, StringView(title_));
        if (!start_.is_null()) {
            patch += ", \"start_scene\": \"";
            patch += asset::to_string(start_).view();
            patch += '"';
        }
        patch += '}';
        cook::JsonDoc doc;
        cook::JsonError je;
        cook::String err;
        if (!doc.parse(patch.view(), je) || !patch_project_file(session.project(), doc.root(), err))
            MCE_LOG_ERROR("editor: project settings not saved: {}", err.view());
        else
            MCE_LOG_INFO("editor: saved {}", file_.view());
    }

    bool loaded_ = false;
    cook::String file_;
    char title_[128] = {};
    asset::Guid start_;
};

}  // namespace

void add_project_settings_panel(EditorShell& shell) { shell.add_panel(mem::make_unique<ProjectSettingsPanel>(mem::heap())); }

}  // namespace mce::editor
