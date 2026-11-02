#include "mce/kit/loadout3d.h"

#include "mce/ecs/world.h"
#include "mce/input/actions.h"
#include "mce/kit/combat3d.h"
#include "mce/kit/seats3d.h"
#include "mce/kit/world_audio.h"
#include "mce/string_id.h"

namespace mce::kit {

static_assert(sizeof(WeaponPreset3D) == 64);
static_assert(sizeof(Loadout3D) == 40);

namespace {

struct Loadout3DInstalled {};

constexpr StringId kSlot[3] = {StringId("Slot1"), StringId("Slot2"), StringId("Slot3")};
constexpr StringId kNext{"WeaponNext"};
constexpr StringId kPrev{"WeaponPrev"};

Entity slot_entity(const Loadout3D& l, u32 i) { return i == 0 ? l.slot0 : (i == 1 ? l.slot1 : l.slot2); }

WeaponPreset3D* preset_of(World& w, const Loadout3D& l, u32 i) {
    const Entity e = slot_entity(l, i);
    return e.is_null() || !w.alive(e) ? nullptr : w.try_get_mut<WeaponPreset3D>(e);
}

// Copies the preset onto the wielder's Weapon3D (an equipment bonus stays on top of the damage).
void apply(World& w, Entity wielder, Weapon3D& wp, WeaponPreset3D& p) {
    wp.damage = p.damage + wp.bonus_applied;
    wp.range = p.range;
    wp.rate = p.rate;
    wp.spread_deg = p.spread_deg;
    wp.impulse = p.impulse;
    wp.reload_time = p.reload_time;
    wp.pellets = p.pellets;
    wp.magazine = p.magazine;
    wp.automatic = p.automatic;
    if (!p.primed) {
        p.ammo = p.magazine;
        p.primed = 1;
    }
    wp.ammo = static_cast<u16>(p.ammo);
    wp.reserve = p.reserve;
    wp.reloading = 0;
    wp.reload_left = 0.0f;
    if (p.shot.guid != asset::Guid{})
        if (WeaponAudio* wa = w.try_get_mut<WeaponAudio>(wielder)) wa->shot = p.shot;
}

// The next filled slot after `from` in direction `dir` (+1 or -1); 3 when there is none other than `from`.
u32 cycle(World& w, const Loadout3D& l, u32 from, i32 dir) {
    for (i32 step = 1; step <= 2; ++step) {
        const u32 i = static_cast<u32>((static_cast<i32>(from) + dir * step + 6) % 3);
        if (preset_of(w, l, i)) return i;
    }
    return 3;
}

void kit_loadout(Frame& f) {
    World& w = f.world();
    const input::InputState* in = w.try_resource<input::InputState>();
    EventAccess ev(w);
    f.query<Loadout3D, Weapon3D>().each([&](Entity e, Loadout3D& l, Weapon3D& wp) {
        if (l.current > 2) l.current = 0;
        if (const Health3D* h = w.try_get<Health3D>(e); h && h->dead) return;
        if (const Rider3D* r = w.try_get<Rider3D>(e); r && r->seated == 1) return;
        if (!l.started) {
            if (WeaponPreset3D* p = preset_of(w, l, l.current)) {
                apply(w, e, wp, *p);
                l.started = 1;
            }
            return;
        }
        u32 want = 3;
        if (l.select >= 1 && l.select <= 3) want = static_cast<u32>(l.select - 1);
        l.select = 0;
        if (want == 3 && l.use_input && in) {
            for (u32 i = 0; i < 3; ++i)
                if (in->has_action(kSlot[i]) && in->pressed(kSlot[i])) want = i;
            if (want == 3 && in->has_action(kNext) && in->pressed(kNext)) want = cycle(w, l, l.current, +1);
            if (want == 3 && in->has_action(kPrev) && in->pressed(kPrev)) want = cycle(w, l, l.current, -1);
        }
        if (want >= 3 || want == l.current) return;
        WeaponPreset3D* to = preset_of(w, l, want);
        if (!to) return;
        if (WeaponPreset3D* from = preset_of(w, l, l.current)) {
            from->ammo = wp.ammo;
            from->reserve = wp.reserve;
        }
        apply(w, e, wp, *to);
        if (wp.cooldown < l.draw_time) wp.cooldown = l.draw_time;
        ev.push(WeaponSwitched3D{e, l.current, want});
        l.current = static_cast<u8>(want);
        ++l.switches;
    });
}

}  // namespace

void install_loadout3d(World& world) {
    if (world.try_resource<Loadout3DInstalled>()) return;
    world.set_resource(Loadout3DInstalled{});
    world.register_components<WeaponPreset3D, Loadout3D>();
    world.register_event<WeaponSwitched3D>(Phase::FixedUpdate_Pre);
    world.add_system(Phase::FixedUpdate_Pre, "kit_loadout", &kit_loadout, "kit3d_health");
}

}  // namespace mce::kit
