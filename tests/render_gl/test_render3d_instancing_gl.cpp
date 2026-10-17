// Automatic instancing (M-VISUAL3 5a) on a real device: a grid of identical crates under three sun cascades,
// a shadowed spot light, SSAO and depth of field (so the main pass, the SSAO prepass, the cascades, a local
// shadow tile and the velocity prepass all draw runs) gives the same pixels with instancing on and off; the
// draws fall by the copies each instanced draw replaced; past the upload budget the runs draw singly and
// the frame stays the same.

#include <doctest/doctest.h>

#include "fixture3d.h"

using namespace mce;
using namespace mce::render;
using namespace mce::render_test;

namespace {

constexpr u32 kCrate = 60;

struct Frame {
    Array<u8> pixels;
    Forward3DStats stats;
    rhi::FrameStats gpu;
};

Frame render_crates(Fixture3D& f, bool instancing) {
    Frame out;
    f.render(0.0f, true, [&](Scene3D& s) {
        s.set_instancing({instancing, 2});
        add_crate_grid(s, kCrate);
        AmbientOcclusion ao;
        ao.enabled = true;
        s.set_ambient_occlusion(ao);
        PostFx post = s.post();
        post.dof.enabled = true;  // the velocity prepass runs (its depth focuses the blur)
        post.dof.focus_distance = 7.0f;
        s.set_post(post);
        LocalShadowSettings ls;
        ls.tile = 256;
        s.set_local_shadows(ls);
        PointLight spot;
        spot.position = {-2.5f, 3.5f, 2.5f};
        spot.direction = {0.6f, -1.0f, -0.5f};
        spot.radius = 12.0f;
        spot.intensity = 20.0f;
        spot.spot_inner = math::radians(25.0f);
        spot.spot_outer = math::radians(40.0f);
        spot.cast_shadows = true;
        s.add_point_light(spot);
    });
    out.pixels = f.pixels;
    out.stats = f.renderer.forward_3d().last_stats();
    out.gpu = f.gpu;
    return out;
}

u32 differing(const Array<u8>& a, const Array<u8>& b) {
    u32 n = 0;
    for (usize i = 0; i < a.size(); ++i) n += a[i] != b[i] ? 1u : 0u;
    return n;
}

}  // namespace

TEST_CASE("instancing gl: the frame with instancing equals the frame without, pixel for pixel") {
    Fixture3D f;
    upload_crate(f, kCrate);
    const Frame off = render_crates(f, false);
    const Frame on = render_crates(f, true);
    const Frame again = render_crates(f, false);
    CHECK(f.device->validation_error_count() == 0);
    REQUIRE(differing(off.pixels, again.pixels) == 0);  // the fixture itself repeats
    CHECK(differing(on.pixels, off.pixels) == 0);

    CHECK(off.stats.runs_instanced == 0);
    CHECK(off.stats.shadow_runs_instanced == 0);
    REQUIRE(on.stats.runs_instanced >= 2);  // the crates' bodies and lids
    CHECK(on.stats.shadow_runs_instanced >= 2);
    CHECK(on.stats.instancing_overflow == 0);
    CHECK(on.stats.instancing_bytes > 0);
    CHECK(on.stats.local_shadow_tiles == 1);
    CHECK(on.stats.ssao);
    CHECK(on.stats.dof);
    // Each instanced draw replaced `count` draws: main_draws falls by the copies folded into it, and the
    // device's draw count by those of the main pass, the SSAO and velocity prepasses and the shadow passes.
    const u32 folded = on.stats.draws_instanced - on.stats.runs_instanced;
    const u32 shadow_folded = on.stats.shadow_draws_instanced - on.stats.shadow_runs_instanced;
    CHECK(on.stats.main_draws == off.stats.main_draws - folded);
    CHECK(on.stats.ssao_draws == off.stats.ssao_draws - folded);
    CHECK(on.stats.velocity_draws == off.stats.velocity_draws - folded);
    CHECK(on.stats.shadow_draws == off.stats.shadow_draws - shadow_folded);
    CHECK(on.gpu.draws == off.gpu.draws - 3 * folded - shadow_folded);
}

