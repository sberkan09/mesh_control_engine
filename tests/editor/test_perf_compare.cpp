// M-PERF step 4, headless: the model behind the Profiler panel's capture view and MCP's
// perf_compare. Two captures compared (times by p95 with a noise floor, counters on any change,
// systems matched by name and phase), one frame taken apart, and a capture read back from its file.

#include <doctest/doctest.h>

#include <cstring>

#include "mce/editor/perf_compare.h"
#include "mce/editor/perf_file.h"

using namespace mce;
using namespace mce::editor;

namespace {

perf::Capture capture(f32 frame_ms, u32 draws) {
    perf::Capture c;
    c.scene = "main";
    c.phases.push_back("FixedUpdate");
    c.phases.push_back("Render_Pre");
    c.phases.push_back("Late");
    c.render_phases = 1u << 1;
    c.gpu_stages.push_back("shadow");
    c.gpu_stages.push_back("main");
    for (u32 i = 0; i < 20; ++i) {
        perf::Frame f;
        f.index = 30 + i;  // after a warm-up
        f.tick = f.index + 1;
        f.ticks = 1;
        f.frame_ms = frame_ms;
        f.sim_ms = 1.0f;
        f.render_ms = 2.0f;
        f.counters.draws = draws;
        f.counters.triangles = 100;
        c.frames.push_back(f);
    }
    perf::SystemStats s;
    s.name = "movement";
    s.phase = 0;
    s.total_ms = 20.0;
    c.systems.push_back(s);
    return c;
}

bool has(const Array<const char*>& list, const char* m) {
    for (const char* x : list)
        if (std::strcmp(x, m) == 0) return true;
    return false;
}

}  // namespace

TEST_CASE("perf compare: times past the noise floor, counters on any change, systems by name") {
    perf::Capture a = capture(10.0f, 40), b = capture(10.5f, 25);  // +5% frame time: noise
    perf::SystemStats extra;
    extra.name = "spawner";
    extra.phase = 2;
    extra.total_ms = 40.0;
    b.systems.push_back(extra);
    b.systems[0].total_ms = 10.0;

    Comparison c;
    compare_captures(a, b, c);
    CHECK(c.same_scene);
    CHECK(StringView(c.times[0].metric) == "frame_ms");
    CHECK(c.times[0].measured);
    CHECK(c.times[0].p95_change_pct == doctest::Approx(5.0));
    CHECK_FALSE(c.times[3].measured);  // no GPU time in either
    CHECK_FALSE(has(c.worse, "frame_ms"));
    CHECK(has(c.better, "draws"));
    CHECK_FALSE(has(c.better, "triangles"));
    REQUIRE(c.counters.size() == 8);
    CHECK(c.counters[0].before == 40);
    CHECK(c.counters[0].after == 25);
    // spawner is new (0 -> 2 ms per frame), movement went 1 -> 0.5: the larger move first.
    REQUIRE(c.systems.size() == 2);
    CHECK(c.systems[0].name.view() == "spawner");
    CHECK(c.systems[0].phase.view() == "Late");
    CHECK(c.systems[0].before_ms == 0.0);
    CHECK(c.systems[0].after_ms == doctest::Approx(2.0));
    CHECK(c.systems[1].before_ms == doctest::Approx(1.0));
    CHECK(c.systems[1].after_ms == doctest::Approx(0.5));

    // Twice the frame time is worse; a different scene is flagged.
    perf::Capture slow = capture(20.0f, 40);
    slow.scene = "other";
    compare_captures(a, slow, c);
    CHECK(has(c.worse, "frame_ms"));
    CHECK(c.times[0].p95_change_pct == doctest::Approx(100.0));
    CHECK_FALSE(c.same_scene);
    CHECK(c.better.empty());
}

TEST_CASE("perf compare: a frame's parts, sim then render then GPU, slowest first") {
    perf::Capture c = capture(10.0f, 4);
    perf::Frame& f = c.frames[3];
    f.phase_ms[0] = 0.5f;  // sim
    f.phase_ms[1] = 1.5f;  // render
    f.phase_ms[2] = 0.8f;  // sim
    f.gpu_stage_ms[0] = 2.0f;
    f.gpu_stage_ms[1] = 3.0f;
    Array<FramePart> parts;
    frame_parts(c, f, parts);
    REQUIRE(parts.size() == 5);
    CHECK(parts[0].kind == FramePart::Sim);
    CHECK(parts[0].name == "Late");
    CHECK(parts[1].name == "FixedUpdate");
    CHECK(parts[2].kind == FramePart::Render);
    CHECK(parts[2].name == "Render_Pre");
    CHECK(parts[3].kind == FramePart::Gpu);
    CHECK(parts[3].name == "main");
    CHECK(parts[4].name == "shadow");
    // Unmeasured or idle parts are left out.
    frame_parts(c, c.frames[0], parts);
    CHECK(parts.empty());

    Array<usize> order;
    perf::SystemStats s;
    s.name = "big";
    s.total_ms = 50.0;
    c.systems.push_back(s);
    rank_capture_systems(c, order);
    REQUIRE(order.size() == 2);
    CHECK(order[0] == 1);
    CHECK(system_ms_per_frame(c, c.systems[1]) == doctest::Approx(2.5));
    CHECK(perf_phase_name(c, 9) == "?");
}

