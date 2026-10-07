// The M0 panel set (editor-tools.md §2.7, §2.9): Viewport, Console, Stats.

#include <imgui.h>

#include <algorithm>
#include <cstring>

#include "imgui_renderer.h"
#include "mce/editor/editor_shell.h"
#include "mce/log.h"

namespace mce::editor {
namespace {

// ---- Viewport ----------------------------------------------------------------------------------
// Displays the stub world's render target. The target is sized to the panel in pixels (widget
// size x framebuffer scale, FR-R2D-080) and rebuilt when that changes; the old one's destroy is
// deferred by the device until in-flight frames are done with it.
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
        ImGui::Image(ImGuiRhiRenderer::to_id(target_), avail, ImVec2(0, 1), ImVec2(1, 0));
        shell.set_viewport(ViewportInfo{target_, size_, ImGui::IsItemHovered()});
    }

    void record(EditorShell& shell, rhi::CommandBuffer& cmd, f32 alpha) override {
        const ViewportInfo& v = shell.viewport();
        if (!v.target.is_null()) shell.world().record(cmd, v.target, v.size, alpha);
    }

    void shutdown(EditorShell& shell) override {
        if (!target_.is_null()) shell.device().destroy_texture(target_);
        target_ = {};
    }

private:
    rhi::TextureHandle target_;
    rhi::Extent2D size_;
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
    shell.add_panel(mem::make_unique<ConsolePanel>(mem::heap()));
    shell.add_panel(mem::make_unique<StatsPanel>(mem::heap()));
}

}  // namespace mce::editor
