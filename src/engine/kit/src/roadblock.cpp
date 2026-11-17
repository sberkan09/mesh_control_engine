#include "mce/kit/roadblock.h"

#include <cmath>
#include <utility>

#include "mce/asset/asset_system.h"
#include "mce/ecs/prefab.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/combat3d.h"
#include "mce/kit/vehicles3d.h"
#include "mce/kit/wanted3d.h"

namespace mce::kit {

static_assert(sizeof(Roadblock3D) == 88);
static_assert(sizeof(RoadblockPiece3D) == sizeof(Entity));

namespace {

struct RoadblockAssets {
    asset::AssetSystem* assets = nullptr;
};

// Where each wanted entity has been heading, smoothed over the fixed ticks (Roadblock3D.ahead_cos).
struct Heading {
    Entity entity;
    math::vec3 last;
    math::vec3 dir;  // flat unit vector, zero until it has moved
};
struct Headings {
    Array<Heading> list;
};

constexpr f32 kMinHeadingSpeed = 2.0f;  // m/s: slower than this keeps the old heading

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

bool occupied(Frame& f, math::vec3 p, f32 radius) {
    bool taken = false;
    const auto near = [&](const LocalTransform& t) { return flat_distance(t.position, p) < radius && std::fabs(t.position.y - p.y) < 3.0f; };
    f.query<const Health3D, const LocalTransform>().each([&](Entity, const Health3D&, const LocalTransform& t) { taken = taken || near(t); });
    f.query<const Vehicle3D, const LocalTransform>().each([&](Entity, const Vehicle3D&, const LocalTransform& t) { taken = taken || near(t); });
    return taken;
}

struct Request {
    Entity marker;
    u32 kind = 0;
    math::vec3 pos;
    math::quat rot;
    asset::AssetRef<asset::Prefab> prefab;
};

constexpr f32 kRetrySeconds = 5.0f;
constexpr f32 kClearRadius = 3.0f;

// FixedUpdate_Pre, after "kit3d_wanted".
void kit3d_roadblocks(Frame& f) {
    World& w = f.world();
    const RoadblockAssets* ra = w.try_resource<RoadblockAssets>();
    const RoadblockSpawnHook* hook = w.try_resource<RoadblockSpawnHook>();
    // Which markers have pieces standing: the pieces say so.
    Array<Entity> standing;
    f.query<const RoadblockPiece3D>().each([&](Entity, const RoadblockPiece3D& p) { standing.push_back(p.owner); });
    const auto has_pieces = [&](Entity m) {
        for (const Entity e : standing)
            if (e == m) return true;
        return false;
    };
    Headings& headings = w.resource<Headings>();
    {
        Array<Heading> next;
        f.query<const Wanted3D, const LocalTransform>().each([&](Entity e, const Wanted3D&, const LocalTransform& ot) {
            Heading h{e, ot.position, math::vec3{0.0f, 0.0f, 0.0f}};
            for (const Heading& old : headings.list)
                if (old.entity == e) {
                    h = old;
                    const f32 dx = ot.position.x - old.last.x, dz = ot.position.z - old.last.z;
                    const f32 len = std::sqrt(dx * dx + dz * dz);
                    if (f.dt > 0.0f && len / f.dt >= kMinHeadingSpeed) {
                        const math::vec3 step{dx / len, 0.0f, dz / len};
                        const f32 k = 0.05f;
                        math::vec3 d = old.dir * (1.0f - k) + step * k;
                        if (old.dir.x == 0.0f && old.dir.z == 0.0f) d = step;
                        const f32 dl = std::sqrt(d.x * d.x + d.z * d.z);
                        h.dir = dl > 1e-4f ? math::vec3{d.x / dl, 0.0f, d.z / dl} : step;
                    }
                    h.last = ot.position;
                    break;
                }
            next.push_back(h);
        });
        headings.list = std::move(next);
    }
    Array<Entity> down;  // markers taken down this tick
    Array<Request> requests;  // pieces to make once the walk over the markers is done
    f.query<Roadblock3D, const LocalTransform>().each([&](Entity me, Roadblock3D& b, const LocalTransform& t) {
        // The nearest offender at or above min_level, and the nearest of any wanted one.
        f32 near_high = 1.0e30f;
        f32 near_any = 1.0e30f;
        f32 near_ahead = 1.0e30f;  // the nearest offender at min_level that is heading at the marker (ahead_cos)
        f.query<const Wanted3D, const LocalTransform>().each([&](Entity oe, const Wanted3D& wd,
                                                                 const LocalTransform& ot) {
            const f32 d = flat_distance(ot.position, t.position);
            near_any = math::min(near_any, d);
            if (wd.level < b.min_level) return;
            near_high = math::min(near_high, d);
            if (b.ahead_cos > 0.0f && d > 1e-3f) {
                for (const Heading& h : headings.list)
                    if (h.entity == oe) {
                        const f32 along =
                            ((t.position.x - ot.position.x) * h.dir.x + (t.position.z - ot.position.z) * h.dir.z) / d;
                        if (along >= b.ahead_cos) near_ahead = math::min(near_ahead, d);
                    }
            }
        });
        const bool wanted = near_high < 1.0e29f;
        b.up = has_pieces(me) ? 1 : 0;
        if (!b.up) {
            b.low_time = 0.0f;
            if (b.retry > 0.0f) {
                b.retry = math::max(b.retry - f.dt, 0.0f);
                return;
            }
            if (!wanted || near_high < b.spawn_min || near_high > b.spawn_max) return;
            if (b.ahead_cos > 0.0f && (near_ahead < b.spawn_min || near_ahead > b.spawn_max)) return;
            const math::vec3 right = math::rotate(t.rotation, math::vec3{1.0f, 0.0f, 0.0f});
            const math::vec3 back = math::rotate(t.rotation, math::vec3{0.0f, 0.0f, 1.0f});
            const usize first = requests.size();
            for (u32 i = 0; i < b.cars; ++i) {
                const f32 across = (static_cast<f32>(i) - 0.5f * static_cast<f32>(b.cars - 1)) * b.car_spacing;
                requests.push_back(Request{me, 0, t.position + right * across, t.rotation, b.car});
            }
            for (u32 i = 0; i < b.officers; ++i) {
                const f32 across = (static_cast<f32>(i) - 0.5f * static_cast<f32>(b.officers - 1)) * b.car_spacing;
                requests.push_back(Request{me, 1, t.position + back * b.officer_back + right * across, t.rotation, b.officer});
            }
            for (usize i = first; i < requests.size(); ++i)
                if (occupied(f, requests[i].pos, kClearRadius)) {
                    requests.resize(first);  // something stands there: try again next tick
                    break;
                }
            return;
        }
        if (wanted) {
            b.low_time = 0.0f;
            return;
        }
        b.low_time += f.dt;
        if (b.low_time >= b.hold && near_any >= b.spawn_min) {
            b.low_time = 0.0f;
            down.push_back(me);
        }
    });
    // Make the pieces (outside the walk over the markers: instantiating records commands, but nothing here depends on that).
    for (usize i = 0; i < requests.size();) {
        const Entity marker = requests[i].marker;
        u32 made = 0;
        usize j = i;
        for (; j < requests.size() && requests[j].marker == marker; ++j) {
            const Request& r = requests[j];
            const Entity e = spawn_piece(f, ra, hook, r.prefab, r.kind, r.pos, r.rot);
            if (e.is_null()) continue;
            f.commands().add<RoadblockPiece3D>(e, RoadblockPiece3D{marker});
            ++made;
        }
        if (Roadblock3D* b = w.try_get_mut<Roadblock3D>(marker)) {
            b->up = made > 0 ? 1 : 0;
            if (made == 0) b->retry = kRetrySeconds;
        }
        i = j;
    }
    // Pieces of a marker that went down (or of one that no longer exists) are removed.
    f.query<const RoadblockPiece3D>().each([&](Entity e, const RoadblockPiece3D& p) {
        bool gone = !w.alive(p.owner) || !w.has<Roadblock3D>(p.owner);
        for (const Entity d : down) gone = gone || d == p.owner;
        if (gone) f.commands().destroy(e);
    });
    for (const Entity d : down)
        if (Roadblock3D* b = w.try_get_mut<Roadblock3D>(d)) b->up = 0;
}

}  // namespace

void install_roadblocks(World& world, asset::AssetSystem* assets) {
    if (RoadblockAssets* a = world.try_resource<RoadblockAssets>()) {
        a->assets = assets;
        return;
    }
    install_wanted3d(world);
    world.set_resource(RoadblockAssets{assets});
    world.set_resource(Headings{});
    world.register_components<Roadblock3D, RoadblockPiece3D>();
    world.add_system(Phase::FixedUpdate_Pre, "kit3d_roadblocks", &kit3d_roadblocks, "kit3d_wanted");
}

}  // namespace mce::kit
