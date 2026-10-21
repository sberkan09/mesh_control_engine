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

constexpr StringId kDashAction = MCE_SID("Dash");

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

// Wall slide / wall jump: holding into a wall while falling caps the fall; jumping off it kicks
// away and up, with steering ignored for a few ticks so the kick carries across a shaft.
constexpr f32 kWallSlideSpeed = 3.0f;
constexpr f32 kWallJumpX = 8.0f;
constexpr f32 kWallJumpY = 12.5f;
constexpr u32 kWallCoyoteTicks = 6;
constexpr u32 kWallLockTicks = 10;
// Dash: a flat burst (no gravity) in the held or facing direction, once per air time.
constexpr f32 kDashSpeed = 18.0f;
constexpr u32 kDashTicks = 10;
// Crumbling blocks: shake, then fall away, then grow back once the player is clear.
constexpr u32 kCrumbleShakeTicks = 30;
constexpr u32 kCrumbleGoneTicks = 150;

constexpr f32 kPatrolSpeed = 1.8f;
constexpr f32 kChaseSpeed = 3.6f;
constexpr f32 kSightRange = 7.0f;
constexpr f32 kLoseRange = 10.0f;
constexpr u32 kChaseMemoryTicks = 90;

constexpr f32 kWorldGravity = 9.81f;  // PhysicsConfig default; crates scale it up to kGravity
constexpr vec2 kCrateHalf{0.5f, 0.5f};
constexpr f32 kOrbitRadius = 1.5f;

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

void fx(GameState& gs, Fx kind, vec2 pos, i32 dir = 0) {
    if (gs.fx_count == sizeof(gs.fx) / sizeof(gs.fx[0])) return;  // the host drains every tick
    gs.fx[gs.fx_count++] = FxRequest{kind, dir, pos};
}

void play(Frame& f, audio::ClipHandle clip, f32 gain_db = 0.0f) {
    if (clip.is_null()) return;
    audio::PlaySfx ev;
    ev.clip_handle = clip;
    ev.gain_db = gain_db;
    f.events().push(ev);
}

vec2 mover_target(const Mover& m, f32 t) {
    const f32 a = math::kTau * t / m.period + m.phase;
    return m.origin + m.extent * std::sin(a) + m.extent_cos * std::cos(a);
}

// Spawns level entities, all tagged with the level: straight into the World at install, through
// Commands when a level loads mid-run (they appear at the phase's sync point).
struct Spawner {
    World& world;
    Commands* cmd;
    LevelTag tag;
    template <class... Ts>
    Entity operator()(const Ts&... cs) {
        return cmd ? cmd->spawn(cs..., tag) : world.spawn_with(cs..., tag);
    }
};

// Dynamic crate: falls like the player (kGravity), rotates, stacks, rides platforms by friction.
void spawn_crate(Spawner& w, vec2 p, u32 id) {
    RigidBody2D rb;
    rb.type = BodyType::Dynamic;
    rb.layer = Layer_World;
    rb.mask = 0xFFFF;
    rb.gravity_scale = kGravity / kWorldGravity;
    Collider2D c;
    c.shape = ColliderShape::box(kCrateHalf);
    c.density = 1.0f;
    c.friction = 0.6f;
    w(rb, c, Crate{id}, PrevPos{p}, look(Sprite::Crate, 2.0f * kCrateHalf, 4), at(p));
}