TEST_CASE("instancing gl: past the upload budget the runs draw singly with equal pixels") {
    Frame reference;
    {
        Fixture3D f;
        upload_crate(f, kCrate);
        reference = render_crates(f, false);
    }
    for (const u32 budget : {0u, 3000u}) {
        CAPTURE(budget);
        Forward3DDesc fd;
        fd.instancing_budget = budget;
        Fixture3D f(4, true, true, 1.0f, &fd);
        upload_crate(f, kCrate);
        const Frame on = render_crates(f, true);
        CHECK(f.device->validation_error_count() == 0);
        CHECK(on.stats.instancing_overflow > 0);
        CHECK(on.stats.instancing_bytes <= budget);
        CHECK(differing(on.pixels, reference.pixels) == 0);
        const u32 folded = on.stats.draws_instanced - on.stats.runs_instanced;
        CHECK(on.stats.main_draws == reference.stats.main_draws - folded);
        if (budget == 0) {
            CHECK(on.stats.runs_instanced == 0);
            CHECK(on.stats.shadow_runs_instanced == 0);
            CHECK(on.gpu.draws == reference.gpu.draws);
        } else {
            CHECK(on.stats.runs_instanced + on.stats.shadow_runs_instanced > 0);  // some fit
        }
    }
}

TEST_CASE("instancing gl: a run longer than the budget draws the copies that fit instanced, the rest singly") {
    // M-OPENWORLD A3: a city's one long run (22k copies of one mesh) used to need its whole block in the budget
    // and drew every copy singly when it did not fit. A 1.5 KiB budget holds 12 main copies of the ~18 in each
    // crate run: the first 12 draw as one instanced draw, the tail singly, and the frame is unchanged.
    Frame reference;
    {
        Fixture3D f;
        upload_crate(f, kCrate);
        reference = render_crates(f, false);
    }
    Forward3DDesc fd;
    fd.instancing_budget = 1536;
    Fixture3D f(4, true, true, 1.0f, &fd);
    upload_crate(f, kCrate);
    const Frame on = render_crates(f, true);
    CHECK(f.device->validation_error_count() == 0);
    CHECK(on.stats.instancing_overflow > 0);
    CHECK(on.stats.instancing_bytes <= 1536);
    REQUIRE(on.stats.runs_instanced >= 1);
    CHECK(on.stats.draws_instanced <= 12);
    CHECK(on.stats.draws_instanced >= 2);
    CHECK(differing(on.pixels, reference.pixels) == 0);
    const u32 folded = on.stats.draws_instanced - on.stats.runs_instanced;
    CHECK(on.stats.main_draws == reference.stats.main_draws - folded);
}

TEST_CASE("instancing gl: a small geometry ring caps the budget; main is served first, the rest draws singly") {
    Frame reference;
    {
        Fixture3D f;
        upload_crate(f, kCrate);
        reference = render_crates(f, false);
    }
    // A 32 KiB ring gives instancing 8 KiB of it for the frame: the main pass's reservation fits, the
    // passes after it overflow into single draws. Without the cap the uploads would overrun the ring, which
    // drops them without binding and leaves the instanced draws reading stale matrices.
    rhi::DeviceDesc dd;
    dd.geometry_ring_bytes = 32 * KiB;
    Forward3DDesc fd;
    fd.geometry_ring_bytes = dd.geometry_ring_bytes;
    Fixture3D f(4, true, true, 1.0f, &fd, &dd);
    upload_crate(f, kCrate);
    const Frame on = render_crates(f, true);
    CHECK(f.device->validation_error_count() == 0);
    CHECK(on.stats.instancing_overflow > 0);
    CHECK(on.stats.instancing_bytes <= dd.geometry_ring_bytes / 4);
    CHECK(on.stats.runs_instanced >= 2);  // the main pass is reserved first
    CHECK(differing(on.pixels, reference.pixels) == 0);
}

