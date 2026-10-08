#include "mce/ecs/hierarchy.h"

#include <algorithm>

#include "mce/log.h"

namespace mce::ecs {

namespace {

constexpr u32 kMaxDepth = 1024;  // a deeper chain is treated as corrupt (cycle guard)

// Parent -> children index; users never write it (ecs-gameplay.md §3.7).
struct HierarchyIndex {
    HashMap<Entity, Entity> parent;           // child -> parent it is linked under
    HashMap<Entity, Array<Entity>> children;  // parent -> children in attach order
    Array<Entity> relinked;  // attached/detached since the last propagation: always re-seeded
    HierarchyStats stats;
    u32 direct_since = 0;  // reader tick of propagate_transforms() calls
    bool direct_ran = false;
};

HierarchyIndex& index_of(World& w) {
    HierarchyIndex* idx = w.try_resource<HierarchyIndex>();
    MCE_ASSERT_MSG(idx != nullptr, "ecs::install_hierarchy(world) has not been called");
    return *idx;
}

bool is_ancestor_or_self(const HierarchyIndex& idx, Entity maybe_ancestor, Entity e) {
    u32 guard = 0;
    for (Entity cur = e; !cur.is_null() && guard < kMaxDepth; ++guard) {
        if (cur == maybe_ancestor) return true;
        const Entity* p = idx.parent.find(cur);
        cur = p ? *p : Entity::null;
    }
    return guard >= kMaxDepth;
}

void unlink(HierarchyIndex& idx, Entity child) {
    const Entity* p = idx.parent.find(child);
    if (!p) return;
    if (Array<Entity>* kids = idx.children.find(*p)) {
        const usize i = kids->find(child);
        if (i != kids->size()) kids->erase(i);
        if (kids->empty()) idx.children.erase(*p);
    }
    idx.parent.erase(child);
    idx.relinked.push_back(child);
}

// Links child under parent in the index. False (and the child stays a root) on a cycle.
bool link(World& w, HierarchyIndex& idx, Entity child, Entity parent) {
    const Entity* cur = idx.parent.find(child);
    if (cur && *cur == parent) return true;
    unlink(idx, child);
    if (parent.is_null()) return true;
    if (!w.alive(parent)) return false;
    if (is_ancestor_or_self(idx, child, parent)) {
        ++idx.stats.cycles_rejected;
        MCE_LOG_WARN("ChildOf rejected: entity {}:{} would become its own ancestor", child.index, child.generation);
        return false;
    }
    idx.parent.insert_or_assign(child, parent);
    idx.relinked.push_back(child);
    auto [kids, inserted] = idx.children.try_emplace(parent);
    (void)inserted;
    kids->push_back(child);
    return true;
}

void on_child_of_added(World& w, Entity e) {
    HierarchyIndex& idx = index_of(w);
    if (const ChildOf* c = w.try_get<ChildOf>(e)) link(w, idx, e, c->parent);
}

void on_child_of_removed(World& w, Entity e) { unlink(index_of(w), e); }

// Orphans-destroy: a destroyed parent takes its subtree with it.
void on_destroy(World& w, Entity e) {
    HierarchyIndex& idx = index_of(w);
    Array<Entity>* kids = idx.children.find(e);
    if (!kids) return;
    Array<Entity> copy;
    for (Entity c : *kids) copy.push_back(c);
    for (Entity c : copy) {
        const ChildOf* co = w.try_get<ChildOf>(c);
        if (w.alive(c) && co && co->parent == e) w.destroy(c);
    }
    idx.children.erase(e);
}

struct WalkItem {
    Entity e;
    WorldTransform parent_world;
    bool has_parent;
};

struct PendingAdd {
    Entity e;
    WorldTransform value;
};

// World pose of `e` from whatever is current: its WorldTransform when present, else its chain.
WorldTransform current_world(World& w, HierarchyIndex& idx, Entity e, u32 depth = 0) {
    if (const WorldTransform* wt = w.try_get<WorldTransform>(e)) return *wt;
    const LocalTransform* lt = w.try_get<LocalTransform>(e);
    const LocalTransform local = lt ? *lt : LocalTransform{};
    const Entity* p = idx.parent.find(e);
    if (!p || depth >= kMaxDepth) return to_world(local);
    return compose(current_world(w, idx, *p, depth + 1), local);
}

void run_propagation(World& w, u32 since, bool has_since) {
    HierarchyIndex& idx = index_of(w);
    HierarchyStats& st = idx.stats;
    st.seeds = 0;
    st.propagated = 0;
    ++st.runs;

    // 1. Reconcile ChildOf writes the hooks did not see (set on an existing component).
    Array<Entity> orphans;
    {
        auto q = w.query<const ChildOf&>().changed<ChildOf>();
        if (has_since) q.since(since);
        q.each([&](Entity e, const ChildOf& c) {
            if (!c.parent.is_null() && !w.alive(c.parent)) {
                orphans.push_back(e);
                return;
            }
            link(w, idx, e, c.parent);
        });
    }

    // 2. Seeds (deduplicated, ordered ancestors-first).
    HashMap<Entity, u8> seen;
    Array<Entity> seeds;
    auto seed = [&](Entity e) {
        if (seen.try_emplace(e, u8(0)).second) seeds.push_back(e);
    };
    {
        auto q = w.query<const LocalTransform&>().changed<LocalTransform>();
        if (has_since) q.since(since);
        q.each([&](Entity e, const LocalTransform&) { seed(e); });
    }
    {
        auto q = w.query<const ChildOf&>().changed<ChildOf>();
        if (has_since) q.since(since);
        q.each([&](Entity e, const ChildOf&) { seed(e); });
    }
    for (Entity e : idx.relinked)
        if (w.alive(e)) seed(e);
    idx.relinked.clear();
    w.query<const LocalTransform&>().without<WorldTransform>().each([&](Entity e, const LocalTransform&) { seed(e); });
    w.query<const ChildOf&>().without<WorldTransform>().each([&](Entity e, const ChildOf&) { seed(e); });
    st.seeds = static_cast<u32>(seeds.size());

    struct Keyed {
        u32 depth;
        Entity e;
    };
    Array<Keyed> order;
    for (Entity e : seeds) order.push_back(Keyed{depth_of(w, e), e});
    std::sort(order.begin(), order.end(), [](const Keyed& a, const Keyed& b) {
        if (a.depth != b.depth) return a.depth < b.depth;
        if (a.e.index != b.e.index) return a.e.index < b.e.index;
        return a.e.generation < b.e.generation;
    });

    // 3. Depth-first walk from every not-yet-visited seed.
    HashMap<Entity, u8> visited;
    Array<PendingAdd> adds;
    Array<WalkItem> stack;
    for (const Keyed& k : order) {
        if (visited.contains(k.e)) continue;
        WalkItem root{k.e, WorldTransform{}, false};
        if (const Entity* p = idx.parent.find(k.e)) {
            root.parent_world = current_world(w, idx, *p);
            root.has_parent = true;
        }
        stack.push_back(root);
        while (!stack.empty()) {
            const WalkItem it = stack.back();
            stack.pop_back();
            if (!visited.try_emplace(it.e, u8(0)).second) continue;
            const LocalTransform* lt = w.try_get<LocalTransform>(it.e);
            const LocalTransform local = lt ? *lt : LocalTransform{};
            const WorldTransform world = it.has_parent ? compose(it.parent_world, local) : to_world(local);
            if (WorldTransform* wt = w.try_get_mut<WorldTransform>(it.e)) {
                *wt = world;
            } else {
                adds.push_back(PendingAdd{it.e, world});
            }
            ++st.propagated;
            if (const Array<Entity>* kids = idx.children.find(it.e)) {
                // Reverse push so children pop in attach order.
                for (usize i = kids->size(); i-- > 0;) stack.push_back(WalkItem{(*kids)[i], world, true});
            }
        }
    }

    // 4. Structural follow-ups: deferred inside a phase, immediate outside.
    if (w.in_phase()) {
        for (const PendingAdd& a : adds) w.commands().add(a.e, a.value);
        for (Entity e : orphans) w.commands().destroy(e);
    } else {
        for (const PendingAdd& a : adds) w.set(a.e, a.value);
        for (Entity e : orphans) w.destroy(e);
    }
}

void propagate_system(Frame& f) {
    World& w = f.world();
    const u32 since = w.reader_tick();
    run_propagation(w, since, since != 0);
}

}  // namespace

WorldTransform to_world(const LocalTransform& local) {
    WorldTransform out;
    out.position = local.position;
    out.rotation = local.rotation;
    out.scale = local.scale;
    out.matrix = math::trs(out.position, out.rotation, out.scale);
    return out;
}

WorldTransform compose(const WorldTransform& parent, const LocalTransform& local) {
    WorldTransform out;
    out.position = parent.position + math::rotate(parent.rotation, parent.scale * local.position);
    out.rotation = parent.rotation * local.rotation;
    out.scale = parent.scale * local.scale;
    out.matrix = math::trs(out.position, out.rotation, out.scale);
    return out;
}

bool hierarchy_installed(World& w) { return w.try_resource<HierarchyIndex>() != nullptr; }

void install_hierarchy(World& w) {
    if (hierarchy_installed(w)) return;
    w.register_components<LocalTransform, ChildOf, WorldTransform>();
    w.set_resource(HierarchyIndex{});
    w.set_hooks<ChildOf>(&on_child_of_added, &on_child_of_removed);
    w.add_destroy_hook(&on_destroy);
    w.add_system(Phase::FixedUpdate_Post, "transform_propagate", &propagate_system);
}

void propagate_transforms(World& w) {
    MCE_ASSERT_MSG(!w.in_phase(), "propagate_transforms runs outside phases (the system covers in-phase)");
    HierarchyIndex& idx = index_of(w);
    const u32 since = idx.direct_since;
    const bool has_since = idx.direct_ran;
    run_propagation(w, since, has_since);
    idx.direct_since = w.change_tick();
    idx.direct_ran = true;
}

bool set_parent(World& w, Entity child, Entity parent) {
    HierarchyIndex& idx = index_of(w);
    if (!w.alive(child)) return false;
    if (parent.is_null()) {
        w.remove<ChildOf>(child);
        return true;
    }
    if (!w.alive(parent) || is_ancestor_or_self(idx, child, parent)) {
        if (w.alive(parent)) ++idx.stats.cycles_rejected;
        return false;
    }
    w.set(child, ChildOf{parent});  // hook links on add; relink here covers the existing-component case
    return link(w, idx, child, parent);
}

Entity parent_of(World& w, Entity e) {
    const Entity* p = index_of(w).parent.find(e);
    return p ? *p : Entity::null;
}

Span<const Entity> children_of(World& w, Entity e) {
    const Array<Entity>* kids = index_of(w).children.find(e);
    return kids ? kids->span() : Span<const Entity>();
}

u32 depth_of(World& w, Entity e) {
    HierarchyIndex& idx = index_of(w);
    u32 d = 0;
    for (const Entity* p = idx.parent.find(e); p && d < kMaxDepth; p = idx.parent.find(*p)) ++d;
    return d;
}

const HierarchyStats& hierarchy_stats(World& w) { return index_of(w).stats; }

}  // namespace mce::ecs
