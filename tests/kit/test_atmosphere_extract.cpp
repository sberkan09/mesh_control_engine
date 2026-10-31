// The atmosphere next to a day cycle and the moon in Kit3DExtract (M-OPENWORLD-LOOK): a DayCycle that is writing the
// environment keeps the ambient (the atmosphere then draws the sky and tints the sun only), and while the moon holds the
// directional light the atmosphere steps aside and the sky clears to the fog colour instead of painting daylight around
// the moon. Without a DayCycle, or with the sun up, the atmosphere behaves as it always did.

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
