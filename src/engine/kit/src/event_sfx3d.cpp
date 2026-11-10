#include "mce/kit/event_sfx3d.h"

#include "mce/audio/components.h"
#include "mce/ecs/world.h"
#include "mce/kit/ammopickup3d.h"
#include "mce/kit/grenade3d.h"
#include "mce/kit/loadout3d.h"
#include "mce/kit/melee3d.h"
#include "mce/kit/progress3d.h"
#include "mce/kit/wanted3d.h"
#include "mce/kit/weapon_events3d.h"

namespace mce::kit {

static_assert(sizeof(EventSfx3D) == 280);

namespace {

struct EventSfx3DInstalled {};

void play(Frame& f, const asset::AssetRef<asset::Sfx>& clip, f32 gain_db) {
    if (clip.guid == asset::Guid{} || !f.world().event_queue<audio::PlaySfx>()) return;
    audio::PlaySfx e;
    e.clip = clip.guid;
    e.gain_db = gain_db;
    f.events().push(e);
}

const EventSfx3D* holder(World& w, Entity e) {
    if (e.is_null() || !w.alive(e)) return nullptr;
    return w.try_get<EventSfx3D>(e);
}

void kit_event_sfx(Frame& f) {
    World& w = f.world();
    auto ev = f.events();
    for (const GrenadeThrown3D& e : ev.read<GrenadeThrown3D>())
        if (const EventSfx3D* s = holder(w, e.entity)) play(f, s->throw_clip, s->throw_db);
    for (const GrenadeBlast3D& e : ev.read<GrenadeBlast3D>())
        if (const EventSfx3D* s = holder(w, e.owner)) play(f, s->blast_clip, s->blast_db);
    for (const MeleeSwung3D& e : ev.read<MeleeSwung3D>())
        if (const EventSfx3D* s = holder(w, e.entity)) play(f, s->melee_clip, s->melee_db);
    for (const CashPickedUp3D& e : ev.read<CashPickedUp3D>())
        if (const EventSfx3D* s = holder(w, e.entity)) play(f, s->cash_clip, s->cash_db);
    for (const AmmoPickedUp3D& e : ev.read<AmmoPickedUp3D>())
        if (const EventSfx3D* s = holder(w, e.entity)) play(f, s->ammo_clip, s->ammo_db);
    for (const GoalDone3D& e : ev.read<GoalDone3D>())
        if (const EventSfx3D* s = holder(w, e.owner)) play(f, s->goal_clip, s->goal_db);
    if (w.event_queue<WeaponReloadStarted3D>())
        for (const WeaponReloadStarted3D& e : ev.read<WeaponReloadStarted3D>())
            if (const EventSfx3D* s = holder(w, e.owner)) play(f, s->reload_start_clip, s->reload_start_db);
    if (w.event_queue<WeaponReloaded3D>())
        for (const WeaponReloaded3D& e : ev.read<WeaponReloaded3D>())
            if (const EventSfx3D* s = holder(w, e.owner)) play(f, s->reloaded_clip, s->reloaded_db);
    if (w.event_queue<WeaponDry3D>())
        for (const WeaponDry3D& e : ev.read<WeaponDry3D>())
            if (const EventSfx3D* s = holder(w, e.owner)) play(f, s->dry_clip, s->dry_db);
    for (const WeaponSwitched3D& e : ev.read<WeaponSwitched3D>())
        if (const EventSfx3D* s = holder(w, e.entity)) play(f, s->switch_clip, s->switch_db);
}

}  // namespace

void install_event_sfx3d(World& world) {
    if (world.try_resource<EventSfx3DInstalled>()) return;
    world.set_resource(EventSfx3DInstalled{});
    install_wanted3d(world);
    install_melee3d(world);
    install_grenade3d(world);
    install_ammopickup3d(world);
    install_progress3d(world);
    install_loadout3d(world);
    world.register_components<EventSfx3D>();
    world.add_system(Phase::FixedUpdate_Post, "kit_event_sfx", &kit_event_sfx);
}

}  // namespace mce::kit
