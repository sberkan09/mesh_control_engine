#include "mce/ecs/origin.h"

#include <cstring>

#include "mce/containers/array.h"
#include "mce/ecs/hierarchy.h"
#include "mce/ecs/world.h"
#include "mce/log.h"
#include "mce/refl/type.h"

namespace mce {

namespace {

struct OriginListeners {
    struct Entry {
        OriginShiftFn fn = nullptr;
        void* user = nullptr;
    };
    Array<Entry> entries;
};

struct PointField {
    ecs::ComponentId component;
    u32 offset;
    u32 count;
};

// The WorldPoint fields of every registered reflected component, as (component, offset, count).
void point_fields(World& w, Array<PointField>& out) {
    for (usize ci = 0; ci < w.component_count(); ++ci) {
        const ecs::ComponentInfo& info = w.component(static_cast<ecs::ComponentId>(ci));
        if (info.is_tag || info.refl_id == 0) continue;
        const refl::TypeRecord* t = refl::find_type(StringId(info.refl_id));
        if (!t) continue;
        for (const refl::FieldRecord& f : t->fields)
            if (f.has(refl::FieldFlags::WorldPoint) && f.kind == refl::FieldKind::Vec3)
                out.push_back(PointField{static_cast<ecs::ComponentId>(ci), f.offset, f.count});
    }
}

void sub(math::vec3& p, const math::vec3& d) {
    p.x -= d.x;
    p.y -= d.y;
    p.z -= d.z;
}

void shift_world_transform(WorldTransform& wt, const math::vec3& d) {
    sub(wt.position, d);
    wt.matrix.c[3][0] -= d.x;
    wt.matrix.c[3][1] -= d.y;
    wt.matrix.c[3][2] -= d.z;
}

void shift_point_fields(u8* row, Span<const PointField> fields, ecs::ComponentId c, const math::vec3& d) {
    for (const PointField& f : fields) {
        if (f.component != c) continue;
        for (u32 i = 0; i < f.count; ++i) {
            math::vec3 p;
            std::memcpy(&p, row + f.offset + i * sizeof(math::vec3), sizeof(p));
            sub(p, d);
            std::memcpy(row + f.offset + i * sizeof(math::vec3), &p, sizeof(p));
        }
    }
}

}  // namespace

WorldOrigin& install_world_origin(World& world, f32 snap) {
    MCE_ASSERT_MSG(snap > 0.0f, "install_world_origin: snap must be positive");
    if (WorldOrigin* o = world.try_resource<WorldOrigin>()) {
        if (o->cell[0] == 0 && o->cell[1] == 0 && o->cell[2] == 0) o->snap = snap;
        return *o;
    }
    WorldOrigin o;
    o.snap = snap;
    return world.set_resource(o);
}

WorldOrigin* world_origin(World& world) { return world.try_resource<WorldOrigin>(); }

void on_origin_shift(World& world, OriginShiftFn fn, void* user) {
    OriginListeners* l = world.try_resource<OriginListeners>();
    if (!l) l = &world.set_resource(OriginListeners{});
    for (const OriginListeners::Entry& e : l->entries)
        if (e.fn == fn && e.user == user) return;
    l->entries.push_back({fn, user});
}

void remove_origin_shift(World& world, OriginShiftFn fn, void* user) {
    OriginListeners* l = world.try_resource<OriginListeners>();
    if (!l) return;
    for (usize i = 0; i < l->entries.size(); ++i)
        if (l->entries[i].fn == fn && l->entries[i].user == user) {
            l->entries.erase(i);
            return;
        }
}

void shift_entities(World& world, Span<const Entity> entities, const math::vec3& delta) {
    if (delta.x == 0.0f && delta.y == 0.0f && delta.z == 0.0f) return;
    Array<PointField> fields;
    point_fields(world, fields);
    const ecs::ComponentId local_id = world.find_component(kTypeHash<LocalTransform>);
    const ecs::ComponentId world_id = world.find_component(kTypeHash<WorldTransform>);
    const ecs::ComponentId child_id = world.find_component(kTypeHash<ChildOf>);
    for (Entity e : entities) {
        if (!world.alive(e)) continue;
        if (local_id != ecs::kInvalidComponent && (child_id == ecs::kInvalidComponent || !world.has(e, child_id)))
            if (void* p = world.get_raw_mut(e, local_id)) sub(static_cast<LocalTransform*>(p)->position, delta);
        if (world_id != ecs::kInvalidComponent)
            if (void* p = world.get_raw_mut(e, world_id)) shift_world_transform(*static_cast<WorldTransform*>(p), delta);
        for (const PointField& f : fields)
            if (void* p = world.get_raw_mut(e, f.component)) shift_point_fields(static_cast<u8*>(p), Span<const PointField>(&f, 1), f.component, delta);
    }
}

void rebase_world(World& world, const i32 cells[3]) {
    MCE_ASSERT_MSG(!world.in_phase(), "rebase_world moves the whole world: call it between ticks");
    if (cells[0] == 0 && cells[1] == 0 && cells[2] == 0) return;
    WorldOrigin& o = install_world_origin(world, world_origin(world) ? world_origin(world)->snap : 256.0f);
    const math::vec3 delta{f32(cells[0]) * o.snap, f32(cells[1]) * o.snap, f32(cells[2]) * o.snap};

    Array<PointField> fields;
    point_fields(world, fields);
    const ecs::ComponentId local_id = world.find_component(kTypeHash<LocalTransform>);
    const ecs::ComponentId world_id = world.find_component(kTypeHash<WorldTransform>);
    const ecs::ComponentId child_id = world.find_component(kTypeHash<ChildOf>);
    // Column-wise over the tables: no change stamps, no per-entity lookups.
    for (const auto& tp : world.tables()) {
        const ecs::detail::Table& t = *tp;
        const u32 rows = t.size();
        if (rows == 0) continue;
        const bool root = child_id == ecs::kInvalidComponent || !t.mask().test(child_id);
        if (root && local_id != ecs::kInvalidComponent)
            if (u8* col = t.column(local_id))
                for (u32 r = 0; r < rows; ++r) sub(reinterpret_cast<LocalTransform*>(col)[r].position, delta);
        if (world_id != ecs::kInvalidComponent)
            if (u8* col = t.column(world_id))
                for (u32 r = 0; r < rows; ++r) shift_world_transform(reinterpret_cast<WorldTransform*>(col)[r], delta);
        for (const PointField& f : fields) {
            u8* col = t.column(f.component);
            if (!col) continue;
            const u32 size = world.component(f.component).size;
            for (u32 r = 0; r < rows; ++r) shift_point_fields(col + usize(r) * size, Span<const PointField>(&f, 1), f.component, delta);
        }
    }

    for (i32 i = 0; i < 3; ++i) o.cell[i] += cells[i];
    ++o.serial;
    o.last_delta = delta;
    if (OriginListeners* l = world.try_resource<OriginListeners>()) {
        // A listener may register another: iterate by index over a copy of the count.
        const usize n = l->entries.size();
        for (usize i = 0; i < n; ++i) {
            const OriginListeners::Entry e = l->entries[i];
            e.fn(world, delta, e.user);
        }
    }
    MCE_LOG_INFO("floating origin: rebased by ({}, {}, {}) m, origin cell ({}, {}, {})", delta.x, delta.y, delta.z, o.cell[0],
                 o.cell[1], o.cell[2]);
}

math::vec3 origin_offset(World& world) {
    const WorldOrigin* o = world_origin(world);
    if (!o) return {};
    return {f32(o->cell[0]) * o->snap, f32(o->cell[1]) * o->snap, f32(o->cell[2]) * o->snap};
}

WorldPoint64 to_absolute(World& world, const math::vec3& local) {
    const WorldOrigin* o = world_origin(world);
    if (!o) return {f64(local.x), f64(local.y), f64(local.z)};
    const f64 s = f64(o->snap);
    return {f64(local.x) + f64(o->cell[0]) * s, f64(local.y) + f64(o->cell[1]) * s, f64(local.z) + f64(o->cell[2]) * s};
}

math::vec3 to_local(World& world, const WorldPoint64& a) {
    const WorldOrigin* o = world_origin(world);
    if (!o) return {f32(a.x), f32(a.y), f32(a.z)};
    const f64 s = f64(o->snap);
    return {f32(a.x - f64(o->cell[0]) * s), f32(a.y - f64(o->cell[1]) * s), f32(a.z - f64(o->cell[2]) * s)};
}

}  // namespace mce
