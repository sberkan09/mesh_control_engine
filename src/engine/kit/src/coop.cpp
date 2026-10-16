#include "mce/kit/coop.h"

#include <algorithm>
#include <cmath>

#include "mce/asset/asset_system.h"
#include "mce/ecs/prefab.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/input/actions.h"
#include "mce/kit/bodies3d.h"
#include "mce/kit/components.h"
#include "mce/kit/views.h"
#include "mce/log.h"
#include "mce/physics/components.h"
#include "mce/physics/world2d.h"

namespace mce::kit {

namespace {

struct CoopInstalled {};
struct CoopAssets {
    asset::AssetSystem* assets = nullptr;
};

StringId action_id(const FixedString<32>& name, const char* fallback) { return StringId(name.empty() ? StringView(fallback) : name.view()); }

const CoopGame* first_game(World& w) {
    const CoopGame* found = nullptr;
    w.query<const CoopGame&>().each([&](Entity, const CoopGame& g) {
        if (!found && g.enabled) found = &g;
    });
    return found;
}

// The action's state for `slot` this tick, or null when the snapshot does not carry it.
const input::ActionState* slot_action(const input::InputState& in, u32 slot, StringId name) {
    return in.has_action(slot, name) ? &in.action(slot, name) : nullptr;
}

math::Color player_color(u32 slot) {
    static const math::Color kColors[input::kMaxLocalPlayers] = {{0.95f, 0.3f, 0.25f, 1.0f}, {0.3f, 0.55f, 0.95f, 1.0f},
                                                                   {0.35f, 0.85f, 0.4f, 1.0f}, {0.98f, 0.85f, 0.25f, 1.0f}};
    return kColors[slot % input::kMaxLocalPlayers];
}

math::vec3 spawn_point(World& w, u32 slot) {
    struct Point {
        u32 index;
        u32 player;
        math::vec3 at;
    };
    Array<Point> points;
    w.query<const PlayerSpawn&, const LocalTransform&>().each([&](Entity e, const PlayerSpawn& s, const LocalTransform& t) {
        points.push_back(Point{e.index, s.player, t.position});
    });
    std::sort(points.begin(), points.end(), [](const Point& a, const Point& b) { return a.index < b.index; });
    for (const Point& p : points)
        if (p.player == slot + 1) return p.at;
    Array<math::vec3> any;
    for (const Point& p : points)
        if (p.player == 0) any.push_back(p.at);
    if (!any.empty()) return any[slot % any.size()];
    return math::vec3{static_cast<f32>(slot) * 2.0f, 0.0f, 0.0f};
}

void spawn_player(Frame& f, const CoopGame& game, asset::AssetSystem* assets, u32 slot) {
    const math::vec3 at = spawn_point(f.world(), slot);
    if (game.player.guid.is_null() || !assets) {
        if (!game.player.guid.is_null()) MCE_LOG_WARN("kit: CoopGame has a player prefab but no assets are mounted; spawning a bare player");
        LocalTransform t;
        t.position = at;
        LocalPlayer lp;
        lp.player = slot + 1;
        Sprite s;
        if (game.tint_players) s.tint = player_color(slot);
        f.commands().spawn(t, lp, s);
        return;
    }
    const asset::AssetKey key = assets->find(game.player.guid);
    Result<asset::AssetHandle<asset::Prefab>> h =
        key == asset::kInvalidAssetKey ? Result<asset::AssetHandle<asset::Prefab>>(Error(Errc::NotFound, "kit.coop.player"))
                                       : assets->load_sync<asset::Prefab>(key);
    if (!h) {
        MCE_LOG_WARN("kit: CoopGame player prefab {} cannot be loaded; player {} does not join", asset::to_string(game.player.guid).view(), slot + 1);
        return;
    }
    const ecs::PrefabOverride overrides[2] = {ecs::PrefabOverride::of("LocalPlayer.player", slot + 1),
                                              ecs::PrefabOverride::of("LocalTransform.position", at)};
    ecs::SpawnParams p;
    p.overrides = Span<const ecs::PrefabOverride>(overrides, 2);
    Array<Entity> made;
    if (Result<u32> r = ecs::instantiate(f.world(), assets->resolve(h.value()), p, &made); !r) {
        FixedString<512> why;
        format_error(r.error(), why);
        MCE_LOG_WARN("kit: CoopGame player prefab {} failed: {} (it needs a LocalPlayer and a LocalTransform)",
                     asset::to_string(game.player.guid).view(), why.view());
    } else if (game.tint_players && !made.empty()) {
        if (Sprite* s = f.world().try_get_mut<Sprite>(made[0])) s->tint = player_color(slot);
    }
    assets->release(asset::AnyHandle(h.value()));
}

// Joins, leaves and the drive, before the tick's gameplay and the 3D character controllers.
void kit_coop(Frame& f) {
    World& w = f.world();
    const CoopGame* gp = first_game(w);
    const input::InputState* in = f.try_resource<input::InputState>();
    const CoopGame game = gp ? *gp : CoopGame{};
    asset::AssetSystem* assets = nullptr;
    if (const CoopAssets* a = w.try_resource<CoopAssets>()) assets = a->assets;
    const StringId join = action_id(game.join_action, "Join"), leave = action_id(game.leave_action, "Leave");
    const StringId move = action_id(game.move_action, "Move"), jump = action_id(game.jump_action, "Jump");
    const u32 max_players = std::min<u32>(std::max<u32>(game.max_players, 1), input::kMaxLocalPlayers);

    if (gp) {
        // Joined slots now; a slot joins on its button, the first auto_join ones once at the start, and leaves on its.
        u32 joined = coop_joined_mask(w);
        Array<Entity> leaving;
        w.query<const LocalPlayer&>().each([&](Entity e, const LocalPlayer& p) {
            if (!in || p.player < 1 || p.player > max_players) return;
            const input::ActionState* a = slot_action(*in, p.player - 1, leave);
            if (a && a->pressed) leaving.push_back(e);
        });
        for (Entity e : leaving) {
            if (const LocalPlayer* p = w.try_get<LocalPlayer>(e)) joined &= ~(1u << (p->player - 1));
            f.commands().destroy(e);
        }
        bool first_run = false;
        w.query<CoopGame&>().each([&](Entity, CoopGame& g) {
            if (&g == gp && !g.started) first_run = g.started = true;
        });
        for (u32 slot = 0; slot < max_players; ++slot) {
            if (joined >> slot & 1u) continue;
            bool go = first_run && slot < game.auto_join;
            if (!go && in) {
                const input::ActionState* a = slot_action(*in, slot, join);
                go = a && a->pressed;
            }
            if (!go) continue;
            spawn_player(f, game, assets, slot);
            joined |= 1u << slot;
        }
    }

    // Drive.
    if (!in) return;
    physics::World2D* phys2 = f.try_resource<physics::World2D>();
    w.query<const LocalPlayer&, CharacterController3D&>().each([&](Entity, const LocalPlayer& p, CharacterController3D& c) {
        if (!p.drive || c.use_input || p.player < 1 || p.player > input::kMaxLocalPlayers) return;
        const u32 slot = p.player - 1;
        math::vec3 walk{};
        if (const input::ActionState* a = slot_action(*in, slot, move)) walk = math::vec3{a->x, 0.0f, -a->y} * c.speed;
        c.move = walk;
        if (const input::ActionState* a = slot_action(*in, slot, jump)) {
            if (a->pressed) c.jump = true;
            if (a->released) c.release_jump = true;
        }
    });
    if (phys2) {
        w.query<const LocalPlayer&, const physics::RigidBody2D&>().each([&](Entity e, const LocalPlayer& p, const physics::RigidBody2D& rb) {
            if (!p.drive || rb.type == physics::BodyType::Static || p.player < 1 || p.player > input::kMaxLocalPlayers) return;
            if (w.try_get<CharacterController3D>(e)) return;
            math::vec2 v{};
            if (const input::ActionState* a = slot_action(*in, p.player - 1, move)) v = math::vec2{a->x, a->y} * p.speed;
            phys2->set_linear_velocity(e, v);
        });
    }
}

f32 blend(f32 smoothing, f32 dt) { return smoothing <= 0.0f ? 1.0f : 1.0f - std::exp(-smoothing * dt); }

// After the tick's movement: each PlayerCamera follows its player (and is enabled while it is in),
// the shared 2D camera frames everyone.
void kit_coop_cameras(Frame& f) {
    World& w = f.world();
    math::vec3 pos[input::kMaxLocalPlayers] = {};
    u32 present = 0;
    w.query<const LocalPlayer&, const LocalTransform&>().each([&](Entity, const LocalPlayer& p, const LocalTransform& t) {
        if (p.player < 1 || p.player > input::kMaxLocalPlayers || (present >> (p.player - 1) & 1u)) return;
        present |= 1u << (p.player - 1);
        pos[p.player - 1] = t.position;
    });
    Array<Entity> cams;
    w.query<const PlayerCamera&>().each([&](Entity e, const PlayerCamera&) { cams.push_back(e); });
    for (Entity e : cams) {
        const PlayerCamera pc = *w.try_get<PlayerCamera>(e);
        const bool here = pc.player >= 1 && pc.player <= input::kMaxLocalPlayers && (present >> (pc.player - 1) & 1u);
        if (CameraView* v = w.try_get_mut<CameraView>(e)) v->enabled = here ? 1 : 0;
        LocalTransform* t = w.try_get_mut<LocalTransform>(e);
        if (!here || !t) continue;
        const math::vec3 want = pos[pc.player - 1] + pc.offset;
        t->position = pc.smoothing <= 0.0f ? want : t->position + (want - t->position) * blend(pc.smoothing, f.dt);
    }

    if (present == 0) return;
    f32 x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f;
    for (u32 s = 0; s < input::kMaxLocalPlayers; ++s) {
        if (!(present >> s & 1u)) continue;
        x0 = std::min(x0, pos[s].x);
        x1 = std::max(x1, pos[s].x);
        y0 = std::min(y0, pos[s].y);
        y1 = std::max(y1, pos[s].y);
    }
    Array<Entity> shared;
    w.query<const SharedCamera2D&>().each([&](Entity e, const SharedCamera2D&) { shared.push_back(e); });
    for (Entity e : shared) {
        const SharedCamera2D sc = *w.try_get<SharedCamera2D>(e);
        Camera2DComponent* cam = w.try_get_mut<Camera2DComponent>(e);
        LocalTransform* t = w.try_get_mut<LocalTransform>(e);
        if (!cam || !t) continue;
        const f32 k = blend(sc.smoothing, f.dt);
        const f32 cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
        const f32 width = std::max(x1 - x0 + 2.0f * sc.margin, sc.min_width);
        const f32 height = std::max(y1 - y0 + 2.0f * sc.margin, sc.min_height);
        t->position.x += (cx - t->position.x) * k;
        t->position.y += (cy - t->position.y) * k;
        // Both fits set: the whole box shows on any window shape, so the sim never reads the window's.
        cam->fit_width += (width - cam->fit_width) * k;
        cam->fit_height += (height - cam->fit_height) * k;
    }
}

}  // namespace

void install_coop(World& world, asset::AssetSystem* assets) {
    if (CoopAssets* a = world.try_resource<CoopAssets>()) {
        a->assets = assets;
        return;
    }
    world.set_resource(CoopAssets{assets});
    world.register_components<CoopGame, PlayerSpawn, LocalPlayer, PlayerCamera, SharedCamera2D, LocalTransform, CameraView,
                              Camera2DComponent, CharacterController3D>();
    world.add_system(Phase::FixedUpdate_Pre, "kit_coop", &kit_coop);
    world.add_system(Phase::FixedUpdate_Post, "kit_coop_cameras", &kit_coop_cameras);
}

u32 coop_joined_mask(World& world) {
    u32 mask = 0;
    world.query<const LocalPlayer&>().each([&](Entity, const LocalPlayer& p) {
        if (p.player >= 1 && p.player <= input::kMaxLocalPlayers) mask |= 1u << (p.player - 1);
    });
    return mask;
}

Entity coop_player(World& world, u32 slot) {
    Entity found = Entity::null;
    world.query<const LocalPlayer&>().each([&](Entity e, const LocalPlayer& p) {
        if (p.player == slot + 1 && (found.is_null() || e.index < found.index)) found = e;
    });
    return found;
}

u32 coop_join_slots(World& world) {
    const CoopGame* g = first_game(world);
    return g ? std::min<u32>(std::max<u32>(g->max_players, 1), input::kMaxLocalPlayers) : 0;
}

}  // namespace mce::kit
