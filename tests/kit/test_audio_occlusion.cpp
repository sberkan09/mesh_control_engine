// M-AUDIO2 5: kit::install_audio_occlusion gives AudioOcclusion a physics ray: a wall between the
// listener and a source occludes it, in 3D (Physics3D) and in the 2D plane (World2D).

#include <doctest/doctest.h>

#include "mce/audio/components.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/stock.h"
#include "mce/physics/module.h"
#include "mce/physics/world2d.h"

using namespace mce;

namespace {

constexpr f64 kDt = 1.0 / 60.0;

LocalTransform at(f32 x, f32 y, f32 z) { return LocalTransform{{x, y, z}, math::quat::identity(), {1, 1, 1}}; }

struct Stage {
    mem::UniquePtr<audio::AudioDevice> dev;
    World world;
    u64 ticks = 0;
    Array<f32> tone;
    Entity source;

    explicit Stage(bool physics3d) {
        audio::AudioDesc d;
        d.backend = audio::Backend::Offline;
        dev = std::move(audio::AudioDevice::create(d)).value();
        world.register_components<LocalTransform>();
        if (physics3d) {
            kit::install_kit3d(world, nullptr);
        } else {
            physics::PhysicsConfig cfg;
            cfg.gravity = {0.0f, 0.0f};
            physics::install_physics2d(world, cfg);
        }
        audio::install(world, *dev);
        kit::install_audio_occlusion(world);
        tone.resize(4800, 0.1f);
        audio::ClipPayload p;
        p.frames = tone.data();
        p.frame_count = 4800;
        audio::AudioSource src;
        src.clip_handle = dev->register_clip(p).value();
        src.loop = true;
        src.play_on_start = true;
        world.spawn_with(audio::AudioListener{}, at(0, 0, 0));
        source = world.spawn_with(src, audio::AudioSpatial{}, audio::AudioOcclusion{}, physics3d ? at(0, 0, -8) : at(8, 0, 0));
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) world.tick(kDt, ticks++);
    }
    f32 amount() { return world.try_get<audio::AudioOcclusion>(source)->amount; }
};

}  // namespace

TEST_CASE("audio occlusion: a wall between the listener and a source occludes it, in 3D and in 2D") {
    {
        Stage s(true);
        REQUIRE(s.world.resource<audio::AudioRef>().occlusion != nullptr);
        kit::BoxCollider3D wall;  // static boxes are built at Startup: before the first tick
        wall.half = {2.0f, 2.0f, 0.2f};
        s.world.spawn_with(at(0, 0, -4), wall);
        s.run(8);
        CHECK(s.amount() == 1.f);
        s.world.try_get_mut<LocalTransform>(s.source)->position = {6, 0, -6};  // out from behind it
        s.run(8);
        CHECK(s.amount() == 0.f);
    }
    {
        Stage s(false);
        physics::RigidBody2D rb;
        rb.type = physics::BodyType::Static;
        physics::Collider2D col;
        col.shape = physics::ColliderShape::box({0.2f, 2.0f});
        const Entity wall = s.world.spawn_with(at(4, 0, 0), rb, col);
        s.run(8);
        CHECK(s.amount() == 1.f);
        // A mask without the wall's layer lets the sound through.
        s.world.try_get_mut<audio::AudioOcclusion>(s.source)->mask = 0;
        s.run(8);
        CHECK(s.amount() == 0.f);
        (void)wall;
    }
}
