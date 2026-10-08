#include "platformer.h"

#include "level.h"

#include <cmath>

#include "mce/audio/components.h"
#include "mce/ecs/transform.h"
#include "mce/input/actions.h"
#include "mce/physics/module.h"

namespace platformer {

using namespace mce;
using namespace mce::physics;

namespace {

// ---- tuning (metres, seconds) -------------------------------------------------------------------
constexpr vec2 kPlayerHalf{0.35f, 0.45f};
constexpr vec2 kEnemyHalf{0.4f, 0.4f};
constexpr f32 kRunSpeed = 7.0f;
constexpr f32 kGroundAccel = 70.0f;
constexpr f32 kAirAccel = 40.0f;
constexpr f32 kGravity = 34.0f;
constexpr f32 kLowJumpGravityScale = 2.5f;  // jump released early: fall back sooner (variable height)
constexpr f32 kJumpSpeed = 13.0f;
constexpr f32 kMaxFall = 20.0f;
constexpr f32 kStompBounce = 10.0f;
constexpr u32 kCoyoteTicks = 6;
constexpr u32 kJumpBufferTicks = 6;
constexpr u32 kInvulnTicks = 60;
constexpr f32 kKillY = -3.0f;

constexpr f32 kPatrolSpeed = 1.8f;
constexpr f32 kChaseSpeed = 3.6f;
constexpr f32 kSightRange = 7.0f;
constexpr f32 kLoseRange = 10.0f;
constexpr u32 kChaseMemoryTicks = 90;

constexpr u16 bit(Layer l) { return static_cast<u16>(1u << l); }

LocalTransform at(vec2 p) {
    LocalTransform t;
    t.position = {p.x, p.y, 0.0f};
    return t;
}

RigidBody2D body(BodyType type, Layer layer, u16 mask) {
    RigidBody2D rb;
    rb.type = type;
    rb.fixed_rotation = true;
    rb.layer = layer;
    rb.mask = mask;
    rb.gravity_scale = 0.0f;
    return rb;
}

Collider2D collider(const ColliderShape& s, bool sensor = false, bool one_way = false) {
    Collider2D c;
    c.shape = s;
    c.density = 0.0f;
    c.is_sensor = sensor;
    c.one_way = one_way;
    return c;
}

Look look(Sprite s, vec2 size, i32 order = 0) {
    Look l;
    l.sprite = s;
    l.size = size;
    l.order = order;
    return l;
}

vec2 position_of(const World2D& pw, Entity e, const LocalTransform& fallback) {
    BodyState s;
    if (pw.body_state(e, s)) return s.position;
    return {fallback.position.x, fallback.position.y};
}

void play(Frame& f, audio::ClipHandle clip, f32 gain_db = 0.0f) {
    if (clip.is_null()) return;
    audio::PlaySfx ev;
    ev.clip_handle = clip;
    ev.gain_db = gain_db;
    f.events().push(ev);
}

vec2 mover_target(const Mover& m, f32 t) {
    const f32 s = std::sin(math::kTau * t / m.period + m.phase);
    return m.origin + m.extent * s;
}

void spawn_level(World& w, const Level& lv, GameState& gs) {
    const u16 world_mask = 0xFFFF;
    for (i32 y = 0; y < static_cast<i32>(lv.height); ++y) {
        // Solid ground and one-way planks: one static box per horizontal run.
        for (i32 x = 0; x < static_cast<i32>(lv.width);) {
            const char c = lv.at(x, y);
            if (c != '#' && c != '=') {
                ++x;
                continue;
            }
            i32 end = x;
            while (lv.at(end + 1, y) == c) ++end;
            const f32 len = static_cast<f32>(end - x + 1);
            const f32 cx = static_cast<f32>(x) + 0.5f * len;
            if (c == '#') {
                w.spawn_with(body(BodyType::Static, Layer_World, world_mask),
                             collider(ColliderShape::box({0.5f * len, 0.5f})), at({cx, static_cast<f32>(y) + 0.5f}));
            } else {
                w.spawn_with(body(BodyType::Static, Layer_World, world_mask),
                             collider(ColliderShape::box({0.5f * len, 0.1f}), false, true),
                             at({cx, static_cast<f32>(y) + 0.9f}));
            }
            x = end + 1;
        }
    }

    for (i32 y = 0; y < static_cast<i32>(lv.height); ++y) {
        for (i32 x = 0; x < static_cast<i32>(lv.width); ++x) {
            const vec2 p = Level::center(x, y);
            switch (lv.at(x, y)) {
                case 'P': {
                    Player pl;
                    pl.spawn = {p.x, static_cast<f32>(y) + kPlayerHalf.y + 0.01f};
                    gs.player = w.spawn_with(body(BodyType::Kinematic, Layer_Player, bit(Layer_World) | bit(Layer_Sensor)),
                                             collider(ColliderShape::box(kPlayerHalf)), CharacterController2D{}, pl,
                                             PrevPos{pl.spawn}, look(Sprite::Player, {1, 1}, 10), at(pl.spawn));
                    break;
                }
                case 'E': {
                    Enemy en;
                    en.home = {p.x, static_cast<f32>(y) + kEnemyHalf.y + 0.01f};
                    en.dir = (x % 2) ? 1.0f : -1.0f;
                    w.spawn_with(body(BodyType::Kinematic, Layer_Enemy, bit(Layer_World)),
                                 collider(ColliderShape::box(kEnemyHalf)), CharacterController2D{}, en, PrevPos{en.home},
                                 look(Sprite::Enemy, {1, 1}, 5), at(en.home));
                    break;
                }
                case 'o': {
                    w.spawn_with(body(BodyType::Static, Layer_Sensor, bit(Layer_Player)),
                                 collider(ColliderShape::circle(0.3f), true), Pickup{}, PrevPos{p},
                                 look(Sprite::Coin, {0.75f, 0.75f}, 2), at(p));
                    ++gs.coins_total;
                    break;
                }
                case '^': {
                    const vec2 c{p.x, static_cast<f32>(y) + 0.3f};
                    w.spawn_with(body(BodyType::Static, Layer_Sensor, bit(Layer_Player)),
                                 collider(ColliderShape::box({0.4f, 0.2f}), true), Hazard{}, PrevPos{c},
                                 look(Sprite::Spike, {1, 0.6f}, 1), at(c));
                    break;
                }
                case 'F': {
                    const vec2 c{p.x, static_cast<f32>(y) + 1.0f};
                    w.spawn_with(body(BodyType::Static, Layer_Sensor, bit(Layer_Player)),
                                 collider(ColliderShape::box({0.3f, 1.0f}), true), Goal{}, PrevPos{c},
                                 look(Sprite::Flag, {1, 2}, 1), at(c));
                    break;
                }
                case 'M':
                case 'V': {
                    Mover m;
                    m.origin = p;
                    m.extent = lv.at(x, y) == 'M' ? vec2{2.0f, 0.0f} : vec2{0.0f, 3.5f};
                    m.period = lv.at(x, y) == 'M' ? 4.0f : 5.0f;
                    const vec2 start = mover_target(m, 0.0f);
                    w.spawn_with(body(BodyType::Kinematic, Layer_World, world_mask),
                                 collider(ColliderShape::box({1.5f, 0.25f})), m, PrevPos{start},
                                 look(Sprite::Platform, {3, 0.5f}, 3), at(start));
                    break;
                }
                default: break;
            }
        }
    }
}

// ---- systems ----------------------------------------------------------------------------------------

void pf_prev(Frame& f) {
    f.query<PrevPos&, const LocalTransform&>().each(
        [](PrevPos& prev, const LocalTransform& t) { prev.p = {t.position.x, t.position.y}; });
}

void pf_state(Frame& f) {
    GameState& gs = f.resource<GameState>();
    const input::InputState& in = f.resource<input::InputState>();
    if (in.pause_pressed && !gs.won) gs.paused = !gs.paused;
    if (gs.flash_ticks > 0) --gs.flash_ticks;
    if (!gs.paused && !gs.won) ++gs.level_ticks;
}

void pf_movers(Frame& f) {
    const GameState& gs = f.resource<GameState>();
    World2D& pw = f.resource<World2D>();
    const f32 t = static_cast<f32>(gs.level_ticks) * f.dt;
    const bool frozen = gs.paused;
    f.query<const Mover&, const LocalTransform&>().each([&](Entity e, const Mover& m, const LocalTransform& lt) {
        if (frozen) {
            pw.set_linear_velocity(e, {});
            return;
        }
        // Velocity that lands exactly on the path at the end of this step (no drift).
        const vec2 pos = position_of(pw, e, lt);
        pw.set_linear_velocity(e, (mover_target(m, t) - pos) * (1.0f / f.dt));
    });
}

void respawn(Frame& f, GameState& gs, World2D& pw, Entity e, Player& p) {
    pw.teleport(e, p.spawn, 0.0f);
    p.vel = {};
    p.coyote = p.jump_buffer = 0;
    p.invuln = kInvulnTicks;
    ++gs.deaths;
    gs.shake = 0.7f;
    play(f, gs.sounds.hurt);
}

void pf_player(Frame& f) {
    GameState& gs = f.resource<GameState>();
    if (gs.paused) return;
    World2D& pw = f.resource<World2D>();
    const input::InputState& in = f.resource<input::InputState>();
    const f32 dt = f.dt;

    f.query<Player&, const LocalTransform&>().each([&](Entity e, Player& p, const LocalTransform& lt) {
        if (!pw.has_body(e)) return;
        if (p.invuln > 0) --p.invuln;
        const bool control = !gs.won;
        const f32 mx = control ? math::clamp(in.move.x, -1.0f, 1.0f) : 0.0f;

        // Grounded as of the previous move; coyote time keeps a ledge jump alive for a few ticks.
        if (p.grounded) p.coyote = kCoyoteTicks;
        else if (p.coyote > 0) --p.coyote;
        if (control && in.jump_pressed) p.jump_buffer = kJumpBufferTicks;
        else if (p.jump_buffer > 0) --p.jump_buffer;

        const f32 target = mx * kRunSpeed;
        const f32 accel = (p.grounded ? kGroundAccel : kAirAccel) * dt;
        if (p.vel.x < target) p.vel.x = math::min(p.vel.x + accel, target);
        else if (p.vel.x > target) p.vel.x = math::max(p.vel.x - accel, target);
        if (mx > 0.1f) p.facing = 1;
        if (mx < -0.1f) p.facing = -1;

        if (p.jump_buffer > 0 && p.coyote > 0) {
            p.vel.y = kJumpSpeed;
            p.jump_buffer = p.coyote = 0;
            play(f, gs.sounds.jump, -4.0f);
        }
        const bool rising_held = p.vel.y > 0.0f && in.jump_held && control;
        p.vel.y -= kGravity * (rising_held || p.vel.y <= 0.0f ? 1.0f : kLowJumpGravityScale) * dt;
        if (p.vel.y < -kMaxFall) p.vel.y = -kMaxFall;

        QueryFilter filter;
        filter.layer_mask = bit(Layer_World);
        const MoveResult2D r = pw.move_and_collide(e, p.vel * dt, filter);
        p.grounded = (r.flags & ControllerFlags::Ground) ? 1u : 0u;
        if (p.grounded && p.vel.y < 0.0f) p.vel.y = 0.0f;
        if (r.hit_ceiling && p.vel.y > 0.0f) p.vel.y = 0.0f;
        if ((r.flags & ControllerFlags::WallLeft) && p.vel.x < 0.0f) p.vel.x = 0.0f;
        if ((r.flags & ControllerFlags::WallRight) && p.vel.x > 0.0f) p.vel.x = 0.0f;
        p.running = (p.grounded && std::fabs(p.vel.x) > 0.5f) ? 1u : 0u;

        if (position_of(pw, e, lt).y < kKillY) respawn(f, gs, pw, e, p);
    });
}

bool ground_ahead(const World2D& pw, vec2 pos, f32 dir) {
    QueryFilter filter;
    filter.layer_mask = bit(Layer_World);
    const vec2 probe{pos.x + dir * (kEnemyHalf.x + 0.1f), pos.y};
    return pw.raycast(probe, {0.0f, -1.0f}, kEnemyHalf.y + 0.6f, filter).has_value();
}

bool can_see(const World2D& pw, vec2 from, vec2 to) {
    const vec2 d = to - from;
    const f32 dist = math::length(d);
    if (dist > kSightRange || std::fabs(d.y) > 2.5f) return false;
    if (dist < 1e-4f) return true;
    QueryFilter filter;
    filter.layer_mask = bit(Layer_World);
    return !pw.raycast(from, d * (1.0f / dist), dist, filter).has_value();
}

void set_state(Enemy& en, EnemyState s) {
    en.state = s;
    en.state_ticks = 0;
}

void pf_enemies(Frame& f) {
    const GameState& gs = f.resource<GameState>();
    if (gs.paused) return;
    World2D& pw = f.resource<World2D>();
    const f32 dt = f.dt;

    vec2 player_pos;
    bool player_ok = false;
    if (const Player* p = f.world().try_get<Player>(gs.player); p && p->invuln == 0 && !gs.won) {
        BodyState s;
        player_ok = pw.body_state(gs.player, s);
        player_pos = s.position;
    }

    f.query<Enemy&, const LocalTransform&>().each([&](Entity e, Enemy& en, const LocalTransform& lt) {
        if (!pw.has_body(e)) return;
        const vec2 pos = position_of(pw, e, lt);
        ++en.state_ticks;
        const bool sees = player_ok && can_see(pw, pos, player_pos);
        en.seen = sees ? 0u : en.seen + 1;

        f32 speed = 0.0f;
        switch (en.state) {
            case EnemyState::Patrol:
                if (sees) {
                    set_state(en, EnemyState::Chase);
                    break;
                }
                if ((en.dir > 0.0f && pos.x > en.home.x + en.patrol_half) ||
                    (en.dir < 0.0f && pos.x < en.home.x - en.patrol_half))
                    en.dir = -en.dir;
                speed = kPatrolSpeed;
                break;
            case EnemyState::Chase: {
                const f32 dx = player_ok ? player_pos.x - pos.x : 0.0f;
                if (!player_ok || en.seen > kChaseMemoryTicks || std::fabs(dx) > kLoseRange) {
                    set_state(en, EnemyState::Return);
                    break;
                }
                if (std::fabs(dx) > 0.2f) en.dir = dx > 0.0f ? 1.0f : -1.0f;
                speed = kChaseSpeed;
                break;
            }
            case EnemyState::Return: {
                if (sees) {
                    set_state(en, EnemyState::Chase);
                    break;
                }
                const f32 dx = en.home.x - pos.x;
                if (std::fabs(dx) < 0.2f) {
                    set_state(en, EnemyState::Patrol);
                    break;
                }
                en.dir = dx > 0.0f ? 1.0f : -1.0f;
                speed = kPatrolSpeed;
                break;
            }
        }

        // Never walk off a ledge; patrol turns around, chase waits at the edge.
        const bool grounded = en.vy == 0.0f;
        if (grounded && speed > 0.0f && !ground_ahead(pw, pos, en.dir)) {
            if (en.state == EnemyState::Patrol) en.dir = -en.dir;
            speed = 0.0f;
        }

        en.vy -= kGravity * dt;
        if (en.vy < -kMaxFall) en.vy = -kMaxFall;
        QueryFilter filter;
        filter.layer_mask = bit(Layer_World);
        const MoveResult2D r = pw.move_and_collide(e, {en.dir * speed * dt, en.vy * dt}, filter);
        if ((r.flags & ControllerFlags::Ground) && en.vy < 0.0f) en.vy = 0.0f;
        if (en.state == EnemyState::Patrol && (r.flags & (ControllerFlags::WallLeft | ControllerFlags::WallRight)))
            en.dir = -en.dir;
        if (r.position.y < kKillY) f.commands().destroy(e);
    });
}

void pf_triggers(Frame& f) {
    GameState& gs = f.resource<GameState>();
    World& w = f.world();
    World2D& pw = f.resource<World2D>();
    for (const TriggerEnter& ev : f.events().read<TriggerEnter>()) {
        if (ev.other != gs.player) continue;
        if (const Pickup* pk = w.try_get<Pickup>(ev.sensor)) {
            gs.coins += pk->value;
            gs.flash_ticks = 8;
            play(f, gs.sounds.pickup, -3.0f);
            f.commands().destroy(ev.sensor);
        } else if (w.has<Hazard>(ev.sensor)) {
            Player* p = w.try_get_mut<Player>(gs.player);
            if (p && p->invuln == 0 && !gs.won) respawn(f, gs, pw, gs.player, *p);
        } else if (Goal* g = w.try_get_mut<Goal>(ev.sensor); g && !gs.won) {
            g->reached = 1;
            gs.won = true;
            gs.won_at = gs.level_ticks;
            gs.flash_ticks = 20;
            play(f, gs.sounds.win);
        }
    }
}

void pf_combat(Frame& f) {
    GameState& gs = f.resource<GameState>();
    if (gs.paused || gs.won) return;
    World2D& pw = f.resource<World2D>();
    Player* p = f.world().try_get_mut<Player>(gs.player);
    BodyState ps;
    if (!p || !pw.body_state(gs.player, ps)) return;
    const math::aabb2 pbox{ps.position - kPlayerHalf, ps.position + kPlayerHalf};

    bool hurt = false;
    f.query<const Enemy&>().each([&](Entity e, const Enemy&) {
        BodyState es;
        if (hurt || !pw.body_state(e, es)) return;
        const vec2 pad{0.05f, 0.05f};
        const math::aabb2 ebox{es.position - kEnemyHalf - pad, es.position + kEnemyHalf + pad};
        if (!pbox.overlaps(ebox)) return;
        if (p->vel.y < 0.0f && pbox.min.y > es.position.y) {  // landed on top: stomp
            p->vel.y = kStompBounce;
            ++gs.stomps;
            gs.shake = 0.3f;
            play(f, gs.sounds.stomp);
            f.commands().destroy(e);
        } else if (p->invuln == 0) {
            hurt = true;
        }
    });
    if (hurt) respawn(f, gs, pw, gs.player, *p);
}

}  // namespace

ResultV install(World& world) {
    if (!world.try_resource<World2D>() || !world.try_resource<input::InputState>() ||
        !world.event_queue<audio::PlaySfx>())
        return Error(Errc::InvalidArgument, "platformer.install",
                     "the host installs input, physics2d and the audio bridge first");

    world.add_system(Phase::FixedUpdate_Pre, "pf_prev", &pf_prev);
    world.add_system(Phase::FixedUpdate_Pre, "pf_state", &pf_state, "pf_prev");
    world.add_system(Phase::FixedUpdate_Pre, "pf_movers", &pf_movers, "pf_state");
    world.add_system(Phase::FixedUpdate_Pre, "pf_player", &pf_player, "pf_movers");
    world.add_system(Phase::FixedUpdate_Pre, "pf_enemies", &pf_enemies, "pf_player");
    world.add_system(Phase::FixedUpdate_Post, "pf_triggers", &pf_triggers);
    world.add_system(Phase::FixedUpdate_Post, "pf_combat", &pf_combat, "pf_triggers");

    const Level lv;
    GameState gs;
    if (const Sounds* sounds = world.try_resource<Sounds>()) gs.sounds = *sounds;
    gs.level_size = {static_cast<f32>(lv.width), static_cast<f32>(lv.height)};
    spawn_level(world, lv, gs);
    world.set_resource(gs);
    return ok();
}

}  // namespace platformer
