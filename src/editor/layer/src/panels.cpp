// The shell's panel set (editor-tools.md §2.7, §2.9): Viewport, Console, Stats, plus the
// scene-editing panels (scene_panels.cpp).

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "imgui_renderer.h"
#include "mce/editor/editor_shell.h"
#include "mce/editor/asset_browser.h"
#include "mce/editor/gizmo.h"
#include "mce/kit/sprite_extract.h"
#include "mce/log.h"
#include "scene_panels.h"

namespace mce::editor {
namespace {

// ---- Viewport ----------------------------------------------------------------------------------
// Displays the stub world (no project) or the open scene's preview world (project open). The
// target is sized to the panel in pixels (widget size x framebuffer scale, FR-R2D-080) and
// rebuilt when that changes; the old one's destroy is deferred by the device until in-flight
// frames are done with it.
//
// Scene mode: wheel zooms at the cursor, right/middle drag pans, left click picks (Ctrl toggles,
// Shift adds), left drag on empty space draws a selection marquee, left drag on an entity moves
// the selection, and the gizmo moves, rotates or scales it (W / E / R; snap: Edit menu, Ctrl
// inverts; Esc or right click cancels the drag), F frames the selection (or the scene), Del
// deletes the selection.
class ViewportPanel final : public IEditorPanel {
public:
    const char* name() const override { return "Viewport"; }

    void draw(EditorShell& shell) override {
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const ImVec2 scale = ImGui::GetIO().DisplayFramebufferScale;
        const rhi::Extent2D px{static_cast<u32>(std::max(1.0f, avail.x * scale.x)),
                               static_cast<u32>(std::max(1.0f, avail.y * scale.y))};
        if (avail.x < 1.0f || avail.y < 1.0f) return;
        if (target_.is_null() || !(px == size_)) {
            rhi::RenderDevice& device = shell.device();
            if (!target_.is_null()) device.destroy_texture(target_);
            rhi::TextureDesc td;
            td.size = px;
            td.format = rhi::TextureFormat::RGBA8_UNorm;
            td.usage = rhi::TextureUsage::SampledColorTarget;
            td.debug_name = "editor.viewport";
            target_ = device.create_texture(td);
            size_ = px;
            if (target_.is_null()) {
                MCE_LOG_ERROR("editor: viewport target creation failed ({}x{})", px.width, px.height);
                return;
            }
        }
        // Render targets are stored bottom row first (GL convention): flip V to show them upright.
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImGui::Image(ImGuiRhiRenderer::to_id(target_), avail, ImVec2(0, 1), ImVec2(1, 0));
        const bool hovered = ImGui::IsItemHovered();
        const bool focused = ImGui::IsWindowFocused() && !ImGui::GetIO().WantTextInput;
        shell.set_viewport(ViewportInfo{target_, size_, hovered, focused});
        // An asset dragged from the browser: a texture becomes a sprite, a prefab an instance,
        // where it is dropped; a scene opens.
        if (shell.scene_mode() && !shell.playing() && ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kAssetPayload)) {
                asset::Guid g;
                std::memcpy(&g, p->Data, sizeof(g));
                if (const ProjectAsset* a = shell.session().project().find(g)) {
                    const ImVec2 m = ImGui::GetIO().MousePos;
                    const math::vec2 at{(m.x - origin.x) * scale.x, (m.y - origin.y) * scale.y};
                    if (asset_drop_kind(a->type) == AssetDrop::OpenScene) shell.request_open_scene(a->path.view());
                    else drop_asset(shell.scene(), *a, shell.camera().screen_to_world(at, size_));
                }
            }
            ImGui::EndDragDropTarget();
        }
        if (shell.scene_mode()) interact(shell, origin, scale, hovered);
    }

