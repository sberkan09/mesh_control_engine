// M-PARTICLEUSER 29b: Kit3DExtract scales the weather rain and snow emitter (an entity with Precipitation3D) by
// QualitySettings::particle_scale, render-side only. A plain ParticleEmitter3D is never scaled.

#include <doctest/doctest.h>

#include "mce/ecs/save.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/kit3d_extract.h"
#include "mce/kit/precipitation.h"
#include "mce/kit/weather.h"
#include "mce/render/quality.h"
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
    Entity rain_e, plain_e;

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
        world.spawn_with(LocalTransform{}, Camera3DComponent{});
        REQUIRE(extract.camera(world, 1.0f, cam));
        ParticleEmitter3D pe;
        pe.rate = 400.0f;
        pe.max_particles = 800;
        rain_e = world.spawn_with(SaveKey{2}, LocalTransform{}, pe, Precipitation3D{});
        plain_e = world.spawn_with(SaveKey{3}, LocalTransform{}, pe);
    }
    Array<render::ParticleEmitter> frame() {
        scene.begin(cam, {320, 180});
        extract.extract(world, scene, 1.0f);
        Array<render::ParticleEmitter> out;
        for (const render::ParticleEmitter& e : scene.particle_emitters()) out.push_back(e);
        return out;
    }
};

}  // namespace

TEST_CASE("precipitation extract: particle_scale scales the rain emitter and leaves a plain emitter alone") {
    Rig r;
    render::QualitySettings q = render::quality_settings(render::QualityTier::Medium);
    r.extract.set_quality(q);
    const Array<render::ParticleEmitter> full = r.frame();
    REQUIRE(full.size() == 2);
    q.particle_scale = 0.5f;
    r.extract.set_quality(q);
    const Array<render::ParticleEmitter> half = r.frame();
    REQUIRE(half.size() == 2);
    u32 scaled = 0, untouched = 0;
    for (u32 i = 0; i < 2; ++i) {
        if (half[i].rate == doctest::Approx(0.5f * full[i].rate) && half[i].max_particles == full[i].max_particles / 2u)
            ++scaled;
        else if (half[i].rate == full[i].rate && half[i].max_particles == full[i].max_particles)
            ++untouched;
    }
    CHECK(scaled == 1);
    CHECK(untouched == 1);
}

TEST_CASE("precipitation extract: scale 0 switches the rain emitter off, the ECS component is never written") {
    Rig r;
    render::QualitySettings q = render::quality_settings(render::QualityTier::Medium);
    q.particle_scale = 0.0f;
    r.extract.set_quality(q);
    const Array<render::ParticleEmitter> none = r.frame();
    u32 off = 0, on = 0;
    for (const render::ParticleEmitter& e : none) (e.emitting ? on : off)++;
    CHECK(off == 1);
    CHECK(on == 1);
    CHECK(r.world.try_get<ParticleEmitter3D>(r.rain_e)->rate == 400.0f);
    CHECK(r.world.try_get<ParticleEmitter3D>(r.rain_e)->max_particles == 800u);
}
