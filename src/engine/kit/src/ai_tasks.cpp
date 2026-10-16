// The stock behavior tree tasks (M-AI 2; the list is in mce/kit/ai.h). Each reaches the kit the
// way a game would: the agents' own fields, the Animator and AnimatedSprite clips, a PlaySfx
// event, a prefab instantiation and the signal events.

#include <cmath>

#include "kit3d_internal.h"
#include "mce/audio/components.h"
#include "mce/ecs/prefab.h"
#include "mce/ecs/transform.h"
#include "mce/kit/ai.h"
#include "mce/kit/components.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/nav3d.h"
#include "mce/kit/pathfinding.h"
#include "mce/kit/perception.h"
#include "mce/kit/sequence.h"
#include "mce/log.h"

namespace mce::kit {

namespace {

math::vec3 position_of(World& w, Entity e) {
    const LocalTransform* t = w.try_get<LocalTransform>(e);
    if (!t) return {};
    math::vec3 pos, scale;
    math::quat rot;
    world_pose(w, e, *t, pos, rot, scale);
    return pos;
}

bool flat(World& w, Entity e) { return w.try_get<PathAgent>(e) || w.try_get<AnimatedSprite>(e) || w.try_get<Sprite>(e); }

// Stops what move_to started.
void stop_moving(World& w, Entity e) {
    if (NavAgent3D* a = w.try_get_mut<NavAgent3D>(e)) {
        a->move_to = 0;
        a->follow = Entity{};
    } else if (PathAgent* p = w.try_get_mut<PathAgent>(e)) {
        p->active = false;
    }
}

// memory: 0 on the tick move_to starts (the agent plans after the brain, so its state is last
// tick's), 1 after.
BtStatus move_to(BtTaskCall& c) {
    World& w = c.frame.world();
    if (c.halting) {
        stop_moving(w, c.entity);
        return BtStatus::Failure;
    }
    math::vec3 target;
    Entity chased;
    if (!bt_key_position(c, target, &chased)) {
        stop_moving(w, c.entity);
        return BtStatus::Failure;
    }
    const bool first = c.memory == 0.0f;
    c.memory = 1.0f;
    const f32 radius = c.value();
    BtStatus r = BtStatus::Running;
    if (NavAgent3D* a = w.try_get_mut<NavAgent3D>(c.entity)) {
        // An entity is chased through the agent's own follow (it re-plans as the entity moves).
        a->follow = chased;
        if (c.entering || chased.is_null()) a->target = target;
        a->move_to = 1;
        if (first) return BtStatus::Running;
        if (radius > 0.0f && math::length(position_of(w, c.entity) - target) <= radius) r = BtStatus::Success;
        else if (a->state == NavAgentState::Arrived) r = BtStatus::Success;
        else if (a->state == NavAgentState::NoPath || a->state == NavAgentState::OffMesh) r = BtStatus::Failure;
    } else if (PathAgent* p = w.try_get_mut<PathAgent>(c.entity)) {
        p->target = {target.x, target.y};
        p->active = true;
        if (first) return BtStatus::Running;
        const math::vec3 at = position_of(w, c.entity);
        if (radius > 0.0f && math::length(math::vec2{at.x - target.x, at.y - target.y}) <= radius) r = BtStatus::Success;
        else if (p->status == PathStatus::Arrived) r = BtStatus::Success;
        else if (p->status == PathStatus::NoPath) r = BtStatus::Failure;
    } else {
        return BtStatus::Failure;
    }
    if (r != BtStatus::Running) stop_moving(w, c.entity);
    return r;
}

BtStatus look_at(BtTaskCall& c) {
    if (c.halting) return BtStatus::Failure;
    World& w = c.frame.world();
    math::vec3 target;
    LocalTransform* t = w.try_get_mut<LocalTransform>(c.entity);
    if (!t || !bt_key_position(c, target)) return BtStatus::Failure;
    const math::vec3 d = target - position_of(w, c.entity);
    if (flat(w, c.entity)) {
        if (d.x * d.x + d.y * d.y > 1e-12f) t->rotation = rotation_z(std::atan2(d.y, d.x));
    } else if (d.x * d.x + d.z * d.z > 1e-12f) {
        t->rotation = math::quat_from_axis_angle({0.0f, 1.0f, 0.0f}, std::atan2(-d.x, -d.z));
    }
    return BtStatus::Success;
}

// memory: 0 on the tick the clip starts (the animators run later in the tick), 1 after.
BtStatus animate(BtTaskCall& c) {
    if (c.halting) return BtStatus::Failure;
    World& w = c.frame.world();
    const StringView clip = c.arg();
    const bool finish = c.value() != 0.0f;
    if (AnimatedSprite* s = w.try_get_mut<AnimatedSprite>(c.entity)) {
        if (c.entering) {
            if (s->clip.view() == clip) s->restart = true;
            s->clip.assign(clip);
            s->playing = true;
        }
        if (!finish) return BtStatus::Success;
        if (c.memory == 0.0f) {
            c.memory = 1.0f;
            return BtStatus::Running;
        }
        return s->finished && s->playing_clip.view() == clip ? BtStatus::Success : BtStatus::Running;
    }
    if (Animator* a = w.try_get_mut<Animator>(c.entity)) {
        if (a->graph.guid != asset::Guid{}) return BtStatus::Failure;  // a graph picks the clips
        if (c.entering) {
            a->clip.assign(clip);
            a->restart = true;
        }
        if (!finish) return BtStatus::Success;
        if (c.memory == 0.0f) {
            c.memory = 1.0f;
            return BtStatus::Running;
        }
        return a->finished ? BtStatus::Success : BtStatus::Running;
    }
    return BtStatus::Failure;
}

BtStatus play_sound(BtTaskCall& c) {
    if (c.halting) return BtStatus::Failure;
    World& w = c.frame.world();
    Result<asset::Guid> guid = asset::parse_guid(c.arg());
    if (!guid || !w.event_queue<audio::PlaySfx>()) return BtStatus::Failure;
    audio::PlaySfx sfx;
    sfx.clip = guid.value();
    sfx.gain_db = c.value();
    c.frame.events().push(sfx);
    return BtStatus::Success;
}

BtStatus spawn(BtTaskCall& c) {
    if (c.halting) return BtStatus::Failure;
    World& w = c.frame.world();
    const BtRegistry* reg = w.try_resource<BtRegistry>();
    asset::AssetSystem* assets = reg ? reg->assets() : nullptr;
    Result<asset::Guid> guid = asset::parse_guid(c.arg());
    if (!assets || !guid) return BtStatus::Failure;
    math::vec3 at;
    if (c.key() == asset::kBtNoKey) at = position_of(w, c.entity);
    else if (!bt_key_position(c, at)) return BtStatus::Failure;
    at = at + c.vector();
    const asset::AssetKey key = assets->find(guid.value());
    if (key == asset::kInvalidAssetKey) return BtStatus::Failure;
    Result<asset::AssetHandle<asset::Prefab>> h = assets->load_sync<asset::Prefab>(key);
    if (!h) return BtStatus::Failure;
    const ecs::PrefabOverride o = ecs::PrefabOverride::of("LocalTransform.position", at);
    ecs::SpawnParams p;
    p.overrides = Span<const ecs::PrefabOverride>(&o, 1);
    const Result<u32> r = ecs::instantiate(w, assets->resolve(h.value()), p);
    assets->release(asset::AnyHandle(h.value()));
    return r ? BtStatus::Success : BtStatus::Failure;
}

BtStatus raise(BtTaskCall& c) {
    if (c.halting) return BtStatus::Failure;
    World& w = c.frame.world();
    const StringId id(c.arg());
    c.frame.events().push(BtSignal{id, c.entity});
    if (w.event_queue<SeqSignal>()) c.frame.events().push(SeqSignal{id, c.entity});
    return BtStatus::Success;
}

BtStatus wait_event(BtTaskCall& c) {
    if (c.halting) return BtStatus::Failure;
    World& w = c.frame.world();
    const StringId id(c.arg());
    for (const BtSignal& s : c.frame.events().read<BtSignal>())
        if (s.id == id) return BtStatus::Success;
    if (w.event_queue<SeqSignal>())
        for (const SeqSignal& s : c.frame.events().read<SeqSignal>())
            if (s.id == id) return BtStatus::Success;
    return BtStatus::Running;
}

// The noise takes the team of the entity's Stimulus (else its Perceiver), so its own side ignores it.
BtStatus make_noise(BtTaskCall& c) {
    if (c.halting) return BtStatus::Failure;
    World& w = c.frame.world();
    Noise n;
    n.position = position_of(w, c.entity);
    n.loudness = c.value() > 0.0f ? c.value() : 10.0f;
    n.source = c.entity;
    if (const Stimulus* s = w.try_get<Stimulus>(c.entity)) n.team = s->team;
    else if (const Perceiver* p = w.try_get<Perceiver>(c.entity)) n.team = p->team;
    c.frame.events().push(n);
    return BtStatus::Success;
}

// find_point (the flags and fields are the importer's: see bt_importer.cpp).
BtStatus find_point(BtTaskCall& c) {
    if (c.halting) return BtStatus::Failure;
    World& w = c.frame.world();
    const asset::BtNode& n = c.node;
    if (n.key2 >= c.tree.keys.size() || !c.blackboard) return BtStatus::Failure;
    const math::vec3 self = position_of(w, c.entity);
    math::vec3 center = self;
    if (n.key != asset::kBtNoKey && !bt_key_position(c, center)) return BtStatus::Failure;
    math::vec3 from;
    Entity from_entity;
    const bool has_from = n.key3 != asset::kBtNoKey && bt_key_position(c, n.key3, from, &from_entity);
    const u32 prefer = n.flags & 3u, need = (n.flags >> 2) & 3u;
    if ((need != 0 || prefer == 2) && !has_from) return BtStatus::Failure;
    const bool plane = flat(w, c.entity);
    const PerceptionSpace space = plane ? PerceptionSpace::Plane2D : PerceptionSpace::Space3D;
    const Nav3D* nav = plane ? nullptr : w.try_resource<Nav3D>();
    const bool on_mesh = nav && nav->mesh.loaded();
    const NavGrid* grid = plane ? w.try_resource<NavGrid>() : nullptr;
    const math::vec3 up = plane ? math::vec3{} : math::vec3{0.0f, 1.0f, 0.0f};  // rays a metre above the ground

    const u32 rings = n.b >= 1.0f ? static_cast<u32>(n.b) : 1u;
    const u32 count = n.c >= 1.0f ? static_cast<u32>(n.c) : 1u;
    bool found = false;
    f32 best = 0.0f;
    math::vec3 pick;
    for (u32 r = 1; r <= rings; ++r) {
        const f32 radius = n.a * static_cast<f32>(r) / static_cast<f32>(rings);
        for (u32 j = 0; j < count; ++j) {
            const f32 angle = 6.2831853f * (static_cast<f32>(j) + 0.5f * static_cast<f32>(r % 2)) / static_cast<f32>(count);
            math::vec3 p = plane ? center + math::vec3{std::cos(angle) * radius, std::sin(angle) * radius, 0.0f}
                                 : center + math::vec3{std::cos(angle) * radius, 0.0f, std::sin(angle) * radius};
            if (on_mesh) {
                const std::optional<math::vec3> on = nav->mesh.nearest_point(p, {0.5f, 2.0f, 0.5f});
                if (!on) continue;
                p = *on;
            } else if (grid && !grid->walkable(grid->cell_at({p.x, p.y}))) {
                continue;
            }
            if (need != 0) {
                const bool clear = line_of_sight(c.frame, space, from + up, p + up, 0xFFFF, c.entity, from_entity);
                if (clear != (need == 1)) continue;
            }
            const f32 score = prefer == 0 ? -math::length(p - self) : prefer == 1 ? math::length(p - (has_from ? from : center))
                                                                                    : -math::length(p - from);
            if (!found || score > best) {
                found = true;
                best = score;
                pick = p;
            }
        }
    }
    if (!found) return BtStatus::Failure;
    const asset::BtKey& store = c.tree.keys[n.key2];
    c.blackboard->vectors[store.slot] = pick;
    c.blackboard->set |= 1u << (asset::kBtNumbers + store.slot);
    return BtStatus::Success;
}

BtStatus log_text(BtTaskCall& c) {
    if (c.halting) return BtStatus::Failure;
    MCE_LOG_INFO("ai: {} (entity {}): {}", c.tree.name.view(), c.entity.index, c.arg());
    return BtStatus::Success;
}

}  // namespace

void register_stock_tasks(BtRegistry& reg) {
    reg.register_task("move_to", &move_to);
    reg.register_task("look_at", &look_at);
    reg.register_task("animate", &animate);
    reg.register_task("play_sound", &play_sound);
    reg.register_task("spawn", &spawn);
    reg.register_task("raise", &raise);
    reg.register_task("wait_event", &wait_event);
    reg.register_task("make_noise", &make_noise);
    reg.register_task("find_point", &find_point);
    reg.register_task("log", &log_text);
}

}  // namespace mce::kit
