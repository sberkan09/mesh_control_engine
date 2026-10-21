// The Animation panel (ROADMAP M-CINEMA step 3): the open .mcanim state graph drawn as boxes and
// arrows (drag a state to move it, right-click for transitions, the start state and removal), an
// inspector for parameters, the selected state (clip or 1D blend, loop, speed, events) and the
// selected transition (conditions, fade, on finish), and a preview of a scene entity with an
// Animator: its mesh's clips posed at a scrubbed time, or its graph running with parameter values
// set here. Edits go through AnimGraphEditor (animgraph_editor.h): read back by the cook before
// they are kept, saved canonically once nothing is held and hot-reloaded. Previewing is play mode,
// paused, so the scene document never changes.

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "mce/editor/animgraph_editor.h"
#include "mce/editor/editor_shell.h"
#include "mce/kit/kit3d.h"
#include "mce/log.h"
#include "scene_panels.h"

namespace mce::editor {
namespace {

using cook::AnimGraphDocState;
using cook::AnimGraphDocTransition;

constexpr f32 kBoxW = 150.0f;
constexpr f32 kBoxH = 48.0f;
const ImU32 kBoxCol = IM_COL32(52, 56, 66, 255);
const ImU32 kBoxBorder = IM_COL32(120, 125, 140, 255);
const ImU32 kBoxSel = IM_COL32(255, 160, 70, 255);
const ImU32 kBoxLive = IM_COL32(90, 200, 120, 255);
const ImU32 kStartCol = IM_COL32(110, 170, 255, 255);
const ImU32 kArrowCol = IM_COL32(200, 200, 210, 220);
const ImU32 kArrowSel = IM_COL32(255, 160, 70, 255);
const ImU32 kAnyCol = IM_COL32(150, 110, 200, 255);

void copy_text(char* dst, usize cap, StringView s) {
    const usize n = std::min(cap - 1, s.size());
    std::memcpy(dst, s.data(), n);
    dst[n] = 0;
}

ImVec2 add(ImVec2 a, ImVec2 b) { return ImVec2(a.x + b.x, a.y + b.y); }
ImVec2 sub(ImVec2 a, ImVec2 b) { return ImVec2(a.x - b.x, a.y - b.y); }
f32 len(ImVec2 v) { return std::sqrt(v.x * v.x + v.y * v.y); }

// Where the segment from the centre of a box toward `to` leaves the box.
ImVec2 box_edge(ImVec2 center, ImVec2 to, f32 w, f32 h) {
    const ImVec2 d = sub(to, center);
    if (std::fabs(d.x) < 1e-3f && std::fabs(d.y) < 1e-3f) return center;
    const f32 sx = std::fabs(d.x) > 1e-3f ? (w * 0.5f) / std::fabs(d.x) : 1e9f;
    const f32 sy = std::fabs(d.y) > 1e-3f ? (h * 0.5f) / std::fabs(d.y) : 1e9f;
    const f32 s = std::min(sx, sy);
    return ImVec2(center.x + d.x * s, center.y + d.y * s);
}

f32 segment_distance(ImVec2 p, ImVec2 a, ImVec2 b) {
    const ImVec2 ab = sub(b, a), ap = sub(p, a);
    const f32 l2 = ab.x * ab.x + ab.y * ab.y;
    const f32 t = l2 > 0.0f ? std::clamp((ap.x * ab.x + ap.y * ab.y) / l2, 0.0f, 1.0f) : 0.0f;
    return len(sub(p, ImVec2(a.x + ab.x * t, a.y + ab.y * t)));
}

class AnimationPanel final : public IEditorPanel {
public:
    AnimationPanel() { open = false; }  // the View menu, or a double click on an .mcanim in Assets

    const char* name() const override { return kAnimationPanel; }

    void draw(EditorShell& shell) override {
        EditorSession& session = shell.session();
        AnimGraphEditor& ed = shell.anim_graph();
        if (!session.project().is_open()) {
            ImGui::TextDisabled("Open a project to edit its animation graphs.");
            return;
        }
        if (ed.dirty() && !ImGui::IsAnyItemActive() && drag_state_ == kNoState) save(shell);
        const f64 now = ImGui::GetTime();
        if (now - synced_ > 0.5) {
            synced_ = now;
            ed.sync(session);
        }
        draw_header(shell, ed);
        if (!ed.last_error().empty())
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%.*s", static_cast<int>(ed.last_error().size()),
                               ed.last_error().data());

        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const f32 side = std::max(260.0f, avail.x * 0.3f);
        if (ImGui::BeginChild("##graph", ImVec2(avail.x - side - ImGui::GetStyle().ItemSpacing.x, 0), ImGuiChildFlags_Borders,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
            if (ed.is_open())
                draw_graph(shell, ed);
            else
                ImGui::TextDisabled("Double-click an .mcanim in Assets, or make a new one.");
        }
        ImGui::EndChild();
        ImGui::SameLine();
        if (ImGui::BeginChild("##side")) {
            draw_preview(shell, ed);
            if (ed.is_open()) draw_inspector(shell, ed);
        }
        ImGui::EndChild();
    }