    void record(EditorShell& shell, rhi::CommandBuffer& cmd, f32 alpha) override {
        const ViewportInfo& v = shell.viewport();
        if (v.target.is_null()) return;
        if (!shell.scene_mode()) {
            shell.world().record(cmd, v.target, v.size, alpha);
            return;
        }
        EditorScene& scene = shell.scene();
        lines_.clear();
        const ViewSettings& view = shell.view();
        if (PlaySession& play = shell.session().play(); play.active()) {
            // Play-in-editor: the play world, through the game's camera (or the editor's).
            EditorCamera2D cam = shell.camera();
            render::Camera2D game;
            f32 ppm = cam.pixels_per_meter;
            math::Color clear{0.08f, 0.09f, 0.12f, 1.0f};
            if (view.play_camera && kit::primary_camera(*play.world(), game, ppm, &clear)) {
                cam.center = game.position;
                cam.zoom = game.zoom;
                cam.pixels_per_meter = ppm;
            }
            if (view.show_grid && !view.play_camera) grid_lines(cam, v.size, view.grid_step, view.grid_major, lines_);
            shell.preview().record(cmd, *play.world(), v.target, v.size, cam, lines_.span(), clear);
            return;
        }
        if (view.orbit_view) {
            record_orbit(shell, cmd, v);
            return;
        }
        if (view.show_grid) grid_lines(shell.camera(), v.size, view.grid_step, view.grid_major, lines_);
        math::vec2 q[4];
        if (hover_ != kNoEntity && !scene.selection().contains(hover_) && entity_quad(scene, hover_, q))
            quad_lines(q, math::Color{1.0f, 1.0f, 1.0f, 0.35f}, 1.0f, lines_);
        for (EntityId id : scene.selection().items())
            if (entity_quad(scene, id, q)) quad_lines(q, math::Color{1.0f, 0.62f, 0.1f, 1.0f}, 2.0f, lines_);
        math::vec2 pivot;
        if (painting_tiles(shell)) {
            tile_lines(shell);
        } else if (TransformGizmo::pivot(scene, pivot)) {
            gizmo_.lines(shell.camera(), pivot, hot_, lines_);
        }
        if (marquee_) {
            const math::vec2 a = marquee_from_, b = marquee_to_;
            const math::vec2 m[4] = {a, {b.x, a.y}, b, {a.x, b.y}};
            quad_lines(m, math::Color{0.45f, 0.75f, 1.0f, 0.9f}, 1.0f, lines_);
        }
        shell.preview().record(cmd, scene.world(), v.target, v.size, shell.camera(), lines_.span(),
                               math::Color{0.11f, 0.11f, 0.12f, 1.0f});
    }