TEST_CASE("instancing gl: r.instancing's desc switch off draws the runs singly") {
    Frame reference;
    {
        Fixture3D f;
        upload_crate(f, kCrate);
        reference = render_crates(f, true);
    }
    Forward3DDesc fd;
    fd.instancing = false;
    Fixture3D f(4, true, true, 1.0f, &fd);
    upload_crate(f, kCrate);
    const Frame on = render_crates(f, true);  // the scene still forms its runs
    CHECK(on.stats.runs_instanced == 0);
    CHECK(on.stats.instancing_bytes == 0);
    CHECK(differing(on.pixels, reference.pixels) == 0);
}

// ---- the draw-call report (M-CONTENT 2) ---------------------------------------------------------------

namespace {

u32 cause(const Forward3DStats& st, DrawCause c) { return st.draw_causes[static_cast<u32>(c)]; }

u32 causes_total(const Forward3DStats& st) {
    u32 n = 0;
    for (u32 c : st.draw_causes) n += c;
    return n;
}

// The fixture draws the ground and the box (two meshes drawn once each) plus whatever `extra` adds.
Forward3DStats render_causes(Fixture3D& f, bool instancing, const std::function<void(Scene3D&)>& extra) {
    f.render(0.0f, false, [&](Scene3D& s) {
        s.set_instancing({instancing, 2});
        s.set_collect_draw_causes(true);
        add_crate_grid(s, kCrate);
        if (extra) extra(s);
    });
    return f.renderer.forward_3d().last_stats();
}

}  // namespace

TEST_CASE("draw causes gl: repeated meshes instance; a unique material and a unique mesh are named") {
    Fixture3D f;
    upload_crate(f, kCrate);
    const Forward3DStats base = render_causes(f, true, {});
    CHECK(f.device->validation_error_count() == 0);
    REQUIRE(base.runs_instanced == 2);  // the crates' bodies and lids
    CHECK(base.draws_instanced % 2 == 0);
    CHECK(cause(base, DrawCause::UniqueMesh) == 2);  // the ground and the box
    CHECK(causes_total(base) == 2);
    CHECK(base.main_draws == 4);  // two runs and two single draws

    // One more crate in another tint: it shares its mesh with the grid but not a run.
    const Forward3DStats tinted = render_causes(f, true, [](Scene3D& s) {
        MeshInstance m;
        m.mesh = kCrate;
        m.world = math::trs({0.0f, 0.0f, 0.0f}, math::quat_from_axis_angle({0, 1, 0}, 0.0f), {1, 1, 1});
        m.tint = {1.0f, 0.2f, 0.2f, 1.0f};
        s.add(m);
    });
    CHECK(tinted.runs_instanced == 2);
    CHECK(tinted.draws_instanced == base.draws_instanced);
    CHECK(cause(tinted, DrawCause::Material) == 2);  // its body and its lid
    CHECK(cause(tinted, DrawCause::UniqueMesh) == 2);
    CHECK(causes_total(tinted) == 4);

    // A material override is another material: the same two draws break away from the runs.
    MaterialOverride other;
    other.params.base_color[0] = 0.1f;
    const Forward3DStats overridden = render_causes(f, true, [&](Scene3D& s) {
        MeshInstance m;
        m.mesh = kCrate;
        m.world = math::trs({0.0f, 0.0f, 0.0f}, math::quat_from_axis_angle({0, 1, 0}, 0.0f), {1, 1, 1});
        m.material = &other;
        s.add(m);
    });
    CHECK(overridden.runs_instanced == 2);
    CHECK(cause(overridden, DrawCause::Material) == 2);
}

