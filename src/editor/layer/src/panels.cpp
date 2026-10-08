// The shell's panel set (editor-tools.md §2.7, §2.9): Viewport, Console, Stats, plus the
// scene-editing panels (scene_panels.cpp).

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "imgui_renderer.h"
#include "mce/editor/editor_shell.h"
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
// Scene mode: wheel zooms at the cursor, right/middle drag pans, left click picks (Ctrl toggles),
// F frames the selection (or the scene), Del deletes the selection.
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
        shell.set_viewport(ViewportInfo{target_, size_, hovered});
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
        if (view.show_grid) grid_lines(shell.camera(), v.size, view.grid_step, view.grid_major, lines_);
        math::vec2 q[4];
        if (hover_ != kNoEntity && !scene.selection().contains(hover_) && entity_quad(scene, hover_, q))
            quad_lines(q, math::Color{1.0f, 1.0f, 1.0f, 0.35f}, 1.0f, lines_);
        for (EntityId id : scene.selection().items())
            if (entity_quad(scene, id, q)) quad_lines(q, math::Color{1.0f, 0.62f, 0.1f, 1.0f}, 2.0f, lines_);
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
        hover_ = hovered ? pick_entity(scene, cam.screen_to_world(mouse, size_)) : kNoEntity;
        if (hovered && io.MouseWheel != 0.0f) cam.zoom_at(mouse, size_, std::pow(1.15f, io.MouseWheel));
        if (hovered && (ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsMouseClicked(ImGuiMouseButton_Middle)))
            panning_ = true;
        if (panning_) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Right) || ImGui::IsMouseDown(ImGuiMouseButton_Middle))
                cam.pan_pixels({io.MouseDelta.x * scale.x, io.MouseDelta.y * scale.y});
            else
                panning_ = false;
        }
        if (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
            ImGui::GetMouseDragDelta(ImGuiMouseButton_Left).x * ImGui::GetMouseDragDelta(ImGuiMouseButton_Left).x +
                    ImGui::GetMouseDragDelta(ImGuiMouseButton_Left).y * ImGui::GetMouseDragDelta(ImGuiMouseButton_Left).y <
                16.0f) {
            if (hover_ == kNoEntity) {
                if (!io.KeyCtrl) scene.selection().clear();
            } else if (io.KeyCtrl) {
                scene.selection().toggle(hover_);
            } else {
                scene.selection().set(hover_);
            }
        }
        if (ImGui::IsWindowFocused() && !io.WantTextInput) {
            if (ImGui::IsKeyPressed(ImGuiKey_F, false)) frame(scene, cam);
            if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) delete_selection(scene);
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
    EntityId hover_ = kNoEntity;
    bool panning_ = false;
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
}

}  // namespace mce::editor