    void shutdown(EditorShell& shell) override {
        if (!target_.is_null()) shell.device().destroy_texture(target_);
        target_ = {};
    }

private:
    void interact(EditorShell& shell, ImVec2 origin, ImVec2 scale, bool hovered) {
        EditorScene& scene = shell.scene();
        EditorCamera2D& cam = shell.camera();
        if (shell.take_frame_request()) frame(scene, cam);

        const ImGuiIO& io = ImGui::GetIO();
        const math::vec2 mouse{(io.MousePos.x - origin.x) * scale.x, (io.MousePos.y - origin.y) * scale.y};
        const math::vec2 mouse_world = cam.screen_to_world(mouse, size_);
        math::vec2 pivot;
        const bool has_pivot = TransformGizmo::pivot(scene, pivot);
        gizmo_.set_mode(shell.view().gizmo);  // ignored mid-drag
        hot_ = hovered && has_pivot && !gizmo_.dragging() ? TransformGizmo::hit(cam, size_, pivot, mouse, gizmo_.mode())
                                                          : GizmoPart::None;
        hover_ = hovered && hot_ == GizmoPart::None ? pick_entity(scene, mouse_world) : kNoEntity;

        if (shell.view().orbit_view && !shell.playing()) {
            interact_orbit(shell, hovered, scale);
            return;
        }
        if (hovered && io.MouseWheel != 0.0f) cam.zoom_at(mouse, size_, std::pow(1.15f, io.MouseWheel));
        if (shell.playing()) {  // play-in-editor: look around, but the document is locked
            if (gizmo_.dragging()) {
                gizmo_.cancel(scene);
                scene.end_edit();
            }
            hot_ = GizmoPart::None;
            hover_ = kNoEntity;
            marquee_ = false;
            tile_hover_ = false;
            pan(cam, hovered, scale);
            return;
        }

        // Marquee in progress: drag a rectangle over empty space, release to select what it touches.
        if (marquee_) {
            marquee_to_ = mouse_world;
            if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                marquee_ = false;
            } else if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                marquee_ = false;
                const math::aabb2 rect{min(marquee_from_, marquee_to_), max(marquee_from_, marquee_to_)};
                Array<EntityId> hit;
                entities_in_rect(scene, rect, hit);
                if (!marquee_add_) scene.selection().clear();
                for (EntityId id : hit)
                    if (!scene.selection().contains(id)) scene.selection().toggle(id);
            }
            return;
        }

        // Drag in progress: move, cancel, or finish.
        if (gizmo_.dragging()) {
            if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                gizmo_.cancel(scene);
                scene.end_edit();
            } else if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                gizmo_.end();
                scene.end_edit();  // the whole drag is one undo step
            } else if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.0f)) {
                const ViewSettings& view = shell.view();
                const bool snap = view.snap != io.KeyCtrl;
                gizmo_.update(scene, mouse_world, snap ? view.snap_step : 0.0f);
                const char* snapped = snap ? "  (snap)" : "";
                if (gizmo_.mode() == GizmoMode::Rotate) {
                    ImGui::SetTooltip("%+.1f deg%s", static_cast<f64>(gizmo_.angle() * 57.2957795f), snapped);
                } else if (gizmo_.mode() == GizmoMode::Scale) {
                    const math::vec2 f = gizmo_.factor();
                    ImGui::SetTooltip("x%.2f, x%.2f%s", static_cast<f64>(f.x), static_cast<f64>(f.y), snapped);
                } else {
                    const math::vec2 d = gizmo_.delta();
                    ImGui::SetTooltip("%+.2f, %+.2f m%s", static_cast<f64>(d.x), static_cast<f64>(d.y), snapped);
                }
            }
            return;
        }

        pan(cam, hovered, scale);
        if (paint_tiles(shell, hovered, mouse_world)) return;

        // Press: a gizmo handle starts a constrained drag (move, rotate or scale, W/E/R); an entity
        // is selected (Ctrl toggles, Shift adds) and, with the move tool, starts a free drag; empty
        // space starts a marquee (Ctrl/Shift add to the selection).
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if (hot_ != GizmoPart::None) {
                if (gizmo_.begin(scene, hot_, mouse_world)) scene.begin_edit(gizmo_mode_label(gizmo_.mode()));
            } else if (hover_ == kNoEntity) {
                marquee_ = true;
                marquee_add_ = io.KeyCtrl || io.KeyShift;
                marquee_from_ = marquee_to_ = mouse_world;
            } else if (io.KeyCtrl) {
                scene.selection().toggle(hover_);
            } else if (io.KeyShift) {
                if (!scene.selection().contains(hover_)) scene.selection().toggle(hover_);
            } else {
                if (!scene.selection().contains(hover_)) scene.selection().set(hover_);
                if (gizmo_.mode() == GizmoMode::Move && gizmo_.begin(scene, GizmoPart::XY, mouse_world))
                    scene.begin_edit("Move");
            }
        }
        if (ImGui::IsWindowFocused() && !io.WantTextInput) {
            if (ImGui::IsKeyPressed(ImGuiKey_F, false)) frame(scene, cam);
            if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) delete_selection(scene);
        }
    }

    // 3D orbit view (M5b camera-collision gate): left drag orbits, right/middle drag pans, wheel
    // dollies, F frames the colliders. Inspection only: picking and gizmos stay in the 2D view.
    void interact_orbit(EditorShell& shell, bool hovered, ImVec2 scale) {
        OrbitCamera& orbit = shell.orbit();
        const ImGuiIO& io = ImGui::GetIO();
        marquee_ = false;
        if (gizmo_.dragging()) {
            gizmo_.cancel(shell.scene());
            shell.scene().end_edit();
        }
        hot_ = GizmoPart::None;
        hover_ = kNoEntity;
        if (shell.take_frame_request() || (ImGui::IsWindowFocused() && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F, false))) {
            shell.orbit_colliders().sync(shell.scene(), shell.view().orbit_depth);
            math::aabb3 box = shell.orbit_colliders().bounds();
            if (box.min.x > box.max.x) {
                const math::aabb2 b2 = scene_bounds(shell.scene());
                box = math::aabb3{{b2.min.x, b2.min.y, -0.5f}, {b2.max.x, b2.max.y, 0.5f}};
            }
            orbit.frame(box);
        }
        if (hovered && io.MouseWheel != 0.0f) orbit.dolly(io.MouseWheel);
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) orbiting_ = true;
        if (orbiting_) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) orbit.orbit({io.MouseDelta.x * scale.x, io.MouseDelta.y * scale.y});
            else orbiting_ = false;
        }
        if (hovered && (ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsMouseClicked(ImGuiMouseButton_Middle)))
            panning_ = true;
        if (panning_) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Right) || ImGui::IsMouseDown(ImGuiMouseButton_Middle))
                orbit.pan({io.MouseDelta.x * scale.x, io.MouseDelta.y * scale.y}, size_);
            else
                panning_ = false;
        }
        const f32 cut = orbit.distance - orbit.effective;
        ImGui::SetCursorScreenPos(ImVec2(ImGui::GetItemRectMin().x + 8, ImGui::GetItemRectMin().y + 6));
        ImGui::TextColored(cut > 1e-3f ? ImVec4(1, 0.7f, 0.3f, 1) : ImVec4(0.7f, 0.7f, 0.7f, 1),
                           "orbit  boom %.2f m%s  (%u colliders)", static_cast<double>(orbit.effective),
                           cut > 1e-3f ? "  cut by collision" : "", shell.orbit_colliders().body_count());
    }

    void record_orbit(EditorShell& shell, rhi::CommandBuffer& cmd, const ViewportInfo& v) {
        const ViewSettings& view = shell.view();
        OrbitCamera& orbit = shell.orbit();
        OrbitColliders& colliders = shell.orbit_colliders();
        colliders.sync(shell.scene(), view.orbit_depth);
        if (view.orbit_collision) orbit.resolve(colliders.world());
        else orbit.unresolved();
        const render::View3D view3d = render::compute_view_3d(orbit.camera(), v.size);
        segments_.clear();
        orbit_wireframe(shell.scene(), view.orbit_depth, segments_);
        // Pixels -> the 2D overlay path: a camera with one pixel per metre centred on the viewport.
        EditorCamera2D px_cam;
        px_cam.pixels_per_meter = 1.0f;
        const math::vec2 half{0.5f * static_cast<f32>(v.size.width), 0.5f * static_cast<f32>(v.size.height)};
        for (const Segment3D& seg : segments_) {
            math::vec2 a, b;
            if (!project_segment(view3d, seg, a, b)) continue;
            lines_.push_back({{a.x - half.x, half.y - a.y}, {b.x - half.x, half.y - b.y}, seg.color, 1.5f});
        }
        shell.preview().record(cmd, empty_, v.target, v.size, px_cam, lines_.span(), math::Color{0.09f, 0.09f, 0.1f, 1.0f});
    }

    // ---- tile painting (Tilemap panel, ADR-0030 §8) ----
    // Active when the paint toggle is on and the single selected entity has a TilemapLayer.
    bool painting_tiles(EditorShell& shell) {
        EditorScene& scene = shell.scene();
        TilemapEditor& tm = shell.tilemap();
        if (!shell.view().tile_paint || scene.selection().items().size() != 1) return false;
        return tm.bind(shell.session(), scene.selection().items()[0]);
    }

    // Left drag paints the brush, Ctrl+left erases, Shift+left drag fills the rectangle on release
    // (Ctrl+Shift erases it), Esc cancels a rectangle. One press to release is one stroke: one
    // save, one re-bake, one tile undo step.
    bool paint_tiles(EditorShell& shell, bool hovered, math::vec2 mouse_world) {
        tile_hover_ = false;
        if (!painting_tiles(shell)) {
            if (shell.tilemap().in_stroke()) finish_stroke(shell);
            tile_rect_ = false;
            return false;
        }
        TilemapEditor& tm = shell.tilemap();
        const ImGuiIO& io = ImGui::GetIO();
        i32 x = 0, y = 0;
        const bool on_map = tm.tile_at(shell.scene(), mouse_world, x, y);
        tile_hover_ = hovered && on_map;
        tile_x_ = x;
        tile_y_ = y;
        const u32 tile = io.KeyCtrl ? 0u : tm.brush;
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            tm.begin_stroke();
            tile_rect_ = io.KeyShift;
            rect_x_ = x;
            rect_y_ = y;
            if (!tile_rect_ && on_map) tm.paint(x, y, tile);
            return true;
        }
        if (!tm.in_stroke()) return hovered;  // the paint tool owns left clicks over the viewport
        if (tile_rect_ && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            tile_rect_ = false;
            finish_stroke(shell);
        } else if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (tile_rect_) tm.fill(rect_x_, rect_y_, x, y, tile);
            tile_rect_ = false;
            finish_stroke(shell);
        } else if (!tile_rect_ && on_map) {
            tm.paint(x, y, tile);
        } else if (tile_rect_) {
            ImGui::SetTooltip("%d x %d", std::abs(x - rect_x_) + 1, std::abs(y - rect_y_) + 1);
        }
        return true;
    }

    void finish_stroke(EditorShell& shell) {
        Result<f64> r = shell.tilemap().end_stroke(shell.session());
        if (!r) {
            FixedString<512> msg;
            format_error(r.error(), msg);
            MCE_LOG_ERROR("editor: tilemap save failed: {}", msg.view());
        }
    }

    void tile_lines(EditorShell& shell) {
        const TilemapEditor& tm = shell.tilemap();
        const EditorScene& scene = shell.scene();
        tm.lines(scene, lines_);
        math::vec2 lo, hi;
        if (tile_rect_ && tm.in_stroke()) {
            math::vec2 lo2, hi2;
            tm.tile_rect(scene, std::min(rect_x_, tile_x_), std::min(rect_y_, tile_y_), lo, hi);
            tm.tile_rect(scene, std::max(rect_x_, tile_x_), std::max(rect_y_, tile_y_), lo2, hi2);
            const math::vec2 q[4] = {lo, {hi2.x, lo.y}, hi2, {lo.x, hi2.y}};
            quad_lines(q, math::Color{0.45f, 0.75f, 1.0f, 0.9f}, 2.0f, lines_);
        } else if (tile_hover_) {
            tm.tile_rect(scene, tile_x_, tile_y_, lo, hi);
            const math::vec2 q[4] = {lo, {hi.x, lo.y}, hi, {lo.x, hi.y}};
            quad_lines(q, math::Color{1.0f, 1.0f, 1.0f, 0.9f}, 2.0f, lines_);
        }
    }

    // Right or middle drag pans the editor camera.
    void pan(EditorCamera2D& cam, bool hovered, ImVec2 scale) {
        const ImGuiIO& io = ImGui::GetIO();
        if (hovered && (ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsMouseClicked(ImGuiMouseButton_Middle)))
            panning_ = true;
        if (panning_) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Right) || ImGui::IsMouseDown(ImGuiMouseButton_Middle))
                cam.pan_pixels({io.MouseDelta.x * scale.x, io.MouseDelta.y * scale.y});
            else
                panning_ = false;
        }
    }

    // Frames the selection, or the whole scene when nothing is selected.
    void frame(const EditorScene& scene, EditorCamera2D& cam) {
        math::aabb2 box{{1e30f, 1e30f}, {-1e30f, -1e30f}};
        bool any = false;
        math::vec2 q[4];
        for (EntityId id : scene.selection().items()) {
            if (!entity_quad(scene, id, q)) continue;
            for (const math::vec2& p : q) {
                box.min = math::min(box.min, p);
                box.max = math::max(box.max, p);
            }
            any = true;
        }
        cam.frame(any ? box : scene_bounds(scene), size_);
    }

    rhi::TextureHandle target_;
    rhi::Extent2D size_;
    Array<OverlayLine> lines_;
    Array<Segment3D> segments_;
    World empty_;  // the orbit view draws overlays only
    bool orbiting_ = false;
    EntityId hover_ = kNoEntity;
    TransformGizmo gizmo_;
    GizmoPart hot_ = GizmoPart::None;
    bool marquee_ = false;
    bool marquee_add_ = false;
    math::vec2 marquee_from_, marquee_to_;  // world
    bool panning_ = false;
    bool tile_hover_ = false;
    bool tile_rect_ = false;  // Shift+drag rectangle fill in progress
    i32 tile_x_ = 0, tile_y_ = 0, rect_x_ = 0, rect_y_ = 0;
};

