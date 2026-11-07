// Kit-side check of the camera under water (M-UNDERWATER 44d; no component). The underwater pass reads only what the
// extract already feeds: Scene3D::water() from the first Water (M-WATER 15b), the camera, the sun and PostFx::underwater,
// which the kit never sets (the defaults, through apply_quality). These cases show that feed reaches the pass's inputs
// unchanged with the camera above, at and below the level; that the extract writes nothing to the world (the state hash
// over a run is the same with the camera above or below the surface, and the same as before the run, with and without a
// Water); that a floating-origin rebase moves the camera and the render-relative level together, so underwater_frame
// agrees before and after; and that a Water with enabled 0 or r.water 0 gives no underwater frame.

#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <utility>

#include "mce/convar.h"
#include "mce/ecs/origin.h"
#include "mce/ecs/save.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/kit3d_extract.h"
#include "mce/kit/water.h"
#include "mce/kit/weather.h"
#include "mce/render/quality.h"
#include "mce/render/scene3d.h"
#include "mce/render/underwater.h"
#include "mce/string_id.h"
#include "mesh_fixture.h"

using namespace mce;
using namespace mce::kit;

namespace {

bool same_bits(f32 a, f32 b) { return std::bit_cast<u32>(a) == std::bit_cast<u32>(b); }
bool same_vec(const math::vec3& a, const math::vec3& b) {
    return same_bits(a.x, b.x) && same_bits(a.y, b.y) && same_bits(a.z, b.z);
}
bool same_water(const render::WaterSurface& a, const render::WaterSurface& b) {
    return same_bits(a.level_y, b.level_y) && same_bits(a.half_extent, b.half_extent) && a.wave_count == b.wave_count &&
           same_bits(a.amplitude, b.amplitude) && same_bits(a.wavelength, b.wavelength) &&
           same_bits(a.steepness, b.steepness) && same_bits(a.wind_dir.x, b.wind_dir.x) &&
           same_bits(a.wind_dir.y, b.wind_dir.y) && same_bits(a.time, b.time) && same_bits(a.speed_scale, b.speed_scale) &&
           same_vec(a.shallow_color, b.shallow_color) && same_vec(a.deep_color, b.deep_color) &&
           same_bits(a.depth_scale_m, b.depth_scale_m) && same_bits(a.reflectivity, b.reflectivity) &&
           same_bits(a.roughness, b.roughness) && same_bits(a.foam_amount, b.foam_amount) &&
           same_bits(a.foam_width_m, b.foam_width_m) && a.enabled == b.enabled;
}
bool default_underwater(const render::Underwater& u) {
    const render::Underwater d;
    return u.enabled == d.enabled && same_bits(u.strength, d.strength) && same_bits(u.wobble, d.wobble) &&
           same_bits(u.fog_scale, d.fog_scale) && same_bits(u.band, d.band);
}

struct Rig {
    render::MeshStore store;
    World world;
    Kit3DExtract extract;
    render::Scene3D scene;
    render::Camera3D cam;
    Entity camera_e;
    Entity water_e;

    Rig() : scene(store, mem::heap()) {
        mesh_fixture::Builder box;
        box.add_node(-1);
        mesh_fixture::Box b;
        b.half = {0.5f, 0.5f, 0.5f};
        box.add_box(b);
        const Array<byte> blob = box.build();
        REQUIRE(store.upload(1, 1, blob.span()).has_value());
        world.register_components<SaveKey>();
        install_kit3d(world, nullptr);
        install_weather(world, nullptr);
        extract.init(store, nullptr);
        extract.set_frame_dt(0.25f);
        camera_e = world.spawn_with(SaveKey{1}, LocalTransform{}, Camera3DComponent{});
    }
    void add_water(const Water& w) { water_e = world.spawn_with(SaveKey{2}, w); }
    void camera_at(f32 y) { world.try_get_mut<LocalTransform>(camera_e)->position = {0.0f, y, 4.0f}; }
    // One extract frame; returns the scene.
    const render::Scene3D& frame() {
        REQUIRE(extract.camera(world, 16.0f / 9.0f, cam));
        scene.begin(cam, {320, 180});
        extract.extract(world, scene, 1.0f);
        return scene;
    }
    // The floating origin Forward3D would hold (the cells times the snap, in f64), zero without a WorldOrigin.
    void origin(f64 out[3]) {
        const WorldOrigin* o = world_origin(world);
        for (int i = 0; i < 3; ++i) out[i] = o ? f64(o->cell[i]) * f64(o->snap) : 0.0;
    }
    bool underwater(render::UnderwaterFrame& fr, u32 detail = 2) {
        f64 org[3];
        origin(org);
        return render::underwater_frame(scene.water(), scene.post().underwater, scene.view(), org, scene.light(),
                                        scene.time(), 16.0f / 9.0f, detail, fr);
    }
};

Water calm_water(f32 level) {
    Water w;
    w.level_y = level;
    w.amplitude = 0.2f;
    w.shallow_color = {0.12f, 0.40f, 0.42f};
    w.deep_color = {0.02f, 0.09f, 0.16f};
    w.depth_scale_m = 10.0f;
    return w;
}

}  // namespace

