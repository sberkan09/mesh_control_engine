// The Timeline panel (ROADMAP M-CINEMA step 2, FR-EDT-130): the open .mctimeline's tracks and keys on
// a time ruler, a curve view of the selected transform or property track, the selected key's
// inspector, record mode and the viewport preview. Edits go through TimelineEditor
// (timeline_editor.h): each is read back by the cook before it is kept, saved canonically once
// nothing is held any more and hot-reloaded; a change made on disk meanwhile (an MCP tool) is read in.
// Scrubbing shows the scene in a paused play session (TimelinePreview), so the document never moves.

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "mce/editor/editor_shell.h"
#include "mce/editor/timeline_editor.h"
#include "mce/kit/timeline.h"
#include "mce/log.h"
#include "scene_panels.h"

namespace mce::editor {
namespace {

using cook::TimelineDocKey;
using cook::TimelineDocTrack;
using T = cook::TimelineDocTrackType;

constexpr f32 kRulerH = 22.0f;
constexpr f32 kRowH = 22.0f;
constexpr f32 kKeyR = 5.5f;
constexpr f32 kCurveH = 150.0f;

const ImU32 kKeyCol = IM_COL32(220, 200, 120, 255);
const ImU32 kKeySel = IM_COL32(255, 140, 60, 255);
const ImU32 kPlayheadCol = IM_COL32(255, 80, 70, 255);
const ImU32 kPlayCol = IM_COL32(110, 200, 255, 255);
const ImU32 kRangeCol = IM_COL32(110, 160, 230, 110);
const ImU32 kGridCol = IM_COL32(255, 255, 255, 28);
const ImU32 kChannelCol[4] = {IM_COL32(240, 90, 90, 255), IM_COL32(110, 220, 110, 255), IM_COL32(100, 150, 255, 255),
                              IM_COL32(200, 200, 200, 255)};

struct Snap {
    const char* name;
    f32 step;
};
constexpr Snap kSnaps[] = {{"Snap: off", 0.0f}, {"Snap: 1/30 s", 1.0f / 30.0f}, {"Snap: 0.1 s", 0.1f},
                           {"Snap: 0.25 s", 0.25f}, {"Snap: 0.5 s", 0.5f}, {"Snap: 1 s", 1.0f}};

bool uses_target(T t) { return t != T::Camera && t != T::Subtitle && t != T::Event; }
bool has_curve(const TimelineDocTrack& t) { return t.type == T::Property || t.type == T::Transform; }
bool has_range(T t) { return t == T::Activation || t == T::Subtitle || t == T::Video; }

void copy_text(char* dst, usize cap, StringView s) {
    const usize n = std::min(cap - 1, s.size());
    std::memcpy(dst, s.data(), n);
    dst[n] = 0;
}

class TimelinePanel final : public IEditorPanel {
public:
    TimelinePanel() { open = false; }  // the View menu, or a double click on a .mctimeline in Assets

    const char* name() const override { return kTimelinePanel; }

    void draw(EditorShell& shell) override {
        EditorSession& session = shell.session();
        TimelineEditor& ed = shell.timeline();
        TimelinePreview& preview = shell.timeline_preview();
        if (!session.project().is_open()) {
            ImGui::TextDisabled("Open a project to edit its timelines.");
            return;
        }
        // A finished edit (nothing held) goes to disk; a change on disk comes in.
        if (ed.dirty() && !ImGui::IsAnyItemActive() && drag_key_ == kNoKey && drag_curve_ == kNoKey) save(shell);
        const f64 now = ImGui::GetTime();
        if (now - synced_ > 0.5) {
            synced_ = now;
            ed.sync(session);
        }
        follow_play(session, ed, preview);
        if (preview.running(session)) preview.update(session, ed);

        draw_header(shell, ed);
        if (!ed.is_open()) {
            ImGui::TextDisabled("Double-click a .mctimeline in Assets, or make a new one.");
            return;
        }
        draw_toolbar(shell, ed, preview);
        if (!ed.last_error().empty())
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%.*s", static_cast<int>(ed.last_error().size()),
                               ed.last_error().data());
        if (!preview.why().empty())
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f), "%.*s", static_cast<int>(preview.why().size()),
                               preview.why().data());

        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const f32 inspector_w = std::max(240.0f, avail.x * 0.28f);
        if (ImGui::BeginChild("##tracks", ImVec2(avail.x - inspector_w - ImGui::GetStyle().ItemSpacing.x, 0))) {
            draw_tracks(shell, ed, preview);
            const TimelineDocTrack* sel = ed.selected_track() < ed.doc().tracks.size() ? &ed.doc().tracks[ed.selected_track()] : nullptr;
            if (sel && has_curve(*sel)) draw_curve(ed, *sel);
        }
        ImGui::EndChild();
        ImGui::SameLine();
        if (ImGui::BeginChild("##key")) draw_inspector(shell, ed);
        ImGui::EndChild();
    }

    void shutdown(EditorShell& shell) override {
        shell.timeline_preview().stop(shell.session());
        if (shell.timeline().dirty()) save(shell);
    }

