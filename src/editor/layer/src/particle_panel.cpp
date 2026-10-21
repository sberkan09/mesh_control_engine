// The Particle panel (M-ASSETLOOK 4): a .mcpart's emitter parameters as a form beside a live swatch (the emitter
// run for 1.5 s on the CPU, thumb_from_particles). A double click on a particle preset in Assets opens it. Save
// cooks the text in memory first (mce/editor/asset_form.h), so a document the cook refuses is not written and the
// panel shows why; Revert reads the file again. The texture is picked from the project's textures or dropped from
// the Assets panel.

#include <imgui.h>

#include <cstring>

#include "mce/editor/asset_form.h"
#include "mce/editor/asset_thumb.h"
#include "mce/editor/editor_shell.h"
#include "mce/log.h"
#include "scene_panels.h"
#include "swatch_texture.h"

namespace mce::editor {
namespace {

constexpr u32 kSwatchPx = 128;

class ParticlePanel final : public IEditorPanel {
public:
    const char* name() const override { return kParticlePanel; }

    void draw(EditorShell& shell) override {
        EditorSession& session = shell.session();
        if (!session.project().is_open()) {
            ImGui::TextDisabled("Open a project to edit its particle presets.");
            return;
        }
        if (shell.particle_request() != guid_) switch_to(session, shell.particle_request());
        const ProjectAsset* asset = guid_.is_null() ? nullptr : session.project().find(guid_);
        if (!asset) {
            ImGui::TextDisabled("Double-click a particle preset in Assets to edit it here.");
            return;
        }
        ImGui::TextUnformatted(asset->path.c_str());
        if (dirty_) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "(unsaved)");
        }
        draw_swatch();
        ImGui::SameLine();
        ImGui::BeginGroup();
        const bool save = ImGui::Button("Save");
        ImGui::SameLine();
        const bool revert = ImGui::Button("Revert");
        ImGui::TextDisabled("The swatch is the emitter's shape and colour\nover 1.5 s; spin and the texture are not drawn.");
        ImGui::EndGroup();
        if (save) do_save(session, *asset);
        if (revert) load(session, *asset);
        if (!error_.empty()) ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", error_.c_str());
        ImGui::Separator();
        draw_form(session);
    }

private:
    void switch_to(EditorSession& session, asset::Guid next) {
        if (dirty_ && !guid_.is_null())
            if (const ProjectAsset* old = session.project().find(guid_)) do_save(session, *old);
        guid_ = next;
        error_ = cook::String();
        dirty_ = false;
        form_ = ParticleForm();
        if (const ProjectAsset* a = session.project().find(guid_)) load(session, *a);
        swatch_stale_ = true;
    }

    void load(EditorSession& session, const ProjectAsset& asset) {
        cook::String err;
        ParticleForm f;
        if (load_particle_form(session.project(), asset, f, err)) {
            form_ = static_cast<ParticleForm&&>(f);
            error_ = cook::String();
        } else {
            error_ = err;
        }
        dirty_ = false;
        swatch_stale_ = true;
    }

    void do_save(EditorSession& session, const ProjectAsset& asset) {
        const cook::String path = asset.path;  // the save rescans the catalog, `asset` dangles after
        cook::String err;
        if (save_particle_form(session.project(), asset, form_, err)) {
            error_ = cook::String();
            dirty_ = false;
            MCE_LOG_INFO("editor: saved {}", path.view());
        } else {
            error_ = err;
        }
    }

    void changed() {
        dirty_ = true;
        swatch_stale_ = true;
    }

    void draw_swatch() {
        if (swatch_stale_) {
            cook::Image img;
            thumb_from_particles(form_.swatch(), kSwatchPx, img);
            upload_swatch(tex_, img, kSwatchPx);
            swatch_stale_ = false;
        }
        if (tex_) ImGui::Image(tex_->GetTexRef(), ImVec2(static_cast<f32>(kSwatchPx), static_cast<f32>(kSwatchPx)));
    }

