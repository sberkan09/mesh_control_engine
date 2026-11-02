#include "mce/kit/loadout3d.h"

#include "mce/ecs/world.h"
#include "mce/input/actions.h"
#include "mce/kit/combat3d.h"
#include "mce/string_id.h"

namespace mce::kit {

static_assert(sizeof(Loadout3D) == 48);

namespace {

struct Loadout3DState {};

constexpr usize kSlots = 4;
constexpr StringId kNext{"NextWeapon"};
constexpr StringId kPrev{"PrevWeapon"};
constexpr StringId kPick[kSlots] = {StringId{"Weapon1"}, StringId{"Weapon2"}, StringId{"Weapon3"}, StringId{"Weapon4"}};

Entity slot_of(const Loadout3D& l, usize i) {
    switch (i) {
        case 0: return l.slot0;
        case 1: return l.slot1;
        case 2: return l.slot2;
        default: return l.slot3;
    }
}

Weapon3D* weapon_at(World& w, const Loadout3D& l, usize i) {
    const Entity e = slot_of(l, i);
    return !e.is_null() && w.alive(e) ? w.try_get_mut<Weapon3D>(e) : nullptr;
}

// FixedUpdate_Pre, after the weapons: switching, and who gets the Fire and Reload input.
void kit3d_loadout(Frame& f) {
    World& w = f.world();
    const input::InputState* in = w.try_resource<input::InputState>();
    f.query<Loadout3D>().each([&](Entity, Loadout3D& l) {
        const auto filled = [&](usize i) { return weapon_at(w, l, i) != nullptr; };
        usize want = l.active < kSlots ? l.active : 0;
        if (l.select >= 1 && l.select <= kSlots && filled(l.select - 1u)) want = l.select - 1u;
        l.select = 0;
        if (l.use_input && in) {
            for (usize i = 0; i < kSlots; ++i)
                if (in->has_action(kPick[i]) && in->pressed(kPick[i]) && filled(i)) want = i;
            const int step = in->has_action(kNext) && in->pressed(kNext) ? 1 : in->has_action(kPrev) && in->pressed(kPrev) ? -1 : 0;
            for (usize k = 1; step != 0 && k < kSlots; ++k) {
                const usize i = (want + (step > 0 ? k : kSlots - k)) % kSlots;
                if (filled(i)) {
                    want = i;
                    break;
                }
            }
        }
        if (!filled(want))  // an empty or lost slot: the first weapon there is
            for (usize i = 0; i < kSlots; ++i)
                if (filled(i)) {
                    want = i;
                    break;
                }
        l.active = static_cast<u8>(want);
        const bool drew = l.applied != 0 && l.applied != l.active + 1u;
        l.applied = static_cast<u8>(l.active + 1u);
        for (usize i = 0; i < kSlots; ++i) {
            Weapon3D* wp = weapon_at(w, l, i);
            if (!wp) continue;
            const bool held = i == l.active;
            wp->use_input = held && l.use_input ? 1 : 0;
            wp->stowed = held ? 0 : 1;
            if (held && drew && wp->cooldown < l.switch_time) wp->cooldown = l.switch_time;
        }
        const Weapon3D* hand = weapon_at(w, l, l.active);
        l.ammo = hand ? hand->ammo : 0;
        l.magazine = hand ? hand->magazine : 0;
        l.reserve = hand ? hand->reserve : 0;
    });
}

}  // namespace

Weapon3D* held_weapon(World& world, Entity holder) {
    if (const Loadout3D* l = world.try_get<Loadout3D>(holder)) return weapon_at(world, *l, l->active < kSlots ? l->active : 0);
    return world.try_get_mut<Weapon3D>(holder);
}

usize weapons_of(World& world, Entity holder, Weapon3D** out, usize cap) {
    usize n = 0;
    if (const Loadout3D* l = world.try_get<Loadout3D>(holder)) {
        for (usize i = 0; i < kSlots && n < cap; ++i)
            if (Weapon3D* wp = weapon_at(world, *l, i)) out[n++] = wp;
        return n;
    }
    if (Weapon3D* wp = world.try_get_mut<Weapon3D>(holder); wp && cap > 0) out[n++] = wp;
    return n;
}

void install_loadout3d(World& world) {
    install_combat3d(world, nullptr);
    if (world.try_resource<Loadout3DState>()) return;
    world.set_resource(Loadout3DState{});
    world.register_components<Loadout3D>();
    world.add_system(Phase::FixedUpdate_Pre, "kit3d_loadout", &kit3d_loadout, "kit3d_weapons");
}

}  // namespace mce::kit
