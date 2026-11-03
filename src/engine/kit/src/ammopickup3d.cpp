#include "mce/kit/ammopickup3d.h"

#include "kit3d_internal.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/combat3d.h"
#include "mce/kit/grenade3d.h"
#include "mce/kit/loadout3d.h"
#include "mce/kit/wanted3d.h"

namespace mce::kit {

static_assert(sizeof(AmmoPickup3D) == 24);
static_assert(sizeof(AmmoPickedUp3D) == 24);

namespace {

struct AmmoPickup3DInstalled {};

math::vec3 world_position(World& w, Entity e) {
    const LocalTransform* t = w.try_get<LocalTransform>(e);
    if (!t) return {};
    math::vec3 pos, scale;
    math::quat rot;
    world_pose(w, e, *t, pos, rot, scale);
    return pos;
}

// Adds up to `want` rounds to `reserve` without passing `cap`; returns what was added.
u32 top_up(u32& reserve, u32 want, u32 cap) {
    if (reserve >= cap) return 0;
    const u32 add = math::min(want, cap - reserve);
    reserve += add;
    return add;
}

void kit3d_ammopickup(Frame& f) {
    World& w = f.world();
    EventAccess ev(w);
    f.query<AmmoPickup3D, LocalTransform>().each([&](Entity pe, AmmoPickup3D& ap, LocalTransform&) {
        if (ap.left > 0.0f) {
            ap.left = ap.left > f.dt ? ap.left - f.dt : 0.0f;
            return;
        }
        const math::vec3 pp = world_position(w, pe);
        Entity taker;
        f.query<Weapon3D, LocalTransform>().each([&](Entity be, Weapon3D& wp, LocalTransform&) {
            if (!taker.is_null() || wp.magazine == 0 || wp.infinite_ammo) return;
            if (math::length(world_position(w, be) - pp) <= ap.radius) taker = be;
        });
        if (taker.is_null()) return;
        u32 rounds = 0, grenades = 0;
        if (Weapon3D* wp = w.try_get_mut<Weapon3D>(taker)) rounds += top_up(wp->reserve, ap.rounds, ap.max_reserve);
        if (const Loadout3D* lo = w.try_get<Loadout3D>(taker)) {
            const Entity slots[3] = {lo->slot0, lo->slot1, lo->slot2};
            for (u32 i = 0; i < 3; ++i) {
                if (i == lo->current || slots[i].is_null() || !w.alive(slots[i])) continue;
                if (WeaponPreset3D* pr = w.try_get_mut<WeaponPreset3D>(slots[i]); pr && pr->magazine != 0)
                    rounds += top_up(pr->reserve, ap.rounds, ap.max_reserve);
            }
        }
        if (Grenade3D* gr = w.try_get_mut<Grenade3D>(taker); gr && gr->count < gr->max_count) {
            const u32 add = math::min<u32>(ap.grenades, static_cast<u32>(gr->max_count - gr->count));
            gr->count = static_cast<u16>(gr->count + add);
            grenades += add;
        }
        if (rounds == 0 && grenades == 0) return;
        ev.push(AmmoPickedUp3D{taker, pe, rounds, grenades});
        if (ap.respawn > 0.0f)
            ap.left = ap.respawn;
        else
            f.commands().destroy(pe);
    });
}

}  // namespace

void install_ammopickup3d(World& world) {
    if (world.try_resource<AmmoPickup3DInstalled>()) return;
    world.set_resource(AmmoPickup3DInstalled{});
    install_wanted3d(world);   // the system below runs after "kit3d_cashpickup"
    install_loadout3d(world);
    install_grenade3d(world);
    world.register_components<AmmoPickup3D>();
    world.register_event<AmmoPickedUp3D>(Phase::FixedUpdate_Pre);
    world.add_system(Phase::FixedUpdate_Pre, "kit3d_ammopickup", &kit3d_ammopickup, "kit3d_cashpickup");
}

}  // namespace mce::kit