    void draw_form(EditorSession& session) {
        ParticleForm& f = form_;
        bool c = false;
        if (ImGui::CollapsingHeader("Emission", ImGuiTreeNodeFlags_DefaultOpen)) {
            c |= ImGui::DragFloat("Rate (per s)", &f.rate, 0.5f, 0.0f, 10000.0f);
            int burst = static_cast<int>(f.burst);
            if (ImGui::DragInt("Burst", &burst, 1.0f, 0, 65536)) {
                f.burst = static_cast<u32>(burst);
                c = true;
            }
            int max_particles = static_cast<int>(f.max_particles);
            if (ImGui::DragInt("Max particles", &max_particles, 1.0f, 1, 65536)) {
                f.max_particles = static_cast<u32>(max_particles);
                c = true;
            }
            c |= ImGui::DragFloat("Duration (s, 0 = forever)", &f.duration, 0.05f, 0.0f, 600.0f);
            c |= ImGui::DragFloat2("Area (half extents)", f.area, 0.01f, 0.0f, 100.0f);
        }
        if (ImGui::CollapsingHeader("Motion", ImGuiTreeNodeFlags_DefaultOpen)) {
            c |= range("Lifetime (s)", f.lifetime, 0.0f, 600.0f, 0.02f);
            c |= range("Speed", f.speed, 0.0f, 1000.0f, 0.05f);
            c |= ImGui::SliderFloat("Direction (deg)", &f.direction, -180.0f, 360.0f);
            c |= ImGui::SliderFloat("Spread (deg)", &f.spread, 0.0f, 360.0f);
            c |= ImGui::DragFloat2("Gravity", f.gravity, 0.05f, -100.0f, 100.0f);
            c |= ImGui::DragFloat("Drag", &f.drag, 0.01f, 0.0f, 100.0f);
            c |= ImGui::DragFloat2("Spin (deg/s)", f.spin, 1.0f, -3600.0f, 3600.0f);
        }
        if (ImGui::CollapsingHeader("Look", ImGuiTreeNodeFlags_DefaultOpen)) {
            c |= ImGui::DragFloat2("Size (start, end)", f.size, 0.005f, 0.0f, 100.0f);
            c |= ImGui::ColorEdit4("Color start", f.color_start, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_AlphaBar);
            c |= ImGui::ColorEdit4("Color end", f.color_end, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_AlphaBar);
            c |= ImGui::Checkbox("Additive blend", &f.additive);
            c |= draw_texture(session);
        }
        if (c) changed();
    }

    // A [min, max] pair that keeps min <= max (the cook refuses the other order).
    static bool range(const char* label, f32* v, f32 lo, f32 hi, f32 speed) {
        return ImGui::DragFloatRange2(label, &v[0], &v[1], speed, lo, hi);
    }

    bool draw_texture(EditorSession& session) {
        bool changed_tex = false;
        cook::String& ref = form_.texture;
        const char* current = "(none)";
        asset::Guid current_guid;
        if (!ref.empty()) {
            current = ref.c_str();  // an unknown reference shows as written
            StringView text = ref.view();
            if (text.size() > 5 && text.substr(0, 5) == "guid:") text = text.substr(5);
            if (const Result<asset::Guid> g = asset::parse_guid(text)) {
                current_guid = g.value();
                if (const ProjectAsset* t = session.project().find(current_guid)) current = t->path.c_str();
            }
        }
        if (ImGui::BeginCombo("Texture", current)) {
            if (ImGui::Selectable("(none)", ref.empty())) {
                ref = cook::String();
                changed_tex = true;
            }
            for (const ProjectAsset& t : session.project().assets()) {
                if (t.type != asset::ChunkType::Texture) continue;
                if (ImGui::Selectable(t.path.c_str(), t.guid == current_guid)) {
                    ref = cook::String("guid:");
                    ref += asset::to_string(t.guid).view();
                    changed_tex = true;
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kAssetPayload)) {
                asset::Guid g;
                std::memcpy(&g, p->Data, sizeof(g));
                if (const ProjectAsset* t = session.project().find(g); t && t->type == asset::ChunkType::Texture) {
                    ref = cook::String("guid:");
                    ref += asset::to_string(t->guid).view();
                    changed_tex = true;
                }
            }
            ImGui::EndDragDropTarget();
        }
        return changed_tex;
    }

    asset::Guid guid_;
    ParticleForm form_;
    cook::String error_;
    bool dirty_ = false;
    bool swatch_stale_ = true;
    ImTextureData* tex_ = nullptr;
};

}  // namespace

void add_particle_panel(EditorShell& shell) { shell.add_panel(mem::make_unique<ParticlePanel>(mem::heap())); }

}  // namespace mce::editor