TEST_CASE("underwater extract: the Water feed reaches Scene3D::water() unchanged above, at and below the level") {
    // Three rigs, the same Water, the camera 3 m above, at and 3 m below the level: the carrier is the same bit for bit
    // (the camera height feeds nothing into it), PostFx::underwater keeps its defaults, and only the camera decides
    // whether the pass runs.
    const f32 heights[3] = {5.0f, 2.0f, -1.0f};
    render::WaterSurface carriers[3];
    bool runs[3] = {};
    for (int i = 0; i < 3; ++i) {
        Rig r;
        r.add_water(calm_water(2.0f));
        r.camera_at(heights[i]);
        const render::Scene3D& s = r.frame();
        carriers[i] = s.water();
        CHECK(s.water().enabled);
        CHECK(same_bits(s.water().level_y, 2.0f));
        CHECK(same_vec(s.water().shallow_color, math::vec3{0.12f, 0.40f, 0.42f}));
        CHECK(same_vec(s.water().deep_color, math::vec3{0.02f, 0.09f, 0.16f}));
        CHECK(same_bits(s.water().depth_scale_m, 10.0f));
        CHECK(default_underwater(s.post().underwater));
        render::UnderwaterFrame fr;
        runs[i] = r.underwater(fr);
        if (runs[i]) {
            // cam_depth is the render-relative mean level minus the camera height (no origin here).
            CHECK(fr.cam_depth == doctest::Approx(2.0f - heights[i]));
            CHECK(fr.submersion > 0.0f);
        }
    }
    CHECK(same_water(carriers[0], carriers[1]));
    CHECK(same_water(carriers[0], carriers[2]));
    CHECK_FALSE(runs[0]);  // 3 m above: nothing
    CHECK(runs[1]);        // at the level: the middle of the crossing band
    CHECK(runs[2]);        // 3 m below: the full look
    Rig mid;
    mid.add_water(calm_water(2.0f));
    mid.camera_at(2.0f);
    mid.frame();
    render::UnderwaterFrame fm;
    REQUIRE(mid.underwater(fm));
    CHECK(fm.submersion == doctest::Approx(0.5f));
    Rig deep;
    deep.add_water(calm_water(2.0f));
    deep.camera_at(-1.0f);
    deep.frame();
    render::UnderwaterFrame fd;
    REQUIRE(deep.underwater(fd));
    CHECK(same_bits(fd.submersion, 1.0f));
    // The pass's colours are the helpers of the carrier the extract handed over.
    const math::vec3 sigma = render::underwater_sigma(deep.scene.water().shallow_color, 10.0f, 1.0f);
    CHECK(same_vec(fd.sigma, sigma));
    CHECK(same_vec(fd.scatter, render::underwater_scatter(deep.scene.water(), fd.cam_depth,
                                                          render::underwater_light(deep.scene.light()))));
}

TEST_CASE("underwater extract: nothing is written to the world; the hash over a run does not depend on the camera") {
    // A run of 40 extract frames with the camera below the surface leaves the state hash where it started, and where the
    // same run with the camera above leaves it (the camera's own height is put back first, so the worlds compare); the
    // same holds for a world without a Water.
    auto run = [](bool water, f32 cam_y) {
        Rig r;
        if (water) r.add_water(calm_water(0.0f));
        r.camera_at(cam_y);
        r.frame();
        r.camera_at(0.0f);
        const u64 before = r.world.state_hash();
        r.camera_at(cam_y);
        u32 frames_under = 0;
        for (int i = 0; i < 40; ++i) {
            r.frame();
            render::UnderwaterFrame fr;
            frames_under += r.underwater(fr) ? 1u : 0u;
        }
        r.camera_at(0.0f);
        const u64 after = r.world.state_hash();
        CHECK(after == before);
        return std::pair<u64, u32>{after, frames_under};
    };
    const auto below = run(true, -3.0f);
    const auto above = run(true, 3.0f);
    CHECK(below.second == 40u);
    CHECK(above.second == 0u);
    CHECK(below.first == above.first);
    const auto dry_below = run(false, -3.0f);
    const auto dry_above = run(false, 3.0f);
    CHECK(dry_below.second == 0u);  // no Water: never under water
    CHECK(dry_below.first == dry_above.first);
}

