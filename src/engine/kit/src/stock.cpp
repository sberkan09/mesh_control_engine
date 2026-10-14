#include "mce/kit/stock.h"

#include "mce/ecs/world.h"
#include "mce/kit/bodies3d.h"
#include "mce/kit/components.h"
#include "mce/kit/effects.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/particles.h"
#include "mce/kit/sequence.h"
#include "mce/kit/sprite_anim.h"
#include "mce/physics/world2d.h"

namespace mce::kit {

static_assert(sizeof(WorldSettings) == 24);

namespace {

struct WorldSettingsInstalled {};

void kit_world_settings(Frame& f) {
    bool found = false;
    f.query<const WorldSettings&>().each([&](const WorldSettings& s) {
        if (found) return;
        found = true;
        if (physics::World2D* w2 = f.try_resource<physics::World2D>()) w2->set_gravity(s.gravity_2d);
        if (Physics3D* p3 = f.try_resource<Physics3D>()) p3->world.set_gravity(s.gravity_3d);
    });
}

}  // namespace

void install_world_settings(World& world) {
    if (world.try_resource<WorldSettingsInstalled>()) return;
    world.set_resource(WorldSettingsInstalled{});
    world.register_components<WorldSettings>();
    world.add_system(Phase::Startup, "kit_world_settings", &kit_world_settings);
}

void install_stock(World& world, asset::AssetSystem* assets) {
    install_particles(world, assets);
    install_sequences(world, assets);
    install_kit3d(world, assets);
    install_bodies3d(world, assets);
    install_world_settings(world);
    install_effects(world, assets);
    install_sprite_animation(world, assets);
}

}  // namespace mce::kit