    void shutdown(EditorShell& shell) override {
        preview_.stop(shell.session());
        if (shell.anim_graph().dirty()) save(shell);
    }

private:
    void save(EditorShell& shell) {
        if (Result<void> r = shell.anim_graph().save(shell.session()); !r) {
            FixedString<512> msg;
            format_error(r.error(), msg);
            MCE_LOG_ERROR("editor: saving the animation graph failed: {}", msg.view());
        }
    }

    // ---- header ----

    void draw_header(EditorShell& shell, AnimGraphEditor& ed) {
        EditorSession& session = shell.session();
        const FixedString<200> label(ed.is_open() ? ed.path() : StringView("(choose a graph)"));
        ImGui::SetNextItemWidth(std::max(160.0f, ImGui::GetContentRegionAvail().x * 0.3f));
        if (ImGui::BeginCombo("##graph_pick", label.c_str())) {
            for (const ProjectAsset& a : session.project().assets()) {
                if (a.type != asset::ChunkType::AnimGraph) continue;
                if (ImGui::Selectable(a.path.c_str(), a.guid == ed.guid()) && a.guid != ed.guid()) shell.open_anim_graph(a.guid);
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("New...")) ImGui::OpenPopup("##new");
        if (ImGui::BeginPopup("##new")) {
            ImGui::InputText("File", new_path_, sizeof(new_path_));
            ImGui::InputText("First clip", new_clip_, sizeof(new_clip_));
            if (ImGui::Button("Create")) {
                if (ed.dirty()) save(shell);
                if (Result<asset::Guid> r = ed.create(session, StringView(new_path_), StringView(new_clip_)); r) {
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
        if (ImGui::Button("+ State")) {
            const ImVec2 at = ImVec2(-pan_.x + 60.0f, -pan_.y + 60.0f);
            ed.add_state("state", first_clip(), at.x, at.y);
        }
        if (link_from_ != kNoState) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f), "Click the state the transition goes to (Esc cancels)");
        }
    }

    StringView first_clip() const { return clips_.empty() ? StringView("Idle") : clips_[0].name.view(); }

    // ---- the graph ----

    ImVec2 state_min(const AnimGraphEditor& ed, u32 i, ImVec2 origin) const {
        f32 x, y;
        ed.position(i, x, y);
        if (drag_state_ == i) x += drag_delta_.x, y += drag_delta_.y;
        return ImVec2(origin.x + pan_.x + x, origin.y + pan_.y + y);
    }
    ImVec2 any_min(ImVec2 origin) const { return ImVec2(origin.x + pan_.x - kBoxW * 0.5f - 40.0f, origin.y + pan_.y + 40.0f); }

    void arrow(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, f32 thick) const {
        dl->AddLine(a, b, col, thick);
        const ImVec2 d = sub(b, a);
        const f32 l = len(d);
        if (l < 1.0f) return;
        const ImVec2 u(d.x / l, d.y / l), n(-u.y, u.x);
        const ImVec2 m = ImVec2(a.x + d.x * 0.55f, a.y + d.y * 0.55f);
        dl->AddTriangleFilled(add(m, ImVec2(u.x * 7.0f, u.y * 7.0f)), add(m, ImVec2(-u.x * 5.0f + n.x * 5.0f, -u.y * 5.0f + n.y * 5.0f)),
                              add(m, ImVec2(-u.x * 5.0f - n.x * 5.0f, -u.y * 5.0f - n.y * 5.0f)), col);
    }

    void draw_graph(EditorShell& shell, AnimGraphEditor& ed) {
        const cook::AnimGraphDocument& doc = ed.doc();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const ImVec2 size = ImGui::GetContentRegionAvail();
        ImGui::InvisibleButton("##canvas", ImVec2(std::max(size.x, 1.0f), std::max(size.y, 1.0f)),
                               ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
        const bool hovered = ImGui::IsItemHovered();
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        dl->PushClipRect(origin, add(origin, size), true);
        for (f32 gx = std::fmod(pan_.x, 40.0f); gx < size.x; gx += 40.0f)
            dl->AddLine(ImVec2(origin.x + gx, origin.y), ImVec2(origin.x + gx, origin.y + size.y), IM_COL32(255, 255, 255, 12));
        for (f32 gy = std::fmod(pan_.y, 40.0f); gy < size.y; gy += 40.0f)
            dl->AddLine(ImVec2(origin.x, origin.y + gy), ImVec2(origin.x + size.x, origin.y + gy), IM_COL32(255, 255, 255, 12));

        const ImVec2 box(kBoxW, kBoxH);
        const StringView live = preview_.state(shell.session());
        bool any_used = false;
        for (const AnimGraphDocTransition& t : doc.transitions) any_used |= t.from == cook::kAnimGraphAny;

        // Transitions first, under the boxes. A pair in both directions is drawn side by side.
        u32 hit_transition = kNoTransition;
        for (u32 i = 0; i < doc.transitions.size(); ++i) {
            const AnimGraphDocTransition& t = doc.transitions[i];
            const ImVec2 bmin = state_min(ed, t.to, origin);
            const ImVec2 amin = t.from == cook::kAnimGraphAny ? any_min(origin) : state_min(ed, t.from, origin);
            const ImVec2 ac = add(amin, ImVec2(kBoxW * 0.5f, kBoxH * 0.5f)), bc = add(bmin, ImVec2(kBoxW * 0.5f, kBoxH * 0.5f));
            ImVec2 d = sub(bc, ac);
            const f32 l = std::max(len(d), 1.0f);
            const ImVec2 n(-d.y / l * 6.0f, d.x / l * 6.0f);
            const ImVec2 a = add(box_edge(ac, bc, kBoxW, kBoxH), n), b = add(box_edge(bc, ac, kBoxW, kBoxH), n);
            const bool sel = ed.selected_transition() == i;
            arrow(dl, a, b, sel ? kArrowSel : t.from == cook::kAnimGraphAny ? kAnyCol : kArrowCol, sel ? 2.5f : 1.5f);
            if (hovered && segment_distance(mouse, a, b) < 5.0f) hit_transition = i;
        }
        if (any_used) {
            const ImVec2 m = any_min(origin);
            dl->AddRectFilled(m, add(m, box), IM_COL32(60, 48, 78, 255), 6.0f);
            dl->AddRect(m, add(m, box), kAnyCol, 6.0f);
            dl->AddText(add(m, ImVec2(10.0f, 15.0f)), IM_COL32(220, 210, 240, 255), "any state");
        }

        // States.
        u32 hit_state = kNoState;
        for (u32 i = 0; i < doc.states.size(); ++i) {
            const AnimGraphDocState& s = doc.states[i];
            const ImVec2 m = state_min(ed, i, origin);
            const bool sel = ed.selected_state() == i;
            const bool is_live = !live.empty() && live == s.name.view();
            dl->AddRectFilled(m, add(m, box), kBoxCol, 6.0f);
            dl->AddRect(m, add(m, box), sel ? kBoxSel : is_live ? kBoxLive : kBoxBorder, 6.0f, sel || is_live ? 2.5f : 1.0f);
            if (doc.start == i) dl->AddRectFilled(m, ImVec2(m.x + 5.0f, m.y + kBoxH), kStartCol, 6.0f, ImDrawFlags_RoundCornersLeft);
            dl->AddText(add(m, ImVec2(10.0f, 6.0f)), IM_COL32(240, 240, 240, 255), s.name.c_str());
            char sub_line[96];
            if (s.blend_2d)
                std::snprintf(sub_line, sizeof(sub_line), "2D blend by %s, %s (%u)",
                              s.blend_param < doc.params.size() ? doc.params[s.blend_param].c_str() : "?",
                              s.blend_param_y < doc.params.size() ? doc.params[s.blend_param_y].c_str() : "?",
                              static_cast<u32>(s.points2d.size()));
            else if (s.is_blend())
                std::snprintf(sub_line, sizeof(sub_line), "blend by %s (%u)",
                              s.blend_param < doc.params.size() ? doc.params[s.blend_param].c_str() : "?", static_cast<u32>(s.points.size()));
            else
                std::snprintf(sub_line, sizeof(sub_line), "%s%s", s.clip.c_str(), s.loop ? "" : "  (once)");
            dl->AddText(add(m, ImVec2(10.0f, 26.0f)), IM_COL32(170, 175, 190, 255), sub_line);
            if (hovered && mouse.x >= m.x && mouse.x <= m.x + kBoxW && mouse.y >= m.y && mouse.y <= m.y + kBoxH) hit_state = i;
        }
        dl->PopClipRect();

        // Mouse: left selects, drags states, finishes a link; middle (or left on empty) pans; right menus.
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) link_from_ = kNoState;
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if (link_from_ != kNoState && hit_state != kNoState) {
                ed.add_transition(link_from_, hit_state);
                link_from_ = kNoState;
            } else if (hit_state != kNoState) {
                ed.select_state(hit_state);
                drag_state_ = hit_state;
                drag_delta_ = ImVec2(0, 0);
            } else if (hit_transition != kNoTransition) {
                ed.select_transition(hit_transition);
            } else {
                panning_ = true;
                link_from_ = kNoState;
            }
        }
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) panning_ = true;
        if (panning_) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseDown(ImGuiMouseButton_Middle))
                pan_ = add(pan_, ImGui::GetIO().MouseDelta);
            else
                panning_ = false;
        }
        if (drag_state_ != kNoState) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                drag_delta_ = add(drag_delta_, ImGui::GetIO().MouseDelta);
            } else {
                if (len(drag_delta_) > 1.0f) {
                    f32 x, y;
                    ed.position(drag_state_, x, y);
                    ed.move_state(drag_state_, x + drag_delta_.x, y + drag_delta_.y);
                }
                drag_state_ = kNoState;
                drag_delta_ = ImVec2(0, 0);
            }
        }
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            menu_state_ = hit_state;
            menu_at_ = sub(sub(mouse, origin), pan_);
            ImGui::OpenPopup("##graph_menu");
        }
        if (ImGui::BeginPopup("##graph_menu")) {
            if (menu_state_ != kNoState && menu_state_ < doc.states.size()) {
                ImGui::TextDisabled("%s", doc.states[menu_state_].name.c_str());
                if (ImGui::MenuItem("Transition to...")) link_from_ = menu_state_;
                if (ImGui::MenuItem("Set as start", nullptr, false, doc.start != menu_state_)) ed.set_start(menu_state_);
                if (ImGui::MenuItem("Preview its clip", nullptr, false, !doc.states[menu_state_].is_blend()))
                    copy_text(clip_, sizeof(clip_), doc.states[menu_state_].clip.view());
                if (ImGui::MenuItem("Remove state")) ed.remove_state(menu_state_);
            } else {
                if (ImGui::MenuItem("Add state here")) ed.add_state("state", first_clip(), menu_at_.x, menu_at_.y);
                if (ImGui::BeginMenu("Any state to", !doc.states.empty())) {
                    for (u32 i = 0; i < doc.states.size(); ++i)
                        if (ImGui::MenuItem(doc.states[i].name.c_str())) ed.add_transition(cook::kAnimGraphAny, i);
                    ImGui::EndMenu();
                }
            }
            ImGui::EndPopup();
        }
        if (ImGui::IsWindowFocused() && (ImGui::IsKeyPressed(ImGuiKey_Delete))) {
            if (ed.selected_transition() != kNoTransition)
                ed.remove_transition(ed.selected_transition());
            else if (ed.selected_state() != kNoState)
                ed.remove_state(ed.selected_state());
        }
    }

    // ---- preview ----

    void pick_entity(EditorShell& shell, AnimGraphEditor& ed) {
        // Default: the selected entity when it has an Animator, else the first that plays this graph.
        const EditorScene& scene = shell.scene();
        auto has_animator = [&](EntityId id) {
            const DocEntity* e = scene.doc().find(id);
            return e && e->find(refl::type_of<kit::Animator>());
        };
        if (entity_ != kNoEntity && has_animator(entity_)) return;
        entity_ = kNoEntity;
        for (EntityId id : scene.selection().items())
            if (has_animator(id)) {
                entity_ = id;
                return;
            }
        Array<EntityId> users;
        ed.users(scene, users);
        if (!users.empty()) entity_ = users[0];
    }

    void refresh_clips(EditorShell& shell) {
        const DocEntity* e = entity_ == kNoEntity ? nullptr : shell.scene().doc().find(entity_);
        const DocComponent* c = e ? e->find(refl::type_of<kit::MeshRef>()) : nullptr;
        const asset::Guid mesh = c ? static_cast<const kit::MeshRef*>(c->data())->mesh.guid : asset::Guid{};
        const u64 gen = shell.session().project().generation();
        if (mesh == clips_mesh_ && gen == clips_gen_) return;
        clips_mesh_ = mesh;
        clips_gen_ = gen;
        mesh_clips(shell.session(), mesh, clips_);
    }

    void draw_preview(EditorShell& shell, AnimGraphEditor& ed) {
        EditorSession& session = shell.session();
        ImGui::SeparatorText("Preview");
        pick_entity(shell, ed);
        refresh_clips(shell);
        const EditorScene& scene = shell.scene();
        const DocEntity* cur = entity_ == kNoEntity ? nullptr : scene.doc().find(entity_);
        ImGui::BeginDisabled(preview_.running(session));
        if (ImGui::BeginCombo("Entity", cur ? cur->name.c_str() : "(an entity with an Animator)")) {
            for (const DocEntity& e : scene.doc().entities())
                if (e.find(refl::type_of<kit::Animator>()) && ImGui::Selectable(e.name.c_str(), e.id == entity_)) entity_ = e.id;
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();

        bool running = preview_.running(session);
        if (ImGui::Checkbox("Preview in the viewport", &running)) {
            if (running) {
                if (preview_.start(session, entity_)) {
                    shell.focus_panel("Viewport");
                    if (clip_[0]) preview_.show_clip(session, StringView(clip_), clip_time_);
                }
            } else {
                preview_.stop(session);
                playing_clip_ = false;
            }
        }
        if (!preview_.why().empty())
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f), "%.*s", static_cast<int>(preview_.why().size()), preview_.why().data());
        running = preview_.running(session);

        // A clip of the entity's mesh at a scrubbed time.
        f32 duration = 0.0f;
        for (const MeshClipInfo& c : clips_)
            if (c.name.view() == StringView(clip_)) duration = c.duration;
        if (ImGui::BeginCombo("Clip", clip_[0] ? clip_ : "(a clip of the mesh)")) {
            for (const MeshClipInfo& c : clips_) {
                char lbl[96];
                std::snprintf(lbl, sizeof(lbl), "%s  (%.2f s)", c.name.c_str(), static_cast<double>(c.duration));
                if (ImGui::Selectable(lbl, c.name.view() == StringView(clip_))) {
                    copy_text(clip_, sizeof(clip_), c.name.view());
                    clip_time_ = 0.0f;
                    if (running) preview_.show_clip(session, StringView(clip_), clip_time_);
                }
            }
            ImGui::EndCombo();
        }
        if (clips_.empty() && entity_ != kNoEntity) ImGui::TextDisabled("The entity's mesh has no clips.");
        ImGui::BeginDisabled(!running || !clip_[0]);
        if (ImGui::Button(playing_clip_ ? "Pause##clip" : "Play##clip")) playing_clip_ = !playing_clip_;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::SliderFloat("##clip_time", &clip_time_, 0.0f, std::max(duration, 0.01f), "%.3f s")) {
            playing_clip_ = false;
            preview_.show_clip(session, StringView(clip_), clip_time_);
        }
        ImGui::EndDisabled();
        if (playing_clip_ && running && clip_[0]) {
            clip_time_ += ImGui::GetIO().DeltaTime;
            if (duration > 0.0f) clip_time_ = std::fmod(clip_time_, duration);
            preview_.show_clip(session, StringView(clip_), clip_time_);
        }

        // The graph running with these parameter values.
        if (ed.is_open() && !ed.doc().params.empty()) {
            ImGui::TextDisabled("Parameters");
            bool changed = false;
            for (u32 i = 0; i < ed.doc().params.size(); ++i) changed |= ImGui::DragFloat(ed.doc().params[i].c_str(), &params_[i], 0.05f);
            if (changed && graph_running_) preview_.run_graph(session, params_);
        }
        ImGui::BeginDisabled(!running);
        if (ImGui::Button("Run the graph")) {
            playing_clip_ = false;
            graph_running_ = preview_.run_graph(session, params_);
        }
        ImGui::EndDisabled();
        if (running) {
            const StringView st = preview_.state(session);
            ImGui::SameLine();
            ImGui::TextDisabled("state: %.*s", static_cast<int>(st.size()), st.data());
        } else {
            graph_running_ = false;
        }
    }

    // ---- inspector ----

    // A clip name: a pick from the mesh's clips, or typed.
    bool clip_field(const char* label, char* buf, usize cap) {
        bool commit = false;
        ImGui::PushID(label);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
        ImGui::InputText(label, buf, cap);
        commit |= ImGui::IsItemDeactivatedAfterEdit();
        if (!clips_.empty()) {
            ImGui::SameLine();
            if (ImGui::BeginCombo("##pick", "", ImGuiComboFlags_NoPreview)) {
                for (const MeshClipInfo& c : clips_)
                    if (ImGui::Selectable(c.name.c_str())) {
                        copy_text(buf, cap, c.name.view());
                        commit = true;
                    }
                ImGui::EndCombo();
            }
        }
        ImGui::PopID();
        return commit;
    }

    bool param_combo(const char* label, const cook::AnimGraphDocument& doc, u8& param) {
        bool changed = false;
        if (ImGui::BeginCombo(label, param < doc.params.size() ? doc.params[param].c_str() : "?")) {
            for (u32 i = 0; i < doc.params.size(); ++i)
                if (ImGui::Selectable(doc.params[i].c_str(), i == param)) {
                    param = static_cast<u8>(i);
                    changed = true;
                }
            ImGui::EndCombo();
        }
        return changed;
    }

    void draw_params(AnimGraphEditor& ed) {
        const cook::AnimGraphDocument& doc = ed.doc();
        ImGui::SeparatorText("Graph");
        if (name_rev_ != ed.revision()) {
            name_rev_ = ed.revision();
            copy_text(graph_name_, sizeof(graph_name_), doc.name.view());
            for (u32 i = 0; i < asset::kAnimGraphMaxParams; ++i)
                copy_text(param_names_[i], sizeof(param_names_[i]), i < doc.params.size() ? doc.params[i].view() : StringView());
        }
        ImGui::InputText("Name", graph_name_, sizeof(graph_name_));
        if (ImGui::IsItemDeactivatedAfterEdit()) ed.set_name(StringView(graph_name_));
        for (u32 i = 0; i < doc.params.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 30.0f);
            ImGui::InputText("##param", param_names_[i], sizeof(param_names_[i]));
            if (ImGui::IsItemDeactivatedAfterEdit()) ed.rename_param(i, StringView(param_names_[i]));
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) ed.remove_param(i);
            ImGui::PopID();
        }
        ImGui::BeginDisabled(doc.params.size() >= asset::kAnimGraphMaxParams);
        if (!doc.layers.empty() || !doc.ik.empty() || !doc.root_motion.empty())
            ImGui::TextDisabled("%u layer(s), %u IK chain(s)%s%s (edited in the .mcanim file)", static_cast<u32>(doc.layers.size()),
                                static_cast<u32>(doc.ik.size()), doc.root_motion.empty() ? "" : ", root motion from ",
                                doc.root_motion.c_str());
        if (ImGui::SmallButton("+ Parameter")) {
            char n[24];
            std::snprintf(n, sizeof(n), "param%u", static_cast<u32>(doc.params.size()) + 1);
            ed.add_param(StringView(n));
        }
        ImGui::EndDisabled();
    }

    void draw_state(AnimGraphEditor& ed, u32 si) {
        const cook::AnimGraphDocument& doc = ed.doc();
        if (state_rev_ != ed.revision() || state_of_ != si) {
            state_rev_ = ed.revision();
            state_of_ = si;
            state_ = doc.states[si];
            copy_text(state_name_, sizeof(state_name_), state_.name.view());
            copy_text(state_clip_, sizeof(state_clip_), state_.clip.view());
            point_clips_.clear();
            for (const cook::AnimGraphDocPoint& p : state_.points) {
                ClipBuf b;
                copy_text(b.text, sizeof(b.text), p.clip.view());
                point_clips_.push_back(b);
            }
            event_names_.clear();
            for (const cook::AnimGraphDocEvent& e : state_.events) {
                ClipBuf b;
                copy_text(b.text, sizeof(b.text), e.name.view());
                event_names_.push_back(b);
            }
        }
        ImGui::SeparatorText("State");
        bool commit = false;
        ImGui::InputText("Name##state", state_name_, sizeof(state_name_));
        commit |= ImGui::IsItemDeactivatedAfterEdit();
        bool blend = state_.is_blend();
        if (state_.blend_2d) {
            ImGui::TextDisabled("A 2D blend space over %s and %s, %u points:",
                                state_.blend_param < doc.params.size() ? doc.params[state_.blend_param].c_str() : "?",
                                state_.blend_param_y < doc.params.size() ? doc.params[state_.blend_param_y].c_str() : "?",
                                static_cast<u32>(state_.points2d.size()));
            for (const cook::AnimGraphDocPoint2D& p : state_.points2d)
                ImGui::TextDisabled("  %s at (%.2f, %.2f)", p.clip.c_str(), static_cast<double>(p.x), static_cast<double>(p.y));
            ImGui::TextDisabled("Its points are edited in the .mcanim file.");
        } else if (ImGui::Checkbox("Blend by a parameter", &blend)) {
            if (blend && doc.params.empty()) {
                ed.add_param("speed");
            } else if (blend) {
                state_.clip = cook::String();
                state_clip_[0] = 0;
                state_.blend_param = 0;
                state_.points.clear();
                point_clips_.clear();
                for (u32 i = 0; i < 2; ++i) {
                    cook::AnimGraphDocPoint p;
                    p.clip.assign(i < clips_.size() ? clips_[i].name.view() : StringView(i ? "Walk" : "Idle"));
                    p.at = static_cast<f32>(i);
                    state_.points.push_back(p);
                    ClipBuf b;
                    copy_text(b.text, sizeof(b.text), p.clip.view());
                    point_clips_.push_back(b);
                }
                commit = true;
            } else {
                copy_text(state_clip_, sizeof(state_clip_), state_.points.empty() ? first_clip() : state_.points[0].clip.view());
                state_.points.clear();
                point_clips_.clear();
                commit = true;
            }
        }
        if (state_.blend_2d) {
        } else if (!blend) {
            commit |= clip_field("Clip", state_clip_, sizeof(state_clip_));
        } else {
            commit |= param_combo("Parameter", doc, state_.blend_param);
            for (u32 i = 0; i < state_.points.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                commit |= clip_field("##pt_clip", point_clips_[i].text, sizeof(point_clips_[i].text));
                ImGui::SameLine();
                ImGui::SetNextItemWidth(60.0f);
                ImGui::DragFloat("##at", &state_.points[i].at, 0.05f);
                commit |= ImGui::IsItemDeactivatedAfterEdit();
                ImGui::SameLine();
                if (state_.points.size() > 2 && ImGui::SmallButton("x")) {
                    state_.points.erase(i);
                    point_clips_.erase(i);
                    commit = true;
                    ImGui::PopID();
                    break;
                }
                ImGui::PopID();
            }
            if (ImGui::SmallButton("+ Blend point")) {
                cook::AnimGraphDocPoint p;
                p.clip.assign(first_clip());
                p.at = state_.points.empty() ? 0.0f : state_.points.back().at + 1.0f;
                state_.points.push_back(p);
                ClipBuf b;
                copy_text(b.text, sizeof(b.text), p.clip.view());
                point_clips_.push_back(b);
                commit = true;
            }
        }
        commit |= ImGui::Checkbox("Loop", &state_.loop);
        ImGui::DragFloat("Speed", &state_.speed, 0.01f, 0.01f, 100.0f, "%.2f");
        commit |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::TextDisabled("Events (seconds into the clip)");
        for (u32 i = 0; i < state_.events.size(); ++i) {
            ImGui::PushID(1000 + static_cast<int>(i));
            ImGui::SetNextItemWidth(60.0f);
            ImGui::DragFloat("##time", &state_.events[i].time, 0.01f, 0.0f, 600.0f, "%.2f");
            commit |= ImGui::IsItemDeactivatedAfterEdit();
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 30.0f);
            ImGui::InputText("##name", event_names_[i].text, sizeof(event_names_[i].text));
            commit |= ImGui::IsItemDeactivatedAfterEdit();
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) {
                state_.events.erase(i);
                event_names_.erase(i);
                commit = true;
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
        if (ImGui::SmallButton("+ Event")) {
            cook::AnimGraphDocEvent e;
            e.name.assign("event");
            state_.events.push_back(e);
            ClipBuf b;
            copy_text(b.text, sizeof(b.text), StringView("event"));
            event_names_.push_back(b);
            commit = true;
        }
        if (commit) {
            state_.name.assign(StringView(state_name_));
            if (!state_.points.empty()) {
                state_.clip = cook::String();
                for (u32 i = 0; i < state_.points.size(); ++i) state_.points[i].clip.assign(StringView(point_clips_[i].text));
            } else {
                state_.clip.assign(StringView(state_clip_));
            }
            for (u32 i = 0; i < state_.events.size(); ++i) state_.events[i].name.assign(StringView(event_names_[i].text));
            if (!ed.set_state(si, state_)) state_rev_ = ~u64(0);
        }
        if (ImGui::Button("Transition to...")) link_from_ = si;
        ImGui::SameLine();
        if (ImGui::Button("Remove state")) ed.remove_state(si);
    }

    bool state_combo(const char* label, const cook::AnimGraphDocument& doc, u32& state, bool any_ok) {
        bool changed = false;
        const char* cur = state == cook::kAnimGraphAny ? "any state" : state < doc.states.size() ? doc.states[state].name.c_str() : "?";
        if (ImGui::BeginCombo(label, cur)) {
            if (any_ok && ImGui::Selectable("any state", state == cook::kAnimGraphAny)) {
                state = cook::kAnimGraphAny;
                changed = true;
            }
            for (u32 i = 0; i < doc.states.size(); ++i)
                if (ImGui::Selectable(doc.states[i].name.c_str(), i == state)) {
                    state = i;
                    changed = true;
                }
            ImGui::EndCombo();
        }
        return changed;
    }

    void draw_transition(AnimGraphEditor& ed, u32 ti) {
        const cook::AnimGraphDocument& doc = ed.doc();
        AnimGraphDocTransition t = doc.transitions[ti];
        ImGui::SeparatorText("Transition");
        bool commit = false;
        commit |= state_combo("From", doc, t.from, true);
        commit |= state_combo("To", doc, t.to, false);
        ImGui::DragFloat("Fade (s)", &t.fade, 0.01f, 0.0f, 10.0f, "%.2f");
        commit |= ImGui::IsItemDeactivatedAfterEdit();
        commit |= ImGui::Checkbox("When the one-shot ends", &t.on_finish);
        ImGui::TextDisabled("When (all hold)");
        for (u32 i = 0; i < t.when.size(); ++i) {
            cook::AnimGraphDocCondition& c = t.when[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::SetNextItemWidth(90.0f);
            commit |= param_combo("##param", doc, c.param);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(50.0f);
            if (ImGui::BeginCombo("##op", cook::anim_compare_name(c.op))) {
                for (u8 k = 0; k <= static_cast<u8>(asset::AnimCompare::NotEqual); ++k) {
                    const auto op = static_cast<asset::AnimCompare>(k);
                    if (ImGui::Selectable(cook::anim_compare_name(op), op == c.op)) {
                        c.op = op;
                        commit = true;
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(60.0f);
            if (cond_rev_ != ed.revision() || cond_of_ != ti) cond_values_[i % 8] = c.value;
            ImGui::DragFloat("##value", &cond_values_[i % 8], 0.05f);
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                c.value = cond_values_[i % 8];
                commit = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) {
                t.when.erase(i);
                commit = true;
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
        cond_rev_ = ed.revision();
        cond_of_ = ti;
        ImGui::BeginDisabled(doc.params.empty() || t.when.size() >= 8);
        if (ImGui::SmallButton("+ Condition")) {
            cook::AnimGraphDocCondition c;
            c.value = 0.5f;
            t.when.push_back(c);
            commit = true;
        }
        ImGui::EndDisabled();
        if (commit) ed.set_transition(ti, t);
        if (ImGui::Button("Remove transition")) ed.remove_transition(ti);
    }

    void draw_inspector(EditorShell&, AnimGraphEditor& ed) {
        draw_params(ed);
        if (ed.selected_state() < ed.doc().states.size())
            draw_state(ed, ed.selected_state());
        else if (ed.selected_transition() < ed.doc().transitions.size())
            draw_transition(ed, ed.selected_transition());
        else
            ImGui::TextDisabled("Click a state or a transition to edit it.");
    }

    struct ClipBuf {
        char text[32] = {};
    };

    f64 synced_ = 0.0;
    ImVec2 pan_ = ImVec2(0, 0);
    bool panning_ = false;
    u32 drag_state_ = kNoState;
    ImVec2 drag_delta_ = ImVec2(0, 0);
    u32 link_from_ = kNoState;
    u32 menu_state_ = kNoState;
    ImVec2 menu_at_ = ImVec2(0, 0);
    // New graph.
    char new_path_[256] = "anim/hero.mcanim";
    char new_clip_[32] = "Idle";
    char new_error_[512] = {};
    // Preview.
    AnimPreview preview_;
    EntityId entity_ = kNoEntity;
    Array<MeshClipInfo> clips_;
    asset::Guid clips_mesh_;
    u64 clips_gen_ = ~u64(0);
    char clip_[32] = {};
    f32 clip_time_ = 0.0f;
    bool playing_clip_ = false;
    bool graph_running_ = false;
    f32 params_[4] = {};
    // Inspector copies.
    char graph_name_[64] = {};
    char param_names_[asset::kAnimGraphMaxParams][24] = {};
    u64 name_rev_ = ~u64(0);
    AnimGraphDocState state_;
    u64 state_rev_ = ~u64(0);
    u32 state_of_ = kNoState;
    char state_name_[32] = {};
    char state_clip_[32] = {};
    Array<ClipBuf> point_clips_;
    Array<ClipBuf> event_names_;
    f32 cond_values_[8] = {};
    u64 cond_rev_ = ~u64(0);
    u32 cond_of_ = kNoTransition;
};

}  // namespace

void add_animation_panel(EditorShell& shell) { shell.add_panel(mem::make_unique<AnimationPanel>(mem::heap())); }

}  // namespace mce::editor
