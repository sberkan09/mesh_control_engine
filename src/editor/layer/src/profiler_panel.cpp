// The Profiler panel (ROADMAP M7 editor perf dashboards, FR-EDT-140): the editor's frame-time
// history against the 16.6 ms budget, the NFR-F-02 stage table (p95 against each allocation), and
// while playing, the play world's systems ranked by cost. Pause freezes the history so a spike
// stays on screen; Copy puts the report on the clipboard as Markdown (the milestone docs' shape).
//
// The Capture tab (M-PERF step 4) opens a .mcperf file the player or MCP's perf_run wrote: the
// verdict against a target's budget, a strip of every frame (spikes red, frames over the budget
// amber; click one), the chosen frame taken apart (sim and render phases, GPU stages, counters),
// the spikes and the slowest systems, and, with a second capture, the two compared.

#include <imgui.h>

#include <algorithm>
#include <cstdio>

#include "mce/editor/editor_shell.h"
#include "mce/editor/perf_compare.h"
#include "mce/editor/perf_file.h"
#include "mce/editor/project_settings.h"
#include "mce/log.h"
#include "mce/perf_budget.h"
#include "scene_panels.h"

namespace mce::editor {
namespace {

const ImVec4 kOver(1.0f, 0.45f, 0.4f, 1.0f);
const ImVec4 kOk(0.55f, 0.85f, 0.55f, 1.0f);
const ImVec4 kWarn(0.95f, 0.75f, 0.3f, 1.0f);

class ProfilerPanel final : public IEditorPanel {
public:
    const char* name() const override { return "Profiler"; }