TEST_CASE("underwater extract: a floating-origin rebase moves the camera and the level together") {
    // The carrier's level is absolute and the camera render-relative: after a rebase that moves the world down one cell
    // (and along x and z), underwater_frame with the moved origin gives the same camera depth and submersion as before.
    Rig r;
    install_world_origin(r.world, 256.0f);
    r.add_water(calm_water(1.5f));
    r.camera_at(0.25f);  // 1.25 m below the mean surface
    r.frame();
    render::UnderwaterFrame before;
    REQUIRE(r.underwater(before));
    CHECK(before.cam_depth == doctest::Approx(1.25f));

    const i32 cells[3] = {1, 1, -1};
    rebase_world(r.world, cells);
    r.frame();
    CHECK(same_vec(r.scene.origin_shift(), math::vec3{256.0f, 256.0f, -256.0f}));
    CHECK(r.scene.view().position.y == doctest::Approx(0.25f - 256.0f));
    CHECK(same_bits(r.scene.water().level_y, 1.5f));  // absolute: the extract does not move it
    render::UnderwaterFrame after;
    REQUIRE(r.underwater(after));
    CHECK(after.cam_depth == doctest::Approx(before.cam_depth).epsilon(1e-4));
    CHECK(after.submersion == doctest::Approx(before.submersion).epsilon(1e-4));
    CHECK(same_vec(after.sigma, before.sigma));
    CHECK(same_vec(after.forward, before.forward));
}

TEST_CASE("underwater extract: Water enabled 0 or r.water 0 gives no underwater frame") {
    Rig off;
    Water w = calm_water(2.0f);
    w.enabled = 0;
    off.add_water(w);
    off.camera_at(-2.0f);
    off.frame();
    CHECK_FALSE(off.scene.water().enabled);
    render::UnderwaterFrame fr;
    CHECK_FALSE(off.underwater(fr));

    Rig cv_off;
    cv_off.add_water(calm_water(2.0f));
    cv_off.camera_at(-2.0f);
    cv_off.frame();
    CHECK(cv_off.underwater(fr));
    ConVar* cv = ConVar::find(StringId("r.water"));
    REQUIRE(cv != nullptr);
    cv->set_float(0.0f);
    cv_off.frame();
    const bool water_on = cv_off.scene.water().enabled;
    const bool frame_on = cv_off.underwater(fr);
    cv->reset();
    CHECK_FALSE(water_on);
    CHECK_FALSE(frame_on);
    cv_off.frame();
    CHECK(cv_off.underwater(fr));
}

TEST_CASE("underwater extract: the kit hands the defaults; r.underwater 0 is the renderer's (and apply_quality's) gate") {
    // The extract never writes PostFx::underwater (no component): it stays the default whatever r.underwater says, and
    // Forward3D::record_underwater reads the tier and the cvar itself. apply_quality on the same PostFx clears `enabled`,
    // after which underwater_frame says no.
    Rig r;
    r.add_water(calm_water(2.0f));
    r.camera_at(-2.0f);
    r.frame();
    render::UnderwaterFrame fr;
    CHECK(r.underwater(fr));
    ConVar* cv = ConVar::find(StringId("r.underwater"));
    REQUIRE(cv != nullptr);
    cv->set_float(0.0f);
    r.frame();
    const bool handed_default = default_underwater(r.scene.post().underwater);
    render::PostFx post = r.scene.post();
    render::ShadowSettings shadows;
    render::apply_quality(render::quality_settings(render::QualityTier::High), shadows, post);
    f64 org[3] = {0.0, 0.0, 0.0};
    const bool frame_on = render::underwater_frame(r.scene.water(), post.underwater, r.scene.view(), org, r.scene.light(),
                                                   r.scene.time(), 16.0f / 9.0f, 2, fr);
    cv->reset();
    CHECK(handed_default);
    CHECK_FALSE(post.underwater.enabled);
    CHECK_FALSE(frame_on);
}
