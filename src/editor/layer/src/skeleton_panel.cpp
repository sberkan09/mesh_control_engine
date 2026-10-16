// The Skeleton panel (ROADMAP M-ANIM2D step 5): the open .mcskel2d as a tree of bones and a list of
// slots. Select a bone to edit its rest pose (x, y, rotation, scale) and read its world transform;
// pick an animation and scrub its time to preview the pose (IK constraints can be aimed at a point),
// key the selected bone's channels at that time, or add bones, slots, constraints and animations.
// Edits go through Skeleton2DEditor (skeleton2d_editor.h), the document code the MCP skel2d_* tools
// share: cooked by the real importer before they are kept, saved once nothing is held and
// hot-reloaded. The viewport draws the bones, in the previewed pose, on selected entities that play
// the skeleton (the scene's sprites show the saved pose).

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "mce/editor/editor_shell.h"
#include "mce/editor/skeleton2d_editor.h"
#include "mce/log.h"
#include "scene_panels.h"

namespace mce::editor {
namespace {

constexpr f64 kRadToDeg = 57.295779513082320876798;
const ImVec4 kErrorColor(1.0f, 0.45f, 0.4f, 1.0f);

class SkeletonPanel final : public IEditorPanel {
public:
    SkeletonPanel() { open = false; }  // the View menu, or a double click on a .mcskel2d in Assets

    const char* name() const override { return kSkeletonPanel; }

    void draw(EditorShell& shell) override {
        EditorSession& session = shell.session();
        Skeleton2DEditor& ed = shell.skeleton2d();
        if (!session.project().is_open()) {
            ImGui::TextDisabled("Open a project to edit its 2D skeletons.");
            return;
        }
        if (ed.dirty() && !ImGui::IsAnyItemActive()) save(shell);
        const f64 now = ImGui::GetTime();
        if (now - synced_ > 0.5) {
            synced_ = now;
            ed.sync(session);
        }
        draw_header(shell, ed);
        if (!ed.last_error().empty()) {
            const FixedString<512> msg(ed.last_error());
            ImGui::TextColored(kErrorColor, "%s", msg.c_str());
        }
        if (!ed.is_open()) {
            ImGui::TextDisabled("Double-click a .mcskel2d in Assets, or make a new one.");
            return;
        }
        draw_animation(shell, ed);

        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const f32 side = std::max(260.0f, avail.x * 0.5f);
        if (ImGui::BeginChild("##tree", ImVec2(avail.x - side - ImGui::GetStyle().ItemSpacing.x, 0),
                              ImGuiChildFlags_Borders)) {
            draw_bones(ed);
            ImGui::SeparatorText("Slots");
            draw_slots(ed);
            ImGui::SeparatorText("IK constraints");
            draw_ik(ed);
        }
        ImGui::EndChild();
        ImGui::SameLine();
        if (ImGui::BeginChild("##side")) draw_bone_inspector(ed);
        ImGui::EndChild();
    }

    void shutdown(EditorShell& shell) override {
        if (shell.skeleton2d().dirty()) save(shell);
    }

private:
    void save(EditorShell& shell) {
        if (Result<void> r = shell.skeleton2d().save(shell.session()); !r) {
            FixedString<512> msg;
            format_error(r.error(), msg);
            MCE_LOG_ERROR("editor: saving the 2D skeleton failed: {}", msg.view());
        }
    }

    // ---- header ----

