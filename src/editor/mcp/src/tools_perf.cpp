// M-PERF step 3: perf_run and perf_compare. perf_run cooks, runs the real player on a scene with
// -perf_capture (one sim tick per frame, so the frames and their counters repeat), reads the .mcperf
// back and reports it against a target's budget: the verdict, where the frame goes (the slowest
// systems, the GPU stages), the spikes, and what to try for each limit it is over. perf_compare
// puts two captures side by side. Captures stay in the project folder (cooked/.perf/ by default),
// so an agent can measure, change something, measure again and compare.

#include <algorithm>
#include <cmath>

#include "mce/cook/fs.h"
#include "mce/editor/perf_compare.h"
#include "mce/editor/perf_file.h"
#include "mce/editor/project_settings.h"
#include "mce/perf_budget.h"
#include "tool_args.h"
#include "tools_agent.h"

namespace mce::mcp {

namespace {

constexpr u64 kMaxFrames = 3600;
constexpr u64 kMaxSide = 3840;

f64 round3(f64 v) { return std::round(v * 1000.0) / 1000.0; }

// A path inside the project folder, relative and without "..": its absolute form.
bool project_file(ToolCall& c, const char* name, StringView rel, cook::String& abs) {
    if (rel.empty() || rel[0] == '/' || rel[0] == '\\' || rel.find("..") != StringView::npos || rel.find(':') != StringView::npos)
        return arg_missing(c, name, "a relative path inside the project folder, e.g. perf/before.mcperf");
    abs = cook::path_join(c.server.session().project().root(), rel);
    return true;
}

void write_stat(JsonWriter& w, const char* name, const perf::Stat& s) {
    w.key(name).begin_object();
    if (s.count == 0) {
        w.key("measured").boolean(false).end_object();
        return;
    }
    w.key("avg").num_f(round3(s.avg)).key("p50").num_f(round3(s.p50)).key("p95").num_f(round3(s.p95));
    w.key("p99").num_f(round3(s.p99)).key("max").num_f(round3(s.max)).end_object();
}

void write_counters(JsonWriter& w, const char* name, const perf::Counters& k) {
    w.key(name).begin_object();
    w.key("draws").num_u(k.draws).key("passes").num_u(k.passes).key("dispatches").num_u(k.dispatches);
    w.key("triangles").num_u(k.triangles).key("meshes").num_u(k.meshes).key("sprites").num_u(k.sprites);
    w.key("sprite_batches").num_u(k.sprite_batches).key("upload_bytes").num_u(k.upload_bytes);
    w.key("heap_allocations").num_u(k.heap_allocations);
    w.key("runs_instanced").num_u(k.runs_instanced).key("draws_instanced").num_u(k.draws_instanced).end_object();
}

// What to try when a limit is over: the next tool or change, in a sentence.
const char* advice(StringView metric) {
    if (metric == "sim_ms") return "the sim is slow: top_systems names the systems; cheaper systems or fewer entities";
    if (metric == "render_ms") return "the CPU side of drawing is slow: Render_* systems in top_systems, or fewer things to extract";
    if (metric == "gpu_ms") return "the GPU is slow: gpu_stages names the passes; turn off or lower the costly effect (Environment3D)";
    if (metric == "frame_ms") return "the frame is slow: compare sim_ms, render_ms and gpu_ms to see which part";
    if (metric == "draws")
        return "too many draw calls: share textures (one spritesheet) so sprites batch, fewer materials, merge static meshes";
    if (metric == "triangles") return "too many triangles: meshes with LODs (gen_mesh or the importer), simpler models, fewer instances";
    if (metric == "sprites") return "too many sprites on screen: cull off-screen ones, use a tilemap for static ground";
    if (metric == "upload_bytes") return "too much uploaded each frame: fewer dynamic quads, static geometry stays on the GPU";
    if (metric == "heap_allocations") return "too many live heap allocations: pool or preallocate what systems create";
    return "";
}

void write_verdict(JsonWriter& w, const perf::Verdict& v) {
    w.key("verdict").begin_object().key("pass").boolean(v.pass).key("lines").begin_array();
    for (const perf::BudgetLine& l : v.lines) {
        w.begin_object().key("metric").str(l.metric).key("stat").str(l.stat).key("measured").num_f(round3(l.measured));
        w.key("limit").num_f(round3(l.limit)).key("over").boolean(l.over);
        if (l.over) w.key("advice").str(advice(l.metric));
        w.end_object();
    }
    w.end_array().end_object();
}

// M-CONTENT 2: the 3D scene draws that were not part of an instanced run, by cause, the largest (mean per frame)
// first, each with what to change. Counts repeat run to run; a capture without 3D has none.
void write_draw_causes(JsonWriter& w, const perf::Summary& s) {
    const perf::Counters& avg = s.counters_avg;
    const perf::Counters& max = s.counters_max;
    u32 order[kDrawCauseCount];
    u32 n = 0, total = 0;
    for (u32 k = 0; k < kDrawCauseCount; ++k) {
        total += avg.draw_causes[k];
        if (max.draw_causes[k] > 0) order[n++] = k;
    }
    std::sort(order, order + n, [&](u32 a, u32 b) { return avg.draw_causes[a] != avg.draw_causes[b] ? avg.draw_causes[a] > avg.draw_causes[b] : a < b; });
    w.key("draw_causes").begin_object();
    w.key("scene_draws_avg").num_u(avg.draws_instanced + total).key("instanced_draws_avg").num_u(avg.draws_instanced);
    w.key("instanced_runs_avg").num_u(avg.runs_instanced).key("not_instanced_avg").num_u(total);
    w.key("causes").begin_array();
    for (u32 i = 0; i < n; ++i) {
        const DrawCause c = static_cast<DrawCause>(order[i]);
        w.begin_object().key("cause").str(draw_cause_name(c)).key("avg").num_u(avg.draw_causes[order[i]]);
        w.key("max").num_u(max.draw_causes[order[i]]).key("hint").str(draw_cause_hint(c)).end_object();
    }
    w.end_array().end_object();
}

void write_report(JsonWriter& w, const perf::Capture& cap, const perf::Summary& s, u64 top) {
    w.key("app").str(cap.app.view()).key("backend").str(cap.backend.view()).key("scene").str(cap.scene.view());
    w.key("frames").num_u(s.frames);
    write_stat(w, "frame_ms", s.frame);
    write_stat(w, "sim_ms", s.sim);
    write_stat(w, "render_ms", s.render);
    write_stat(w, "gpu_ms", s.gpu);
    write_counters(w, "counters_max", s.counters_max);
    write_counters(w, "counters_avg", s.counters_avg);
    write_draw_causes(w, s);

    Array<usize> order;
    editor::rank_capture_systems(cap, order);
    w.key("top_systems").begin_array();
    for (usize i = 0; i < order.size() && i < top; ++i) {
        const perf::SystemStats& sy = cap.systems[order[i]];
        if (sy.runs == 0) continue;
        w.begin_object().key("name").str(sy.name.view()).key("phase").str(editor::perf_phase_name(cap, sy.phase));
        w.key("ms_per_frame").num_f(round3(editor::system_ms_per_frame(cap, sy))).key("max_frame_ms").num_f(round3(sy.max_frame_ms));
        w.key("runs").num_u(sy.runs).end_object();
    }
    w.end_array();

    // GPU stages with any measured frame, most expensive (p50) first.
    struct StageRow {
        usize index;
        perf::Stat stat;
    };
    Array<StageRow> stages;
    Array<f32> col;
    for (usize g = 0; g < cap.gpu_stages.size(); ++g) {
        col.clear();
        for (const perf::Frame& f : cap.frames) col.push_back(f.gpu_stage_ms[g]);
        const perf::Stat st = perf::stat_of(Span<const f32>(col.data(), col.size()));
        if (st.count) stages.push_back({g, st});
    }
    std::sort(stages.begin(), stages.end(), [](const StageRow& a, const StageRow& b) {
        return a.stat.p50 != b.stat.p50 ? a.stat.p50 > b.stat.p50 : a.index < b.index;
    });
    w.key("gpu_stages").begin_array();
    for (const StageRow& r : stages)
        w.begin_object().key("name").str(cap.gpu_stages[r.index].view()).key("p50").num_f(round3(r.stat.p50)).key("p95").num_f(round3(r.stat.p95)).end_object();
    w.end_array();

    w.key("spikes").begin_array();
    for (const perf::Spike& sp : s.spikes)
        w.begin_object().key("frame").num_u(sp.frame).key("tick").num_u(sp.tick).key("frame_ms").num_f(round3(sp.frame_ms)).key("cause").str(sp.cause.view()).end_object();
    w.end_array();
}

bool arg_target(ToolCall& c, perf::Target& t) {
    const cook::JsonValue v = arg(c, "target");
    if (!v.valid()) return true;
    if (!v.is_string() || !perf::parse_target(v.as_string(), t)) return arg_missing(c, "target", "desktop, mobile or web");
    return true;
}

}  // namespace

bool perf_run(ToolCall& c) {
    if (!need_project(c)) return false;
    // The scene: 'scene' (a path under assets/) or the open one, saved.
    StringView other;
    if (!arg_string(c, "scene", other, false)) return false;
    const Server::SceneFile& file = c.server.scene_file();
    StringView scene_rel = file.rel.view();
    asset::Guid guid = scene(c).doc().guid();
    if (!other.empty() && other != file.rel.view()) {
        const editor::ProjectAsset* found = nullptr;
        for (const editor::ProjectAsset& a : c.server.session().project().assets())
            if (a.type == asset::ChunkType::Scene && a.path.view() == other) found = &a;
        if (!found) return fail(c, "argument 'scene': no such scene in the project (asset_list with type Scene lists them)");
        scene_rel = other;
        guid = found->guid;
    } else {
        if (file.rel.empty()) return fail(c, "the scene was never saved: scene_save it (with a path under assets/) first");
        if (scene(c).dirty()) return fail(c, "the scene has unsaved changes: scene_save first (the run plays the cooked file)");
    }
    u64 frames = 300, warmup = 30, width = 960, height = 540, top = 8;
    bool headless = false, software = false;
    perf::Target target = perf::Target::Desktop;
    input::InputScript script;
    ConvarArgs convars;
    if (!arg_uint(c, "frames", frames, 10, kMaxFrames, false) || !arg_uint(c, "warmup", warmup, 0, kMaxFrames, false) ||
        !arg_uint(c, "width", width, 64, kMaxSide, false) || !arg_uint(c, "height", height, 64, kMaxSide, false) ||
        !arg_uint(c, "top", top, 1, 64, false) || !arg_bool(c, "headless", headless) || !arg_bool(c, "software", software) ||
        !arg_target(c, target) || !arg_input(c, script) || !arg_convars(c, convars))
        return false;
    perf::Budget budget;
    if (!editor::project_budget(c.server.session().project(), target, budget, c.error)) return false;

    // Where the capture goes: 'save', else cooked/.perf/last.mcperf (the one before it becomes
    // previous.mcperf, so perf_compare with no arguments compares the last two runs).
    const StringView root = c.server.session().project().root();
    const cook::String perf_dir = cook::path_join(root, "cooked/.perf");
    if (Result<void> mk = cook::fs::create_directories(perf_dir.view()); !mk) return fail(c, "cannot create cooked/.perf");
    const cook::String last = cook::path_join(perf_dir.view(), "last.mcperf");
    const cook::String previous = cook::path_join(perf_dir.view(), "previous.mcperf");
    cook::String out_abs = last, out_rel("cooked/.perf/last.mcperf");
    StringView save;
    if (!arg_string(c, "save", save, false)) return false;
    if (!save.empty()) {
        if (!project_file(c, "save", save, out_abs)) return false;
        out_rel = cook::String(save);
        const StringView a = out_abs.view();
        if (const usize slash = a.rfind('/'); slash != StringView::npos)
            if (Result<void> mk = cook::fs::create_directories(a.substr(0, slash)); !mk) return fail(c, "cannot create the folder of 'save'");
    }
    cook::String bytes;
    if (cook::fs::read_text(last.view(), bytes))
        (void)cook::fs::write_file_atomic(previous.view(), Span<const byte>(reinterpret_cast<const byte*>(bytes.c_str()), bytes.size()));
    (void)cook::fs::remove(out_abs.view());

    const cook::CookReport& r = c.server.session().cook(false);
    if (!r.ok()) {
        c.error += "cook failed:";
        append_diags(c.error, r.diags, 10);
        return false;
    }
    cook::String player;
    if (!find_player(c.server, player))
        return fail(c, "no player to measure with: start the server with --player <mce_player or the game's host>");
    const cook::String cooked = cook::path_join(root, "cooked");
    cook::String gs(asset::to_string(guid).view()), total, warm, window, input_path;
    const u64 total_frames = frames + warmup;
    cook::appendf(total, "%llu", static_cast<unsigned long long>(total_frames));
    cook::appendf(warm, "%llu", static_cast<unsigned long long>(warmup));
    cook::appendf(window, "%llux%llu", static_cast<unsigned long long>(width), static_cast<unsigned long long>(height));
    Array<StringView> args;
    args.push_back(player.view());
    for (StringView a : {StringView("-pack"), cooked.view(), StringView("-scene"), gs.view(), StringView("-perf_capture"), out_abs.view(),
                         StringView("-perf_warmup"), warm.view(), StringView("-platform.error_box"), StringView("0"),
                         StringView("-settings"), StringView("0"), StringView("-audio.enabled"), StringView("0")})
        args.push_back(a);
    const bool use_software = !headless && (software || headless_display());
    if (headless) {
        for (StringView a : {StringView("-headless"), StringView("1"), StringView("-ticks"), total.view()}) args.push_back(a);
    } else {
        // One sim tick per frame (fast-forward with a catch-up of 1): the frames, the ticks they show
        // and so the counters repeat run to run, whatever the machine's speed.
        for (StringView a : {StringView("-window"), window.view(), StringView("-frames"), total.view(), StringView("-sim.fast_forward"),
                             StringView("1"), StringView("-sim.max_catchup"), StringView("1"), StringView("-r.vsync"), StringView("0"),
                             StringView("-pause_on_focus_loss"), StringView("0"), StringView("-mouse_look"), StringView("0"),
                             StringView("-console_open"), StringView("0")})
            args.push_back(a);
        if (use_software) {
            args.push_back("-platform.video_driver");
            args.push_back("offscreen");
        }
    }
    if (c.server.module().loaded()) {
        args.push_back("-module");
        args.push_back(c.server.module().source_path.view());
    }
    if (!script.empty()) {
        cook::String text;
        script_text(script, text);
        input_path = cook::path_join(perf_dir.view(), "input.txt");
        if (!cook::fs::write_file_atomic(input_path.view(), Span<const byte>(reinterpret_cast<const byte*>(text.view().data()), text.view().size())))
            return fail(c, "cannot write cooked/.perf/input.txt");
        args.push_back("-input");
        args.push_back(input_path.view());
    }
    Array<cook::String> flags;
    for (const cook::String& n : convars.names) {
        cook::String f("-");
        f += n.view();
        flags.push_back(static_cast<cook::String&&>(f));
    }
    for (usize i = 0; i < flags.size(); ++i) {
        args.push_back(flags[i].view());
        args.push_back(convars.values[i].view());
    }
    Array<char> output;
    i32 code = 0;
    if (!spawn_player(c, player.view(), args.span(), use_software, output, code)) return false;

    perf::Capture cap;
    cook::String err;
    if (!editor::load_mcperf(out_abs.view(), cap, err)) {
        cook::appendf(c.error, "the player (exit %d) wrote no capture; its last output:\n", code);
        const usize from = output.size() > 1500 ? output.size() - 1500 : 0;
        c.error += StringView(output.data() + from, output.size() - from);
        return false;
    }
    if (!save.empty() && cook::fs::read_text(out_abs.view(), bytes))  // a saved run is also the last one
        (void)cook::fs::write_file_atomic(last.view(), Span<const byte>(reinterpret_cast<const byte*>(bytes.c_str()), bytes.size()));
    const perf::Summary s = perf::summarize(cap);
    const perf::Verdict v = perf::judge(s, budget);

    JsonWriter& w = c.out;
    w.begin_object().key("file").str(out_rel.view()).key("scene_path").str(scene_rel).key("target").str(perf::target_name(target));
    w.key("headless").boolean(headless).key("software").boolean(use_software).key("exit_code").num_i(code);
    write_verdict(w, v);
    write_report(w, cap, s, top);
    w.end_object();
    return true;
}

bool perf_compare(ToolCall& c) {
    if (!need_project(c)) return false;
    StringView before_rel("cooked/.perf/previous.mcperf"), after_rel("cooked/.perf/last.mcperf");
    u64 top = 8;
    perf::Target target = perf::Target::Desktop;
    if (!arg_string(c, "before", before_rel, false) || !arg_string(c, "after", after_rel, false) || !arg_uint(c, "top", top, 1, 64, false) ||
        !arg_target(c, target))
        return false;
    if (before_rel.empty()) before_rel = "cooked/.perf/previous.mcperf";
    if (after_rel.empty()) after_rel = "cooked/.perf/last.mcperf";
    cook::String before_abs, after_abs;
    if (!project_file(c, "before", before_rel, before_abs) || !project_file(c, "after", after_rel, after_abs)) return false;
    perf::Capture a, b;
    if (!editor::load_mcperf(before_abs.view(), a, c.error) || !editor::load_mcperf(after_abs.view(), b, c.error)) {
        c.error += " (perf_run writes captures; with no arguments perf_compare reads its last two)";
        return false;
    }
    perf::Budget budget;
    if (!editor::project_budget(c.server.session().project(), target, budget, c.error)) return false;
    editor::Comparison cmp;
    editor::compare_captures(a, b, cmp);

    JsonWriter& w = c.out;
    w.begin_object().key("before").str(before_rel).key("after").str(after_rel).key("target").str(perf::target_name(target));
    if (!cmp.same_scene) w.key("note").str("the captures are of different scenes");

    // Times: p50 and p95 before and after. A change of 10% and 0.1 ms or more counts as one.
    w.key("times").begin_object();
    for (const editor::TimeChange& t : cmp.times) {
        w.key(t.metric).begin_object();
        if (!t.measured) {
            w.key("measured").boolean(false).end_object();
            continue;
        }
        w.key("p50_before").num_f(round3(t.p50_before)).key("p50_after").num_f(round3(t.p50_after));
        w.key("p95_before").num_f(round3(t.p95_before)).key("p95_after").num_f(round3(t.p95_after));
        w.key("p95_change_pct").num_f(t.p95_change_pct).end_object();
    }
    w.end_object();

    // Counters: the worst frame before and after; any change counts (they repeat run to run).
    w.key("counters").begin_object();
    for (const editor::CounterChange& k : cmp.counters)
        w.key(k.metric).begin_object().key("before").num_u(k.before).key("after").num_u(k.after)
            .key("change").num_i(static_cast<i64>(k.after) - static_cast<i64>(k.before)).end_object();
    w.end_object();

    // Systems whose time per frame moved the most (either way), by name and phase.
    w.key("systems").begin_array();
    for (usize i = 0; i < cmp.systems.size() && i < top; ++i) {
        const editor::SystemChange& r = cmp.systems[i];
        w.begin_object().key("name").str(r.name.view()).key("phase").str(r.phase.view()).key("ms_per_frame_before").num_f(round3(r.before_ms))
            .key("ms_per_frame_after").num_f(round3(r.after_ms)).end_object();
    }
    w.end_array();

    w.key("better").begin_array();
    for (const char* m : cmp.better) w.str(m);
    w.end_array().key("worse").begin_array();
    for (const char* m : cmp.worse) w.str(m);
    w.end_array();
    const perf::Verdict va = perf::judge(cmp.before, budget), vb = perf::judge(cmp.after, budget);
    w.key("pass_before").boolean(va.pass);
    write_verdict(w, vb);
    w.end_object();
    return true;
}

}  // namespace mce::mcp