void spawn_level(Spawner& w, const Level& lv, GameState& gs) {
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
                w(body(BodyType::Static, Layer_World, world_mask),
                  collider(ColliderShape::box({0.5f * len, 0.5f})), at({cx, static_cast<f32>(y) + 0.5f}));
            } else {
                w(body(BodyType::Static, Layer_World, world_mask),
                  collider(ColliderShape::box({0.5f * len, 0.1f}), false, true),
                  at({cx, static_cast<f32>(y) + 0.9f}));
            }
            x = end + 1;
        }
    }

    u32 crates = 0;
    for (i32 y = 0; y < static_cast<i32>(lv.height); ++y) {
        for (i32 x = 0; x < static_cast<i32>(lv.width); ++x) {
            const vec2 p = Level::center(x, y);
            switch (lv.at(x, y)) {
                case 'P': {
                    Player pl;
                    pl.spawn = {p.x, static_cast<f32>(y) + kPlayerHalf.y + 0.01f};
                    gs.player = w(body(BodyType::Kinematic, Layer_Player, bit(Layer_World) | bit(Layer_Sensor)),
                                  collider(ColliderShape::box(kPlayerHalf)), CharacterController2D{}, pl,
                                  PrevPos{pl.spawn}, look(Sprite::Player, {1, 1}, 10), at(pl.spawn));
                    break;
                }
                case 'E': {
                    Enemy en;
                    en.home = {p.x, static_cast<f32>(y) + kEnemyHalf.y + 0.01f};
                    en.dir = (x % 2) ? 1.0f : -1.0f;
                    w(body(BodyType::Kinematic, Layer_Enemy, bit(Layer_World)),
                      collider(ColliderShape::box(kEnemyHalf)), CharacterController2D{}, en, PrevPos{en.home},
                      look(Sprite::Enemy, {1, 1}, 5), at(en.home));
                    break;
                }
                case 'o': {
                    w(body(BodyType::Static, Layer_Sensor, bit(Layer_Player)),
                      collider(ColliderShape::circle(0.3f), true), Pickup{}, PrevPos{p},
                      look(Sprite::Coin, {0.75f, 0.75f}, 2), at(p));
                    ++gs.coins_total;
                    break;
                }
                case '^': {
                    const vec2 c{p.x, static_cast<f32>(y) + 0.3f};
                    w(body(BodyType::Static, Layer_Sensor, bit(Layer_Player)),
                      collider(ColliderShape::box({0.4f, 0.2f}), true), Hazard{}, PrevPos{c},
                      look(Sprite::Spike, {1, 0.6f}, 1), at(c));
                    break;
                }
                case 'F': {
                    const vec2 c{p.x, static_cast<f32>(y) + 1.0f};
                    w(body(BodyType::Static, Layer_Sensor, bit(Layer_Player)),
                      collider(ColliderShape::box({0.3f, 1.0f}), true), Goal{}, PrevPos{c},
                      look(Sprite::Flag, {1, 2}, 1), at(c));
                    break;
                }
                case 'X': {
                    w(body(BodyType::Static, Layer_World, world_mask), collider(ColliderShape::box({0.5f, 0.5f})),
                      Crumble{}, PrevPos{p}, look(Sprite::Crumble, {1, 1}, 1), at(p));
                    break;
                }
                case 'C': {
                    spawn_crate(w, {p.x, static_cast<f32>(y) + kCrateHalf.y}, crates++);
                    break;
                }
                case 'O': {
                    // Orbit: starts at the top of a 1.5 m circle around the tile and goes clockwise,
                    // with a crate riding on it (friction carry through the solver, M3.5 demo).
                    Mover m;
                    m.origin = p;
                    m.extent = {kOrbitRadius, 0.0f};
                    m.extent_cos = {0.0f, kOrbitRadius};
                    m.period = 6.0f;
                    const vec2 start = mover_target(m, 0.0f);
                    w(body(BodyType::Kinematic, Layer_World, world_mask),
                      collider(ColliderShape::box({1.5f, 0.25f})), m, PrevPos{start},
                      look(Sprite::Platform, {3, 0.5f}, 3), at(start));
                    spawn_crate(w, {start.x, start.y + 0.25f + kCrateHalf.y}, crates++);
                    break;
                }
                case 'M':
                case 'V': {
                    Mover m;
                    m.origin = p;
                    m.extent = lv.at(x, y) == 'M' ? vec2{2.0f, 0.0f} : vec2{0.0f, 3.5f};
                    m.period = lv.at(x, y) == 'M' ? 4.0f : 5.0f;
                    const vec2 start = mover_target(m, 0.0f);
                    w(body(BodyType::Kinematic, Layer_World, world_mask),
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

void reset_run(GameState& gs) {
    gs.level_ticks = 0;
    gs.coins = gs.coins_total = gs.deaths = gs.stomps = gs.flash_ticks = 0;
    gs.won = gs.new_best = false;
    gs.won_at = 0;
    gs.results_timer = 0;
}

// Swaps the level: everything tagged with the old one goes, the new one spawns through Commands
// (both land at this phase's sync point). The rest of this tick is frozen.
void load_level(Frame& f, GameState& gs, u32 level) {
    f.query<const LevelTag&>().each([&](Entity e, const LevelTag&) { f.commands().destroy(e); });
    reset_run(gs);
    gs.level = level < kLevelCount ? level : 0;
    ++gs.level_serial;
    const Level lv(gs.level);
    gs.level_size = {static_cast<f32>(lv.width), static_cast<f32>(lv.height)};
    Spawner sp{f.world(), &f.commands(), LevelTag{gs.level}};
    spawn_level(sp, lv, gs);
    gs.paused = true;
}

// The menu / pause / results state machine; asks for a level (re)load through `load`.
void menu_flow(GameState& gs, const input::InputState& in, bool& load, u32& next_level) {
    switch (gs.mode) {
        case Mode::Menu:
            if (in.confirm_pressed) {
                load = true;
                next_level = gs.selection;
                gs.mode = Mode::Playing;
            }
            break;
        case Mode::Playing:
            if (gs.won) {
                if (gs.results_timer > 0) {
                    --gs.results_timer;
                    if (gs.results_timer == 0) {
                        gs.mode = Mode::Results;
                        gs.selection = 0;
                    }
                }
            } else if (in.pause_pressed) {
                gs.mode = Mode::Paused;
                gs.selection = 0;
            }
            break;
        case Mode::Paused:  // Resume / Restart / Menu
            if (in.pause_pressed || (in.confirm_pressed && gs.selection == 0)) {
                gs.mode = Mode::Playing;
            } else if (in.confirm_pressed && gs.selection == 1) {
                load = true;
                gs.mode = Mode::Playing;
            } else if (in.confirm_pressed) {
                gs.mode = Mode::Menu;
                gs.selection = gs.level;
            }
            break;
        case Mode::Results: {  // [Next] / Retry / Menu
            const bool has_next = gs.level + 1 < kLevelCount;
            const u32 pick = has_next ? gs.selection : gs.selection + 1;
            if (in.confirm_pressed && pick == 0) {
                load = true;
                next_level = gs.level + 1;
                gs.mode = Mode::Playing;
            } else if (in.confirm_pressed && pick == 1) {
                load = true;
                gs.mode = Mode::Playing;
            } else if (in.confirm_pressed || in.cancel_pressed) {
                gs.mode = Mode::Menu;
                gs.selection = gs.level;
            }
            break;
        }
    }

}

void pf_state(Frame& f) {
    GameState& gs = f.resource<GameState>();
    const input::InputState& in = f.resource<input::InputState>();
    const Records* rec = f.world().try_get<Records>(gs.records);

    // Menu navigation: one step per push of Move.y past half way (up = previous entry).
    const f32 ny = in.move.y;
    const i32 nav = (ny > 0.5f && gs.nav_prev <= 0.5f) ? -1 : (ny < -0.5f && gs.nav_prev >= -0.5f) ? 1 : 0;
    gs.nav_prev = ny;
    const u32 options = menu_options(gs, rec);
    if (options > 0 && gs.selection >= options) gs.selection = options - 1;
    if (nav != 0 && options > 0) gs.selection = (gs.selection + (nav > 0 ? 1u : options - 1u)) % options;

    bool load = false;
    u32 next_level = gs.level;
    if (gs.load_request >= 0) {  // host request (level select from the command line, tests)
        load = true;
        next_level = static_cast<u32>(gs.load_request);
        gs.load_request = -1;
        gs.mode = Mode::Playing;
    }
    if (!load) menu_flow(gs, in, load, next_level);

    gs.paused = gs.mode != Mode::Playing;
    if (load) load_level(f, gs, next_level);
    if (gs.flash_ticks > 0) --gs.flash_ticks;
    if (!gs.paused && !gs.won) ++gs.level_ticks;
}

void pf_crumble(Frame& f) {
    const GameState& gs = f.resource<GameState>();
    if (gs.paused) return;
    World2D& pw = f.resource<World2D>();
    BodyState ps;
    const bool player_ok = pw.body_state(gs.player, ps);
    f.query<Crumble&, const LocalTransform&>().each([&](Entity e, Crumble& c, const LocalTransform& lt) {
        if (c.state == CrumbleState::Solid) return;
        if (c.timer > 0) {
            --c.timer;
            if (c.timer > 0) return;
        }
        if (c.state == CrumbleState::Shaking) {
            pw.set_enabled(e, false);
            c.state = CrumbleState::Gone;
            c.timer = kCrumbleGoneTicks;
            return;
        }
        // Grows back only when the player is not standing in it.
        const vec2 home{lt.position.x, lt.position.y};
        if (player_ok && std::fabs(ps.position.x - home.x) < 0.5f + kPlayerHalf.x &&
            std::fabs(ps.position.y - home.y) < 0.5f + kPlayerHalf.y) {
            c.timer = 1;
            return;
        }
        pw.set_enabled(e, true);
        c.state = CrumbleState::Solid;
    });
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
    BodyState s;
    if (pw.body_state(e, s)) fx(gs, Fx::Hurt, s.position);
    pw.teleport(e, p.spawn, 0.0f);
    p.vel = {};
    p.coyote = p.jump_buffer = 0;
    p.wall = 0;
    p.wall_coyote = p.wall_lock = p.dash_ticks = p.sliding = 0;
    p.dash_ready = 1;
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

    const bool dash_pressed = in.has_action(kDashAction) ? in.pressed(kDashAction) : in.fire_pressed;

    f.query<Player&, const LocalTransform&>().each([&](Entity e, Player& p, const LocalTransform& lt) {
        if (e != gs.player || !pw.has_body(e)) return;
        if (p.invuln > 0) --p.invuln;
        const bool control = !gs.won;
        const f32 mx = control ? math::clamp(in.move.x, -1.0f, 1.0f) : 0.0f;
        const vec2 pos = position_of(pw, e, lt);

        // Grounded as of the previous move; coyote time keeps a ledge jump alive for a few ticks.
        if (p.grounded) p.coyote = kCoyoteTicks;
        else if (p.coyote > 0) --p.coyote;
        if (p.wall != 0) p.wall_coyote = kWallCoyoteTicks;
        else if (p.wall_coyote > 0) --p.wall_coyote;
        if (p.grounded || p.wall != 0) p.dash_ready = 1;
        if (p.wall_lock > 0) --p.wall_lock;
        if (control && in.jump_pressed) p.jump_buffer = kJumpBufferTicks;
        else if (p.jump_buffer > 0) --p.jump_buffer;

        if (p.wall_lock == 0) {
            const f32 target = mx * kRunSpeed;
            const f32 accel = (p.grounded ? kGroundAccel : kAirAccel) * dt;
            if (p.vel.x < target) p.vel.x = math::min(p.vel.x + accel, target);
            else if (p.vel.x > target) p.vel.x = math::max(p.vel.x - accel, target);
            if (mx > 0.1f) p.facing = 1;
            if (mx < -0.1f) p.facing = -1;
        }

        if (p.jump_buffer > 0 && p.coyote > 0) {
            p.vel.y = kJumpSpeed;
            p.jump_buffer = p.coyote = p.wall_coyote = 0;
            p.dash_ticks = 0;
            play(f, gs.sounds.jump, -4.0f);
            fx(gs, Fx::Jump, pos);
        } else if (p.jump_buffer > 0 && p.wall_coyote > 0 && !p.grounded) {
            // Kick off the wall last touched (it still counts for a few ticks after leaving it).
            const i32 away = -p.last_wall;
            p.vel = {static_cast<f32>(away) * kWallJumpX, kWallJumpY};
            p.facing = away;
            p.jump_buffer = p.wall_coyote = 0;
            p.wall_lock = kWallLockTicks;
            p.dash_ticks = 0;
            play(f, gs.sounds.jump, -4.0f);
            fx(gs, Fx::WallJump, pos, away);
        }

        if (control && dash_pressed && p.dash_ready && p.dash_ticks == 0) {
            p.dash_dir = mx > 0.1f ? 1 : mx < -0.1f ? -1 : p.facing;
            p.facing = p.dash_dir;
            p.dash_ticks = kDashTicks;
            p.dash_ready = 0;
            p.wall_lock = 0;
            ++p.dashes;
            play(f, gs.sounds.jump, -10.0f);
            fx(gs, Fx::Dash, pos, p.dash_dir);
        }

        if (p.dash_ticks > 0) {
            p.vel = {static_cast<f32>(p.dash_dir) * kDashSpeed, 0.0f};
            if (--p.dash_ticks == 0) p.vel.x = static_cast<f32>(p.dash_dir) * kRunSpeed;
        } else {
            const bool rising_held = p.vel.y > 0.0f && in.jump_held && control;
            p.vel.y -= kGravity * (rising_held || p.vel.y <= 0.0f ? 1.0f : kLowJumpGravityScale) * dt;
            if (p.vel.y < -kMaxFall) p.vel.y = -kMaxFall;
            // Wall slide: holding into the wall while falling.
            const bool into_wall = p.wall != 0 && mx * static_cast<f32>(p.wall) > 0.1f;
            p.sliding = (into_wall && !p.grounded && p.vel.y < -kWallSlideSpeed) ? 1u : 0u;
            if (p.sliding) p.vel.y = -kWallSlideSpeed;
        }

        QueryFilter filter;
        filter.layer_mask = bit(Layer_World);
        const bool was_grounded = p.grounded != 0;
        const f32 fall_speed = -p.vel.y;
        const MoveResult2D r = pw.move_and_collide(e, p.vel * dt, filter);
        p.grounded = (r.flags & ControllerFlags::Ground) ? 1u : 0u;
        if (p.grounded && p.vel.y < 0.0f) p.vel.y = 0.0f;
        if (r.hit_ceiling && p.vel.y > 0.0f) p.vel.y = 0.0f;
        const bool wall_left = (r.flags & ControllerFlags::WallLeft) != 0;
        const bool wall_right = (r.flags & ControllerFlags::WallRight) != 0;
        if (wall_left && p.vel.x < 0.0f) p.vel.x = 0.0f;
        if (wall_right && p.vel.x > 0.0f) p.vel.x = 0.0f;
        if ((wall_left || wall_right) && p.dash_ticks > 0) p.dash_ticks = 0;  // a dash ends on a wall
        p.wall = p.grounded ? 0 : wall_left ? -1 : wall_right ? 1 : 0;
        if (p.wall != 0) p.last_wall = p.wall;
        if (p.wall == 0) p.sliding = 0;
        p.running = (p.grounded && std::fabs(p.vel.x) > 0.5f) ? 1u : 0u;
        if (p.grounded && !was_grounded && fall_speed > 6.0f) fx(gs, Fx::Land, r.position);

        // Standing on a crumbling block starts it shaking.
        if (p.grounded && !r.ground_body.is_null()) {
            if (Crumble* c = f.world().try_get_mut<Crumble>(r.ground_body); c && c->state == CrumbleState::Solid) {
                c->state = CrumbleState::Shaking;
                c->timer = kCrumbleShakeTicks;
                if (const LocalTransform* ct = f.world().try_get<LocalTransform>(r.ground_body))
                    fx(gs, Fx::Crumble, {ct->position.x, ct->position.y});
            }
        }

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

void finish_level(Frame& f, GameState& gs) {
    gs.won = true;
    gs.won_at = gs.level_ticks;
    gs.flash_ticks = 20;
    gs.results_timer = kResultsDelayTicks;
    if (Records* rec = f.world().try_get_mut<Records>(gs.records)) {
        const u32 t = static_cast<u32>(gs.won_at);
        if (rec->best(gs.level) == 0 || t < rec->best(gs.level)) {
            rec->set_best(gs.level, t);
            gs.new_best = true;
        }
        if (rec->unlocked < gs.level + 2 && gs.level + 2 <= kLevelCount) rec->unlocked = gs.level + 2;
        ++gs.records_serial;
    }
}

void pf_triggers(Frame& f) {
    GameState& gs = f.resource<GameState>();
    if (gs.paused) return;
    World& w = f.world();
    World2D& pw = f.resource<World2D>();
    for (const TriggerEnter& ev : f.events().read<TriggerEnter>()) {
        if (ev.other != gs.player) continue;
        if (const Pickup* pk = w.try_get<Pickup>(ev.sensor)) {
            gs.coins += pk->value;
            gs.flash_ticks = 8;
            play(f, gs.sounds.pickup, -3.0f);
            if (const LocalTransform* t = w.try_get<LocalTransform>(ev.sensor))
                fx(gs, Fx::Coin, {t->position.x, t->position.y});
            f.commands().destroy(ev.sensor);
        } else if (w.has<Hazard>(ev.sensor)) {
            Player* p = w.try_get_mut<Player>(gs.player);
            if (p && p->invuln == 0 && !gs.won) respawn(f, gs, pw, gs.player, *p);
        } else if (Goal* g = w.try_get_mut<Goal>(ev.sensor); g && !gs.won) {
            g->reached = 1;
            finish_level(f, gs);
            play(f, gs.sounds.win);
            if (const LocalTransform* t = w.try_get<LocalTransform>(ev.sensor))
                fx(gs, Fx::Win, {t->position.x, t->position.y});
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
            fx(gs, Fx::Stomp, es.position);
            f.commands().destroy(e);
        } else if (p->invuln == 0) {
            hurt = true;
        }
    });
    if (hurt) respawn(f, gs, pw, gs.player, *p);
}

}  // namespace

ResultV install(World& world) {
    if (!world.try_resource<World2D>() || !world.try_resource<input::InputState>())
        return Error(Errc::InvalidArgument, "platformer.install", "the host installs input and physics2d first");
    // The audio bridge registers it too (idempotent); a host without audio just drops the events.
    world.register_event<audio::PlaySfx>(Phase::FixedUpdate_Pre);

    world.add_system(Phase::FixedUpdate_Pre, "pf_prev", &pf_prev);
    world.add_system(Phase::FixedUpdate_Pre, "pf_state", &pf_state, "pf_prev");
    world.add_system(Phase::FixedUpdate_Pre, "pf_movers", &pf_movers, "pf_state");
    world.add_system(Phase::FixedUpdate_Pre, "pf_crumble", &pf_crumble, "pf_movers");
    world.add_system(Phase::FixedUpdate_Pre, "pf_player", &pf_player, "pf_crumble");
    world.add_system(Phase::FixedUpdate_Pre, "pf_enemies", &pf_enemies, "pf_player");
    world.add_system(Phase::FixedUpdate_Post, "pf_triggers", &pf_triggers);
    world.add_system(Phase::FixedUpdate_Post, "pf_combat", &pf_combat, "pf_triggers");

    world.register_components<SaveKey, Records>();

    const Level lv(0);
    GameState gs;
    if (const Sounds* sounds = world.try_resource<Sounds>()) gs.sounds = *sounds;
    gs.level_size = {static_cast<f32>(lv.width), static_cast<f32>(lv.height)};
    Spawner sp{world, nullptr, LevelTag{0}};
    spawn_level(sp, lv, gs);
    gs.records = world.spawn_with(SaveKey{kRecordsSaveKey}, Records{});
    world.set_resource(gs);
    return ok();
}

}  // namespace platformer
