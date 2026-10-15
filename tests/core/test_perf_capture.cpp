// .mcperf captures and budgets (M-PERF steps 1-2): the summary's percentiles, counters and spike causes, the
// JSON text the player writes, and the verdict against a target's budget.

#include <doctest/doctest.h>

#include <cstring>

#include "mce/perf_budget.h"
#include "mce/perf_capture.h"

using namespace mce;

namespace {

perf::Capture make_capture() {
    perf::Capture c;
    c.app = "test";
    c.backend = "headless";
    c.scene = "main \"quoted\"";
    c.sim_hz = 60;
    c.phases.push_back("FixedUpdate_Pre");
    c.phases.push_back("Render_Pre");
    c.render_phases = 1u << 1;
    c.gpu_stages.push_back("shadow");
    c.gpu_stages.push_back("main");
    for (u32 i = 0; i < 100; ++i) {
        perf::Frame f;
        f.index = i;
        f.tick = i + 1;
        f.ticks = 1;
        f.frame_ms = i == 0 ? -1.0f : 10.0f;
        f.sim_ms = 2.0f;
        f.render_ms = 3.0f;
        f.gpu_ms = 4.0f;
        f.phase_ms[0] = 2.0f;
        f.phase_ms[1] = 1.0f;
        f.gpu_stage_ms[0] = 1.0f;
        f.gpu_stage_ms[1] = 3.0f;
        f.counters.draws = 10 + (i % 3);
        f.counters.triangles = 1000;
        c.frames.push_back(f);
    }
    return c;
}

}  // namespace

TEST_CASE("perf: stat_of skips unmeasured values and takes nearest-rank percentiles") {
    const f32 v[] = {-1.0f, 5.0f, 1.0f, 3.0f, 2.0f, 4.0f};
    const perf::Stat s = perf::stat_of(Span<const f32>(v, 6));
    CHECK(s.count == 5);
    CHECK(s.avg == doctest::Approx(3.0f));
    CHECK(s.p50 == 3.0f);
    CHECK(s.max == 5.0f);
    CHECK(perf::stat_of(Span<const f32>()).count == 0);
}

TEST_CASE("perf: a steady capture has no spikes; counters keep their max and mean") {
    const perf::Capture c = make_capture();
    const perf::Summary s = perf::summarize(c);
    CHECK(s.frames == 100);
    CHECK(s.frame.count == 99);  // the first frame has no previous one
    CHECK(s.frame.p95 == 10.0f);
    CHECK(s.gpu.p50 == 4.0f);
    CHECK(s.spikes.empty());
    CHECK(s.counters_max.draws == 12);
    CHECK(s.counters_avg.draws == 10);  // (34 * 10 + 33 * 11 + 33 * 12) / 100 = 10.99, rounded down
    CHECK(s.counters_avg.triangles == 1000);
}

TEST_CASE("perf: spikes name the part and the phase or GPU stage that grew") {
    perf::Capture c = make_capture();
    c.frames[40].frame_ms = 30.0f;  // the sim: a FixedUpdate_Pre system
    c.frames[40].sim_ms = 22.0f;
    c.frames[40].phase_ms[0] = 22.0f;
    c.frames[60].frame_ms = 25.0f;  // the GPU's shadow pass
    c.frames[60].gpu_ms = 19.0f;
    c.frames[60].gpu_stage_ms[0] = 16.0f;
    c.frames[70].frame_ms = 21.0f;  // nothing measured grew: the frame waited
    c.frames[80].frame_ms = 15.0f;  // under twice the median: not a spike
    const perf::Summary s = perf::summarize(c);
    REQUIRE(s.spikes.size() == 3);
    CHECK(s.spikes[0].frame == 40);  // the worst first
    CHECK(s.spikes[0].tick == 41);
    CHECK(s.spikes[0].cause == "sim: FixedUpdate_Pre");
    CHECK(s.spikes[1].frame == 60);
    CHECK(s.spikes[1].cause == "gpu: shadow");
    CHECK(s.spikes[2].frame == 70);
    CHECK(s.spikes[2].cause == "wait");

    // A render spike looks at the render phases only.
    perf::Capture r = make_capture();
    r.frames[10].frame_ms = 40.0f;
    r.frames[10].render_ms = 33.0f;
    r.frames[10].phase_ms[1] = 30.0f;
    r.frames[10].phase_ms[0] = 2.4f;
    const perf::Summary rs = perf::summarize(r);
    REQUIRE(rs.spikes.size() == 1);
    CHECK(rs.spikes[0].cause == "render: Render_Pre");
}

