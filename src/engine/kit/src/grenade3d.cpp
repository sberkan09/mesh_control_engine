#include "mce/kit/grenade3d.h"

#include <algorithm>

#include "kit3d_internal.h"
#include "mce/containers/array.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/input/actions.h"
#include "mce/kit/combat3d.h"
#include "mce/kit/progress3d.h"
#include "mce/kit/seats3d.h"
#include "mce/string_id.h"

namespace mce::kit {

static_assert(sizeof(Grenade3D) == 72);
static_assert(sizeof(GrenadeLive3D) == 40);
static_assert(sizeof(GrenadeThrown3D) == 16);
static_assert(sizeof(GrenadeBlast3D) == 32);

namespace {

struct Grenade3DInstalled {};

constexpr StringId kThrow{"Throw"};
constexpr math::vec3 kDefaultGravity{0.0f, -9.81f, 0.0f};

void throw_one(Frame& f, Entity e, Grenade3D& g, EventAccess& ev) {
    World& w = f.world();
    const LocalTransform* t = w.try_get<LocalTransform>(e);
    if (!t) return;
    math::vec3 pos, scale;
    math::quat rot;
    world_pose(w, e, *t, pos, rot, scale);
    math::vec3 fwd = math::rotate(rot, math::vec3{0.0f, 0.0f, -1.0f});
    fwd.y = 0.0f;
    const f32 fl = math::length(fwd);
    fwd = fl > 1.0e-4f ? fwd * (1.0f / fl) : math::vec3{0.0f, 0.0f, -1.0f};
    GrenadeLive3D live;
    live.owner = e;
    live.velocity = fwd * g.throw_speed + math::vec3{0.0f, g.lift, 0.0f};
    live.timer = g.fuse;
    live.damage = g.damage;
    live.radius = g.radius;
    live.bounce = g.bounce;
    LocalTransform lt;
    lt.position = pos + math::vec3{0.0f, 1.2f, 0.0f} + fwd * 0.5f;  // chest height, a little ahead
    lt.rotation = math::quat::identity();
    lt.scale = {g.scale, g.scale, g.scale};
    if (g.mesh.guid.is_null()) {
        f.commands().spawn(lt, live);
    } else {
        MeshRef m;
        m.mesh = g.mesh;
        m.interpolate = 1;
        f.commands().spawn(lt, live, m);
    }
    --g.count;
    ++g.thrown;
    g.cooldown = 1.0f / math::max(g.rate, 0.01f);
    ev.push(GrenadeThrown3D{e, g.count, 0});
}

void explode(Frame& f, Entity grenade, const GrenadeLive3D& live, const math::vec3& at, EventAccess& ev) {
    World& w = f.world();
    Array<Entity> targets;
    f.query<Health3D, LocalTransform>().each([&](Entity o, Health3D& h, LocalTransform&) {
        if (h.dead) return;
        targets.push_back(o);
    });
    std::sort(targets.begin(), targets.end(), [](Entity a, Entity b) { return a.index < b.index; });
    u32 hits = 0;
    for (Entity o : targets) {
        if (!w.alive(o)) continue;
        const LocalTransform* lt = w.try_get<LocalTransform>(o);
        if (!lt) continue;
        math::vec3 op, os;
        math::quat orot;
        world_pose(w, o, *lt, op, orot, os);
        const math::vec3 d = op - at;
        const f32 dist = math::length(d);
        if (dist >= live.radius) continue;
        const f32 falloff = 1.0f - dist / live.radius;
        DamageInfo di;
        di.amount = live.damage * falloff;
        di.instigator = live.owner;
        di.point = at;
        di.direction = dist > 1.0e-4f ? d * (1.0f / dist) : math::vec3{0.0f, 1.0f, 0.0f};
        if (apply_damage(w, o, di)) ++hits;
    }
    ev.push(GrenadeBlast3D{grenade, live.owner, at, hits});
}

void kit_grenade(Frame& f) {
    World& w = f.world();
    const input::InputState* in = w.try_resource<input::InputState>();
    EventAccess ev(w);
    f.query<Grenade3D>().each([&](Entity e, Grenade3D& g) {
        g.cooldown = math::max(g.cooldown - f.dt, 0.0f);
        const Health3D* h = w.try_get<Health3D>(e);
        const Rider3D* r = w.try_get<Rider3D>(e);
        const bool blocked = (h && h->dead) || (r && r->seated == 1);
        bool want = g.trigger != 0;
        g.trigger = 0;
        if (g.use_input && in && in->has_action(kThrow) && in->pressed(kThrow)) want = true;
        if (blocked || !want || g.count == 0 || g.cooldown > 0.0f) return;
        throw_one(f, e, g, ev);
    });

    Physics3D* phys = w.try_resource<Physics3D>();
    const math::vec3 gravity = phys ? phys->world.config().gravity : kDefaultGravity;
    Array<Entity> live_ids;
    f.query<GrenadeLive3D, LocalTransform>().each([&](Entity ge, GrenadeLive3D&, LocalTransform&) { live_ids.push_back(ge); });
    std::sort(live_ids.begin(), live_ids.end(), [](Entity a, Entity b) { return a.index < b.index; });
    for (Entity ge : live_ids) {
        GrenadeLive3D* live = w.try_get_mut<GrenadeLive3D>(ge);
        LocalTransform* t = w.try_get_mut<LocalTransform>(ge);
        if (!live || !t) continue;
        live->timer -= f.dt;
        if (live->timer <= 0.0f) {
            const GrenadeLive3D copy = *live;
            explode(f, ge, copy, t->position, ev);
            f.commands().destroy(ge);
            continue;
        }
        live->velocity += gravity * f.dt;
        const math::vec3 step = live->velocity * f.dt;
        const f32 len = math::length(step);
        if (len <= 1.0e-6f) continue;
        const math::vec3 dir = step * (1.0f / len);
        if (phys) {
            physics::QueryFilter3D filter;
            filter.include_characters = true;
            Entity skip[1] = {live->owner};
            filter.exclude = Span<const Entity>(skip, 1);
            const std::optional<physics::RayHit3D> hit = phys->world.raycast(t->position, dir, len, filter);
            if (hit) {
                // Stop just short of the surface and reflect: the normal part keeps `bounce`, the rest keeps 0.8.
                t->position = hit->point + hit->normal * 0.05f;
                const f32 vn = math::dot(live->velocity, hit->normal);
                const math::vec3 normal_part = hit->normal * vn;
                live->velocity = (live->velocity - normal_part) * 0.8f - normal_part * live->bounce;
                continue;
            }
        }
        t->position += step;
    }
}

}  // namespace

void install_grenade3d(World& world) {
    if (world.try_resource<Grenade3DInstalled>()) return;
    world.set_resource(Grenade3DInstalled{});
    install_progress3d(world);  // the system below runs after "kit_progress"
    world.register_components<Grenade3D, GrenadeLive3D>();
    world.register_event<GrenadeThrown3D>(Phase::FixedUpdate_Pre);
    world.register_event<GrenadeBlast3D>(Phase::FixedUpdate_Pre);
    world.add_system(Phase::FixedUpdate_Pre, "kit_grenade", &kit_grenade, "kit_progress");
}

}  // namespace mce::kit
