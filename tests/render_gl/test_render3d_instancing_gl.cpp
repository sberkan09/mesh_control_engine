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