// ---- Console -----------------------------------------------------------------------------------
class ConsolePanel final : public IEditorPanel {
public:
    const char* name() const override { return "Console"; }

    void draw(EditorShell& shell) override {
        ConsoleModel& console = shell.console();

        if (ImGui::Button("Clear")) console.clear();
        ImGui::SameLine();
        static constexpr const char* kLevels[] = {"Trace", "Debug", "Info", "Warn", "Error"};
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
        ImGui::Combo("##level", &min_level_, kLevels, IM_ARRAYSIZE(kLevels));
        ImGui::SameLine();
        filter_.Draw("Filter", ImGui::GetFontSize() * 14.0f);
        ImGui::SameLine();
        ImGui::Checkbox("Auto-scroll", &auto_scroll_);
        if (console.dropped()) {
            ImGui::SameLine();
            ImGui::TextDisabled("(%llu lines dropped)", static_cast<unsigned long long>(console.dropped()));
        }
        ImGui::Separator();

        const f32 input_height = ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeightWithSpacing();
        if (ImGui::BeginChild("##scrollback", ImVec2(0, -input_height), ImGuiChildFlags_None,
                              ImGuiWindowFlags_HorizontalScrollbar)) {
            visible_.clear();
            for (usize i = 0; i < console.size(); ++i) {
                const log::ConsoleLine& l = console.line(i);
                if (static_cast<int>(l.level) < min_level_) continue;
                if (!filter_.PassFilter(l.text.c_str(), l.text.c_str() + l.text.view().size())) continue;
                visible_.push_back(static_cast<u32>(i));
            }
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 1));
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(visible_.size()));
            while (clipper.Step()) {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                    const log::ConsoleLine& l = console.line(visible_[static_cast<usize>(row)]);
                    ImGui::PushStyleColor(ImGuiCol_Text, level_color(l.level));
                    ImGui::TextUnformatted(l.text.c_str(), l.text.c_str() + l.text.view().size());
                    ImGui::PopStyleColor();
                }
            }
            ImGui::PopStyleVar();
            // Stick to the bottom while the user has not scrolled up.
            if (auto_scroll_ && console.revision() != seen_revision_ &&
                ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - ImGui::GetTextLineHeight() * 2.0f)
                ImGui::SetScrollHereY(1.0f);
            seen_revision_ = console.revision();
        }
        ImGui::EndChild();
        ImGui::Separator();

        const ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_EscapeClearsAll |
                                          ImGuiInputTextFlags_CallbackHistory | ImGuiInputTextFlags_CallbackCompletion;
        ImGui::SetNextItemWidth(-1.0f);
        bool reclaim_focus = false;
        if (ImGui::InputTextWithHint("##command", "command (Tab completes, Up/Down history)", input_, sizeof(input_),
                                     flags, &ConsolePanel::on_input, &console)) {
            console.exec(input_);
            input_[0] = 0;
            reclaim_focus = true;
            ImGui::SetScrollHereY(1.0f);
        }
        ImGui::SetItemDefaultFocus();
        if (reclaim_focus) ImGui::SetKeyboardFocusHere(-1);
    }

