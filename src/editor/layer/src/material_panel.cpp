// The Material panel (M-ASSETLOOK 3): a .mcmat's parameters and texture slots as a form beside a live swatch (a lit
// sphere drawn on the CPU from the scalar look, thumb_from_material). A double click on a material in Assets opens
// it. Save cooks the text in memory first (mce/editor/asset_form.h), so a document the cook refuses is not written
// and the panel shows why; Revert reads the file again. Textures are picked from the project's textures or dropped
// from the Assets panel.

#include <imgui.h>
#include <imgui_internal.h>  // RegisterUserTexture, ImTextureDataQueueUpload (the swatch is a user texture)

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

class MaterialPanel final : public IEditorPanel {
public:
    const char* name() const override { return kMaterialPanel; }

    void draw(EditorShell& shell) override {
        EditorSession& session = shell.session();
        if (!session.project().is_open()) {
            ImGui::TextDisabled("Open a project to edit its materials.");
            return;
        }
        if (shell.material_request() != guid_) switch_to(session, shell.material_request());
        const ProjectAsset* asset = guid_.is_null() ? nullptr : session.project().find(guid_);
        if (!asset) {
            ImGui::TextDisabled("Double-click a material in Assets to edit it here.");
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
        ImGui::TextDisabled("The swatch shows the scalar look;\ntextures and effects need the renderer.");
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
        form_ = MaterialForm();
        if (const ProjectAsset* a = session.project().find(guid_)) load(session, *a);
        swatch_stale_ = true;
    }

    void load(EditorSession& session, const ProjectAsset& asset) {
        cook::String err;
        MaterialForm f;
        if (load_material_form(session.project(), asset, f, err)) {
            form_ = static_cast<MaterialForm&&>(f);
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
        if (save_material_form(session.project(), asset, form_, err)) {
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
            thumb_from_material(form_.swatch(), kSwatchPx, img);
            upload_swatch(tex_, img, kSwatchPx);
            swatch_stale_ = false;
        }
        if (tex_) ImGui::Image(tex_->GetTexRef(), ImVec2(static_cast<f32>(kSwatchPx), static_cast<f32>(kSwatchPx)));
    }

    void draw_form(EditorSession& session) {
        MaterialForm& f = form_;
        bool c = false;
        c |= ImGui::ColorEdit4("Base color", f.base_color, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_AlphaBar);
        c |= ImGui::SliderFloat("Metallic", &f.metallic, 0.0f, 1.0f);
        c |= ImGui::SliderFloat("Roughness", &f.roughness, 0.0f, 1.0f);
        c |= ImGui::ColorEdit3("Emissive", f.emissive, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
        c |= ImGui::DragFloat2("UV scale", f.uv_scale, 0.05f, 0.0f, 64.0f);
        static constexpr const char* kModes[] = {"opaque", "mask", "blend"};
        c |= ImGui::Combo("Alpha mode", &f.alpha_mode, kModes, IM_ARRAYSIZE(kModes));
        if (f.alpha_mode == 1) c |= ImGui::SliderFloat("Alpha cutoff", &f.alpha_cutoff, 0.0f, 1.0f);
        c |= ImGui::Checkbox("Double sided", &f.double_sided);
        ImGui::SameLine();
        c |= ImGui::Checkbox("Unlit", &f.unlit);
        if (ImGui::CollapsingHeader("Surface layers")) {
            c |= ImGui::SliderFloat("Clearcoat", &f.clearcoat, 0.0f, 1.0f);
            c |= ImGui::SliderFloat("Clearcoat roughness", &f.clearcoat_roughness, 0.0f, 1.0f);
            c |= ImGui::SliderFloat("Transmission", &f.transmission, 0.0f, 1.0f);
            c |= ImGui::SliderFloat("IOR", &f.ior, 1.0f, 3.0f);
            c |= ImGui::SliderFloat("Height scale", &f.height_scale, 0.0f, 0.5f);
        }
        if (ImGui::CollapsingHeader("Textures", ImGuiTreeNodeFlags_DefaultOpen))
            for (u32 slot = 0; slot < MaterialForm::kSlots; ++slot) c |= draw_slot(session, slot);
        if (c) changed();
    }

    bool draw_slot(EditorSession& session, u32 slot) {
        bool changed_slot = false;
        cook::String& ref = form_.texture[slot];
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
        ImGui::PushID(static_cast<int>(slot));
        if (ImGui::BeginCombo(MaterialForm::slot_name(slot), current)) {
            if (ImGui::Selectable("(none)", ref.empty())) {
                ref = cook::String();
                changed_slot = true;
            }
            for (const ProjectAsset& t : session.project().assets()) {
                if (t.type != asset::ChunkType::Texture) continue;
                if (ImGui::Selectable(t.path.c_str(), t.guid == current_guid)) {
                    ref = cook::String("guid:");
                    ref += asset::to_string(t.guid).view();
                    changed_slot = true;
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
                    changed_slot = true;
                }
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::PopID();
        return changed_slot;
    }

    asset::Guid guid_;
    MaterialForm form_;
    cook::String error_;
    bool dirty_ = false;
    bool swatch_stale_ = true;
    ImTextureData* tex_ = nullptr;
};

}  // namespace

void add_material_panel(EditorShell& shell) { shell.add_panel(mem::make_unique<MaterialPanel>(mem::heap())); }

}  // namespace mce::editor