TEST_CASE("perf: write_mcperf writes the header, summary, systems and one line per frame") {
    perf::Capture c = make_capture();
    perf::SystemStats sys;
    sys.name = "movement";
    sys.phase = 0;
    sys.total_ms = 12.5;
    sys.runs = 100;
    sys.max_frame_ms = 0.25f;
    c.systems.push_back(sys);
    Array<char> out;
    perf::write_mcperf(c, perf::summarize(c), out);
    out.push_back('\0');
    const char* text = out.data();
    CHECK(std::strstr(text, "\"format\": \"mce-perf\"") != nullptr);
    CHECK(std::strstr(text, "\"version\": 1") != nullptr);
    CHECK(std::strstr(text, "\"scene\": \"main \\\"quoted\\\"\"") != nullptr);
    CHECK(std::strstr(text, "\"render_phases\": [\"Render_Pre\"]") != nullptr);
    CHECK(std::strstr(text, "\"gpu_stages\": [\"shadow\", \"main\"]") != nullptr);
    CHECK(std::strstr(text, "\"frame_ms\": {\"count\": 99, \"avg\": 10.000, \"p50\": 10.000") != nullptr);
    CHECK(std::strstr(text, "{\"name\": \"movement\", \"phase\": \"FixedUpdate_Pre\", \"runs\": 100, \"total_ms\": 12.500, "
                            "\"max_frame_ms\": 0.250}") != nullptr);
    CHECK(std::strstr(text, "{\"index\": 0, \"tick\": 1, \"ticks\": 1, \"frame_ms\": -1, \"sim_ms\": 2.000") != nullptr);
    CHECK(std::strstr(text, "\"gpu_stage_ms\": [1.000, 3.000]") != nullptr);
    usize lines = 0;
    for (const char* p = std::strstr(text, "{\"index\": "); p; p = std::strstr(p + 1, "{\"index\": ")) ++lines;
    CHECK(lines == 100);
}

TEST_CASE("perf: targets and budget fields by name") {
    perf::Target t = perf::Target::Desktop;
    CHECK(perf::parse_target("mobile", t));
    CHECK(t == perf::Target::Mobile);
    CHECK_FALSE(perf::parse_target("console", t));
    CHECK(StringView(perf::target_name(perf::Target::Web)) == "web");
    const perf::BudgetField* f = perf::find_budget_field("draws");
    REQUIRE(f != nullptr);
    CHECK(f->integer);
    perf::Budget b = perf::default_budget(perf::Target::Mobile);
    CHECK(f->get(b) == 400.0);
    f->set(b, 250.0);
    CHECK(b.draws == 250);
    CHECK_FALSE(perf::find_budget_field("frame_ms")->integer);
    CHECK(perf::find_budget_field("fps") == nullptr);
    // Phones get the smaller numbers.
    const perf::Budget d = perf::default_budget(perf::Target::Desktop);
    CHECK(b.frame_ms > d.frame_ms);
    CHECK(perf::default_budget(perf::Target::Mobile).draws < d.draws);
    CHECK(perf::default_budget(perf::Target::Mobile).texture_size < d.texture_size);
}

TEST_CASE("perf: judge passes a capture inside the budget and names what is over") {
    perf::Capture c = make_capture();  // p95 frame 10 ms, sim 2, render 3, gpu 4; max 12 draws, 1000 triangles
    perf::Budget b;
    b.frame_ms = 16.7f;
    b.gpu_ms = 3.0f;      // over: 4 ms
    b.draws = 12;         // at the limit: passes
    b.triangles = 500;    // over: 1000
    perf::Verdict v = perf::judge(perf::summarize(c), b);
    CHECK_FALSE(v.pass);
    REQUIRE(v.lines.size() == 4);  // the unlimited fields have no line
    CHECK(StringView(v.lines[0].metric) == "frame_ms");
    CHECK(StringView(v.lines[0].stat) == "p95");
    CHECK_FALSE(v.lines[0].over);
    CHECK(StringView(v.lines[1].metric) == "gpu_ms");
    CHECK(v.lines[1].over);
    CHECK(v.lines[1].measured == doctest::Approx(4.0));
    CHECK(StringView(v.lines[2].metric) == "draws");
    CHECK(StringView(v.lines[2].stat) == "max");
    CHECK_FALSE(v.lines[2].over);
    CHECK(StringView(v.lines[3].metric) == "triangles");
    CHECK(v.lines[3].over);

    // A headless capture has no GPU time: the GPU limit has no line rather than a pass.
    for (perf::Frame& f : c.frames) f.gpu_ms = -1.0f;
    b.triangles = 0;
    v = perf::judge(perf::summarize(c), b);
    CHECK(v.pass);
    for (const perf::BudgetLine& l : v.lines) CHECK(StringView(l.metric) != "gpu_ms");
}
