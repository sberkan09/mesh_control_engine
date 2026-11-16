#include "mce/kit/missions.h"

#include "kit3d_internal.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/input/actions.h"
#include "mce/kit/bodies3d.h"
#include "mce/kit/combat3d.h"
#include "mce/kit/wanted3d.h"
#include "mce/scene/entity_names.h"
#include "mce/string_id.h"

namespace mce::kit {
static_assert(sizeof(MissionMarker) == 16);
static_assert(sizeof(MissionRetry) == 8);
static_assert(sizeof(MissionWantedFail) == 4);
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

// M-MISSIONPAY: a completed mission pays its actor the MissionReward3D on the mission entity.
void pay_reward(World& w, Entity mission, const Mission& m) {
    const MissionReward3D* r = w.try_get<MissionReward3D>(mission);
    if (r && r->cash > 0) give_cash(w, m.actor, r->cash);
}

// M-MISSIONFEE: a failed mission takes the MissionReward3D's fail_fee from its actor.
void charge_failure(World& w, Entity mission, const Mission& m) {
    const MissionReward3D* r = w.try_get<MissionReward3D>(mission);
    if (r && r->fail_fee > 0) take_cash(w, m.actor, r->fail_fee);
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
            if (o.ticks == 1 && !any && !o.target.is_null() && dead(w, o.target)) return true;  // dead or gone before the step began: nothing left to kill
            for (const Died3D& d : in.died) {
                if (o.team != 0) {
                    const Health3D* h = w.try_get<Health3D>(d.entity);
                    if (!h || h->team != o.team) continue;
                }
                if (o.by_actor && d.killer != m.actor) continue;
                if (any || (!o.target.is_null() && d.entity == o.target)) ++o.progress;
            }
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

    f.query<MissionMarker, LocalTransform>().each([&](Entity me, MissionMarker& mk, LocalTransform&) {
        if (!mk.enabled || mk.mission.is_null() || !w.alive(mk.mission)) return;
        Mission* mm = w.try_get_mut<Mission>(mk.mission);
        if (!mm || mm->state != MissionState::Idle) return;
        math::vec3 actor_pos, marker_pos;
        if (pose_of(w, mm->actor, actor_pos) && pose_of(w, me, marker_pos) && math::length(actor_pos - marker_pos) <= mk.radius) mm->start = 1;
    });

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
        if (m.state == MissionState::Failed) {
            MissionRetry* rt = w.try_get_mut<MissionRetry>(e);
            if (!rt) return;
            if (rt->left <= 0.0f) rt->left = rt->delay;
            rt->left -= f.dt;
            if (rt->left > 0.0f) return;
            rt->left = 0.0f;
            Entity at = m.objective;
            for (u32 n = 0; n < 64 && !at.is_null() && w.alive(at); ++n) {  // the chain is short; the cap guards a loop
                Objective* o = w.try_get_mut<Objective>(at);
                if (!o) break;
                o->state = ObjectiveState::Pending;
                o->progress = 0;
                o->ticks = 0;
                at = o->next;
            }
            m.state = MissionState::Idle;
            m.start = 0;
            return;
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
            charge_failure(w, e, m);
            return;
        }
        const MissionWantedFail* wf = w.try_get<MissionWantedFail>(e);
        const Wanted3D* wd = wf && !m.actor.is_null() && w.alive(m.actor) ? w.try_get<Wanted3D>(m.actor) : nullptr;
        if (wf && wd && wd->level >= wf->level) {
            m.state = MissionState::Failed;
            f.events().push(MissionFailed{e, 3u});
            charge_failure(w, e, m);
            return;
        }
        Objective* o = m.current.is_null() || !w.alive(m.current) ? nullptr : w.try_get_mut<Objective>(m.current);
        if (!o) {  // the objective went: the mission has nothing left to wait for
            m.state = MissionState::Failed;
            f.events().push(MissionFailed{e, 2u});
            charge_failure(w, e, m);
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
            pay_reward(w, e, m);
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
    world.register_components<Mission, Objective, MissionMarker, MissionWantedFail, MissionRetry>();
    world.register_event<MissionStarted>(Phase::FixedUpdate_Pre);
    world.register_event<ObjectiveDone>(Phase::FixedUpdate_Pre);
    world.register_event<MissionCompleted>(Phase::FixedUpdate_Pre);
    world.register_event<MissionFailed>(Phase::FixedUpdate_Pre);
    world.register_event<MissionSignal>(Phase::FixedUpdate_Pre);
    world.add_system(Phase::FixedUpdate_Pre, "kit_missions", &kit_missions, "kit3d_health");
}

}  // namespace mce::kit
