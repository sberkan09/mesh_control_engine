// Static merge (M-VISUAL3 5b) on a real device: a modular kit drawn as its pieces and as two merged cells gives the
// same frame within the re-quantization's tolerance (sun, three cascades, SSAO and a shadowed spot light so the main
// pass, the prepasses and the shadow passes all draw it), and the merged cells draw once per material and cell in
// every pass.

#include <doctest/doctest.h>

#include "fixture3d.h"

using namespace mce;
using namespace mce::render;
using namespace mce::render_test;

namespace {

struct Frame {
    Array<u8> pixels;
    Forward3DStats stats;
    rhi::FrameStats gpu;
};

enum class Kit { None, Pieces, Merged };

Frame render_kit(Fixture3D& f, const MergeKit& kit, Kit what) {
    Frame out;
    f.render(0.0f, true, [&](Scene3D& s) {
        s.set_instancing({false, 2});  // count draws one by one
        if (what != Kit::None) add_merge_kit(s, kit, what == Kit::Merged);
        AmbientOcclusion ao;
        ao.enabled = true;
        s.set_ambient_occlusion(ao);
        LocalShadowSettings ls;
        ls.tile = 256;
        s.set_local_shadows(ls);
        PointLight spot;
        spot.position = {-2.0f, 3.5f, 2.5f};
        spot.direction = {0.5f, -1.0f, -0.7f};
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

}  // namespace

TEST_CASE("static merge gl: the merged kit matches the pieces within the quantization tolerance") {
    Fixture3D f;
    f.pitch_deg = 30.0f;
    MergeKit kit;
    upload_merge_kit(f, kit);
    const Frame pieces = render_kit(f, kit, Kit::Pieces);
    const Frame merged = render_kit(f, kit, Kit::Merged);
    const Frame again = render_kit(f, kit, Kit::Pieces);
    CHECK(f.device->validation_error_count() == 0);
    REQUIRE(pieces.pixels.size() == merged.pixels.size());
    for (usize i = 0; i < pieces.pixels.size(); ++i) REQUIRE(pieces.pixels[i] == again.pixels[i]);  // the fixture repeats

    // Half a quantum is 0.5 mm at 32 m and far less here: a few edge pixels may move by a step, nothing else.
    u64 sum = 0;
    u32 max_diff = 0, over = 0;
    for (usize i = 0; i < pieces.pixels.size(); ++i) {
        const u32 d = static_cast<u32>(std::abs(int(pieces.pixels[i]) - int(merged.pixels[i])));
        sum += d;
        max_diff = std::max(max_diff, d);
        over += d > 8 ? 1u : 0u;
    }
    const f64 mean = f64(sum) / f64(pieces.pixels.size());
    MESSAGE("mean diff ", mean, ", max ", max_diff, ", bytes over 8: ", over);
    CHECK(mean < 0.25);
    CHECK(over * 200 < pieces.pixels.size());  // under 0.5 % of the bytes differ by more than 8
    // And the kit is really in the frame: the empty frame differs from it a lot.
    const Frame empty = render_kit(f, kit, Kit::None);
    u64 kit_sum = 0;
    for (usize i = 0; i < empty.pixels.size(); ++i) kit_sum += static_cast<u32>(std::abs(int(empty.pixels[i]) - int(pieces.pixels[i])));
    CHECK(f64(kit_sum) / f64(empty.pixels.size()) > 10.0 * mean);
}

TEST_CASE("static merge gl: draws per pass equal materials x cells") {
    Fixture3D f;
    f.pitch_deg = 30.0f;
    MergeKit kit;
    upload_merge_kit(f, kit);
    const Frame none = render_kit(f, kit, Kit::None);
    const Frame pieces = render_kit(f, kit, Kit::Pieces);
    const Frame merged = render_kit(f, kit, Kit::Merged);
    const u32 mc = kit.materials * kit.cells;
    CHECK(pieces.stats.main_draws == none.stats.main_draws + u32(kit.placements.size()));
    CHECK(merged.stats.main_draws == none.stats.main_draws + mc);
    CHECK(merged.stats.ssao_draws == none.stats.ssao_draws + mc);
    CHECK(merged.stats.cascades == 3);
    // Each cascade and shadow tile draws a (material, cell) at most once, and far fewer than the pieces would.
    const u32 extra_shadow = merged.stats.shadow_draws - none.stats.shadow_draws;
    CHECK(extra_shadow > 0);
    CHECK(extra_shadow <= (merged.stats.cascades + merged.stats.local_shadow_tiles) * mc);
    CHECK(pieces.stats.shadow_draws - none.stats.shadow_draws > extra_shadow);
    CHECK(merged.gpu.draws < pieces.gpu.draws);
}
