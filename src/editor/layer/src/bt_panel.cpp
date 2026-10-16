// The Behavior Tree panel (ROADMAP M-AI step 5): the open .mcbt drawn as a tree (right-click a node
// to add a child, wrap, unwrap, move or remove it), an inspector for the selected node's value and
// members, the blackboard's keys, and the cook's problems. Edits go through BtEditor
// (bt_editor.h): cooked before they are kept, saved canonically once nothing is held and
// hot-reloaded. In play-in-editor the panel debugs a brain running the tree: each node's status
// (the running path green, how the others last finished), a utility child's score, the root's result
// and the blackboard's live values; "Run edits" runs the unsaved tree in the play world. The
// viewport draws the perceivers' cones while playing (View > Perception).

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "mce/editor/bt_editor.h"
#include "mce/editor/editor_shell.h"
#include "mce/log.h"
#include "scene_panels.h"

namespace mce::editor {
namespace {

using kit::BtStatus;

void copy_text(char* dst, usize cap, StringView s) {
    const usize n = std::min(cap - 1, s.size());
    std::memcpy(dst, s.data(), n);
    dst[n] = 0;
}

ImVec4 status_color(BtStatus s) {
    switch (s) {
    case BtStatus::Running: return ImVec4(0.35f, 0.85f, 0.45f, 1.0f);
    case BtStatus::Success: return ImVec4(0.45f, 0.65f, 1.0f, 1.0f);
    case BtStatus::Failure: return ImVec4(1.0f, 0.45f, 0.4f, 1.0f);
    case BtStatus::Idle: break;
    }
    return ImVec4(0.6f, 0.6f, 0.62f, 1.0f);
}

const char* status_name(BtStatus s) {
    switch (s) {
    case BtStatus::Running: return "running";
    case BtStatus::Success: return "success";
    case BtStatus::Failure: return "failure";
    case BtStatus::Idle: break;
    }
    return "idle";
}

struct FieldBuf {
    char name[64];
    char json[256];
};

class BtPanel final : public IEditorPanel {
public:
    BtPanel() { open = false; }  // the View menu, or a double click on an .mcbt in Assets

    const char* name() const override { return kBtPanel; }

    void draw(EditorShell& shell) override {
        EditorSession& session = shell.session();
        BtEditor& ed = shell.bt();
        if (!session.project().is_open()) {
            ImGui::TextDisabled("Open a project to edit its behavior trees.");
            return;
        }
        if (ed.dirty() && !ImGui::IsAnyItemActive()) save(shell);
        const f64 now = ImGui::GetTime();
        if (now - synced_ > 0.5) {
            synced_ = now;
            ed.sync(session);
        }
        draw_header(shell, ed);
        if (!ed.last_error().empty())
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%.*s", static_cast<int>(ed.last_error().size()), ed.last_error().data());
        if (!ed.is_open()) {
            ImGui::TextDisabled("Double-click an .mcbt in Assets, or make a new one.");
            return;
        }
        live_ok_ = false;
        draw_play(shell, ed);

        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const f32 side = std::max(300.0f, avail.x * 0.4f);
        if (ImGui::BeginChild("##tree", ImVec2(avail.x - side - ImGui::GetStyle().ItemSpacing.x, 0), ImGuiChildFlags_Borders)) {
            if (!ed.doc().nodes.empty()) draw_node(ed, 0);
        }
        ImGui::EndChild();
        ImGui::SameLine();
        if (ImGui::BeginChild("##side")) {
            draw_inspector(ed);
            draw_blackboard(ed);
            draw_problems(ed);
        }
        ImGui::EndChild();
    }

    void shutdown(EditorShell& shell) override {
        if (shell.bt().dirty()) save(shell);
    }

private:
    void save(EditorShell& shell) {
        if (Result<void> r = shell.bt().save(shell.session()); !r) {
            FixedString<512> msg;
            format_error(r.error(), msg);
            MCE_LOG_ERROR("editor: saving the behavior tree failed: {}", msg.view());
        }
    }

    // ---- header ----