    void draw_header(EditorShell& shell, Skeleton2DEditor& ed) {
        EditorSession& session = shell.session();
        const FixedString<200> label(ed.is_open() ? ed.path() : StringView("(choose a skeleton)"));
        ImGui::SetNextItemWidth(std::max(160.0f, ImGui::GetContentRegionAvail().x * 0.3f));
        if (ImGui::BeginCombo("##skel_pick", label.c_str())) {
            for (const ProjectAsset& a : session.project().assets()) {
                if (a.type != asset::ChunkType::Skeleton2D) continue;
                if (ImGui::Selectable(a.path.c_str(), a.guid == ed.guid()) && a.guid != ed.guid())
                    shell.open_skeleton2d(a.guid);
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
            if (new_error_[0]) ImGui::TextColored(kErrorColor, "%s", new_error_);
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
        ImGui::TextDisabled("%u bones, %u slots", static_cast<u32>(ed.info().bones.size()),
                            static_cast<u32>(ed.info().slots.size()));
    }

    // ---- animation preview ----

    void draw_animation(EditorShell& shell, Skeleton2DEditor& ed) {
        const Skeleton2DInfo& info = ed.info();
        const FixedString<64> current(ed.animation().empty() ? StringView("(rest pose)") : ed.animation());
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::BeginCombo("Animation", current.c_str())) {
            if (ImGui::Selectable("(rest pose)", ed.animation().empty())) ed.set_animation({});
            for (const Skeleton2DInfo::Animation& a : info.animations)
                if (ImGui::Selectable(a.name.c_str(), a.name.view() == ed.animation())) ed.set_animation(a.name.view());
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("New animation...")) ImGui::OpenPopup("##new_anim");
        if (ImGui::BeginPopup("##new_anim")) {
            ImGui::InputText("Name", anim_name_, sizeof(anim_name_));
            ImGui::TextDisabled("Starts with a rotation key at 0 s on the selected bone (or the first).");
            if (ImGui::Button("Create") && anim_name_[0]) {
                skel2d::KeySpec k;
                k.animation = cook::String(StringView(anim_name_));
                k.target = info.bones.empty()
                               ? cook::String()
                               : info.bones[ed.selected_bone() < info.bones.size() ? ed.selected_bone() : 0].name;
                if (ed.key(k)) ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        if (ed.animation().empty()) return;
        const f32 length = std::max(ed.animation_length(), 0.01f);
        ImGui::SameLine();
        if (ImGui::Button(playing_ ? "Pause" : "Play")) playing_ = !playing_;
        if (playing_) {
            f32 t = ed.time() + ImGui::GetIO().DeltaTime;
            if (t > length) t = std::fmod(t, length);
            ed.set_time(t);
        }
        f32 t = ed.time();
        ImGui::SetNextItemWidth(std::max(120.0f, ImGui::GetContentRegionAvail().x - 80.0f));
        if (ImGui::SliderFloat("Time", &t, 0.0f, length, "%.3f s")) {
            playing_ = false;
            ed.set_time(t);
        }
        (void)shell;
    }

    // ---- the tree of bones ----

    void draw_bones(Skeleton2DEditor& ed) {
        const Skeleton2DInfo& info = ed.info();
        ImGui::SeparatorText("Bones");
        for (u32 i = 0; i < info.bones.size(); ++i)
            if (info.bones[i].parent < 0) draw_bone(ed, i);
        if (ImGui::Button("Add bone...")) ImGui::OpenPopup("##add_bone");
        if (ImGui::BeginPopup("##add_bone")) {
            ImGui::InputText("Name", bone_name_, sizeof(bone_name_));
            const u32 sel = ed.selected_bone();
            if (sel < info.bones.size())
                ImGui::Text("Child of %s", info.bones[sel].name.c_str());
            else
                ImGui::TextDisabled("No bone is selected: a new root.");
            ImGui::DragFloat("Length", &bone_length_, 0.01f, 0.0f, 100.0f);
            if (ImGui::Button("Add") && bone_name_[0]) {
                skel2d::BoneSpec b;
                b.name = cook::String(StringView(bone_name_));
                if (sel < info.bones.size()) {
                    b.parent = info.bones[sel].name;
                    b.x = info.bones[sel].length;  // starts at the parent's tip
                }
                b.length = static_cast<f64>(bone_length_);
                if (ed.add_bone(b)) {
                    bone_name_[0] = 0;
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndPopup();
        }
    }

    void draw_bone(Skeleton2DEditor& ed, u32 index) {
        const Skeleton2DInfo& info = ed.info();
        bool has_children = false;
        for (const Skeleton2DInfo::Bone& b : info.bones) has_children |= b.parent == static_cast<i32>(index);
        ImGuiTreeNodeFlags flags =
            ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (!has_children) flags |= ImGuiTreeNodeFlags_Leaf;
        if (ed.selected_bone() == index) flags |= ImGuiTreeNodeFlags_Selected;
        const bool open_node = ImGui::TreeNodeEx(info.bones[index].name.c_str(), flags);
        if (ImGui::IsItemClicked()) ed.select_bone(index);
        if (open_node) {
            for (u32 i = 0; i < info.bones.size(); ++i)
                if (info.bones[i].parent == static_cast<i32>(index)) draw_bone(ed, i);
            ImGui::TreePop();
        }
    }

    // ---- slots and constraints ----

    void draw_slots(Skeleton2DEditor& ed) {
        const Skeleton2DInfo& info = ed.info();
        for (const Skeleton2DInfo::Slot& s : info.slots) {
            const i32 bone = info.find_bone(s.bone.view());
            if (ImGui::Selectable(s.name.c_str(), false) && bone >= 0) ed.select_bone(static_cast<u32>(bone));
            ImGui::SameLine();
            ImGui::TextDisabled("on %s, %u attachment(s)", s.bone.c_str(), s.attachments);
        }
        if (ImGui::Button("Add slot...")) ImGui::OpenPopup("##add_slot");
        if (ImGui::BeginPopup("##add_slot")) {
            ImGui::InputText("Name##slot", slot_name_, sizeof(slot_name_));
            ImGui::InputText("Texture", slot_texture_, sizeof(slot_texture_));
            ImGui::TextDisabled("guid:<uuid> of a texture, or empty for a solid quad.");
            ImGui::DragFloat2("Size", slot_size_, 0.01f, 0.01f, 100.0f);
            const u32 sel = ed.selected_bone();
            if (sel >= info.bones.size())
                ImGui::TextDisabled("Select the bone the slot rides first.");
            else
                ImGui::Text("Rides %s", info.bones[sel].name.c_str());
            ImGui::BeginDisabled(sel >= info.bones.size());
            if (ImGui::Button("Add") && slot_name_[0]) {
                skel2d::SlotSpec s;
                s.name = cook::String(StringView(slot_name_));
                s.bone = info.bones[sel].name;
                s.texture = cook::String(StringView(slot_texture_));
                s.size[0] = static_cast<f64>(slot_size_[0]);
                s.size[1] = static_cast<f64>(slot_size_[1]);
                if (ed.add_slot(s)) {
                    slot_name_[0] = 0;
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndDisabled();
            ImGui::EndPopup();
        }
    }

    // A preview target for each constraint: tick it and drag its point (in the skeleton's space).
    void draw_ik(Skeleton2DEditor& ed) {
        const Skeleton2DInfo& info = ed.info();
        ik_aim_.resize(info.ik.size());
        for (usize i = 0; i < info.ik.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            ImGui::Checkbox(info.ik[i].c_str(), &ik_aim_[i].on);
            if (ik_aim_[i].on) {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(160.0f);
                ImGui::DragFloat2("##aim", ik_aim_[i].pos, 0.01f);
            }
            ImGui::PopID();
        }
        if (info.ik.empty()) ImGui::TextDisabled("(none: skel2d_add_ik adds one over MCP)");
        Array<Skeleton2DIKTarget>& targets = ed.ik_targets();
        targets.clear();
        for (usize i = 0; i < info.ik.size(); ++i) {
            if (!ik_aim_[i].on) continue;
            Skeleton2DIKTarget t;
            t.constraint = info.ik[i];
            t.position = {ik_aim_[i].pos[0], ik_aim_[i].pos[1]};
            targets.push_back(static_cast<Skeleton2DIKTarget&&>(t));
        }
    }

    // ---- the selected bone ----

    bool field(const char* label, f64& v, f32 speed) {
        return ImGui::DragScalar(label, ImGuiDataType_Double, &v, speed);
    }

    void draw_bone_inspector(Skeleton2DEditor& ed) {
        const Skeleton2DInfo& info = ed.info();
        const u32 sel = ed.selected_bone();
        if (sel >= info.bones.size()) {
            ImGui::TextDisabled("Select a bone to edit its rest pose.");
            return;
        }
        const Skeleton2DInfo::Bone& b = info.bones[sel];
        ImGui::Text("%s", b.name.c_str());
        if (b.parent >= 0) {
            ImGui::SameLine();
            ImGui::TextDisabled("child of %s", info.bones[static_cast<usize>(b.parent)].name.c_str());
        }
        ImGui::SeparatorText("Rest pose");
        f64 x = b.x, y = b.y, rot = b.rotation, sx = b.scale_x, sy = b.scale_y;
        bool changed = false;
        changed |= field("X (m)", x, 0.01f);
        changed |= field("Y (m)", y, 0.01f);
        changed |= field("Rotation (deg)", rot, 0.5f);
        changed |= field("Scale X", sx, 0.01f);
        changed |= field("Scale Y", sy, 0.01f);
        if (changed) ed.set_bone_rest(sel, x, y, rot, sx, sy);

        const Span<const kit::Bone2DTransform> pose = ed.pose();
        ImGui::SeparatorText(ed.animation().empty() ? "World transform (rest)" : "World transform (previewed)");
        if (sel < pose.size()) {
            const kit::Bone2DTransform& m = pose[sel];
            ImGui::Text("Origin  %.4f, %.4f m", static_cast<f64>(m.x), static_cast<f64>(m.y));
            ImGui::Text("Rotation  %.2f deg", std::atan2(static_cast<f64>(m.b), static_cast<f64>(m.a)) * kRadToDeg);
            ImGui::Text("Axes  (%.3f, %.3f)  (%.3f, %.3f)", static_cast<f64>(m.a), static_cast<f64>(m.b),
                        static_cast<f64>(m.c), static_cast<f64>(m.d));
        }
        if (!ed.animation().empty()) draw_keying(ed, sel);
    }

    // Keys the selected bone's channels in the previewed animation at the scrubbed time.
    void draw_keying(Skeleton2DEditor& ed, u32 sel) {
        ImGui::SeparatorText("Key at the current time");
        ImGui::DragFloat("Rotate (deg)", &key_rot_, 0.5f);
        ImGui::SameLine();
        if (ImGui::Button("Key##rot")) key(ed, sel, skel2d::Channel::Rotate);
        ImGui::DragFloat2("Move (m)", key_move_, 0.01f);
        ImGui::SameLine();
        if (ImGui::Button("Key##move")) key(ed, sel, skel2d::Channel::Translate);
        ImGui::DragFloat2("Scale", key_scale_, 0.01f);
        ImGui::SameLine();
        if (ImGui::Button("Key##scale")) key(ed, sel, skel2d::Channel::Scale);
        ImGui::TextDisabled("Offsets from the rest pose, as the animation stores them.");
    }

    void key(Skeleton2DEditor& ed, u32 sel, skel2d::Channel channel) {
        skel2d::KeySpec k;
        k.animation = cook::String(ed.animation());
        k.channel = channel;
        k.target = ed.info().bones[sel].name;
        k.t = static_cast<f64>(ed.time());
        k.value = static_cast<f64>(key_rot_);
        if (channel == skel2d::Channel::Translate) {
            k.x = static_cast<f64>(key_move_[0]);
            k.y = static_cast<f64>(key_move_[1]);
        } else {
            k.x = static_cast<f64>(key_scale_[0]);
            k.y = static_cast<f64>(key_scale_[1]);
        }
        ed.key(k);
    }

    struct Aim {
        bool on = false;
        f32 pos[2] = {0.0f, 0.0f};
    };

    f64 synced_ = 0.0;
    bool playing_ = false;
    char new_path_[200] = "rigs/new.mcskel2d";
    char new_error_[512] = {};
    char anim_name_[32] = "idle";
    char bone_name_[32] = {};
    f32 bone_length_ = 0.5f;
    char slot_name_[32] = {};
    char slot_texture_[96] = {};
    f32 slot_size_[2] = {0.5f, 0.5f};
    f32 key_rot_ = 0.0f;
    f32 key_move_[2] = {0.0f, 0.0f};
    f32 key_scale_[2] = {1.0f, 1.0f};
    Array<Aim> ik_aim_;
};

}  // namespace

void add_skeleton_panel(EditorShell& shell) { shell.add_panel(mem::make_unique<SkeletonPanel>(mem::heap())); }

}  // namespace mce::editor