    void draw(EditorShell& shell) override {
        FixedString<256> requested;
        const bool request = shell.take_perf_capture(requested);
        if (request) {
            std::snprintf(path_, sizeof(path_), "%s", requested.c_str());
            open_capture(shell);
        }
        if (!ImGui::BeginTabBar("profiler_tabs")) return;
        if (ImGui::BeginTabItem("Live")) {
            draw_live(shell);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Capture", nullptr, request ? ImGuiTabItemFlags_SetSelected : 0)) {
            draw_capture(shell);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

private:
    static constexpr u32 kTopSystems = 24;

    void draw_live(EditorShell& shell) {
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

    // --- Capture tab -------------------------------------------------------------------------

    // Reads `rel` (relative to the project folder, or absolute) into `out`; the error in status_.
    bool load(EditorShell& shell, const char* rel, perf::Capture& out) {
        cook::String path(rel);
        const StringView root = shell.session().project().root();
        if (!root.empty() && rel[0] != '/' && rel[0] != '\\' && !(rel[0] && rel[1] == ':')) path = cook::path_join(root, StringView(rel));
        cook::String err;
        out = perf::Capture{};
        if (!load_mcperf(path.view(), out, err)) {
            status_ = rel;
            status_ += ": ";
            status_ += err.view();
            return false;
        }
        status_.clear();
        return true;
    }

    void open_capture(EditorShell& shell) {
        has_capture_ = load(shell, path_, capture_);
        has_compare_ = false;
        selected_ = 0;
        if (!has_capture_) {
            MCE_LOG_WARN("Profiler: {}", status_.c_str());
            return;
        }
        summary_ = perf::summarize(capture_);
        rank_capture_systems(capture_, order_);
        if (!summary_.spikes.empty()) selected_ = position_of(summary_.spikes[0].frame);
        rejudge(shell);
        MCE_LOG_INFO("Profiler: opened {} ({} frames, {} systems, {} spikes)", path_, capture_.frames.size(), capture_.systems.size(),
                     summary_.spikes.size());
    }

    void rejudge(EditorShell& shell) {
        budget_ = perf::default_budget(static_cast<perf::Target>(target_));
        cook::String err;
        if (shell.session().project().is_open() &&
            !project_budget(shell.session().project(), static_cast<perf::Target>(target_), budget_, err))
            status_ = err;
        verdict_ = perf::judge(summary_, budget_);
    }

    void draw_capture(EditorShell& shell) {
        ImGui::SetNextItemWidth(320.0f);
        ImGui::InputText("##capture", path_, sizeof(path_));
        ImGui::SameLine();
        if (ImGui::Button("Open")) open_capture(shell);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100.0f);
        if (ImGui::Combo("Target", &target_, "desktop\0mobile\0web\0") && has_capture_) rejudge(shell);
        ImGui::SetNextItemWidth(320.0f);
        ImGui::InputText("##before", before_path_, sizeof(before_path_));
        ImGui::SameLine();
        if (ImGui::Button("Compare with") && has_capture_) {
            perf::Capture before;
            has_compare_ = load(shell, before_path_, before);
            if (has_compare_) compare_captures(before, capture_, compare_);
        }
        if (!status_.empty()) ImGui::TextColored(kOver, "%s", status_.c_str());
        if (!has_capture_) {
            ImGui::TextDisabled("Open a .mcperf capture: MCP perf_run writes cooked/.perf/last.mcperf,");
            ImGui::TextDisabled("and mce_player -perf_capture <file> writes one from any run.");
            return;
        }
        ImGui::Separator();
        draw_verdict();
        draw_strip();
        ImGui::Separator();
        if (ImGui::BeginTable("capture_cols", 2, ImGuiTableFlags_Resizable)) {
            ImGui::TableNextColumn();
            draw_frame();
            ImGui::TableNextColumn();
            draw_spikes();
            draw_capture_systems();
            ImGui::EndTable();
        }
        if (has_compare_) {
            ImGui::Separator();
            draw_compare();
        }
    }

    void draw_verdict() {
        ImGui::Text("%s  %s  %ux%u  %u frames", capture_.scene.c_str(), capture_.backend.c_str(), capture_.width, capture_.height,
                    summary_.frames);
        ImGui::SameLine();
        ImGui::TextColored(verdict_.pass ? kOk : kOver, verdict_.pass ? "  within the %s budget" : "  over the %s budget",
                           perf::target_name(static_cast<perf::Target>(target_)));
        for (const perf::BudgetLine& l : verdict_.lines) {
            if (!l.over) continue;
            ImGui::TextColored(kOver, "  %s %s %.3g > %.3g", l.metric, l.stat, l.measured, l.limit);
        }
        const perf::Stat* stats[4] = {&summary_.frame, &summary_.sim, &summary_.render, &summary_.gpu};
        const char* names[4] = {"frame", "sim", "render", "gpu"};
        for (usize i = 0; i < 4; ++i) {
            if (i) ImGui::SameLine();
            if (stats[i]->count == 0) ImGui::TextDisabled("%s n/a   ", names[i]);
            else ImGui::Text("%s p50 %.2f p95 %.2f   ", names[i], static_cast<f64>(stats[i]->p50), static_cast<f64>(stats[i]->p95));
        }
    }

    // The frame's place in the capture (frames are numbered from the app's start, after the warm-up).
    usize position_of(u64 frame) const {
        for (usize i = 0; i < capture_.frames.size(); ++i)
            if (capture_.frames[i].index == frame) return i;
        return 0;
    }

    bool is_spike(u64 frame) const {
        for (const perf::Spike& s : summary_.spikes)
            if (s.frame == frame) return true;
        return false;
    }

    // Every frame as a bar, budget line across; clicking a bar picks the frame.
    void draw_strip() {
        const usize n = capture_.frames.size();
        if (n == 0) return;
        f32 top = budget_.frame_ms;
        for (const perf::Frame& f : capture_.frames) top = std::max(top, f.frame_ms);
        top *= 1.1f;
        const ImVec2 size(std::max(ImGui::GetContentRegionAvail().x, 1.0f), 80.0f);
        const ImVec2 o = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##strip", size);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(o, ImVec2(o.x + size.x, o.y + size.y), ImGui::GetColorU32(ImGuiCol_FrameBg));
        const f32 bw = size.x / static_cast<f32>(n);
        for (usize i = 0; i < n; ++i) {
            const perf::Frame& f = capture_.frames[i];
            if (f.frame_ms <= 0.0f) continue;
            const ImVec4 col = is_spike(f.index) ? kOver : f.frame_ms > budget_.frame_ms ? kWarn : kOk;
            const f32 x = o.x + bw * static_cast<f32>(i);
            const f32 y = o.y + size.y * (1.0f - f.frame_ms / top);
            dl->AddRectFilled(ImVec2(x, y), ImVec2(x + std::max(bw - 1.0f, 1.0f), o.y + size.y), ImGui::GetColorU32(col));
        }
        if (budget_.frame_ms > 0.0f) {
            const f32 y = o.y + size.y * (1.0f - budget_.frame_ms / top);
            dl->AddLine(ImVec2(o.x, y), ImVec2(o.x + size.x, y), ImGui::GetColorU32(kOver), 1.0f);
        }
        if (selected_ < n) {
            const f32 x = o.x + bw * (static_cast<f32>(selected_) + 0.5f);
            dl->AddLine(ImVec2(x, o.y), ImVec2(x, o.y + size.y), ImGui::GetColorU32(ImGuiCol_Text), 1.0f);
        }
        if (ImGui::IsItemHovered()) {
            const usize i = std::min(n - 1, static_cast<usize>(std::max(0.0f, (ImGui::GetIO().MousePos.x - o.x) / bw)));
            const perf::Frame& f = capture_.frames[i];
            ImGui::SetTooltip("frame %llu  %.2f ms  draws %u", static_cast<unsigned long long>(f.index), static_cast<f64>(f.frame_ms),
                              f.counters.draws);
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) selected_ = i;
        }
    }

    void draw_frame() {
        if (selected_ >= capture_.frames.size()) return;
        const perf::Frame& f = capture_.frames[selected_];
        ImGui::Text("Frame %llu (tick %llu, %u ticks)", static_cast<unsigned long long>(f.index), static_cast<unsigned long long>(f.tick),
                    f.ticks);
        ImGui::Text("frame %.2f  sim %.2f  render %.2f ms", static_cast<f64>(f.frame_ms), static_cast<f64>(f.sim_ms),
                    static_cast<f64>(f.render_ms));
        if (f.gpu_ms >= 0.0f) ImGui::Text("gpu %.2f ms", static_cast<f64>(f.gpu_ms));
        frame_parts(capture_, f, parts_);
        if (!parts_.empty() && ImGui::BeginTable("parts", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableSetupColumn("Part");
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("ms");
            ImGui::TableHeadersRow();
            const char* kinds[3] = {"sim", "render", "gpu"};
            for (const FramePart& p : parts_) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(kinds[p.kind]);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(p.name.data(), p.name.data() + p.name.size());
                ImGui::TableNextColumn();
                ImGui::Text("%.3f", static_cast<f64>(p.ms));
            }
            ImGui::EndTable();
        }
        const perf::Counters& k = f.counters;
        ImGui::Text("draws %u  passes %u  triangles %u  sprites %u (%u batches)", k.draws, k.passes, k.triangles, k.sprites,
                    k.sprite_batches);
        ImGui::Text("upload %llu bytes  heap allocations %llu", static_cast<unsigned long long>(k.upload_bytes),
                    static_cast<unsigned long long>(k.heap_allocations));
    }

    void draw_spikes() {
        if (summary_.spikes.empty()) {
            ImGui::TextDisabled("No spikes.");
            return;
        }
        ImGui::TextUnformatted("Spikes");
        for (const perf::Spike& s : summary_.spikes) {
            char label[128];
            std::snprintf(label, sizeof(label), "frame %llu  %.2f ms  %s", static_cast<unsigned long long>(s.frame),
                          static_cast<f64>(s.frame_ms), s.cause.c_str());
            if (ImGui::Selectable(label, selected_ == position_of(s.frame))) selected_ = position_of(s.frame);
        }
    }

    void draw_capture_systems() {
        if (order_.empty()) return;
        if (!ImGui::BeginTable("capture_systems", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) return;
        ImGui::TableSetupColumn("System");
        ImGui::TableSetupColumn("Phase");
        ImGui::TableSetupColumn("ms / frame");
        ImGui::TableHeadersRow();
        for (usize i = 0; i < order_.size() && i < kTopSystems; ++i) {
            const perf::SystemStats& s = capture_.systems[order_[i]];
            const StringView phase = perf_phase_name(capture_, s.phase);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(s.name.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(phase.data(), phase.data() + phase.size());
            ImGui::TableNextColumn();
            ImGui::Text("%.3f", system_ms_per_frame(capture_, s));
        }
        ImGui::EndTable();
    }

    static bool listed(const Array<const char*>& list, const char* metric) {
        for (const char* m : list)
            if (m == metric) return true;
        return false;
    }

    const ImVec4& change_color(const char* metric) const {
        static const ImVec4 kSame(0.7f, 0.7f, 0.7f, 1.0f);
        return listed(compare_.better, metric) ? kOk : listed(compare_.worse, metric) ? kOver : kSame;
    }

    void draw_compare() {
        ImGui::Text("Compared with %s%s", before_path_, compare_.same_scene ? "" : " (a different scene)");
        if (ImGui::BeginTable("cmp_times", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableSetupColumn("Time");
            ImGui::TableSetupColumn("p95 before");
            ImGui::TableSetupColumn("p95 after");
            ImGui::TableSetupColumn("change");
            ImGui::TableHeadersRow();
            for (const TimeChange& t : compare_.times) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(t.metric);
                if (!t.measured) {
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("n/a");
                    continue;
                }
                ImGui::TableNextColumn();
                ImGui::Text("%.2f", static_cast<f64>(t.p95_before));
                ImGui::TableNextColumn();
                ImGui::Text("%.2f", static_cast<f64>(t.p95_after));
                ImGui::TableNextColumn();
                ImGui::TextColored(change_color(t.metric), "%+.1f%%", t.p95_change_pct);
            }
            ImGui::EndTable();
        }
        if (ImGui::BeginTable("cmp_counters", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableSetupColumn("Counter (worst frame)");
            ImGui::TableSetupColumn("before");
            ImGui::TableSetupColumn("after");
            ImGui::TableSetupColumn("change");
            ImGui::TableHeadersRow();
            for (const CounterChange& k : compare_.counters) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(k.metric);
                ImGui::TableNextColumn();
                ImGui::Text("%llu", static_cast<unsigned long long>(k.before));
                ImGui::TableNextColumn();
                ImGui::Text("%llu", static_cast<unsigned long long>(k.after));
                ImGui::TableNextColumn();
                ImGui::TextColored(change_color(k.metric), "%+lld", static_cast<long long>(k.after) - static_cast<long long>(k.before));
            }
            ImGui::EndTable();
        }
        if (compare_.systems.empty() || !ImGui::BeginTable("cmp_systems", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) return;
        ImGui::TableSetupColumn("System (moved most)");
        ImGui::TableSetupColumn("Phase");
        ImGui::TableSetupColumn("ms / frame before");
        ImGui::TableSetupColumn("after");
        ImGui::TableHeadersRow();
        for (usize i = 0; i < compare_.systems.size() && i < kTopSystems; ++i) {
            const SystemChange& r = compare_.systems[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r.name.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r.phase.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%.3f", r.before_ms);
            ImGui::TableNextColumn();
            ImGui::TextColored(r.after_ms > r.before_ms ? kOver : kOk, "%.3f", r.after_ms);
        }
        ImGui::EndTable();
    }

    int graph_stage_ = 0;
    Array<f32> plot_;
    Array<BudgetRow> rows_;
    Array<SystemTiming> systems_;

    char path_[256] = "cooked/.perf/last.mcperf";
    char before_path_[256] = "cooked/.perf/previous.mcperf";
    int target_ = 0;
    bool has_capture_ = false, has_compare_ = false;
    perf::Capture capture_;
    perf::Summary summary_;
    perf::Budget budget_;
    perf::Verdict verdict_;
    Array<usize> order_;
    Array<FramePart> parts_;
    usize selected_ = 0;
    Comparison compare_;
    cook::String status_;
};

}  // namespace

void add_profiler_panel(EditorShell& shell) { shell.add_panel(mem::make_unique<ProfilerPanel>(mem::heap())); }

}  // namespace mce::editor