    void draw_header(EditorShell& shell, BtEditor& ed) {
        EditorSession& session = shell.session();
        const FixedString<200> label(ed.is_open() ? ed.path() : StringView("(choose a tree)"));
        ImGui::SetNextItemWidth(std::max(160.0f, ImGui::GetContentRegionAvail().x * 0.3f));
        if (ImGui::BeginCombo("##bt_pick", label.c_str())) {
            for (const ProjectAsset& a : session.project().assets()) {
                if (a.type != asset::ChunkType::BehaviorTree) continue;
                if (ImGui::Selectable(a.path.c_str(), a.guid == ed.guid()) && a.guid != ed.guid()) shell.open_bt(a.guid);
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("New...")) ImGui::OpenPopup("##new");
        if (ImGui::BeginPopup("##new")) {
            ImGui::InputText("File", new_path_, sizeof(new_path_));
            if (ImGui::Button("Create")) {
                if (ed.dirty()) save(shell);
                if (Result<asset::Guid> r = ed.create(session, StringView(new_path_)); r) {
                    ImGui::CloseCurrentPopup();
                    new_error_[0] = 0;
                } else {
                    FixedString<512> msg;
                    format_error(r.error(), msg);
                    std::snprintf(new_error_, sizeof(new_error_), "%s", msg.c_str());
                }
            }
            if (new_error_[0]) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", new_error_);
            ImGui::EndPopup();
        }
        if (!ed.is_open()) return;
        ImGui::SameLine();
        ImGui::BeginDisabled(!ed.can_undo());
        if (ImGui::Button("Undo")) ed.undo(session);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!ed.can_redo());
        if (ImGui::Button("Redo")) ed.redo(session);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ed.cooks()) ImGui::TextColored(ImVec4(0.45f, 0.8f, 0.5f, 1.0f), "cooks");
        else ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%u problem(s)", static_cast<u32>(ed.problems().size()));
        if (name_rev_ != ed.revision()) {
            name_rev_ = ed.revision();
            copy_text(tree_name_, sizeof(tree_name_), ed.doc().name.view());
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputText("Name##tree", tree_name_, sizeof(tree_name_));
        if (ImGui::IsItemDeactivatedAfterEdit()) ed.set_name(StringView(tree_name_));
    }

    // ---- play-in-editor ----

    void draw_play(EditorShell& shell, BtEditor& ed) {
        EditorSession& session = shell.session();
        if (!session.play().active()) {
            ImGui::TextDisabled("Play to debug the brains running this tree.");
            return;
        }
        bt_play_brains(session, ed, brains_);
        if (brains_.empty()) {
            ImGui::TextDisabled("No entity in the play world runs this tree.");
            return;
        }
        if (brain_ >= brains_.size()) brain_ = 0;
        char label[64];
        std::snprintf(label, sizeof(label), "Brain #%u", brains_[brain_].index);
        ImGui::SetNextItemWidth(140.0f);
        if (ImGui::BeginCombo("##brain", label)) {
            for (u32 i = 0; i < brains_.size(); ++i) {
                std::snprintf(label, sizeof(label), "Brain #%u", brains_[i].index);
                if (ImGui::Selectable(label, i == brain_)) brain_ = i;
            }
            ImGui::EndCombo();
        }
        live_ok_ = bt_live(session, ed, brains_[brain_], live_);
        if (!live_ok_) return;
        ImGui::SameLine();
        ImGui::TextColored(status_color(live_.result), "root %s", status_name(live_.result));
        ImGui::SameLine();
        ImGui::TextDisabled("runs %u  %.1f s%s", live_.runs, static_cast<double>(live_.time), live_.active ? "" : "  (paused)");
        ImGui::SameLine();
        ImGui::BeginDisabled(!ed.cooks());
        if (ImGui::Button("Run edits")) bt_push_to_play(session, ed);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Run the tree as it is now in the play world (its brains restart); nothing is saved.");
        ImGui::SameLine();
        ImGui::Checkbox("Perception", &shell.view().show_perception);
        if (!live_.current) ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f), "The play world runs an older tree: Run edits to debug this one.");
    }

    // ---- the tree ----

    void node_label(const BtEditor& ed, u32 i, char* out, usize cap) const {
        const cook::BtDocNode& n = ed.doc().nodes[i];
        const StringView name = n.field("name");
        int len = std::snprintf(out, cap, "%s", n.kind.c_str());
        if (!n.value.empty() && len >= 0 && static_cast<usize>(len) < cap)
            len += std::snprintf(out + len, cap - static_cast<usize>(len), " %.*s", static_cast<int>(n.value.size()), n.value.view().data());
        if (!name.empty() && len >= 0 && static_cast<usize>(len) < cap)
            len += std::snprintf(out + len, cap - static_cast<usize>(len), "  %.*s", static_cast<int>(name.size()), name.data());
        if (live_ok_ && i < live_.score.size() && !std::isnan(live_.score[i]) && len >= 0 && static_cast<usize>(len) < cap)
            std::snprintf(out + len, cap - static_cast<usize>(len), "  [%.2f]", static_cast<double>(live_.score[i]));
    }

