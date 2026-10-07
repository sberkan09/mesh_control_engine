#include "mce/physics/module.h"

#include <algorithm>
#include <cstring>

#include "mce/ecs/transform.h"
#include "mce/log.h"

namespace mce::physics {

namespace {

// Last component values pushed into the world, per entity index (change detection until the ECS
// grows change ticks at v0.5).
struct SyncEntry {
    Entity entity;
    RigidBody2D body;
    Collider2D collider;
    CharacterController2D controller;
    bool has_controller = false;
    bool failed = false;  // malformed shape: no body until the components change
};

struct SyncState {
    Array<SyncEntry> entries;
};

struct ImportItem {
    Entity entity;
    const RigidBody2D* body;
    const Collider2D* collider;
};

template <class T>
bool same(const T& a, const T& b) {
    return std::memcmp(&a, &b, sizeof(T)) == 0;
}

void on_body_removed(World& w, Entity e) {
    if (World2D* pw = w.try_resource<World2D>()) pw->destroy_body(e);
    if (SyncState* s = w.try_resource<SyncState>(); s && e.index < s->entries.size()) s->entries[e.index] = {};
}

void import(Frame& f, World2D& pw, SyncState& sync) {
    World& w = f.world();
    SmallVector<ImportItem, 64> items;
    f.query<const RigidBody2D&, const Collider2D&>().each([&](Entity e, const RigidBody2D& rb, const Collider2D& c) {
        items.push_back({e, &rb, &c});
    });
    std::sort(items.begin(), items.end(),
              [](const ImportItem& a, const ImportItem& b) { return a.entity.index < b.entity.index; });

    for (const ImportItem& it : items) {
        const Entity e = it.entity;
        const CharacterController2D* ctrl = w.try_get<CharacterController2D>(e);
        if (e.index >= sync.entries.size()) sync.entries.resize(e.index + 1);
        SyncEntry& s = sync.entries[e.index];
        const bool known = s.entity == e;

        if (known && pw.has_body(e) && s.has_controller != (ctrl != nullptr)) pw.destroy_body(e);

        if (!pw.has_body(e)) {
            if (known && s.failed && same(s.body, *it.body) && same(s.collider, *it.collider)) continue;
            BodyDesc d;
            d.body = *it.body;
            d.collider = *it.collider;
            d.controller = ctrl;
            if (const LocalTransform* t = w.try_get<LocalTransform>(e)) {
                d.position = t->position.xy();
                d.angle = angle_z(t->rotation);
            }
            s = SyncEntry{};
            s.entity = e;
            s.body = *it.body;
            s.collider = *it.collider;
            s.has_controller = ctrl != nullptr;
            if (ctrl) s.controller = *ctrl;
            s.failed = !pw.create_body(e, d);
            if (s.failed) MCE_LOG_WARN("physics2d: entity {} has a malformed Collider2D; no body created", e.index);
        } else {
            if (!same(s.body, *it.body)) {
                pw.set_body_config(e, *it.body);
                s.body = *it.body;
            }
            if (!same(s.collider, *it.collider)) {
                if (!pw.set_collider(e, *it.collider))
                    MCE_LOG_WARN("physics2d: entity {} Collider2D edit is malformed; keeping the old shape", e.index);
                s.collider = *it.collider;
            }
            if (ctrl && !same(s.controller, *ctrl)) {
                pw.set_controller(e, *ctrl);
                s.controller = *ctrl;
            }
        }
        if (ctrl && !w.has<CharacterGround2D>(e)) f.commands().add<CharacterGround2D>(e);
    }
}

void export_state(Frame& f, const World2D& pw) {
    World& w = f.world();
    pw.each_body([&](const BodyState& b) {
        if (b.type == BodyType::Static || !b.enabled) return;
        if (LocalTransform* t = w.try_get_mut<LocalTransform>(b.entity)) {
            t->position.x = b.position.x;
            t->position.y = b.position.y;
            t->rotation = rotation_z(b.angle);
        }
        if (b.controller) {
            MoveResult2D r;
            if (CharacterGround2D* g = w.try_get_mut<CharacterGround2D>(b.entity); g && pw.controller_state(b.entity, r)) {
                g->flags = r.flags;
                g->ground_normal = r.ground_normal;
                g->platform_velocity = r.platform_velocity;
                g->ground_body = r.ground_body;
            }
        }
    });
}

void publish_events(Frame& f, const World2D& pw) {
    EventAccess ev = f.events();
    for (const TriggerEnter& e : pw.trigger_enters()) ev.push(e);
    for (const TriggerExit& e : pw.trigger_exits()) ev.push(e);
    for (const ContactBegin2D& e : pw.contact_begins()) ev.push(e);
    for (const ContactEnd2D& e : pw.contact_ends()) ev.push(e);
}

}  // namespace

void physics_step(Frame& f) {
    World2D& pw = f.resource<World2D>();
    SyncState& sync = f.resource<SyncState>();
    import(f, pw, sync);
    pw.step(f.dt);
    export_state(f, pw);
    publish_events(f, pw);
}

void install_physics2d(World& world, const PhysicsConfig& config) {
    world.register_components<RigidBody2D, Collider2D, CharacterController2D, CharacterGround2D, LocalTransform>();
    world.register_event<TriggerEnter>(Phase::FixedUpdate_Physics);
    world.register_event<TriggerExit>(Phase::FixedUpdate_Physics);
    world.register_event<ContactBegin2D>(Phase::FixedUpdate_Physics);
    world.register_event<ContactEnd2D>(Phase::FixedUpdate_Physics);
    world.set_hooks<RigidBody2D>(nullptr, &on_body_removed);
    world.set_hooks<Collider2D>(nullptr, &on_body_removed);
    world.set_resource(World2D(config));
    world.set_resource(SyncState{});
    world.add_system(Phase::FixedUpdate_Physics, "physics_step", &physics_step);
}

}  // namespace mce::physics