private:
    static ImVec4 level_color(log::Level level) {
        switch (level) {
            case log::Level::Trace:
            case log::Level::Debug: return ImVec4(0.55f, 0.57f, 0.62f, 1.0f);
            case log::Level::Warn: return ImVec4(1.0f, 0.78f, 0.35f, 1.0f);
            case log::Level::Error:
            case log::Level::Critical: return ImVec4(1.0f, 0.42f, 0.40f, 1.0f);
            default: return ImGui::GetStyleColorVec4(ImGuiCol_Text);
        }
    }

    static int on_input(ImGuiInputTextCallbackData* data) {
        auto* console = static_cast<ConsoleModel*>(data->UserData);
        if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory) {
            const StringView entry = console->browse_history(data->EventKey == ImGuiKey_UpArrow ? -1 : 1);
            data->DeleteChars(0, data->BufTextLen);
            data->InsertChars(0, entry.data(), entry.data() + entry.size());
        } else if (data->EventFlag == ImGuiInputTextFlags_CallbackCompletion) {
            // Completes the command name (the first word) only.
            const StringView text(data->Buf, static_cast<usize>(data->BufTextLen));
            if (text.find(' ') != StringView::npos) return 0;
            Array<const char*> names;
            if (console->complete(text, names) > 1) {
                FixedString<512> list;
                for (const char* n : names) {
                    if (list.view().size() + std::strlen(n) + 2 > 500) {
                        list.append(" ...");
                        break;
                    }
                    list.append(n);
                    list.append("  ");
                }
                MCE_LOG_INFO("{}", list.view());
            }
            const FixedString<128> common = console->complete_common(text);
            data->DeleteChars(0, data->BufTextLen);
            data->InsertChars(0, common.c_str());
            if (names.size() == 1) data->InsertChars(data->CursorPos, " ");
        }
        return 0;
    }

    ImGuiTextFilter filter_;
    Array<u32> visible_;
    char input_[256] = {};
    int min_level_ = 2;  // Info
    u64 seen_revision_ = ~u64(0);
    bool auto_scroll_ = true;
};

