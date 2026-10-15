#include "mce/editor/project_settings.h"

#include <filesystem>
#include <string>

#include "mce/cook/fs.h"
#include "mce/kit/components.h"

namespace mce::editor {

namespace {

bool is_string_array(cook::JsonValue v) {
    if (!v.is_array()) return false;
    for (u32 i = 0; i < v.size(); ++i)
        if (!v.at(i).is_string() || v.at(i).as_string().empty()) return false;
    return true;
}

// start_scene: a registered Scene by GUID or path; the bare GUID text into `out`.
bool scene_guid(const EditorProject& project, cook::JsonValue v, cook::String& out, cook::String& err) {
    if (!v.is_string()) {
        err += "start_scene must be a scene's \"guid:<uuid>\" or its path under assets/";
        return false;
    }
    StringView text = v.as_string();
    const ProjectAsset* found = nullptr;
    for (const ProjectAsset& a : project.assets())
        if (a.path.view() == text) found = &a;
    if (!found) {
        if (text.size() > 5 && text.substr(0, 5) == "guid:") text = text.substr(5);
        if (const Result<asset::Guid> g = asset::parse_guid(text)) found = project.find(g.value());
    }
    if (!found || found->type != asset::ChunkType::Scene) {
        err += "start_scene: no scene of the project has that GUID or path (asset_list with type Scene lists them)";
        return false;
    }
    out = cook::String(asset::to_string(found->guid).view());
    return true;
}

}  // namespace

bool check_budgets(cook::JsonValue budgets, cook::String& err) {
    if (!budgets.is_object()) {
        err += "budgets must be an object of targets (desktop, mobile, web), e.g. {\"mobile\": {\"draws\": 300}}";
        return false;
    }
    for (u32 i = 0; i < budgets.size(); ++i) {
        perf::Target t;
        if (!perf::parse_target(budgets.key_at(i), t)) {
            err += "budgets: '";
            err += budgets.key_at(i);
            err += "' is not a target (desktop, mobile, web)";
            return false;
        }
        const cook::JsonValue b = budgets.at(i);
        if (!b.is_object()) {
            err += "budgets.";
            err += budgets.key_at(i);
            err += " must be an object of limits, e.g. {\"draws\": 300, \"frame_ms\": 33.3}";
            return false;
        }
        for (u32 k = 0; k < b.size(); ++k) {
            const perf::BudgetField* f = perf::find_budget_field(b.key_at(k));
            const cook::JsonValue v = b.at(k);
            if (!f) {
                err += "budgets.";
                err += budgets.key_at(i);
                err += ": '";
                err += b.key_at(k);
                err += "' is not a budget (";
                bool first = true;
                for (const perf::BudgetField& bf : perf::budget_fields()) {
                    err += first ? "" : ", ";
                    err += bf.name;
                    first = false;
                }
                err += ")";
                return false;
            }
            const bool ok = v.is_number() && v.as_number() >= 0 && v.as_number() <= (f->integer ? 4e9 : 10000.0) &&
                            (!f->integer || v.is_integer());
            if (!ok) {
                err += "budgets.";
                err += budgets.key_at(i);
                err += ".";
                err += f->name;
                err += f->integer ? " must be a whole number from 0 (no limit)" : " must be milliseconds from 0 (no limit) to 10000";
                return false;
            }
        }
    }
    return true;
}

bool project_budget(const EditorProject& project, perf::Target target, perf::Budget& out, cook::String& err) {
    out = perf::default_budget(target);
    const cook::String path = project_file_path(project);
    if (path.empty()) return true;
    cook::String text;
    cook::JsonDoc doc;
    cook::JsonError je;
    if (!cook::fs::read_text(path.view(), text) || !doc.parse(text.view(), je) || !doc.root().is_object()) {
        err += "the .mceproj file is not a JSON object";
        return false;
    }
    const cook::JsonValue budgets = doc.root().get("budgets");
    if (!budgets.valid()) return true;
    if (!check_budgets(budgets, err)) return false;
    const cook::JsonValue mine = budgets.get(perf::target_name(target));
    for (u32 k = 0; mine.valid() && k < mine.size(); ++k) perf::find_budget_field(mine.key_at(k))->set(out, mine.at(k).as_number());
    return true;
}

cook::String project_file_path(const EditorProject& project) {
    if (!project.is_open()) return {};
    // The root folder only, sorted: the first .mceproj by name.
    std::error_code ec;
    std::string best;
    for (std::filesystem::directory_iterator it(std::string(project.root()), ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.size() > 8 && name.compare(name.size() - 8, 8, ".mceproj") == 0 && (best.empty() || name < best)) best = name;
    }
    if (best.empty()) return {};
    return cook::path_join(project.root(), StringView(best.data(), best.size()));
}

bool patch_project_file(const EditorProject& project, cook::JsonValue patch, cook::String& err) {
    const cook::String path = project_file_path(project);
    if (path.empty()) {
        err += "the project has no .mceproj file in its folder";
        return false;
    }
    if (!patch.is_object() || patch.size() == 0) {
        err += "give an object of .mceproj members to change";
        return false;
    }
    cook::String start;
    for (u32 i = 0; i < patch.size(); ++i) {
        const StringView key = patch.key_at(i);
        const cook::JsonValue v = patch.at(i);
        bool ok = true;
        if (key == "name" || key == "title" || key == "engine_version") ok = v.is_string() && !v.as_string().empty() && v.as_string().size() < 128;
        else if (key == "games" || key == "plugins") ok = is_string_array(v);
        else if (key == "start_scene") {
            if (!scene_guid(project, v, start, err)) return false;
        } else if (key == "budgets") {
            if (!check_budgets(v, err)) return false;
        } else {
            err += "'";
            err += key;
            err += "' is not a project setting (name, title, engine_version, games, plugins, start_scene, budgets)";
            return false;
        }
        if (!ok) {
            err += "'";
            err += key;
            err += key == "games" || key == "plugins" ? "' must be an array of non-empty strings" : "' must be a non-empty string under 128 bytes";
            return false;
        }
    }
    cook::String text;
    cook::JsonDoc doc;
    cook::JsonError je;
    if (!cook::fs::read_text(path.view(), text) || !doc.parse(text.view(), je) || !doc.root().is_object()) {
        err += "the .mceproj file is not a JSON object";
        return false;
    }
    // Rewritten in the file's own layout: two-space indent, one member per line, arrays inline.
    const auto write_value = [&](cook::String& out, StringView key, cook::JsonValue v) {
        // Only a patched start_scene is the resolved GUID; an untouched one keeps the file's value.
        if (key == "start_scene" && patch.get("start_scene").valid()) return cook::append_json_string(out, start.view());
        cook::write_json(v, out, true);
    };
    const cook::JsonValue root = doc.root();
    cook::String out("{\n");
    bool first = true;
    const auto member = [&](StringView key, cook::JsonValue v) {
        out += first ? "  " : ",\n  ";
        first = false;
        cook::append_json_string(out, key);
        out += ": ";
        write_value(out, key, v);
    };
    for (u32 i = 0; i < root.size(); ++i) {
        const cook::JsonValue over = patch.get(root.key_at(i));
        member(root.key_at(i), over.valid() ? over : root.at(i));
    }
    for (u32 i = 0; i < patch.size(); ++i)
        if (!root.get(patch.key_at(i)).valid()) member(patch.key_at(i), patch.at(i));
    out += "\n}\n";
    if (!cook::fs::write_file_atomic(path.view(), Span<const byte>(reinterpret_cast<const byte*>(out.c_str()), out.size()))) {
        err += "could not write the .mceproj file";
        return false;
    }
    return true;
}

EntityId world_settings_entity(const EditorScene& scene) {
    for (const DocEntity& e : scene.doc().entities())
        if (e.find(refl::type_of<kit::WorldSettings>())) return e.id;
    return kNoEntity;
}

EntityId ensure_world_settings(EditorScene& scene) {
    if (const EntityId id = world_settings_entity(scene); id != kNoEntity) return id;
    scene.begin_edit("Add World Settings");
    const EntityId id = scene.create_entity("World Settings", false);
    scene.add_component(id, refl::type_of<kit::WorldSettings>());
    scene.end_edit();
    return id;
}

}  // namespace mce::editor
