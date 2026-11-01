#include "mce/kit/hit_marker.h"

#include "mce/audio/components.h"
#include "mce/ecs/world.h"
#include "mce/kit/combat3d.h"

namespace mce::kit {

static_assert(sizeof(HitMarker3D) == 72);

namespace {

struct HitMarkerInstalled {};

void play(Frame& f, const asset::AssetRef<asset::Sfx>& clip, f32 gain_db) {
    if (clip.guid == asset::Guid{} || !f.world().event_queue<audio::PlaySfx>()) return;
    audio::PlaySfx e;
    e.clip = clip.guid;
    e.gain_db = gain_db;
    f.events().push(e);
}

void kit_hit_marker(Frame& f) {
    World& w = f.world();
    auto ev = f.events();
    for (const WeaponHit3D& h : ev.read<WeaponHit3D>()) {
        if (h.owner.is_null() || h.owner == h.target || !w.alive(h.owner) || !w.alive(h.target)) continue;
        HitMarker3D* m = w.try_get_mut<HitMarker3D>(h.owner);
        const Health3D* hp = w.try_get<Health3D>(h.target);
        if (!m || !hp || hp->dead) continue;
        m->timer = m->duration;
        ++m->hits;
        play(f, m->hit_clip, m->gain_db);
    }
    for (const Died3D& d : ev.read<Died3D>()) {
        if (d.killer.is_null() || d.killer == d.entity || !w.alive(d.killer)) continue;
        if (HitMarker3D* m = w.try_get_mut<HitMarker3D>(d.killer)) {
            m->kill_timer = m->duration * 2.0f;
            play(f, m->kill_clip, m->gain_db);
        }
    }
    f.query<HitMarker3D>().each([&](Entity, HitMarker3D& m) {
        m.timer = m.timer > f.dt ? m.timer - f.dt : 0.0f;
        m.kill_timer = m.kill_timer > f.dt ? m.kill_timer - f.dt : 0.0f;
    });
}

}  // namespace

void install_hit_marker(World& world, asset::AssetSystem*) {
    if (world.try_resource<HitMarkerInstalled>()) return;
    world.set_resource(HitMarkerInstalled{});
    world.register_components<HitMarker3D>();
    world.add_system(Phase::FixedUpdate_Pre, "kit_hit_marker", &kit_hit_marker, "kit3d_health");
}

}  // namespace mce::kit