// ---- Stats -------------------------------------------------------------------------------------
class StatsPanel final : public IEditorPanel {
public:
    const char* name() const override { return "Stats"; }

    void draw(EditorShell& shell) override {
        rhi::RenderDevice& device = shell.device();
        const f32 fps = shell.framerate();
        ImGui::Text("Editor  %.1f fps  (%.2f ms)", fps, fps > 0 ? 1000.0f / fps : 0.0f);
        ImGui::Text("Sim ticks  %llu", static_cast<unsigned long long>(shell.world().ticks()));
        ImGui::Separator();
        const ViewportInfo& v = shell.viewport();
        ImGui::Text("Viewport  %u x %u px", v.size.width, v.size.height);
        const ImVec2 fb = ImGui::GetIO().DisplayFramebufferScale;
        ImGui::Text("Display scale  %.2f  (fb %.2f)", static_cast<double>(shell.window().display_scale()),
                    static_cast<double>(fb.x));
        ImGui::Separator();
        const rhi::FrameStats& s = device.last_frame_stats();
        ImGui::Text("Draws %u  passes %u  cmds %u", s.draws, s.passes, s.commands);
        ImGui::Text("Geometry %.1f KiB  uniforms %.1f KiB", static_cast<double>(s.geometry_bytes) / 1024.0,
                    static_cast<double>(s.uniform_bytes) / 1024.0);
        ImGui::Text("UI vertices %u  (%u from clipping)", shell.imgui_vertex_count(), shell.imgui_clipped_vertex_count());
        if (s.ring_overflows) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Ring overflows %u", s.ring_overflows);
        const u32 errors = device.validation_error_count();
        if (errors) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "GL validation errors %u", errors);
        ImGui::Separator();
        const rhi::DeviceCaps& caps = device.caps();
        ImGui::TextWrapped("%s", caps.api_name.c_str());
        ImGui::TextWrapped("%s", caps.device_name.c_str());
        ImGui::Text("Texture memory %.1f MiB", static_cast<double>(device.texture_memory_bytes()) / (1024.0 * 1024.0));
    }
};

}  // namespace

void add_default_panels(EditorShell& shell) {
    shell.add_panel(mem::make_unique<ViewportPanel>(mem::heap()));
    add_scene_panels(shell);
    shell.add_panel(mem::make_unique<ConsolePanel>(mem::heap()));
    shell.add_panel(mem::make_unique<StatsPanel>(mem::heap()));
    add_profiler_panel(shell);
    add_play_panel(shell);
    add_asset_panel(shell);
    add_tilemap_panel(shell);
}

}  // namespace mce::editor
