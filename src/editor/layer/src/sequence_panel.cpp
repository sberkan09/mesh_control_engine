// The Sequence panel (M-SEQEDIT 2): a .mcseq's steps as a list that can be added to, removed and reordered, with the
// selected step's fields below. A double click on a sequence in Assets opens it. Save cooks the text in memory first
// (mce/editor/asset_form.h), so a document the cook refuses (a negative wait, a loop that never waits, a missing
// sound) is not written and the panel shows why; Revert reads the file again.

#include <imgui.h>

#include <cfloat>
#include <cstdio>
#include <cstring>

#include "mce/editor/asset_form.h"
#include "mce/editor/editor_shell.h"
#include "mce/log.h"
#include "scene_panels.h"

namespace mce::editor {
namespace {

class SequencePanel final : public IEditorPanel {
public:
    const char* name() const override { return kSequencePanel; }

    void draw(EditorShell& shell) override {
        EditorSession& session = shell.session();
        if (!session.project().is_open()) {
            ImGui::TextDisabled("Open a project to edit its sequences.");
            return;
        }
        if (shell.sequence_request() != guid_) switch_to(session, shell.sequence_request());
        const ProjectAsset* asset = guid_.is_null() ? nullptr : session.project().find(guid_);
        if (!asset) {
            ImGui::TextDisabled("Double-click a sequence in Assets to edit it here.");
            return;
        }
        ImGui::TextUnformatted(asset->path.c_str());
        if (dirty_) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "(unsaved)");
        }
        if (ImGui::Button("Save")) do_save(session, *asset);
        ImGui::SameLine();
        if (ImGui::Button("Revert")) load(session, *asset);
        if (!error_.empty()) ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", error_.c_str());
        ImGui::Separator();
        draw_header();
        draw_steps();
        draw_selected(session);
    }

private:
    void switch_to(EditorSession& session, asset::Guid next) {
        if (dirty_ && !guid_.is_null())
            if (const ProjectAsset* old = session.project().find(guid_)) do_save(session, *old);
        guid_ = next;
        error_ = cook::String();
        dirty_ = false;
        selected_ = -1;
        form_ = SequenceForm();
        if (const ProjectAsset* a = session.project().find(guid_)) load(session, *a);
    }

    void load(EditorSession& session, const ProjectAsset& asset) {
        cook::String err;
        SequenceForm f;
        if (load_sequence_form(session.project(), asset, f, err)) {
            form_ = static_cast<SequenceForm&&>(f);
            error_ = cook::String();
        } else {
            error_ = err;
        }
        dirty_ = false;
        selected_ = form_.steps.empty() ? -1 : 0;
    }

    void do_save(EditorSession& session, const ProjectAsset& asset) {
        const cook::String path = asset.path;  // the save rescans the catalog, `asset` dangles after
        cook::String err;
        if (save_sequence_form(session.project(), asset, form_, err)) {
            error_ = cook::String();
            dirty_ = false;
            MCE_LOG_INFO("editor: saved {}", path.view());
        } else {
            error_ = err;
        }
    }

    void draw_header() {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s", form_.name.c_str());
        if (ImGui::InputText("Name", buf, sizeof(buf))) {
            form_.name = cook::String(buf);
            dirty_ = true;
        }
        if (ImGui::Checkbox("Loop", &form_.loop)) dirty_ = true;
    }

    void draw_steps() {
        const int count = static_cast<int>(form_.steps.size());
        if (ImGui::BeginListBox("##steps", ImVec2(-FLT_MIN, 8 * ImGui::GetTextLineHeightWithSpacing()))) {
            for (int i = 0; i < count; ++i) {
                cook::String label = SequenceForm::describe(form_.steps[static_cast<u32>(i)]);
                cook::appendf(label, "###step%d", i);
                cook::String numbered;
                cook::appendf(numbered, "%d. ", i + 1);
                numbered += label.view();
                if (ImGui::Selectable(numbered.c_str(), i == selected_)) selected_ = i;
            }
            ImGui::EndListBox();
        }
        static constexpr const char* kKindNames[SequenceStepForm::kKinds] = {"wait", "wait_event", "move_to", "anim", "sound",
                                                                            "stop_sound", "emit", "call", "spawn", "destroy"};
        ImGui::SetNextItemWidth(130.0f);
        ImGui::Combo("##new_kind", &new_kind_, kKindNames, SequenceStepForm::kKinds);
        ImGui::SameLine();
        if (ImGui::Button("Add")) {
            SequenceStepForm s;
            s.kind = new_kind_;
            if (s.kind == SequenceStepForm::Wait) s.seconds = 1.0f;
            const int at = selected_ >= 0 ? selected_ + 1 : count;
            form_.steps.insert(static_cast<u32>(at), static_cast<SequenceStepForm&&>(s));
            selected_ = at;
            dirty_ = true;
        }
        ImGui::SameLine();
        const bool have = selected_ >= 0 && selected_ < count;
        ImGui::BeginDisabled(!have);
        if (ImGui::Button("Remove")) {
            form_.steps.erase(static_cast<u32>(selected_));
            selected_ = selected_ >= static_cast<int>(form_.steps.size()) ? static_cast<int>(form_.steps.size()) - 1 : selected_;
            dirty_ = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Up") && selected_ > 0) {
            swap_steps(selected_, selected_ - 1);
            --selected_;
        }
        ImGui::SameLine();
        if (ImGui::Button("Down") && selected_ + 1 < count) {
            swap_steps(selected_, selected_ + 1);
            ++selected_;
        }
        ImGui::EndDisabled();
    }

