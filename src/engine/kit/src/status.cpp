#include "mce/kit/status.h"

#include <cmath>

#include "kit3d_internal.h"
#include "mce/ecs/world.h"
#include "mce/hash.h"
#include "mce/kit/combat3d.h"
#include "mce/kit/craft.h"

namespace mce::kit {
namespace {

struct StatusInstalled {};

u32 us_of(f32 seconds) {
    const f32 us = seconds * 1.0e6f + 0.5f;
    return us <= 1.0f ? 1u : (us >= 4.0e9f ? 4000000000u : static_cast<u32>(us));
}

u32 step_us(f32 dt) {
    const f32 us = dt * 1.0e6f + 0.5f;
    return us <= 0.0f ? 0u : (us >= 4.0e9f ? 4000000000u : static_cast<u32>(us));
}

// Moves a bonus kind's added amount toward `want` (0 when the effect ends): only the change is applied, and what really
// changed after the clamps is what is remembered.
void settle_bonus(Health3D& hp, StatusKind kind, f32& applied, f32 want) {
    if (kind == StatusKind::Armor) {
        const f32 now = hp.armor + (want - applied);
        const f32 next = now < 0.0f ? 0.0f : (now > 1.0f ? 1.0f : now);
        applied += next - hp.armor;
        hp.armor = next;
    } else if (kind == StatusKind::MaxHealth) {
        const f32 now = hp.max + (want - applied);
        const f32 next = now < 1.0f ? 1.0f : now;
        applied += next - hp.max;
        hp.max = next;
        if (hp.current > hp.max) hp.current = hp.max;
    }
}

void clear_slot(StatusEffects& s, u32 i) {
    s.key[i] = 0;
    s.id[i].clear();
    s.kind[i] = 0;
    s.amount[i] = 0.0f;
    s.left_us[i] = 0;
    s.pulse_us[i] = 0;
    s.applied[i] = 0.0f;
    s.source[i] = Entity();
}

void kit_status(Frame& f) {
    World& w = f.world();
    EventAccess ev = f.events();

    for (const ClearStatus& c : ev.read<ClearStatus>()) {
        StatusEffects* s = c.target.is_null() || !w.alive(c.target) ? nullptr : w.try_get_mut<StatusEffects>(c.target);
        if (!s) continue;
        Health3D* hp = w.try_get_mut<Health3D>(c.target);
        const u32 key = c.id.empty() ? 0u : status_key(c.id.view());
        for (u32 i = 0; i < kStatusSlots; ++i) {
            if (s->key[i] == 0 || (key != 0 && s->key[i] != key)) continue;
            if (hp) settle_bonus(*hp, static_cast<StatusKind>(s->kind[i]), s->applied[i], 0.0f);
            ev.push(StatusEnded{c.target, s->id[i], 0});
            clear_slot(*s, i);
        }
    }

    for (const ApplyStatus& a : ev.read<ApplyStatus>()) {
        StatusEffects* s = a.target.is_null() || !w.alive(a.target) ? nullptr : w.try_get_mut<StatusEffects>(a.target);
        const bool sane = a.kind <= static_cast<u32>(StatusKind::Cooldown) && a.seconds >= 0.01f && a.seconds <= kStatusMaxSeconds &&
                          std::isfinite(a.amount) && a.amount > -1.0e6f && a.amount < 1.0e6f && !a.id.empty() &&
                          (a.kind >= static_cast<u32>(StatusKind::Armor) || a.amount > 0.0f);
        if (!s || !sane) {
            ev.push(StatusRefused{a.target, a.id});
            continue;
        }
        const u32 key = status_key(a.id.view());
        i32 slot = status_find(*s, key);
        const bool refreshed = slot >= 0;
        if (!refreshed)
            for (u32 i = 0; i < kStatusSlots && slot < 0; ++i)
                if (s->key[i] == 0) slot = static_cast<i32>(i);
        if (slot < 0) {
            ev.push(StatusRefused{a.target, a.id});
            continue;
        }
        const u32 i = static_cast<u32>(slot);
        if (refreshed && s->kind[i] != static_cast<u8>(a.kind)) {  // a new kind: give back what the old one added first
            if (Health3D* hp = w.try_get_mut<Health3D>(a.target)) settle_bonus(*hp, static_cast<StatusKind>(s->kind[i]), s->applied[i], 0.0f);
            s->applied[i] = 0.0f;
        }
        if (!refreshed) s->applied[i] = 0.0f;
        s->key[i] = key;
        s->id[i] = a.id;
        s->kind[i] = static_cast<u8>(a.kind);
        s->amount[i] = a.amount;
        s->left_us[i] = us_of(a.seconds);
        if (!refreshed) s->pulse_us[i] = 0;
        s->source[i] = a.source;
        ev.push(StatusApplied{a.target, a.id, a.kind, refreshed ? 1u : 0u});
    }
    const u32 dt_us = step_us(f.dt);
    f.query<StatusEffects>().each([&](Entity e, StatusEffects& s) {
        Health3D* hp = w.try_get_mut<Health3D>(e);
        for (u32 i = 0; i < kStatusSlots; ++i) {
            if (s.key[i] == 0) continue;
            const StatusKind kind = static_cast<StatusKind>(s.kind[i]);
            if (s.kind[i] > static_cast<u8>(StatusKind::Cooldown) || !std::isfinite(s.amount[i]) || !std::isfinite(s.applied[i])) {
                clear_slot(s, i);  // a hostile or corrupt save: drop it rather than let it poison Health3D
                continue;
            }
            if (s.pulse_us[i] >= kStatusPulseUs) s.pulse_us[i] = 0;
            const u32 used = dt_us < s.left_us[i] ? dt_us : s.left_us[i];  // the last tick is a short one
            if (hp && !hp->dead) {
                if (kind == StatusKind::Regen) {
                    const f32 gain = s.amount[i] * (static_cast<f32>(used) * 1.0e-6f);
                    hp->current = gain > 0.0f ? math::min(hp->current + gain, hp->max) : hp->current;
                } else if (kind == StatusKind::Poison) {
                    s.pulse_us[i] += used;
                    while (s.pulse_us[i] >= kStatusPulseUs) {
                        s.pulse_us[i] -= kStatusPulseUs;
                        DamageInfo d;
                        d.amount = s.amount[i] * (static_cast<f32>(kStatusPulseUs) * 1.0e-6f);
                        d.instigator = s.source[i];
                        apply_damage(w, e, d);
                        if (hp->dead) break;
                    }
                } else {
                    settle_bonus(*hp, kind, s.applied[i], s.amount[i]);
                }
            }
            s.left_us[i] -= used;
            if (s.left_us[i] != 0) continue;
            if (hp) settle_bonus(*hp, kind, s.applied[i], 0.0f);
            ev.push(StatusEnded{e, s.id[i], 1});
            clear_slot(s, i);
        }
    });
}

}  // namespace

u32 status_key(StringView id) {
    const u64 h = fnv1a64(id);
    const u32 k = static_cast<u32>(h ^ (h >> 32));
    return k == 0 ? 1u : k;
}

i32 status_find(const StatusEffects& s, u32 key) {
    for (u32 i = 0; i < kStatusSlots; ++i)
        if (s.key[i] == key && key != 0) return static_cast<i32>(i);
    return -1;
}

void install_status(World& world, asset::AssetSystem* assets) {
    install_crafting(world, assets);
    if (world.try_resource<StatusInstalled>()) return;
    world.set_resource(StatusInstalled{});
    world.register_components<StatusEffects>();
    world.register_event<ApplyStatus>(Phase::FixedUpdate_Pre);
    world.register_event<ClearStatus>(Phase::FixedUpdate_Pre);
    world.register_event<StatusApplied>(Phase::FixedUpdate_Pre);
    world.register_event<StatusEnded>(Phase::FixedUpdate_Pre);
    world.register_event<StatusRefused>(Phase::FixedUpdate_Pre);
    world.add_system(Phase::FixedUpdate_Pre, "kit_status", &kit_status, "kit_craft");
}

}  // namespace mce::kit