    void draw_node(BtEditor& ed, u32 i) {
        const cook::BtDocNode& n = ed.doc().nodes[i];
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (n.children.empty()) flags |= ImGuiTreeNodeFlags_Leaf;
        if (ed.selected() == i) flags |= ImGuiTreeNodeFlags_Selected;
        char label[256];
        node_label(ed, i, label, sizeof(label));
        const BtStatus st = live_ok_ && live_.current && i < live_.status.size() ? live_.status[i] : BtStatus::Idle;
        ImGui::PushID(static_cast<int>(i));
        ImGui::PushStyleColor(ImGuiCol_Text, live_ok_ && live_.current ? status_color(st) : ImGui::GetStyleColorVec4(ImGuiCol_Text));
        const bool opened = ImGui::TreeNodeEx("##node", flags, "%s", label);
        ImGui::PopStyleColor();
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) ed.select(i);
        if (ImGui::BeginPopupContextItem("##menu")) {
            ed.select(i);
            node_menu(ed, i);
            ImGui::EndPopup();
        }
        if (opened) {
            // Copy: an edit from the menu renumbers the document.
            const u64 rev = ed.revision();
            const Array<u32> kids = n.children;
            for (u32 c : kids) {
                if (ed.revision() != rev) break;
                if (c < ed.doc().nodes.size()) draw_node(ed, c);
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }

    void node_menu(BtEditor& ed, u32 i) {
        const cook::BtArity arity = cook::bt_kind_arity(ed.doc().nodes[i].kind.view());
        const bool can_add = arity == cook::BtArity::Many || (arity == cook::BtArity::One && ed.doc().nodes[i].children.empty());
        if (ImGui::BeginMenu("Add child", can_add)) {
            for (const char* k : cook::bt_kind_names())
                if (ImGui::MenuItem(k)) ed.add_node(i, k);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Wrap in")) {
            for (const char* k : cook::bt_kind_names())
                if (cook::bt_kind_arity(k) != cook::BtArity::Leaf && ImGui::MenuItem(k)) ed.wrap_node(i, k);
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Unwrap", nullptr, false, ed.doc().nodes[i].children.size() == 1)) ed.unwrap_node(i);
        const bool child = ed.doc().parent(i) != kNoBtNode;
        if (ImGui::MenuItem("Move up", nullptr, false, child)) ed.move_node(i, -1);
        if (ImGui::MenuItem("Move down", nullptr, false, child)) ed.move_node(i, 1);
        ImGui::Separator();
        if (ImGui::MenuItem("Remove", nullptr, false, child)) ed.remove_node(i);
    }

    // ---- the inspector ----

    void load_node(const BtEditor& ed, u32 i) {
        node_rev_ = ed.revision();
        node_of_ = i;
        fields_.clear();
        value_[0] = 0;
        if (i >= ed.doc().nodes.size()) return;
        const cook::BtDocNode& n = ed.doc().nodes[i];
        copy_text(value_, sizeof(value_), n.value.view());
        for (const cook::BtDocField& f : n.fields) {
            FieldBuf b;
            copy_text(b.name, sizeof(b.name), f.name.view());
            copy_text(b.json, sizeof(b.json), f.json.view());
            fields_.push_back(b);
        }
    }

    void draw_inspector(BtEditor& ed) {
        ImGui::SeparatorText("Node");
        const u32 i = ed.selected();
        if (i >= ed.doc().nodes.size()) {
            ImGui::TextDisabled("Select a node (right-click it to add, wrap or remove).");
            return;
        }
        if (node_rev_ != ed.revision() || node_of_ != i) load_node(ed, i);
        const cook::BtDocNode& n = ed.doc().nodes[i];
        ImGui::Text("%s", n.kind.c_str());
        if (live_ok_ && live_.current && i < live_.status.size()) {
            ImGui::SameLine();
            ImGui::TextColored(status_color(live_.status[i]), "%s", status_name(live_.status[i]));
        }
        ImGui::TextDisabled("Values are JSON: \"text\", 2, true, [0, 1, 0].");
        if (cook::bt_kind_arity(n.kind.view()) == cook::BtArity::Leaf) {
            ImGui::InputText("Value", value_, sizeof(value_));
            if (ImGui::IsItemDeactivatedAfterEdit()) ed.set_value(i, StringView(value_));
        }
        for (u32 f = 0; f < fields_.size(); ++f) {
            ImGui::PushID(static_cast<int>(f));
            ImGui::InputText(fields_[f].name, fields_[f].json, sizeof(fields_[f].json));
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                const FieldBuf b = fields_[f];
                ed.set_field(i, StringView(b.name), StringView(b.json));
                ImGui::PopID();
                return;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) {
                const FieldBuf b = fields_[f];
                ed.set_field(i, StringView(b.name), {});
                ImGui::PopID();
                return;
            }
            ImGui::PopID();
        }
        ImGui::SetNextItemWidth(100.0f);
        ImGui::InputText("##new_field", new_field_, sizeof(new_field_));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140.0f);
        ImGui::InputText("##new_json", new_json_, sizeof(new_json_));
        ImGui::SameLine();
        if (ImGui::Button("+ Field") && new_field_[0] && ed.set_field(i, StringView(new_field_), StringView(new_json_))) new_field_[0] = new_json_[0] = 0;
    }

