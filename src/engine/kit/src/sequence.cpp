#include "mce/kit/sequence.h"

#include <cstring>

#include "mce/audio/components.h"
#include "mce/ecs/prefab.h"
#include "mce/ecs/transform.h"
#include "mce/log.h"
#include "mce/physics/world2d.h"

namespace mce::kit {

using asset::SequenceStep;
using asset::SequenceStepKind;

namespace {

constexpr u32 kMaxSteps = 4096;

bool waits(const SequenceStep& s) {
    return (s.kind == SequenceStepKind::Wait && s.a > 0.0f) || s.kind == SequenceStepKind::WaitForEvent ||
           s.kind == SequenceStepKind::MoveTo;
}

Result<void> validate(Span<const SequenceStep> steps, bool loops) {
    if (steps.empty()) return Error(Errc::InvalidArgument, "kit.sequence.empty");
    if (steps.size() >= kMaxSteps) return Error(Errc::InvalidArgument, "kit.sequence.too_long");
    bool any_wait = false;
    for (usize i = 0; i < steps.size(); ++i) {
        if (steps[i].kind > SequenceStepKind::End) return Error(Errc::InvalidArgument, "kit.sequence.bad_kind");
        if (steps[i].kind == SequenceStepKind::End && i + 1 != steps.size()) return Error(Errc::InvalidArgument, "kit.sequence.early_end");
        any_wait |= waits(steps[i]);
    }
    if (loops && !any_wait) return Error(Errc::InvalidArgument, "kit.sequence.loop_never_waits");
    return {};
}

SequenceStep make(SequenceStepKind kind) {
    SequenceStep s{};
    s.kind = kind;
    return s;
}

SequenceStep named(SequenceStepKind kind, StringView id) {
    SequenceStep s = make(kind);
    s.id = StringId(id).value;
    return s;
}

void set_pos(SequenceStep& s, math::vec3 p) {
    s.pos[0] = p.x;
    s.pos[1] = p.y;
    s.pos[2] = p.z;
}

math::vec3 step_pos(const SequenceStep& s) { return {s.pos[0], s.pos[1], s.pos[2]}; }

// The entity's position: its physics body when it has one (physics owns the pose in play), else
// its LocalTransform.
math::vec3 position_of(World& w, physics::World2D* phys, Entity e) {
    const LocalTransform* t = w.try_get<LocalTransform>(e);
    math::vec3 p = t ? t->position : math::vec3{};
    physics::BodyState b;
    if (phys && phys->body_state(e, b)) {
        p.x = b.position.x;
        p.y = b.position.y;
    }
    return p;
}

// One MoveTo tick; true once the entity is within the tolerance of `target`.
bool move_toward(World& w, physics::World2D* phys, Entity e, math::vec3 target, const SequenceStep& s, f32 dt) {
    const math::vec3 from = position_of(w, phys, e);
    const math::vec3 delta = target - from;
    const f32 dist = math::length(delta);
    const bool body = phys && phys->has_body(e);
    if (dist <= s.b) {
        if (body) phys->set_linear_velocity(e, {0.0f, 0.0f});
        return true;
    }
    // Within a thousandth of a tick of travel counts as arriving (float sums of speed × dt).
    const f32 travel = s.a * dt;
    const f32 reach = travel * 1.001f;
    if (body) {
        // Velocity for the next physics step: full speed, or exactly the rest of the way.
        const math::vec3 v = dist <= reach ? delta * (1.0f / dt) : delta * (s.a / dist);
        phys->set_linear_velocity(e, {v.x, v.y});
        return false;
    }
    if (LocalTransform* t = w.try_get_mut<LocalTransform>(e)) {
        if (dist <= reach) {
            t->position = target;
            return true;
        }
        t->position = from + delta * (travel / dist);
        return false;
    }
    return true;  // nothing to move
}

void spawn_prefab(Frame& f, asset::AssetSystem* assets, asset::Guid guid, math::vec3 at) {
    if (!assets) {
        MCE_LOG_WARN("kit: sequence spawn needs mounted assets");
        return;
    }
    const asset::AssetKey key = assets->find(guid);
    Result<asset::AssetHandle<asset::Prefab>> h =
        key == asset::kInvalidAssetKey ? Result<asset::AssetHandle<asset::Prefab>>(Error(Errc::NotFound, "kit.sequence.spawn"))
                                       : assets->load_sync<asset::Prefab>(key);
    if (!h) {
        MCE_LOG_WARN("kit: sequence spawn: prefab {} cannot be loaded", asset::to_string(guid).view());
        return;
    }
    const ecs::PrefabOverride o = ecs::PrefabOverride::of("LocalTransform.position", at);
    ecs::SpawnParams p;
    p.overrides = Span<const ecs::PrefabOverride>(&o, 1);
    if (Result<u32> r = ecs::instantiate(f.world(), assets->resolve(h.value()), p); !r) {
        FixedString<512> why;
        format_error(r.error(), why);
        MCE_LOG_WARN("kit: sequence spawn of prefab {} failed: {}", asset::to_string(guid).view(), why.view());
    }
    assets->release(asset::AnyHandle(h.value()));
}

}  // namespace

// ------------------------------------------------------------------ registry

Result<void> SequenceRegistry::define(StringView name, Span<const SequenceStep> steps, bool loops) {
    if (name.empty()) return Error(Errc::InvalidArgument, "kit.sequence.no_name");
    Array<SequenceStep> list;
    for (const SequenceStep& s : steps) list.push_back(s);
    if (list.empty() || list.back().kind != SequenceStepKind::End) list.push_back(make(SequenceStepKind::End));
    if (Result<void> r = validate(list.span(), loops); !r) return r;
    const StringId id(name);
    for (Def& d : defs_)
        if (d.guid == asset::Guid{} && d.name == id) {
            d.steps = static_cast<Array<SequenceStep>&&>(list);
            d.loops = loops;
            return {};
        }
    Def d;
    d.name = id;
    d.loops = loops;
    d.steps = static_cast<Array<SequenceStep>&&>(list);
    defs_.push_back(static_cast<Def&&>(d));
    return {};
}

void SequenceRegistry::register_call(StringView name, SequenceCallFn fn) {
    const StringId id(name);
    for (Call& c : calls_)
        if (c.id == id) {
            c.fn = fn;
            return;
        }
    calls_.push_back(Call{id, fn});
}

SequenceCallFn SequenceRegistry::call(StringId id) const {
    for (const Call& c : calls_)
        if (c.id == id) return c.fn;
    return nullptr;
}

Result<void> SequenceRegistry::validate_calls() const {
    for (const Def& d : defs_)
        for (const SequenceStep& s : d.steps)
            if (s.kind == SequenceStepKind::Call && !call(StringId(s.id))) return Error(Errc::NotFound, "kit.sequence.call_unresolved");
    return {};
}

const SequenceRegistry::Def* SequenceRegistry::resolve(const SequencePlayer& p) {
    const asset::Guid guid = p.sequence.guid;
    if (guid == asset::Guid{}) {
        const StringId id(p.name.view());
        for (const Def& d : defs_)
            if (d.guid == asset::Guid{} && d.name == id) return &d;
        return nullptr;
    }
    Def* cached = nullptr;
    for (Def& d : defs_)
        if (d.guid == guid) cached = &d;
    if (cached && (!assets_ || cached->key == asset::kInvalidAssetKey || assets_->generation(cached->key) == cached->generation))
        return cached;
    if (!assets_) return nullptr;
    const asset::AssetKey key = assets_->find(guid);
    Result<asset::AssetHandle<asset::Sequence>> h =
        key == asset::kInvalidAssetKey ? Result<asset::AssetHandle<asset::Sequence>>(Error(Errc::NotFound, "kit.sequence"))
                                       : assets_->load_sync<asset::Sequence>(key);
    Result<asset::SequenceView> view = h ? asset::SequenceView::read(assets_->resolve(h.value()))
                                         : Result<asset::SequenceView>(h.error());
    if (view && validate(view.value().steps, view.value().loops())) {
        Def d;
        d.name = StringId(view.value().name());
        d.guid = guid;
        d.key = key;
        d.generation = h.value().generation;
        d.loops = view.value().loops();
        for (const SequenceStep& s : view.value().steps) d.steps.push_back(s);
        if (cached) *cached = static_cast<Def&&>(d);
        else defs_.push_back(static_cast<Def&&>(d));
        cached = cached ? cached : &defs_.back();
    } else {
        bool warned = false;
        for (const asset::Guid& g : warned_) warned |= g == guid;
        if (!warned) {
            MCE_LOG_WARN("kit: sequence {} cannot be loaded; its players stay idle", asset::to_string(guid).view());
            warned_.push_back(guid);
        }
        cached = nullptr;
    }
    if (h) assets_->release(asset::AnyHandle(h.value()));
    return cached;
}

SequenceStep SequenceRegistry::wait(f32 seconds) {
    SequenceStep s = make(SequenceStepKind::Wait);
    s.a = seconds;
    return s;
}
SequenceStep SequenceRegistry::wait_event(StringView id) { return named(SequenceStepKind::WaitForEvent, id); }
SequenceStep SequenceRegistry::move_to(math::vec3 target, f32 speed, bool relative, f32 tolerance) {
    SequenceStep s = make(SequenceStepKind::MoveTo);
    set_pos(s, target);
    s.a = speed;
    s.b = tolerance;
    if (relative) s.flags |= asset::SequenceStepFlags::Relative;
    return s;
}
SequenceStep SequenceRegistry::anim(StringView clip) { return named(SequenceStepKind::PlayAnim, clip); }
SequenceStep SequenceRegistry::sound(asset::Guid sfx, f32 gain_db) {
    SequenceStep s = make(SequenceStepKind::PlaySound);
    s.guid[0] = sfx.hi;
    s.guid[1] = sfx.lo;
    s.a = gain_db;
    return s;
}
SequenceStep SequenceRegistry::stop_sound() { return make(SequenceStepKind::StopSound); }
SequenceStep SequenceRegistry::emit(StringView id) { return named(SequenceStepKind::Emit, id); }
SequenceStep SequenceRegistry::call_step(StringView id) { return named(SequenceStepKind::Call, id); }
SequenceStep SequenceRegistry::spawn(asset::Guid prefab, math::vec3 at, bool relative) {
    SequenceStep s = make(SequenceStepKind::Spawn);
    s.guid[0] = prefab.hi;
    s.guid[1] = prefab.lo;
    set_pos(s, at);
    if (relative) s.flags |= asset::SequenceStepFlags::Relative;
    return s;
}
SequenceStep SequenceRegistry::destroy() { return make(SequenceStepKind::Destroy); }

// ------------------------------------------------------------------ runner

void sequence_runner(Frame& f) {
    SequenceRegistry* reg = f.try_resource<SequenceRegistry>();
    if (!reg) return;
    World& w = f.world();
    physics::World2D* phys = f.try_resource<physics::World2D>();
    audio::AudioRef* audio_ref = f.try_resource<audio::AudioRef>();
    asset::AssetSystem* assets = reg->assets();

    // Snapshot: steps may spawn and destroy (Commands), and calls may run queries of their own.
    Array<Entity> players;
    f.query<const SequencePlayer>().each([&](Entity e, const SequencePlayer& p) {
        if (p.active) players.push_back(e);
    });
    if (players.empty()) return;
    const Span<const SeqSignal> signals = f.events().read<SeqSignal>();

    for (Entity e : players) {
        const SequencePlayer* cp = w.try_get<SequencePlayer>(e);
        if (!cp) continue;
        const SequenceRegistry::Def* def = reg->resolve(*cp);
        if (!def) continue;
        SequencePlayer* p = w.try_get_mut<SequencePlayer>(e);
        bool waited = false;  // a waiting step already used this tick
        u32 budget = SequenceRegistry::kStepBudget;
        auto next = [&] {
            ++p->step;
            p->elapsed = 0.0f;
            p->anchored = false;
        };
        while (p->active) {
            if (budget-- == 0) {
                MCE_LOG_ERROR("kit: sequence on entity {} ran {} steps in one tick; stopped", e.index, SequenceRegistry::kStepBudget);
                p->active = false;
                break;
            }
            if (p->step >= def->steps.size()) p->step = static_cast<u16>(def->steps.size() - 1);
            const SequenceStep& s = def->steps[p->step];
            if (waits(s) && waited) break;
            bool stop = false;
            switch (s.kind) {
            case SequenceStepKind::Wait:
                if (s.a > 0.0f) {
                    waited = true;
                    p->elapsed += f.dt;
                    // Within a thousandth of a tick counts as arrived: the sum of N ticks of dt
                    // must complete an N-tick wait whatever the float rounding.
                    if (p->elapsed + f.dt * 1e-3f < s.a) stop = true;
                }
                if (!stop) next();
                break;
            case SequenceStepKind::WaitForEvent: {
                waited = true;
                bool seen = false;
                for (const SeqSignal& sig : signals) seen |= sig.id.value == s.id;
                if (seen) next();
                else stop = true;
                break;
            }
            case SequenceStepKind::MoveTo: {
                waited = true;
                if (!p->anchored) {
                    p->anchor = position_of(w, phys, e);
                    p->anchored = true;
                }
                const math::vec3 target = (s.flags & asset::SequenceStepFlags::Relative) ? p->anchor + step_pos(s) : step_pos(s);
                if (move_toward(w, phys, e, target, s, f.dt)) next();
                else stop = true;
                break;
            }
            case SequenceStepKind::PlayAnim:
                f.events().push(SeqAnimRequest{e, StringId(s.id)});
                next();
                break;
            case SequenceStepKind::PlaySound:
                if (audio_ref && w.event_queue<audio::PlaySfx>()) {
                    audio::PlaySfx sfx;
                    sfx.clip = s.asset();
                    sfx.gain_db = s.a;
                    f.events().push(sfx);
                } else {
                    MCE_LOG_WARN("kit: sequence sound step without audio installed");
                }
                next();
                break;
            case SequenceStepKind::StopSound:
                if (w.try_get<audio::AudioSource>(e)) f.commands().remove<audio::AudioSource>(e);
                next();
                break;
            case SequenceStepKind::Emit:
                f.events().push(SeqSignal{StringId(s.id), e});
                next();
                break;
            case SequenceStepKind::Call:
                if (SequenceCallFn fn = reg->call(StringId(s.id))) {
                    next();  // before the call: the hook may read or replace the player
                    fn(f, e);
                    p = w.try_get_mut<SequencePlayer>(e);
                    if (!p) stop = true;
                } else {
                    MCE_LOG_ERROR("kit: sequence on entity {} calls an unregistered function ({:x}); stopped", e.index, s.id);
                    p->active = false;
                }
                break;
            case SequenceStepKind::Spawn: {
                const math::vec3 at =
                    (s.flags & asset::SequenceStepFlags::Relative) ? position_of(w, phys, e) + step_pos(s) : step_pos(s);
                spawn_prefab(f, assets, s.asset(), at);
                next();
                break;
            }
            case SequenceStepKind::Destroy:
                p->active = false;
                f.commands().destroy(e);
                break;
            case SequenceStepKind::End:
                if (def->loops) {
                    p->step = 0;
                    p->elapsed = 0.0f;
                    p->anchored = false;
                } else {
                    p->active = false;
                }
                break;
            }
            if (stop) break;
        }
    }
}

void install_sequences(World& world, asset::AssetSystem* assets) {
    if (SequenceRegistry* r = world.try_resource<SequenceRegistry>()) {
        r->set_assets(assets);
        return;
    }
    world.register_components<SequencePlayer>();
    world.register_event<SeqSignal>(Phase::FixedUpdate_Post);
    world.register_event<SeqAnimRequest>(Phase::FixedUpdate_Post);
    world.set_resource(SequenceRegistry(assets));
    world.add_system(Phase::FixedUpdate_Post, "sequence_runner", &sequence_runner);
}

}  // namespace mce::kit
