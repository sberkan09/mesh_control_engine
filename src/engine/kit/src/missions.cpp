#include "mce/kit/missions.h"

#include "kit3d_internal.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/input/actions.h"
#include "mce/kit/bodies3d.h"
#include "mce/kit/combat3d.h"
#include "mce/scene/entity_names.h"
#include "mce/string_id.h"

namespace mce::kit {
namespace {

struct MissionsInstalled {};

constexpr StringId kInteract{"Interact"};

bool pose_of(World& w, Entity e, math::vec3& pos) {
    if (e.is_null() || !w.alive(e)) return false;
    const LocalTransform* t = w.try_get<LocalTransform>(e);
    if (!t) return false;
    math::quat rot;
    math::vec3 scale;
    world_pose(w, e, *t, pos, rot, scale);
    return true;
}

// Resolves the target by name while it is empty.
void resolve_target(World& w, Objective& o) {
    if (!o.target.is_null() || o.tag.empty()) return;
    o.target = scene::find_entity(w, o.tag.view());
}

void activate(World& w, Entity obj) {
    Objective* o = obj.is_null() || !w.alive(obj) ? nullptr : w.try_get_mut<Objective>(obj);
    if (!o) return;
    o->state = ObjectiveState::Active;
    o->progress = 0;
    o->ticks = 0;
    resolve_target(w, *o);
}

bool dead(World& w, Entity e) {
    if (e.is_null()) return false;
    if (!w.alive(e)) return true;
    const Health3D* h = w.try_get<Health3D>(e);
    return h && h->dead;
}

struct Inputs {
    Span<const Died3D> died;
    Span<const TriggerEnter3D> enters;
    Span<const MissionSignal> signals;
    bool interact = false;
};

// Whether the active objective is done this tick.
bool evaluate(World& w, const Mission& m, Objective& o, const Inputs& in, f32 dt) {
    ++o.ticks;
    resolve_target(w, o);
    switch (o.kind) {
        case ObjectiveKind::ReachZone:
        case ObjectiveKind::Interact: {
            if (o.kind == ObjectiveKind::Interact && !in.interact) return false;
            math::vec3 a, b;
            if (!pose_of(w, m.actor, a) || !pose_of(w, o.target, b)) return false;
            return math::length(a - b) <= o.radius;
        }
        case ObjectiveKind::Kill: {
            const bool any = o.target.is_null() && o.tag.empty();
            for (const Died3D& d : in.died)
                if (any || (!o.target.is_null() && d.entity == o.target)) ++o.progress;
            return o.progress >= o.count;
        }
        case ObjectiveKind::Collect: {
            if (o.target.is_null()) return false;
            for (const TriggerEnter3D& e : in.enters)
                if (e.trigger == o.target && (m.actor.is_null() || e.other == m.actor)) ++o.progress;
            return o.progress >= o.count;
        }
        case ObjectiveKind::Signal: {
            for (const MissionSignal& s : in.signals)
                if (s.name.view() == o.signal.view()) o.progress += s.amount;
            return o.progress >= o.count;
        }
        case ObjectiveKind::Timer:
            return static_cast<f64>(o.ticks) * dt >= static_cast<f64>(o.duration) - 1e-6;
    }
    return false;
}

void kit_missions(Frame& f) {
    World& w = f.world();
    const input::InputState* is = w.try_resource<input::InputState>();
    Inputs in;
    in.died = f.events().read<Died3D>();
    in.enters = f.events().read<TriggerEnter3D>();
    in.signals = f.events().read<MissionSignal>();
    in.interact = is && is->has_action(kInteract) && is->pressed(kInteract);

    f.query<Mission>().each([&](Entity e, Mission& m) {
        if (m.state == MissionState::Idle) {
            if (!m.start && !m.start_on_load) return;
            m.start = 0;
            m.state = MissionState::Active;
            m.ticks = 0;
            m.elapsed = 0.0f;
            m.done = 0;
            m.current = m.objective;
            f.events().push(MissionStarted{e});
            if (m.current.is_null() || !w.alive(m.current) || !w.try_get<Objective>(m.current)) {
                m.state = MissionState::Complete;
                f.events().push(MissionCompleted{e});
                return;
            }
            activate(w, m.current);
            return;  // the first objective is looked at next tick
        }
        if (m.state != MissionState::Active) return;
        ++m.ticks;
        m.elapsed = static_cast<f32>(m.ticks) * f.dt;
        bool fail_death = false;
        if (!m.fail_on_death.is_null()) {
            fail_death = dead(w, m.fail_on_death);
            for (const Died3D& d : in.died)
                if (d.entity == m.fail_on_death) fail_death = true;
        }
        if (fail_death ||
            (m.time_limit > 0.0f && static_cast<f64>(m.ticks) * f.dt >= static_cast<f64>(m.time_limit) - 1e-6)) {
            m.state = MissionState::Failed;
            f.events().push(MissionFailed{e, fail_death ? 1u : 0u});
            return;
        }
        Objective* o = m.current.is_null() || !w.alive(m.current) ? nullptr : w.try_get_mut<Objective>(m.current);
        if (!o) {  // the objective went: the mission has nothing left to wait for
            m.state = MissionState::Failed;
            f.events().push(MissionFailed{e, 2u});
            return;
        }
        if (!evaluate(w, m, *o, in, f.dt)) return;
        o->state = ObjectiveState::Done;
        f.events().push(ObjectiveDone{e, m.current, m.done});
        ++m.done;
        const Entity next = o->next;
        if (next.is_null() || !w.alive(next) || !w.try_get<Objective>(next)) {
            m.state = MissionState::Complete;
            f.events().push(MissionCompleted{e});
            return;
        }
        m.current = next;
        activate(w, next);
    });
}

}  // namespace

void install_missions(World& world, asset::AssetSystem* assets) {
    install_combat3d(world, assets);
    if (world.try_resource<MissionsInstalled>()) return;
    world.set_resource(MissionsInstalled{});
    world.register_components<Mission, Objective>();
    world.register_event<MissionStarted>(Phase::FixedUpdate_Pre);
    world.register_event<ObjectiveDone>(Phase::FixedUpdate_Pre);
    world.register_event<MissionCompleted>(Phase::FixedUpdate_Pre);
    world.register_event<MissionFailed>(Phase::FixedUpdate_Pre);
    world.register_event<MissionSignal>(Phase::FixedUpdate_Pre);
    world.add_system(Phase::FixedUpdate_Pre, "kit_missions", &kit_missions, "kit3d_health");
}

}  // namespace mce::kit