private:
    void save(EditorShell& shell) {
        if (Result<void> r = shell.timeline().save(shell.session()); !r) {
            FixedString<512> msg;
            format_error(r.error(), msg);
            MCE_LOG_ERROR("editor: saving the timeline failed: {}", msg.view());
        }
    }

    f32 snap(f32 t) const { return snap_time(t, kSnaps[snap_].step); }

    // While the preview plays on, the play head shown is the player's; a pause hands it back.
    void follow_play(EditorSession& session, TimelineEditor& ed, TimelinePreview& preview) {
        play_time_ = -1.0f;
        if (!preview.running(session)) return;
        const World* w = session.play().world();
        const kit::TimelinePlayer* p = w->try_get<kit::TimelinePlayer>(preview.player());
        if (!p) return;
        if (session.play().state() == PlaySession::State::Playing) {
            play_time_ = p->time;
            was_playing_ = true;
        } else if (was_playing_) {
            was_playing_ = false;
            ed.set_playhead(p->time);
        }
    }

    // ---- header and toolbar ----

    void draw_header(EditorShell& shell, TimelineEditor& ed) {
        EditorSession& session = shell.session();
        const FixedString<200> label(ed.is_open() ? ed.path() : StringView("(choose a timeline)"));
        ImGui::SetNextItemWidth(std::max(160.0f, ImGui::GetContentRegionAvail().x * 0.35f));
        if (ImGui::BeginCombo("##timeline", label.c_str())) {
            for (const ProjectAsset& a : session.project().assets()) {
                if (a.type != asset::ChunkType::Timeline) continue;
                if (ImGui::Selectable(a.path.c_str(), a.guid == ed.guid()) && a.guid != ed.guid()) shell.open_timeline(a.guid);
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("New...")) ImGui::OpenPopup("##new");
        if (ImGui::BeginPopup("##new")) {
            ImGui::InputText("File", new_path_, sizeof(new_path_));
            ImGui::DragFloat("Duration (s)", &new_duration_, 0.1f, 0.1f, 600.0f, "%.2f");
            if (ImGui::Button("Create")) {
                if (ed.dirty()) save(shell);
                shell.timeline_preview().stop(session);
                if (Result<asset::Guid> r = ed.create(session, StringView(new_path_), new_duration_); r) {
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
        if (name_of_ != ed.revision()) {
            name_of_ = ed.revision();
            copy_text(name_, sizeof(name_), ed.doc().name.view());
        }
        ImGui::SetNextItemWidth(140.0f);
        ImGui::InputText("##name", name_, sizeof(name_));
        if (ImGui::IsItemDeactivatedAfterEdit()) ed.set_name(StringView(name_));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The timeline's name");
    }

    void draw_toolbar(EditorShell& shell, TimelineEditor& ed, TimelinePreview& preview) {
        EditorSession& session = shell.session();
        ImGui::BeginDisabled(!ed.can_undo());
        if (ImGui::Button("Undo")) ed.undo(session);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!ed.can_redo());
        if (ImGui::Button("Redo")) ed.redo(session);
        ImGui::EndDisabled();
        ImGui::SameLine();

        // Preview: the scene in a paused play session, scrubbed to the playhead.
        bool previewing = preview.running(session);
        if (ImGui::Checkbox("Preview", &previewing)) {
            if (previewing) {
                if (preview.start(session, ed)) shell.focus_panel("Viewport");
            } else {
                preview.stop(session);
            }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show the timeline at the playhead in the viewport (play mode, paused)");
        ImGui::SameLine();
        ImGui::BeginDisabled(!previewing);
        const bool playing = previewing && session.play().state() == PlaySession::State::Playing;
        if (ImGui::Button(playing ? "Pause" : "Play")) {
            if (playing) {
                session.play().pause();
            } else {
                if (ed.playhead() >= ed.doc().length()) ed.set_playhead(0.0f);
                preview.update(session, ed);
                session.play().resume();
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();

        // Record: inspector and gizmo edits key the open timeline at the playhead.
        bool rec = ed.recording();
        if (rec) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.3f, 1.0f));
        if (ImGui::Checkbox("Record", &rec)) ed.set_recording(rec);
        if (ed.recording()) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Key inspector and gizmo edits of the entities this timeline drives at the playhead\n"
                              "(an entity it does not drive yet takes a free binding of the TimelinePlayer)");
        if (ed.recording() && ed.player_entity(shell.scene()) == kNoEntity) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f), "(no TimelinePlayer in the scene plays this timeline)");
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0f);
        if (ImGui::BeginCombo("##snap", kSnaps[snap_].name)) {
            for (u32 i = 0; i < sizeof(kSnaps) / sizeof(kSnaps[0]); ++i)
                if (ImGui::Selectable(kSnaps[i].name, i == snap_)) snap_ = i;
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        f32 head = ed.playhead();
        ImGui::SetNextItemWidth(90.0f);
        if (ImGui::DragFloat("##head", &head, 0.01f, 0.0f, ed.doc().length(), "%.3f s")) ed.set_playhead(snap(head));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Playhead");
        ImGui::SameLine();
        if (duration_of_ != ed.revision()) {
            duration_of_ = ed.revision();
            duration_ = ed.doc().duration;
        }
        ImGui::SetNextItemWidth(90.0f);
        ImGui::DragFloat("##duration", &duration_, 0.05f, 0.0f, 3600.0f, duration_ > 0.0f ? "length %.2f s" : "length: last key");
        if (ImGui::IsItemDeactivatedAfterEdit() && !ed.set_duration(duration_)) duration_ = ed.doc().duration;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The timeline's length (0: the end of its last key)");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.0f);
        ImGui::SliderFloat("##zoom", &zoom_, 1.0f, 16.0f, "zoom %.1fx", ImGuiSliderFlags_Logarithmic);
    }

    // ---- tracks ----

    void target_label(const EditorScene& scene, EntityId player, u8 target, char* out, usize cap) const {
        if (target == asset::kTimelineSelf) {
            std::snprintf(out, cap, "self");
            return;
        }
        const EntityId id = player == kNoEntity ? kNoEntity : TimelineEditor::entity_of(scene, player, target);
        const DocEntity* e = id == kNoEntity ? nullptr : scene.doc().find(id);
        if (e)
            std::snprintf(out, cap, "%u: %s", target, e->name.c_str());
        else
            std::snprintf(out, cap, "binding %u", target);
    }

    void track_label(const EditorScene& scene, EntityId player, const TimelineDocTrack& t, char* out, usize cap) const {
        char who[96];
        target_label(scene, player, t.target, who, sizeof(who));
        const char* type = cook::timeline_track_type_name(t.type);
        if (!t.path.empty())
            std::snprintf(out, cap, "%s %s  (%s)", type, t.path.c_str(), who);
        else if (uses_target(t.type))
            std::snprintf(out, cap, "%s  (%s)", type, who);
        else
            std::snprintf(out, cap, "%s", type);
    }

    // A target picker: self or one of the eight bindings, with the bound entities' names.
    bool target_combo(const char* id, const EditorScene& scene, EntityId player, u8& target) const {
        char cur[96];
        target_label(scene, player, target, cur, sizeof(cur));
        bool changed = false;
        if (ImGui::BeginCombo(id, cur)) {
            for (u32 i = 0; i <= asset::kTimelineBindings; ++i) {
                const u8 t = i == asset::kTimelineBindings ? asset::kTimelineSelf : static_cast<u8>(i);
                char lbl[96];
                target_label(scene, player, t, lbl, sizeof(lbl));
                if (ImGui::Selectable(lbl, t == target)) {
                    target = t;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        return changed;
    }

    void draw_add_track(EditorShell& shell, TimelineEditor& ed, EntityId player) {
        if (ImGui::Button("+ Track")) ImGui::OpenPopup("##add_track");
        if (!ImGui::BeginPopup("##add_track")) return;
        const EditorScene& scene = shell.scene();
        if (ImGui::BeginCombo("Type", cook::timeline_track_type_name(add_type_))) {
            for (u32 i = 0; i < cook::kTimelineDocTrackTypes; ++i) {
                const T t = static_cast<T>(i);
                if (ImGui::Selectable(cook::timeline_track_type_name(t), t == add_type_)) add_type_ = t;
            }
            ImGui::EndCombo();
        }
        if (uses_target(add_type_)) target_combo("Target", scene, player, add_target_);
        if (add_type_ == T::Property || add_type_ == T::Activation) {
            ImGui::InputTextWithHint("Field", add_type_ == T::Property ? "Camera3DComponent.fov_deg" : "MeshRef.visible",
                                     add_path_, sizeof(add_path_));
        }
        if (add_type_ == T::Audio || add_type_ == T::Video) {
            const bool video = add_type_ == T::Video;
            const asset::ChunkType want = video ? asset::ChunkType::Video : asset::ChunkType::Sfx;
            const ProjectAsset* cur = shell.session().project().find(add_sound_);
            if (cur && cur->type != want) cur = nullptr;
            if (ImGui::BeginCombo(video ? "Video" : "Sound", cur ? cur->path.c_str() : (video ? "(choose a video)" : "(choose a sound)"))) {
                for (const ProjectAsset& a : shell.session().project().assets())
                    if (a.type == want && ImGui::Selectable(a.path.c_str(), a.guid == add_sound_)) add_sound_ = a.guid;
                ImGui::EndCombo();
            }
        }
        if (ImGui::Button("Add")) {
            bool ok = false;
            if (add_type_ == T::Audio || add_type_ == T::Video) {
                // An audio or video key needs its asset up front (an empty track does not read back).
                cook::TimelineDocument next = ed.doc();
                TimelineDocTrack t;
                t.type = add_type_;
                t.target = add_target_;
                TimelineDocKey k;
                k.t = ed.playhead();
                if (add_type_ == T::Video) k.video = add_sound_;
                else k.sound = add_sound_;
                t.keys.push_back(k);
                next.tracks.push_back(t);
                ok = ed.apply(next);
                if (ok) ed.select(static_cast<u32>(ed.doc().tracks.size()) - 1, 0);
            } else {
                ok = ed.add_track(add_type_, add_target_, StringView(add_path_));
            }
            if (ok) ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    f32 to_x(f32 t, f32 x0, f32 pps) const { return x0 + (t - view_start_) * pps; }
    f32 to_t(f32 x, f32 x0, f32 pps) const { return view_start_ + (x - x0) / pps; }

    void draw_tracks(EditorShell& shell, TimelineEditor& ed, TimelinePreview& preview) {
        const EditorScene& scene = shell.scene();
        const EntityId player = ed.player_entity(scene);
        const cook::TimelineDocument& doc = ed.doc();
        const f32 length = std::max(doc.length(), 0.001f);
        const u32 rows = static_cast<u32>(doc.tracks.size());

        // The track list (left) and the key rows (right) share the row layout.
        const f32 list_w = std::clamp(ImGui::GetContentRegionAvail().x * 0.3f, 160.0f, 300.0f);
        const f32 avail_h = ImGui::GetContentRegionAvail().y;
        const TimelineDocTrack* sel = ed.selected_track() < rows ? &doc.tracks[ed.selected_track()] : nullptr;
        const f32 rows_h = std::max(kRulerH + kRowH * static_cast<f32>(rows) + 4.0f,
                                    avail_h - (sel && has_curve(*sel) ? kCurveH + 8.0f : 0.0f));

        if (ImGui::BeginChild("##list", ImVec2(list_w, rows_h))) {
            draw_add_track(shell, ed, player);
            ImGui::SetCursorPosY(kRulerH + 2.0f);
            for (u32 i = 0; i < rows; ++i) {
                char label[256];
                track_label(scene, player, doc.tracks[i], label, sizeof(label));
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::Selectable(label, ed.selected_track() == i, 0, ImVec2(0, kRowH - ImGui::GetStyle().ItemSpacing.y)))
                    ed.select(i);
                if (ImGui::BeginPopupContextItem("##ctx")) {
                    if (ImGui::MenuItem("Add key at playhead")) ed.add_key(i, snap(ed.playhead()));
                    if (ImGui::MenuItem("Remove track")) ed.remove_track(i);
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
        ImGui::SameLine();

        if (!ImGui::BeginChild("##rows", ImVec2(0, rows_h), 0, ImGuiWindowFlags_NoScrollWithMouse)) {
            ImGui::EndChild();
            return;
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const ImVec2 size = ImGui::GetContentRegionAvail();
        const f32 x0 = p0.x + 8.0f;
        const f32 width = std::max(size.x - 16.0f, 10.0f);
        const f32 pps = width / length * zoom_;
        const f32 visible = width / pps;
        view_start_ = std::clamp(view_start_, 0.0f, std::max(0.0f, length - visible));
        ImGui::InvisibleButton("##canvas", ImVec2(std::max(size.x, 1.0f), std::max(size.y, 1.0f)));
        const bool hovered = ImGui::IsItemHovered();
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        if (hovered && ImGui::GetIO().MouseWheel != 0.0f) {
            if (ImGui::GetIO().KeyCtrl)
                zoom_ = std::clamp(zoom_ * (ImGui::GetIO().MouseWheel > 0 ? 1.25f : 0.8f), 1.0f, 16.0f);
            else
                view_start_ -= ImGui::GetIO().MouseWheel * visible * 0.1f;
        }

        // Ruler: a tick per step that keeps labels about 60 px apart.
        dl->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + kRulerH), IM_COL32(40, 40, 46, 255));
        const f32 steps[] = {1.0f / 30.0f, 0.1f, 0.25f, 0.5f, 1.0f, 2.0f, 5.0f, 10.0f, 30.0f, 60.0f};
        f32 step = steps[0];
        for (f32 s : steps)
            if (s * pps >= 60.0f) {
                step = s;
                break;
            } else {
                step = s;
            }
        for (f32 t = std::floor(view_start_ / step) * step; t <= length + 1e-4f; t += step) {
            const f32 x = to_x(t, x0, pps);
            if (x < p0.x || x > p0.x + size.x) continue;
            dl->AddLine(ImVec2(x, p0.y + kRulerH - 6.0f), ImVec2(x, p0.y + size.y), kGridCol);
            char buf[16];
            std::snprintf(buf, sizeof(buf), step < 1.0f ? "%.2f" : "%.0f", static_cast<double>(t));
            dl->AddText(ImVec2(x + 2.0f, p0.y + 3.0f), IM_COL32(200, 200, 200, 255), buf);
        }
        const f32 end_x = to_x(length, x0, pps);
        dl->AddRectFilled(ImVec2(end_x, p0.y), ImVec2(p0.x + size.x, p0.y + size.y), IM_COL32(0, 0, 0, 60));

        // Rows and keys.
        const f32 rows_y = p0.y + kRulerH + 2.0f;
        u32 hit_track = kNoTrack, hit_key = kNoKey;
        for (u32 i = 0; i < rows; ++i) {
            const TimelineDocTrack& tr = doc.tracks[i];
            const f32 y = rows_y + kRowH * static_cast<f32>(i);
            const f32 cy = y + kRowH * 0.5f;
            if (ed.selected_track() == i) dl->AddRectFilled(ImVec2(p0.x, y), ImVec2(p0.x + size.x, y + kRowH), IM_COL32(255, 255, 255, 14));
            dl->AddLine(ImVec2(p0.x, y + kRowH), ImVec2(p0.x + size.x, y + kRowH), kGridCol);
            for (u32 k = 0; k < tr.keys.size(); ++k) {
                const TimelineDocKey& key = tr.keys[k];
                f32 t = key.t;
                if (drag_key_ == k && drag_track_ == i) t = drag_t_;
                const f32 x = to_x(t, x0, pps);
                if (has_range(tr.type)) {
                    const f32 x1 = to_x(t + key.duration, x0, pps);
                    dl->AddRectFilled(ImVec2(x, cy - 6.0f), ImVec2(x1, cy + 6.0f), kRangeCol, 3.0f);
                    if (tr.type == T::Subtitle) {
                        dl->PushClipRect(ImVec2(x, y), ImVec2(std::max(x1, x + 1.0f), y + kRowH), true);
                        dl->AddText(ImVec2(x + 8.0f, cy - 7.0f), IM_COL32(230, 230, 230, 255), key.name.c_str());
                        dl->PopClipRect();
                    }
                } else if (!key.name.empty()) {
                    dl->AddText(ImVec2(x + 8.0f, cy - 7.0f), IM_COL32(200, 200, 200, 200), key.name.c_str());
                }
                const bool selected = ed.selected_track() == i && ed.selected_key() == k;
                const ImU32 col = selected ? kKeySel : kKeyCol;
                dl->AddQuadFilled(ImVec2(x, cy - kKeyR), ImVec2(x + kKeyR, cy), ImVec2(x, cy + kKeyR), ImVec2(x - kKeyR, cy), col);
                if (hovered && std::fabs(mouse.x - x) <= kKeyR + 1.0f && std::fabs(mouse.y - cy) <= kKeyR + 1.0f) {
                    hit_track = i;
                    hit_key = k;
                }
            }
        }

        // Playhead (and the player's time while the preview plays).
        const f32 hx = to_x(ed.playhead(), x0, pps);
        dl->AddLine(ImVec2(hx, p0.y), ImVec2(hx, p0.y + size.y), kPlayheadCol, 2.0f);
        dl->AddTriangleFilled(ImVec2(hx - 6.0f, p0.y), ImVec2(hx + 6.0f, p0.y), ImVec2(hx, p0.y + 8.0f), kPlayheadCol);
        if (play_time_ >= 0.0f) {
            const f32 px = to_x(play_time_, x0, pps);
            dl->AddLine(ImVec2(px, p0.y), ImVec2(px, p0.y + size.y), kPlayCol, 1.5f);
        }
        (void)preview;

        // Mouse: the ruler scrubs; a key selects and drags; a double click on a row adds a key.
        const f32 mouse_t = to_t(mouse.x, x0, pps);
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if (mouse.y < p0.y + kRulerH) {
                scrubbing_ = true;
            } else if (hit_key != kNoKey) {
                ed.select(hit_track, hit_key);
                drag_track_ = hit_track;
                drag_key_ = hit_key;
                drag_t_ = doc.tracks[hit_track].keys[hit_key].t;
                drag_offset_ = drag_t_ - mouse_t;
            } else {
                const f32 row = (mouse.y - rows_y) / kRowH;
                if (row >= 0.0f && row < static_cast<f32>(rows)) ed.select(static_cast<u32>(row));
            }
        }
        if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && hit_key == kNoKey && mouse.y >= rows_y) {
            const f32 row = (mouse.y - rows_y) / kRowH;
            if (row >= 0.0f && row < static_cast<f32>(rows)) ed.add_key(static_cast<u32>(row), snap(std::clamp(mouse_t, 0.0f, length)));
        }
        if (scrubbing_) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
                ed.set_playhead(snap(std::clamp(mouse_t, 0.0f, length)));
            else
                scrubbing_ = false;
        }
        if (drag_key_ != kNoKey) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                drag_t_ = snap(std::max(0.0f, mouse_t + drag_offset_));
                ImGui::SetTooltip("%.3f s", static_cast<double>(drag_t_));
            } else {
                if (drag_track_ < rows && drag_key_ < doc.tracks[drag_track_].keys.size() &&
                    std::fabs(doc.tracks[drag_track_].keys[drag_key_].t - drag_t_) > TimelineEditor::kSameTime)
                    ed.move_key(drag_track_, drag_key_, drag_t_);
                drag_key_ = kNoKey;
                drag_track_ = kNoTrack;
            }
        }
        if (ImGui::IsWindowFocused() && ed.selected_key() != kNoKey &&
            (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace)))
            ed.remove_key(ed.selected_track(), ed.selected_key());
        ImGui::EndChild();
    }

    // ---- curves ----

    // The selected transform or property track as curves over the timeline: one line per component,
    // the keys as dots; dragging a dot up or down changes that component of the key.
    void draw_curve(TimelineEditor& ed, const TimelineDocTrack& tr) {
        if (tr.type == T::Transform) {
            ImGui::SetNextItemWidth(110.0f);
            const char* names[] = {"position", "rotation", "scale"};
            ImGui::Combo("##channel", &channel_, names, 3);
            ImGui::SameLine();
        }
        ImGui::TextDisabled("curve (drag a key up or down)");
        if (!ImGui::BeginChild("##curve", ImVec2(0, kCurveH - ImGui::GetFrameHeightWithSpacing()), ImGuiChildFlags_Borders)) {
            ImGui::EndChild();
            return;
        }
        const u8 channel = static_cast<u8>(1u << channel_);
        const u32 n = tr.type == T::Property ? tr.value_count : channel == cook::TimelineChannel::Rotation ? 4u : 3u;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const ImVec2 size = ImGui::GetContentRegionAvail();
        const f32 x0 = p0.x + 8.0f;
        const f32 width = std::max(size.x - 16.0f, 10.0f);
        const f32 length = std::max(ed.doc().length(), 0.001f);
        const f32 pps = width / length * zoom_;
        ImGui::InvisibleButton("##curve_canvas", ImVec2(std::max(size.x, 1.0f), std::max(size.y, 1.0f)));
        const bool hovered = ImGui::IsItemHovered();

        // The value range: every key and sample, padded.
        constexpr u32 kSamples = 160;
        f32 lo = 1e30f, hi = -1e30f;
        f32 v[4];
        for (u32 s = 0; s <= kSamples; ++s) {
            sample_timeline_track(tr, length * static_cast<f32>(s) / kSamples, v, channel);
            for (u32 c = 0; c < n; ++c) lo = std::min(lo, v[c]), hi = std::max(hi, v[c]);
        }
        if (drag_curve_ != kNoKey) lo = curve_lo_, hi = curve_hi_;  // a drag keeps the scale still
        if (hi - lo < 1e-3f) lo -= 1.0f, hi += 1.0f;
        const f32 pad = (hi - lo) * 0.1f;
        lo -= pad;
        hi += pad;
        const f32 y_top = p0.y + 4.0f, y_bot = p0.y + size.y - 4.0f;
        auto to_y = [&](f32 val) { return y_bot - (val - lo) / (hi - lo) * (y_bot - y_top); };
        auto from_y = [&](f32 y) { return lo + (y_bot - y) / (y_bot - y_top) * (hi - lo); };
        if (lo < 0.0f && hi > 0.0f) dl->AddLine(ImVec2(p0.x, to_y(0.0f)), ImVec2(p0.x + size.x, to_y(0.0f)), kGridCol);

        for (u32 c = 0; c < n; ++c) {
            ImVec2 pts[kSamples + 1];
            for (u32 s = 0; s <= kSamples; ++s) {
                const f32 t = length * static_cast<f32>(s) / kSamples;
                sample_timeline_track(tr, t, v, channel);
                pts[s] = ImVec2(to_x(t, x0, pps), to_y(v[c]));
            }
            dl->AddPolyline(pts, kSamples + 1, kChannelCol[c], 1.5f, 0);
        }
        const f32 hx = to_x(ed.playhead(), x0, pps);
        dl->AddLine(ImVec2(hx, p0.y), ImVec2(hx, p0.y + size.y), kPlayheadCol, 1.0f);

        const u32 track = ed.selected_track();
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        for (u32 k = 0; k < tr.keys.size(); ++k) {
            const TimelineDocKey& key = tr.keys[k];
            if (tr.type == T::Transform && !(key.channels & channel)) continue;
            const f32* vals = tr.type == T::Property ? key.value
                              : channel == cook::TimelineChannel::Position ? key.position
                              : channel == cook::TimelineChannel::Rotation ? key.rotation
                                                                            : key.scale;
            const f32 x = to_x(key.t, x0, pps);
            for (u32 c = 0; c < n; ++c) {
                f32 val = vals[c];
                if (drag_curve_ == k && drag_comp_ == c) val = drag_value_;
                const f32 y = to_y(val);
                const bool sel = ed.selected_key() == k;
                dl->AddCircleFilled(ImVec2(x, y), sel ? 5.0f : 4.0f, sel ? kKeySel : kChannelCol[c]);
                if (hovered && drag_curve_ == kNoKey && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                    std::fabs(mouse.x - x) <= 6.0f && std::fabs(mouse.y - y) <= 6.0f) {
                    ed.select(track, k);
                    drag_curve_ = k;
                    drag_comp_ = c;
                    drag_value_ = val;
                    curve_lo_ = lo + pad;
                    curve_hi_ = hi - pad;
                }
            }
        }
        if (drag_curve_ != kNoKey) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                drag_value_ = from_y(mouse.y);
                ImGui::SetTooltip("%.4g", static_cast<double>(drag_value_));
            } else {
                if (drag_curve_ < tr.keys.size()) {
                    TimelineDocKey key = tr.keys[drag_curve_];
                    f32* vals = tr.type == T::Property ? key.value
                                : channel == cook::TimelineChannel::Position ? key.position
                                : channel == cook::TimelineChannel::Rotation ? key.rotation
                                                                              : key.scale;
                    vals[drag_comp_] = drag_value_;
                    if ((tr.type == T::Transform && channel == cook::TimelineChannel::Rotation) ||
                        (tr.type == T::Property && tr.quat)) {
                        const f32 len = std::sqrt(vals[0] * vals[0] + vals[1] * vals[1] + vals[2] * vals[2] + vals[3] * vals[3]);
                        if (len > 1e-6f)
                            for (u32 c = 0; c < 4; ++c) vals[c] /= len;
                    }
                    ed.set_key(track, drag_curve_, key);
                }
                drag_curve_ = kNoKey;
            }
        }
        ImGui::EndChild();
    }

    // ---- the selected key ----

    bool ease_combo(asset::TimelineEase& e) {
        bool changed = false;
        if (ImGui::BeginCombo("Ease", cook::timeline_ease_name(e))) {
            for (u32 i = 0; i <= static_cast<u32>(asset::TimelineEase::EaseOut); ++i) {
                const auto v = static_cast<asset::TimelineEase>(i);
                if (ImGui::Selectable(cook::timeline_ease_name(v), v == e)) {
                    e = v;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("How the value moves from this key to the next");
        return changed;
    }

    // A field edited in place: commit when it is let go (or, for buttons and combos, at once).
    static bool done() { return ImGui::IsItemDeactivatedAfterEdit(); }

    void draw_inspector(EditorShell& shell, TimelineEditor& ed) {
        const u32 ti = ed.selected_track(), ki = ed.selected_key();
        if (ti >= ed.doc().tracks.size()) {
            ImGui::TextDisabled("Select a track or a key.");
            return;
        }
        const TimelineDocTrack& tr = ed.doc().tracks[ti];
        const EditorScene& scene = shell.scene();
        const EntityId player = ed.player_entity(scene);
        char label[256];
        track_label(scene, player, tr, label, sizeof(label));
        ImGui::SeparatorText(label);
        if (uses_target(tr.type)) {
            u8 target = tr.target;
            if (target_combo("Target", scene, player, target)) {
                cook::TimelineDocument next = ed.doc();
                next.tracks[ti].target = target;
                ed.apply(next);
            }
        }
        if (ImGui::Button("Key at playhead")) ed.add_key(ti, snap(ed.playhead()));
        ImGui::SameLine();
        if (ImGui::Button("Remove track")) {
            ed.remove_track(ti);
            return;
        }
        if (ki >= tr.keys.size()) {
            ImGui::TextDisabled("%u keys. Click a key to edit it.", static_cast<u32>(tr.keys.size()));
            return;
        }

        // The key as last read; edits go to `key_` and are committed as one set_key.
        if (key_of_rev_ != ed.revision() || key_of_track_ != ti || key_of_key_ != ki) {
            key_of_rev_ = ed.revision();
            key_of_track_ = ti;
            key_of_key_ = ki;
            key_ = tr.keys[ki];
            copy_text(text_, sizeof(text_), key_.name.view());
            copy_text(speaker_, sizeof(speaker_), key_.speaker.view());
        }
        ImGui::SeparatorText("Key");
        bool commit = false;
        ImGui::DragFloat(tr.type == T::Activation ? "From (s)" : "Time (s)", &key_.t, 0.01f, 0.0f, 3600.0f, "%.3f");
        commit |= done();
        switch (tr.type) {
            case T::Transform: {
                bool pos = key_.channels & cook::TimelineChannel::Position;
                bool rot = key_.channels & cook::TimelineChannel::Rotation;
                bool scl = key_.channels & cook::TimelineChannel::Scale;
                if (ImGui::Checkbox("Position", &pos)) commit = true;
                ImGui::SameLine();
                if (ImGui::Checkbox("Rotation", &rot)) commit = true;
                ImGui::SameLine();
                if (ImGui::Checkbox("Scale", &scl)) commit = true;
                key_.channels = static_cast<u8>((pos ? cook::TimelineChannel::Position : 0) |
                                                (rot ? cook::TimelineChannel::Rotation : 0) |
                                                (scl ? cook::TimelineChannel::Scale : 0));
                if (pos) {
                    ImGui::DragFloat3("##position", key_.position, 0.01f);
                    commit |= done();
                }
                if (rot) {
                    ImGui::DragFloat4("##rotation", key_.rotation, 0.005f, -1.0f, 1.0f);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Rotation (quaternion x y z w)");
                    commit |= done();
                }
                if (scl) {
                    ImGui::DragFloat3("##scale", key_.scale, 0.01f);
                    commit |= done();
                }
                commit |= ease_combo(key_.ease);
                break;
            }
            case T::Property:
                ImGui::DragScalarN("Value", ImGuiDataType_Float, key_.value, static_cast<int>(tr.value_count), 0.01f);
                commit |= done();
                commit |= ease_combo(key_.ease);
                break;
            case T::Activation:
                ImGui::DragFloat("Length (s)", &key_.duration, 0.01f, 0.0f, 3600.0f, "%.3f");
                commit |= done();
                break;
            case T::Animation:
                ImGui::InputText("Clip", text_, sizeof(text_));
                commit |= done();
                commit |= ImGui::Checkbox("Loop", &key_.loop);
                ImGui::DragFloat("Fade (s)", &key_.fade, 0.01f, -1.0f, 10.0f, key_.fade < 0.0f ? "Animator's" : "%.2f");
                commit |= done();
                break;
            case T::Audio: {
                const ProjectAsset* cur = shell.session().project().find(key_.sound);
                if (ImGui::BeginCombo("Sound", cur ? cur->path.c_str() : "(missing)")) {
                    for (const ProjectAsset& a : shell.session().project().assets())
                        if (a.type == asset::ChunkType::Sfx && ImGui::Selectable(a.path.c_str(), a.guid == key_.sound)) {
                            key_.sound = a.guid;
                            commit = true;
                        }
                    ImGui::EndCombo();
                }
                ImGui::DragFloat("Gain (dB)", &key_.gain_db, 0.1f, -60.0f, 12.0f, "%.1f");
                commit |= done();
                break;
            }
            case T::Video: {
                const ProjectAsset* cur = shell.session().project().find(key_.video);
                if (ImGui::BeginCombo("Video", cur ? cur->path.c_str() : "(missing)")) {
                    for (const ProjectAsset& a : shell.session().project().assets())
                        if (a.type == asset::ChunkType::Video && ImGui::Selectable(a.path.c_str(), a.guid == key_.video)) {
                            key_.video = a.guid;
                            commit = true;
                        }
                    ImGui::EndCombo();
                }
                ImGui::DragFloat("Length (s)", &key_.duration, 0.01f, 0.0f, 3600.0f, key_.duration > 0.0f ? "%.3f" : "to the end");
                commit |= done();
                ImGui::DragFloat("Gain (dB)", &key_.gain_db, 0.1f, -60.0f, 12.0f, "%.1f");
                commit |= done();
                break;
            }
            case T::Camera:
                commit |= target_combo("Camera", scene, player, key_.camera);
                ImGui::DragFloat("Blend (s)", &key_.blend, 0.01f, 0.0f, 30.0f, "%.2f");
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Ease the view over from the previous cut's camera (0 = a hard cut)");
                commit |= done();
                break;
            case T::Event:
                ImGui::InputText("Signal", text_, sizeof(text_));
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Pushed as a TimelineSignal (and a SeqSignal) when the key is reached");
                commit |= done();
                break;
            case T::Subtitle:
                ImGui::InputTextMultiline("Text", text_, sizeof(text_), ImVec2(0, ImGui::GetTextLineHeight() * 3.5f));
                commit |= done();
                ImGui::InputText("Speaker", speaker_, sizeof(speaker_));
                commit |= done();
                ImGui::DragFloat("Length (s)", &key_.duration, 0.01f, 0.0f, 3600.0f, "%.3f");
                commit |= done();
                break;
        }
        if (commit) {
            key_.name.assign(StringView(text_));
            key_.speaker.assign(StringView(speaker_));
            if (!ed.set_key(ti, ki, key_)) key_of_rev_ = ~u64(0);  // refused: show the key as it is
        }
        if (ImGui::Button("Remove key")) ed.remove_key(ti, ki);
    }

    f64 synced_ = 0.0;
    u32 snap_ = 2;  // 0.1 s
    f32 zoom_ = 1.0f;
    f32 view_start_ = 0.0f;
    f32 play_time_ = -1.0f;
    bool was_playing_ = false;
    bool scrubbing_ = false;
    u32 drag_track_ = kNoTrack, drag_key_ = kNoKey;
    f32 drag_t_ = 0.0f, drag_offset_ = 0.0f;
    u32 drag_curve_ = kNoKey, drag_comp_ = 0;
    f32 drag_value_ = 0.0f, curve_lo_ = 0.0f, curve_hi_ = 1.0f;
    int channel_ = 0;
    // The new-timeline and add-track popups.
    char new_path_[256] = "cutscenes/intro.mctimeline";
    f32 new_duration_ = 5.0f;
    char new_error_[512] = {};
    T add_type_ = T::Transform;
    u8 add_target_ = asset::kTimelineSelf;
    char add_path_[128] = {};
    asset::Guid add_sound_;
    // Header fields and the key inspector's copy of the selected key.
    char name_[64] = {};
    u64 name_of_ = ~u64(0);
    f32 duration_ = 0.0f;
    u64 duration_of_ = ~u64(0);
    TimelineDocKey key_;
    u64 key_of_rev_ = ~u64(0);
    u32 key_of_track_ = kNoTrack, key_of_key_ = kNoKey;
    char text_[512] = {};
    char speaker_[64] = {};
};

}  // namespace

void add_timeline_panel(EditorShell& shell) { shell.add_panel(mem::make_unique<TimelinePanel>(mem::heap())); }

}  // namespace mce::editor
