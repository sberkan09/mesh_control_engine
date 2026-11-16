// The atmosphere next to a day cycle and the moon in Kit3DExtract (M-OPENWORLD-LOOK, M-OPENWORLD-LIGHT): a DayCycle that is
// writing the environment keeps the ambient (the atmosphere then draws the sky and tints the sun only). While the moon holds
// the directional light the atmosphere's sky follows the sun itself with a DayCycle (Atmosphere::sky_sun_direction, plus a
// fog-coloured twilight floor), and without one it steps aside and the sky clears to the fog colour. With a DayCycle the sky
// IBL's diffuse is scaled to the DayCycle's ambient. Without a DayCycle, or with the sun up, the atmosphere behaves as it
// always did.

#include <doctest/doctest.h>

#include "mce/ecs/save.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/day_cycle.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/kit3d_extract.h"
#include "mce/kit/moon.h"
#include "mce/kit/weather.h"
#include "mce/kit/world_clock.h"
#include "mce/render/scene3d.h"
#include "mesh_fixture.h"

using namespace mce;
using namespace mce::kit;

namespace {

struct Rig {
    render::MeshStore store;
    World world;
    Kit3DExtract extract;
    render::Scene3D scene;
    render::Camera3D cam;

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
        install_day_cycle(world, nullptr);
        extract.init(store, nullptr);
        world.spawn_with(LocalTransform{}, Camera3DComponent{});
        world.spawn_with(SaveKey{1}, WorldClock{});
        env().atmosphere = 1;
        env().sky_enabled = 1;
        REQUIRE(extract.camera(world, 1.0f, cam));
    }
    Environment3D& env() { return world.resource<Environment3D>(); }
    void frame(f32 hour) {
        env().time_of_day = hour;
        scene.begin(cam, {320, 180});
        extract.extract(world, scene, 1.0f);
    }
};

}  // namespace

TEST_CASE("atmosphere extract: a DayCycle owns the ambient, without one the atmosphere sets it") {
    Rig r;
    r.frame(12.0f);
    CHECK(r.scene.atmosphere().enabled);
    CHECK(r.scene.atmosphere().ambient);  // no DayCycle in the world
    r.world.spawn_with(SaveKey{2}, DayCycle{});
    r.frame(12.0f);
    CHECK(r.scene.atmosphere().enabled);  // the sky is still the atmosphere's
    CHECK_FALSE(r.scene.atmosphere().ambient);
    DayCycle off;
    off.enabled = 0;
    Rig q;
    q.world.spawn_with(SaveKey{2}, off);
    q.frame(12.0f);
    CHECK(q.scene.atmosphere().ambient);  // a disabled DayCycle writes nothing, so it owns nothing
    Rig p;
    p.world.spawn_with(SaveKey{2}, DayCycle{});
    p.frame(-1.0f);
    CHECK(p.scene.atmosphere().ambient);  // no clock hour: the day cycle does nothing either
}

TEST_CASE("atmosphere extract: the atmosphere yields to the moon at night and returns with the sun") {
    Rig r;
    r.world.spawn_with(SaveKey{2}, Moon{});
    r.frame(0.0f);  // midnight, a moon above the horizon holds the light
    CHECK_FALSE(r.scene.atmosphere().enabled);
    CHECK_FALSE(r.scene.sky().enabled);
    CHECK(r.env().atmosphere == 1);  // Environment3D is never written
    CHECK(r.env().sky_enabled == 1);
    r.frame(12.0f);
    CHECK(r.scene.atmosphere().enabled);
    CHECK(r.scene.sky().enabled);
    Rig n;  // no Moon: the atmosphere stays on at midnight as before
    n.frame(0.0f);
    CHECK(n.scene.atmosphere().enabled);
    CHECK(n.scene.sky().enabled);
}

TEST_CASE("atmosphere extract: auto exposure's clamps reach the post chain") {
    Rig r;
    r.env().auto_exposure = 1;
    r.frame(12.0f);
    CHECK(r.scene.post().auto_exposure.min_exposure == doctest::Approx(0.03f));  // the defaults are PostFx's
    CHECK(r.scene.post().auto_exposure.max_exposure == doctest::Approx(32.0f));
    r.env().auto_exposure_min = 0.5f;
    r.env().auto_exposure_max = 1.5f;
    r.frame(12.0f);
    CHECK(r.scene.post().auto_exposure.min_exposure == doctest::Approx(0.5f));
    CHECK(r.scene.post().auto_exposure.max_exposure == doctest::Approx(1.5f));
}

TEST_CASE("atmosphere extract: with a DayCycle the sky follows the sun through the moon's hand-off") {
    Rig r;
    r.world.spawn_with(SaveKey{2}, DayCycle{});
    r.world.spawn_with(SaveKey{3}, Moon{});
    r.env().fog_color = {0.2f, 0.3f, 0.6f};
    r.frame(12.0f);  // noon: as before, no override and no floor
    CHECK(r.scene.atmosphere().enabled);
    CHECK(math::length_sq(r.scene.atmosphere().sky_sun_direction) == 0.0f);
    CHECK_FALSE(r.scene.sky().texture_additive);
    r.frame(0.0f);  // midnight: the moon holds the light, the sky stays the atmosphere's, lit by the sun under the ground
    CHECK(r.scene.atmosphere().enabled);
    CHECK(r.scene.sky().enabled);
    const math::vec3 s = r.scene.atmosphere().sky_sun_direction;
    const math::vec3 natural = render::sun_direction_at(0.0f);
    CHECK(s.x == doctest::Approx(natural.x));
    CHECK(s.y == doctest::Approx(natural.y));
    CHECK(s.z == doctest::Approx(natural.z));
    CHECK(s.y > 0.0f);  // the light travels up: the sun is below the horizon
    CHECK(r.scene.light().direction.y < 0.0f);  // the moonlight comes down from above
    // The twilight floor: the fog colour at the horizon, darker at the zenith, added under the atmosphere's sky.
    CHECK(r.scene.sky().texture_additive);
    CHECK(r.scene.sky().horizon.z == doctest::Approx(0.6f));
    CHECK(r.scene.sky().zenith.z < r.scene.sky().horizon.z);
    CHECK(r.env().atmosphere == 1);  // Environment3D is never written
    CHECK(r.env().sky_texture_additive == 0);
}

TEST_CASE("atmosphere extract: the sky IBL's diffuse follows the DayCycle's ambient") {
    Rig r;
    r.env().sky_ibl = 1;
    r.frame(12.0f);
    CHECK(r.scene.sky().diffuse_ambient == doctest::Approx(1.0f));  // no DayCycle: the IBL as it was
    r.world.spawn_with(SaveKey{2}, DayCycle{});
    r.env().ambient_sky = {1.0f, 1.1f, 1.3f};
    r.frame(12.0f);
    const f32 bright = r.scene.sky().diffuse_ambient;
    CHECK(bright > 1.0f);  // the single-scattering sky is dimmer than the authored ambient
    CHECK(bright <= 32.0f);
    r.env().ambient_sky = {0.5f, 0.55f, 0.65f};
    r.frame(12.0f);
    CHECK(r.scene.sky().diffuse_ambient == doctest::Approx(bright * 0.5f).epsilon(1e-3));
    r.env().sky_ibl = 0;
    r.frame(12.0f);
    CHECK(r.scene.sky().diffuse_ambient == doctest::Approx(1.0f));
}
