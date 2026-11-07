// M-DYNRES 46c against a real driver, GL 4.5 and (as test_render_sdlgpu) the SDL GPU backend: the dynamic resolution
// driver in Forward3D (update_dynres once a frame on the frame's first view, the frame ring, the per-view scale rule and
// ensure_targets keeping the TAA history over a scale-only change).
// Off (the setting, r.dynres 0, timestamps = false, no completed timestamps yet) draws the plain frame bit for bit; on
// and never over budget draws it too, at a ceiling on the 1/16 grid (1.0) and off it (the Low tier's 0.67); an injected
// overload (r.dynres.inject) steps Forward3DStats::render_scale down in grid steps with the HDR target following and
// recovers to exactly the tier's scale, the sequence equal to the CPU controller's (dynres.h) with the same latency; a
// step with TAA keeps the history (the same handles, taa_history true) and the adjacent-frame mean difference stays
// under a bound; a step with the sun shafts, the flare and the outline on draws them at the new size with no validation
// error and no texture growth; a render-texture view keeps the tier's scale while the window view follows the
// controller; split-screen cells scale together; the same run twice gives the same bytes; a device frame not in the
// ring gives no sample and two first views in one device frame step the controller once. 160 x 120 frames.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <vector>

#include "fixture3d.h"
#include "mce/convar.h"
#include "mce/hash.h"
#include "mce/render/dynres.h"

using namespace mce;
using namespace mce::render;
using namespace mce::render_test;