TEST_CASE("perf compare: a capture written and read back compares equal to itself") {
    perf::Capture a = capture(12.0f, 7);
    a.frames[5].phase_ms[0] = 0.25f;
    a.frames[5].counters.runs_instanced = 3;  // M-VISUAL3 5a
    a.frames[5].counters.draws_instanced = 40;
    a.frames[5].counters.draw_causes[static_cast<u32>(DrawCause::Material)] = 7;  // M-CONTENT 2
    a.frames[5].counters.draw_causes[static_cast<u32>(DrawCause::Budget)] = 2;
    a.frames[6].counters.draw_causes[static_cast<u32>(DrawCause::Material)] = 3;
    a.frames[5].counters.merged_groups = 2;  // M-VISUAL3 5b
    a.frames[5].counters.merged_children = 31;
    Array<char> text;
    perf::write_mcperf(a, perf::summarize(a), text);
    perf::Capture b;
    cook::String err;
    REQUIRE_MESSAGE(read_mcperf(StringView(text.data(), text.size()), b, err), err.c_str());
    CHECK(b.frames.size() == a.frames.size());
    CHECK(b.frames[5].index == 35);
    CHECK(b.frames[5].counters.runs_instanced == 3);
    CHECK(b.frames[5].counters.draws_instanced == 40);
    CHECK(b.frames[5].counters.merged_groups == 2);
    CHECK(b.frames[5].counters.merged_children == 31);
    CHECK(perf::summarize(b).counters_max.draws_instanced == 40);
    CHECK(b.frames[5].counters.draw_causes[static_cast<u32>(DrawCause::Material)] == 7);
    CHECK(b.frames[5].counters.draw_causes[static_cast<u32>(DrawCause::Budget)] == 2);
    CHECK(b.frames[5].counters.draw_causes[static_cast<u32>(DrawCause::Skinned)] == 0);
    CHECK(perf::summarize(b).counters_max.draw_causes[static_cast<u32>(DrawCause::Material)] == 7);
    CHECK(perf::summarize(b).counters_avg.draw_causes[static_cast<u32>(DrawCause::Material)] == static_cast<u32>(10 / a.frames.size()));  // the mean, rounded down
    CHECK(StringView(text.data(), text.size()).find("\"draw_causes\": [\"material\", \"unique_mesh\"") != StringView::npos);
    CHECK(StringView(text.data(), text.size()).find("\"draw_cause_hints\"") != StringView::npos);
    // A file from before the report has no names and no counts: they read as zero.
    {
        perf::Capture old;
        cook::String err2;
        const char* doc = "{\"format\": \"mce-perf\", \"version\": 1, \"frames\": [{\"index\": 0, \"counters\": {\"draws\": 5}}]}";
        REQUIRE_MESSAGE(read_mcperf(doc, old, err2), err2.c_str());
        CHECK(old.frames[0].counters.draws == 5);
        CHECK(old.frames[0].counters.draw_causes[0] == 0);
    }
    // An old capture against a new one: the causes and instanced counts are not compared (no counter row, no verdict).
    {
        perf::Capture old = capture(12.0f, 7);
        Comparison oc;
        compare_captures(old, a, oc);
        for (const auto& k : oc.counters) {
            CHECK(StringView(k.metric) != StringView("draw_causes"));
            CHECK(StringView(k.metric) != StringView("draws_instanced"));
        }
        CHECK(oc.better.empty());
        CHECK(oc.worse.empty());
    }
    Comparison c;
    compare_captures(a, b, c);
    CHECK(c.better.empty());
    CHECK(c.worse.empty());
    Array<FramePart> parts;
    frame_parts(b, b.frames[5], parts);
    REQUIRE(parts.size() == 1);
    CHECK(parts[0].ms == doctest::Approx(0.25f));
}
