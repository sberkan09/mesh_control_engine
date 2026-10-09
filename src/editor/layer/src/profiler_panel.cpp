// The Profiler panel (ROADMAP M7 editor perf dashboards, FR-EDT-140): the editor's frame-time
// history against the 16.6 ms budget, the NFR-F-02 stage table (p95 against each allocation), and
// while playing, the play world's systems ranked by cost. Pause freezes the history so a spike
// stays on screen; Copy puts the report on the clipboard as Markdown (the milestone docs' shape).

#include <imgui.h>

#include <cstdio>

#include "mce/editor/editor_shell.h"
#include "scene_panels.h"

namespace mce::editor {
namespace {

const ImVec4 kOver(1.0f, 0.45f, 0.4f, 1.0f);
const ImVec4 kOk(0.55f, 0.85f, 0.55f, 1.0f);

class ProfilerPanel final : public IEditorPanel {
public:
    const char* name() const override { return "Profiler"; }

    void draw(EditorShell& shell) override {
        PerfMonitor& perf = shell.perf();
        bool frozen = perf.frozen();
        if (ImGui::Checkbox("Pause", &frozen)) perf.set_frozen(frozen);
        ImGui::SameLine();
        if (ImGui::Button("Clear")) perf.clear();
        ImGui::SameLine();
        const bool playing = shell.playing() && shell.session().play().world();
        if (ImGui::Button("Copy as Markdown")) {
            cook::String md;
            if (playing) PerfMonitor::rank_systems(*shell.session().play().world(), kTopSystems, systems_);
            perf.to_markdown(md, playing ? systems_.span() : Span<const SystemTiming>{});
            ImGui::SetClipboardText(md.c_str());
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(220.0f);
        ImGui::Combo("Graph", &graph_stage_, "frame\0simulation\0editor UI\0cull + sort + record\0RHI translate + submit\0GPU\0");

        draw_graph(perf);
        ImGui::Separator();
        draw_budget(perf);
        ImGui::Separator();
        if (playing) draw_systems(*shell.session().play().world());
        else ImGui::TextDisabled("Play (F5) to rank the scene's systems by cost.");
    }

private:
    static constexpr u32 kTopSystems = 24;

    void draw_graph(const PerfMonitor& perf) {
        const auto stage = static_cast<PerfStage>(graph_stage_);
        perf.series(stage, plot_);
        const PerfSummary s = perf.summarize(stage);
        char overlay[160];
        std::snprintf(overlay, sizeof(overlay), "%s  mean %.2f  p95 %.2f  max %.2f ms   (%u frames over 16.6 ms)",
                      perf_stage_name(stage), static_cast<f64>(s.mean), static_cast<f64>(s.p95), static_cast<f64>(s.max),
                      perf.slow_frames());
        // The vertical scale fits the budget line (16.6 ms for the frame) and any spike above it.
        const f32 top = s.max > PerfMonitor::kFrameBudgetMs ? s.max * 1.1f
                        : stage == PerfStage::Frame        ? PerfMonitor::kFrameBudgetMs * 1.25f
                                                           : (s.max > 0.0f ? s.max * 1.25f : 1.0f);
        const ImVec2 size(ImGui::GetContentRegionAvail().x, 90.0f);
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImGui::PlotLines("##history", plot_.data(), static_cast<int>(plot_.size()), 0, overlay, 0.0f, top, size);
        if (stage == PerfStage::Frame && top > PerfMonitor::kFrameBudgetMs) {
            const f32 y = origin.y + size.y * (1.0f - PerfMonitor::kFrameBudgetMs / top);
            ImGui::GetWindowDrawList()->AddLine(ImVec2(origin.x, y), ImVec2(origin.x + size.x, y),
                                                ImGui::GetColorU32(kOver), 1.0f);
        }
    }

    void draw_budget(const PerfMonitor& perf) {
        perf.budget(rows_);
        if (!ImGui::BeginTable("budget", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) return;
        ImGui::TableSetupColumn("Stage");
        ImGui::TableSetupColumn("Budget");
        ImGui::TableSetupColumn("mean");
        ImGui::TableSetupColumn("p95");
        ImGui::TableSetupColumn("p99");
        ImGui::TableSetupColumn("max");
        ImGui::TableHeadersRow();
        for (const BudgetRow& r : rows_) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r.label);
            ImGui::TableNextColumn();
            ImGui::Text("%.1f", static_cast<f64>(r.budget_ms));
            if (r.measured.samples == 0) {
                ImGui::TableNextColumn();
                ImGui::TextDisabled("n/a");
                continue;
            }
            ImGui::TableNextColumn();
            ImGui::Text("%.2f", static_cast<f64>(r.measured.mean));
            ImGui::TableNextColumn();
            ImGui::TextColored(r.over() ? kOver : kOk, "%.2f", static_cast<f64>(r.measured.p95));
            ImGui::TableNextColumn();
            ImGui::Text("%.2f", static_cast<f64>(r.measured.p99));
            ImGui::TableNextColumn();
            ImGui::Text("%.2f", static_cast<f64>(r.measured.max));
        }
        ImGui::EndTable();
    }

    void draw_systems(const World& world) {
        PerfMonitor::rank_systems(world, kTopSystems, systems_);
        if (systems_.empty()) {
            ImGui::TextDisabled("No system has run yet.");
            return;
        }
        if (!ImGui::BeginTable("systems", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) return;
        ImGui::TableSetupColumn("System");
        ImGui::TableSetupColumn("Phase");
        ImGui::TableSetupColumn("mean ms");
        ImGui::TableSetupColumn("last ms");
        ImGui::TableHeadersRow();
        for (const SystemTiming& s : systems_) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(s.name);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(phase_name(s.phase));
            ImGui::TableNextColumn();
            ImGui::Text("%.3f", s.mean_ms());
            ImGui::TableNextColumn();
            ImGui::Text("%.3f", s.last_ms);
        }
        ImGui::EndTable();
    }

    int graph_stage_ = 0;
    Array<f32> plot_;
    Array<BudgetRow> rows_;
    Array<SystemTiming> systems_;
};

}  // namespace

void add_profiler_panel(EditorShell& shell) { shell.add_panel(mem::make_unique<ProfilerPanel>(mem::heap())); }

}  // namespace mce::editor
