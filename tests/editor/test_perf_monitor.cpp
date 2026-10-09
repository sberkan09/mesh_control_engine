// Editor perf dashboards (ROADMAP M7, FR-EDT-140), headless: the Profiler's frame history and its
// summaries, the NFR-F-02 budget table, the play world's per-system ranking and the Markdown report.

#include <doctest/doctest.h>

#include <filesystem>
#include <string>

#include "cook_test_util.h"
#include "mce/editor/editor_session.h"
#include "mce/editor/perf_monitor.h"

using namespace mce;
using namespace mce::editor;
using namespace cook_test;

namespace {

FrameSample frame(f32 ms) {
    FrameSample s;
    s.frame_ms = ms;
    s.ui_ms = ms * 0.1f;
    s.record_ms = 0.5f;
    s.submit_ms = 1.0f;
    return s;  // sim 0 (not playing), GPU unmeasured
}

bool contains(const cook::String& s, const char* what) {
    return std::string(s.c_str()).find(what) != std::string::npos;
}

void spin(Frame&) {
    volatile u32 x = 0;
    for (u32 i = 0; i < 20000; ++i) x = x + i;
}
void idle(Frame&) {}

}  // namespace

TEST_CASE("perf monitor: rolling history, oldest first, frozen while paused") {
    PerfMonitor m;
    CHECK(m.size() == 0);
    CHECK(m.summarize(PerfStage::Frame).samples == 0);
    for (u32 i = 0; i < PerfMonitor::kCapacity + 20; ++i) m.push(frame(static_cast<f32>(i)));
    CHECK(m.size() == PerfMonitor::kCapacity);
    CHECK(m.frames_seen() == PerfMonitor::kCapacity + 20);
    CHECK(m.at(0).frame_ms == 20.0f);  // the first 20 rolled out
    CHECK(m.at(m.size() - 1).frame_ms == static_cast<f32>(PerfMonitor::kCapacity + 19));
    m.set_frozen(true);
    m.push(frame(1000.0f));
    CHECK(m.at(m.size() - 1).frame_ms == static_cast<f32>(PerfMonitor::kCapacity + 19));
    m.set_frozen(false);
    m.clear();
    CHECK(m.size() == 0);
}

TEST_CASE("perf monitor: mean and nearest-rank percentiles; unmeasured stages are left out") {
    PerfMonitor m;
    for (u32 i = 1; i <= 100; ++i) m.push(frame(static_cast<f32>(i)));  // 1..100 ms
    const PerfSummary f = m.summarize(PerfStage::Frame);
    CHECK(f.samples == 100);
    CHECK(f.mean == doctest::Approx(50.5f));
    CHECK(f.p50 == 50.0f);
    CHECK(f.p95 == 95.0f);
    CHECK(f.p99 == 99.0f);
    CHECK(f.max == 100.0f);
    CHECK(m.summarize(PerfStage::Gpu).samples == 0);   // -1: never measured
    CHECK(m.summarize(PerfStage::Sim).samples == 100);  // 0 is a measurement (nothing ran)
    CHECK(m.slow_frames() == 84);                       // 17..100 ms are over 16.6
    Array<f32> plot;
    m.series(PerfStage::Gpu, plot);
    CHECK(plot.size() == 100);
    CHECK(plot[0] == 0.0f);  // unmeasured plots as 0
}

TEST_CASE("perf monitor: the NFR-F-02 budget table flags the stages over their allocation") {
    PerfMonitor m;
    for (u32 i = 0; i < 60; ++i) {
        FrameSample s = frame(12.0f);
        s.sim_ms = 4.0f;  // over the 3.0 ms simulation allocation
        s.gpu_ms = 5.0f;
        m.push(s);
    }
    Array<BudgetRow> rows;
    m.budget(rows);
    REQUIRE(rows.size() == 6);
    CHECK(rows[0].stage == PerfStage::Frame);
    CHECK(rows[0].budget_ms == doctest::Approx(16.6f));
    CHECK_FALSE(rows[0].over());
    CHECK(rows[1].stage == PerfStage::Sim);
    CHECK(rows[1].over());
    CHECK(rows[2].stage == PerfStage::Record);
    CHECK_FALSE(rows[2].over());
    CHECK(rows[4].stage == PerfStage::Gpu);
    CHECK_FALSE(rows[4].over());
    cook::String md;
    m.to_markdown(md);
    CHECK(contains(md, "| simulation | 3.0 | 4.00 | 4.00 | 4.00 (over) | 4.00 | 4.00 |"));
    CHECK(contains(md, "| GPU | 6.0 | 5.00 |"));
    CHECK(contains(md, "0 over 16.6 ms"));
}

TEST_CASE("perf monitor: systems ranked by mean cost, unrun systems left out") {
    World w;
    w.add_system(Phase::FixedUpdate_Pre, "idle", &idle);
    w.add_system(Phase::FixedUpdate_Physics, "spin", &spin);
    w.add_system(Phase::Render_Pre, "never", &idle);  // ticks never run render phases
    w.set_profiling(true);
    for (u32 t = 0; t < 5; ++t) w.tick(1.0 / 60.0, t);
    Array<SystemTiming> top;
    PerfMonitor::rank_systems(w, 8, top);
    REQUIRE(top.size() == 2);
    CHECK(StringView(top[0].name) == StringView("spin"));
    CHECK(top[0].runs == 5);
    CHECK(top[0].mean_ms() >= top[1].mean_ms());
    PerfMonitor::rank_systems(w, 1, top);
    CHECK(top.size() == 1);
    PerfMonitor m;
    m.push(frame(10.0f));
    cook::String md;
    m.to_markdown(md, top.span());
    CHECK(contains(md, "| spin | FixedUpdate_Physics |"));
}

TEST_CASE("perf monitor: play-in-editor profiles the play world without changing its run") {
    const std::string root = fresh_dir("perf_play");
    std::filesystem::copy(std::string(MCE_SOURCE_DIR) + "/samples/scene_demo/assets", root + "/assets",
                          std::filesystem::copy_options::recursive);
    EditorSession session;
    REQUIRE(session.open_project(StringView(root.data(), root.size())));
    REQUIRE(session.open_scene("scenes/demo.mcscene"));
    REQUIRE(session.enter_play());
    PlaySession& play = session.play();
    CHECK(play.world()->profiling());
    CHECK(play.advance(2.0 / 60.0) == 2);
    CHECK(play.last_advance_ticks() == 2);
    CHECK(play.last_advance_ms() > 0.0);
    Array<SystemTiming> top;
    PerfMonitor::rank_systems(*play.world(), 64, top);
    CHECK_FALSE(top.empty());
    for (const SystemTiming& s : top) CHECK(s.runs > 0);
    play.pause();
    CHECK(play.advance(1.0) == 0);
    CHECK(play.last_advance_ms() == 0.0);
    CHECK(play.last_advance_ticks() == 0);
}