    void swap_steps(int a, int b) {
        SequenceStepForm tmp = static_cast<SequenceStepForm&&>(form_.steps[static_cast<u32>(a)]);
        form_.steps[static_cast<u32>(a)] = static_cast<SequenceStepForm&&>(form_.steps[static_cast<u32>(b)]);
        form_.steps[static_cast<u32>(b)] = static_cast<SequenceStepForm&&>(tmp);
        dirty_ = true;
    }

    void draw_selected(EditorSession& session) {
        if (selected_ < 0 || selected_ >= static_cast<int>(form_.steps.size())) return;
        SequenceStepForm& s = form_.steps[static_cast<u32>(selected_)];
        ImGui::Separator();
        ImGui::Text("Step %d: %s", selected_ + 1, SequenceStepForm::kind_name(s.kind));
        bool c = false;
        switch (s.kind) {
        case SequenceStepForm::Wait: c |= ImGui::DragFloat("Seconds", &s.seconds, 0.05f, 0.0f, 3600.0f); break;
        case SequenceStepForm::WaitEvent:
        case SequenceStepForm::Anim:
        case SequenceStepForm::Emit:
        case SequenceStepForm::Call: c |= name_field(s); break;
        case SequenceStepForm::MoveTo:
            c |= ImGui::DragFloat2("Target", s.pos, 0.05f);
            c |= ImGui::DragFloat("Speed (m/s)", &s.speed, 0.05f, 0.01f, 1000.0f);
            c |= ImGui::DragFloat("Tolerance (m)", &s.tolerance, 0.005f, 0.0f, 10.0f);
            c |= ImGui::Checkbox("Relative to the entity", &s.relative);
            break;
        case SequenceStepForm::Sound:
            c |= asset_field(session, s, asset::ChunkType::Sfx, "Sound");
            c |= ImGui::DragFloat("Gain (dB)", &s.gain_db, 0.1f, -60.0f, 12.0f);
            break;
        case SequenceStepForm::Spawn:
            c |= asset_field(session, s, asset::ChunkType::Prefab, "Prefab");
            c |= ImGui::DragFloat2("At", s.pos, 0.05f);
            c |= ImGui::Checkbox("Relative to the entity", &s.relative);
            break;
        default: ImGui::TextDisabled("This step has no fields."); break;
        }
        if (c) dirty_ = true;
    }

    static bool name_field(SequenceStepForm& s) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%s", s.text.c_str());
        if (!ImGui::InputText("Name", buf, sizeof(buf))) return false;
        s.text = cook::String(buf);
        return true;
    }

    // A "guid:<uuid>" reference picked from the project's assets of one type.
    static bool asset_field(EditorSession& session, SequenceStepForm& s, asset::ChunkType type, const char* label) {
        bool changed = false;
        const char* current = s.text.empty() ? "(none)" : s.text.c_str();
        asset::Guid current_guid;
        StringView text = s.text.view();
        if (text.size() > 5 && text.substr(0, 5) == "guid:") text = text.substr(5);
        if (const Result<asset::Guid> g = asset::parse_guid(text)) {
            current_guid = g.value();
            if (const ProjectAsset* a = session.project().find(current_guid)) current = a->path.c_str();
        }
        if (ImGui::BeginCombo(label, current)) {
            for (const ProjectAsset& a : session.project().assets()) {
                if (a.type != type) continue;
                if (ImGui::Selectable(a.path.c_str(), a.guid == current_guid)) {
                    s.text = cook::String("guid:");
                    s.text += asset::to_string(a.guid).view();
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kAssetPayload)) {
                asset::Guid g;
                std::memcpy(&g, p->Data, sizeof(g));
                if (const ProjectAsset* a = session.project().find(g); a && a->type == type) {
                    s.text = cook::String("guid:");
                    s.text += asset::to_string(a->guid).view();
                    changed = true;
                }
            }
            ImGui::EndDragDropTarget();
        }
        return changed;
    }

    asset::Guid guid_;
    SequenceForm form_;
    cook::String error_;
    bool dirty_ = false;
    int selected_ = -1;
    int new_kind_ = 0;
};

}  // namespace

void add_sequence_panel(EditorShell& shell) { shell.add_panel(mem::make_unique<SequencePanel>(mem::heap())); }

}  // namespace mce::editor
