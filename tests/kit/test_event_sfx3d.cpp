// Event sounds (M-GAMEFEEL-SFX): each game event of the holder pushes one PlaySfx; a null clip and other owners stay
// silent.

#include <doctest/doctest.h>

#include "mce/audio/components.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/ammopickup3d.h"
#include "mce/kit/combat3d.h"
#include "mce/kit/event_sfx3d.h"
#include "mce/kit/grenade3d.h"
#include "mce/kit/loadout3d.h"
#include "mce/kit/melee3d.h"
#include "mce/kit/progress3d.h"
#include "mce/kit/wanted3d.h"
#include "mce/kit/weapon_events3d.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

struct Scene {
    World w;
    u64 t = 0;
    Entity player, other;
    u32 sfx = 0;
    asset::Guid last_clip{};
    f32 last_db = 0.0f;
    Scene() {
        w.register_components<LocalTransform>();
        install_combat3d(w, nullptr);
        install_event_sfx3d(w);
        w.register_event<audio::PlaySfx>(Phase::FixedUpdate_Pre);
        EventSfx3D s;
        s.throw_clip.guid = asset::Guid{1, 1};
        s.blast_clip.guid = asset::Guid{2, 2};
        s.melee_clip.guid = asset::Guid{3, 3};
        s.cash_clip.guid = asset::Guid{4, 4};
        s.ammo_clip.guid = asset::Guid{5, 5};
        s.reload_start_clip.guid = asset::Guid{6, 6};
        s.reloaded_clip.guid = asset::Guid{7, 7};
        s.dry_clip.guid = asset::Guid{8, 8};
        s.switch_clip.guid = asset::Guid{9, 9};
        // goal_clip stays null
        player = w.spawn_with(s);
        other = w.spawn_with(EventSfx3D{});
        run(2);
        sfx = 0;
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) {
            w.tick(kDt, t++);
            for (const audio::PlaySfx& e : EventAccess(w).read<audio::PlaySfx>()) {
                ++sfx;
                last_clip = e.clip;
                last_db = e.gain_db;
            }
        }
    }
};

}  // namespace

TEST_CASE("event_sfx3d: every event of the holder plays its own clip") {
    Scene s;
    GrenadeThrown3D th;
    th.entity = s.player;
    EventAccess(s.w).push(th);
    s.run(2);
    CHECK(s.sfx == 1);
    CHECK(s.last_clip == asset::Guid{1, 1});
    CHECK(s.last_db == doctest::Approx(-6.0f));

    GrenadeBlast3D bl;
    bl.owner = s.player;
    EventAccess(s.w).push(bl);
    s.run(2);
    CHECK(s.sfx == 2);
    CHECK(s.last_clip == asset::Guid{2, 2});

    MeleeSwung3D me;
    me.entity = s.player;
    EventAccess(s.w).push(me);
    s.run(2);
    CHECK(s.last_clip == asset::Guid{3, 3});

    CashPickedUp3D ca;
    ca.entity = s.player;
    EventAccess(s.w).push(ca);
    s.run(2);
    CHECK(s.last_clip == asset::Guid{4, 4});

    AmmoPickedUp3D am;
    am.entity = s.player;
    EventAccess(s.w).push(am);
    s.run(2);
    CHECK(s.last_clip == asset::Guid{5, 5});
    CHECK(s.sfx == 5);
}

TEST_CASE("event_sfx3d: reload, dry and switch events of the holder play their clips") {
    Scene s;
    WeaponReloadStarted3D rs;
    rs.owner = s.player;
    EventAccess(s.w).push(rs);
    s.run(2);
    CHECK(s.last_clip == asset::Guid{6, 6});

    WeaponReloaded3D rd;
    rd.owner = s.player;
    EventAccess(s.w).push(rd);
    s.run(2);
    CHECK(s.last_clip == asset::Guid{7, 7});

    WeaponDry3D dr;
    dr.owner = s.player;
    EventAccess(s.w).push(dr);
    s.run(2);
    CHECK(s.last_clip == asset::Guid{8, 8});

    WeaponSwitched3D sw;
    sw.entity = s.player;
    EventAccess(s.w).push(sw);
    s.run(2);
    CHECK(s.last_clip == asset::Guid{9, 9});
    CHECK(s.sfx == 4);

    WeaponDry3D other;
    other.owner = s.other;  // holder with no clips
    EventAccess(s.w).push(other);
    s.run(2);
    CHECK(s.sfx == 4);
}

TEST_CASE("event_sfx3d: a null clip, another owner and a dead entity stay silent") {
    Scene s;
    GoalDone3D g;
    g.owner = s.player;  // goal_clip is null
    EventAccess(s.w).push(g);
    s.run(2);
    CHECK(s.sfx == 0);

    GrenadeThrown3D th;
    th.entity = s.other;  // holds an EventSfx3D with no clips
    EventAccess(s.w).push(th);
    s.run(2);
    CHECK(s.sfx == 0);

    GrenadeThrown3D none;  // no entity at all
    EventAccess(s.w).push(none);
    s.run(2);
    CHECK(s.sfx == 0);

    s.w.destroy(s.player);
    GrenadeThrown3D gone;
    gone.entity = s.player;
    EventAccess(s.w).push(gone);
    s.run(2);
    CHECK(s.sfx == 0);
}

TEST_CASE("event_sfx3d: without audio events installed nothing is pushed and nothing breaks") {
    World w;
    w.register_components<LocalTransform>();
    install_event_sfx3d(w);
    EventSfx3D e;
    e.throw_clip.guid = asset::Guid{1, 1};
    const Entity p = w.spawn_with(e);
    GrenadeThrown3D th;
    th.entity = p;
    EventAccess(w).push(th);
    w.tick(kDt, 0);
    CHECK(w.alive(p));
}