TEST_CASE("draw causes gl: instancing off, no instanced pipelines and a spent budget are told apart") {
    u32 grid_draws = 0;
    {
        Fixture3D f;
        upload_crate(f, kCrate);
        const Forward3DStats on = render_causes(f, true, {});
        grid_draws = on.draws_instanced;
        REQUIRE(grid_draws >= 4);
        CHECK(cause(on, DrawCause::Disabled) == 0);
        CHECK(cause(on, DrawCause::Budget) == 0);

        const Forward3DStats off = render_causes(f, false, {});  // the scene forms no runs
        CHECK(off.runs_instanced == 0);
        CHECK(cause(off, DrawCause::Disabled) == grid_draws + 2);
        CHECK(causes_total(off) == off.main_draws);
        CHECK(cause(off, DrawCause::UniqueMesh) == 0);
    }
    {
        Forward3DDesc fd;
        fd.instancing = false;  // -r.instancing 0: the scene's runs exist, the renderer draws none of them
        Fixture3D f(4, true, true, 1.0f, &fd);
        upload_crate(f, kCrate);
        const Forward3DStats st = render_causes(f, true, {});
        CHECK(st.runs_instanced == 0);
        CHECK(cause(st, DrawCause::Disabled) == grid_draws);
        CHECK(cause(st, DrawCause::UniqueMesh) == 2);
        CHECK(causes_total(st) == st.main_draws);
    }
    {
        Forward3DDesc fd;
        fd.instancing_budget = 0;
        Fixture3D f(4, true, true, 1.0f, &fd);
        upload_crate(f, kCrate);
        const Forward3DStats st = render_causes(f, true, {});
        CHECK(st.runs_instanced == 0);
        CHECK(st.draws_instanced == 0);
        CHECK(cause(st, DrawCause::Budget) == grid_draws);
        CHECK(cause(st, DrawCause::UniqueMesh) == 2);
        CHECK(causes_total(st) == st.main_draws);
    }
}

TEST_CASE("draw causes gl: the causes plus the drawn instanced draws add up to the scene's draws in every setup") {
    u32 scene_draws = 0;  // the scene's main-view draws: instancing off in the scene counts every one as Disabled
    {
        Fixture3D f;
        upload_crate(f, kCrate);
        const Forward3DStats off = render_causes(f, false, {});
        scene_draws = causes_total(off);
        REQUIRE(scene_draws > 4);
        CHECK(off.draws_instanced == 0);
        const Forward3DStats on = render_causes(f, true, {});
        CHECK(on.draws_instanced > 0);
        CHECK(causes_total(on) + on.draws_instanced == scene_draws);
        // Asked not to classify, a frame reports no causes at all but the same drawn instanced numbers.
        f.render(0.0f, false, [&](Scene3D& s) {
            s.set_instancing({true, 2});
            add_crate_grid(s, kCrate);
        });
        const Forward3DStats quiet = f.renderer.forward_3d().last_stats();
        CHECK(causes_total(quiet) == 0);
        CHECK(quiet.draws_instanced == on.draws_instanced);
    }
    {  // the frame's budget is spent: Scene3D still plans runs, the renderer draws none; the player reads the renderer's
        Forward3DDesc fd;
        fd.instancing_budget = 0;
        Fixture3D f(4, true, true, 1.0f, &fd);
        upload_crate(f, kCrate);
        const Forward3DStats st = render_causes(f, true, {});
        CHECK(st.draws_instanced == 0);
        CHECK(causes_total(st) + st.draws_instanced == scene_draws);
    }
    {  // a budget that holds some runs only
        Forward3DDesc fd;
        fd.instancing_budget = 3000;
        Fixture3D f(4, true, true, 1.0f, &fd);
        upload_crate(f, kCrate);
        const Forward3DStats st = render_causes(f, true, {});
        CHECK(causes_total(st) + st.draws_instanced == scene_draws);
    }
    {  // no instanced pipelines
        Forward3DDesc fd;
        fd.instancing = false;
        Fixture3D f(4, true, true, 1.0f, &fd);
        upload_crate(f, kCrate);
        const Forward3DStats st = render_causes(f, true, {});
        CHECK(st.draws_instanced == 0);
        CHECK(causes_total(st) + st.draws_instanced == scene_draws);
    }
}