    // ---- the blackboard ----

    void draw_blackboard(BtEditor& ed) {
        ImGui::SeparatorText("Blackboard");
        const cook::BtDocument& doc = ed.doc();
        if (keys_rev_ != ed.revision()) {
            keys_rev_ = ed.revision();
            keys_.clear();
            for (const cook::BtDocKey& k : doc.keys) {
                FieldBuf b;
                copy_text(b.name, sizeof(b.name), k.name.view());
                copy_text(b.json, sizeof(b.json), k.def.view());
                keys_.push_back(b);
            }
        }
        const Span<const char* const> types = cook::bt_key_type_names();
        if (ImGui::BeginTable("##keys", live_ok_ ? 5 : 4, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
            for (u32 k = 0; k < doc.keys.size() && k < keys_.size(); ++k) {
                ImGui::PushID(static_cast<int>(k));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::InputText("##name", keys_[k].name, sizeof(keys_[k].name));
                bool commit = ImGui::IsItemDeactivatedAfterEdit();
                ImGui::TableNextColumn();
                cook::BtDocKey next = doc.keys[k];
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::BeginCombo("##type", next.type.c_str())) {
                    for (const char* t : types)
                        if (ImGui::Selectable(t, next.type.view() == StringView(t))) {
                            next.type.assign(t);
                            if (next.type.view() == "entity") next.def = cook::String();
                            commit = true;
                        }
                    ImGui::EndCombo();
                }
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::BeginDisabled(next.type.view() == "entity");
                ImGui::InputTextWithHint("##def", "no default", keys_[k].json, sizeof(keys_[k].json));
                commit |= ImGui::IsItemDeactivatedAfterEdit();
                ImGui::EndDisabled();
                if (live_ok_) {
                    ImGui::TableNextColumn();
                    if (k < live_.keys.size() && live_.keys[k].set) ImGui::TextColored(ImVec4(0.45f, 0.8f, 0.5f, 1.0f), "%s", live_.keys[k].value.c_str());
                    else ImGui::TextDisabled("unset");
                }
                ImGui::TableNextColumn();
                const bool remove = ImGui::SmallButton("x");
                ImGui::PopID();
                if (remove) {
                    ed.remove_key(k);
                    break;
                }
                if (commit) {
                    next.name.assign(StringView(keys_[k].name));
                    if (next.type.view() != "entity") next.def.assign(StringView(keys_[k].json));
                    ed.set_key(k, next);
                    break;
                }
            }
            ImGui::EndTable();
        }
        ImGui::SetNextItemWidth(120.0f);
        ImGui::InputTextWithHint("##key_name", "key", new_key_, sizeof(new_key_));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        ImGui::Combo("##key_type", &new_type_, types.data(), static_cast<int>(types.size()));
        ImGui::SameLine();
        if (ImGui::Button("+ Key") && new_key_[0] && ed.add_key(StringView(new_key_), types[static_cast<usize>(new_type_)])) new_key_[0] = 0;
    }

    void draw_problems(const BtEditor& ed) {
        if (ed.problems().empty()) return;
        ImGui::SeparatorText("Problems");
        for (const cook::String& p : ed.problems()) ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.45f, 1.0f), "%s", p.c_str());
    }

    char new_path_[256] = "ai/brain.mcbt";
    char new_error_[256] = {};
    char tree_name_[64] = {};
    u64 name_rev_ = ~u64(0);
    char value_[256] = {};
    Array<FieldBuf> fields_;
    u64 node_rev_ = ~u64(0);
    u32 node_of_ = kNoBtNode;
    char new_field_[64] = {};
    char new_json_[256] = {};
    Array<FieldBuf> keys_;
    u64 keys_rev_ = ~u64(0);
    char new_key_[64] = {};
    int new_type_ = 0;
    Array<Entity> brains_;
    u32 brain_ = 0;
    BtLive live_;
    bool live_ok_ = false;
    f64 synced_ = 0.0;
};

}  // namespace

void add_bt_panel(EditorShell& shell) { shell.add_panel(mem::make_unique<BtPanel>(mem::heap())); }

}  // namespace mce::editor
