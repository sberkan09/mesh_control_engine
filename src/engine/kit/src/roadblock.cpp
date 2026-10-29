#include "mce/kit/roadblock.h"

#include "mce/asset/asset_system.h"
#include "mce/ecs/prefab.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/wanted3d.h"

namespace mce::kit {

static_assert(sizeof(Roadblock3D) == 80);
static_assert(sizeof(RoadblockPiece3D) == sizeof(Entity));

namespace {

struct RoadblockAssets {
    asset::AssetSystem* assets = nullptr;
};

f32 flat_distance(math::vec3 a, math::vec3 b) {
    const f32 dx = a.x - b.x, dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}

// One piece at `pos`, from its prefab or the game's hook. Null when neither can make it.
Entity spawn_piece(Frame& f, const RoadblockAssets* ra, const RoadblockSpawnHook* hook, const asset::AssetRef<asset::Prefab>& prefab,
                   u32 kind, math::vec3 pos, math::quat rot) {
    World& w = f.world();
    if (prefab.guid != asset::Guid{} && ra && ra->assets) {
        const asset::AssetKey key = ra->assets->find(prefab.guid);
        if (key == asset::kInvalidAssetKey) return Entity{};
        Result<asset::AssetHandle<asset::Prefab>> ph = ra->assets->load_sync<asset::Prefab>(key);
        if (!ph) return Entity{};
        const ecs::PrefabOverride o[2] = {ecs::PrefabOverride::of("LocalTransform.position", pos),
                                          ecs::PrefabOverride::of("LocalTransform.rotation", rot)};
        ecs::SpawnParams p;
        p.overrides = Span<const ecs::PrefabOverride>(o, 2);
        Array<Entity> out;
        const Result<u32> res = ecs::instantiate(w, ra->assets->resolve(ph.value()), p, &out);
        ra->assets->release(asset::AnyHandle(ph.value()));
        return res && !out.empty() ? out[0] : Entity{};
    }
    if (hook && hook->spawn) return hook->spawn(f, kind, pos, rot);
    return Entity{};
}

// FixedUpdate_Pre, after "kit3d_wanted".
void kit3d_roadblocks(Frame& f) {
    World& w = f.world();
    const RoadblockAssets* ra = w.try_resource<RoadblockAssets>();
    const RoadblockSpawnHook* hook = w.try_resource<RoadblockSpawnHook>();
    Array<Entity> down;  // markers taken down this tick
    f.query<Roadblock3D, const LocalTransform>().each([&](Entity me, Roadblock3D& b, const LocalTransform& t) {
        // The nearest offender at or above min_level, and the nearest of any wanted one.
        f32 near_high = 1.0e30f;
        f32 near_any = 1.0e30f;
        f.query<const Wanted3D, const LocalTransform>().each([&](Entity, const Wanted3D& wd, const LocalTransform& ot) {
            const f32 d = flat_distance(ot.position, t.position);
            near_any = math::min(near_any, d);
            if (wd.level >= b.min_level) near_high = math::min(near_high, d);
        });
        const bool wanted = near_high < 1.0e29f;
        if (!b.up) {
            b.low_time = 0.0f;
            if (!wanted || near_high < b.spawn_min || near_high > b.spawn_max) return;
            const math::vec3 right = math::rotate(t.rotation, math::vec3{1.0f, 0.0f, 0.0f});
            const math::vec3 back = math::rotate(t.rotation, math::vec3{0.0f, 0.0f, 1.0f});
            u32 made = 0;
            for (u32 i = 0; i < b.cars; ++i) {
                const f32 across = (static_cast<f32>(i) - 0.5f * static_cast<f32>(b.cars - 1)) * b.car_spacing;
                const Entity e = spawn_piece(f, ra, hook, b.car, 0, t.position + right * across, t.rotation);
                if (e.is_null()) continue;
                f.commands().add<RoadblockPiece3D>(e, RoadblockPiece3D{me});
                ++made;
            }
            for (u32 i = 0; i < b.officers; ++i) {
                const f32 across = (static_cast<f32>(i) - 0.5f * static_cast<f32>(b.officers - 1)) * b.car_spacing;
                const Entity e = spawn_piece(f, ra, hook, b.officer, 1, t.position + back * b.officer_back + right * across, t.rotation);
                if (e.is_null()) continue;
                f.commands().add<RoadblockPiece3D>(e, RoadblockPiece3D{me});
                ++made;
            }
            b.up = made > 0 ? 1 : 0;
            return;
        }
        if (wanted) {
            b.low_time = 0.0f;
            return;
        }
        b.low_time += f.dt;
        if (b.low_time >= b.hold && near_any >= b.spawn_min) {
            b.up = 0;
            b.low_time = 0.0f;
            down.push_back(me);
        }
    });
    // Pieces of a marker that went down (or of one that no longer exists) are removed.
    f.query<const RoadblockPiece3D>().each([&](Entity e, const RoadblockPiece3D& p) {
        bool gone = !w.alive(p.owner) || !w.has<Roadblock3D>(p.owner);
        for (const Entity d : down) gone = gone || d == p.owner;
        if (gone) f.commands().destroy(e);
    });
}

}  // namespace

void install_roadblocks(World& world, asset::AssetSystem* assets) {
    if (RoadblockAssets* a = world.try_resource<RoadblockAssets>()) {
        a->assets = assets;
        return;
    }
    install_wanted3d(world);
    world.set_resource(RoadblockAssets{assets});
    world.register_components<Roadblock3D, RoadblockPiece3D>();
    world.add_system(Phase::FixedUpdate_Pre, "kit3d_roadblocks", &kit3d_roadblocks, "kit3d_wanted");
}

}  // namespace mce::kit