namespace {

struct CvarScope {  // sets a convar for the scope, resets it after
    ConVar* cv;
    CvarScope(const char* name, const char* value) : cv(ConVar::find(StringId(name))) {
        REQUIRE(cv != nullptr);
        REQUIRE(cv->set_string(value));
    }
    ~CvarScope() { cv->reset(); }
    CvarScope(const CvarScope&) = delete;
    CvarScope& operator=(const CvarScope&) = delete;
};

bool same(const Array<u8>& a, const Array<u8>& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

// The mean absolute channel difference (RGB) of two frames, in 8-bit levels.
f64 mean_diff(const Array<u8>& a, const Array<u8>& b) {
    u64 sum = 0;
    for (usize i = 0; i < a.size(); i += 4)
        for (usize c = 0; c < 3; ++c) sum += u64(std::abs(int(a[i + c]) - int(b[i + c])));
    return f64(sum) / (f64(a.size()) * 0.75);  // a.size() / 4 pixels, 3 channels each
}

bool on_grid_or(f32 s, f32 ceiling) {
    const f32 k = s * f32(kDynResGrid);
    return s == ceiling || k == std::floor(k);
}

rhi::Extent2D scaled(rhi::Extent2D out, f32 s) {
    if (s >= 1.0f) return out;
    return {std::max(1u, u32(std::lround(f32(out.width) * s))), std::max(1u, u32(std::lround(f32(out.height) * s)))};
}

DynamicResolution dynres_on() {
    DynamicResolution d;
    d.enabled = true;
    d.min_scale = 0.5f;
    return d;
}

// The scale sequence the CPU controller gives for an injected schedule with the driver's latency: the sample of frame k
// (its cost at the scale frame k drew at) arrives at frame k + MCE_FRAMES_IN_FLIGHT.
std::vector<f32> cpu_sequence(const DynamicResolution& d, f32 tier_floor, f32 tier_ceiling, const char* schedule,
                              u32 frames) {
    DynResParams p;
    REQUIRE(dynres_resolve(d, true, 0.0f, tier_floor, tier_ceiling, p));
    DynResInject inj;
    REQUIRE(dynres_parse_inject(schedule, inj));
    DynResState st;
    st.scale = p.ceiling;
    std::vector<f32> scales;
    for (u32 k = 0; k < frames; ++k) {
        const bool has = k >= MCE_FRAMES_IN_FLIGHT;
        f32 ms = 0.0f, sc = 1.0f;
        if (has) {
            sc = scales[k - MCE_FRAMES_IN_FLIGHT];
            ms = dynres_ms_at(dynres_inject_ms(inj, k - MCE_FRAMES_IN_FLIGHT), sc, p.variable_share);
        }
        scales.push_back(dynres_step(p, st, has, ms, sc));
    }
    return scales;
}

Forward3DDesc desc_for(f32 render_scale, u32 msaa = 1, bool timestamps = true) {
    Forward3DDesc d;
    d.msaa = msaa;
    d.render_scale = render_scale;
    d.dynres_floor = 0.5f;
    d.timestamps = timestamps;
    return d;
}

struct Shot {
    Array<u8> px;
    Forward3DStats stats;
    rhi::Extent2D hdr{};
    rhi::TextureHandle history;
};

struct DynFixture {
    Fixture3D f;
    explicit DynFixture(const Forward3DDesc& d) : f(d.msaa, true, true, d.render_scale, &d) {}
    // One frame of the fixture's box scene with `dynres` (null: the default PostFx, off), TAA as asked, `more` last.
    Shot draw(const DynamicResolution* dynres, bool taa = false, const std::function<void(Scene3D&)>& more = {}) {
        f.render(0.0f, true, [&](Scene3D& s) {
            PostFx post = s.post();
            post.taa = taa;
            if (dynres) post.dynres = *dynres;
            s.set_post(post);
            if (more) more(s);
        });
        Shot shot;
        shot.px = f.pixels;
        const Forward3D& fw = f.renderer.forward_3d();
        shot.stats = fw.last_stats();
        shot.hdr = f.device->texture_size(fw.hdr_target());
        shot.history = fw.taa_history();
        return shot;
    }
};

// The fixture's box scene from `yaw_deg` at `size`, `dynres` on, recorded into `target` as `out`.
void record_box(Fixture3D& f, rhi::CommandBuffer& cmd, rhi::TextureHandle target, rhi::Extent2D size, f32 yaw_deg,
                const Forward3D::ViewOutput& out, const DynamicResolution& dynres) {
    const Camera3D cam = orbit_camera({0, 0.5f, 0}, math::radians(yaw_deg), math::radians(35.0f), 7.0f);
    Scene3D scene(f.renderer.meshes(), mem::heap());
    scene.begin(cam, size);
    DirectionalLight sun;
    sun.direction = {-0.5f, -1.0f, -0.3f};
    scene.set_light(sun);
    ShadowSettings ss;
    ss.resolution = 1024;
    scene.set_shadows(ss);
    PostFx post = scene.post();
    post.dynres = dynres;
    scene.set_post(post);
    MeshInstance ground;
    ground.mesh = 2;
    scene.add(ground);
    MeshInstance box;
    box.mesh = 1;
    scene.add(box);
    scene.build();
    f.renderer.record_3d(cmd, scene, target, out);
}

}  // namespace

TEST_CASE("dynres: off (setting, r.dynres 0, timestamps = false, no timestamps yet) draws the plain frame bit for bit") {
    constexpr u32 kFrames = 8;
    std::vector<Array<u8>> plain;
    {
        DynFixture d(desc_for(1.0f, 4));
        for (u32 i = 0; i < kFrames; ++i) {
            const Shot s = d.draw(nullptr);
            CHECK_FALSE(s.stats.dynres);
            CHECK(s.stats.render_scale == 1.0f);
            plain.push_back(s.px);
        }
    }
    {  // the setting off (other fields odd)
        DynFixture d(desc_for(1.0f, 4));
        DynamicResolution off;
        off.target_ms = 4.0f;
        off.min_scale = 0.5f;
        for (u32 i = 0; i < kFrames; ++i) {
            const Shot s = d.draw(&off);
            CHECK_FALSE(s.stats.dynres);
            CHECK(same(s.px, plain[i]));
        }
    }
    {  // r.dynres 0 with an overload injected
        CvarScope cvar("r.dynres", "0");
        CvarScope inject("r.dynres.inject", "40@0");
        DynFixture d(desc_for(1.0f, 4));
        const DynamicResolution on = dynres_on();
        for (u32 i = 0; i < kFrames; ++i) {
            const Shot s = d.draw(&on);
            CHECK_FALSE(s.stats.dynres);
            CHECK(s.stats.render_scale == 1.0f);
            CHECK(same(s.px, plain[i]));
        }
    }
    {  // timestamps = false and nothing injected: no sample, the ceiling holds
        DynFixture d(desc_for(1.0f, 4, false));
        const DynamicResolution on = dynres_on();
        for (u32 i = 0; i < kFrames; ++i) {
            const Shot s = d.draw(&on);
            CHECK(s.stats.dynres);
            CHECK(s.stats.render_scale == 1.0f);
            CHECK(s.stats.dynres_cost_ms == 0.0f);
            CHECK(same(s.px, plain[i]));
        }
    }
    {  // measured, before any timestamps complete
        // The first frame only: GL hands a frame's stamps over MCE_FRAMES_IN_FLIGHT frames later, SDL GPU's fence
        // timestamps as soon as the frame's fence signals (measured: the next frame on lavapipe), and a measured
        // software-rasterizer frame may be over any budget.
        DynFixture d(desc_for(1.0f, 4));
        const DynamicResolution on = dynres_on();
        const Shot s = d.draw(&on);
        CHECK(s.stats.dynres);
        CHECK(s.stats.render_scale == 1.0f);
        CHECK(same(s.px, plain[0]));
    }
}

TEST_CASE("dynres: on and never over budget draws the plain frame bit for bit, at a ceiling on and off the grid") {
    CvarScope inject("r.dynres.inject", "4@0");
    for (const f32 ceiling : {1.0f, 0.67f}) {
        CAPTURE(ceiling);
        constexpr u32 kFrames = 12;
        std::vector<Array<u8>> plain;
        {
            DynFixture d(desc_for(ceiling));
            for (u32 i = 0; i < kFrames; ++i) plain.push_back(d.draw(nullptr).px);
        }
        DynFixture d(desc_for(ceiling));
        const DynamicResolution on = dynres_on();
        for (u32 i = 0; i < kFrames; ++i) {
            const Shot s = d.draw(&on);
            CHECK(s.stats.dynres);
            CHECK(s.stats.render_scale == (ceiling >= 1.0f ? 1.0f : ceiling));  // the tier's own value, bit for bit
            CHECK(s.hdr == scaled({kW, kH}, ceiling));
            CHECK(same(s.px, plain[i]));
            if (i >= MCE_FRAMES_IN_FLIGHT) CHECK(s.stats.dynres_cost_ms > 0.0f);
        }
    }
}

TEST_CASE("dynres: an injected overload steps down in grid steps, the HDR target follows, recovery reaches the tier "
          "exactly; the sequence is the CPU controller's") {
    // Medium's 0.85 is off the grid: the way back must end on it bit for bit.
    constexpr f32 kCeiling = 0.85f;
    constexpr u32 kFrames = 110;
    const char* kSchedule = "20@0,4@30";
    CvarScope inject("r.dynres.inject", kSchedule);
    const DynamicResolution on = dynres_on();
    const std::vector<f32> want = cpu_sequence(on, 0.5f, kCeiling, kSchedule, kFrames);
    DynFixture d(desc_for(kCeiling));
    f32 lowest = 1.0f;
    u32 drops = 0, rises = 0;
    f32 prev = kCeiling;
    for (u32 k = 0; k < kFrames; ++k) {
        const Shot s = d.draw(&on);
        CAPTURE(k);
        CHECK(s.stats.dynres);
        CHECK(s.stats.render_scale == want[k]);
        CHECK(on_grid_or(s.stats.render_scale, kCeiling));
        CHECK(s.hdr == scaled({kW, kH}, s.stats.render_scale));
        lowest = std::min(lowest, s.stats.render_scale);
        if (s.stats.render_scale < prev) ++drops;
        if (s.stats.render_scale > prev) ++rises;
        prev = s.stats.render_scale;
    }
    MESSAGE("lowest scale " << lowest << ", drops " << drops << ", rises " << rises);
    CHECK(lowest < kCeiling);
    CHECK(drops >= 1);
    CHECK(rises >= 1);
    CHECK(prev == kCeiling);  // recovered to exactly the tier's scale
    CHECK(d.f.device->validation_error_count() == 0);
}

TEST_CASE("dynres: a step with TAA keeps the history and the adjacent-frame difference stays bounded") {
    CvarScope inject("r.dynres.inject", "26@0");
    const DynamicResolution on = dynres_on();
    constexpr u32 kFrames = 16;
    const std::vector<f32> want = cpu_sequence(on, 0.5f, 1.0f, "26@0", kFrames);
    DynFixture d(desc_for(1.0f));
    std::vector<Shot> shots;
    for (u32 k = 0; k < kFrames; ++k) shots.push_back(d.draw(&on, true));
    f64 steady = 0.0, worst_step = 0.0;
    u32 steps = 0;
    for (u32 k = 1; k < kFrames; ++k) {
        CAPTURE(k);
        const Shot& s = shots[k];
        CHECK(s.stats.render_scale == want[k]);
        CHECK(s.stats.taa);
        CHECK(s.stats.taa_history);  // valid from frame 1 on, through every step
        if (k >= 2) CHECK(s.history == shots[k - 2].history);  // the two output-size histories survive the steps
        const f64 m = mean_diff(s.px, shots[k - 1].px);
        if (s.stats.render_scale != shots[k - 1].stats.render_scale) {
            ++steps;
            worst_step = std::max(worst_step, m);
            CHECK(!(s.hdr == shots[k - 1].hdr));
        } else if (k >= 2) {
            steady = std::max(steady, m);
        }
    }
    MESSAGE("steps " << steps << ", worst adjacent mean difference over a step " << worst_step << " levels, steady "
                     << steady);
    CHECK(steps >= 2);
    // The bound: a step blends the kept history at a new jitter density, a soft frame at worst, never a restart. Measured
    // on llvmpipe: 0.17 levels with the history kept, 1.42 with it thrown away (ensure_targets without keep_history).
    CHECK(worst_step < 0.75);
    CHECK(d.f.device->validation_error_count() == 0);
}

TEST_CASE("dynres: a step with the sun shafts, the flare and the outline on draws them at the new size") {
    CvarScope inject("r.dynres.inject", "26@0");
    const DynamicResolution on = dynres_on();
    constexpr u32 kFrames = 14;
    const std::vector<f32> want = cpu_sequence(on, 0.5f, 1.0f, "26@0", kFrames);
    auto run = [&](bool effects, std::vector<Shot>& out, std::vector<usize>& mem) {
        DynFixture d(desc_for(1.0f));
        upload_outline_box(d.f);
        sun_shafts_camera(d.f);
        for (u32 k = 0; k < kFrames; ++k) {
            out.push_back(d.draw(&on, false, [&](Scene3D& s) {
                const vec3 toward = lens_flare_toward(s.view(), kShaftsSunU, kShaftsSunV);
                const SunShafts shafts = sun_shafts_setting();
                sun_shafts_scene(s, toward, effects ? &shafts : nullptr);
                PostFx post = s.post();
                if (effects) {
                    post.flare = lens_flare_setting();
                    post.outline = outline_setting();
                }
                s.set_post(post);
                MeshInstance b;
                b.mesh = kOutlineBoxMesh;
                b.world = math::translation(kOutlineBoxAt);
                s.add(b);
            }));
            mem.push_back(d.f.device->texture_memory_bytes());
        }
        CHECK(d.f.device->validation_error_count() == 0);
    };
    std::vector<Shot> with, without;
    std::vector<usize> mem_with, mem_without;
    run(true, with, mem_with);
    run(false, without, mem_without);
    u32 steps = 0;
    for (u32 k = 0; k < kFrames; ++k) {
        CAPTURE(k);
        CHECK(with[k].stats.render_scale == want[k]);
        CHECK(with[k].hdr == scaled({kW, kH}, want[k]));
        CHECK(with[k].stats.edge_outline);
        CHECK(with[k].stats.lens_flare);
        CHECK(with[k].stats.sun_shafts);
        CHECK(mean_diff(with[k].px, without[k].px) > 0.2);  // the effects draw on every frame, the step frames included
        if (k > 0 && want[k] != want[k - 1]) ++steps;
    }
    CHECK(steps >= 2);
    // No growth: the retired targets of each step are freed (MCE_FRAMES_IN_FLIGHT frames later), so the memory after the
    // last step settles at or under what the full-size frame held.
    MESSAGE("texture bytes: frame 2 " << mem_with[2] << ", last " << mem_with.back());
    CHECK(mem_with.back() <= mem_with[2]);
}

TEST_CASE("dynres: a render-texture view keeps the tier's scale while the window view follows the controller") {
    constexpr f32 kCeiling = 0.85f;
    constexpr u32 kFrames = 14;
    CvarScope inject("r.dynres.inject", "26@0");
    const DynamicResolution on = dynres_on();
    const std::vector<f32> want = cpu_sequence(on, 0.5f, kCeiling, "26@0", kFrames);
    DynFixture d(desc_for(kCeiling));
    Fixture3D& f = d.f;
    rhi::TextureDesc td;
    td.size = {kW, kH};
    td.format = rhi::TextureFormat::RGBA16F;
    td.usage = rhi::TextureUsage::SampledColorTarget;
    td.dimension = rhi::TextureDimension::Tex2DArray;
    td.debug_name = "test.render_texture";
    const rhi::TextureHandle rt = f.device->create_texture(td);
    const u32 key = f.renderer.textures().allocate_key();
    f.renderer.textures().adopt(key, 1, rt);
    bool changed = false;
    for (u32 k = 0; k < kFrames; ++k) {
        CAPTURE(k);
        f.device->begin_frame(f.frame++);
        rhi::CommandBuffer cmd;
        Forward3D::ViewOutput tex;  // the frame's first view: a render texture (kit::record_views sorts them first)
        tex.slot = 1;
        tex.linear_output = true;
        record_box(f, cmd, rt, {kW, kH}, 30.0f, tex, on);
        const Forward3DStats tex_stats = f.renderer.forward_3d().last_stats();
        Forward3D::ViewOutput win;
        win.slot = 0;
        win.first = false;
        record_box(f, cmd, f.target, {kW, kH}, 0.0f, win, on);
        const Forward3DStats win_stats = f.renderer.forward_3d().last_stats();
        f.device->submit(cmd);
        f.device->end_frame();
        CHECK(tex_stats.dynres);
        CHECK(tex_stats.render_scale == kCeiling);
        CHECK(win_stats.render_scale == want[k]);
        changed = changed || win_stats.render_scale != kCeiling;
    }
    CHECK(changed);
    CHECK(f.device->validation_error_count() == 0);
    f.renderer.textures().release(key);
}

TEST_CASE("dynres: split-screen cells scale together") {
    constexpr u32 kFrames = 14;
    constexpr u32 kHalf = kW / 2;
    CvarScope inject("r.dynres.inject", "26@0");
    const DynamicResolution on = dynres_on();
    const std::vector<f32> want = cpu_sequence(on, 0.5f, 1.0f, "26@0", kFrames);
    DynFixture d(desc_for(1.0f));
    Fixture3D& f = d.f;
    bool changed = false;
    for (u32 k = 0; k < kFrames; ++k) {
        CAPTURE(k);
        f.device->begin_frame(f.frame++);
        rhi::CommandBuffer cmd;
        Forward3D::ViewOutput left;
        left.slot = 1;
        left.rect = {0, 0, kHalf, kH};
        record_box(f, cmd, f.target, {kHalf, kH}, 0.0f, left, on);
        const Forward3DStats a = f.renderer.forward_3d().last_stats();
        Forward3D::ViewOutput right;
        right.slot = 2;
        right.rect = {i32(kHalf), 0, kHalf, kH};
        right.first = false;
        record_box(f, cmd, f.target, {kHalf, kH}, 90.0f, right, on);
        const Forward3DStats b = f.renderer.forward_3d().last_stats();
        f.device->submit(cmd);
        f.device->end_frame();
        CHECK(a.render_scale == want[k]);
        CHECK(b.render_scale == want[k]);
        changed = changed || a.render_scale < 1.0f;
    }
    CHECK(changed);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("dynres: the same injected run twice gives the same bytes") {
    CvarScope inject("r.dynres.inject", "30@0,6@20");
    const DynamicResolution on = dynres_on();
    auto run = [&] {
        DynFixture d(desc_for(1.0f, 4));
        Hasher64 h;
        std::vector<f32> scales;
        for (u32 k = 0; k < 40; ++k) {
            const Shot s = d.draw(&on, true);
            h.bytes(s.px.data(), s.px.size());
            scales.push_back(s.stats.render_scale);
        }
        return std::make_pair(h.value(), scales);
    };
    const auto a = run();
    const auto b = run();
    CHECK(a.first == b.first);
    CHECK(a.second == b.second);
    CHECK(a.second == cpu_sequence(on, 0.5f, 1.0f, "30@0,6@20", 40));
}

TEST_CASE("dynres: a device frame not in the ring gives no sample; two first views in one device frame step once") {
    const DynamicResolution on = dynres_on();
    SUBCASE("a jump in the device frame index") {
        CvarScope inject("r.dynres.inject", "26@0");
        DynFixture d(desc_for(1.0f));
        std::vector<f32> got;
        for (u32 k = 0; k < 8; ++k) {
            if (k == 3) d.f.frame = 100;  // frames 100..102 look for 97..99: not in the ring, no sample
            got.push_back(d.draw(&on).stats.render_scale);
        }
        // Without the jump the first drop is at frame 3 (cpu_sequence); with it the first sample is frame 100's, at 103.
        CHECK(cpu_sequence(on, 0.5f, 1.0f, "26@0", 8)[3] < 1.0f);
        for (u32 k = 0; k < 6; ++k) CHECK(got[k] == 1.0f);
        CHECK(got[6] < 1.0f);
    }
    SUBCASE("two first views per device frame") {
        constexpr u32 kFrames = 14;
        CvarScope inject("r.dynres.inject", "26@0");
        const std::vector<f32> want = cpu_sequence(on, 0.5f, 1.0f, "26@0", kFrames);
        DynFixture d(desc_for(1.0f));
        Fixture3D& f = d.f;
        for (u32 k = 0; k < kFrames; ++k) {
            CAPTURE(k);
            f.device->begin_frame(f.frame++);
            rhi::CommandBuffer cmd;
            for (u32 v = 0; v < 2; ++v) {
                Forward3D::ViewOutput out;  // first = true both times (an editor drawing two panels)
                out.slot = v;
                record_box(f, cmd, f.target, {kW, kH}, 0.0f, out, on);
                CHECK(f.renderer.forward_3d().last_stats().render_scale == want[k]);
            }
            f.device->submit(cmd);
            f.device->end_frame();
        }
        CHECK(f.device->validation_error_count() == 0);
    }
}

TEST_CASE("dynres: a frame stamped by a render-texture view gives no measured sample; injection still steps") {
    // record_views records render textures first, so such a frame's `first` view (the one that writes the GPU timestamps)
    // draws at the tier's static scale: its span says nothing about the window scale and is not a sample. The control
    // run (the window view first, the same views) shows the measured path does take samples on this backend.
    constexpr f32 kCeiling = 0.85f;
    constexpr u32 kFrames = 12;
    const DynamicResolution on = dynres_on();
    auto run = [&](bool texture_first, std::vector<Forward3DStats>& win, bool& has_stamps) {
        DynFixture d(desc_for(kCeiling));
        Fixture3D& f = d.f;
        has_stamps = f.device->caps().timestamp_queries || f.device->caps().fence_timestamps;
        rhi::TextureDesc td;
        td.size = {kW, kH};
        td.format = rhi::TextureFormat::RGBA16F;
        td.usage = rhi::TextureUsage::SampledColorTarget;
        td.dimension = rhi::TextureDimension::Tex2DArray;
        td.debug_name = "test.render_texture";
        const rhi::TextureHandle rt = f.device->create_texture(td);
        const u32 key = f.renderer.textures().allocate_key();
        f.renderer.textures().adopt(key, 1, rt);
        for (u32 k = 0; k < kFrames; ++k) {
            f.device->begin_frame(f.frame++);
            rhi::CommandBuffer cmd;
            Forward3D::ViewOutput tex;
            tex.slot = 1;
            tex.linear_output = true;
            tex.first = texture_first;
            Forward3D::ViewOutput w;
            w.slot = 0;
            w.first = !texture_first;
            if (texture_first) record_box(f, cmd, rt, {kW, kH}, 30.0f, tex, on);
            record_box(f, cmd, f.target, {kW, kH}, 0.0f, w, on);
            win.push_back(f.renderer.forward_3d().last_stats());
            if (!texture_first) record_box(f, cmd, rt, {kW, kH}, 30.0f, tex, on);
            f.device->submit(cmd);
            f.device->end_frame();
        }
        CHECK(f.device->validation_error_count() == 0);
        f.renderer.textures().release(key);
    };
    {  // measured
        std::vector<Forward3DStats> tex_first, win_first;
        bool stamps = false;
        run(true, tex_first, stamps);
        for (const Forward3DStats& s : tex_first) {
            CHECK(s.dynres);
            CHECK(s.render_scale == kCeiling);  // no sample: the ceiling holds and no `dynres: scale` line is logged
            CHECK(s.dynres_cost_ms == 0.0f);   // the smoothed cost was never seeded
        }
        run(false, win_first, stamps);
        bool sampled = false;
        for (const Forward3DStats& s : win_first) sampled = sampled || s.dynres_cost_ms > 0.0f;
        MESSAGE("backend timestamps: " << stamps << ", window-first run took a measured sample: " << sampled);
        if (stamps) CHECK(sampled);  // the control: the same frames stamped by the window view are samples
    }
    {  // injected: the cost is the whole frame's, so a texture-first frame still steps as the CPU controller says
        CvarScope inject("r.dynres.inject", "26@0");
        const std::vector<f32> want = cpu_sequence(on, 0.5f, kCeiling, "26@0", kFrames);
        std::vector<Forward3DStats> tex_first;
        bool stamps = false;
        run(true, tex_first, stamps);
        for (u32 k = 0; k < kFrames; ++k) {
            CAPTURE(k);
            CHECK(tex_first[k].render_scale == want[k]);
        }
        CHECK(want.back() < kCeiling);
    }
}
