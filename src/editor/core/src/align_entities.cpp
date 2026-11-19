#include "mce/editor/align_entities.h"

#include <algorithm>
#include <cmath>

#include "mce/ecs/transform.h"
#include "mce/editor/parents_first.h"

namespace mce::editor {

namespace {

struct Item {
    EntityId id;
    f32 pos;
};

f32& axis_of(WorldTransform& w, AlignAxis axis) { return axis == AlignAxis::X ? w.position.x : w.position.y; }

// The entities of `ids` that have a transform, once each, with their world position on `axis`.
Array<Item> collect(const EditorScene& scene, Span<const EntityId> ids, AlignAxis axis) {
    Array<Item> out;
    for (EntityId id : ids) {
        if (!scene.get<LocalTransform>(id)) continue;
        bool seen = false;
        for (const Item& it : out) seen = seen || it.id == id;
        if (seen) continue;
        WorldTransform w = scene.world_transform(id);
        out.push_back({id, axis_of(w, axis)});
    }
    return out;
}

// Puts `id` at `value` on `axis` (world space); true when its local transform changed.
bool move_to(EditorScene& scene, EntityId id, AlignAxis axis, f32 value) {
    WorldTransform w = scene.world_transform(id);
    f32& p = axis_of(w, axis);
    if (std::fabs(p - value) <= 1e-6f) return false;
    p = value;
    return scene.set(id, scene.local_for_world(id, w));
}

}  // namespace

bool align_entities(EditorScene& scene, Span<const EntityId> ids, AlignAxis axis, AlignMode mode) {
    const Array<Item> items = collect(scene, ids, axis);
    if (items.size() < 2) return false;
    f32 lo = items[0].pos, hi = items[0].pos;
    for (const Item& it : items) {
        lo = std::min(lo, it.pos);
        hi = std::max(hi, it.pos);
    }
    const f32 target = mode == AlignMode::Min ? lo : mode == AlignMode::Max ? hi : (lo + hi) * 0.5f;
    // Parents first: a parent that moves carries its children, which then still have to land on the target.
    Array<Item> order = items;
    sort_parents_first(scene, order, [](const Item& it) { return it.id; });
    bool moved = false;
    scene.begin_edit("Align");
    for (const Item& it : order) moved = move_to(scene, it.id, axis, target) || moved;
    scene.end_edit();
    return moved;
}

bool distribute_entities(EditorScene& scene, Span<const EntityId> ids, AlignAxis axis) {
    Array<Item> items = collect(scene, ids, axis);
    if (items.size() < 3) return false;
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.pos < b.pos; });
    const f32 lo = items[0].pos;
    const f32 step = (items[items.size() - 1].pos - lo) / static_cast<f32>(items.size() - 1);
    // Every entity gets its slot (the two outermost keep theirs), applied parents first: a middle entity that moves
    // carries its children, an outermost one among them included, and they have to be put back.
    struct Slot {
        EntityId id;
        f32 value;
    };
    Array<Slot> slots;
    for (usize i = 0; i < items.size(); ++i) slots.push_back({items[i].id, lo + step * static_cast<f32>(i)});
    slots[0].value = items[0].pos;
    slots[slots.size() - 1].value = items[items.size() - 1].pos;
    sort_parents_first(scene, slots, [](const Slot& s) { return s.id; });
    bool moved = false;
    scene.begin_edit("Distribute");
    for (const Slot& sl : slots) moved = move_to(scene, sl.id, axis, sl.value) || moved;
    scene.end_edit();
    return moved;
}

}  // namespace mce::editor
