#include "mce/kit/melee3d.h"

#include <algorithm>
#include <cmath>

#include "kit3d_internal.h"
#include "mce/containers/array.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/input/actions.h"
#include "mce/kit/combat3d.h"
#include "mce/kit/loadout3d.h"
#include "mce/kit/seats3d.h"
#include "mce/string_id.h"

namespace mce::kit {

static_assert(sizeof(Melee3D) == 40);

namespace {

struct Melee3DInstalled {};

constexpr StringId kMelee{"Melee"};

void resolve(Frame& f, Entity e, const Melee3D& m, EventAccess& ev) {
    World& w = f.world();
    const LocalTransform* t = w.try_get<LocalTransform>(e);
    u32 hits = 0;
    if (t) {
        math::vec3 pos, scale;
        math::quat rot;
        world_pose(w, e, *t, pos, rot, scale);
        math::vec3 fwd = math::rotate(rot, math::vec3{0.0f, 0.0f, -1.0f});
        fwd.y = 0.0f;
        const f32 fl = math::length(fwd);
        if (fl > 1.0e-4f) fwd = fwd * (1.0f / fl);
        const f32 cos_half = std::cos(math::radians(math::clamp(m.arc_deg, 1.0f, 360.0f) * 0.5f));
        const Health3D* mine = w.try_get<Health3D>(e);
        Array<Entity> targets;
        f.query<Health3D, LocalTransform>().each([&](Entity o, Health3D& h, LocalTransform& lt) {
            if (o == e || h.dead || h.invulnerable) return;
            if (mine && mine->team != 0 && mine->team == h.team) return;
            math::vec3 op, os;
            math::quat orot;
            world_pose(w, o, lt, op, orot, os);
            const math::vec3 d = op - pos;
            if (math::length(d) > m.reach) return;
            math::vec3 flat{d.x, 0.0f, d.z};
            const f32 len = math::length(flat);
            if (len > 1.0e-4f && fl > 1.0e-4f && math::dot(flat * (1.0f / len), fwd) < cos_half) return;
            targets.push_back(o);
        });
        std::sort(targets.begin(), targets.end(), [](Entity a, Entity b) { return a.index < b.index; });
        for (Entity o : targets) {
            if (!w.alive(o)) continue;
            const LocalTransform* lt = w.try_get<LocalTransform>(o);
            math::vec3 op = lt ? lt->position : pos, os;
            math::quat orot;
            if (lt) world_pose(w, o, *lt, op, orot, os);
            DamageInfo di;
            di.amount = m.damage;
            di.instigator = e;
            di.point = op;
            const math::vec3 dir = op - pos;
            const f32 dl = math::length(dir);
            di.direction = dl > 1.0e-4f ? dir * (1.0f / dl) : math::vec3{0.0f, 0.0f, 0.0f};
            if (apply_damage(w, o, di)) ++hits;
        }
    }
    ev.push(MeleeSwung3D{e, hits, 0});
}

void kit_melee(Frame& f) {
    World& w = f.world();
    const input::InputState* in = w.try_resource<input::InputState>();
    EventAccess ev(w);
    f.query<Melee3D>().each([&](Entity e, Melee3D& m) {
        m.cooldown = math::max(m.cooldown - f.dt, 0.0f);
        const Health3D* h = w.try_get<Health3D>(e);
        const Rider3D* r = w.try_get<Rider3D>(e);
        const bool blocked = (h && h->dead) || (r && r->seated == 1);
        bool want = m.trigger != 0;
        m.trigger = 0;
        if (m.use_input && in && in->has_action(kMelee) && in->pressed(kMelee)) want = true;
        if (blocked) {
            m.swinging = 0;
            return;
        }
        if (want && !m.swinging && m.cooldown <= 0.0f) {
            m.swinging = 1;
            m.timer = m.windup;
            m.cooldown = 1.0f / math::max(m.rate, 0.01f);
        }
        if (!m.swinging) return;
        m.timer -= f.dt;
        if (m.timer > 0.0f) return;
        m.swinging = 0;
        ++m.swings;
        resolve(f, e, m, ev);
    });
}

}  // namespace

void install_melee3d(World& world) {
    if (world.try_resource<Melee3DInstalled>()) return;
    world.set_resource(Melee3DInstalled{});
    install_loadout3d(world);  // the system below runs after "kit_loadout"
    world.register_components<Melee3D>();
    world.register_event<MeleeSwung3D>(Phase::FixedUpdate_Pre);
    world.add_system(Phase::FixedUpdate_Pre, "kit_melee", &kit_melee, "kit_loadout");
}

}  // namespace mce::kit
