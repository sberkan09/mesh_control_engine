// Raycast3D and Overlap3D (M-GAMEPLAY3D step 6): physics::World3D queries run every tick from an
// entity, their changes pushed as TriggerEnter3D / TriggerExit3D so PhysicsEvents and effects see
// them like a Trigger3D's.

#include "mce/kit/sensors3d.h"

#include "kit3d_internal.h"
#include "mce/containers/array.h"
#include "mce/ecs/hierarchy.h"
#include "mce/ecs/world.h"
#include "mce/kit/kit3d.h"

namespace mce::kit {

// World::state_hash hashes component bytes: these layouts have no implicit padding.
static_assert(sizeof(Raycast3D) == 80);
static_assert(sizeof(Overlap3D) == 52);

namespace {

using math::quat;
using math::vec3;

constexpr u32 kMaxOverlaps = 64;  // entities one Overlap3D tracks

// What each sensor saw last tick, in the order sensors were first seen (so exits replay in order).
struct Sensors3DState {
    struct Entry {
        Entity sensor;
        u8 kind = 0;  // 0 Raycast3D, 1 Overlap3D
        bool seen = false;
        Array<Entity> inside;
    };
    Array<Entry> entries;

    Entry& entry(Entity e, u8 kind) {
        for (Entry& x : entries)
            if (x.sensor == e && x.kind == kind) return x;
        entries.push_back(Entry{e, kind, false, {}});
        return entries.back();
    }
};

bool contains(const Array<Entity>& list, Entity e) {
    for (const Entity x : list)
        if (x == e) return true;
    return false;
}

// The sensor itself and every entity it hangs under.
usize self_and_parents(World& w, Entity e, Entity (&out)[16]) {
    usize n = 0;
    for (Entity x = e; !x.is_null() && n < 16 && w.alive(x);) {
        out[n++] = x;
        const ChildOf* c = w.try_get<ChildOf>(x);
        x = c ? c->parent : Entity{};
    }
    return n;
}

// Pushes the enters and exits between `before` and `now`; returns the last entity that changed.
template <typename OnEnter>
Entity diff(Frame& f, World& w, Entity sensor, const Array<Entity>& before, const Array<Entity>& now, OnEnter&& on_enter) {
    auto events = f.events();
    Entity last;
    for (const Entity x : before)
        if (!contains(now, x)) {
            events.push(TriggerExit3D{sensor, x});
            last = x;
        }
    for (const Entity x : now)
        if (!contains(before, x) && w.alive(x)) {
            events.push(TriggerEnter3D{sensor, x});
            on_enter();
            last = x;
        }
    return last;
}

void kit3d_sensors(Frame& f) {
    World& w = f.world();
    Physics3D& phys = w.resource<Physics3D>();
    Sensors3DState& st = w.resource<Sensors3DState>();
    for (Sensors3DState::Entry& e : st.entries) e.seen = false;

    f.query<Raycast3D&, LocalTransform&>().each([&](Entity e, Raycast3D& r, LocalTransform& t) {
        Sensors3DState::Entry& entry = st.entry(e, 0);
        entry.seen = true;
        Array<Entity> now;
        r.hit = Entity{};
        r.point = r.normal = {};
        r.hit_distance = 0.0f;
        vec3 pos, scale;
        quat rot;
        world_pose(w, e, t, pos, rot, scale);
        const vec3 dir = math::rotate(rot, r.direction);
        if (r.enabled && r.distance > 0.0f && math::length_sq(dir) > 0.0f) {
            Entity skip[16];
            physics::QueryFilter3D filter;
            filter.layer_mask = r.mask;
            filter.include_dynamic = r.dynamic != 0;
            filter.include_characters = r.characters != 0 && !(r.radius > 0.0f);
            filter.exclude = Span<const Entity>(skip, self_and_parents(w, e, skip));
            const vec3 from = pos + math::rotate(rot, r.offset * scale);
            if (r.radius > 0.0f) {
                const vec3 unit = math::normalize(dir);
                if (const std::optional<physics::ShapeHit3D> h = phys.world.shape_cast(r.radius, from, from + unit * r.distance, filter)) {
                    r.hit = h->entity;
                    r.point = h->point;
                    r.normal = h->normal;
                    r.hit_distance = h->t * r.distance;
                }
            } else if (const std::optional<physics::RayHit3D> h = phys.world.raycast(from, dir, r.distance, filter)) {
                r.hit = h->entity;
                r.point = h->point;
                r.normal = h->normal;
                r.hit_distance = h->t;
            }
        }
        if (!r.hit.is_null()) now.push_back(r.hit);
        diff(f, w, e, entry.inside, now, [&] { ++r.hits; });
        entry.inside = static_cast<Array<Entity>&&>(now);
    });

    f.query<Overlap3D&, LocalTransform&>().each([&](Entity e, Overlap3D& o, LocalTransform& t) {
        Sensors3DState::Entry& entry = st.entry(e, 1);
        entry.seen = true;
        Array<Entity> now;
        if (o.enabled) {
            vec3 pos, scale;
            quat rot;
            world_pose(w, e, t, pos, rot, scale);
            Entity skip[16];
            physics::QueryFilter3D filter;
            filter.layer_mask = o.mask;
            filter.include_dynamic = o.dynamic != 0;
            filter.include_characters = o.characters != 0;
            filter.exclude = Span<const Entity>(skip, self_and_parents(w, e, skip));
            const vec3 center = pos + math::rotate(rot, o.offset * scale);
            Entity found[kMaxOverlaps];
            const u32 n = o.shape == TriggerShape3D::Sphere
                              ? phys.world.overlap_sphere(center, o.radius * math::max(std::fabs(scale.x), math::max(std::fabs(scale.y), std::fabs(scale.z))),
                                                          Span<Entity>(found, kMaxOverlaps), filter)
                              : phys.world.overlap_box(center, {std::fabs(o.half.x * scale.x), std::fabs(o.half.y * scale.y), std::fabs(o.half.z * scale.z)},
                                                       rot, Span<Entity>(found, kMaxOverlaps), filter);
            for (u32 i = 0; i < n && i < kMaxOverlaps; ++i) now.push_back(found[i]);
        }
        const Entity last = diff(f, w, e, entry.inside, now, [&] { ++o.enters; });
        if (!last.is_null()) o.last_other = last;
        o.count = static_cast<u32>(now.size());
        entry.inside = static_cast<Array<Entity>&&>(now);
    });

    // Sensors gone (the component removed, the entity destroyed): everything inside leaves.
    auto events = f.events();
    for (usize i = 0; i < st.entries.size();) {
        Sensors3DState::Entry& e = st.entries[i];
        if (e.seen) {
            ++i;
            continue;
        }
        for (const Entity x : e.inside) events.push(TriggerExit3D{e.sensor, x});
        st.entries.erase(i);
    }
}

}  // namespace

void install_sensors3d(World& world, asset::AssetSystem* assets) {
    install_bodies3d(world, assets);
    if (world.try_resource<Sensors3DState>()) return;
    world.set_resource(Sensors3DState{});
    world.register_components<Raycast3D, Overlap3D>();
    world.add_system(Phase::FixedUpdate_Physics, "kit3d_sensors", &kit3d_sensors, "kit3d_triggers");
}

}  // namespace mce::kit
